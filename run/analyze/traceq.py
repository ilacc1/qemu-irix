#!/usr/bin/env python3
"""Query the sgi_gio64 probe logs.

One maintained analyzer for all the REX3 trace questions, so we stop
writing throwaway scripts. The logs are line oriented: a tag then
key=value fields, e.g.

  SEQ  n=.. m0=000c9102 m1=.. ...
  SEQW n=.. len=.. w0=.. w1=..
  SWR  n=.. off=00110 val=.. size=8 go=0
  DCBW slave=2 reg=1 msw=.. lsw=.. size=4
  XMAPMODE h=0 idx=13 val=da0900
  LUT  i=00 r=.. g=.. b=..
  DIDROW y=.. did=.. tbl=.. pm=.. ps=.. aux=.. main=.. auxw=..

Commands:
  tags                       count every tag
  hist TAG FIELD [--top N]   value histogram for TAG's FIELD
  filter TAG [FIELD=VAL..]   print matching lines (substring on values)
  sum TAG FIELD              numeric sum
  seq OFFSET                 SEQ/SEQW/SWR lines whose n == OFFSET
  count TAG [FIELD=VAL..]    count matches
  bbox [MODE0]               PRIMLOG coverage: per primitive mode0, sum of
                             intended bbox area vs primitives that stored 0
                             pixels (ZEROSTORE) and short-fill ratios

Reads a log path, or "-" for stdin. Work on the *tail* window of a big
log by passing --from/--to line numbers, or --tail N.
"""
import argparse
import sys
from collections import Counter


def iter_lines(path, frm, to, tail):
    if tail:
        from collections import deque
        with open(path, "r", errors="replace") as f:
            dq = deque(f, maxlen=tail)
        for line in dq:
            yield line
        return
    with open(path, "r", errors="replace") as f:
        for i, line in enumerate(f, 1):
            if frm and i < frm:
                continue
            if to and i > to:
                break
            yield line


def parse(line):
    parts = line.split()
    if not parts:
        return None, {}
    tag = parts[0]
    fields = {}
    for p in parts[1:]:
        if "=" in p:
            k, _, v = p.partition("=")
            fields[k] = v
    return tag, fields


def match(fields, conds):
    for c in conds:
        k, _, v = c.partition("=")
        if k not in fields or fields[k] != v:
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("cmd")
    ap.add_argument("tag", nargs="?")
    ap.add_argument("extra", nargs="*")
    ap.add_argument("--field")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--from", dest="frm", type=int)
    ap.add_argument("--to", type=int)
    ap.add_argument("--tail", type=int)
    ap.add_argument("--n", type=int, default=40)
    args = ap.parse_args()

    src = sys.stdin if args.log == "-" else iter_lines(
        args.log, args.frm, args.to, args.tail)

    if args.cmd == "tags":
        c = Counter()
        for line in src:
            t, _ = parse(line)
            if t:
                c[t] += 1
        for t, n in c.most_common(args.top):
            print(f"{n:>12} {t}")
        return

    if args.cmd == "filter":
        n = 0
        for line in src:
            t, f = parse(line)
            if t != args.tag:
                continue
            if match(f, args.extra):
                print(line.rstrip())
                n += 1
                if n >= args.n:
                    break
        return

    if args.cmd == "count":
        n = 0
        for line in src:
            t, f = parse(line)
            if t == args.tag and match(f, args.extra):
                n += 1
        print(n)
        return

    if args.cmd == "sum":
        s = 0
        for line in src:
            t, f = parse(line)
            if t == args.tag and match(f, args.extra):
                try:
                    s += int(f.get(args.field, "0"), 0)
                except ValueError:
                    pass
        print(s)
        return

    if args.cmd == "hist":
        field = args.field or (args.extra[0] if args.extra else None)
        conds = args.extra[1:] if not args.field else args.extra
        if field is None:
            print("hist needs a field", file=sys.stderr)
            sys.exit(2)
        c = Counter()
        for line in src:
            t, f = parse(line)
            if t == args.tag and match(f, conds):
                c[f.get(field, "?")] += 1
        for v, n in c.most_common(args.top):
            print(f"{n:>10} {v}")
        return

    if args.cmd == "bbox":
        want = args.tag  # optional mode0 filter (hex without 0x ok)
        seq_by_m0 = Counter()
        area_by_m0 = {}
        zerostore_by_m0 = Counter()
        zerostore_lines = []
        for line in src:
            t, f = parse(line)
            if t == "SEQ":
                m0 = f.get("m0", "?")
                if want and m0 != want:
                    continue
                seq_by_m0[m0] += 1
                try:
                    x0, y0 = int(f["xs"]), int(f["ys"])
                    x1, y1 = int(f["xe"]), int(f["ye"])
                except (KeyError, ValueError):
                    continue
                area = abs(x1 - x0) * abs(y1 - y0)
                area_by_m0[m0] = area_by_m0.get(m0, 0) + area
            elif t == "ZEROSTORE":
                # ZEROSTORE has flags, not m0; join via the op/plane proxy.
                zerostore_by_m0[f.get("plane", "?")] += 1
                zerostore_lines.append((f.get("op"), f.get("bbox", "?")))
        print("m0         prims    sum_bbox_area")
        for m0, n in seq_by_m0.most_common(args.top):
            print(f"{m0}  {n:>7}    {area_by_m0.get(m0, 0):>12}")
        if zerostore_by_m0:
            print("\nZEROSTORE (intended bbox, 0 pixels stored) by plane:")
            for pl, n in zerostore_by_m0.most_common():
                print(f"  plane={pl}  n={n}")
        return

    if args.cmd == "seq":
        want = args.tag
        out = 0
        for line in src:
            t, f = parse(line)
            if t in ("SEQ", "SEQW", "SWR", "SMW") and f.get("n") == want:
                print(line.rstrip())
                out += 1
                if out >= args.n:
                    break
        return

    print("unknown command", file=sys.stderr)
    sys.exit(2)


if __name__ == "__main__":
    main()
