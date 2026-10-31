#!/bin/sh
# Minimal launcher for IP19/Challenge (Everest) - adjust paths/prom as needed
DIR=$(dirname "$0")
QEMU="$DIR/build/qemu-system-mips64"
PROM="$DIR/sgi/challenge/ip19prom.bin"
DISK="$IRIX_DISK"
SMP="${IRIX_SMP:-1}"
RAM="${IRIX_RAM:-512M}"
DISPLAY_MODE="${IRIX_DISPLAY:-gtk,zoom-to-fit=on}"

if [ -z "$PROM" ] || [ ! -f "$PROM" ]; then
  echo "ip19prom.bin not found in sgi/challenge/; set PROM=..." >&2
fi
if [ -z "$DISK" ]; then
  echo "IRIX_DISK not set (point to CHD/disk image)" >&2
fi

exec "$QEMU" -M ip19 -smp "$SMP" -m "$RAM" \
  -bios "$PROM" -L "$DIR/sgi/challenge" \
  ${DISK:+-drive file="$DISK",format=chd,if=scsi,bus=0,unit=1,media=disk} \
  -display "$DISPLAY_MODE" -serial stdio -monitor none \
  "$@"
