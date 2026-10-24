@echo off
rem ============================================================================
rem  SGI Indy (IP24) - IRIX 6.5 startup script for Windows (MSYS2 build).
rem
rem  Mirrors run-irix.sh: indy machine, PROM indy\ip24prom.070-9101-011.bin,
rem  NVRAM seeded from indy\nvram beside first run, CHD disk supplied by
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
rem --- execution for A/B comparison; override any of these beforehand, e.g.
rem ---   set SGI_GFX_SYNC=1
rem --- GFX: 0 = surface draws go to the GIO64 worker thread (default).
if not defined SGI_GFX_SYNC set "SGI_GFX_SYNC=0"
rem --- WD33C93: 0 = batched run-DMA fast path (default).
if not defined SGI_WD_SYNC set "SGI_WD_SYNC=0"
rem --- CHD: 0 = inflate hunks in the block thread pool (default).
if not defined SGI_CHD_SYNC set "SGI_CHD_SYNC=0"

set "QEMU=qemu\build\qemu-system-mips64.exe"
set "BIOS=indy\ip24prom.070-9101-011.bin"
if not "%IRIX_DISK%"=="" set "DISK=%IRIX_DISK%"
if "%DISK%"=="" set "DISK=irix65_4020.chd"
rem Optional CD image (SCSI ID 4); leave IRIX_CDROM unset for no CD, e.g.:
rem   set IRIX_CDROM=C:\isos\overlay.iso
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
    echo ERROR: disk image %DISK% not found - set IRIX_DISK to your IRIX 6.5 CHD.
    goto end
)

rem --- first run: seed NVRAM beside the binary from the in-tree seeds ------
if not exist "qemu\build\nvram\indy_4020\rtc" (
    echo Seeding NVRAM from indy\nvram ...
    mkdir "qemu\build\nvram\indy_4020" 2>nul
    copy /y "indy\nvram\eeprom" "qemu\build\nvram\indy_4020\" >nul
    copy /y "indy\nvram\rtc" "qemu\build\nvram\indy_4020\" >nul
)

rem --- overlay/install CD is attached only when present --------------------
set "CDROM_ARGS="
if defined CDROM (
    if exist "%CDROM%" (
        set "CDROM_ARGS=-drive "file=%CDROM%,format=raw,if=none,id=cdrom0,media=cdrom,readonly=on" -device "scsi-cd,bus=scsi.0,scsi-id=4,drive=cdrom0,physical_block_size=512""
    )
)

rem --- Tolerate a missing TAP adapter: warn and run without networking ----
ipconfig | findstr /i /c:"my-tap" >nul
if errorlevel 1 (
    echo WARNING: no my-tap TAP adapter found - guest will have no network.
    echo See https://github.com/ilacc1/qemu-irix/blob/master/networking.md
    echo to set it up.
    set "NET_ARGS="
) else (
    set "NET_ARGS=-net "nic,model=seeq80c03,netdev=net0" -netdev "tap,ifname=my-tap,id=net0""
)

echo Starting SGI Indy (IRIX 6.5) ...
echo   Disk:         %DISK%
echo   PIO console:  telnet 127.0.0.1 4444
echo   QEMU monitor: telnet 127.0.0.1 5555
echo.

"%QEMU%" -M indy,audiodev=sgiaudio -audiodev none,id=sgiaudio -global sgi-mc.simms=0x0eee -global sgi-mc.mc-rev=3 -m 512M -bios "%BIOS%" -drive "file=%DISK%,format=chd,if=scsi,bus=0,unit=1,media=disk" %CDROM_ARGS% %NET_ARGS% -display gtk,zoom-to-fit=on -serial null -serial null -chardev "socket,host=127.0.0.1,port=4444,server=on,wait=off,id=pc" -serial chardev:pc -monitor "tcp:127.0.0.1:5555,server=on,wait=off" %*

set "RC=%ERRORLEVEL%"
echo.
echo QEMU exited with code %RC%.

:end
exit /b %RC%
