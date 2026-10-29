# AirDC++ Dark Mode

A dark theme for the [AirDC++](https://www.airdcpp.net/) Windows client that follows the Windows light/dark setting and switches live, with no restart.

It is a small drop-in DLL: no AirDC++ files or settings are changed, and removing the DLL brings back the normal look.

> Unofficial add-on. Not made by or affiliated with the AirDC++ developers.

## What it covers

- Title bars, menu bar, popup and context menus (with an accent-blue selection)
- Toolbars, tabs, status bar, lists, trees, column headers and group headers
- Hub and private chat, system log and other rich text views, keeping AirDC++'s chat colours readable
- Transfer and queue progress bars (white status text in dark mode)
- The Settings dialog and the other dialogs, including tab pages, edit and combo boxes, spin buttons, date pickers and message boxes
- Switching Windows between light and dark while AirDC++ is running, including chat text that is already on screen

Your own AirDC++ colour settings are used as the base: every colour AirDC++ draws is converted to its dark equivalent (same hue, flipped lightness), so the dark theme still matches your settings.

## Requirements

- AirDC++ 4.30 (64-bit) on Windows 10 or 11

## Install

1. Download `msimg32.dll` from the [latest release](../../releases/latest).
2. Close AirDC++.
3. Copy `msimg32.dll` into the AirDC++ folder, next to `AirDC.exe` (usually `C:\Program Files\AirDC++`).
4. Start AirDC++.

To uninstall, close AirDC++ and delete that `msimg32.dll`.

## Options

By default the theme follows Windows (Settings > Personalization > Colors > Choose your app mode). To force a mode, put a `darkmode.ini` next to the DLL:

```ini
[DarkMode]
Mode=auto   ; auto (follow Windows), dark or light
```

The file is read again whenever Windows reports a theme change.

## Build

Requires Visual Studio with the C++ x64 build tools.

```
build.bat          -> build\msimg32.dll
build.bat trace    -> debug build that logs to %TEMP%\darkmode-trace.log
```

## How it works

AirDC.exe imports `msimg32.dll`, so a DLL with that name in the program folder is loaded in place of the system one. This DLL forwards the real msimg32 functions and, while dark mode is on:

- hooks the GDI/USER calls AirDC.exe uses to pick colours, brushes and pens, and passes each colour through a hue-preserving lightness flip in OKLab;
- gives Windows-drawn controls the native dark themes and a dark system palette inside comctl32 and the rich edit control;
- draws the parts Windows has no dark style for (menu bar, popup menus, borders, spin buttons, group boxes, disabled text, progress bar text) itself.

## License

[MIT](LICENSE)
