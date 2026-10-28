#!/usr/bin/env bash
# Configure and build qemu-irix for the Indy on Linux.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

./configure \
    --target-list=mips64-softmmu \
    --enable-gtk \
    --disable-werror \
    --disable-docs \
    --disable-tools \
    "$@"

ninja -C build qemu-system-mips64
