#!/usr/bin/env bash
#
# Record a perf profile of the running guest and print the standard report.
#
# Usage: run/profile.sh [seconds] [data-file]
#
# The guest must already be running the workload you want to profile.
# perf's --children and --sort dso reports are unusably slow on this
# machine, so the report uses --no-children with a comm/symbol sort.
set -euo pipefail

SECONDS_PROFILED=${1:-20}
DATA=${2:-/tmp/perf-run.data}

PID=$(pgrep -f "qemu-system-mips64 -M indy" | head -1 || true)
if [ -z "$PID" ]; then
    echo "no running guest found" >&2
    exit 1
fi

echo "profiling pid $PID for ${SECONDS_PROFILED}s -> $DATA"
perf record -F 999 -g --call-graph dwarf -p "$PID" -o "$DATA" -- sleep "$SECONDS_PROFILED"

echo
echo "=== per-thread (--sort comm) ==="
perf report -i "$DATA" --stdio --no-children -g none --sort comm | head -25
echo
echo "=== per-symbol (--sort comm,symbol) ==="
perf report -i "$DATA" --stdio --no-children -g none --sort comm,symbol | grep -E "sgi_gio|update_display|raster_segment|put_pixel|rex3|vc2|gfx_sync|vblank" | head -30
