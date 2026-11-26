# SPDX-License-Identifier: GPL-3.0-or-later
"""M4.14 checks for tests/m4/19-memory.txt, the reference and the port
logging alike in each mode:

- a load 16 MiB past the named address reads zeros in its first frame (the
  page has no copy; the shader notes it) and the title's bytes in the next
  (the noted page was given its copy);
- the structure builder's kind of program (a spilled user-data table) is
  dropped by default and runs with `VICTORY_BVH_BUILDER=1`.

Usage: python -I tools/check_memory.py <title-driver-ref> <title-driver-cpp> <scenario> <work dir>
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
    failed = False
    for mode, env, checks in [
        ("default", {}, [(256, True), (512, False), (2048, True)]),
        ("builder", {"VICTORY_BVH_BUILDER": "1"}, [(2048, False)]),
    ]:
        r = run(ref, scenario, work / f"{mode}-ref", env)
        c = run(cpp, scenario, work / f"{mode}-cpp", env)
        if r != c:
            print(f"{mode}: the logs differ")
            failed = True
        for address, zeros in checks:
            got = peek(c, address)
            ok = (got == ZEROS) == zeros
            print(f"{mode}: out+{address} {got} {'ok' if ok else 'WRONG'}")
            failed |= not ok
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
