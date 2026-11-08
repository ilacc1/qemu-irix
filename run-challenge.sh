#!/usr/bin/env bash
# IP19/Challenge (Everest) launcher.
#
# Headless console/monitor harness:
#   IRIX_HEADLESS=1 IRIX_DISK=... ./run-challenge.sh
# exposes:
#   run/ip19-serial.sock  (guest serial console)
#   run/ip19-mon.sock     (QEMU HMP monitor)
# and can be driven with the machine-agnostic run/console.py and run/mon.py.
#
# Disks/CDs:
#   IRIX_DISK=...       target 1, format IRIX_DISK_FORMAT (default chd)
#   IRIX_CD=...         target 5, format IRIX_CD_FORMAT (default raw)
#
# Network (EPC et0):
#   IRIX_NET=tap        attach to host bridge via IRIX_TAP_IF (default)
#   IRIX_NET=user       attach QEMU user-mode networking (slirp)
#   IRIX_NET=off        leave the NIC disconnected
#   IRIX_NET=<spec>     pass any other -nic specification through
# The default TAP interface is tap-iris, matching run-irix.sh.
DIR=$(dirname "$0")
QEMU="$DIR/qemu/build/qemu-system-mips64"
PROM="$DIR/sgi/challenge/ip19prom.bin"
DISK="$IRIX_DISK"
CD="$IRIX_CD"
DISK_FORMAT="${IRIX_DISK_FORMAT:-chd}"
SMP="${IRIX_SMP:-4}"
RAM="${IRIX_RAM:-2G}"
DISPLAY_MODE="${IRIX_DISPLAY:-gtk,zoom-to-fit=on}"
MON_SOCK="${IRIX_MONITOR:-$DIR/run/ip19-mon.sock}"
SERIAL_SOCK="${IRIX_SERIAL_SOCK:-$DIR/run/ip19-serial.sock}"
SNAPSHOT="${IRIX_SNAPSHOT:-0}"
NET="${IRIX_NET:-tap}"
NET_PORT="${IRIX_NET_PORT:-2323}"
TAP_IF="${IRIX_TAP_IF:-tap-iris}"
CONSOLE="${IRIX_CONSOLE:-window}"
USER_ARGS=("$@")
export IP19_NVRAM_READONLY="${IP19_NVRAM_READONLY:-1}"

if [ -z "$DISK" ]; then
  for candidate in "$DIR/sgi/ip19-main.chd" "$DIR/ip19-main.chd" "$DIR/../ip19-main.chd"; do
    if [ -f "$candidate" ]; then
      DISK="$candidate"
      DISK_FORMAT=chd
      break
    fi
  done
fi
if [ -z "$DISK" ]; then
  echo "ERROR: main Challenge image ip19-main.chd not found; set IRIX_DISK." >&2
  exit 1
fi

if [ ! -f "$PROM" ]; then
  echo "ip19prom.bin not found in sgi/challenge/; set PROM=..." >&2
fi

set -- -M ip19 -smp "$SMP" -m "$RAM" -parallel none \
  -bios "$PROM" -L "$DIR/sgi/challenge"

if [ "$SNAPSHOT" = 1 ]; then
  set -- "$@" -snapshot
fi

if [ -n "$DISK" ]; then
  set -- "$@" -drive \
    "file=$DISK,format=$DISK_FORMAT,if=none,id=ip19disk" \
    -device "scsi-hd,drive=ip19disk,bus=main-system-bus/sgi-s1/scsi.0,scsi-id=1"
fi
# Keep a CD-ROM drive present without media so the GTK removable-media
# menu can insert an ISO later.
if [ -n "$CD" ]; then
  set -- "$@" -drive \
    "file=$CD,format=${IRIX_CD_FORMAT:-raw},if=none,id=ip19cdrom,media=cdrom,readonly=on"
else
  set -- "$@" -drive "if=none,id=ip19cdrom,media=cdrom"
fi
set -- "$@" -device \
  "scsi-cd,drive=ip19cdrom,bus=main-system-bus/sgi-s1/scsi.0,scsi-id=5,logical_block_size=512,physical_block_size=512"
case "$NET" in
  off|none|"")
    ;;
  tap)
    if ! (command -v ip >/dev/null 2>&1 && ip link show "$TAP_IF" >/dev/null 2>&1); then
      echo "WARNING: TAP interface '$TAP_IF' not found - guest will have no network." >&2
      echo "See networking.md to set it up." >&2
    else
      set -- "$@" -net "nic,model=epc-enet,netdev=net0" \
        -netdev "tap,id=net0,ifname=$TAP_IF,script=,downscript="
    fi
    ;;
  user)
    set -- "$@" -nic \
      "user,model=epc-enet,net=10.6.2.0/24,host=10.6.2.2,dhcpstart=10.6.2.15,hostfwd=tcp:127.0.0.1:${NET_PORT}-10.6.2.1:23"
    ;;
  *)
    set -- "$@" -nic "$NET"
    ;;
esac

if [ "${IRIX_HEADLESS:-0}" = 1 ]; then
  mkdir -p "$DIR/run"
  rm -f "$MON_SOCK" "$SERIAL_SOCK"
  set -- "$@" -display none \
    -serial "unix:$SERIAL_SOCK,server=on,wait=off" \
    -monitor "unix:$MON_SOCK,server,nowait"
else
  case "$CONSOLE" in
    shell)
      set -- "$@" -display none -serial stdio \
        -monitor "unix:$MON_SOCK,server,nowait"
      ;;
    telnet)
      set -- "$@" -display none \
        -serial "telnet:127.0.0.1:4444,server=on,wait=off" \
        -monitor "unix:$MON_SOCK,server,nowait"
      ;;
    *)
      set -- "$@" -display "$DISPLAY_MODE" -serial vc \
        -monitor "unix:$MON_SOCK,server,nowait"
      ;;
  esac
fi

exec "$QEMU" "$@" "${USER_ARGS[@]}"
