#!/usr/bin/env bash
#
# SGI Indy (IP24) startup script — interactive PROM/sash session.
#
# Machine identity: Indy (R4000SC, 200MHz), QEMU `-M indy`. The PROM is
# indy/ip24prom.070-9101-011.bin (MAME indy_4020 default b10) and the NVRAM
# seeds are indy/nvram/{eeprom,rtc} (IP24 DS1386 8KiB RTC). QEMU reads disks
# through the native CHD block driver (block/chd.c), so no chdman/raw
# extraction step exists anywhere.
#
# You supply the disk image yourself (it is not distributed with this
# repository): set IRIX_DISK to your IRIX 6.5 CHD for the Indy (e.g. a
# copy of the image you boot in MAME). The image is opened READ-WRITE
# (IRIX XFS log recovery writes back during mount).
#
# What this gives you:
#   - Indy machine (XL24 graphics via sgi-gio64; IP24 PROM) with GTK
#     display showing the PROM maintenance menu.
#   - System disk on HD0 ID 1 plus, optionally, an SGI install/overlay CD
#     ISO on HD0 ID 4 (SGI standard CDROM target, set IRIX_CDROM).
#     SGI install CDs use 512-byte sectors (SGI volhdr/EFS layout); the
#     -device line below sets physical_block_size=512 (QEMU scsi-cd
#     defaults to 2048, which breaks the PROM's volhdr READ).
#   - Network: TAP onto the host bridge br0 by default (tap interface must
#     exist and be owned by the invoking user). IRIX_NETMODE=user selects
#     SLIRP user-mode instead (if the build includes it); IRIX_NONET=1
#     drops the NIC entirely.
#   - PIO8 text console on TELNET: prints the NVRAM notice on first boot,
#     then goes quiet — with valid NVRAM the PROM selects the graphics
#     console and the menu lives on video, not telnet.
#     Connect with:  telnet 127.0.0.1 4444   (or set IRIX_TELNET_PORT)
#   - Keyboard/mouse: wired to the pair the PROM PS/2 driver polls (HPC3
#     PIO ch8). With GTK focused, physical keypresses reach the PROM
#     (also HMP sendkey via the monitor socket). Prefer short commands
#     (`boot`); long pasted lines can overrun the PROM line editor.
#   - Audio: the Indy PROM plays its boot chime through HAL2. QEMU needs
#     an explicit `-audiodev` plus `-M indy,audiodev=<id>`; IRIX_AUDIO
#     selects the backend (default "pa"), IRIX_AUDIO=wav captures the
#     chime to a file instead (path via IRIX_AUDIO_WAV), IRIX_AUDIO=none
#     keeps an explicit silent sink, IRIX_AUDIO=off preserves the legacy
#     no-audiodev command line.
#   - NVRAM seeds from indy/nvram/{eeprom,rtc}, loaded little-endian like
#     MAME. On first boot QEMU copies them into build/nvram/indy_4020/
#     (resolved beside the QEMU binary); all write-back goes to that
#     copy, and the guest clock is re-seeded from the host on every boot.
#     Delete build/nvram/ to force a fresh PROM env.
#   - Guest RAM: the PROM sizes memory from the MC SIMM registers, not
#     from -m; -m only bounds the backing store. IRIX_MC_SIMMS sets the
#     per-bank SIMM nibbles and IRIX_MC_REV the MC revision nibble (both
#     surfaced as sgi-mc -global properties). The default models a 384MB
#     Indy: three banks (A+B+C) at MC rev C, which IRIX 6.5 sizes via its
#     MAX_MEM_BANKS=3 loop.
#
# Env overrides: IRIX_DISK (disk image path, required),
# IRIX_CDROM (iso path, attached only if set and present),
# IRIX_DISPLAY ("gtk,zoom-to-fit=on"; plain "gtk" for 1:1 pixels),
# IRIX_TELNET_PORT (4444), IRIX_MONITOR (monitor socket path),
# IRIX_NETMODE ("tap" default; "user" for SLIRP), IRIX_TAP_IF (default
# tap-iris), IRIX_NONET (set to drop NIC), IRIX_AUDIO (default "pa";
# "wav"/"none"/"off", see above), IRIX_AUDIO_WAV (default run/chime.wav).
# IRIX_ESCC_LOG: set to a directory to capture the two ESCC serial
# ports (kernel serial console) to files there, e.g.
#   IRIX_ESCC_LOG=run/escc ./run-irix.sh
# then in the PROM Command Monitor: `setenv console d`, then `boot`.
# IRIX_ESCC_SOCK: path of a unix socket for the second ESCC port, so the
# guest's serial console can be driven from the host.
# IRIX_DISK_COW: copy-on-write mode for the disk image.
#   overlay (default) - a qcow2 overlay with the image as its backing
#                    file. The image is opened READ-ONLY and every guest
#                    write goes to the overlay (by default
#                    <image>.cow.qcow2 next to it), so the image itself
#                    can never be damaged while the guest's data persists
#                    between runs. Delete the overlay to go back to the
#                    pristine image; IRIX_DISK_OVERLAY names it.
#   off            - direct read-write access to the image (old
#                    behaviour; the image can be damaged by a kill).
# Extra QEMU args may be appended: ./run-irix.sh -d guest_errors
#
# Tuning switches (value-based; 0 and unset both mean "offload enabled"):
#   SGI_GFX_SYNC=1   run every REX3 draw inline on the vCPU thread
#                    (default 0: surface draws go to the GIO64 worker thread)
#   SGI_WD_SYNC=1    force WD33C93 DMA through the per-byte path
#                    (default 0: batched run-DMA fast path)
#   SGI_CHD_SYNC=1   inflate CHD hunks inline instead of in the block thread
#                    pool (default 0)
#   SGI_CHD_CACHE_MB decompressed-hunk cache size, default 256; 0 disables
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

cd "$ROOT_DIR"

DISPLAY_MODE=${IRIX_DISPLAY:-"gtk,zoom-to-fit=on"}
MACHINE=${IRIX_MACHINE:-indy}
# bios set b10 = index 0 = MAME indy_4020's default BIOS.
BIOS_IMAGE=${IRIX_BIOS:-ip24prom.070-9101-011.bin}
# Optional CD image (SCSI ID 4). Leave IRIX_CDROM unset for no CD, e.g.:
#   IRIX_DISK=~/irix65_4020.chd IRIX_CDROM=~/overlay.iso ./run-irix.sh
CDROM_ISO=${IRIX_CDROM:-}
PIO_TELNET_PORT=${IRIX_TELNET_PORT:-4444}
MON_SOCK=${IRIX_MONITOR:-$ROOT_DIR/run/mon.sock}
# The disk image is supplied by the user (not distributed with this
# repository): point IRIX_DISK at your IRIX 6.5 CHD for the Indy, e.g.
#   IRIX_DISK=~/irix65_4020.chd ./run-irix.sh
# A file named irix65_4020.chd next to this script is used by default.
DISK=${IRIX_DISK:-$ROOT_DIR/irix65_4020.chd}

# Guest RAM. The PROM sizes memory from the MC SIMM registers, not from
# -m; -m only bounds the backing store (see header).
GUEST_RAM=${IRIX_RAM:-512M}
MC_SIMMS=${IRIX_MC_SIMMS:-0x0eee}
MC_REV=${IRIX_MC_REV:-3}
MC_ARGS=(-global "sgi-mc.simms=$MC_SIMMS" -global "sgi-mc.mc-rev=$MC_REV")

if [ ! -f "$DISK" ]; then
    echo "Disk image not found: $DISK" >&2
    echo "Put irix65_4020.chd next to this script or export IRIX_DISK=/path/to/irix65.chd and retry." >&2
    exit 1
fi

# Prefer the bundled lib/ closure over host libraries when present
# (binary distributions ship it; source checkouts do not have it).
if [ -d "$ROOT_DIR/lib" ]; then
    export LD_LIBRARY_PATH="$ROOT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

if [ -n "$CDROM_ISO" ] && [ ! -f "$CDROM_ISO" ]; then
    echo "CDROM image not found: $CDROM_ISO (ignoring)" >&2
    CDROM_ISO=
fi

if [ ! -x "$ROOT_DIR/build/qemu-system-mips64" ]; then
    echo "QEMU binary not found. Build it first:" >&2
    echo "  ninja -C build qemu-system-mips64   (from the repository root)" >&2
    exit 1
fi

# A stale monitor socket from a killed QEMU blocks startup; the PIO
# side is TCP so it needs no cleanup.
mkdir -p "$ROOT_DIR/run"
rm -f "$MON_SOCK"

# Seed NVRAM from the in-tree seeds once; afterwards it persists (the
# guest clock is re-seeded from the host on every boot). Delete
# build/nvram/ to force a fresh PROM env.
# QEMU resolves NVRAM beside its own binary (see sgi_hpc3_open_nvram), so
# the working-copy location below must stay in sync with build/nvram/.
NVRAM_DIR="$ROOT_DIR/build/nvram/indy_4020"
if [ ! -f "$NVRAM_DIR/rtc" ]; then
    mkdir -p "$NVRAM_DIR"
    cp "$ROOT_DIR/indy/nvram/eeprom" \
       "$ROOT_DIR/indy/nvram/rtc" \
       "$NVRAM_DIR/"
fi

if [ -n "${IRIX_ESCC_LOG:-}" ]; then
    mkdir -p "$IRIX_ESCC_LOG"
    ESCC_A=(-serial "file:$IRIX_ESCC_LOG/esccA.log")
    ESCC_B=(-serial "file:$IRIX_ESCC_LOG/esccB.log")
elif [ -n "${IRIX_ESCC_SOCK:-}" ]; then
    # ESCC_B as a unix socket so the guest's serial console can be driven
    # from the host (login prompt, command output). IRIX_ESCC_SOCK holds
    # the socket path; a client connects with e.g. run/escccon.py.
    ESCC_A=(-serial null)
    ESCC_B=(-serial "unix:$IRIX_ESCC_SOCK,server=on,wait=off")
else
    ESCC_A=(-serial null)
    ESCC_B=(-serial null)
fi

# IRIX_NONET=1 drops the NIC entirely (for isolating network-dependent
# hangs; the guest then has no ec0 at all).
# IRIX_NETMODE=user attaches the NIC to SLIRP user-mode networking instead
# of the default TAP. IRIX_TAP_IF selects the tap interface (must exist
# and be owned by the invoking user).
if [ -n "${IRIX_NONET:-}" ]; then
    NET_ARGS=()
elif [ "${IRIX_NETMODE:-tap}" = "tap" ]; then
    IRIX_TAP_IF="${IRIX_TAP_IF:-tap-iris}"
    NET_ARGS=(-net "nic,model=seeq80c03,netdev=net0"
              -netdev "tap,id=net0,ifname=$IRIX_TAP_IF,script=,downscript=")
else
    NET_ARGS=(-net "nic,model=seeq80c03,netdev=net0"
              -netdev "user,id=net0")
fi

# Tolerate a missing TAP interface: warn (with a pointer to the setup
# guide) and boot without networking instead of failing.
if [ "${IRIX_NETMODE:-tap}" = "tap" ] && [ -z "${IRIX_NONET:-}" ]; then
    if ! (command -v ip >/dev/null 2>&1 && ip link show "$IRIX_TAP_IF" >/dev/null 2>&1); then
        echo "WARNING: TAP interface '$IRIX_TAP_IF' not found - guest will have no network." >&2
        echo "See https://github.com/ilacc1/qemu-irix/blob/master/networking.md to set it up." >&2
        NET_ARGS=()
    fi
fi

# Audio wiring for the HAL2 boot chime (see header).
AUDIO_BACKEND=${IRIX_AUDIO:-pa}
AUDIO_ID=sgiaudio
if [ "$AUDIO_BACKEND" = "off" ]; then
    AUDIO_ARGS=()
    MACHINE_ARG=(-M "$MACHINE")
elif [ "$AUDIO_BACKEND" = "wav" ]; then
    WAV_PATH=${IRIX_AUDIO_WAV:-$ROOT_DIR/run/chime.wav}
    mkdir -p "$(dirname "$WAV_PATH")"
    AUDIO_ARGS=(-audiodev "wav,id=$AUDIO_ID,path=$WAV_PATH")
    MACHINE_ARG=(-M "$MACHINE,audiodev=$AUDIO_ID")
else
    AUDIO_ARGS=(-audiodev "$AUDIO_BACKEND,id=$AUDIO_ID")
    MACHINE_ARG=(-M "$MACHINE,audiodev=$AUDIO_ID")
fi

# The overlay/install CD is attached only when IRIX_CDROM names a file
# that exists.
if [ -n "$CDROM_ISO" ]; then
    CDROM_ARGS=(-drive "file=$CDROM_ISO,format=raw,if=none,id=cdrom0,media=cdrom,readonly=on"
                -device "scsi-cd,bus=scsi.0,scsi-id=4,drive=cdrom0,physical_block_size=512")
else
    CDROM_ARGS=()
fi

# Disk access mode. The default is copy-on-write: a qcow2 overlay with
# the CHD as its backing file. The CHD is opened read-only (backing files
# always are) and every guest write lands in the overlay, so the original
# image can never be damaged while the guest's data persists between
# runs. Delete the overlay to start over from the pristine image.
COW=${IRIX_DISK_COW:-overlay}
case "$COW" in
overlay)
    OVERLAY=${IRIX_DISK_OVERLAY:-$DISK.cow.qcow2}
    if [ ! -f "$OVERLAY" ]; then
        echo "Creating copy-on-write overlay $OVERLAY"
        "$ROOT_DIR/run/mkoverlay.py" --disk "$DISK" --overlay "$OVERLAY"
    fi
    DISK_ARGS=("file=$OVERLAY,format=qcow2,if=scsi,bus=0,unit=1,media=disk")
    ;;
off)
    DISK_ARGS=("file=$DISK,format=chd,if=scsi,bus=0,unit=1,media=disk")
    ;;
*)
    echo "IRIX_DISK_COW must be 'overlay' or 'off' (got '$COW')" >&2
    exit 1
    ;;
esac

echo "GTK window shows the PROM menu. Type there (window focused)."
echo "PIO console (early boot only): telnet 127.0.0.1 $PIO_TELNET_PORT"
echo "Monitor socket: $MON_SOCK"
if [ "$COW" = "overlay" ]; then
    echo "Disk copy-on-write: $DISK is read-only, guest writes go to $OVERLAY"
fi
exec env -u GTK_PATH -u GTK_EXE_PREFIX -u GTK_IM_MODULE_FILE \
    -u LD_PRELOAD \
    "$ROOT_DIR/build/qemu-system-mips64" \
    "${MACHINE_ARG[@]}" \
    "${AUDIO_ARGS[@]}" \
    "${MC_ARGS[@]}" \
    -m "$GUEST_RAM" \
    -bios "$ROOT_DIR/indy/$BIOS_IMAGE" \
    -drive "${DISK_ARGS[@]}" \
    "${CDROM_ARGS[@]}" \
    ${NET_ARGS[@]} \
    -display "$DISPLAY_MODE" \
    "${ESCC_A[@]}" \
    "${ESCC_B[@]}" \
    -chardev "socket,host=127.0.0.1,port=$PIO_TELNET_PORT,server=on,wait=off,id=pc" \
    -serial chardev:pc \
    -monitor "unix:$MON_SOCK,server,nowait" \
    "$@"
