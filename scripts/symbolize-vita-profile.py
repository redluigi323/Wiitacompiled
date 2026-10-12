#!/usr/bin/env python3
"""Symbolize static65+ HPROF PC/LR buckets using the ELF from the same VPK build."""
import argparse
from collections import defaultdict
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="watchdog.log from the Vita")
    parser.add_argument("--elf", type=Path, default=Path("build-vita/vita/wiicompiled-vita"))
    parser.add_argument("--sdk", type=Path, default=Path("/usr/local/vitasdk"))
    args = parser.parse_args()
    nm = args.sdk / "bin/arm-vita-eabi-nm"
    addr2line = args.sdk / "bin/arm-vita-eabi-addr2line"
    symbols = subprocess.check_output([str(nm), "-nC", str(args.elf)], text=True)
    anchor = None
    for line in symbols.splitlines():
        match = re.match(r"([0-9a-fA-F]+)\s+[tT]\s+mkwv::platform::start_host_profiler\(\)$", line)
        if match:
            anchor = int(match[1], 16) & ~1
            break
    if anchor is None:
        parser.error("ELF has no start_host_profiler symbol; use the matching unstripped ELF")
    delta = None
    profiles = []
    pcs = set()
    for line in args.log.read_text(errors="replace").splitlines():
        match = re.search(r"HPROF anchor start_host_profiler=(?:0x)?([0-9a-fA-F]+)", line)
        if match:
            delta = (int(match[1], 16) & ~1) - anchor
        match = re.search(r"HPROF (\S+) (pc|lr) n=(\d+):(.+)", line)
        if not match:
            continue
        if delta is None:
            parser.error("HPROF buckets precede their relocation anchor; use the complete log")
        tag, kind, total, buckets = match.groups()
        weights = []
        for address, percent in re.findall(r"([0-9a-fA-F]{8}):([0-9.]+)", buckets):
            # The runtime buckets are rounded down to 32 bytes; point into their middle.
            pc = int(address, 16) + 16 - delta
            if pc < 0 or pc > 0xffffffff:
                parser.error("relocated address is outside ARM32; check the ELF/log pairing")
            weights.append((pc, float(percent)))
            pcs.add(pc)
        profiles.append((tag, kind, total, weights))
    if not profiles:
        parser.error("no HPROF buckets in log (profiling disabled or context reads refused)")
    ordered = sorted(pcs)
    resolved = subprocess.check_output(
        [str(addr2line), "-e", str(args.elf), "-f", "-C", *[hex(pc) for pc in ordered]], text=True
    ).splitlines()
    names = {pc: (resolved[i * 2], resolved[i * 2 + 1]) for i, pc in enumerate(ordered)}
    for tag, kind, total, weights in profiles:
        sums = defaultdict(float)
        for pc, weight in weights:
            name, _ = names[pc]
            sums[name] += weight
        print(f"{tag} {kind}, {total} READY samples (top buckets only):")
        for name, weight in sorted(sums.items(), key=lambda item: -item[1])[:20]:
            print(f"  {weight:5.1f}%  {name}")
        print()


if __name__ == "__main__":
    main()
