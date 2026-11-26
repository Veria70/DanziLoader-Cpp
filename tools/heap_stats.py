# SPDX-License-Identifier: GPL-3.0-or-later
"""The M4.12 bench's sections (tests/m4/bench/m412-heap-index.txt) from a
`dirty-diff bench` work directory, or from two timing files: per section,
the median over the runs (a repeat line's time is the mean per call).

Usage: python -I tools/heap_stats.py <work-dir>
       python -I tools/heap_stats.py <rust timing.txt> <c++ timing.txt>
"""
import glob
import statistics
import sys


def sections(path):
    rows = [line.split() for line in open(path)]
    rows = [(int(n), name, float(ns)) for n, name, ns in rows]
    disp = [r for r in rows if r[1] == "dispatch_cycle"]
    draws = [r for r in rows if r[1] == "draw_cycle"]
    rec = [r for r in rows if r[1] == "dispatch"]
    frames = [r[2] for r in disp[2:]]
    return {
        "first dispatch cycle (every view, 128 textures)": disp[0][2],
        "dispatch cycles later in that frame": disp[1][2],
        "next frame's dispatch cycle": frames[0],
        "dispatch cycles, one a frame (median of 200)": statistics.median(frames),
        "dispatches recorded in one list": rec[-1][2],
        "first draw cycle of a frame (median of 100)": statistics.median(r[2] for r in draws[:-1]),
        "draw cycles later in a frame": draws[-1][2],
    }


def main():
    if len(sys.argv) == 3:
        runs = {"rust": [sections(sys.argv[1])], "cpp": [sections(sys.argv[2])]}
    else:
        runs = {side: [sections(f) for f in sorted(glob.glob(f"{sys.argv[1]}/m412-heap-index-run*-{side}-timing.txt"))]
                for side in ("rust", "cpp")}
    for name in runs["rust"][0]:
        r = statistics.median(s[name] for s in runs["rust"])
        c = statistics.median(s[name] for s in runs["cpp"])
        print(f"{name:50s} rust {r / 1e3:10.1f} us   c++ {c / 1e3:10.1f} us   {r / c:6.2f}x")


if __name__ == "__main__":
    main()
