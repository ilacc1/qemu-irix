#!/usr/bin/env bash
#
# Reproducible release packaging for qemu-irix.
#
#   ./release.sh [version] [--with-windows]
#
# Builds dist/qemu-irix-<version>-ubuntu.tar.gz from the local Linux
# build, and optionally dist/qemu-irix-<version>-windows.zip by pulling
# the Windows binary + DLL closure from the Windows build host over ssh.
#
# No credentials live in this script: the Windows side uses plain ssh/scp
# (keys or agent). Configure via environment:
#   WIN_SSH_TARGET  user@host of the Windows build machine (enables --with-windows)
#   WIN_SRC_DIR     repo path on that machine (/c/apps/0irix/qemu-irix)
#   WIN_MINGW       mingw root on that machine (/mingw64)
#   BUILD_DIR       local Linux build dir (default: build)
#
# The disk image is never bundled: it stays a user-supplied default
# (irix65_4020.chd) resolved by the launchers at runtime.
set -euo pipefail

VER="${1:-0.1.0}"
WITH_WINDOWS=0
[ "${2:-}" = "--with-windows" ] && WITH_WINDOWS=1

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DIST="$ROOT_DIR/dist"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
WIN_SSH_TARGET="${WIN_SSH_TARGET:-}"
WIN_SRC_DIR="${WIN_SRC_DIR:-/c/apps/0irix/qemu-irix}"
# msys root (parent of mingw64/); / resolves via the /mingw64 mount.
WIN_MINGW="${WIN_MINGW:-/}"

mkdir -p "$DIST"

# --- Ubuntu package -------------------------------------------------------
UBUNTU_STAGE="$DIST/stage-ubuntu/qemu-irix-$VER"
rm -rf "$DIST/stage-ubuntu"
mkdir -p "$UBUNTU_STAGE/build" "$UBUNTU_STAGE/lib"

EXE="$BUILD_DIR/qemu-system-mips64"
if [ ! -x "$EXE" ]; then
    echo "Linux emulator not found: $EXE (set BUILD_DIR or build it first)" >&2
    exit 1
fi
cp "$EXE" "$UBUNTU_STAGE/build/"

# Bundle the shared-library closure, minus libc/loader (always host-side).
ldd "$EXE" | awk '/=> \//{print $1 "=" $3}' | while IFS='=' read -r soname path; do
    real=$(realpath "$path")
    pkg=$(dpkg -S "$real" 2>/dev/null | cut -d: -f1 | head -1 || true)
    case "$pkg" in libc6|libc6-amd64) continue;; esac
    cp -L "$path" "$UBUNTU_STAGE/lib/$soname"
done

cp -r "$ROOT_DIR/indy" "$UBUNTU_STAGE/"
cp "$ROOT_DIR/run-irix.sh" "$ROOT_DIR/install.sh" \
   "$ROOT_DIR/readme.md" "$ROOT_DIR/building.md" "$ROOT_DIR/networking.md" \
   "$UBUNTU_STAGE/"
chmod +x "$UBUNTU_STAGE/build/qemu-system-mips64" \
         "$UBUNTU_STAGE/run-irix.sh" "$UBUNTU_STAGE/install.sh"

# Sanity: everything must resolve through the bundle.
if LD_LIBRARY_PATH="$UBUNTU_STAGE/lib" ldd "$UBUNTU_STAGE/build/qemu-system-mips64" \
    | grep -q "not found"; then
    echo "ERROR: bundled lib/ does not close the linkage:" >&2
    LD_LIBRARY_PATH="$UBUNTU_STAGE/lib" ldd "$UBUNTU_STAGE/build/qemu-system-mips64" \
        | grep "not found" >&2
    exit 1
fi

rm -f "$DIST/qemu-irix-$VER-ubuntu.tar.gz"
tar czf "$DIST/qemu-irix-$VER-ubuntu.tar.gz" -C "$DIST/stage-ubuntu" "qemu-irix-$VER"
echo "wrote $DIST/qemu-irix-$VER-ubuntu.tar.gz"

# --- Windows package (from the Windows build host) -------------------------
if [ "$WITH_WINDOWS" -eq 1 ]; then
    if [ -z "$WIN_SSH_TARGET" ]; then
        echo "WIN_SSH_TARGET not set (user@windows-build-host)" >&2
        exit 1
    fi
    WIN_STAGE="$DIST/stage-windows/qemu-irix-$VER"
    rm -rf "$DIST/stage-windows"
    mkdir -p "$WIN_STAGE/qemu/build" "$WIN_STAGE/msys64/mingw64/bin" \
             "$WIN_STAGE/msys64/mingw64"

    # On the remote: tar the exe plus the exact mingw64 DLL closure
    # (computed with ldd there, not hardcoded) plus GTK runtime data.
    # Everything lands under mingw64/ in the tarball to match the moves
    # below. WIN_MINGW is the msys root (parent of mingw64/); the default
    # / works on a standard MSYS2 install via its /mingw64 mount. The
    # remote shell must be bourne-compatible and ssh key/agent auth is
    # expected (no credentials live in this script).
    # shellcheck disable=SC2029
    ssh "$WIN_SSH_TARGET" \
        "msys64/usr/bin/bash.exe -lc 'cd $WIN_SRC_DIR && EXE=qemu/build/qemu-system-mips64.exe && DLLS=\$(MSYSTEM=MINGW64 PATH=$WIN_MINGW/mingw64/bin:/usr/bin:\$PATH ldd \$EXE | awk '\''{print \$3}'\'' | grep -i mingw64 | sed '\''s|.*/mingw64/|mingw64/|'\'' | sort -u) && tar czf /tmp/qemu-irix-win.tgz -C qemu/build qemu-system-mips64.exe -C $WIN_MINGW \$DLLS mingw64/lib/gdk-pixbuf-2.0 mingw64/share/glib-2.0/schemas && ls -la /tmp/qemu-irix-win.tgz'"
    scp "$WIN_SSH_TARGET:/tmp/qemu-irix-win.tgz" "$DIST/win-remote.tgz"

    mkdir -p "$DIST/win-remote" && rm -rf "$DIST/win-remote" && mkdir -p "$DIST/win-remote"
    tar xzf "$DIST/win-remote.tgz" -C "$DIST/win-remote"
    mv "$DIST/win-remote/qemu-system-mips64.exe" "$WIN_STAGE/qemu/build/"
    mv "$DIST/win-remote"/mingw64/bin/*.dll "$WIN_STAGE/msys64/mingw64/bin/"
    mv "$DIST/win-remote"/mingw64/{lib,share} "$WIN_STAGE/msys64/mingw64/"

    cp -r "$ROOT_DIR/indy" "$WIN_STAGE/"
    # CRLF for everything cmd.exe parses (normalize first: repo copies
    # may be LF or CRLF depending on checkout settings).
    for f in run-irix.bat install.bat; do
        sed 's/\r$//' "$ROOT_DIR/$f" | sed 's/$/\r/' > "$WIN_STAGE/$f"
    done
    cp "$ROOT_DIR/readme.md" "$ROOT_DIR/building.md" "$ROOT_DIR/networking.md" \
       "$WIN_STAGE/"

    rm -f "$DIST/qemu-irix-$VER-windows.zip"
    python3 -c "import shutil; shutil.make_archive('$DIST/qemu-irix-$VER-windows','zip',root_dir='$DIST/stage-windows',base_dir='qemu-irix-$VER')"
    echo "wrote $DIST/qemu-irix-$VER-windows.zip"
fi

echo "done."
