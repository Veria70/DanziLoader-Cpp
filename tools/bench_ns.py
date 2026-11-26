# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-call medians in nanoseconds from a `dirty-diff bench` work directory.

`dirty-diff bench` prints microseconds per operation, too coarse for calls
timed with `repeat <n>` (a few ns each). This reads the runs' timing files
(<work>/<fixture>-run<n>-{rust,cpp}-timing.txt), keeps the operations after
the fixture's first `quiet` line, and prints the median per operation name.

Usage: python -I tools/bench_ns.py <fixture.txt> <work-dir> [<out.txt>]
"""
import pathlib
import statistics
import sys


def quiet_op(fixture):
    n = 0
    for line in fixture.read_text(encoding="utf-8").splitlines():
        t = line.split("#", 1)[0].strip()
        if not t:
            continue
        if t.split()[0] == "quiet":
            return n
        n += 1
    return 0


def main():
    fixture = pathlib.Path(sys.argv[1])
    work = pathlib.Path(sys.argv[2])
    first = quiet_op(fixture)
    times = {"oracle": {}, "cpp": {}}

    runs = sorted(work.glob(f"{fixture.stem}-run*-rust-timing.txt"))
    for run in runs:
        for side in times:
            path = run if side == "oracle" else run.with_name(run.name.replace("-rust-", "-cpp-"))
            for line in path.read_text().splitlines():
                n, name, ns = line.split()
                if int(n) > first:
                    times[side].setdefault(name, []).append(float(ns))
    out = [f"{fixture.stem}: per call, median over {len(runs)} runs (ns)"]
    for name in sorted(times["oracle"]):
        r = statistics.median(times["oracle"][name])
        c = statistics.median(times["cpp"].get(name, [float("nan")]))
        out.append(f"{name:<20} rust {r:12.2f} ns  c++ {c:12.2f} ns  {r / c if c else float('inf'):7.2f}x")
    text = "\n".join(out) + "\n"
    print(text, end="")
    if len(sys.argv) > 3:
        pathlib.Path(sys.argv[3]).write_text(text, encoding="utf-8")


main()
