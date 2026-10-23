#!/usr/bin/env python3
"""Compare screen dumps numerically and print a coarse ASCII view.

Usage:
  run/imgcmp.py view <file.ppm> [cols] [rows]      # ASCII overview
  run/imgcmp.py diff <a.ppm> <b.ppm> [tol]          # pixel differences
  run/imgcmp.py stats <file.ppm>                    # colour statistics

The ASCII view maps each block to a character chosen from the block's
average colour, so a screen can be inspected without an image viewer.
"""
import sys

from PIL import Image

CHARS = " .:-=+*#%@"


def load(path):
    return Image.open(path).convert("RGB")


def ascii_view(img, cols=80, rows=40):
    w, h = img.size
    bw, bh = max(1, w // cols), max(1, h // rows)
    px = img.load()
    out = []
    for by in range(0, h, bh):
        line = []
        for bx in range(0, w, bw):
            r = g = b = n = 0
            for y in range(by, min(by + bh, h)):
                for x in range(bx, min(bx + bw, w)):
                    pr, pg, pb = px[x, y]
                    r += pr
                    g += pg
                    b += pb
                    n += 1
            r //= n
            g //= n
            b //= n
            lum = (r * 299 + g * 587 + b * 114) // 1000
            line.append(CHARS[min(len(CHARS) - 1, lum * len(CHARS) // 256)])
        out.append("".join(line))
    return "\n".join(out)


def stats(img):
    w, h = img.size
    px = img.load()
    hist = {}
    total = 0
    sr = sg = sb = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            c = px[x, y]
            hist[c] = hist.get(c, 0) + 1
            sr += c[0]
            sg += c[1]
            sb += c[2]
            total += 1
    top = sorted(hist.items(), key=lambda kv: -kv[1])[:8]
    print(f"size {w}x{h} sampled {total} colours {len(hist)}")
    print(f"mean rgb {(sr // total, sg // total, sb // total)}")
    for c, n in top:
        print(f"  {c} {100.0 * n / total:5.1f}%")


def diff(a_path, b_path, tol=0):
    a, b = load(a_path), load(b_path)
    if a.size != b.size:
        print(f"size mismatch {a.size} vs {b.size}")
        return 1
    w, h = a.size
    pa, pb = a.load(), b.load()
    bad = 0
    maxd = 0
    first = []
    for y in range(h):
        for x in range(w):
            ca, cb = pa[x, y], pb[x, y]
            d = max(abs(ca[i] - cb[i]) for i in range(3))
            if d > maxd:
                maxd = d
            if d > tol:
                bad += 1
                if len(first) < 12:
                    first.append((x, y, ca, cb))
    print(f"{a_path} vs {b_path}: {bad} of {w * h} pixels differ "
          f"({100.0 * bad / (w * h):.3f}%), max channel delta {maxd}")
    for x, y, ca, cb in first:
        print(f"  ({x},{y}) {ca} != {cb}")
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    if cmd == "view":
        img = load(sys.argv[2])
        cols = int(sys.argv[3]) if len(sys.argv) > 3 else 80
        rows = int(sys.argv[4]) if len(sys.argv) > 4 else 40
        print(ascii_view(img, cols, rows))
    elif cmd == "diff":
        tol = int(sys.argv[4]) if len(sys.argv) > 4 else 0
        return diff(sys.argv[2], sys.argv[3], tol)
    elif cmd == "stats":
        stats(load(sys.argv[2]))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
