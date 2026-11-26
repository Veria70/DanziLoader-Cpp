# SPDX-License-Identifier: GPL-3.0-or-later
"""M4.12 controls for tests/m4/17-heap-index.txt: with the texture pass off
(`VICTORY_HEAP_TEXTURES=0`) or the early pass off
(`VICTORY_EARLY_TEXTURES_MS=0`), the reference and the port still log alike,
and the texture the pass would have uploaded reads as zeros, which shows
that the pass (not something else) made it there. With both on, it does
not read as zeros.

Usage: python -I tools/check_heap_index.py <title-driver-ref> <title-driver-cpp> <scenario> <work dir>
"""
import os
import pathlib
import shutil
import subprocess
import sys

ZEROS = "00000000000000000000000000000000"


def peek(log, address):
    lines = log.splitlines()
    for i, line in enumerate(lines):
        if line.endswith(f"peek out {address} 16"):
            return lines[i + 1].split()[-1]
    raise SystemExit(f"no peek of out+{address}")


def run(exe, scenario, out, env):
    shutil.rmtree(out, ignore_errors=True)
    subprocess.run([exe, scenario, str(out)], env={**os.environ, **env}, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return (out / "log.txt").read_text()


def main():
    ref, cpp, scenario, work = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4])

    cases = [
        ("on", {}, 512, False),
        ("on", {}, 1536, False),
        ("textures-off", {"VICTORY_HEAP_TEXTURES": "0"}, 512, True),
        ("early-off", {"VICTORY_EARLY_TEXTURES_MS": "0"}, 1536, True),
    ]
    logs = {}
    failed = False
    for mode, env, address, zeros in cases:
        if mode not in logs:
            r = run(ref, scenario, work / f"{mode}-ref", env)
            c = run(cpp, scenario, work / f"{mode}-cpp", env)
            if r != c:
                print(f"{mode}: the logs differ")
                failed = True
            logs[mode] = c
        got = peek(logs[mode], address)
        ok = (got == ZEROS) == zeros
        print(f"{mode}: out+{address} {got} {'ok' if ok else 'WRONG'}")
        failed |= not ok
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
