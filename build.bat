@echo off
rem Builds the 64-bit msimg32.dll proxy. Requires Visual Studio C++ x64 tools.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -prerelease -products * -property installationPath`) do set "VS=%%i"
if not defined VS for /d %%d in ("%ProgramFiles%\Microsoft Visual Studio\*") do for /d %%e in ("%%d\*") do if exist "%%e\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%e"
if not defined VS (echo Visual Studio not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
set "EXTRA="
if /i "%~1"=="trace" set "EXTRA=/DDARKMODE_TRACE"
cl /nologo /O2 /W4 /wd4152 /wd4201 /GS- /MT /LD %EXTRA% darkmode.c /Fobuild\ /Febuild\msimg32.dll /link /DEF:msimg32.def /NOLOGO psapi.lib || exit /b 1
echo Built build\msimg32.dll
