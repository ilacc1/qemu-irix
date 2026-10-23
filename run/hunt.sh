#!/usr/bin/env bash
#
# Repeatedly boot the guest until the black-screen hang shows up.
#
# The hang is intermittent, so a single boot proves nothing: this loop runs
# run/bootcheck.sh again and again (each attempt is a cold boot) and stops
# at the first hang, keeping the QEMU log and the hang snapshot.
#
# Usage: run/hunt.sh [--attempts N] [--timeout SECONDS] [--logdir DIR] [--no-log]
#                    [env=VAL ...]
#
#   --attempts N       how many boots to try (default 10)
#   --timeout SECONDS  per-boot watch timeout (default 180)
#   --logdir DIR       where to keep the per-boot logs (default /tmp/irix-hunt)
#   --no-log           skip the -d int,mmu log (a booting kernel writes
#                      gigabytes of it, which fills /tmp)
#   RAM=...  MC=...    IRIX_RAM / IRIX_MC_SIMMS overrides
set -uo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT_DIR"

ATTEMPTS=10
TIMEOUT=180
LOGDIR=/tmp/irix-hunt
LOG=1
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --attempts) ATTEMPTS=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --logdir) LOGDIR=$2; shift 2 ;;
        --no-log) LOG=0; shift ;;
        RAM=*) EXTRA+=("RAM=${1#RAM=}"); shift ;;
        MC=*) EXTRA+=("MC=${1#MC=}"); shift ;;
        *) echo "unknown argument: $1" >&2; exit 64 ;;
    esac
done

mkdir -p "$LOGDIR"

for n in $(seq 1 "$ATTEMPTS"); do
    log="$LOGDIR/boot-$(printf '%02d' $n).log"
    snap="$LOGDIR/hang-$(printf '%02d' $n).txt"
    echo "=== attempt $n/$ATTEMPTS (log $log) ==="
    args=(--timeout "$TIMEOUT" --snapshot "$snap")
    [ "$LOG" = 1 ] && args+=(--log "$log")
    out=$(run/bootcheck.sh "${args[@]}" "${EXTRA[@]}" 2>&1)
    echo "$out" | grep -E "VERDICT|in PROM|left the PROM|snapshot|log:"
    if echo "$out" | grep -q "VERDICT: HANG"; then
        echo "=== HANG reproduced on attempt $n ==="
        echo "log: $log"
        echo "snapshot: $snap"
        exit 1
    fi
done

echo "=== no hang in $ATTEMPTS attempts ==="
exit 0
