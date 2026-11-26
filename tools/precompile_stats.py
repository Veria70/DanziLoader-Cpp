# SPDX-License-Identifier: GPL-3.0-or-later
"""The M4.16 bench's sections (tests/m4/bench/m416-precompile.txt) from a
`dirty-diff bench` work directory: median over the runs of each run's
median (the 20 shaders) or the repeat line's mean.

Usage: python -I tools/precompile_stats.py <work-dir>
"""
import glob
import statistics
import sys


def sections(path):
    rows = [line.split() for line in open(path)]
    pc = [float(r[2]) for r in rows if r[1] == "precompile_compute"]
    p = [float(r[2]) for r in rows if r[1] == "precompile"]
    return {
        "prepare from the shader cache (20 shaders)": statistics.median(p[:20]),
        "compile into an empty library (20 shaders)": statistics.median(pc[:20]),
        "load from the library (20 shaders)": statistics.median(pc[20:40]),
        "prepare again (repeat)": p[-1],
        "load again (repeat)": pc[-1],
    }


def main():
    runs = {side: [sections(f) for f in sorted(glob.glob(f"{sys.argv[1]}/m416-precompile-run*-{side}-timing.txt"))] for side in ("rust", "cpp")}
    for name in runs["rust"][0]:
        r = statistics.median(s[name] for s in runs["rust"])
        c = statistics.median(s[name] for s in runs["cpp"])
        print(f"{name:46s} rust {r / 1e3:10.2f} us   c++ {c / 1e3:10.2f} us   {r / c:6.2f}x")


if __name__ == "__main__":
    main()
