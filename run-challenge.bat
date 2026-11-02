@echo off
rem ============================================================================
rem  SGI Challenge/Onyx (IP19) - IRIX 6.5 startup script for Windows.
rem
rem  Requires the IP19 PROM under sgi\challenge and a user-supplied CHD.
rem  Set IRIX_DISK to the installed Challenge disk image.
rem ============================================================================
setlocal
cd /d "%~dp0"

set "MSYS2=%MSYS2_ROOT%"
if "%MSYS2%"=="" set "MSYS2=msys64"
if not exist "%MSYS2%\mingw64\bin\libgtk-3-0.dll" (
    echo ERROR: MSYS2 MinGW64 runtime not found under "%MSYS2%".
    echo Set MSYS2_ROOT to your MSYS2 install directory.
    goto end
)
set "PATH=%MSYS2%\mingw64\bin;%PATH%"

set "QEMU=qemu\build\qemu-system-mips64.exe"
set "BIOS=sgi\challenge\ip19prom.bin"
if not "%IRIX_DISK%"=="" set "DISK=%IRIX_DISK%"
if "%DISK%"=="" if exist "ip19-installed.chd" set "DISK=ip19-installed.chd"
if "%DISK%"=="" if exist "..\ip19-installed.chd" set "DISK=..\ip19-installed.chd"
if "%DISK%"=="" if exist "..\ip19-yoda2.raw" (
    set "DISK=..\ip19-yoda2.raw"
    set "IRIX_DISK_FORMAT=raw"
)
if "%DISK%"=="" set "DISK=ip19-installed.chd"
if not "%IRIX_SMP%"=="" set "SMP=%IRIX_SMP%"
if "%SMP%"=="" set "SMP=2"
if not "%IRIX_RAM%"=="" set "RAM=%IRIX_RAM%"
if "%RAM%"=="" set "RAM=2G"
if not "%IRIX_MTTCG%"=="" set "MTTCG=%IRIX_MTTCG%"
if "%MTTCG%"=="" set "MTTCG=1"
set "MTTCG_ARGS="
if "%MTTCG%"=="1" set "MTTCG_ARGS=-accel tcg,thread=multi"
set "DISK_FORMAT=chd"
if not "%IRIX_DISK_FORMAT%"=="" set "DISK_FORMAT=%IRIX_DISK_FORMAT%"

if not exist "%QEMU%" (
    echo ERROR: %QEMU% not found - build it first.
    goto end
)
if not exist "%BIOS%" (
    echo ERROR: %BIOS% not found.
    goto end
)
if not exist "%DISK%" (
    echo ERROR: disk image %DISK% not found - set IRIX_DISK.
    goto end
)

set "CDROM_ARGS="
if not "%IRIX_CD%"=="" if exist "%IRIX_CD%" (
    set "CDROM_ARGS=-drive "file=%IRIX_CD%,format=raw,if=none,id=ip19cdrom,readonly=on""
)
set "CDROM2_ARGS="
if not "%IRIX_CD2%"=="" if exist "%IRIX_CD2%" (
    set "CDROM2_ARGS=-drive "file=%IRIX_CD2%,format=raw,if=none,id=ip19cdrom2,readonly=on""
)

rem Console selection:
rem   window = GTK window showing the serial VC (default)
rem   shell  = no window; serial console on this shell
rem   telnet = no serial window; telnet 127.0.0.1:4444
if "%IRIX_CONSOLE%"=="" set "IRIX_CONSOLE=window"
if /I "%IRIX_CONSOLE%"=="shell" (
    set "DISPLAY_MODE=none"
    set "SERIAL_SPEC=stdio"
) else if /I "%IRIX_CONSOLE%"=="telnet" (
    set "DISPLAY_MODE=gtk,zoom-to-fit=on"
    set "SERIAL_SPEC=telnet:127.0.0.1:4444,server=on,wait=off"
) else (
    set "DISPLAY_MODE=gtk,zoom-to-fit=on"
    set "SERIAL_SPEC=vc"
)

set "TAP_IF=%IRIX_TAP_IF%"
if "%TAP_IF%"=="" set "TAP_IF=my-tap"
set "NET_ARGS="
if "%IRIX_NONET%"=="" (
    netsh interface show interface name="%TAP_IF%" >nul 2>&1
    if errorlevel 1 (
        echo WARNING: no "%TAP_IF%" TAP adapter found - guest will have no network.
        echo See https://github.com/ilacc1/qemu-irix/blob/master/networking.md
    ) else (
        set "NET_ARGS=-net "nic,model=epc-enet,netdev=net0" -netdev "tap,ifname=%TAP_IF%,id=net0""
    )
)

set "SNAP_ARGS="
if "%IRIX_SNAPSHOT%"=="1" set "SNAP_ARGS=-snapshot"

echo Starting SGI Challenge/Onyx (IRIX 6.5) ...
echo   CPUs:         %SMP%
echo   RAM:          %RAM%
echo   Disk:         %DISK%
echo   Console:      %IRIX_CONSOLE%
echo   QEMU monitor: telnet 127.0.0.1 5555
echo.

"%QEMU%" -M ip19 -smp %SMP% -m %RAM% %MTTCG_ARGS% -bios "%BIOS%" -L "sgi\challenge" %SNAP_ARGS% -drive "file=%DISK%,format=%DISK_FORMAT%,if=none,id=ip19disk" %CDROM_ARGS% %CDROM2_ARGS% %NET_ARGS% -parallel none -display "%DISPLAY_MODE%" -serial "%SERIAL_SPEC%" -monitor "telnet:127.0.0.1:5555,server=on,wait=off" %*

set "RC=%ERRORLEVEL%"
echo.
echo QEMU exited with code %RC%.

:end
exit /b %RC%
