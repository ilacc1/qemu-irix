#!/usr/bin/env bash
#
# Restart the IRIX guest with the current build.
#
# Used to validate changes: the guest keeps running the binary it was started
# with, so a new build needs a restart. The guest is matched by /proc
# (comm == qemu-system-mip AND IRIX_QEMU_MATCH in the cmdline) instead of a
# bare pkill pattern, so only the intended machine is touched.
#
# Usage: run/restart-guest.sh --match PATTERN [--run-script SCRIPT]
#                           [--no-launch]
#   --match PATTERN       required cmdline substring (or $IRIX_QEMU_MATCH)
#   --run-script SCRIPT   launcher to start (or $IRIX_RUN_SCRIPT)
#   --no-launch           only stop the running guest
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LAUNCH=1
MATCH=${IRIX_QEMU_MATCH:-}
RUN_SCRIPT=${IRIX_RUN_SCRIPT:-}
while [ $# -gt 0 ]; do
    case "$1" in
        --no-launch) LAUNCH=0; shift ;;
        --match) MATCH=$2; shift 2 ;;
        --run-script) RUN_SCRIPT=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 64 ;;
    esac
done
if [ -z "$MATCH" ]; then
    echo "restart-guest: --match (or IRIX_QEMU_MATCH) is required" >&2
    exit 64
fi
if [ "$LAUNCH" = 1 ] && [ -z "$RUN_SCRIPT" ]; then
    echo "restart-guest: --run-script (or IRIX_RUN_SCRIPT) is required" >&2
    exit 64
fi

stop_guest() {
    local pid stopped=0
    for pid in $(ls /proc | grep -E '^[0-9]+$'); do
        [ -r "/proc/$pid/comm" ] || continue
        [ "$(cat "/proc/$pid/comm" 2>/dev/null || true)" = "qemu-system-mip" ] \
            || continue
        tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null \
            | grep -q -- "$MATCH" || continue
        echo "stopping guest pid $pid"
        kill "$pid" 2>/dev/null || true
        stopped=1
    done
    [ "$stopped" = 1 ] || { echo "no guest running"; return 0; }
    for _ in $(seq 1 50); do
        local alive=0
        for pid in $(ls /proc | grep -E '^[0-9]+$'); do
            [ -r "/proc/$pid/comm" ] || continue
            [ "$(cat "/proc/$pid/comm" 2>/dev/null || true)" = "qemu-system-mip" ] \
                || continue
            tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null \
                | grep -q -- "$MATCH" || continue
            alive=1
        done
        [ "$alive" = 0 ] && { echo "guest stopped"; return 0; }
        sleep 0.2
    done
    echo "guest did not exit, sending SIGKILL" >&2
    for pid in $(ls /proc | grep -E '^[0-9]+$'); do
        [ -r "/proc/$pid/comm" ] || continue
        [ "$(cat "/proc/$pid/comm" 2>/dev/null || true)" = "qemu-system-mip" ] \
            || continue
        tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null \
            | grep -q -- "$MATCH" || continue
        kill -9 "$pid" 2>/dev/null || true
    done
    sleep 0.5
}

stop_guest

[ "$LAUNCH" = 1 ] || exit 0

cd "$ROOT_DIR"
setsid "$RUN_SCRIPT" >/tmp/irix-run.log 2>/tmp/irix-store.log </dev/null &
disown || true
sleep 1
echo "guest launched (log: /tmp/irix-run.log)"
