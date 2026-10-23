#!/usr/bin/env python3
"""OCR the guest's screen: screendump through the monitor, then tesseract.

Usage: run/ocrshot.py OUTPUT.png [--ppm FILE] [--monitor PATH]
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shot import monitor_cmd, DEFAULT_MON  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--ppm")
    ap.add_argument("--monitor", default=DEFAULT_MON)
    args = ap.parse_args()

    ppm = args.ppm or (os.path.splitext(args.output)[0] + ".ppm")
    monitor_cmd(args.monitor, f"screendump {ppm}", wait=1.5)
    from PIL import Image

    im = Image.open(ppm).convert("L")
    im = im.resize((im.width * 3 // 2, im.height * 3 // 2))
    im.save(args.output)
    out = subprocess.run(["tesseract", args.output, "stdout"],
                         capture_output=True, text=True)
    text = out.stdout.strip()
    print(text if text else "(no text recognised)")
    return 0


if __name__ == "__main__":
    main()
