@echo off
rem ============================================================================
rem  SGI Indy (IP24) - IRIX 5.3 startup script for Windows (MSYS2 build).
rem
rem  Mirrors run-irix53.sh: indy machine, PROM sgi\indy\ip24prom.070-9101-011.bin,
rem  NVRAM seeded from sgi\indy\nvram beside first run, CHD disk supplied by
rem  the user via IRIX_DISK (not distributed with this repository).
rem ============================================================================
setlocal
cd /d "%~dp0"

rem --- QEMU GUI (GTK3) needs the MSYS2 MinGW runtime DLLs on PATH -----------
set "MSYS2=%MSYS2_ROOT%"
if "%MSYS2%"=="" set "MSYS2=msys64"
if not exist "%MSYS2%\mingw64\bin\libgtk-3-0.dll" (
    echo ERROR: MSYS2 MinGW64 runtime not found under "%MSYS2%".
    echo Set MSYS2_ROOT to your MSYS2 install directory.
    goto end
)
set "PATH=%MSYS2%\mingw64\bin;%PATH%"

rem --- async offload paths (worker threads). =1 forces synchronous inline
rem --- execution for A/B comparison; override any of these beforehand.
if not defined SGI_GFX_SYNC set "SGI_GFX_SYNC=0"
if not defined SGI_WD_SYNC set "SGI_WD_SYNC=0"
if not defined SGI_CHD_SYNC set "SGI_CHD_SYNC=0"
rem --- IRIX 5.3's HAL2 driver spins when the modelled chip is incomplete.
if not defined SGI_HAL2_ABSENT set "SGI_HAL2_ABSENT=1"
rem --- Audio: SDL2 through WASAPI on Windows (not DirectSound).
if not defined IRIX_AUDIO set "IRIX_AUDIO=sdl"
if /i "%IRIX_AUDIO%"=="sdl" if not defined SDL_AUDIODRIVER set "SDL_AUDIODRIVER=wasapi"
set "QEMU=qemu\build\qemu-system-mips64.exe"
set "BIOS=sgi\indy\ip24prom.070-9101-011.bin"
if not "%IRIX_DISK%"=="" set "DISK=%IRIX_DISK%"
if "%DISK%"=="" set "DISK=sgi\irix53.chd"
rem Optional CD image (SCSI ID 4); leave IRIX_CDROM unset for no CD.
if not "%IRIX_CDROM%"=="" set "CDROM=%IRIX_CDROM%"

if not exist "%QEMU%" (
    echo ERROR: %QEMU% not found - build it first.
    goto end
)
if not exist "%BIOS%" (
    echo ERROR: %BIOS% not found.
    goto end
)
if not exist "%DISK%" (
    echo ERROR: disk image %DISK% not found - set IRIX_DISK to your IRIX 5.3 CHD.
    goto end
)

rem --- first run: seed NVRAM beside the binary from the in-tree seeds ------
if not exist "qemu\build\nvram\indy_4020\rtc" (
    echo Seeding NVRAM from sgi\indy\nvram ...
    mkdir "qemu\build\nvram\indy_4020" 2>nul
    copy /y "sgi\indy\nvram\eeprom" "qemu\build\nvram\indy_4020\" >nul
    copy /y "sgi\indy\nvram\rtc" "qemu\build\nvram\indy_4020\" >nul
)

rem --- CD drive is always present (empty tray when no image) so the GTK
rem --- Removable drive menu can insert and eject media at runtime. ------
set "CDROM_ARGS=-drive "if=none,id=cdrom0,media=cdrom,readonly=on" -device "scsi-cd,bus=scsi.0,scsi-id=4,drive=cdrom0,physical_block_size=512""
if defined CDROM (
    if exist "%CDROM%" (
        set "CDROM_ARGS=-drive "file=%CDROM%,format=raw,if=none,id=cdrom0,media=cdrom,readonly=on" -device "scsi-cd,bus=scsi.0,scsi-id=4,drive=cdrom0,physical_block_size=512""
    )
)

rem --- Tolerate a missing TAP adapter: warn and run without networking ----
netsh interface show interface name="my-tap" >nul 2>&1
if errorlevel 1 (
    echo WARNING: no my-tap TAP adapter found - guest will have no network.
    echo See https://github.com/ilacc1/qemu-irix/blob/master/networking.md
    echo to set it up.
    set "NET_ARGS="
) else (
    set "NET_ARGS=-net "nic,model=seeq80c03,netdev=net0" -netdev "tap,ifname=my-tap,id=net0""
)

rem Console: window exposes the PIO console as a GTK serial VC; shell uses
rem stdio; telnet uses the original PIO socket.
if "%IRIX_CONSOLE%"=="" set "IRIX_CONSOLE=window"
set "DISPLAY_MODE=gtk,zoom-to-fit=on"
set "SERIAL_ARGS=-serial null -serial null -serial vc"
if /I "%IRIX_CONSOLE%"=="console" (
    set "DISPLAY_MODE=gtk,zoom-to-fit=on"
    set "SERIAL_ARGS=-serial null -serial null -serial stdio"
)
if /I "%IRIX_CONSOLE%"=="shell" (
    set "DISPLAY_MODE=none"
    set "SERIAL_ARGS=-serial null -serial null -serial stdio"
)
if /I "%IRIX_CONSOLE%"=="telnet" (
    set "DISPLAY_MODE=none"
    set "SERIAL_ARGS=-serial null -serial null -chardev "socket,host=127.0.0.1,port=4444,server=on,wait=off,id=pc" -serial chardev:pc"
)

if /I "%IRIX_CONSOLE%"=="window" (
    start "SGI Indy (IRIX 5.3)" "%QEMU%" -M indy,audiodev=sgiaudio -audiodev "%IRIX_AUDIO%,id=sgiaudio" -global sgi-mc.simms=0x0eee -global sgi-mc.mc-rev=3 -m 512M -bios "%BIOS%" -drive "file=%DISK%,format=chd,if=scsi,bus=0,unit=1,media=disk,cache=writethrough" %CDROM_ARGS% %NET_ARGS% -parallel none -display gtk,zoom-to-fit=on -serial null -serial null -serial vc -monitor none %*
    set "RC=0"
    goto end
)

echo Starting SGI Indy (IRIX 5.3) ...
echo   Disk:         %DISK%
echo   Console:      %IRIX_CONSOLE%
echo.

"%QEMU%" -M indy,audiodev=sgiaudio -audiodev "%IRIX_AUDIO%,id=sgiaudio" -global sgi-mc.simms=0x0eee -global sgi-mc.mc-rev=3 -m 512M -bios "%BIOS%" -drive "file=%DISK%,format=chd,if=scsi,bus=0,unit=1,media=disk,cache=writethrough" %CDROM_ARGS% %NET_ARGS% -parallel none -display "%DISPLAY_MODE%" %SERIAL_ARGS% -monitor none %*

set "RC=%ERRORLEVEL%"
echo.
echo QEMU exited with code %RC%.

:end
exit /b %RC%
