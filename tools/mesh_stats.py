# SPDX-License-Identifier: GPL-3.0-or-later
"""The M4.13 bench's sections (tests/m4/bench/m413-mesh.txt) from a
`dirty-diff bench` work directory: per section, the median over the runs (a
repeat line's time is the mean per call; the first dispatches of the 20 new
pipelines, their median).

Usage: python -I tools/mesh_stats.py <work-dir>
"""
import glob
import statistics
import sys


def sections(path):
    rows = [line.split() for line in open(path)]
    rows = [(int(n), name, float(ns)) for n, name, ns in rows]
    quiet = min(n for n, name, _ in rows if name == "mesh_cycle")
    rows = [r for r in rows if r[0] >= quiet]
    cycles = [r[2] for r in rows if r[1] == "mesh_cycle"]
    meshes = [r[2] for r in rows if r[1] == "dispatch_mesh"]
    return {
        "mesh cycle, 1 dispatch": statistics.median([cycles[0], cycles[2]]),
        "mesh cycle, 64 dispatches": statistics.median([cycles[1], cycles[3]]),
        "draw cycle, 1 draw (beside them)": next(r[2] for r in rows if r[1] == "draw_cycle"),
        "mesh dispatch recorded": meshes[0],
        "indirect mesh dispatch recorded": next(r[2] for r in rows if r[1] == "execute_indirect"),
        "the title makes a mesh pipeline": next(r[2] for r in rows if r[1] == "graphics_pipeline"),
        "first dispatch with a new mesh pipeline (median of 20)": statistics.median(meshes[1:]),
    }


def main():
    runs = {side: [sections(f) for f in sorted(glob.glob(f"{sys.argv[1]}/m413-mesh-run*-{side}-timing.txt"))] for side in ("rust", "cpp")}
    for name in runs["rust"][0]:
        r = statistics.median(s[name] for s in runs["rust"])
        c = statistics.median(s[name] for s in runs["cpp"])
        unit, div = ("ms", 1e6) if r > 1e6 else ("us", 1e3)
        print(f"{name:56s} rust {r / div:10.2f} {unit}   c++ {c / div:10.2f} {unit}   {r / c:6.2f}x")


if __name__ == "__main__":
    main()
