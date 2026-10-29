// AirDC++ dark mode - msimg32.dll proxy.
//
// Loaded by AirDC.exe through its static import of msimg32.dll. Forwards the
// real msimg32 API, and when Windows is in dark app mode:
//   - every color AirDC.exe itself hands to Windows (GDI text/background colors,
//     brushes and pens it selects or fills with, list/tree/rich edit colors it
//     sends, gradient vertices, custom-draw results) is passed through a
//     hue-preserving lightness flip in OKLab, so the user's light AirDC theme
//     becomes a matching dark one,
//   - controls drawn by Windows get the native dark themes, comctl32/rich edit
//     get a dark system palette, dialogs and MDI client get dark backgrounds,
//   - the menu bar is painted dark (UAH messages), title bars use the system
//     dark mode, popup menus are drawn here (accent-blue selection),
//   - live OS light/dark switches (WM_SETTINGCHANGE "ImmersiveColorSet") are
//     followed, including recoloring existing chat text.
//
// Optional darkmode.ini next to this DLL:
//   [DarkMode]
//   Mode=auto      ; auto (follow Windows), dark, light

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <psapi.h>
#include <intrin.h>
#include <math.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// msimg32.dll forwarding

static HMODULE g_self, g_exe;
static BYTE *g_exeStart, *g_exeEnd;
static volatile BOOL g_dark;

static BOOL (WINAPI *p_AlphaBlend)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
static BOOL (WINAPI *p_GradientFill)(HDC, PTRIVERTEX, ULONG, PVOID, ULONG, ULONG);
static BOOL (WINAPI *p_TransparentBlt)(HDC, int, int, int, int, HDC, int, int, int, int, UINT);
static BOOL (WINAPI *p_DllInitialize)(HINSTANCE, DWORD, LPVOID);
static void (WINAPI *p_vSetDdrawflag)(void);

static void LoadRealMsimg32(void)
{
    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH - 16);
    lstrcpyW(path + n, L"\\msimg32.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real)
        return;
    p_AlphaBlend = (void *)GetProcAddress(real, "AlphaBlend");
    p_GradientFill = (void *)GetProcAddress(real, "GradientFill");
    p_TransparentBlt = (void *)GetProcAddress(real, "TransparentBlt");
    p_DllInitialize = (void *)GetProcAddress(real, "DllInitialize");
    p_vSetDdrawflag = (void *)GetProcAddress(real, "vSetDdrawflag");
}

static BOOL FromExe(void *ret)
{
    return (BYTE *)ret >= g_exeStart && (BYTE *)ret < g_exeEnd;
}

static COLORREF Fwd(COLORREF c);

BOOL WINAPI Proxy_AlphaBlend(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, int sw, int sh, BLENDFUNCTION f)
{
    return p_AlphaBlend(d, x, y, w, h, s, sx, sy, sw, sh, f);
}

BOOL WINAPI Proxy_TransparentBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, int sw, int sh, UINT t)
{
    return p_TransparentBlt(d, x, y, w, h, s, sx, sy, sw, sh, t);
}

BOOL WINAPI Proxy_DllInitialize(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    return p_DllInitialize ? p_DllInitialize(inst, reason, reserved) : TRUE;
}

void WINAPI Proxy_vSetDdrawflag(void)
{
    if (p_vSetDdrawflag)
        p_vSetDdrawflag();
}

BOOL WINAPI Proxy_GradientFill(HDC dc, PTRIVERTEX v, ULONG nv, PVOID mesh, ULONG nm, ULONG mode)
{
    if (!g_dark || !FromExe(_ReturnAddress()) || nv > 64)
        return p_GradientFill(dc, v, nv, mesh, nm, mode);
    TRIVERTEX copy[64];
    for (ULONG i = 0; i < nv; i++) {
        copy[i] = v[i];
        COLORREF c = Fwd(RGB(v[i].Red >> 8, v[i].Green >> 8, v[i].Blue >> 8));
        copy[i].Red = (COLOR16)(GetRValue(c) << 8);
        copy[i].Green = (COLOR16)(GetGValue(c) << 8);
        copy[i].Blue = (COLOR16)(GetBValue(c) << 8);
    }
    return p_GradientFill(dc, copy, nv, mesh, nm, mode);
}

// ---------------------------------------------------------------------------
// Settings

typedef enum { MODE_AUTO, MODE_DARK, MODE_LIGHT } DarkModeSetting;
typedef enum { AppModeDefault, AppModeAllowDark, AppModeForceDark, AppModeForceLight } PreferredAppMode;

static DarkModeSetting g_setting = MODE_AUTO;
static int g_palette;  // index into kPalettes; 0 = AirDC's own colors
static int PaletteFromKey(const WCHAR *key);

static BOOL SystemUsesDarkApps(void)
{
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS)
        return FALSE;
    return value == 0;
}

static BOOL ComputeDark(void)
{
    if (g_setting == MODE_DARK)
        return TRUE;
    if (g_setting == MODE_LIGHT)
        return FALSE;
    return SystemUsesDarkApps();
}

static void ReadSettings(void)
{
    WCHAR ini[MAX_PATH], mode[16];
    DWORD n = GetModuleFileNameW(g_self, ini, MAX_PATH);
    while (n && ini[n - 1] != L'\\')
        n--;
    lstrcpyW(ini + n, L"darkmode.ini");
    GetPrivateProfileStringW(L"DarkMode", L"Mode", L"auto", mode, ARRAYSIZE(mode), ini);
    // Palette: the one picked from the View menu (per user), else the DLL's ini.
    WCHAR user[MAX_PATH], palette[32] = L"";
    if (ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\AirDC++\\darkmode.ini", user, MAX_PATH))
        GetPrivateProfileStringW(L"DarkMode", L"Palette", L"", palette, ARRAYSIZE(palette), user);
    if (!palette[0])
        GetPrivateProfileStringW(L"DarkMode", L"Palette", L"", palette, ARRAYSIZE(palette), ini);
    g_palette = PaletteFromKey(palette);
    if (!lstrcmpiW(mode, L"dark") || !lstrcmpiW(mode, L"on"))
        g_setting = MODE_DARK;
    else if (!lstrcmpiW(mode, L"light") || !lstrcmpiW(mode, L"off"))
        g_setting = MODE_LIGHT;
    else
        g_setting = MODE_AUTO;
}

#ifdef DARKMODE_TRACE
static void Trace(const char *fmt, ...)
{
    static HANDLE f = INVALID_HANDLE_VALUE;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (f == INVALID_HANDLE_VALUE) {
        // darkmode-trace.log in the temp folder.
        WCHAR path[MAX_PATH];
        DWORD len = GetTempPathW(MAX_PATH - 24, path);
        lstrcpyW(path + len, L"darkmode-trace.log");
        f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    }
    DWORD w;
    WriteFile(f, buf, n, &w, NULL);
}
#define TRACE(...) Trace(__VA_ARGS__)
#else
#define TRACE(...)
#endif

// ---------------------------------------------------------------------------
// Color transform: flip OKLab lightness, keep hue and chroma.
//
// White maps to about #1F1F1F, black to about #E6E6E6. Every result is also
// remembered so the exact original can be recovered (Inv), which keeps colors
// that round-trip through the app (Get*Color, custom draw) stable.

#define L_TOP 0.93f
#define L_SCALE 0.685f

static float g_toLinear[256];

static float ToSrgb(float x)
{
    x = x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

static void ToLab(COLORREF c, float *L, float *a, float *b)
{
    float r = g_toLinear[GetRValue(c)], g = g_toLinear[GetGValue(c)], bl = g_toLinear[GetBValue(c)];
    float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * bl);
    float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * bl);
    float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * bl);
    *L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    *a = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    *b = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static COLORREF FromLab(float L, float a, float b)
{
    float l = L + 0.3963377774f * a + 0.2158037573f * b;
    float m = L - 0.1055613458f * a - 0.0638541728f * b;
    float s = L - 0.0894841775f * a - 1.2914855480f * b;
    l = l * l * l; m = m * m * m; s = s * s * s;
    float r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    float bl = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    return RGB((int)(ToSrgb(r) * 255 + 0.5f), (int)(ToSrgb(g) * 255 + 0.5f), (int)(ToSrgb(bl) * 255 + 0.5f));
}

// Open addressed caches of 64-bit entries: (key + 1) << 32 | value.
#define CACHE_SIZE 8192
static volatile LONG64 g_fwdCache[CACHE_SIZE], g_invCache[CACHE_SIZE];

static BOOL CacheGet(volatile LONG64 *cache, COLORREF key, COLORREF *value)
{
    for (UINT i = (key * 2654435761u) % CACHE_SIZE, n = 0; n < 16; i = (i + 1) % CACHE_SIZE, n++) {
        LONG64 e = cache[i];
        if (!e)
            return FALSE;
        if ((COLORREF)(e >> 32) == key + 1) {
            *value = (COLORREF)e;
            return TRUE;
        }
    }
    return FALSE;
}

static void CachePut(volatile LONG64 *cache, COLORREF key, COLORREF value)
{
    for (UINT i = (key * 2654435761u) % CACHE_SIZE, n = 0; n < 16; i = (i + 1) % CACHE_SIZE, n++) {
        LONG64 e = cache[i];
        if (!e || (COLORREF)(e >> 32) == key + 1) {
            cache[i] = ((LONG64)(key + 1) << 32) | value;
            return;
        }
    }
}

static COLORREF Fwd(COLORREF c)
{
    if (c & 0xFF000000)  // CLR_NONE, CLR_DEFAULT, palette indices
        return c;
    COLORREF r;
    if (CacheGet(g_fwdCache, c, &r))
        return r;
    float L, a, b;
    ToLab(c, &L, &a, &b);
    r = FromLab(L_TOP - L_SCALE * L, a, b);
    CachePut(g_fwdCache, c, r);
    CachePut(g_invCache, r, c);
    return r;
}

static COLORREF Inv(COLORREF c)
{
    if (c & 0xFF000000)
        return c;
    COLORREF r;
    if (CacheGet(g_invCache, c, &r))
        return r;
    float L, a, b;
    ToLab(c, &L, &a, &b);
    L = (L_TOP - L) / L_SCALE;
    r = FromLab(L > 1 ? 1 : L < 0 ? 0 : L, a, b);
    return r;
}

static COLORREF FwdIf(COLORREF c) { return g_dark ? Fwd(c) : c; }

// ---------------------------------------------------------------------------
// Text color palettes: every text color is moved to the palette's shade of
// its hue family; greys and white become the palette's dim or main text color.
// Backgrounds are not touched.

typedef struct {
    const WCHAR *name, *key;
    COLORREF fg, dim;
    COLORREF hue[8];  // red, orange, yellow, green, cyan, blue, purple, pink
} Palette;

#define HEX(x) RGB(((x) >> 16) & 0xFF, ((x) >> 8) & 0xFF, (x) & 0xFF)
static const Palette kPalettes[] = {
    { L"AirDC++ colors", L"airdc" },  // no remapping
    { L"Catppuccin Mocha", L"catppuccin", HEX(0xCDD6F4), HEX(0xA6ADC8),
      { HEX(0xF38BA8), HEX(0xFAB387), HEX(0xF9E2AF), HEX(0xA6E3A1), HEX(0x94E2D5), HEX(0x89B4FA), HEX(0xCBA6F7), HEX(0xF5C2E7) } },
    { L"Dracula", L"dracula", HEX(0xF8F8F2), HEX(0x8A93B8),
      { HEX(0xFF5555), HEX(0xFFB86C), HEX(0xF1FA8C), HEX(0x50FA7B), HEX(0x8BE9FD), HEX(0xBD93F9), HEX(0xBD93F9), HEX(0xFF79C6) } },
    { L"Nord", L"nord", HEX(0xECEFF4), HEX(0x9AA5B8),
      { HEX(0xBF616A), HEX(0xD08770), HEX(0xEBCB8B), HEX(0xA3BE8C), HEX(0x88C0D0), HEX(0x81A1C1), HEX(0xB48EAD), HEX(0xB48EAD) } },
    { L"Gruvbox", L"gruvbox", HEX(0xEBDBB2), HEX(0xA89984),
      { HEX(0xFB4934), HEX(0xFE8019), HEX(0xFABD2F), HEX(0xB8BB26), HEX(0x8EC07C), HEX(0x83A598), HEX(0xD3869B), HEX(0xD3869B) } },
    { L"Cream (default)", L"cream", HEX(0xF2E6D0), HEX(0xB3A895),
      { HEX(0xE5534B), HEX(0xF07C5A), HEX(0xF2A07B), HEX(0x7FB8F0), HEX(0x8FD0F5), HEX(0x5E9EFF), HEX(0x8F9FFF), HEX(0xF0717A) } },
};
#undef HEX

#define ID_PALETTE_FIRST 0xDA10  // View menu commands, one per palette
static void AddPaletteMenu(HMENU menu);

// OKLab hue angles (degrees) of the families above.
static const float kHueAnchors[8] = { 25, 60, 100, 140, 195, 260, 305, 345 };

#define PALETTE_DEFAULT 5  // Cream: cream text, blues and reds

static int PaletteFromKey(const WCHAR *key)
{
    for (int i = 0; i < ARRAYSIZE(kPalettes); i++)
        if (!lstrcmpiW(key, kPalettes[i].key))
            return i;
    return PALETTE_DEFAULT;
}

// c is a final (dark mode) text color.
static COLORREF PaletteText(COLORREF c)
{
    if (!g_palette || (c & 0xFF000000))
        return c;
    const Palette *p = &kPalettes[g_palette];
    float L, a, b;
    ToLab(c, &L, &a, &b);
    if (sqrtf(a * a + b * b) < 0.04f)
        return L >= 0.7f ? p->fg : p->dim;
    float h = atan2f(b, a) * 57.29578f;
    if (h < 0)
        h += 360;
    int best = 0;
    float bestDist = 1000;
    for (int i = 0; i < ARRAYSIZE(kHueAnchors); i++) {
        float d = fabsf(h - kHueAnchors[i]);
        if (d > 180)
            d = 360 - d;
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return p->hue[best];
}

// Text on a flipped background: the lighter of c and Fwd(c). Black still turns
// white, but AirDC's status colors (green, red, grey), already light enough,
// stay as they are instead of turning dark.
static COLORREF TextFwd(COLORREF c)
{
    COLORREF f = Fwd(c);
    if (c & 0xFF000000)
        return f;
    float Lc, Lf, a, b;
    ToLab(c, &Lc, &a, &b);
    ToLab(f, &Lf, &a, &b);
    COLORREF r = Lf >= Lc ? f : c;
    COLORREF pal = PaletteText(r);
    if (pal != r || r == c)
        CachePut(g_invCache, pal, c);  // so Inv() gives AirDC its own color back
    return pal;
}
static COLORREF InvIf(COLORREF c) { return g_dark ? Inv(c) : c; }

// ---------------------------------------------------------------------------
// Dark system palette for the code Windows draws (comctl32, rich edit).

static COLORREF g_accent = RGB(0, 120, 212);

static void ReadAccent(void)
{
    DWORD value, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS)
        g_accent = value & 0xFFFFFF;
}

static COLORREF Blend(COLORREF a, COLORREF b, int pct)
{
    return RGB((GetRValue(a) * pct + GetRValue(b) * (100 - pct)) / 100,
               (GetGValue(a) * pct + GetGValue(b) * (100 - pct)) / 100,
               (GetBValue(a) * pct + GetBValue(b) * (100 - pct)) / 100);
}

static DWORD (WINAPI *Real_GetSysColor)(int);
static HBRUSH (WINAPI *Real_GetSysColorBrush)(int);

// System colors as AirDC's own drawing would show them: flipped.
static COLORREF FlippedSys(int index)
{
    return Fwd(Real_GetSysColor(index));
}

static COLORREF DarkSysColor(int index, BOOL *mapped)
{
    *mapped = TRUE;
    switch (index) {
    case COLOR_HIGHLIGHT:     return Blend(g_accent, RGB(32, 32, 32), 55);
    case COLOR_HIGHLIGHTTEXT: return RGB(255, 255, 255);
    case COLOR_HOTLIGHT:      return Blend(g_accent, RGB(255, 255, 255), 60);
    case COLOR_MENUHILIGHT:   return Blend(g_accent, RGB(32, 32, 32), 55);
    case COLOR_GRAYTEXT:      return RGB(130, 130, 130);
    case COLOR_INFOBK:        return RGB(43, 43, 43);
    case COLOR_INFOTEXT:      return RGB(230, 230, 230);
    }
    if (index >= 0 && index <= COLOR_MENUBAR)
        return FlippedSys(index);
    *mapped = FALSE;
    return 0;
}

static DWORD WINAPI Lib_GetSysColor(int index)
{
    if (g_dark) {
        BOOL mapped;
        COLORREF c = DarkSysColor(index, &mapped);
        if (mapped)
            return c;
    }
    return Real_GetSysColor(index);
}

static HBRUSH SolidBrush(COLORREF c);

static HBRUSH WINAPI Lib_GetSysColorBrush(int index)
{
    if (g_dark) {
        BOOL mapped;
        COLORREF c = DarkSysColor(index, &mapped);
        if (mapped)
            return SolidBrush(c);
    }
    return Real_GetSysColorBrush(index);
}

// Real (light) system brush index for h, or -1.
static int SystemBrushIndex(HGDIOBJ h)
{
    for (int i = 0; i <= COLOR_MENUBAR; i++)
        if (h == Real_GetSysColorBrush(i))
            return i;
    return -1;
}

static void RegisterFinal(HGDIOBJ h);

// Cached solid brush in a given (final) color, for backgrounds this DLL paints.
// These are never freed: they may still be in use as control brushes.
static HBRUSH SolidBrush(COLORREF c)
{
    static struct { COLORREF c; HBRUSH b; } cache[256];
    static int count;
    for (int i = 0; i < count; i++)
        if (cache[i].c == c)
            return cache[i].b;
    HBRUSH b = CreateSolidBrush(c);
    RegisterFinal(b);
    if (count < ARRAYSIZE(cache)) {
        cache[count].c = c;
        cache[count++].b = b;
    }
    return b;
}

// Same color as SolidBrush, different handle: comctl32 edits treat a
// WM_CTLCOLOREDIT brush equal to GetSysColorBrush(COLOR_WINDOW) as "parent
// did not color me" and paint the theme's (light) fill instead.
static HBRUSH ControlBrush(COLORREF c)
{
    static struct { COLORREF c; HBRUSH b; } cache[256];
    static int count;
    for (int i = 0; i < count; i++)
        if (cache[i].c == c)
            return cache[i].b;
    if (count == ARRAYSIZE(cache))
        return SolidBrush(c);
    HBRUSH b = CreateSolidBrush(c);
    RegisterFinal(b);
    cache[count].c = c;
    cache[count++].b = b;
    return b;
}

// A control brush for h when it is one of the (patched) system color brushes.
static HBRUSH Distinct(HBRUSH h)
{
    for (int i = 0; i <= COLOR_MENUBAR; i++) {
        BOOL mapped;
        COLORREF c = DarkSysColor(i, &mapped);
        if (mapped && h == SolidBrush(c))
            return ControlBrush(c);
    }
    return h;
}

// ---------------------------------------------------------------------------
// Brush/pen twins: AirDC's own brushes and pens get a flipped twin that is
// swapped in wherever AirDC hands them to GDI.
//
// Open-addressed table keyed by handle. Three kinds of entry:
//   source: key = AirDC's object, twin = its flipped twin
//   twin:   key = twin, twin = twin, owner = the source (so SelectObject can
//           hand AirDC its own object back)
//   final:  key = an object already in dark colors (twin = key, owner = NULL)
// Deleting a source or a twin removes both entries and frees the twin, so the
// table only holds live objects (a process has at most 10000 GDI objects).

typedef struct {
    HGDIOBJ key, twin, owner;
    COLORREF color;
} Twin;

#define TWIN_SIZE 32768
#define TWIN_TOMB ((HGDIOBJ)(ULONG_PTR)1)
static Twin g_twins[TWIN_SIZE];
static LONG g_twinUsed;  // live entries + tombstones
static SRWLOCK g_twinLock = SRWLOCK_INIT;

static UINT TwinSlot(HGDIOBJ h)
{
    return (UINT)(((ULONG_PTR)h >> 2) * 2654435761u) % TWIN_SIZE;
}

// Entry for h, or NULL. Caller holds the lock.
static Twin *TwinFind(HGDIOBJ h)
{
    UINT i = TwinSlot(h);
    for (UINT n = 0; n < TWIN_SIZE; n++, i = (i + 1) % TWIN_SIZE) {
        Twin *t = &g_twins[i];
        if (!t->key)
            return NULL;
        if (t->key == h)
            return t;
    }
    return NULL;
}

static void TwinRemove(Twin *t)
{
    t->key = TWIN_TOMB;
    t->twin = t->owner = NULL;
}

// Rebuild without tombstones. Caller holds the lock exclusively.
static void TwinCompact(void)
{
    static Twin old[TWIN_SIZE];
    memcpy(old, g_twins, sizeof(g_twins));
    memset(g_twins, 0, sizeof(g_twins));
    g_twinUsed = 0;
    for (UINT i = 0; i < TWIN_SIZE; i++) {
        if (!old[i].key || old[i].key == TWIN_TOMB)
            continue;
        UINT j = TwinSlot(old[i].key);
        while (g_twins[j].key)
            j = (j + 1) % TWIN_SIZE;
        g_twins[j] = old[i];
        g_twinUsed++;
    }
}

// Entry for h, created if needed; NULL if the table is full. Caller holds the
// lock exclusively.
static Twin *TwinInsert(HGDIOBJ h)
{
    Twin *t = TwinFind(h);
    if (t)
        return t;
    if (g_twinUsed >= TWIN_SIZE * 3 / 4)
        TwinCompact();
    if (g_twinUsed >= TWIN_SIZE * 3 / 4)
        return NULL;
    UINT i = TwinSlot(h);
    for (UINT n = 0; n < TWIN_SIZE; n++, i = (i + 1) % TWIN_SIZE) {
        t = &g_twins[i];
        if (!t->key || t->key == TWIN_TOMB) {
            if (!t->key)
                g_twinUsed++;
            t->key = h;
            t->twin = t->owner = NULL;
            return t;
        }
    }
    return NULL;
}

// Solid color of a brush or plain pen, or FALSE.
static BOOL ObjectColor(HGDIOBJ h, DWORD type, COLORREF *c)
{
    if (type == OBJ_BRUSH) {
        LOGBRUSH lb;
        if (GetObjectW(h, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID)
            return FALSE;
        *c = lb.lbColor;
        return TRUE;
    }
    if (type == OBJ_PEN) {
        LOGPEN lp;
        if (GetObjectW(h, sizeof(lp), &lp) != sizeof(lp))
            return FALSE;
        *c = lp.lopnColor;
        return TRUE;
    }
    return FALSE;
}

// Drop source entry s and its twin's entry; returns the twin to free, or NULL.
// Caller holds the lock exclusively.
static HGDIOBJ TwinUnlink(Twin *s)
{
    HGDIOBJ twin = s->twin;
    if (twin && twin != s->key) {
        Twin *t = TwinFind(twin);
        if (t && t->owner == s->key)
            TwinRemove(t);
    } else {
        twin = NULL;
    }
    TwinRemove(s);
    return twin;
}

// While set, pen twins are made in this color instead of the flipped one.
static COLORREF g_forcePen = CLR_INVALID;

static HGDIOBJ MapObject(HGDIOBJ h)
{
    if (!g_dark || !h)
        return h;
    DWORD type = GetObjectType(h);
    if (type != OBJ_BRUSH && type != OBJ_PEN)
        return h;
    COLORREF color;
    if (!ObjectColor(h, type, &color))
        return h;

    // Twins and finals map to themselves; a stale entry (handle reused with
    // another color) is rebuilt below.
    AcquireSRWLockShared(&g_twinLock);
    Twin *t = TwinFind(h);
    HGDIOBJ found = t && t->twin && (t->twin == h || t->color == color) ? t->twin : NULL;
    ReleaseSRWLockShared(&g_twinLock);
    if (found)
        return found;

    HGDIOBJ twin;
    if (type == OBJ_BRUSH) {
        twin = CreateSolidBrush(Fwd(color));
    } else {
        LOGPEN lp;
        GetObjectW(h, sizeof(lp), &lp);
        lp.lopnColor = g_forcePen != CLR_INVALID ? g_forcePen : Fwd(color);
        twin = CreatePenIndirect(&lp);
    }
    if (!twin)
        return h;

    HGDIOBJ stale = NULL, result = twin;
    AcquireSRWLockExclusive(&g_twinLock);
    t = TwinFind(h);
    if (t && t->twin && (t->twin == h || t->color == color)) {
        // Another thread made one meanwhile.
        result = t->twin;
        stale = twin;
    } else {
        if (t)
            stale = TwinUnlink(t);
        Twin *s = TwinInsert(h);
        Twin *w = s ? TwinInsert(twin) : NULL;
        if (w) {
            s->twin = twin;
            s->color = color;
            w->twin = twin;
            w->owner = h;
            w->color = type == OBJ_PEN && g_forcePen != CLR_INVALID ? g_forcePen : Fwd(color);
        } else {
            // Table full: draw unflipped rather than leak.
            if (s)
                TwinRemove(s);
            DeleteObject(twin);
            result = h;
        }
    }
    ReleaseSRWLockExclusive(&g_twinLock);
    if (stale)
        DeleteObject(stale);
    return result;
}

// AirDC's own object for h when h is one of the twins, otherwise h.
static HGDIOBJ UnmapObject(HGDIOBJ h)
{
    if (!h)
        return h;
    AcquireSRWLockShared(&g_twinLock);
    Twin *t = TwinFind(h);
    HGDIOBJ owner = t && t->owner ? t->owner : h;
    ReleaseSRWLockShared(&g_twinLock);
    return owner;
}

// Objects already in final (dark) colors map to themselves.
static void RegisterFinal(HGDIOBJ h)
{
    COLORREF color;
    if (!h || !ObjectColor(h, GetObjectType(h), &color))
        return;
    AcquireSRWLockExclusive(&g_twinLock);
    Twin *t = TwinInsert(h);
    if (t) {
        t->twin = h;
        t->owner = NULL;
        t->color = color;
    }
    ReleaseSRWLockExclusive(&g_twinLock);
}

// h is about to be deleted: free its twin and forget both.
static void ForgetObject(HGDIOBJ h)
{
    HGDIOBJ twin = NULL;
    AcquireSRWLockExclusive(&g_twinLock);
    Twin *t = TwinFind(h);
    if (t) {
        if (t->owner) {
            // h is a twin AirDC got hold of (GetCurrentObject or similar):
            // its source gets a new twin on next use.
            Twin *s = TwinFind(t->owner);
            if (s && s->twin == h)
                TwinRemove(s);
            TwinRemove(t);
        } else {
            twin = TwinUnlink(t);
        }
    }
    ReleaseSRWLockExclusive(&g_twinLock);
    if (twin)
        DeleteObject(twin);
}

// ---------------------------------------------------------------------------
// AirDC.exe GDI/USER hooks

static COLORREF (WINAPI *Real_SetTextColor)(HDC, COLORREF);
static COLORREF (WINAPI *Real_SetBkColor)(HDC, COLORREF);
static COLORREF (WINAPI *Real_GetBkColor)(HDC);
static HGDIOBJ (WINAPI *Real_SelectObject)(HDC, HGDIOBJ);
static BOOL (WINAPI *Real_DeleteObject)(HGDIOBJ);
static COLORREF (WINAPI *Real_SetPixelV)(HDC, int, int, COLORREF);
static COLORREF (WINAPI *Real_GetPixel)(HDC, int, int);
static HDC (WINAPI *Real_CreateCompatibleDC)(HDC);
static int (WINAPI *Real_FillRect)(HDC, const RECT *, HBRUSH);
static int (WINAPI *Real_FrameRect)(HDC, const RECT *, HBRUSH);
static HDC (WINAPI *Real_BeginPaint)(HWND, LPPAINTSTRUCT);
static BOOL (WINAPI *Real_DrawEdge)(HDC, LPRECT, UINT, UINT);
static BOOL (WINAPI *Real_DrawFrameControl)(HDC, LPRECT, UINT, UINT);
static LRESULT (WINAPI *Real_DefWindowProcW)(HWND, UINT, WPARAM, LPARAM);
static LRESULT (WINAPI *Real_DefFrameProcW)(HWND, HWND, UINT, WPARAM, LPARAM);
static LRESULT (WINAPI *Real_DefMDIChildProcW)(HWND, UINT, WPARAM, LPARAM);
static LRESULT (WINAPI *Real_SendMessageW)(HWND, UINT, WPARAM, LPARAM);
static HWND (WINAPI *Real_CreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static BOOL (WINAPI *Real_ImageList_DrawEx)(HIMAGELIST, int, HDC, int, int, int, int, COLORREF, COLORREF, UINT);
static BOOL (WINAPI *Real_ImageList_DrawIndirect)(IMAGELISTDRAWPARAMS *);
static HTHEME (WINAPI *Real_OpenThemeData)(HWND, LPCWSTR);
static HRESULT (WINAPI *Real_DrawThemeBackground)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
static HTHEME (WINAPI *Real_OpenThemeDataEx)(HWND, LPCWSTR, DWORD);
static HTHEME (WINAPI *Real_OpenThemeDataForDpi)(HWND, LPCWSTR, UINT);
static FARPROC (WINAPI *Real_GetProcAddress)(HMODULE, LPCSTR);

static BOOL IsBarDC(HDC dc);

static COLORREF WINAPI Hook_SetTextColor(HDC dc, COLORREF c)
{
    if (g_dark && IsBarDC(dc))
        return InvIf(Real_SetTextColor(dc, RGB(255, 255, 255)));
    return InvIf(Real_SetTextColor(dc, g_dark ? TextFwd(c) : c));
}

static COLORREF WINAPI Hook_SetBkColor(HDC dc, COLORREF c)
{
    return InvIf(Real_SetBkColor(dc, FwdIf(c)));
}

static COLORREF WINAPI Hook_GetBkColor(HDC dc)
{
    return InvIf(Real_GetBkColor(dc));
}

static COLORREF WINAPI Hook_SetPixelV(HDC dc, int x, int y, COLORREF c)
{
    return Real_SetPixelV(dc, x, y, FwdIf(c));
}

static COLORREF WINAPI Hook_GetPixel(HDC dc, int x, int y)
{
    return InvIf(Real_GetPixel(dc, x, y));
}

// Nesting count of WM_PAINT in AirDC's window tab strip (FlatTabCtrl).
static int g_tabPaint;
#define BORDER_NORMAL RGB(52, 52, 52)

static HGDIOBJ WINAPI Hook_SelectObject(HDC dc, HGDIOBJ h)
{
    // The tab strip's 1px border pens (the tab border colors in AirDC's
    // settings) come out light grey and white once flipped: draw them dark.
    // Its 2px pen joins the active tab to the window and keeps its color.
    LOGPEN lp;
    if (g_dark && g_tabPaint && h && GetObjectType(h) == OBJ_PEN && GetObjectW(h, sizeof(lp), &lp) == sizeof(lp) &&
        lp.lopnWidth.x <= 1) {
        g_forcePen = BORDER_NORMAL;
        HGDIOBJ twin = MapObject(h);
        g_forcePen = CLR_INVALID;
        return UnmapObject(Real_SelectObject(dc, twin));
    }
    // AirDC often deletes what SelectObject returns: hand back its own
    // object, never the twin.
    return UnmapObject(Real_SelectObject(dc, MapObject(h)));
}

static BOOL WINAPI Hook_DeleteObject(HGDIOBJ h)
{
    DWORD type = GetObjectType(h);
    if (type == OBJ_BRUSH || type == OBJ_PEN)
        ForgetObject(h);
    return Real_DeleteObject(h);
}

// Fresh DCs start with black text on white; give them the flipped defaults.
static void DefaultDcColors(HDC dc)
{
    if (dc && g_dark) {
        Real_SetTextColor(dc, Fwd(RGB(0, 0, 0)));
        Real_SetBkColor(dc, Fwd(RGB(255, 255, 255)));
    }
}

// Memory DCs AirDC creates while custom drawing a list: its progress bars
// (transfer status, queue). Their text is drawn white in dark mode.
static DWORD g_uiThread;  // the thread with the window hooks
static HDC g_barDCs[8];
static int g_barNext;
static int g_cdTop;

static BOOL IsBarDC(HDC dc)
{
    for (int i = 0; i < ARRAYSIZE(g_barDCs); i++)
        if (g_barDCs[i] == dc)
            return TRUE;
    return FALSE;
}

static HDC WINAPI Hook_CreateCompatibleDC(HDC dc)
{
    HDC r = Real_CreateCompatibleDC(dc);
    DefaultDcColors(r);
    if (r && g_dark && g_cdTop && GetCurrentThreadId() == g_uiThread) {
        g_barDCs[g_barNext] = r;
        g_barNext = (g_barNext + 1) % ARRAYSIZE(g_barDCs);
    }
    return r;
}

static HDC WINAPI Hook_BeginPaint(HWND hwnd, LPPAINTSTRUCT ps)
{
    HDC r = Real_BeginPaint(hwnd, ps);
    DefaultDcColors(r);
    return r;
}

static int WINAPI Hook_FillRect(HDC dc, const RECT *rc, HBRUSH b)
{
    // (HBRUSH)(COLOR_x + 1) means a system color.
    if (g_dark && (ULONG_PTR)b > 0 && (ULONG_PTR)b <= COLOR_MENUBAR + 1)
        return Real_FillRect(dc, rc, SolidBrush(FlippedSys((int)(ULONG_PTR)b - 1)));
    return Real_FillRect(dc, rc, MapObject(b));
}

static int WINAPI Hook_FrameRect(HDC dc, const RECT *rc, HBRUSH b)
{
    return Real_FrameRect(dc, rc, MapObject(b));
}

static void EdgeLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF c)
{
    RECT r = { x1, y1, x2, y2 };
    Real_SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
}

// DrawEdge reads the real system colors inside user32; draw it flipped.
static BOOL WINAPI Hook_DrawEdge(HDC dc, LPRECT rc, UINT edge, UINT flags)
{
    if (!g_dark || (flags & BF_DIAGONAL))
        return Real_DrawEdge(dc, rc, edge, flags);

    COLORREF hi = FlippedSys(COLOR_BTNHIGHLIGHT), lt = FlippedSys(COLOR_3DLIGHT);
    COLORREF sh = FlippedSys(COLOR_BTNSHADOW), dk = FlippedSys(COLOR_3DDKSHADOW);
    COLORREF face = FlippedSys(COLOR_BTNFACE);
    COLORREF oldBk = Real_GetBkColor(dc);
    RECT r = *rc;

    for (int ring = 0; ring < 2; ring++) {
        UINT bits = ring == 0 ? (edge & BDR_OUTER) : (edge & BDR_INNER);
        if (!bits)
            continue;
        COLORREF tl, br;
        if (flags & BF_FLAT) {
            tl = br = ring == 0 ? sh : face;
        } else if (ring == 0) {
            tl = (bits & BDR_RAISEDOUTER) ? lt : sh;
            br = (bits & BDR_RAISEDOUTER) ? dk : hi;
        } else {
            tl = (bits & BDR_RAISEDINNER) ? hi : dk;
            br = (bits & BDR_RAISEDINNER) ? sh : lt;
        }
        if (flags & BF_TOP)    EdgeLine(dc, r.left, r.top, r.right, r.top + 1, tl);
        if (flags & BF_LEFT)   EdgeLine(dc, r.left, r.top, r.left + 1, r.bottom, tl);
        if (flags & BF_BOTTOM) EdgeLine(dc, r.left, r.bottom - 1, r.right, r.bottom, br);
        if (flags & BF_RIGHT)  EdgeLine(dc, r.right - 1, r.top, r.right, r.bottom, br);
        if (flags & BF_TOP)    r.top++;
        if (flags & BF_LEFT)   r.left++;
        if (flags & BF_BOTTOM) r.bottom--;
        if (flags & BF_RIGHT)  r.right--;
    }
    if (flags & BF_MIDDLE)
        EdgeLine(dc, r.left, r.top, r.right, r.bottom, face);
    if (flags & BF_ADJUST)
        *rc = r;
    Real_SetBkColor(dc, oldBk);
    return TRUE;
}

static void DrawCaptionGlyph(HDC dc, const RECT *rc, WCHAR glyph, COLORREF bg, COLORREF fg);

// MDI child caption buttons on the command bar: user32 draws them light.
static BOOL WINAPI Hook_DrawFrameControl(HDC dc, LPRECT rc, UINT type, UINT state)
{
    if (!g_dark || type != DFC_CAPTION)
        return Real_DrawFrameControl(dc, rc, type, state);

    WCHAR glyph;
    switch (state & 0xFF) {
    case DFCS_CAPTIONCLOSE:   glyph = L'r'; break;
    case DFCS_CAPTIONMIN:     glyph = L'0'; break;
    case DFCS_CAPTIONMAX:     glyph = L'1'; break;
    case DFCS_CAPTIONRESTORE: glyph = L'2'; break;
    case DFCS_CAPTIONHELP:    glyph = L's'; break;
    default: return Real_DrawFrameControl(dc, rc, type, state);
    }
    COLORREF bg = FlippedSys(COLOR_BTNFACE);
    if (state & DFCS_PUSHED)
        bg = Blend(bg, RGB(255, 255, 255), 85);
    DrawCaptionGlyph(dc, rc, glyph, bg, (state & DFCS_INACTIVE) ? RGB(110, 110, 110) : RGB(225, 225, 225));
    return TRUE;
}

// Caption button: background plus a Marlett glyph.
static void DrawCaptionGlyph(HDC dc, const RECT *rc, WCHAR glyph, COLORREF bg, COLORREF fg)
{
    Real_FillRect(dc, rc, SolidBrush(bg));
    int size = min(rc->right - rc->left, rc->bottom - rc->top) * 2 / 3;
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, 0, 0, 0, SYMBOL_CHARSET, 0, 0, 0, 0, L"Marlett");
    HGDIOBJ oldFont = SelectObject(dc, font);
    int oldMode = SetBkMode(dc, TRANSPARENT);
    COLORREF oldFg = Real_SetTextColor(dc, fg);
    RECT r = *rc;
    DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    Real_SetTextColor(dc, oldFg);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

// What DefWindowProc would do for backgrounds and control colors, flipped.
static BOOL DarkDefault(HWND hwnd, UINT msg, WPARAM wp, LRESULT *result)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        HBRUSH cls = (HBRUSH)GetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND);
        if (!cls)
            return FALSE;
        RECT rc;
        GetClientRect(hwnd, &rc);
        if ((ULONG_PTR)cls <= COLOR_MENUBAR + 1)
            Real_FillRect((HDC)wp, &rc, SolidBrush(FlippedSys((int)(ULONG_PTR)cls - 1)));
        else
            Real_FillRect((HDC)wp, &rc, MapObject(cls));
        *result = 1;
        return TRUE;
    }
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC: {
        int sys = (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) ? COLOR_WINDOW : COLOR_BTNFACE;
        Real_SetTextColor((HDC)wp, PaletteText(FlippedSys(COLOR_WINDOWTEXT)));
        Real_SetBkColor((HDC)wp, FlippedSys(sys));
        *result = (LRESULT)SolidBrush(FlippedSys(sys));
        return TRUE;
    }
    }
    return FALSE;
}

static LRESULT WINAPI Hook_DefWindowProcW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    if (g_dark && DarkDefault(hwnd, msg, wp, &r))
        return r;
    return Real_DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT WINAPI Hook_DefFrameProcW(HWND hwnd, HWND client, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    if (g_dark && DarkDefault(hwnd, msg, wp, &r))
        return r;
    return Real_DefFrameProcW(hwnd, client, msg, wp, lp);
}

static LRESULT WINAPI Hook_DefMDIChildProcW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    if (g_dark && DarkDefault(hwnd, msg, wp, &r))
        return r;
    return Real_DefMDIChildProcW(hwnd, msg, wp, lp);
}

static BOOL WINAPI Hook_ImageList_DrawEx(HIMAGELIST il, int i, HDC dc, int x, int y, int dx, int dy,
                                         COLORREF bk, COLORREF fg, UINT style)
{
    return Real_ImageList_DrawEx(il, i, dc, x, y, dx, dy, FwdIf(bk), FwdIf(fg), style);
}

static BOOL WINAPI Hook_ImageList_DrawIndirect(IMAGELISTDRAWPARAMS *p)
{
    if (!g_dark || !p)
        return Real_ImageList_DrawIndirect(p);
    IMAGELISTDRAWPARAMS copy = *p;
    copy.rgbBk = Fwd(p->rgbBk);
    copy.rgbFg = Fwd(p->rgbFg);
    return Real_ImageList_DrawIndirect(&copy);
}

// ---------------------------------------------------------------------------
// Color messages AirDC sends to controls. Originals are kept in window
// properties so a live switch can resend them.

static BOOL IsClass(HWND hwnd, LPCWSTR prefix)
{
    WCHAR cls[64];
    if (!RealGetWindowClassW(hwnd, cls, ARRAYSIZE(cls)))
        return FALSE;
    int n = lstrlenW(prefix);
    return CompareStringOrdinal(cls, min(n, lstrlenW(cls)), prefix, n, TRUE) == CSTR_EQUAL;
}

// Control family, seeing through WTL superclasses ("ATL:xxxx" class names).
typedef enum { KIND_OTHER, KIND_LISTVIEW, KIND_TREEVIEW, KIND_RICHEDIT, KIND_TOOLBAR, KIND_PROGRESS } Kind;

static Kind WindowKind(HWND hwnd)
{
    static const WCHAR prop[] = L"DarkMode.kind";
    ULONG_PTR cached = (ULONG_PTR)GetPropW(hwnd, prop);
    if (cached)
        return (Kind)(cached - 1);

    Kind k = KIND_OTHER;
    if (IsClass(hwnd, WC_LISTVIEWW))
        k = KIND_LISTVIEW;
    else if (IsClass(hwnd, WC_TREEVIEWW))
        k = KIND_TREEVIEW;
    else if (IsClass(hwnd, L"RichEdit") || IsClass(hwnd, L"RICHEDIT"))
        k = KIND_RICHEDIT;
    else if (IsClass(hwnd, TOOLBARCLASSNAMEW) || IsClass(hwnd, L"WTL_CommandBar") || IsClass(hwnd, L"WTL_MDICommandBar"))
        k = KIND_TOOLBAR;
    else if (IsClass(hwnd, PROGRESS_CLASSW))
        k = KIND_PROGRESS;
    else if (IsClass(hwnd, L"ATL:")) {
        // A list view owns a SysHeader32 child; a tree view reports an indent.
        // Progress bars (in toolbars/status bars) report a state and range.
        HWND header = (HWND)Real_SendMessageW(hwnd, LVM_GETHEADER, 0, 0);
        HWND parent = GetParent(hwnd);
        LRESULT state;
        if (header && IsWindow(header) && GetParent(header) == hwnd && IsClass(header, WC_HEADERW))
            k = KIND_LISTVIEW;
        else if (parent && (IsClass(parent, TOOLBARCLASSNAMEW) || IsClass(parent, STATUSCLASSNAMEW) ||
                            IsClass(parent, REBARCLASSNAMEW)) &&
                 (state = Real_SendMessageW(hwnd, PBM_GETSTATE, 0, 0)) >= PBST_NORMAL && state <= PBST_PAUSED &&
                 Real_SendMessageW(hwnd, PBM_GETRANGE, FALSE, 0) > 0)
            k = KIND_PROGRESS;
        else if ((GetWindowLongW(hwnd, GWL_STYLE) & (TVS_HASLINES | TVS_HASBUTTONS)) &&
                 Real_SendMessageW(hwnd, TVM_GETINDENT, 0, 0))
            k = KIND_TREEVIEW;
        else
            return k;  // may not be fully created yet; ask again later
    }
    SetPropW(hwnd, prop, (HANDLE)(ULONG_PTR)(k + 1));
    return k;
}

static BOOL IsRichEdit(HWND hwnd)
{
    return WindowKind(hwnd) == KIND_RICHEDIT;
}

// Messages whose lParam is a single color, per control family.
static BOOL IsColorMessage(HWND hwnd, UINT msg)
{
    switch (msg) {
    case LVM_SETBKCOLOR: case LVM_SETTEXTCOLOR: case LVM_SETTEXTBKCOLOR: case LVM_SETOUTLINECOLOR:
        return WindowKind(hwnd) == KIND_LISTVIEW;
    case TVM_SETBKCOLOR: case TVM_SETTEXTCOLOR: case TVM_SETLINECOLOR:
        return WindowKind(hwnd) == KIND_TREEVIEW;
    case EM_SETBKGNDCOLOR:
        return IsRichEdit(hwnd);
    case CCM_SETBKCOLOR:
        return WindowKind(hwnd) == KIND_PROGRESS || IsClass(hwnd, STATUSCLASSNAMEW) || IsClass(hwnd, REBARCLASSNAMEW);
    case PBM_SETBARCOLOR:
        return WindowKind(hwnd) == KIND_PROGRESS;
    case TTM_SETTIPBKCOLOR:  // == RB_SETBKCOLOR
        return IsClass(hwnd, TOOLTIPS_CLASSW) || IsClass(hwnd, REBARCLASSNAMEW);
    case TTM_SETTIPTEXTCOLOR:
        return IsClass(hwnd, TOOLTIPS_CLASSW);
    case RB_SETTEXTCOLOR:
        return IsClass(hwnd, REBARCLASSNAMEW);
    }
    return FALSE;
}

static BOOL IsGetColorMessage(HWND hwnd, UINT msg)
{
    switch (msg) {
    case LVM_GETBKCOLOR: case LVM_GETTEXTCOLOR: case LVM_GETTEXTBKCOLOR:
        return WindowKind(hwnd) == KIND_LISTVIEW;
    case TVM_GETBKCOLOR: case TVM_GETTEXTCOLOR: case TVM_GETLINECOLOR:
        return WindowKind(hwnd) == KIND_TREEVIEW;
    }
    return FALSE;
}

static BOOL IsTextColorMessage(UINT msg)
{
    return msg == LVM_SETTEXTCOLOR || msg == TVM_SETTEXTCOLOR || msg == TTM_SETTIPTEXTCOLOR || msg == RB_SETTEXTCOLOR;
}

// Dark mode value of a color sent with msg.
static COLORREF MessageColor(UINT msg, COLORREF c)
{
    return !g_dark ? c : IsTextColorMessage(msg) ? TextFwd(c) : Fwd(c);
}

static void PropName(UINT msg, WCHAR *out)
{
    wsprintfW(out, L"DarkMode.%X", msg);
}

static const UINT kColorMessages[] = {
    LVM_SETBKCOLOR, LVM_SETTEXTCOLOR, LVM_SETTEXTBKCOLOR, LVM_SETOUTLINECOLOR,
    TVM_SETBKCOLOR, TVM_SETTEXTCOLOR, TVM_SETLINECOLOR, EM_SETBKGNDCOLOR,
    CCM_SETBKCOLOR, PBM_SETBARCOLOR, TTM_SETTIPBKCOLOR, TTM_SETTIPTEXTCOLOR, RB_SETTEXTCOLOR,
};

static void CopyCharFormat(const CHARFORMATW *src, CHARFORMAT2W *dst, BOOL toDark)
{
    UINT size = src->cbSize <= sizeof(CHARFORMAT2W) ? src->cbSize : sizeof(CHARFORMAT2W);
    ZeroMemory(dst, sizeof(*dst));
    CopyMemory(dst, src, size);
    if ((dst->dwMask & CFM_COLOR) && !(dst->dwEffects & CFE_AUTOCOLOR))
        dst->crTextColor = toDark ? TextFwd(dst->crTextColor) : Inv(dst->crTextColor);
    if (size >= sizeof(CHARFORMAT2W) && (dst->dwMask & CFM_BACKCOLOR) && !(dst->dwEffects & CFE_AUTOBACKCOLOR))
        dst->crBackColor = toDark ? Fwd(dst->crBackColor) : Inv(dst->crBackColor);
}

static LRESULT WINAPI Hook_SendMessageW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == EM_SETCHARFORMAT && lp && g_dark && IsRichEdit(hwnd)) {
        CHARFORMAT2W cf;
        CopyCharFormat((const CHARFORMATW *)lp, &cf, TRUE);
        return Real_SendMessageW(hwnd, msg, wp, (LPARAM)&cf);
    }
    if (msg == EM_GETCHARFORMAT && lp && g_dark && IsRichEdit(hwnd)) {
        CHARFORMATW *out = (CHARFORMATW *)lp;
        LRESULT r = Real_SendMessageW(hwnd, msg, wp, lp);
        CHARFORMAT2W cf;
        CopyCharFormat(out, &cf, FALSE);
        CopyMemory(out, &cf, out->cbSize <= sizeof(cf) ? out->cbSize : sizeof(cf));
        return r;
    }
    if (IsColorMessage(hwnd, msg)) {
        WCHAR prop[32];
        PropName(msg, prop);
        // EM_SETBKGNDCOLOR with wParam set means "use the system color".
        BOOL plain = !(msg == EM_SETBKGNDCOLOR && wp);
        if (plain)
            SetPropW(hwnd, prop, (HANDLE)(ULONG_PTR)((ULONG64)(COLORREF)lp + 1));
        else
            RemovePropW(hwnd, prop);
        LRESULT r = Real_SendMessageW(hwnd, msg, wp, plain ? (LPARAM)MessageColor(msg, (COLORREF)lp) : lp);
        return (LRESULT)InvIf((COLORREF)r);
    }
    if (IsGetColorMessage(hwnd, msg))
        return (LRESULT)InvIf((COLORREF)Real_SendMessageW(hwnd, msg, wp, lp));
    return Real_SendMessageW(hwnd, msg, wp, lp);
}

// Live switch: resend stored originals in the current mode.
static void ResendColors(HWND hwnd)
{
    for (int i = 0; i < ARRAYSIZE(kColorMessages); i++) {
        WCHAR prop[32];
        PropName(kColorMessages[i], prop);
        ULONG_PTR v = (ULONG_PTR)GetPropW(hwnd, prop);
        if (v)
            Real_SendMessageW(hwnd, kColorMessages[i], 0, (LPARAM)MessageColor(kColorMessages[i], (COLORREF)(v - 1)));
    }
}

static const IID kIID_ITextDocument = { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };

// Live switch: recolor every run of existing rich edit text.
// how: RECOLOR_LIGHT, RECOLOR_DARK, or RECOLOR_PALETTE (dark to dark, new palette).
enum { RECOLOR_LIGHT, RECOLOR_DARK, RECOLOR_PALETTE };
static void RecolorRichEdit(HWND hwnd, int how)
{
    IRichEditOle *ole = NULL;
    if (!Real_SendMessageW(hwnd, EM_GETOLEINTERFACE, 0, (LPARAM)&ole) || !ole)
        return;
    // TOM refuses to format a read-only control (the chat/log views are).
    BOOL readOnly = (GetWindowLongW(hwnd, GWL_STYLE) & ES_READONLY) != 0;
    if (readOnly)
        Real_SendMessageW(hwnd, EM_SETREADONLY, FALSE, 0);
    ITextDocument *doc = NULL;
    if (SUCCEEDED(ole->lpVtbl->QueryInterface(ole, &kIID_ITextDocument, (void **)&doc)) && doc) {
        long frozen;
        ITextDocument_Freeze(doc, &frozen);
        ITextDocument_Undo(doc, tomSuspend, NULL);
        ITextRange *r = NULL;
        if (SUCCEEDED(ITextDocument_Range(doc, 0, 0, &r)) && r) {
            for (int guard = 0; guard < 1000000; guard++) {
                long moved = 0;
                if (FAILED(ITextRange_MoveEnd(r, tomCharFormat, 1, &moved)) || !moved)
                    break;
                ITextFont *f = NULL;
                if (SUCCEEDED(ITextRange_GetFont(r, &f)) && f) {
                    long c;
                    if (SUCCEEDED(ITextFont_GetForeColor(f, &c)) && c != tomAutoColor && c != tomUndefined && !(c & 0xFF000000))
                        ITextFont_SetForeColor(f, how == RECOLOR_LIGHT ? Inv(c) : how == RECOLOR_DARK ? TextFwd(c) : TextFwd(Inv(c)));
                    if (how != RECOLOR_PALETTE && SUCCEEDED(ITextFont_GetBackColor(f, &c)) && c != tomAutoColor &&
                        c != tomUndefined && !(c & 0xFF000000))
                        ITextFont_SetBackColor(f, how == RECOLOR_DARK ? Fwd(c) : Inv(c));
                    ITextFont_Release(f);
                }
                ITextRange_Collapse(r, tomEnd);
            }
            ITextRange_Release(r);
        }
        ITextDocument_Undo(doc, tomResume, NULL);
        ITextDocument_Unfreeze(doc, &frozen);
        ITextDocument_Release(doc);
    }
    if (readOnly)
        Real_SendMessageW(hwnd, EM_SETREADONLY, TRUE, 0);
    ole->lpVtbl->Release(ole);
}

// ---------------------------------------------------------------------------
// Theme class redirection for AirDC's own uxtheme drawing

static const WCHAR *const kDarkThemeClasses[] = {
    L"Button", L"Combobox", L"Edit", L"Header", L"ListView", L"Listbox",
    L"Progress", L"Rebar", L"ScrollBar", L"Spin", L"Static", L"Status",
    L"Tab", L"TaskDialog", L"Toolbar", L"Tooltip", L"TreeView",
};

static BOOL DarkClassList(LPCWSTR classes, WCHAR *out, int cap)
{
    BOOL changed = FALSE;
    int o = 0;
    const WCHAR *p = classes;
    while (*p) {
        const WCHAR *end = p;
        while (*end && *end != L';')
            end++;
        int len = (int)(end - p);
        BOOL qualified = FALSE, known = FALSE;
        for (int i = 0; i < len; i++)
            if (p[i] == L':')
                qualified = TRUE;
        if (!qualified)
            for (int k = 0; k < ARRAYSIZE(kDarkThemeClasses); k++)
                if (lstrlenW(kDarkThemeClasses[k]) == len &&
                    CompareStringOrdinal(p, len, kDarkThemeClasses[k], len, TRUE) == CSTR_EQUAL)
                    known = TRUE;
        static const WCHAR prefix[] = L"DarkMode_DarkTheme::";
        int need = (known ? ARRAYSIZE(prefix) - 1 : 0) + len + 2;
        if (o + need >= cap)
            return FALSE;
        if (known) {
            lstrcpyW(out + o, prefix);
            o += ARRAYSIZE(prefix) - 1;
            changed = TRUE;
        }
        CopyMemory(out + o, p, len * sizeof(WCHAR));
        o += len;
        if (*end)
            out[o++] = L';';
        p = *end ? end + 1 : end;
    }
    out[o] = 0;
    return changed;
}

// "Window" theme handles AirDC opened: the command bar draws the MDI child
// buttons with them.
static HTHEME g_windowThemes[16];

static HTHEME WINAPI Hook_OpenThemeData(HWND hwnd, LPCWSTR classes)
{
    WCHAR buf[512];
    HTHEME t = NULL;
    if (g_dark && classes && DarkClassList(classes, buf, ARRAYSIZE(buf)))
        t = Real_OpenThemeData(hwnd, buf);
    if (!t)
        t = Real_OpenThemeData(hwnd, classes);
    if (t && classes && !lstrcmpiW(classes, L"Window")) {
        for (int i = 0; i < ARRAYSIZE(g_windowThemes); i++)
            if (!g_windowThemes[i] || g_windowThemes[i] == t) {
                g_windowThemes[i] = t;
                break;
            }
    }
    return t;
}

static HRESULT WINAPI Hook_DrawThemeBackground(HTHEME theme, HDC dc, int part, int state, LPCRECT rc, LPCRECT clip)
{
    if (g_dark && theme) {
        BOOL window = FALSE;
        for (int i = 0; i < ARRAYSIZE(g_windowThemes); i++)
            if (g_windowThemes[i] == theme)
                window = TRUE;
        WCHAR glyph = 0;
        if (window) {
            switch (part) {
            case WP_MDIMINBUTTON:     glyph = L'0'; break;
            case WP_MDICLOSEBUTTON:   glyph = L'r'; break;
            case WP_MDIRESTOREBUTTON: glyph = L'2'; break;
            case WP_MDIHELPBUTTON:    glyph = L's'; break;
            }
        }
        if (glyph) {
            // States: 1 normal, 2 hot, 3 pushed, 4 disabled.
            COLORREF bg = FlippedSys(COLOR_BTNFACE);
            if (state == 2)
                bg = Blend(bg, RGB(255, 255, 255), 88);
            else if (state == 3)
                bg = Blend(bg, RGB(255, 255, 255), 78);
            if (part == WP_MDICLOSEBUTTON && (state == 2 || state == 3))
                bg = RGB(196, 43, 28);
            DrawCaptionGlyph(dc, rc, glyph, bg, state == 4 ? RGB(110, 110, 110) : RGB(225, 225, 225));
            return S_OK;
        }
    }
    return Real_DrawThemeBackground(theme, dc, part, state, rc, clip);
}

static HTHEME WINAPI Hook_OpenThemeDataEx(HWND hwnd, LPCWSTR classes, DWORD flags)
{
    WCHAR buf[512];
    if (g_dark && classes && DarkClassList(classes, buf, ARRAYSIZE(buf))) {
        HTHEME t = Real_OpenThemeDataEx(hwnd, buf, flags);
        if (t)
            return t;
    }
    return Real_OpenThemeDataEx(hwnd, classes, flags);
}

static HTHEME WINAPI Hook_OpenThemeDataForDpi(HWND hwnd, LPCWSTR classes, UINT dpi)
{
    WCHAR buf[512];
    if (g_dark && classes && DarkClassList(classes, buf, ARRAYSIZE(buf))) {
        HTHEME t = Real_OpenThemeDataForDpi(hwnd, buf, dpi);
        if (t)
            return t;
    }
    return Real_OpenThemeDataForDpi(hwnd, classes, dpi);
}

static FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC p = Real_GetProcAddress(module, name);
    if (!p || IS_INTRESOURCE(name) || !FromExe(_ReturnAddress()))
        return p;
    if (!lstrcmpA(name, "OpenThemeDataEx")) {
        Real_OpenThemeDataEx = (void *)p;
        return (FARPROC)Hook_OpenThemeDataEx;
    }
    if (!lstrcmpA(name, "OpenThemeDataForDpi")) {
        Real_OpenThemeDataForDpi = (void *)p;
        return (FARPROC)Hook_OpenThemeDataForDpi;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Borders and spin buttons drawn by comctl32 and rich edit: even the dark
// themes draw edit/combo borders and up-down buttons light grey.

typedef enum { TK_NONE, TK_EDIT, TK_COMBO, TK_SPIN, TK_GROUPBOX, TK_LISTVIEW, TK_DATEPICKER, TK_TOOLBAR, TK_STATUS } ThemeKind;

static void PatchImportByAddress(HMODULE module, FARPROC target, void *hook);

static HTHEME(WINAPI *LibReal_OpenThemeData)(HWND, LPCWSTR);
static HTHEME(WINAPI *LibReal_OpenThemeDataForDpi)(HWND, LPCWSTR, UINT);
static HRESULT(WINAPI *LibReal_CloseThemeData)(HTHEME);
static HRESULT(WINAPI *LibReal_DrawThemeBackground)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
static HRESULT(WINAPI *LibReal_GetThemeColor)(HTHEME, int, int, int, COLORREF *);
static HRESULT(WINAPI *LibReal_DrawThemeText)(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
static HRESULT(WINAPI *LibReal_DrawThemeTextEx)(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS *);

static struct { HTHEME t; ThemeKind k; } g_themeKinds[256];
static SRWLOCK g_themeLock = SRWLOCK_INIT;

// Kind of the first class in a "Class1;Class2" list ("Sub::Class" allowed).
static ThemeKind ClassKind(LPCWSTR classes)
{
    if (!classes)
        return TK_NONE;
    const WCHAR *p = classes, *end = classes;
    while (*end && *end != L';')
        end++;
    for (const WCHAR *q = p; q + 1 < end; q++)
        if (q[0] == L':' && q[1] == L':')
            p = q + 2;
    int len = (int)(end - p);
    static const struct { const WCHAR *name; ThemeKind k; } names[] = {
        { L"Edit", TK_EDIT }, { L"Combobox", TK_COMBO }, { L"Spin", TK_SPIN }, { L"Button", TK_GROUPBOX },
        { L"ListView", TK_LISTVIEW }, { L"DatePicker", TK_DATEPICKER },
        { L"Toolbar", TK_TOOLBAR }, { L"Status", TK_STATUS },
    };
    for (int i = 0; i < ARRAYSIZE(names); i++)
        if (lstrlenW(names[i].name) == len && CompareStringOrdinal(p, len, names[i].name, len, TRUE) == CSTR_EQUAL)
            return names[i].k;
    return TK_NONE;
}

static void RecordTheme(HTHEME t, LPCWSTR classes)
{
    ThemeKind k = ClassKind(classes);
    if (!t || k == TK_NONE)
        return;
    AcquireSRWLockExclusive(&g_themeLock);
    int slot = -1;
    for (int i = 0; i < ARRAYSIZE(g_themeKinds); i++) {
        if (g_themeKinds[i].t == t) {
            slot = i;
            break;
        }
        if (!g_themeKinds[i].t && slot < 0)
            slot = i;
    }
    if (slot >= 0) {
        g_themeKinds[slot].t = t;
        g_themeKinds[slot].k = k;
    }
    ReleaseSRWLockExclusive(&g_themeLock);
}

static ThemeKind KindOfTheme(HTHEME t)
{
    ThemeKind k = TK_NONE;
    AcquireSRWLockShared(&g_themeLock);
    for (int i = 0; i < ARRAYSIZE(g_themeKinds); i++)
        if (g_themeKinds[i].t == t) {
            k = g_themeKinds[i].k;
            break;
        }
    ReleaseSRWLockShared(&g_themeLock);
    return k;
}

static HTHEME WINAPI Lib_OpenThemeData(HWND hwnd, LPCWSTR classes)
{
    HTHEME t = LibReal_OpenThemeData(hwnd, classes);
    TRACE("open %ls -> %p\r\n", classes ? classes : L"(null)", t);
    RecordTheme(t, classes);
    return t;
}

static HTHEME WINAPI Lib_OpenThemeDataForDpi(HWND hwnd, LPCWSTR classes, UINT dpi)
{
    HTHEME t = LibReal_OpenThemeDataForDpi(hwnd, classes, dpi);
    TRACE("openDpi %ls -> %p\r\n", classes ? classes : L"(null)", t);
    RecordTheme(t, classes);
    return t;
}

static HRESULT WINAPI Lib_CloseThemeData(HTHEME t)
{
    AcquireSRWLockExclusive(&g_themeLock);
    for (int i = 0; i < ARRAYSIZE(g_themeKinds); i++)
        if (g_themeKinds[i].t == t)
            g_themeKinds[i].t = NULL;
    ReleaseSRWLockExclusive(&g_themeLock);
    return LibReal_CloseThemeData(t);
}

#define BORDER_HOT    RGB(85, 85, 85)
#define STATUS_BG     RGB(44, 44, 44)  // the dark theme's status bar fill

static void FlatBox(HDC dc, const RECT *rc, COLORREF border, COLORREF fill)
{
    RECT in = *rc;
    InflateRect(&in, -1, -1);
    Real_FillRect(dc, &in, SolidBrush(fill));
    Real_FrameRect(dc, rc, SolidBrush(border));
}

// TRUE when the part was drawn here.
static BOOL DrawDarkPart(ThemeKind k, HDC dc, int part, int state, const RECT *rc)
{
    COLORREF field = FlippedSys(COLOR_WINDOW), face = FlippedSys(COLOR_BTNFACE);
    switch (k) {
    case TK_EDIT:
        if (part == EP_EDITTEXT) {
            // States: 1 normal, 2 hot, 3 selected, 4 disabled, 5 focused, 6 read-only.
            BOOL hot = state == ETS_HOT || state == ETS_FOCUSED || state == ETS_SELECTED;
            FlatBox(dc, rc, hot ? BORDER_HOT : BORDER_NORMAL,
                    state == ETS_DISABLED || state == ETS_READONLY ? face : field);
            return TRUE;
        }
        if (part >= EP_EDITBORDER_NOSCROLL && part <= EP_EDITBORDER_HVSCROLL) {
            // States: 1 normal, 2 hot, 3 focused, 4 disabled.
            FlatBox(dc, rc, state == 2 || state == 3 ? BORDER_HOT : BORDER_NORMAL, state == 4 ? face : field);
            return TRUE;
        }
        break;
    case TK_COMBO:
        if (part == CP_BORDER) {
            FlatBox(dc, rc, state == 2 || state == 3 ? BORDER_HOT : BORDER_NORMAL, state == 4 ? face : field);
            return TRUE;
        }
        if (part == CP_READONLY) {
            // Drop-down list face: 1 normal, 2 hot, 3 pressed, 4 disabled.
            COLORREF fill = state == 2 ? Blend(face, RGB(255, 255, 255), 92) :
                            state == 3 ? Blend(face, RGB(255, 255, 255), 85) : face;
            FlatBox(dc, rc, state == 2 || state == 3 ? BORDER_HOT : BORDER_NORMAL, fill);
            return TRUE;
        }
        break;
    case TK_SPIN: {
        WCHAR glyph = part == SPNP_UP ? L'5' : part == SPNP_DOWN ? L'6' :
                      part == SPNP_UPHORZ ? L'4' : part == SPNP_DOWNHORZ ? L'3' : 0;
        if (!glyph)
            break;
        COLORREF bg = state == 2 ? Blend(face, RGB(255, 255, 255), 88) :
                      state == 3 ? Blend(face, RGB(255, 255, 255), 78) : face;
        DrawCaptionGlyph(dc, rc, glyph, bg, state == 4 ? RGB(110, 110, 110) : RGB(200, 200, 200));
        Real_FrameRect(dc, rc, SolidBrush(BORDER_NORMAL));
        return TRUE;
    }
    case TK_DATEPICKER:
        // States: 1 normal, 2 hot/focused, 3 pressed, 4 disabled.
        if (part == DP_DATEBORDER) {
            FlatBox(dc, rc, state == 2 || state == 3 ? BORDER_HOT : BORDER_NORMAL, state == 4 ? face : field);
            return TRUE;
        }
        if (part == DP_DATETEXT) {
            Real_FillRect(dc, rc, SolidBrush(state == 4 ? face : field));
            return TRUE;
        }
        if (part == DP_SHOWCALENDARBUTTONRIGHT) {
            COLORREF bg = state == 2 ? Blend(field, RGB(255, 255, 255), 88) :
                          state == 3 ? Blend(field, RGB(255, 255, 255), 78) : field;
            DrawCaptionGlyph(dc, rc, L'6', bg, state == 4 ? RGB(110, 110, 110) : RGB(200, 200, 200));
            return TRUE;
        }
        break;
    case TK_STATUS:
        // The dark status bar background has a grey line along its top.
        if (part == 0) {
            Real_FillRect(dc, rc, SolidBrush(STATUS_BG));
            return TRUE;
        }
        break;
    case TK_TOOLBAR:
        // Checked buttons (the open frames) get no box: their icons sit on the
        // toolbar background. Hot and pressed states keep the theme's look.
        if ((part == TP_BUTTON || part == TP_DROPDOWNBUTTON || part == TP_SPLITBUTTON ||
             part == TP_SPLITBUTTONDROPDOWN) && state == TS_CHECKED)
            return TRUE;
        if (part == TP_SEPARATOR || part == TP_SEPARATORVERT) {
            RECT line = *rc;
            if (part == TP_SEPARATOR) {
                line.left = (rc->left + rc->right) / 2;
                line.right = line.left + 1;
                InflateRect(&line, 0, -(rc->bottom - rc->top) / 6);
            } else {
                line.top = (rc->top + rc->bottom) / 2;
                line.bottom = line.top + 1;
                InflateRect(&line, -(rc->right - rc->left) / 6, 0);
            }
            Real_FillRect(dc, &line, SolidBrush(BORDER_NORMAL));
            return TRUE;
        }
        break;
    case TK_GROUPBOX:
        if (part == BP_GROUPBOX) {
            // Frame only: the caption gap is clipped out by the caller.
            Real_FrameRect(dc, rc, SolidBrush(BORDER_NORMAL));
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

// Kind from the window being painted, for theme handles opened before the
// imports were patched (or through other entry points).
static ThemeKind KindOfDC(HDC dc)
{
    HWND hwnd = WindowFromDC(dc);
    WCHAR cls[32];
    if (!hwnd || !RealGetWindowClassW(hwnd, cls, ARRAYSIZE(cls)))
        return TK_NONE;
    if (!lstrcmpiW(cls, UPDOWN_CLASSW))
        return TK_SPIN;
    if (!lstrcmpiW(cls, STATUSCLASSNAMEW))
        return TK_STATUS;
    if (!lstrcmpiW(cls, WC_EDITW))
        return TK_EDIT;
    if (!lstrcmpiW(cls, WC_COMBOBOXW))
        return TK_COMBO;
    if (!lstrcmpiW(cls, WC_BUTTONW) && (GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX)
        return TK_GROUPBOX;
    return TK_NONE;
}

static HRESULT WINAPI Lib_DrawThemeBackground(HTHEME theme, HDC dc, int part, int state, LPCRECT rc, LPCRECT clip)
{
    ThemeKind k;
    if (g_dark && rc && (((k = KindOfTheme(theme)) != TK_NONE && k != TK_LISTVIEW) || (k = KindOfDC(dc)) != TK_NONE)) {
        int saved = SaveDC(dc);
        if (clip)
            IntersectClipRect(dc, clip->left, clip->top, clip->right, clip->bottom);
        BOOL done = DrawDarkPart(k, dc, part, state, rc);
        RestoreDC(dc, saved);
        if (done)
            return S_OK;
    }
    return LibReal_DrawThemeBackground(theme, dc, part, state, rc, clip);
}

// List view group headers (the Extensions frame's "Installed"/"Not installed")
// take their text color from the theme: navy even in the dark themes.
static BOOL IsGroupHeader(HTHEME theme, int part);

static HRESULT WINAPI Lib_GetThemeColor(HTHEME theme, int part, int state, int prop, COLORREF *color)
{
    HRESULT hr = LibReal_GetThemeColor(theme, part, state, prop, color);
    TRACE("color %p part %d prop %d kind %d\r\n", theme, part, prop, KindOfTheme(theme));
    if (SUCCEEDED(hr) && color && IsGroupHeader(theme, part) &&
        GetRValue(*color) + GetGValue(*color) + GetBValue(*color) < 3 * 128)
        *color = TextFwd(*color);
    return hr;
}

// Theme text drawn in the theme's own (dark) color: list view group headers
// and the date picker's date.
static BOOL IsGroupHeader(HTHEME theme, int part)
{
    if (!g_dark)
        return FALSE;
    ThemeKind k = KindOfTheme(theme);
    return (k == TK_LISTVIEW && part == LVP_GROUPHEADER) || k == TK_DATEPICKER;
}

// ... and draw the header text with DrawThemeText(Ex), which reads the color
// inside uxtheme: pass it explicitly.
static HRESULT WINAPI Lib_DrawThemeTextEx(HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int len,
                                          DWORD flags, LPRECT rc, const DTTOPTS *opts)
{
    TRACE("textex %p part %d kind %d\r\n", theme, part, KindOfTheme(theme));
    if (!IsGroupHeader(theme, part))
        return LibReal_DrawThemeTextEx(theme, dc, part, state, text, len, flags, rc, opts);
    DTTOPTS o = { sizeof(o) };
    if (opts)
        memcpy(&o, opts, min(opts->dwSize, sizeof(o)));
    o.dwSize = sizeof(o);
    COLORREF c = o.crText;
    if (!(o.dwFlags & DTT_TEXTCOLOR) && FAILED(LibReal_GetThemeColor(theme, part, state, TMT_HEADING1TEXTCOLOR, &c)) &&
        FAILED(LibReal_GetThemeColor(theme, part, state, TMT_TEXTCOLOR, &c)))
        c = RGB(0, 51, 153);
    o.crText = TextFwd(c);
    // DTT_COLORPROP (a theme color property) would override crText.
    o.dwFlags = (o.dwFlags | DTT_TEXTCOLOR) & ~DTT_COLORPROP;
    return LibReal_DrawThemeTextEx(theme, dc, part, state, text, len, flags, rc, &o);
}

static HRESULT WINAPI Lib_DrawThemeText(HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int len,
                                        DWORD flags, DWORD flags2, LPCRECT rc)
{
    TRACE("text %p part %d kind %d\r\n", theme, part, KindOfTheme(theme));
    if (!IsGroupHeader(theme, part))
        return LibReal_DrawThemeText(theme, dc, part, state, text, len, flags, flags2, rc);
    RECT r = *rc;
    return Lib_DrawThemeTextEx(theme, dc, part, state, text, len, flags, &r, NULL);
}

// comctl32 and rich edit delay-load uxtheme (rich edit through an API set):
// patch those slots by name, resolved or not.
static void PatchDelayImport(HMODULE module, const char *dllPart, const char *func, void *hook)
{
    BYTE *base = (BYTE *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress)
        return;
    int partLen = lstrlenA(dllPart);
    for (IMAGE_DELAYLOAD_DESCRIPTOR *d = (IMAGE_DELAYLOAD_DESCRIPTOR *)(base + dir.VirtualAddress); d->DllNameRVA; d++) {
        const char *dll = (const char *)(base + d->DllNameRVA);
        BOOL match = FALSE;
        for (const char *p = dll; *p && !match; p++)
            match = CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, p, min(partLen, lstrlenA(p)), dllPart, partLen) == CSTR_EQUAL;
        if (!match)
            continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->ImportNameTableRVA);
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->ImportAddressTableRVA);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (lstrcmpA((const char *)ibn->Name, func))
                continue;
            DWORD old;
            if (VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                slots->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            }
        }
    }
}

static void PatchThemeImports(HMODULE module)
{
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux)
        return;
    if (!LibReal_OpenThemeData) {
        LibReal_OpenThemeData = (void *)GetProcAddress(ux, "OpenThemeData");
        LibReal_OpenThemeDataForDpi = (void *)GetProcAddress(ux, "OpenThemeDataForDpi");
        LibReal_CloseThemeData = (void *)GetProcAddress(ux, "CloseThemeData");
        LibReal_DrawThemeBackground = (void *)GetProcAddress(ux, "DrawThemeBackground");
        LibReal_GetThemeColor = (void *)GetProcAddress(ux, "GetThemeColor");
        LibReal_DrawThemeText = (void *)GetProcAddress(ux, "DrawThemeText");
        LibReal_DrawThemeTextEx = (void *)GetProcAddress(ux, "DrawThemeTextEx");
    }
    PatchImportByAddress(module, (FARPROC)LibReal_OpenThemeData, Lib_OpenThemeData);
    PatchImportByAddress(module, (FARPROC)LibReal_OpenThemeDataForDpi, Lib_OpenThemeDataForDpi);
    PatchImportByAddress(module, (FARPROC)LibReal_CloseThemeData, Lib_CloseThemeData);
    PatchImportByAddress(module, (FARPROC)LibReal_DrawThemeBackground, Lib_DrawThemeBackground);
    PatchImportByAddress(module, (FARPROC)LibReal_GetThemeColor, Lib_GetThemeColor);
    PatchImportByAddress(module, (FARPROC)LibReal_DrawThemeText, Lib_DrawThemeText);
    PatchImportByAddress(module, (FARPROC)LibReal_DrawThemeTextEx, Lib_DrawThemeTextEx);
    PatchDelayImport(module, "uxtheme", "OpenThemeData", Lib_OpenThemeData);
    PatchDelayImport(module, "uxtheme", "OpenThemeDataForDpi", Lib_OpenThemeDataForDpi);
    PatchDelayImport(module, "uxtheme", "CloseThemeData", Lib_CloseThemeData);
    PatchDelayImport(module, "uxtheme", "DrawThemeBackground", Lib_DrawThemeBackground);
    PatchDelayImport(module, "uxtheme", "GetThemeColor", Lib_GetThemeColor);
    PatchDelayImport(module, "uxtheme", "DrawThemeText", Lib_DrawThemeText);
    PatchDelayImport(module, "uxtheme", "DrawThemeTextEx", Lib_DrawThemeTextEx);
}

// ---------------------------------------------------------------------------
// uxtheme private dark mode API

typedef BOOL (WINAPI *AllowDarkModeForWindow_t)(HWND, BOOL);
typedef PreferredAppMode (WINAPI *SetPreferredAppMode_t)(PreferredAppMode);
typedef void (WINAPI *FlushMenuThemes_t)(void);
typedef void (WINAPI *RefreshImmersiveColorPolicyState_t)(void);

static AllowDarkModeForWindow_t pAllowDarkModeForWindow;
static SetPreferredAppMode_t pSetPreferredAppMode;
static FlushMenuThemes_t pFlushMenuThemes;
static RefreshImmersiveColorPolicyState_t pRefreshImmersiveColorPolicyState;

static void ApplyAppMode(void)
{
    if (pSetPreferredAppMode)
        pSetPreferredAppMode(g_dark ? AppModeForceDark : AppModeForceLight);
    if (pRefreshImmersiveColorPolicyState)
        pRefreshImmersiveColorPolicyState();
    if (pFlushMenuThemes)
        pFlushMenuThemes();
}

// ---------------------------------------------------------------------------
// Menu bar (undocumented UAH messages, as used by Notepad++ and others)

#define WM_UAHDRAWMENU         0x0091
#define WM_UAHDRAWMENUITEM     0x0092

typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef union { struct { DWORD cx, cy; } rgsizeBar[2]; struct { DWORD cx, cy; } rgsizePopup[4]; } UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; } UAHMENUPOPUPMETRICS;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

#define MENUBAR_BG   RGB(32, 32, 32)
#define MENUBAR_HOT  RGB(62, 62, 62)
#define MENUBAR_TEXT RGB(230, 230, 230)

// ---------------------------------------------------------------------------
// Popup menus. Windows draws them with its own dark theme, which selects with
// a faint grey fill, or a white outline when the menu was opened from the
// keyboard or a button. Each popup (menu bar drop-downs, context menus,
// submenus) has its plain items made owner-drawn when it opens, after every
// handler of WM_INITMENUPOPUP has run (WTL's command bar gives the menu bar
// items their icon bitmaps there), and drawn here with their icons
// (accent-blue selection, dark separators); they are put back when the menu
// loop ends. Items that are already owner-drawn
// (AirDC's menu titles, the command bar's icon menus) are left alone, and item
// data is never touched: AirDC keeps its own there.

#define MENU_BG   RGB(43, 43, 43)
#define MENU_TEXT RGB(230, 230, 230)
#define MENU_GRAY RGB(120, 120, 120)
#define MENU_LINE RGB(64, 64, 64)

typedef struct {
    HMENU menu;
    UINT pos;
    UINT id;  // command id, or the submenu handle for items that open one
    ULONG_PTR data;
    UINT type;  // original fType
    HBITMAP bmp;
    WCHAR text[256];
} OdItem;

static OdItem g_od[1024];
static int g_odCount;
static struct { HMENU menu; MENUINFO mi; } g_odMenus[64];
static int g_odMenuCount;
static HFONT g_menuFont, g_menuBold, g_menuGlyphs;

static int MenuScale(int v)
{
    return MulDiv(v, GetDpiForSystem(), 96);
}

static COLORREF MenuSelection(void)
{
    BOOL mapped;
    return DarkSysColor(COLOR_HIGHLIGHT, &mapped);
}

static void MenuFonts(void)
{
    if (g_menuFont)
        return;
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_menuFont = CreateFontIndirectW(&ncm.lfMenuFont);
    ncm.lfMenuFont.lfWeight = FW_BOLD;
    g_menuBold = CreateFontIndirectW(&ncm.lfMenuFont);
    g_menuGlyphs = CreateFontW(-MenuScale(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, SYMBOL_CHARSET, 0, 0, 0, 0, L"Marlett");
}

static void OwnerDrawMenu(HMENU menu)
{
    if (!menu || !IsMenu(menu) || g_odMenuCount == ARRAYSIZE(g_odMenus))
        return;
    for (int i = 0; i < g_odMenuCount; i++)
        if (g_odMenus[i].menu == menu)
            return;  // a submenu shared by several items
    g_odMenus[g_odMenuCount].menu = menu;
    g_odMenus[g_odMenuCount].mi.cbSize = sizeof(MENUINFO);
    g_odMenus[g_odMenuCount].mi.fMask = MIM_BACKGROUND;
    GetMenuInfo(menu, &g_odMenus[g_odMenuCount].mi);
    g_odMenuCount++;
    MENUINFO mi = { sizeof(mi), MIM_BACKGROUND };
    mi.hbrBack = SolidBrush(MENU_BG);
    SetMenuInfo(menu, &mi);

    int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; i++) {
        MENUITEMINFOW bm = { sizeof(bm) };
        bm.fMask = MIIM_BITMAP;
        if (GetMenuItemInfoW(menu, i, TRUE, &bm) && bm.hbmpItem == HBMMENU_CALLBACK) {
            // The owner draws these bitmaps: leave the whole menu to Windows.
            SetMenuInfo(menu, &g_odMenus[--g_odMenuCount].mi);
            return;
        }
    }
    for (int i = 0; i < n; i++) {
        WCHAR text[256] = L"";
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU | MIIM_DATA | MIIM_STRING | MIIM_BITMAP;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text);
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii))
            continue;
        if ((mii.fType & (MFT_OWNERDRAW | MFT_BITMAP)) || g_odCount == ARRAYSIZE(g_od))
            continue;
        MENUITEMINFOW set = { sizeof(set) };
        set.fMask = MIIM_FTYPE | (mii.hbmpItem ? MIIM_BITMAP : 0);
        set.fType = mii.fType | MFT_OWNERDRAW;
        set.hbmpItem = NULL;
        if (!SetMenuItemInfoW(menu, i, TRUE, &set))
            continue;
        OdItem *it = &g_od[g_odCount++];
        it->menu = menu;
        it->pos = i;
        it->id = mii.hSubMenu ? (UINT)(ULONG_PTR)mii.hSubMenu : mii.wID;
        it->data = mii.dwItemData;
        it->type = mii.fType;
        it->bmp = mii.hbmpItem;
        lstrcpynW(it->text, text, ARRAYSIZE(it->text));
    }
}

static void RestoreMenus(int items, int menus)
{
    while (g_odCount > items) {
        OdItem *it = &g_od[--g_odCount];
        if (!IsMenu(it->menu))
            continue;
        // AirDC may have inserted or removed items while the menu was open:
        // restore the item that is still ours, wherever it is now.
        int pos = -1, n = GetMenuItemCount(it->menu);
        for (int i = 0; i < n && pos < 0; i++) {
            int k = (it->pos + i) % n;
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU | MIIM_DATA;
            if (GetMenuItemInfoW(it->menu, k, TRUE, &mii) && (mii.fType & MFT_OWNERDRAW) && mii.dwItemData == it->data &&
                (mii.hSubMenu ? (UINT)(ULONG_PTR)mii.hSubMenu : mii.wID) == it->id)
                pos = k;
        }
        if (pos < 0)
            continue;
        it->pos = pos;
        MENUITEMINFOW set = { sizeof(set) };
        set.fMask = MIIM_FTYPE | (it->bmp ? MIIM_BITMAP : 0);
        set.fType = it->type;
        set.hbmpItem = it->bmp;
        if (!(it->type & MFT_SEPARATOR)) {
            set.fMask |= MIIM_STRING;
            set.dwTypeData = it->text;
        }
        SetMenuItemInfoW(it->menu, it->pos, TRUE, &set);
    }
    while (g_odMenuCount > menus) {
        g_odMenuCount--;
        if (IsMenu(g_odMenus[g_odMenuCount].menu))
            SetMenuInfo(g_odMenus[g_odMenuCount].menu, &g_odMenus[g_odMenuCount].mi);
    }
}

static OdItem *FindOdItem(HMENU menu, UINT id, ULONG_PTR data)
{
    for (int i = g_odCount - 1; i >= 0; i--)
        if (g_od[i].id == id && g_od[i].data == data && (!menu || g_od[i].menu == menu))
            return &g_od[i];
    return NULL;
}

static BOOL MeasureMenuItem(MEASUREITEMSTRUCT *mis)
{
    OdItem *it = FindOdItem(NULL, mis->itemID, mis->itemData);
    if (!it)
        return FALSE;
    if (it->type & MFT_SEPARATOR) {
        mis->itemWidth = 0;
        mis->itemHeight = MenuScale(9);
        return TRUE;
    }
    MenuFonts();
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, g_menuBold);  // bold: default items are wider
    WCHAR *tab = wcschr(it->text, L'\t');
    int len = tab ? (int)(tab - it->text) : lstrlenW(it->text);
    RECT a = { 0 }, b = { 0 };
    DrawTextW(dc, it->text, len, &a, DT_CALCRECT | DT_SINGLELINE);
    if (tab)
        DrawTextW(dc, tab + 1, -1, &b, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    mis->itemWidth = MenuScale(36) + a.right + (tab ? MenuScale(32) + b.right : 0) + MenuScale(20);
    mis->itemHeight = max(a.bottom + MenuScale(10), MenuScale(26));
    return TRUE;
}

static BOOL DrawOdMenuItem(const DRAWITEMSTRUCT *d)
{
    OdItem *it = FindOdItem((HMENU)d->hwndItem, d->itemID, d->itemData);
    if (!it)
        return FALSE;
    HDC dc = d->hDC;
    RECT rc = d->rcItem;
    Real_FillRect(dc, &rc, SolidBrush(MENU_BG));
    if (it->type & MFT_SEPARATOR) {
        int y = (rc.top + rc.bottom) / 2;
        RECT line = { rc.left + MenuScale(10), y, rc.right - MenuScale(10), y + 1 };
        Real_FillRect(dc, &line, SolidBrush(MENU_LINE));
        return TRUE;
    }
    BOOL disabled = (d->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    if ((d->itemState & ODS_SELECTED) && !disabled) {
        HGDIOBJ oldBrush = SelectObject(dc, SolidBrush(MenuSelection()));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, rc.left + MenuScale(4), rc.top + MenuScale(1), rc.right - MenuScale(4) + 1, rc.bottom - MenuScale(1) + 1,
                  MenuScale(8), MenuScale(8));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }
    COLORREF fg = disabled ? MENU_GRAY : (d->itemState & ODS_SELECTED) ? RGB(255, 255, 255) : PaletteText(MENU_TEXT);
    MenuFonts();
    int oldMode = SetBkMode(dc, TRANSPARENT);
    COLORREF oldFg = Real_SetTextColor(dc, fg);

    RECT gutter = { rc.left + MenuScale(8), rc.top, rc.left + MenuScale(32), rc.bottom };
    if (d->itemState & ODS_CHECKED) {
        WCHAR glyph = (it->type & MFT_RADIOCHECK) ? L'h' : L'a';
        HGDIOBJ old = SelectObject(dc, g_menuGlyphs);
        DrawTextW(dc, &glyph, 1, &gutter, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    } else if (it->bmp && (ULONG_PTR)it->bmp > (ULONG_PTR)HBMMENU_POPUP_MINIMIZE) {
        BITMAP bm;
        if (GetObjectW(it->bmp, sizeof(bm), &bm)) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, it->bmp);
            int x = (gutter.left + gutter.right - bm.bmWidth) / 2, y = (rc.top + rc.bottom - bm.bmHeight) / 2;
            if (bm.bmBitsPixel == 32) {
                BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                GdiAlphaBlend(dc, x, y, bm.bmWidth, bm.bmHeight, mem, 0, 0, bm.bmWidth, bm.bmHeight, bf);
            } else {
                BitBlt(dc, x, y, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
            }
            SelectObject(mem, old);
            DeleteDC(mem);
        }
    }

    HGDIOBJ oldFont = SelectObject(dc, (d->itemState & ODS_DEFAULT) ? g_menuBold : g_menuFont);
    UINT fmt = DT_SINGLELINE | DT_VCENTER | ((d->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0);
    RECT text = { rc.left + MenuScale(36), rc.top, rc.right - MenuScale(20), rc.bottom };
    WCHAR *tab = wcschr(it->text, L'\t');
    DrawTextW(dc, it->text, tab ? (int)(tab - it->text) : -1, &text, fmt | DT_LEFT);
    if (tab) {
        if (!disabled && !(d->itemState & ODS_SELECTED))
            Real_SetTextColor(dc, RGB(160, 160, 160));
        DrawTextW(dc, tab + 1, -1, &text, fmt | DT_RIGHT | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
    Real_SetTextColor(dc, oldFg);
    SetBkMode(dc, oldMode);
    return TRUE;
}

static BOOL (WINAPI *Real_TrackPopupMenu)(HMENU, UINT, int, int, int, HWND, const RECT *);
static BOOL (WINAPI *Real_TrackPopupMenuEx)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);

static BOOL WINAPI Hook_TrackPopupMenu(HMENU menu, UINT flags, int x, int y, int reserved, HWND hwnd, const RECT *rc)
{
    // Items are converted on WM_INITMENUPOPUP; this only makes sure they are
    // put back even if the owner never saw WM_EXITMENULOOP.
    int items = g_odCount, menus = g_odMenuCount;
    BOOL r = Real_TrackPopupMenu(menu, flags, x, y, reserved, hwnd, rc);
    RestoreMenus(items, menus);
    return r;
}

static BOOL WINAPI Hook_TrackPopupMenuEx(HMENU menu, UINT flags, int x, int y, HWND hwnd, LPTPMPARAMS params)
{
    int items = g_odCount, menus = g_odMenuCount;
    BOOL r = Real_TrackPopupMenuEx(menu, flags, x, y, hwnd, params);
    RestoreMenus(items, menus);
    return r;
}

static void MenuBarRect(HWND hwnd, RECT *rc)
{
    MENUBARINFO mbi = { sizeof(mbi) };
    RECT wr;
    GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
    GetWindowRect(hwnd, &wr);
    *rc = mbi.rcBar;
    OffsetRect(rc, -wr.left, -wr.top);
}

static BOOL DrawMenuBarItem(UAHDRAWMENUITEM *d)
{
    WCHAR text[256];
    MENUITEMINFOW mii = { sizeof(mii), MIIM_STRING | MIIM_FTYPE | MIIM_BITMAP };
    mii.dwTypeData = text;
    mii.cch = ARRAYSIZE(text) - 1;
    if (!GetMenuItemInfoW(d->um.hmenu, d->umi.iPosition, TRUE, &mii) || !(mii.fType == MFT_STRING) || mii.hbmpItem)
        return FALSE;  // MDI child icon/buttons and other bitmap items: let Windows draw them

    UINT state = d->dis.itemState;
    COLORREF bg = MENUBAR_BG, fg = MENUBAR_TEXT;
    if (state & (ODS_HOTLIGHT | ODS_SELECTED))
        bg = MENUBAR_HOT;
    if (state & (ODS_GRAYED | ODS_DISABLED | ODS_INACTIVE))
        fg = RGB(130, 130, 130);

    Real_FillRect(d->um.hdc, &d->dis.rcItem, SolidBrush(bg));
    int oldMode = SetBkMode(d->um.hdc, TRANSPARENT);
    COLORREF oldFg = Real_SetTextColor(d->um.hdc, fg);
    UINT fmt = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
    if (state & ODS_NOACCEL)
        fmt |= DT_HIDEPREFIX;
    DrawTextW(d->um.hdc, text, -1, &d->dis.rcItem, fmt);
    Real_SetTextColor(d->um.hdc, oldFg);
    SetBkMode(d->um.hdc, oldMode);
    return TRUE;
}

// Windows draws a light line under the menu bar after non-client painting.
static void PaintMenuBarLine(HWND hwnd)
{
    if (!GetMenu(hwnd))
        return;
    RECT client, wr;
    GetClientRect(hwnd, &client);
    MapWindowPoints(hwnd, NULL, (POINT *)&client, 2);
    GetWindowRect(hwnd, &wr);
    OffsetRect(&client, -wr.left, -wr.top);
    RECT line = { client.left, client.top - 1, client.right, client.top };
    HDC dc = GetWindowDC(hwnd);
    Real_FillRect(dc, &line, SolidBrush(MENUBAR_BG));
    ReleaseDC(hwnd, dc);
}

// Sunken 3D client edges are drawn light by the system: one subtle border
// line, then the control's own background color.
static void PaintClientEdge(HWND hwnd)
{
    RECT wr;
    GetWindowRect(hwnd, &wr);
    OffsetRect(&wr, -wr.left, -wr.top);
    int cx = GetSystemMetrics(SM_CXEDGE), cy = GetSystemMetrics(SM_CYEDGE);
    HDC dc = GetWindowDC(hwnd);
    // The control's own background; sampling the client area picks up text
    // (the light block glyphs of hub ASCII banners) in chat views.
    COLORREF inner = CLR_INVALID;
    WCHAR prop[32];
    switch (WindowKind(hwnd)) {
    case KIND_RICHEDIT: {
        PropName(EM_SETBKGNDCOLOR, prop);
        ULONG_PTR v = (ULONG_PTR)GetPropW(hwnd, prop);
        inner = v ? Fwd((COLORREF)(v - 1)) : FlippedSys(COLOR_WINDOW);
        break;
    }
    case KIND_LISTVIEW:
        inner = (COLORREF)Real_SendMessageW(hwnd, LVM_GETBKCOLOR, 0, 0);
        break;
    case KIND_TREEVIEW:
        inner = (COLORREF)Real_SendMessageW(hwnd, TVM_GETBKCOLOR, 0, 0);
        break;
    default:
        inner = GetPixel(dc, wr.left + cx + 1, wr.top + cy + 1);
        break;
    }
    if (inner == CLR_INVALID || inner == CLR_NONE)
        inner = FlippedSys(COLOR_WINDOW);
    HBRUSH line = SolidBrush(BORDER_NORMAL), fill = SolidBrush(inner);
    Real_FrameRect(dc, &wr, line);
    for (int i = 1; i < max(cx, cy); i++) {
        InflateRect(&wr, -1, -1);
        Real_FrameRect(dc, &wr, fill);
    }
    ReleaseDC(hwnd, dc);
}

// MessageBox dialogs run user32's own dialog procedure.
static BOOL IsMessageBox(HWND hwnd)
{
    static MODULEINFO user32;
    if (!user32.lpBaseOfDll)
        GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(L"user32.dll"), &user32, sizeof(user32));
    ULONG_PTR proc = (ULONG_PTR)GetWindowLongPtrW(hwnd, DWLP_DLGPROC);
    ULONG_PTR base = (ULONG_PTR)user32.lpBaseOfDll;
    return proc >= base && proc < base + user32.SizeOfImage;
}

// user32 paints the message box footer light inside its own WM_PAINT: paint
// the content area and footer (which starts as far above the buttons as it
// ends below them) ourselves.
static void PaintMessageBox(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc, body;
    GetClientRect(hwnd, &rc);
    body = rc;
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        WCHAR cls[16];
        if (!RealGetWindowClassW(c, cls, ARRAYSIZE(cls)) || lstrcmpiW(cls, L"Button") ||
            (GetWindowLongW(c, GWL_STYLE) & BS_TYPEMASK) > BS_DEFPUSHBUTTON)
            continue;
        RECT b;
        GetWindowRect(c, &b);
        MapWindowPoints(NULL, hwnd, (POINT *)&b, 2);
        body.bottom = min(body.bottom, b.top - (rc.bottom - b.bottom));
    }
    RECT footer = { rc.left, body.bottom, rc.right, rc.bottom };
    Real_FillRect(dc, &body, SolidBrush(FlippedSys(COLOR_WINDOW)));
    Real_FillRect(dc, &footer, SolidBrush(FlippedSys(COLOR_BTNFACE)));
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Window subclass: dialog/control backgrounds, MDI client, menu bar.

// Text statics that are disabled: drawn here flat grey instead of embossed.
static BOOL IsDisabledTextStatic(HWND hwnd)
{
    if (IsWindowEnabled(hwnd) || !IsClass(hwnd, L"Static"))
        return FALSE;
    DWORD type = GetWindowLongW(hwnd, GWL_STYLE) & SS_TYPEMASK;
    return type == SS_LEFT || type == SS_CENTER || type == SS_RIGHT || type == SS_SIMPLE || type == SS_LEFTNOWORDWRAP;
}

static void PaintDisabledStatic(HWND hwnd, HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH br = (HBRUSH)SendMessageW(GetParent(hwnd), WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)hwnd);
    Real_FillRect(dc, &rc, br ? (HBRUSH)MapObject(br) : SolidBrush(FlippedSys(COLOR_BTNFACE)));

    DWORD style = GetWindowLongW(hwnd, GWL_STYLE), type = style & SS_TYPEMASK;
    UINT fmt = DT_EXPANDTABS;
    if (type == SS_CENTER)
        fmt |= DT_CENTER | DT_WORDBREAK;
    else if (type == SS_RIGHT)
        fmt |= DT_RIGHT | DT_WORDBREAK;
    else if (type == SS_LEFT)
        fmt |= DT_WORDBREAK;
    else
        fmt |= DT_SINGLELINE;
    if (style & SS_NOPREFIX)
        fmt |= DT_NOPREFIX;
    if (style & SS_CENTERIMAGE)
        fmt = (fmt & ~DT_WORDBREAK) | DT_SINGLELINE | DT_VCENTER;
    if (style & SS_ENDELLIPSIS)
        fmt |= DT_END_ELLIPSIS;
    if (style & SS_PATHELLIPSIS)
        fmt |= DT_PATH_ELLIPSIS;

    WCHAR text[1024];
    GetWindowTextW(hwnd, text, ARRAYSIZE(text));
    HGDIOBJ oldFont = SelectObject(dc, (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0));
    int oldMode = SetBkMode(dc, TRANSPARENT);
    COLORREF oldFg = Real_SetTextColor(dc, FlippedSys(COLOR_GRAYTEXT));
    DrawTextW(dc, text, -1, &rc, fmt);
    Real_SetTextColor(dc, oldFg);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

// Popup menus and combo box drop-down lists get a light grey 1px frame from
// Windows: fill their non-client area with their own background instead.
static BOOL PopupFrameColor(HWND hwnd, COLORREF *c)
{
    if (IsClass(hwnd, L"#32768")) {
        *c = MENU_BG;
        return TRUE;
    }
    if (IsClass(hwnd, L"ComboLBox")) {
        *c = FlippedSys(COLOR_WINDOW);
        return TRUE;
    }
    return FALSE;
}

static void PaintPopupFrame(HWND hwnd, HDC given, COLORREF c)
{
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    MapWindowPoints(hwnd, NULL, (POINT *)&cr, 2);
    OffsetRect(&cr, -wr.left, -wr.top);
    OffsetRect(&wr, -wr.left, -wr.top);
    if (EqualRect(&wr, &cr))
        return;
    HDC dc = given ? given : GetWindowDC(hwnd);
    int saved = SaveDC(dc);
    ExcludeClipRect(dc, cr.left, cr.top, cr.right, cr.bottom);
    Real_FillRect(dc, &wr, SolidBrush(c));
    RestoreDC(dc, saved);
    if (!given)
        ReleaseDC(hwnd, dc);
}

// Menu windows (#32768) are run by win32k: their messages never reach the
// hooks or subclasses here. Their owner gets WM_ENTERIDLE while a menu is open,
// so the frames of all open menus are painted over then.
static BOOL CALLBACK PaintMenuFrame(HWND hwnd, LPARAM lp)
{
    (void)lp;
    if (IsWindowVisible(hwnd) && IsClass(hwnd, L"#32768"))
        PaintPopupFrame(hwnd, NULL, MENU_BG);
    return TRUE;
}

// Windows 11's dark scroll bars show a thin light-grey thumb line while the
// mouse is elsewhere. It is repainted here in a darker shade after Windows
// draws it; the wide thumb shown while the mouse is over the bar is left alone.
#define SB_TRACK RGB(23, 23, 23)
#define SB_THUMB RGB(78, 78, 78)
#define SB_TIMER 0xDA4C
static const WCHAR kScrollHot[] = L"DarkMode.sbhot";

static void PaintThinThumbs(HWND hwnd)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!g_dark || !(style & (WS_HSCROLL | WS_VSCROLL)) || !IsWindowVisible(hwnd) || GetCapture() == hwnd ||
        GetPropW(hwnd, kScrollHot))
        return;
    Kind kind = WindowKind(hwnd);
    if (kind != KIND_LISTVIEW && kind != KIND_TREEVIEW && kind != KIND_RICHEDIT)
        return;
    RECT wr;
    GetWindowRect(hwnd, &wr);
    HDC dc = NULL;
    int thick = MulDiv(2, GetDpiForWindow(hwnd), 96);
    for (int vert = 0; vert < 2; vert++) {
        if (!(style & (vert ? WS_VSCROLL : WS_HSCROLL)))
            continue;
        SCROLLBARINFO sbi = { sizeof(sbi) };
        if (!GetScrollBarInfo(hwnd, vert ? OBJID_VSCROLL : OBJID_HSCROLL, &sbi) ||
            (sbi.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)))
            continue;
        RECT r = sbi.rcScrollBar;
        OffsetRect(&r, -wr.left, -wr.top);
        if (!dc)
            dc = GetWindowDC(hwnd);
        Real_FillRect(dc, &r, SolidBrush(SB_TRACK));
        if ((sbi.rgstate[0] & STATE_SYSTEM_UNAVAILABLE) || sbi.xyThumbBottom <= sbi.xyThumbTop)
            continue;
        RECT t;
        if (vert) {
            int x = r.left + (r.right - r.left) / 2;
            SetRect(&t, x, r.top + sbi.xyThumbTop, x + thick, r.top + sbi.xyThumbBottom);
        } else {
            int y = r.top + (r.bottom - r.top) / 2;
            SetRect(&t, r.left + sbi.xyThumbTop, y, r.left + sbi.xyThumbBottom, y + thick);
        }
        Real_FillRect(dc, &t, SolidBrush(SB_THUMB));
    }
    if (dc)
        ReleaseDC(hwnd, dc);
}

static void SetPalette(int index);

static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    (void)id; (void)data;
    if (msg == WM_COMMAND && !lp && LOWORD(wp) >= ID_PALETTE_FIRST && LOWORD(wp) < ID_PALETTE_FIRST + ARRAYSIZE(kPalettes)) {
        SetPalette(LOWORD(wp) - ID_PALETTE_FIRST);
        return 0;
    }
    if (!g_dark)
        return DefSubclassProc(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_HSCROLL: case WM_VSCROLL: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_SIZE: case WM_KEYDOWN: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        PaintThinThumbs(hwnd);
        return r;
    }

    case WM_NCMOUSEMOVE:
        // Over a scroll bar Windows shows the wide thumb: leave it alone.
        if ((wp == HTHSCROLL || wp == HTVSCROLL) && !GetPropW(hwnd, kScrollHot))
            SetPropW(hwnd, kScrollHot, (HANDLE)1);
        break;

    case WM_NCMOUSELEAVE:
    case WM_MOUSEMOVE:
        if (GetPropW(hwnd, kScrollHot)) {
            RemovePropW(hwnd, kScrollHot);
            // After Windows' collapse animation.
            SetTimer(hwnd, SB_TIMER, 700, NULL);
        }
        break;

    case WM_TIMER:
        if (wp == SB_TIMER) {
            KillTimer(hwnd, SB_TIMER);
            PaintThinThumbs(hwnd);
            return 0;
        }
        break;

    case WM_NCDESTROY:
        RemovePropW(hwnd, kScrollHot);
        break;

    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        HDC dc = (HDC)wp;
        int sys = SystemBrushIndex((HGDIOBJ)r);
        LOGBRUSH lb;
        // Pattern brushes (the tab-page texture) and handles that are not
        // brushes at all: AirDC's tab pages return GetStockObject(COLOR_3DFACE),
        // which is the default palette, and the label is left unpainted.
        if (sys < 0 && r && (GetObjectType((HGDIOBJ)r) != OBJ_BRUSH ||
            (GetObjectW((HGDIOBJ)r, sizeof(lb), &lb) == sizeof(lb) &&
             (lb.lbStyle == BS_PATTERN || lb.lbStyle == BS_DIBPATTERN || lb.lbStyle == BS_DIBPATTERNPT))))
            sys = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX ? COLOR_WINDOW : COLOR_BTNFACE;
        if (sys >= 0) {
            // Default handling (DefWindowProc/DefDlgProc) picked a system color,
            // or uxtheme the light tab-page texture of property pages, and set
            // the DC colors inside user32: redo them flipped.
            Real_SetTextColor(dc, PaletteText(FlippedSys(COLOR_WINDOWTEXT)));
            Real_SetBkColor(dc, FlippedSys(sys));
            return (LRESULT)ControlBrush(FlippedSys(sys));
        }
        return (LRESULT)Distinct((HBRUSH)MapObject((HGDIOBJ)r));
    }

    case WM_ERASEBKGND:
    case WM_PRINTCLIENT: {
        if (msg == WM_PRINTCLIENT && IsDisabledTextStatic(hwnd)) {
            PaintDisabledStatic(hwnd, (HDC)wp);
            return 0;
        }
        // MDI client: user32 erases with the light app workspace color.
        // Dialogs: uxtheme paints the light tab-page texture when themed
        // children (radio buttons, group boxes) ask for their parent's
        // background with WM_PRINTCLIENT.
        int sys;
        if (IsClass(hwnd, L"MDIClient"))
            sys = COLOR_APPWORKSPACE;
        else if (IsClass(hwnd, L"#32770"))
            sys = COLOR_BTNFACE;
        else
            break;
        if (msg == WM_PRINTCLIENT && !(lp & (PRF_ERASEBKGND | PRF_CLIENT)))
            break;
        RECT rc;
        GetClientRect(hwnd, &rc);
        Real_FillRect((HDC)wp, &rc, SolidBrush(FlippedSys(sys)));
        return msg == WM_ERASEBKGND ? 1 : 0;
    }

    case WM_PAINT:
        if (IsClass(hwnd, L"FlatTabCtrl")) {
            g_tabPaint++;
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            g_tabPaint--;
            return r;
        }
        if (IsClass(hwnd, L"#32770") && IsMessageBox(hwnd)) {
            PaintMessageBox(hwnd);
            return 0;
        }
        if (IsDisabledTextStatic(hwnd)) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            PaintDisabledStatic(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_MEASUREITEM:
        if (!wp && lp && ((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU && MeasureMenuItem((MEASUREITEMSTRUCT *)lp))
            return TRUE;
        break;

    case WM_DRAWITEM:
        if (!wp && lp && ((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU && DrawOdMenuItem((DRAWITEMSTRUCT *)lp))
            return TRUE;
        break;

    case WM_UAHDRAWMENU: {
        UAHMENU *m = (UAHMENU *)lp;
        RECT rc;
        MenuBarRect(hwnd, &rc);
        Real_FillRect(m->hdc, &rc, SolidBrush(MENUBAR_BG));
        return TRUE;
    }

    case WM_UAHDRAWMENUITEM:
        if (DrawMenuBarItem((UAHDRAWMENUITEM *)lp))
            return TRUE;
        break;

    case WM_PRINT: {
        // Menu fade-in animations are drawn from a WM_PRINT copy.
        COLORREF c;
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if ((lp & PRF_NONCLIENT) && PopupFrameColor(hwnd, &c))
            PaintPopupFrame(hwnd, (HDC)wp, c);
        return r;
    }

    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        COLORREF c;
        if (PopupFrameColor(hwnd, &c)) {
            PaintPopupFrame(hwnd, NULL, c);
            return r;
        }
        PaintThinThumbs(hwnd);
        if ((GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD) && (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_STATICEDGE) &&
            !(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_CLIENTEDGE)) {
            // Settings pages: the static edge user32 draws is light grey.
            PaintPopupFrame(hwnd, NULL, BORDER_NORMAL);
            return r;
        }
        if (!(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD))
            PaintMenuBarLine(hwnd);
        else if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_CLIENTEDGE)
            PaintClientEdge(hwnd);
        return r;
    }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Per-window dark themes

static void ApplyToWindow(HWND hwnd)
{
    BOOL dark = g_dark;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);

    if (pAllowDarkModeForWindow)
        pAllowDarkModeForWindow(hwnd, dark);

    if (!(style & WS_CHILD)) {
        BOOL v = dark;
        if (FAILED(DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof(v))))
            DwmSetWindowAttribute(hwnd, 19, &v, sizeof(v));
        COLORREF caption = dark ? MENUBAR_BG : 0xFFFFFFFF /* DWMWA_COLOR_DEFAULT */;
        COLORREF text = dark ? MENUBAR_TEXT : 0xFFFFFFFF;
        COLORREF border = dark ? MENUBAR_BG : 0xFFFFFFFF;
        DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
        DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
        DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    }

    WCHAR cls[64];
    if (!RealGetWindowClassW(hwnd, cls, ARRAYSIZE(cls)))
        return;

    Kind kind = WindowKind(hwnd);
    if (!lstrcmpiW(cls, REBARCLASSNAMEW) || kind == KIND_PROGRESS) {
        // Unthemed, rebars and progress bars paint from the (patched) system
        // colors instead of their light visual styles.
        SetWindowTheme(hwnd, dark ? L"" : NULL, dark ? L"" : NULL);
    } else if (kind == KIND_LISTVIEW) {
        SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);
        // Lists AirDC never colors use comctl32's cached (light) defaults.
        WCHAR prop[32];
        PropName(LVM_SETBKCOLOR, prop);
        if (!GetPropW(hwnd, prop)) {
            COLORREF bk = dark ? FlippedSys(COLOR_WINDOW) : Real_GetSysColor(COLOR_WINDOW);
            Real_SendMessageW(hwnd, LVM_SETBKCOLOR, 0, bk);
            Real_SendMessageW(hwnd, LVM_SETTEXTBKCOLOR, 0, bk);
            Real_SendMessageW(hwnd, LVM_SETTEXTCOLOR, 0, dark ? PaletteText(FlippedSys(COLOR_WINDOWTEXT)) : Real_GetSysColor(COLOR_WINDOWTEXT));
        }
        HWND header = ListView_GetHeader(hwnd);
        if (header) {
            if (pAllowDarkModeForWindow)
                pAllowDarkModeForWindow(header, dark);
            SetWindowTheme(header, dark ? L"DarkMode_ItemsView" : NULL, NULL);
        }
    } else if (kind == KIND_TREEVIEW) {
        SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);
        // Trees AirDC never colors use comctl32's cached (light) defaults.
        WCHAR prop[32];
        PropName(TVM_SETBKCOLOR, prop);
        if (!GetPropW(hwnd, prop)) {
            Real_SendMessageW(hwnd, TVM_SETBKCOLOR, 0, dark ? (LPARAM)FlippedSys(COLOR_WINDOW) : -1);
            Real_SendMessageW(hwnd, TVM_SETTEXTCOLOR, 0, dark ? (LPARAM)PaletteText(FlippedSys(COLOR_WINDOWTEXT)) : -1);
        }
    } else if (!lstrcmpiW(cls, TOOLTIPS_CLASSW)) {
        SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);
    } else if (!lstrcmpiW(cls, WC_HEADERW)) {
        SetWindowTheme(hwnd, dark ? L"DarkMode_ItemsView" : NULL, NULL);
    } else if (!lstrcmpiW(cls, WC_EDITW) && (style & (WS_VSCROLL | WS_HSCROLL))) {
        // Multi-line edits (chat input): CFD leaves their scrollbars light.
        SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);
    } else if (!lstrcmpiW(cls, WC_EDITW) || !lstrcmpiW(cls, WC_COMBOBOXW)) {
        // The file dialog's dark style has quieter borders than DarkTheme.
        SetWindowTheme(hwnd, dark ? L"DarkMode_CFD" : NULL, NULL);
    } else if (!lstrcmpiW(cls, WC_BUTTONW) ||
               !lstrcmpiW(cls, L"ComboLBox") || !lstrcmpiW(cls, WC_LISTBOXW) || !lstrcmpiW(cls, WC_SCROLLBARW) ||
               !lstrcmpiW(cls, WC_TABCONTROLW) || !lstrcmpiW(cls, STATUSCLASSNAMEW) ||
               !lstrcmpiW(cls, UPDOWN_CLASSW) ||
               !lstrcmpiW(cls, TOOLBARCLASSNAMEW) ||
               !lstrcmpiW(cls, WC_STATICW) || !lstrcmpiW(cls, L"ComboBoxEx32") ||
               !lstrcmpiW(cls, L"msctls_trackbar32") || !lstrcmpiW(cls, L"SysLink")) {
        SetWindowTheme(hwnd, dark ? L"DarkMode_DarkTheme" : NULL, NULL);
    } else if (IsRichEdit(hwnd) || (style & (WS_VSCROLL | WS_HSCROLL))) {
        // Scrollbars of rich edits and AirDC's own scrolling windows.
        SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);
    }
}

// Every window is subclassed: any of them can be the parent that answers
// WM_CTLCOLOR* for an edit or static (toolbars and rebars included).
static BOOL NeedsSubclass(HWND hwnd)
{
    (void)hwnd;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Dark system palette inside comctl32 and rich edit (patched IATs)

static void PatchImportByAddress(HMODULE module, FARPROC target, void *hook)
{
    BYTE *base = (BYTE *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !target)
        return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        for (IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + d->FirstThunk); t->u1.Function; t++) {
            if ((FARPROC)t->u1.Function != target)
                continue;
            DWORD old;
            if (VirtualProtect(&t->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                t->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&t->u1.Function, sizeof(void *), old, &old);
            }
        }
    }
}

static void PatchImport(HMODULE module, const char *dll, const char *func, void *hook, void **original)
{
    FARPROC target = GetProcAddress(GetModuleHandleA(dll), func);
    if (!target)
        return;
    if (!*original)
        *original = (void *)target;
    PatchImportByAddress(module, target, hook);
}

// List boxes and combo drop-down lists (comctl32) select items with a white
// background in dark mode: use the accent blue with white text instead.
static COLORREF (WINAPI *LibReal_SetBkColor)(HDC, COLORREF);
static COLORREF (WINAPI *LibReal_SetTextColor)(HDC, COLORREF);

static BOOL IsListBoxDC(HDC dc)
{
    HWND w = WindowFromDC(dc);
    return w && (IsClass(w, L"ComboLBox") || IsClass(w, L"ListBox"));
}

static COLORREF ListSelection(void)
{
    BOOL mapped;
    return DarkSysColor(COLOR_HIGHLIGHT, &mapped);
}

static COLORREF WINAPI Lib_SetBkColor(HDC dc, COLORREF c)
{
    if (g_dark && c == RGB(255, 255, 255) && IsListBoxDC(dc)) {
        LibReal_SetTextColor(dc, RGB(255, 255, 255));
        return LibReal_SetBkColor(dc, ListSelection());
    }
    return LibReal_SetBkColor(dc, c);
}

static COLORREF WINAPI Lib_SetTextColor(HDC dc, COLORREF c)
{
    if (g_dark && GetBkColor(dc) == ListSelection() && IsListBoxDC(dc))
        c = RGB(255, 255, 255);
    return LibReal_SetTextColor(dc, c);
}

static int (WINAPI *LibReal_SetScrollInfo)(HWND, int, LPCSCROLLINFO, BOOL);

static int WINAPI Lib_SetScrollInfo(HWND hwnd, int bar, LPCSCROLLINFO si, BOOL redraw)
{
    int r = LibReal_SetScrollInfo(hwnd, bar, si, redraw);
    if (redraw && g_dark && GetCurrentThreadId() == g_uiThread)
        PaintThinThumbs(hwnd);
    return r;
}

static void PatchSystemLibraries(void)
{
    static HMODULE done[16];
    HMODULE mods[512];
    DWORD needed;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return;
    FARPROC gsc = (FARPROC)Real_GetSysColor, gscb = (FARPROC)Real_GetSysColorBrush;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < ARRAYSIZE(mods); i++) {
        WCHAR name[MAX_PATH];
        if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH))
            continue;
        if (lstrcmpiW(name, L"comctl32.dll") && lstrcmpiW(name, L"msftedit.dll") && lstrcmpiW(name, L"riched20.dll"))
            continue;
        BOOL seen = FALSE;
        int slot = -1;
        for (int k = 0; k < ARRAYSIZE(done); k++) {
            if (done[k] == mods[i])
                seen = TRUE;
            if (!done[k] && slot < 0)
                slot = k;
        }
        if (seen || slot < 0)
            continue;
        done[slot] = mods[i];
        PatchImportByAddress(mods[i], gsc, Lib_GetSysColor);
        PatchImportByAddress(mods[i], gscb, Lib_GetSysColorBrush);
        if (Real_DrawEdge)
            PatchImportByAddress(mods[i], (FARPROC)Real_DrawEdge, Hook_DrawEdge);
        PatchThemeImports(mods[i]);
        if (!LibReal_SetScrollInfo)
            LibReal_SetScrollInfo = (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetScrollInfo");
        PatchImportByAddress(mods[i], (FARPROC)LibReal_SetScrollInfo, Lib_SetScrollInfo);
        if (!lstrcmpiW(name, L"comctl32.dll")) {
            HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
            if (!LibReal_SetBkColor) {
                LibReal_SetBkColor = (void *)GetProcAddress(gdi, "SetBkColor");
                LibReal_SetTextColor = (void *)GetProcAddress(gdi, "SetTextColor");
            }
            PatchImportByAddress(mods[i], (FARPROC)LibReal_SetBkColor, Lib_SetBkColor);
            PatchImportByAddress(mods[i], (FARPROC)LibReal_SetTextColor, Lib_SetTextColor);
        }
    }
}

// ---------------------------------------------------------------------------
// Message hooks: window creation, custom draw, theme switches

static HHOOK g_hookPre, g_hookRet;
static BOOL g_uiInitDone;

// Custom draw structs of list/tree views (clrText, clrTextBk) and toolbars
// (clrText .. clrHighlightHotTrack) carry colors. AirDC sees them in its own
// (light) color space and the control gets them back flipped. A notification
// can pass through several windows (toolbar -> rebar -> frame), so each struct
// is converted only on its outermost delivery.
static COLORREF *CustomDrawColors(UINT msg, LPARAM lp, int *count)
{
    if (msg != WM_NOTIFY || !lp)
        return NULL;
    NMHDR *h = (NMHDR *)lp;
    if (h->code != NM_CUSTOMDRAW || !h->hwndFrom)
        return NULL;
    switch (WindowKind(h->hwndFrom)) {
    case KIND_LISTVIEW:
    case KIND_TREEVIEW:
        *count = 2;
        return &((NMLVCUSTOMDRAW *)h)->clrText;
    case KIND_TOOLBAR:
        *count = 6;
        return &((NMTBCUSTOMDRAW *)h)->clrText;
    default:
        return NULL;
    }
}

static struct { void *p; int depth; } g_cdStack[16];

static void ReapplyAll(void);

static LRESULT CALLBACK CallWndProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_dark) {
        const CWPSTRUCT *c = (const CWPSTRUCT *)lp;
        int n;
        COLORREF *cd = CustomDrawColors(c->message, c->lParam, &n);
        if (cd) {
            if (g_cdTop && g_cdStack[g_cdTop - 1].p == cd) {
                g_cdStack[g_cdTop - 1].depth++;
            } else if (g_cdTop < ARRAYSIZE(g_cdStack)) {
                g_cdStack[g_cdTop].p = cd;
                g_cdStack[g_cdTop++].depth = 1;
                for (int i = 0; i < n; i++)
                    cd[i] = Inv(cd[i]);
            }
        }
    }
    return CallNextHookEx(g_hookPre, code, wp, lp);
}

static LRESULT CALLBACK CallWndRetProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const CWPRETSTRUCT *c = (const CWPRETSTRUCT *)lp;
        int n;
        COLORREF *cd = CustomDrawColors(c->message, c->lParam, &n);
        if (cd && g_cdTop && g_cdStack[g_cdTop - 1].p == cd && --g_cdStack[g_cdTop - 1].depth == 0) {
            g_cdTop--;
            for (int i = 0; i < n; i++)
                cd[i] = i == 0 && n == 2 ? TextFwd(cd[i]) : Fwd(cd[i]);
            if (!g_cdTop)
                ZeroMemory(g_barDCs, sizeof(g_barDCs));
        }
        if (c->message == WM_ENTERIDLE && c->wParam == MSGF_MENU && g_dark) {
            EnumThreadWindows(GetCurrentThreadId(), PaintMenuFrame, 0);
        } else if (c->message == WM_INITMENUPOPUP && !HIWORD(c->lParam)) {  // not the window (system) menu
            AddPaletteMenu((HMENU)c->wParam);
            if (g_dark)
                OwnerDrawMenu((HMENU)c->wParam);
        } else if (c->message == WM_EXITMENULOOP) {
            RestoreMenus(0, 0);
        }
        if (c->message == WM_CREATE && c->lResult != -1) {
            if (IsRichEdit(c->hwnd))
                PatchSystemLibraries();
            // comctl32 caches system colors (process-wide and per control) and
            // refreshes them on WM_SYSCOLORCHANGE; the caches predate the
            // patched GetSysColor.
            if (g_dark && (IsClass(c->hwnd, L"Sys") || IsClass(c->hwnd, L"msctls_") ||
                           IsClass(c->hwnd, TOOLBARCLASSNAMEW) || IsClass(c->hwnd, REBARCLASSNAMEW)))
                Real_SendMessageW(c->hwnd, WM_SYSCOLORCHANGE, 0, 0);
            if (NeedsSubclass(c->hwnd))
                SetWindowSubclass(c->hwnd, SubclassProc, 1, 0);
            if (g_dark)
                ApplyToWindow(c->hwnd);
        } else if (c->message == WM_SETTINGCHANGE && c->lParam &&
                   !lstrcmpiW((LPCWSTR)c->lParam, L"ImmersiveColorSet") &&
                   !(GetWindowLongW(c->hwnd, GWL_STYLE) & WS_CHILD)) {
            // The ini is reread too, so an edited Mode= applies on the next
            // theme notification.
            ReadSettings();
            BOOL dark = ComputeDark();
            if (dark != g_dark) {
                g_dark = dark;
                ReadAccent();
                ApplyAppMode();
                ReapplyAll();
            }
        }
    }
    return CallNextHookEx(g_hookRet, code, wp, lp);
}

static HMENU g_paletteMenu;

// Adds "Dark mode text colors" to the end of AirDC's View menu (the one with
// Public hubs) and checks the current palette.
static void AddPaletteMenu(HMENU menu)
{
    if (GetMenuState(menu, 42772 /* View > Public hubs */, MF_BYCOMMAND) == (UINT)-1)
        return;
    if (!g_paletteMenu) {
        g_paletteMenu = CreatePopupMenu();
        static const int order[] = { PALETTE_DEFAULT, 1, 2, 3, 4, 0 };
        for (int i = 0; i < ARRAYSIZE(order); i++)
            AppendMenuW(g_paletteMenu, MF_STRING, ID_PALETTE_FIRST + order[i], kPalettes[order[i]].name);
    }
    BOOL present = FALSE;
    for (int i = GetMenuItemCount(menu) - 1; i >= 0 && !present; i--)
        present = GetSubMenu(menu, i) == g_paletteMenu;
    if (!present) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)g_paletteMenu, L"Dark mode text colors");
    }
    int n = GetMenuItemCount(g_paletteMenu), current = 0;
    for (int i = 0; i < n; i++)
        if (GetMenuItemID(g_paletteMenu, i) == ID_PALETTE_FIRST + (UINT)g_palette)
            current = i;
    CheckMenuRadioItem(g_paletteMenu, 0, n - 1, current, MF_BYPOSITION);
}

static BOOL CALLBACK RepaletteChild(HWND hwnd, LPARAM lp)
{
    (void)lp;
    ApplyToWindow(hwnd);
    ResendColors(hwnd);
    if (IsRichEdit(hwnd))
        RecolorRichEdit(hwnd, RECOLOR_PALETTE);
    return TRUE;
}

static BOOL CALLBACK RepaletteTop(HWND hwnd, LPARAM lp)
{
    (void)lp;
    EnumChildWindows(hwnd, RepaletteChild, 0);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    return TRUE;
}

// Picked from the menu: remember it per user (the program folder is usually
// not writable) and recolor everything that is open.
static void SetPalette(int index)
{
    if (index < 0 || index >= ARRAYSIZE(kPalettes) || index == g_palette)
        return;
    WCHAR dir[MAX_PATH], ini[MAX_PATH];
    if (ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\AirDC++", dir, MAX_PATH)) {
        CreateDirectoryW(dir, NULL);
        wsprintfW(ini, L"%s\\darkmode.ini", dir);
        WritePrivateProfileStringW(L"DarkMode", L"Palette", kPalettes[index].key, ini);
    }
    g_palette = index;
    if (g_dark)
        EnumThreadWindows(GetCurrentThreadId(), RepaletteTop, 0);
}

static BOOL CALLBACK ReapplyChild(HWND hwnd, LPARAM lp)
{
    (void)lp;
    ApplyToWindow(hwnd);
    ResendColors(hwnd);
    if (IsRichEdit(hwnd))
        RecolorRichEdit(hwnd, g_dark ? RECOLOR_DARK : RECOLOR_LIGHT);
    SendMessageW(hwnd, WM_SYSCOLORCHANGE, 0, 0);
    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
    return TRUE;
}

static BOOL CALLBACK ReapplyTop(HWND hwnd, LPARAM lp)
{
    (void)lp;
    ApplyToWindow(hwnd);
    EnumChildWindows(hwnd, ReapplyChild, 0);
    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    DrawMenuBar(hwnd);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    return TRUE;
}

static void ReapplyAll(void)
{
    EnumThreadWindows(GetCurrentThreadId(), ReapplyTop, 0);
}

// Runs on the first window creation, outside the loader lock.
static void InitUi(void)
{
    if (g_uiInitDone)
        return;
    g_uiInitDone = TRUE;

    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux) {
        pRefreshImmersiveColorPolicyState = (void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
        pAllowDarkModeForWindow = (void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        pSetPreferredAppMode = (void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        pFlushMenuThemes = (void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    }
    if (g_dark)
        ApplyAppMode();
    PatchSystemLibraries();
    g_uiThread = GetCurrentThreadId();
    g_hookPre = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, NULL, GetCurrentThreadId());
    g_hookRet = SetWindowsHookExW(WH_CALLWNDPROCRET, CallWndRetProc, NULL, GetCurrentThreadId());
}

static HWND WINAPI Hook_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y,
                                        int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    // Only the GUI thread gets the message hooks; AirDC creates hidden helper
    // windows on worker threads too.
    if (!g_uiInitDone && !(style & WS_CHILD) && (style & (WS_CAPTION | WS_VISIBLE)))
        InitUi();
    return Real_CreateWindowExW(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

// ---------------------------------------------------------------------------
// Setup

static BOOL IsAirDcProcess(void)
{
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    const WCHAR *name = path + n;
    while (name > path && name[-1] != L'\\')
        name--;
    return !lstrcmpiW(name, L"AirDC.exe");
}

static void InstallHooks(void)
{
    HMODULE exe = g_exe;
    PatchImport(exe, "gdi32.dll", "SetTextColor", Hook_SetTextColor, (void **)&Real_SetTextColor);
    PatchImport(exe, "gdi32.dll", "SetBkColor", Hook_SetBkColor, (void **)&Real_SetBkColor);
    PatchImport(exe, "gdi32.dll", "GetBkColor", Hook_GetBkColor, (void **)&Real_GetBkColor);
    PatchImport(exe, "gdi32.dll", "SelectObject", Hook_SelectObject, (void **)&Real_SelectObject);
    PatchImport(exe, "gdi32.dll", "DeleteObject", Hook_DeleteObject, (void **)&Real_DeleteObject);
    PatchImport(exe, "gdi32.dll", "SetPixelV", Hook_SetPixelV, (void **)&Real_SetPixelV);
    PatchImport(exe, "gdi32.dll", "GetPixel", Hook_GetPixel, (void **)&Real_GetPixel);
    PatchImport(exe, "gdi32.dll", "CreateCompatibleDC", Hook_CreateCompatibleDC, (void **)&Real_CreateCompatibleDC);
    PatchImport(exe, "user32.dll", "FillRect", Hook_FillRect, (void **)&Real_FillRect);
    PatchImport(exe, "user32.dll", "TrackPopupMenu", Hook_TrackPopupMenu, (void **)&Real_TrackPopupMenu);
    PatchImport(exe, "user32.dll", "TrackPopupMenuEx", Hook_TrackPopupMenuEx, (void **)&Real_TrackPopupMenuEx);
    PatchImport(exe, "user32.dll", "FrameRect", Hook_FrameRect, (void **)&Real_FrameRect);
    PatchImport(exe, "user32.dll", "BeginPaint", Hook_BeginPaint, (void **)&Real_BeginPaint);
    PatchImport(exe, "user32.dll", "DrawEdge", Hook_DrawEdge, (void **)&Real_DrawEdge);
    PatchImport(exe, "user32.dll", "DrawFrameControl", Hook_DrawFrameControl, (void **)&Real_DrawFrameControl);
    PatchImport(exe, "user32.dll", "DefWindowProcW", Hook_DefWindowProcW, (void **)&Real_DefWindowProcW);
    PatchImport(exe, "user32.dll", "DefFrameProcW", Hook_DefFrameProcW, (void **)&Real_DefFrameProcW);
    PatchImport(exe, "user32.dll", "DefMDIChildProcW", Hook_DefMDIChildProcW, (void **)&Real_DefMDIChildProcW);
    PatchImport(exe, "user32.dll", "SendMessageW", Hook_SendMessageW, (void **)&Real_SendMessageW);
    PatchImport(exe, "user32.dll", "CreateWindowExW", Hook_CreateWindowExW, (void **)&Real_CreateWindowExW);
    PatchImport(exe, "comctl32.dll", "ImageList_DrawEx", Hook_ImageList_DrawEx, (void **)&Real_ImageList_DrawEx);
    PatchImport(exe, "comctl32.dll", "ImageList_DrawIndirect", Hook_ImageList_DrawIndirect, (void **)&Real_ImageList_DrawIndirect);
    PatchImport(exe, "uxtheme.dll", "OpenThemeData", Hook_OpenThemeData, (void **)&Real_OpenThemeData);
    PatchImport(exe, "uxtheme.dll", "DrawThemeBackground", Hook_DrawThemeBackground, (void **)&Real_DrawThemeBackground);
    PatchImport(exe, "kernel32.dll", "GetProcAddress", Hook_GetProcAddress, (void **)&Real_GetProcAddress);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        LoadRealMsimg32();
        if (IsAirDcProcess()) {
            g_exe = GetModuleHandleW(NULL);
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
            g_exeStart = (BYTE *)g_exe;
            g_exeEnd = g_exeStart + nt->OptionalHeader.SizeOfImage;
            for (int i = 0; i < 256; i++) {
                float x = i / 255.0f;
                g_toLinear[i] = x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
            }
            Real_GetSysColor = (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSysColor");
            Real_GetSysColorBrush = (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSysColorBrush");
            ReadSettings();
            ReadAccent();
            g_dark = ComputeDark();
            // Hooks are installed even in light mode so a live switch to dark works.
            InstallHooks();
        }
    }
    return TRUE;
}
