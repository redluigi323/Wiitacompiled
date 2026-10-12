#!/usr/bin/env python3
"""Rank the translated shards by sampled guest time, to maintain vita/cmake/shard_hot_list.cmake.

Reads one or more watchdog.log files. Uses the guest profiler's PROF self/lr lines (guest
addresses) and, when given symbolize-vita-profile.py output (--symbolized), any func_XXXXXXXX
lines with a percentage. Each address goes to the shard whose first function is the closest
one at or below it (shards are contiguous guest address ranges).

  python3 scripts/rank-hot-shards.py watchdog.log [more.log ...] [--symbolized sym.txt] [--cover 98]
"""
import argparse
import bisect
import collections
import re
from pathlib import Path


def shard_starts(root):
    starts = []
    for path in sorted(root.glob("shard_*.cpp")):
        with path.open(errors="replace") as f:
            for line in f:
                m = re.search(r'"func_([0-9A-F]{8})\.cpp"', line)
                if m:
                    starts.append((int(m[1], 16), path.name))
                    break
    starts.sort()
    return starts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", type=Path)
    ap.add_argument("--symbolized", type=Path, action="append", default=[])
    ap.add_argument("--shards", type=Path, default=Path("upstream/generated/build_shards/base_common"))
    ap.add_argument("--cover", type=float, default=98.0, help="print the cmake list covering this %% of samples")
    args = ap.parse_args()
    starts = shard_starts(args.shards)
    keys = [s[0] for s in starts]

    def shard_of(addr):
        i = bisect.bisect_right(keys, addr) - 1
        return starts[i][1] if i >= 0 else None

    heat = collections.Counter()
    for log in args.logs:
        for line in log.read_text(errors="replace").splitlines():
            m = re.search(r"PROF (?:snd )?(?:self|lr) (.*)", line)
            if not m:
                continue
            for a, p in re.findall(r"([0-9A-F]{8}):([\d.]+)", m[1]):
                s = shard_of(int(a, 16))
                if s:
                    heat[s] += float(p)
    for sym in args.symbolized:
        for line in sym.read_text(errors="replace").splitlines():
            m = re.search(r"func_([0-9A-Fa-f]{8}).*?([\d.]+)\s*%", line)
            if m:
                s = shard_of(int(m[1], 16))
                if s:
                    heat[s] += float(m[2])
    total = sum(heat.values()) or 1.0
    cum = 0.0
    chosen = []
    for s, h in heat.most_common():
        cum += h
        print(f"{100 * h / total:6.2f}%  cum {100 * cum / total:6.2f}%  {s}")
        if 100 * (cum - h) / total < args.cover:
            chosen.append(s)
    print("\nset(MKWV_HOT_SHARDS")
    for s in chosen:
        print(f"  {s}")
    print(")")


if __name__ == "__main__":
    main()
