#!/usr/bin/env python3
"""Drive any QEMU machine's unix-socket serial console.

The launcher only needs to expose a unix serial socket.  This tool is
machine-agnostic: it waits for prompts and sends input, in order.

Usage:
  run/console.py --sock PATH [--log FILE] [--timeout SECONDS] \
      [--expect REGEX --send TEXT]...

Each --expect/--send pair is processed in order.  Output is always printed
and optionally appended to --log.  Exit status is 0 when the final expect
matched, 1 on timeout or connection failure.
"""
import argparse
import re
import socket
import sys
import time


def read_until(sock, rx, deadline, log, timeout):
    text = ""
    while time.time() < deadline:
        sock.settimeout(min(0.5, max(0.05, deadline - time.time())))
        try:
            chunk = sock.recv(8192)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        decoded = chunk.decode("latin-1", "replace")
        text += decoded
        sys.stdout.write(decoded)
        sys.stdout.flush()
        if log:
            log.write(decoded)
            log.flush()
        if rx.search(text):
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", required=True)
    ap.add_argument("--log")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--expect", action="append", default=[])
    ap.add_argument("--send", action="append", default=[])
    ap.add_argument("--send-now", default=None,
                    help="send this immediately after connecting")
    args = ap.parse_args()

    if len(args.send) == len(args.expect) - 1:
        args.send.append("")
    if len(args.expect) != len(args.send):
        ap.error("give --expect/--send pairs, optionally with a final --expect")

    log = open(args.log, "a", encoding="latin-1") if args.log else None
    try:
        last_err = None
        for _ in range(50):
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(args.sock)
                break
            except OSError as err:
                last_err = err
                time.sleep(0.2)
        else:
            print(f"console: cannot connect to {args.sock}: {last_err}",
                  file=sys.stderr)
            return 1

        if args.send_now:
            sock.sendall(args.send_now.encode("latin-1"))

        for expect, send in zip(args.expect, args.send):
            deadline = time.time() + args.timeout
            if not read_until(sock, re.compile(expect, re.I), deadline, log,
                              args.timeout):
                print(f"\nconsole: timeout waiting for {expect!r}",
                      file=sys.stderr)
                return 1
            sock.sendall(send.encode("latin-1"))
        return 0
    finally:
        if log:
            log.close()


if __name__ == "__main__":
    sys.exit(main())
