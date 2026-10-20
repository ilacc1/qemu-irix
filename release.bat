@echo off
rem ============================================================================
rem  Reproducible release packaging for qemu-irix, Windows side.
rem
rem  Run on the Windows build machine from the repo root:
rem    release.bat [version]
rem
rem  Builds dist\qemu-irix-<version>-windows.zip from the local Windows
rem  build: the exe plus the exact mingw64 DLL closure computed with ldd
rem  (not hardcoded), GTK runtime data, indy firmware, scripts and docs.
rem  Needs MSYS2 (MSYS2_ROOT, msys64 next to the script, or C:\msys64).
rem  Repo path must not contain spaces.
rem
rem  The disk image is never bundled: it stays a user-supplied default
rem  resolved by the launchers at runtime.
rem ============================================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "VER=%~1"
if "%VER%"=="" set "VER=0.1.0"

rem --- locate MSYS2 bash ------------------------------------------------------
set "BASH="
if defined MSYS2_ROOT (
    if exist "%MSYS2_ROOT%\usr\bin\bash.exe" set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
)
if "%BASH%"=="" (
    if exist "msys64\usr\bin\bash.exe" set "BASH=%CD%\msys64\usr\bin\bash.exe"
)
if "%BASH%"=="" (
    if exist "C:\msys64\usr\bin\bash.exe" set "BASH=C:\msys64\usr\bin\bash.exe"
)
if "%BASH%"=="" (
    echo ERROR: MSYS2 bash not found. Set MSYS2_ROOT and retry.
    exit /b 1
)
set "MSYS2=%BASH:\usr\bin\bash.exe=%"
echo Using MSYS2 at %MSYS2%

if not exist "qemu\build\qemu-system-mips64.exe" (
    echo ERROR: qemu\build\qemu-system-mips64.exe not found - build it first.
    exit /b 1
)

set "STAGE=dist\stage-windows\qemu-irix-%VER%"
if exist dist\stage-windows rmdir /s /q dist\stage-windows
mkdir "%STAGE%\qemu\build"
mkdir "%STAGE%\msys64\mingw64\bin"

copy /y "qemu\build\qemu-system-mips64.exe" "%STAGE%\qemu\build\" >nul

rem --- DLL closure, computed with ldd inside msys bash -----------------------
rem REPO_WIN passes the repo path through the environment so the bash
rem one-liner needs no quoting gymnastics.
set "REPO_WIN=%CD%"
if not exist dist mkdir dist
"%BASH%" -lc "cd $(cygpath -u '%REPO_WIN%') && ldd qemu/build/qemu-system-mips64.exe | awk '{print $3}' | grep -i mingw64 | sed 's|.*/mingw64/|mingw64/|' | sort -u > dist/dlls.txt"
for /f "delims=" %%P in (dist\dlls.txt) do (
    set "REL=%%P"
    copy /y "%MSYS2%\!REL:/=\!" "%STAGE%\msys64\mingw64\bin\" >nul
)

mkdir "%STAGE%\msys64\mingw64\lib"
mkdir "%STAGE%\msys64\mingw64\share"
xcopy /e /i /y "%MSYS2%\mingw64\lib\gdk-pixbuf-2.0" "%STAGE%\msys64\mingw64\lib\gdk-pixbuf-2.0\" >nul
xcopy /e /i /y "%MSYS2%\mingw64\share\glib-2.0\schemas" "%STAGE%\msys64\mingw64\share\glib-2.0\schemas\" >nul

xcopy /e /i /y "indy" "%STAGE%\indy\" >nul
copy /y run-irix.bat "%STAGE%\" >nul
copy /y install.bat "%STAGE%\" >nul
copy /y readme.md "%STAGE%\" >nul
copy /y building.md "%STAGE%\" >nul
copy /y networking.md "%STAGE%\" >nul

powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%' -DestinationPath 'dist\qemu-irix-%VER%-windows.zip' -Force"
echo Wrote dist\qemu-irix-%VER%-windows.zip
exit /b 0
