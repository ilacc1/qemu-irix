@echo off
rem ============================================================================
rem  qemu-irix 0.1.0 - Windows dependency installer.
rem
rem  The GTK runtime DLLs ship inside this distribution (msys64\mingw64\...),
rem  so on a complete extraction there is nothing to download. If they are
rem  missing, this script falls back to a real MSYS2: it finds yours
rem  (MSYS2_ROOT) or downloads and installs MSYS2, plus the GTK3 runtime
rem  with pacman, then points run-irix.bat at it via MSYS2_ROOT.
rem
rem  What it cannot do for you is the TAP network adapter (needs a driver
rem  install + admin rights); it reports whether the expected 'my-tap'
rem  adapter exists and tells you how to create it.
rem
rem  The IRIX disk image itself is NOT included: set IRIX_DISK to your
rem  IRIX 6.5 CHD, e.g.  set IRIX_DISK=C:\irix\irix65_4020.chd
rem ============================================================================
setlocal
cd /d "%~dp0"

rem --- emulator binary + firmware (fatal; only a re-extract fixes these) ----
if not exist "qemu\build\qemu-system-mips64.exe" (
    echo MISSING: qemu\build\qemu-system-mips64.exe
    echo Re-extract the distribution zip.
    exit /b 1
)
if not exist "indy\ip24prom.070-9101-011.bin" (
    echo MISSING: indy\ip24prom.070-9101-011.bin
    echo Re-extract the distribution zip.
    exit /b 1
)
if not exist "indy\nvram\rtc" (
    echo MISSING: indy\nvram\rtc
    echo Re-extract the distribution zip.
    exit /b 1
)

rem --- locate a GTK runtime: bundled first, then MSYS2 -----------------------
set "DLLSRC="
if exist "msys64\mingw64\bin\libgtk-3-0.dll" set "DLLSRC=%CD%\msys64\mingw64\bin"
if "%DLLSRC%"=="" (
    if defined MSYS2_ROOT (
        if exist "%MSYS2_ROOT%\mingw64\bin\libgtk-3-0.dll" set "DLLSRC=%MSYS2_ROOT%\mingw64\bin"
    )
)
if "%DLLSRC%"=="" (
    if exist "C:\msys64\mingw64\bin\libgtk-3-0.dll" (
        set "DLLSRC=C:\msys64\mingw64\bin"
        set "MSYS2_ROOT=C:\msys64"
        setx MSYS2_ROOT "C:\msys64" >nul
        echo Using MSYS2 at C:\msys64 - saved MSYS2_ROOT.
    )
)

if not "%DLLSRC%"=="" goto havedeps

rem --- no GTK runtime anywhere: fetch our MSYS2 installer --------------------
echo No GTK runtime found - neither bundled nor MSYS2. Downloading MSYS2 ...
set "MSYS2_URL=https://github.com/ilacc1/qemu-irix/releases/download/0.1.0/msys2-x86_64-20260927.exe"
set "MSYS2_EXE=%TEMP%\msys2-installer.exe"
curl.exe -L -o "%MSYS2_EXE%" "%MSYS2_URL%"
if errorlevel 1 (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%MSYS2_URL%' -OutFile '%MSYS2_EXE%'"
)
if not exist "%MSYS2_EXE%" (
    echo.
    echo ERROR: download failed. Get MSYS2 by hand from https://www.msys2.org/
    echo - default C:\msys64 - then re-run install.bat, it will fetch the
    echo GTK3 runtime via pacman.
    exit /b 2
)
echo Installing MSYS2 to C:\msys64 - silent, a minute or two ...
"%MSYS2_EXE%" in --confirm-command --accept-messages --root C:/msys64
if not exist "C:\msys64\usr\bin\bash.exe" (
    echo.
    echo ERROR: MSYS2 install did not produce C:\msys64\usr\bin\bash.exe.
    echo Install it by hand from https://www.msys2.org/ and re-run install.bat.
    exit /b 2
)
set "MSYS2_ROOT=C:\msys64"
setx MSYS2_ROOT "C:\msys64" >nul
echo MSYS2 installed. Fetching the GTK3 runtime - a few minutes ...
C:\msys64\usr\bin\bash.exe -lc "pacman -Syu --noconfirm || pacman -Syu --noconfirm"
C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm --needed mingw-w64-x86_64-gtk3"
if not exist "C:\msys64\mingw64\bin\libgtk-3-0.dll" (
    echo.
    echo ERROR: pacman did not deliver libgtk-3-0.dll. Open the MINGW64 shell
    echo - C:\msys64\mingw64.exe - and run:  pacman -S mingw-w64-x86_64-gtk3
    exit /b 2
)
set "DLLSRC=C:\msys64\mingw64\bin"

:havedeps
echo Using GTK runtime at: %DLLSRC%

rem --- verify the runtime is complete ----------------------------------------
set "FAIL=0"
for %%D in (
    libgtk-3-0.dll libgdk-3-0.dll libglib-2.0-0.dll libgobject-2.0-0.dll
    libgio-2.0-0.dll libgmodule-2.0-0.dll libpixman-1-0.dll
    libcairo-2.dll libpango-1.0-0.dll libgdk_pixbuf-2.0-0.dll
) do (
    if not exist "%DLLSRC%\%%D" (
        echo MISSING DLL: %DLLSRC%\%%D
        set "FAIL=1"
    )
)
if not "%FAIL%"=="0" (
    echo.
    echo Dependency check FAILED - re-extract the distribution zip.
    exit /b 1
)
echo All runtime DLLs present.

rem --- TAP adapter - driver install needs admin; only reported here ---------
ipconfig | findstr /i /c:"my-tap" >nul
if errorlevel 1 (
    echo.
    echo NOTE: no 'my-tap' TAP adapter found. run-irix.bat uses
    echo tap,ifname=my-tap - bridged networking needs the adapter:
    echo   1. Install the TAP driver - OpenVPN TAP-Windows6 package.
    echo   2. Rename the new TAP adapter to my-tap in Network Connections.
    echo Until then the guest has no network; everything else still works.
) else (
    echo TAP adapter 'my-tap' found.
)

if "%IRIX_DISK%"=="" (
    echo.
    echo Reminder: set IRIX_DISK to your IRIX 6.5 disk image before running:
    echo   set IRIX_DISK=C:\irix\irix65_4020.chd
)

echo.
echo Done. Start the machine with run-irix.bat
exit /b 0
