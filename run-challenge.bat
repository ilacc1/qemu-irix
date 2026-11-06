@echo off
rem ============================================================================
rem  SGI Challenge/Onyx (IP19) - IRIX 6.5 startup script for Windows.
rem
rem  Requires the IP19 PROM under sgi\challenge and a user-supplied CHD.
rem  Set IRIX_DISK to the installed Challenge disk image.
rem ============================================================================
setlocal EnableExtensions
cd /d "%~dp0"
set "BASE=%~dp0"

set "MSYS2="
if defined MSYS2_ROOT if exist "%MSYS2_ROOT%\mingw64\bin\libgtk-3-0.dll" set "MSYS2=%MSYS2_ROOT%"
if not defined MSYS2 if exist "%BASE%msys64\mingw64\bin\libgtk-3-0.dll" set "MSYS2=%BASE%msys64"
if not defined MSYS2 if exist "%BASE%..\msys64\mingw64\bin\libgtk-3-0.dll" set "MSYS2=%BASE%..\msys64"
if not defined MSYS2 if exist "C:\msys64\mingw64\bin\libgtk-3-0.dll" set "MSYS2=C:\msys64"
if not defined MSYS2 (
    echo ERROR: MSYS2 MinGW64 runtime not found.
    echo Looked for msys64 beside the script, one level up, and at C:\msys64.
    echo Set MSYS2_ROOT to your MSYS2 install directory.
    goto end
)
set "PATH=%MSYS2%\mingw64\bin;%PATH%"

set "QEMU="
if exist "%BASE%qemu\build\qemu-system-mips64.exe" set "QEMU=%BASE%qemu\build\qemu-system-mips64.exe"
if not defined QEMU if exist "%BASE%build\qemu-system-mips64.exe" set "QEMU=%BASE%build\qemu-system-mips64.exe"
if not defined QEMU if exist "%BASE%..\qemu\build\qemu-system-mips64.exe" set "QEMU=%BASE%..\qemu\build\qemu-system-mips64.exe"
if not defined QEMU (
    echo ERROR: qemu-system-mips64.exe not found.
    echo Looked under qemu\build, build, and ..\qemu\build.
    goto end
)

set "BIOS="
if exist "%BASE%sgi\challenge\ip19prom.bin" set "BIOS=%BASE%sgi\challenge\ip19prom.bin"
if not defined BIOS if exist "%BASE%..\sgi\challenge\ip19prom.bin" set "BIOS=%BASE%..\sgi\challenge\ip19prom.bin"
if not defined BIOS (
    echo ERROR: Challenge PROM ip19prom.bin not found.
    goto end
)
for %%F in ("%BIOS%") do set "BIOS_DIR=%%~dpF"
if "%BIOS_DIR:~-1%"=="\" set "BIOS_DIR=%BIOS_DIR:~0,-1%"

if not "%IRIX_DISK%"=="" set "DISK=%IRIX_DISK%"
if "%DISK%"=="" if exist "%BASE%sgi\ip19-main.chd" set "DISK=%BASE%sgi\ip19-main.chd"
if "%DISK%"=="" if exist "%BASE%ip19-main.chd" set "DISK=%BASE%ip19-main.chd"
if "%DISK%"=="" if exist "%BASE%..\ip19-main.chd" set "DISK=%BASE%..\ip19-main.chd"
if "%DISK%"=="" set "DISK=%BASE%ip19-main.chd"
if not "%IRIX_SMP%"=="" set "SMP=%IRIX_SMP%"
if "%SMP%"=="" set "SMP=2"
if not "%IRIX_RAM%"=="" set "RAM=%IRIX_RAM%"
if "%RAM%"=="" set "RAM=2G"
if not "%IRIX_MTTCG%"=="" set "MTTCG=%IRIX_MTTCG%"
if "%MTTCG%"=="" set "MTTCG=1"
set "MTTCG_ARGS="
if "%MTTCG%"=="1" set "MTTCG_ARGS=-accel tcg,thread=multi"
rem Temporary default while debugging PROM boot failures.
if "%IRIX_MONITOR%"=="" set "IRIX_MONITOR=tcp:127.0.0.1:4445,server=on,wait=off"
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

rem Keep one CD-ROM drive present even with no media, so the GTK
rem removable-media menu can be used to insert an ISO later.
set "CDROM_ARGS=-drive "if=none,id=ip19cdrom,media=cdrom" -device "scsi-cd,drive=ip19cdrom,bus=main-system-bus/sgi-s1/scsi.0,scsi-id=5,logical_block_size=512,physical_block_size=512""
if not "%IRIX_CD%"=="" if exist "%IRIX_CD%" (
    set "CDROM_ARGS=-drive "file=%IRIX_CD%,format=raw,if=none,id=ip19cdrom,media=cdrom,readonly=on" -device "scsi-cd,drive=ip19cdrom,bus=main-system-bus/sgi-s1/scsi.0,scsi-id=5,logical_block_size=512,physical_block_size=512""
)
rem Console selection:
rem   window = dedicated console window (default)
rem   gtk    = GTK window with serial VC/View menu
rem   shell  = run the VM in this console
rem   telnet = no serial window; telnet 127.0.0.1:4444
if "%IRIX_CONSOLE%"=="" set "IRIX_CONSOLE=window"
set "DISPLAY_MODE=gtk,zoom-to-fit=on"
set "SERIAL_SPEC=vc"
if /I "%IRIX_CONSOLE%"=="console" (
    set "DISPLAY_MODE=gtk,zoom-to-fit=on"
    set "SERIAL_SPEC=stdio"
)
if /I "%IRIX_CONSOLE%"=="shell" (
    set "DISPLAY_MODE=none"
    set "SERIAL_SPEC=stdio"
)
if /I "%IRIX_CONSOLE%"=="telnet" (
    set "DISPLAY_MODE=none"
    set "SERIAL_SPEC=telnet:127.0.0.1:4444,server=on,wait=off"
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
echo.

if /I "%IRIX_CONSOLE%"=="window" (
    start "SGI Challenge" "%QEMU%" -M ip19 -smp %SMP% -m %RAM% %MTTCG_ARGS% -bios "%BIOS%" -L "%BIOS_DIR%" %SNAP_ARGS% -drive "file=%DISK%,format=%DISK_FORMAT%,if=none,id=ip19disk" -device "scsi-hd,drive=ip19disk,bus=main-system-bus/sgi-s1/scsi.0,scsi-id=1" %CDROM_ARGS% %NET_ARGS% -parallel none -display gtk,zoom-to-fit=on -serial vc -monitor "%IRIX_MONITOR%" %*
    set "RC=0"
    goto end
)

"%QEMU%" -M ip19 -smp %SMP% -m %RAM% %MTTCG_ARGS% -bios "%BIOS%" -L "%BIOS_DIR%" %SNAP_ARGS% -drive "file=%DISK%,format=%DISK_FORMAT%,if=none,id=ip19disk" -device "scsi-hd,drive=ip19disk,bus=main-system-bus/sgi-s1/scsi.0,scsi-id=1" %CDROM_ARGS% %NET_ARGS% -parallel none -display "%DISPLAY_MODE%" -serial "%SERIAL_SPEC%" -monitor "%IRIX_MONITOR%" %*

set "RC=%ERRORLEVEL%"
echo.
echo QEMU exited with code %RC%.

:end
exit /b %RC%
