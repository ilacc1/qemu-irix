#!/usr/bin/env bash
# Configure and build qemu-irix for the Indy on Linux.
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$ROOT_DIR/qemu"

./configure \
    --target-list=mips64-softmmu \
    --enable-gtk \
    --disable-werror \
    --disable-docs \
    --disable-tools \
    "$@"

ninja -C build qemu-system-mips64
