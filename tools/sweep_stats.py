# SPDX-License-Identifier: GPL-3.0-or-later
"""The sweep bench's submissions (tests/m4/bench/m411-sweep.txt) from a
`dirty-diff bench` work directory: the submission that sweeps, the 200
cycles after it, and the 200 cycles a second later (median over the runs;
a repeat line's time is the mean per call).

Usage: python -I tools/sweep_stats.py <work-dir>
"""
import glob
import statistics
import sys

NAMES = [
    "submission that sweeps (500 textures let go)",
    "200 cycles after it, per cycle",
    "200 cycles 1 s later, per cycle",
]


def main():
    work = sys.argv[1]
    res = {}
    for side in ("rust", "cpp"):
        cols = {}
        for f in sorted(glob.glob(f"{work}/m411-sweep-run*-{side}-timing.txt")):
            rows = [line.split() for line in open(f)]
            rows = [float(ns) for _, name, ns in rows if name == "submit_cycle"]
            for i, ns in enumerate(rows):
                cols.setdefault(i, []).append(ns)
        res[side] = {i: statistics.median(v) for i, v in cols.items()}
    for i, name in enumerate(NAMES):
        if i not in res["rust"] or i not in res["cpp"]:
            continue
        r, c = res["rust"][i], res["cpp"][i]
        print(f"{name:48s} rust {r / 1e3:10.1f} us   c++ {c / 1e3:10.1f} us   {r / c:7.2f}x")


if __name__ == "__main__":
    main()
