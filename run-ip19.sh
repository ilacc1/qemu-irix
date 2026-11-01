#!/bin/sh
# IP19/Challenge (Everest) launcher.
#
# Headless console/monitor harness:
#   IRIX_HEADLESS=1 IRIX_DISK=... ./run-ip19.sh
# exposes:
#   run/ip19-serial.sock  (guest serial console)
#   run/ip19-mon.sock     (QEMU HMP monitor)
# and can be driven with the machine-agnostic run/console.py and run/mon.py.
#
# Disks/CDs:
#   IRIX_DISK=...       target 1, format IRIX_DISK_FORMAT (default chd)
#   IRIX_CD=...         target 5, format IRIX_CD_FORMAT (default raw)
#   IRIX_CD2=...        target 6
DIR=$(dirname "$0")
QEMU="$DIR/build/qemu-system-mips64"
PROM="$DIR/sgi/challenge/ip19prom.bin"
DISK="$IRIX_DISK"
CD="$IRIX_CD"
CD2="$IRIX_CD2"
SMP="${IRIX_SMP:-1}"
RAM="${IRIX_RAM:-512M}"
DISPLAY_MODE="${IRIX_DISPLAY:-gtk,zoom-to-fit=on}"
MON_SOCK="${IRIX_MONITOR:-$DIR/run/ip19-mon.sock}"
SERIAL_SOCK="${IRIX_SERIAL_SOCK:-$DIR/run/ip19-serial.sock}"
SNAPSHOT="${IRIX_SNAPSHOT:-0}"

if [ ! -f "$PROM" ]; then
  echo "ip19prom.bin not found in sgi/challenge/; set PROM=..." >&2
fi

set -- -M ip19 -smp "$SMP" -m "$RAM" \
  -bios "$PROM" -L "$DIR/sgi/challenge"

if [ "$SNAPSHOT" = 1 ]; then
  set -- "$@" -snapshot
fi

if [ -n "$DISK" ]; then
  set -- "$@" -drive \
    "file=$DISK,format=${IRIX_DISK_FORMAT:-chd},if=none,id=ip19disk"
fi
if [ -n "$CD" ]; then
  set -- "$@" -drive \
    "file=$CD,format=${IRIX_CD_FORMAT:-raw},if=none,id=ip19cdrom,readonly=on"
fi
if [ -n "$CD2" ]; then
  set -- "$@" -drive \
    "file=$CD2,format=${IRIX_CD2_FORMAT:-raw},if=none,id=ip19cdrom2,readonly=on"
fi

if [ "${IRIX_HEADLESS:-0}" = 1 ]; then
  mkdir -p "$DIR/run"
  rm -f "$MON_SOCK" "$SERIAL_SOCK"
  set -- "$@" -display none \
    -serial "unix:$SERIAL_SOCK,server=on,wait=off" \
    -monitor "unix:$MON_SOCK,server,nowait"
else
  set -- "$@" -display "$DISPLAY_MODE" -serial stdio -monitor none
fi

exec "$QEMU" "$@"
