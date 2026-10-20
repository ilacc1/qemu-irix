#!/usr/bin/env bash
#
# qemu-irix 0.1.0 — Ubuntu dependency checker.
#
# The runtime libraries qemu-system-mips64 needs ship inside this
# distribution (lib/), so there is nothing to download. This script
# just verifies the bundle is complete, makes the binaries executable,
# and checks the TAP setup for bridged networking. Run once after
# unpacking:
#
#   ./install.sh
#
# The IRIX disk image itself is NOT included: put irix65_4020.chd next
# to run-irix.sh, or point IRIX_DISK at it, e.g.
#   IRIX_DISK=~/irix65_4020.chd ./run-irix.sh
set -euo pipefail

cd "$(dirname "$0")"

BIN=build/qemu-system-mips64

if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN not found - re-extract the distribution tarball." >&2
    exit 1
fi

# The bundle carries its own libs; the loader must resolve everything
# through lib/ (plus the host libc, which always stays system-side).
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
STILL_MISSING=$(ldd "$BIN" 2>/dev/null | grep "not found" | awk '{print $1}' | sort -u || true)
if [ -n "$STILL_MISSING" ]; then
    echo "ERROR: unresolved libraries even with the bundled lib/: $STILL_MISSING" >&2
    echo "Re-extract the distribution tarball." >&2
    exit 1
fi
echo "All runtime libraries present (bundled lib/)."

chmod +x "$BIN" run-irix.sh install.sh 2>/dev/null || true

if [ ! -f indy/ip24prom.070-9101-011.bin ]; then
    echo "ERROR: indy/ip24prom.070-9101-011.bin missing - re-extract the tarball." >&2
    exit 1
fi
if [ ! -f indy/nvram/rtc ]; then
    echo "ERROR: indy/nvram/rtc missing - re-extract the tarball." >&2
    exit 1
fi

# Bridged networking needs a TAP interface owned by you (default tap-iris).
# This only REPORTS; setup is documented in networking.md. run-irix.sh
# boots without networking when the TAP is absent; use IRIX_NETMODE=user
# for SLIRP (if built in) or IRIX_NONET=1 to skip networking entirely.
if ip tuntap show 2>/dev/null | grep -q .; then
    echo "TAP interfaces present:"
    ip tuntap show
else
    echo "NOTE: no TAP interfaces found - the guest will have no network."
    echo "Setup guide: https://github.com/ilacc1/qemu-irix/blob/master/networking.md"
fi

if [ ! -f "${IRIX_DISK:-irix65_4020.chd}" ]; then
    echo "Reminder: put irix65_4020.chd next to run-irix.sh, or set IRIX_DISK"
    echo "to your IRIX 6.5 disk image before running, e.g.:"
    echo "  IRIX_DISK=\$HOME/irix65_4020.chd ./run-irix.sh"
fi

echo "Done."
