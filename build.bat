@echo off
rem ============================================================================
rem  qemu-irix - Windows build helper (MSYS2 MINGW64).
rem
rem  Runs the same configure line as building.md and then ninja. Extra
rem  configure options can be appended, e.g.  build.bat --enable-debug
rem ============================================================================
setlocal
cd /d "%~dp0"
rem Accept a release-style parent that holds the source in qemu\.
if not exist "configure" if exist "qemu\configure" cd /d "%~dp0qemu"

set "MSYS2=%MSYS2_ROOT%"
if not defined MSYS2 if exist "%~dp0msys64\usr\bin\bash.exe" set "MSYS2=%~dp0msys64"
if not defined MSYS2 if exist "%~dp0..\msys64\usr\bin\bash.exe" set "MSYS2=%~dp0..\msys64"
if not defined MSYS2 set "MSYS2=C:\msys64"
if not exist "%MSYS2%\usr\bin\bash.exe" (
    echo ERROR: MSYS2 not found under "%MSYS2%".
    echo Set MSYS2_ROOT to your MSYS2 install directory.
    exit /b 1
)

set "MSYSTEM=MINGW64"
set "ROOT=%CD%"
"%MSYS2%\usr\bin\bash.exe" -lc "cd $(cygpath -u '%ROOT%') && ./configure --target-list=mips64-softmmu --enable-gtk --enable-sdl --disable-vnc --disable-spice --disable-opengl --disable-werror --disable-docs --disable-tools --disable-guest-agent --disable-capstone --disable-fdt --disable-libusb --disable-usb-redir --disable-brlapi --disable-curl --disable-tests --disable-slirp --disable-vte --disable-nettle --disable-gcrypt --disable-gnutls %* && ninja -C build qemu-system-mips64.exe"

exit /b %ERRORLEVEL%
