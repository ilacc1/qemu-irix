#!/usr/bin/env python3
"""Type a string into the guest through the QEMU monitor (HMP sendkey).

Usage:
  run/sendkeys.py "text" [--monitor PATH] [--delay S]

Characters that HMP sendkey cannot express directly are replaced by the
closest key combination (shift/ctrl) or skipped.  A trailing newline is
sent unless --no-newline is given.
"""
import argparse
import socket
import sys
import time

SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "shift-equal",
    ":": "shift-semicolon", '"': "shift-apostrophe", "<": "shift-comma",
    ">": "shift-dot", "?": "shift-slash", "|": "shift-backslash",
    "~": "shift-grave_accent", "{": "shift-bracket_left",
    "}": "shift-bracket_right",
}

NAMED = {
    " ": "spc", "\t": "tab", "\r": "ret", "\n": "ret",
    "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    ";": "semicolon", "'": "apostrophe", ",": "comma", ".": "dot",
    "/": "slash", "\\": "backslash", "`": "grave_accent",
}


def key_for(ch):
    if ch in NAMED:
        return NAMED[ch]
    if ch in SHIFTED:
        return "shift-" + SHIFTED[ch]
    if ch.isupper():
        return "shift-" + ch.lower()
    if ch.isalnum():
        return ch
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text")
    ap.add_argument("--monitor", default=None)
    ap.add_argument("--delay", type=float, default=0.03)
    ap.add_argument("--no-newline", action="store_true")
    args = ap.parse_args()

    monitor = args.monitor
    if monitor is None:
        import os
        monitor = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "mon.sock")

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(monitor)
    time.sleep(0.2)
    sock.recv(65536)

    keys = [key_for(c) for c in args.text]
    keys = [k for k in keys if k]
    if not args.no_newline:
        keys.append("ret")
    for k in keys:
        sock.sendall(("sendkey %s\n" % k).encode())
        sock.recv(65536)
        time.sleep(args.delay)
    sock.close()
    print("sent %d keys" % len(keys))


if __name__ == "__main__":
    main()
