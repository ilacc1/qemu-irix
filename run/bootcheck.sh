#!/usr/bin/env bash
#
# Boot the IRIX guest and report whether it hangs.
#
# The hang this script looks for is the black-screen one: the PROM spins at
# physical 0x1fc03878 after its boot-mode TLB refill handler gave up, so the
# CPU sits in PROM code and never reaches the kernel.
#
# Usage: run/bootcheck.sh [--timeout SECONDS] [--log FILE] [env=VAL ...]
#
#   --timeout SECONDS  how long to watch the boot (default 180)
#   --log FILE         also write QEMU's -d int,mmu log to FILE
#   --snapshot FILE    write a hang snapshot (registers + PROM fixed area)
#   RAM=...  MC=...    IRIX_RAM / IRIX_MC_SIMMS overrides
#
# The -d int,mmu log grows by millions of lines once the kernel runs, so
# logging is switched off as soon as the verdict is known.
#
# Exit status: 0 = kernel is running, 1 = hang detected, 2 = still in the
# PROM when the timeout expired.
set -uo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT_DIR"

TIMEOUT=180
LOGFILE=""
SNAPFILE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) TIMEOUT=$2; shift 2 ;;
        --log) LOGFILE=$2; shift 2 ;;
        --snapshot) SNAPFILE=$2; shift 2 ;;
        RAM=*) export IRIX_RAM=${1#RAM=}; shift ;;
        MC=*) export IRIX_MC_SIMMS=${1#MC=}; shift ;;
        *) echo "unknown argument: $1" >&2; exit 64 ;;
    esac
done

# The PROM lives at physical 0x1fc00000 (256K), the kernel is in RAM.
PROM_LO=$((0x1fc00000))
PROM_HI=$((0x1fc40000))
PROM_SPIN=$((0x1fc03878))

MON=run/mon.py

pc_of() {
    # "info registers" prints "pc=0xffffffff9fc03878 HI=..."
    "$MON" "info registers" 2>/dev/null \
        | grep -o 'pc=0x[0-9a-f]*' | head -1 | cut -d= -f2
}

# Stop any running guest, then start a fresh one.
run/restart-guest.sh --no-launch >/dev/null 2>&1

QEMU_ARGS=()
if [ -n "$LOGFILE" ]; then
    QEMU_ARGS=(-d int,mmu -D "$LOGFILE")
fi

setsid ./run-irix.sh "${QEMU_ARGS[@]}" >/tmp/irix-run.log 2>/tmp/irix-store.log </dev/null &
disown || true

# Wait for the monitor socket to show up.
for _ in $(seq 1 50); do
    [ -S run/mon.sock ] && break
    sleep 0.2
done

deadline=$((SECONDS + TIMEOUT))
verdict="timeout"
last_pc=""
while [ $SECONDS -lt $deadline ]; do
    pc=$(pc_of)
    if [ -z "$pc" ]; then
        sleep 2
        continue
    fi
    phys=$(( pc & 0x1fffffff ))
    if [ "$phys" -ge "$PROM_LO" ] && [ "$phys" -lt "$PROM_HI" ]; then
        if [ "$phys" -eq "$PROM_SPIN" ]; then
            verdict="hang"
            break
        fi
        [ "$pc" != "$last_pc" ] && { echo "in PROM: pc=$pc"; last_pc=$pc; }
    elif [ "$pc" != "$last_pc" ]; then
        echo "left the PROM: pc=$pc"
        last_pc=$pc
        verdict="kernel"
        break
    fi
    sleep 2
done

echo "---"
# Keep QEMU's stderr (SGI_TRACE output) next to the log of this attempt.
if [ -n "$LOGFILE" ] && [ -f /tmp/irix-store.log ]; then
    cp /tmp/irix-store.log "${LOGFILE%.log}.store.log" 2>/dev/null
fi
case "$verdict" in
    kernel) echo "VERDICT: kernel is running (pc=$pc)" ;;
    hang)   echo "VERDICT: HANG - PROM spin at pc=$pc (physical $(printf '0x%x' $phys))" ;;
    timeout) echo "VERDICT: timeout - still in the PROM after ${TIMEOUT}s (pc=$pc)" ;;
esac

# The exception log runs at millions of lines per second once the kernel
# starts, so stop logging now that the verdict is known.
if [ -n "$LOGFILE" ]; then
    "$MON" "log none" >/dev/null 2>&1
    echo "log: $LOGFILE ($(wc -l < "$LOGFILE") lines)"
fi

if [ "$verdict" = "hang" ]; then
    snap="$SNAPFILE"
    [ -n "$snap" ] || snap="/tmp/irix-hang-snapshot.txt"
    echo "snapshot: $snap"
    run/panicsnap.py --out "$snap" >/dev/null 2>&1
    grep -E "^(pc =|CP0 Status|BadVAddr)" "$snap" 2>/dev/null | head -20
fi
exit 0
