#!/usr/bin/env python3
"""Capture the state of a hung guest (black screen).

The hang is a PROM-level one: the CPU ends up spinning at physical
0x1fc03878 (the PROM's panic spin, virtual 0xffffffff9fc03878) after an
exception.  The PROM keeps its state in a "fixed" area at physical
0x1fa00000 (the MC register window is at 0x1fa00000, the PROM's own
fixed area sits just above it), so the snapshot dumps that area as well
as the registers.

Usage: run/panicsnap.py [--monitor PATH] [--out FILE]
"""
import argparse
import os
import re
import socket
import sys
import time

DEFAULT_MON = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "mon.sock")
# The PROM's panic spin loop, physical 0x1fc03878.
PROM_SPIN_PC = 0xFFFFFFFF9FC03878
# The PROM's fixed area (kseg1 alias 0xbfa00000 -> physical 0x1fa00000).
FIXED_BASE = 0x1FA00000
FIXED_SIZE = 0x3000
KSEG0_MASK = 0x1FFFFFFF


def mon_cmd(mon, cmd, wait=1.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(mon)
    time.sleep(0.2)
    s.recv(65536)
    s.sendall(cmd.encode() + b"\n")
    time.sleep(wait)
    s.setblocking(False)
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
    except BlockingIOError:
        pass
    s.close()
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out.decode("utf-8", "replace"))


def physical(vaddr):
    """Map a virtual address to the physical address."""
    v = int(vaddr, 16) & 0xFFFFFFFF
    if v >= 0x80000000:
        return v & KSEG0_MASK
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--monitor", default=DEFAULT_MON)
    ap.add_argument("--out")
    args = ap.parse_args()

    regs = mon_cmd(args.monitor, "info registers")
    pc = re.search(r"pc=\s*(0x[0-9a-f]+)", regs)
    pc_val = int(pc.group(1), 16) if pc else 0
    print(f"pc = 0x{pc_val:08x} (physical 0x{pc_val & KSEG0_MASK:08x})")
    if pc_val != PROM_SPIN_PC:
        print("guest is not in the PROM panic spin", file=sys.stderr)
        if args.out:
            open(args.out, "w").write(regs)
        return 1

    text = [regs]

    # The PROM's fixed area: saved context, message pointers, etc.
    fixed = mon_cmd(args.monitor,
                    f"xp /{FIXED_SIZE // 4}xw 0x{FIXED_BASE:08x}")
    text.append("=== PROM fixed area (physical 0x1fa00000) ===")
    text.append(fixed)

    # Follow pointers found in the fixed area (message strings).
    ptrs = []
    for m in re.finditer(r"^([0-9a-f]{8}): ((?:0x[0-9a-f]{8} ?)+)$",
                         fixed, re.M):
        base = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            val = int(w, 16)
            if 0x80000000 <= val < 0xC0000000:
                ptrs.append((base + 4 * i, val))
    seen = set()
    for off, ptr in ptrs:
        if ptr in seen:
            continue
        seen.add(ptr)
        addr = physical(ptr)
        chunk = mon_cmd(args.monitor, f"xp /64cb 0x{addr:08x}")
        text.append(f"=== string at 0x{ptr:08x} (physical 0x{addr:08x}) ===")
        text.append(chunk)

    out = "\n".join(text)
    if args.out:
        open(args.out, "w").write(out)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
