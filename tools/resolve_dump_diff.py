#!/usr/bin/env python3
"""Compare resolve dumps of two runs (exactness harness for scaled resolves).

Run the same save twice with FABLE2_DIAG_RESOLVE_DUMP=<dir> and FABLE2_DIAG_RESOLVE_RANGE=<lo>-<hi> (hex guest
addresses), once at 1x and once at 2x/3x. Each resolve into the range is written as r<NNNN>-<ADDR>-<LEN>.bin: at 1x
the bytes the resolve wrote, at scale > 1 the scaled result downscaled to 1x (centre sample, XeniOS resolve_downscale;
only whole 32x32-tile lengths). Resolves are matched by address and order.

A scaled downscale is not bit-exact against 1x (the game renders from scaled sources), so the check is statistical:
mean absolute byte difference per resolve, over its 32x32-texel tiles. A resolve that is all zero at scale but not at
1x, or whose difference is far above the others, is lost data (docs/lake-water-20261014.md: 4 of 10 lake heightmaps
were zero at 2x). A single all-zero tile inside an otherwise matching resolve is usually a hole in the written extent
(tiled addressing leaves bytes the resolve does not write; at 1x they keep the old guest data): reported as "hole?",
left out of the difference, not a failure.

  python3 tools/resolve_dump_diff.py <dir 1x> <dir 2x> [--max-mad 8]
Exit status 1 when any matched resolve fails.
"""
import argparse
import os
import sys


def index(folder):
    out = {}
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".bin"):
            continue
        parts = name[:-4].split("-")
        if len(parts) != 3:
            continue
        with open(os.path.join(folder, name), "rb") as f:
            out.setdefault(parts[1], []).append(f.read())
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("reference", help="1x dump folder")
    ap.add_argument("scaled", help="2x/3x dump folder")
    ap.add_argument("--max-mad", type=float, default=8.0,
                    help="mean absolute byte difference that fails a resolve (default 8)")
    args = ap.parse_args()
    ref, scl = index(args.reference), index(args.scaled)
    failed = matched = skipped = 0
    for addr in sorted(ref):
        for i, a in enumerate(ref[addr]):
            others = scl.get(addr, [])
            if i >= len(others):
                skipped += 1
                continue
            b = others[i]
            n = min(len(a), len(b))
            if not n:
                continue
            matched += 1
            zero_lost = not any(b[:n]) and any(a[:n])
            # Per tile: the tile size follows the bytes per texel, which the dump name does not carry, so use 1 KB
            # (an 8bpp 32x32 tile); larger formats just split their tiles in parts.
            tile = 1024
            holes, diff, count = [], 0, 0
            for t in range(0, n, tile):
                x, y = a[t:t + tile], b[t:t + tile]
                if not zero_lost and not any(y) and any(x):
                    holes.append(t // tile)
                    continue
                diff += sum(abs(p - q) for p, q in zip(x, y))
                count += len(x)
            mad = diff / count if count else 0.0
            bad = zero_lost or mad > args.max_mad
            failed += bad
            print("%s #%d %5d bytes  1x mean %6.1f  scaled mean %6.1f  mad %6.2f%s%s" % (
                addr, i, n, sum(a[:n]) / n, sum(b[:n]) / n, mad,
                "  hole? tiles %s" % holes if holes else "",
                "  FAIL (all zero at scale)" if zero_lost else ("  FAIL" if bad else "")))
    print("matched %d, failed %d, unmatched %d (scaled dumps need whole-tile lengths)" % (matched, failed, skipped))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
