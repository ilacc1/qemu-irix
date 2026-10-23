#!/usr/bin/env python3
"""Take a screendump of the running guest through the QEMU monitor.

Usage: run/shot.py OUTPUT.ppm [--monitor PATH] [--cmd "COMMAND"]
"""
import argparse
import os
import socket
import time

DEFAULT_MON = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "mon.sock")


def monitor_cmd(mon_path, cmd, wait=1.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(mon_path)
    time.sleep(0.2)
    s.recv(65536)
    s.sendall(cmd.encode() + b"\n")
    time.sleep(wait)
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--monitor", default=DEFAULT_MON)
    ap.add_argument("--cmd", help="monitor command to run before the dump")
    args = ap.parse_args()

    if args.cmd:
        monitor_cmd(args.monitor, args.cmd)
    monitor_cmd(args.monitor, f"screendump {args.output}", wait=1.5)
    print(args.output)


if __name__ == "__main__":
    main()
