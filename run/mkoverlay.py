#!/usr/bin/env python3
"""Create a qcow2 copy-on-write overlay for a CHD disk image.

The overlay keeps the original image pristine: QEMU opens the CHD
read-only as the backing file and stores every guest write in the
overlay, so the guest's data persists between runs while the original
image can never be damaged.

The fork's QEMU has the CHD block driver but no qemu-img (the upstream
tools are not part of this tree), so the overlay is created with QEMU's
own "blockdev-create" job through a QMP session on a throw-away
"-M none" machine.

Usage: run/mkoverlay.py [--disk PATH] [--overlay PATH] [--force]
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(ROOT_DIR, "build", "qemu-system-mips64")
CHD_MAGIC = b"MComprHD"


def chd_logical_size(path):
    """Return the virtual size of a CHD image (its header is little endian
    big endian mixed; the values used here are big endian)."""
    import struct

    with open(path, "rb") as f:
        hdr = f.read(128)
    if hdr[:8] != CHD_MAGIC:
        raise SystemExit(f"{path}: not a CHD image")
    version = struct.unpack_from(">I", hdr, 12)[0]
    if version == 5:
        return struct.unpack_from(">Q", hdr, 32)[0]
    if version in (3, 4):
        return struct.unpack_from(">Q", hdr, 28)[0]
    if version == 2:
        seclen = struct.unpack_from(">I", hdr, 76)[0]
        return (struct.unpack_from(">I", hdr, 32)[0] *
                struct.unpack_from(">I", hdr, 36)[0] *
                struct.unpack_from(">I", hdr, 40)[0] * seclen)
    if version == 1:
        return (struct.unpack_from(">I", hdr, 32)[0] *
                struct.unpack_from(">I", hdr, 36)[0] *
                struct.unpack_from(">I", hdr, 40)[0] * 512)
    raise SystemExit(f"{path}: unsupported CHD version {version}")


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.sock.settimeout(30.0)
        self.buf = ""
        self.greeting = self.read_message()

    def read_message(self):
        while "\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise SystemExit("QMP connection closed")
            self.buf += chunk.decode("utf-8", "replace")
        line, self.buf = self.buf.split("\n", 1)
        return json.loads(line)

    def execute(self, command, **args):
        cmd = {"execute": command}
        if args:
            cmd["arguments"] = args
        self.sock.sendall((json.dumps(cmd) + "\r\n").encode())
        while True:
            msg = self.read_message()
            if "event" in msg:
                continue
            if "error" in msg:
                raise SystemExit(f"{command} failed: {msg['error']}")
            return msg.get("return")

    def close(self):
        self.sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk", default=os.path.join(ROOT_DIR, "irix65_4020.chd"))
    ap.add_argument("--overlay")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    disk = os.path.abspath(args.disk)
    overlay = os.path.abspath(args.overlay or (disk + ".cow.qcow2"))

    if not os.path.exists(disk):
        raise SystemExit(f"disk image not found: {disk}")
    if os.path.exists(overlay):
        if not args.force:
            print(f"overlay already exists: {overlay}")
            return 0
        os.unlink(overlay)

    size = chd_logical_size(disk)
    # The qcow2 create job opens the target file node read-write, so the
    # file has to exist before the job starts.
    with open(overlay, "wb"):
        pass
    tmpdir = tempfile.mkdtemp(prefix="mkoverlay-")
    sock_path = os.path.join(tmpdir, "qmp.sock")
    proc = subprocess.Popen(
        [QEMU, "-M", "none", "-display", "none",
         "-qmp", f"unix:{sock_path},server=on,wait=off"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            if os.path.exists(sock_path):
                break
            time.sleep(0.05)
        else:
            raise SystemExit("QEMU did not create the QMP socket")

        qmp = Qmp(sock_path)
        qmp.execute("qmp_capabilities")
        job_id = "cow-create"
        qmp.execute(
            "blockdev-create",
            **{
                "job-id": job_id,
                "options": {
                    "driver": "qcow2",
                    "file": {"driver": "file", "filename": overlay},
                    "size": size,
                    "backing-file": disk,
                    "backing-fmt": "chd",
                },
            })
        deadline = time.time() + 120
        while time.time() < deadline:
            jobs = qmp.execute("query-jobs")
            job = next((j for j in jobs if j.get("id") == job_id), None)
            if job is None:
                break
            status = job.get("status")
            if status in ("concluded", "null"):
                if job.get("error"):
                    raise SystemExit(f"overlay creation failed: {job['error']}")
                break
            if status in ("cancelled", "aborting"):
                raise SystemExit("overlay creation was cancelled")
            time.sleep(0.2)
        else:
            raise SystemExit("timed out creating the overlay")
        try:
            qmp.execute("job-dismiss", **{"id": job_id})
        except SystemExit:
            pass
        qmp.execute("quit")
        qmp.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        os.unlink(sock_path) if os.path.exists(sock_path) else None
        os.rmdir(tmpdir)

    print(f"created {overlay} (virtual size {size} bytes, backing {disk})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
