#!/usr/bin/env python3
"""Drive the guest's serial console (second ESCC port) over a unix socket.

The launcher can expose the serial port as a socket:

    IRIX_ESCC_SOCK=run/esccB.sock ./run-irix.sh

This client logs in (root has an empty password on the IRIX 6.5 demo
image) and runs commands, printing everything the guest sends back.

Usage:
  run/escccon.py [--sock PATH] [--user USER] [--password PW] [--wait S] \
                 [--timeout S] ["command" ...]
"""
import argparse
import re
import socket
import sys
import time

DEFAULT_SOCK = "run/esccB.sock"


class Console:
    def __init__(self, path, timeout=20.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5.0)
        self.sock.connect(path)
        self.text = ""
        self.deadline = time.time() + timeout

    def read(self, seconds=1.0):
        end = time.time() + seconds
        out = ""
        while time.time() < end:
            self.sock.settimeout(max(0.1, min(0.5, end - time.time())))
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            out += data.decode("latin-1", "replace")
        self.text += out
        return out

    def wait_for(self, pattern, seconds=20.0):
        rx = re.compile(pattern, re.I)
        end = time.time() + seconds
        while time.time() < end:
            chunk = self.read(0.5)
            if rx.search(self.text):
                return True
        return False

    def send(self, line):
        self.sock.sendall((line + "\r").encode())

    def command(self, cmd, timeout=20.0):
        self.send(cmd)
        # Wait for the shell to print the prompt again; the prompt that was
        # already on screen does not count.
        end = time.time() + timeout
        start = len(self.text)
        while time.time() < end:
            self.read(0.5)
            new = self.text[start:]
            if re.search(r"[\$#] $|~> ?$", new.rstrip("\r\n")):
                break
        return self.text[start:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--user", default="root")
    ap.add_argument("--password", default="")
    ap.add_argument("--wait", type=float, default=5.0)
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("command", nargs="*")
    args = ap.parse_args()

    con = Console(args.sock, timeout=args.wait)
    con.read(2.0)
    # The getty only prints its prompt after a carriage return, and anything
    # printed before the host connected is gone.
    con.send("")
    if not con.wait_for(r"login:|[\$#] $|~> ?$", args.wait):
        print("!! no login prompt on the serial console", file=sys.stderr)
        print(con.text)
        return 1
    if not re.search(r"login:", con.text, re.I):
        # Already logged in from an earlier session.
        pass
    else:
        con.send(args.user)
        con.read(1.0)
        if con.wait_for(r"[Pp]assword", 3.0):
            con.send(args.password)
            con.read(2.0)
        con.wait_for(r"[\$#] $|~> ?$", args.wait)

    cmd = " ".join(args.command)
    if cmd:
        out = con.command(cmd, args.timeout)
        sys.stdout.write(out)
    else:
        sys.stdout.write(con.text)
    con.sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
