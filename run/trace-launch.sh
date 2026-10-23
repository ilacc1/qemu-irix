#!/usr/bin/env bash
#
# Launch a traced guest detached, so the process survives this shell.
# Probes enabled: PRIMLOG (SEQ/ZEROSTORE/SYNCMIS/MISADDR/SYNCZERO/LOOP),
# SEQLOG (per-command header), FILLSTATS (paths/pixel counts), STORE
# (first stores per mode). Log goes to /tmp/irix-trace.log, startup to
# /tmp/irix-run.log.
#
# Usage: run/trace-launch.sh [extra run-irix.sh args]
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
LOG=${IRIX_TRACE_LOG:-/tmp/irix-trace.log}
RUNLOG=${IRIX_RUN_LOG:-/tmp/irix-run.log}

: > "$LOG"
: > "$RUNLOG"

export SGI_GIO_PRIMLOG=1
export SGI_GIO_SEQLOG=1
export SGI_GIO_FILLSTATS=1
export SGI_GIO_STORE=1

echo "trace -> $LOG"
echo "run   -> $RUNLOG"

setsid env \
    "$ROOT_DIR/run-irix.sh" "$@" >"$RUNLOG" 2>>"$LOG" </dev/null &
disown

sleep 2
pgrep -af qemu-system-mips64 | head
