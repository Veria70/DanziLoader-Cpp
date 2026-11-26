# SPDX-License-Identifier: GPL-3.0-or-later
"""The M4.14 bench's sections (tests/m4/bench/m414-memory.txt) from a
`dirty-diff bench` work directory: per section, the median over the runs
(a repeat line's time is the mean per call; per-frame dispatches, the median
of the 100 frames).

Usage: python -I tools/memory_stats.py <work-dir>
"""
import glob
import statistics
import sys


def sections(path):
    rows = [line.split() for line in open(path)]
    rows = [(int(n), name, float(ns)) for n, name, ns in rows]
    first = min(n for n, name, _ in rows if name == "frame_wait" and n > 40)
    d = [r for r in rows if r[1] == "dispatch" and r[0] > first]
    frames = d[:-2]
    return {
        "a frame's first memory dispatch on a list (16 copies)": statistics.median(r[2] for r in frames[0::2]),
        "the next on that list": statistics.median(r[2] for r in frames[1::2]),
        "memory dispatches recorded in one list": d[-2][2],
        "plain dispatches recorded in one list (beside them)": d[-1][2],
    }


def main():
    runs = {side: [sections(f) for f in sorted(glob.glob(f"{sys.argv[1]}/m414-memory-run*-{side}-timing.txt"))] for side in ("rust", "cpp")}
    for name in runs["rust"][0]:
        r = statistics.median(s[name] for s in runs["rust"])
        c = statistics.median(s[name] for s in runs["cpp"])
        print(f"{name:56s} rust {r / 1e3:10.2f} us   c++ {c / 1e3:10.2f} us   {r / c:6.2f}x")


if __name__ == "__main__":
    main()
