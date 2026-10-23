#!/usr/bin/env python3
"""Log into the IRIX guest over telnet and run a command.

Usage:
  run/telnet.py [--host H] [--port P] [--user U] [--pass P] [--wait S] "command"

Minimal telnet option handling (answer DO with WONT, WILL with DO) so the
IRIX login prompt appears.  Prints everything the shell writes back.
"""
import argparse
import re
import socket
import sys
import time

IAC = 255
DONT = 254
DO = 253
WONT = 252
WILL = 251
SB = 250
SE = 240


def strip_iac(buf):
    """Remove telnet IAC sequences, returning (clean_text, replies)."""
    out = bytearray()
    replies = bytearray()
    i = 0
    n = len(buf)
    while i < n:
        b = buf[i]
        if b != IAC:
            out.append(b)
            i += 1
            continue
        if i + 1 >= n:
            break
        cmd = buf[i + 1]
        if cmd in (DO, DONT, WILL, WONT):
            if i + 2 >= n:
                break
            opt = buf[i + 2]
            if cmd == DO:
                replies += bytes([IAC, WONT, opt])
            elif cmd == WILL:
                replies += bytes([IAC, DONT, opt])
            i += 3
            continue
        if cmd == SB:
            j = i + 2
            while j + 1 < n and not (buf[j] == IAC and buf[j + 1] == SE):
                j += 1
            i = j + 2
            continue
        if cmd == IAC:
            out.append(IAC)
            i += 2
            continue
        i += 2
    return bytes(out), bytes(replies)


def drain(sock, seconds):
    """Read for a while, answering telnet negotiation."""
    out = b""
    end = time.time() + seconds
    sock.settimeout(0.5)
    while time.time() < end:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        clean, reply = strip_iac(chunk)
        if reply:
            try:
                sock.sendall(reply)
            except OSError:
                break
        out += clean
    return out.decode("latin-1", "replace")


def wait_for(sock, pattern, seconds):
    """Read until pattern matches (or timeout); returns (text, matched)."""
    end = time.time() + seconds
    text = ""
    rx = re.compile(pattern, re.I)
    while time.time() < end:
        chunk = drain(sock, 1.0)
        text += chunk
        if rx.search(text):
            return text, True
    return text, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.6.1.1")
    ap.add_argument("--port", type=int, default=23)
    ap.add_argument("--user", default="guest")
    ap.add_argument("--pass", dest="password", default="")
    ap.add_argument("--wait", type=float, default=3.0)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("command", nargs="*")
    args = ap.parse_args()

    cmd = " ".join(args.command)
    sock = socket.create_connection((args.host, args.port), timeout=10)

    text, ok = wait_for(sock, r"login|name|username", args.wait)
    sys.stdout.write(text)
    if not ok:
        print("!! no login prompt seen", file=sys.stderr)
    sock.sendall((args.user + "\r\n").encode())

    text, ok = wait_for(sock, r"password", args.wait)
    sys.stdout.write(text)
    if ok:
        sock.sendall((args.password + "\r\n").encode())

    text, _ = wait_for(sock, r"[$#>]\s*$", args.wait)
    sys.stdout.write(text)

    if cmd:
        sock.sendall((cmd + "\r\n").encode())
        out = drain(sock, args.timeout)
        sys.stdout.write(out)
    sock.close()


if __name__ == "__main__":
    main()
