#!/usr/bin/env python3
"""Send HMP commands to a QEMU monitor socket and print the reply.

Usage: run/mon.py [socket] command [args...]

The socket defaults to run/mon.sock next to this script (the path
run-irix.sh uses). Multiple commands can be given as separate
arguments; each one is sent on its own line.
"""
import socket
import sys
import time
from pathlib import Path

DEFAULT_SOCK = Path(__file__).resolve().parent / "mon.sock"


def drain(s, wait=0.4):
    """Read whatever the monitor has to say until it goes quiet."""
    out = b""
    s.settimeout(wait)
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        out += chunk
        if b"(qemu)" in out:
            break
    return out


def main():
    args = sys.argv[1:]
    if args and not args[0].startswith("/") and not args[0].endswith(".sock"):
        path = DEFAULT_SOCK
    elif args:
        path = Path(args[0])
        args = args[1:]
    else:
        path = DEFAULT_SOCK
    if not args:
        print(__doc__)
        return 1

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(str(path))
    banner = drain(s)
    for cmd in args:
        s.sendall((cmd + "\n").encode())
        reply = drain(s)
        text = reply.decode(errors="replace")
        # Drop the echoed command (the monitor echoes it with terminal
        # escapes) and the prompt noise.
        lines = [ln for ln in text.splitlines()
                 if ln.strip() and "\x1b" not in ln
                 and not ln.startswith(cmd.strip())
                 and not ln.startswith("(qemu)")]
        if lines:
            print("\n".join(lines))
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
