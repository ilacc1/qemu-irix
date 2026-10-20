#!/usr/bin/env bash
#
# qemu-irix 0.1.0 — Ubuntu dependency installer.
#
# Installs the runtime libraries qemu-system-mips64 needs (GTK3, GLib,
# Pixman, audio), makes the binaries executable, and checks the TAP
# setup for bridged networking. Run once after unpacking:
#
#   ./install.sh
#
# The IRIX disk image itself is NOT included: point IRIX_DISK at your
# IRIX 6.5 CHD, e.g.  IRIX_DISK=~/irix65_4020.chd ./run-irix.sh
set -euo pipefail

cd "$(dirname "$0")"

# soname -> Ubuntu package for the libraries the emulator links.
declare -A PKGMAP=(
    [libgtk-3.so.0]=libgtk-3-0
    [libgdk-3.so.0]=libgtk-3-0
    [libgdk_pixbuf-2.0.so.0]=libgdk-pixbuf-2.0-0
    [libcairo.so.2]=libcairo2
    [libcairo-gobject.so.2]=libcairo-gobject2
    [libpango-1.0.so.0]=libpango-1.0-0
    [libpangocairo-1.0-0.so.0]=libpango-1.0-0
    [libatk-1.0.so.0]=libatk1.0-0t64
    [libatk-bridge-2.0.so.0]=libatk-bridge2.0-0t64
    [libglib-2.0.so.0]=libglib2.0-0t64
    [libgobject-2.0.so.0]=libglib2.0-0t64
    [libgio-2.0.so.0]=libglib2.0-0t64
    [libgmodule-2.0.so.0]=libglib2.0-0t64
    [libpixman-1.so.0]=libpixman-1-0
    [libpulse.so.0]=libpulse0
    [libasound.so.2]=libasound2t64
    [libslirp.so.0]=libslirp0
    [libcurl.so.4]=libcurl4
    [libaio.so.1]=libaio1t64
    [liburing.so.2]=liburing2
)

BIN=build/qemu-system-mips64
MISSING_PKGS=()
if [ -x "$BIN" ]; then
    while read -r soname _; do
        pkg="${PKGMAP[$soname]:-}"
        if [ -z "$pkg" ]; then
            echo "NOTE: $soname has no package mapping — install it manually if ldd reports it missing."
            continue
        fi
        if ! dpkg -s "$pkg" >/dev/null 2>&1; then
            MISSING_PKGS+=("$pkg")
        fi
    done < <(ldd "$BIN" 2>/dev/null | grep "not found" | awk '{print $1}')
    # Also cover linked-but-present libs whose package may still be absent
    # on minimal systems: check the mapped set directly.
    while read -r soname _; do
        pkg="${PKGMAP[$soname]:-}"
        if [ -n "$pkg" ] && ! dpkg -s "$pkg" >/dev/null 2>&1; then
            MISSING_PKGS+=("$pkg")
        fi
    done < <(ldd "$BIN" 2>/dev/null | awk '{print $1}' | grep -E '^lib(g(tk|dk|lib|object|io|module)|pixman|pulse|asound|slirp|curl|aio|uring|cairo|pango|atk|gdk_pixbuf)' | sort -u)
    MISSING_PKGS=($(printf '%s\n' "${MISSING_PKGS[@]}" | sort -u))
fi

if [ "${#MISSING_PKGS[@]}" -gt 0 ]; then
    echo "Installing missing runtime libraries: ${MISSING_PKGS[*]}"
    sudo apt-get update
    sudo apt-get install -y "${MISSING_PKGS[@]}"
else
    echo "All runtime libraries present."
fi

chmod +x "$BIN" run-irix.sh install.sh 2>/dev/null || true

# Bridged networking needs a TAP interface owned by you (default tap-iris).
# This only REPORTS; creating interfaces/bridges is host-specific, so it is
# left to you. run-irix.sh defaults to TAP; use IRIX_NETMODE=user for
# SLIRP (if built in) or IRIX_NONET=1 to skip networking entirely.
if ip tuntap show 2>/dev/null | grep -q .; then
    echo "TAP interfaces present:"
    ip tuntap show
else
    echo "NOTE: no TAP interfaces found. For bridged networking create one, e.g.:"
    echo "  sudo ip tuntap add dev tap-iris mode tap user \$USER"
    echo "  (then enslave it to your bridge; or run with IRIX_NETMODE=user / IRIX_NONET=1)"
fi

if [ -z "${IRIX_DISK:-}" ]; then
    echo "Reminder: set IRIX_DISK to your IRIX 6.5 disk image before running, e.g.:"
    echo "  IRIX_DISK=\$HOME/irix65_4020.chd ./run-irix.sh"
fi

echo "Done."
