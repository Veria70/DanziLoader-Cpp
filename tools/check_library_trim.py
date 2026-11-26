# SPDX-License-Identifier: GPL-3.0-or-later
"""Port-only check (M4.17): tests/m4-port/library-trim.txt run with an
emptied pipeline library (it stores and saves; nothing is mapped, nothing
let go), then again (the saved file is mapped and loaded from; past the idle
time the sweep lets its pages go: the trimmed bytes are the file's size),
and again with VICTORY_LIBRARY_TRIM=0 (nothing let go).

Usage: python -I tools/check_library_trim.py <title-driver-cpp.exe> <work>
"""
import os
import re
import shutil
import subprocess
import sys

driver, work = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scenario = os.path.join(here, "tests", "m4-port", "library-trim.txt")
shutil.rmtree("c:/tmp/m4-trim-port", ignore_errors=True)
got = []
for run, env in enumerate([{}, {}, {"VICTORY_LIBRARY_TRIM": "0"}]):
    out = os.path.join(work, f"library-trim-{run}")
    subprocess.run([driver, scenario, out], check=True, env={**os.environ, **env})
    log = open(os.path.join(out, "log.txt"), encoding="utf-8").read()
    got.append(int(re.search(r"trimmed (\d+)", log).group(1)))
size = sum(os.path.getsize(os.path.join("c:/tmp/m4-trim-port", f)) for f in os.listdir("c:/tmp/m4-trim-port"))
print("trimmed bytes (fresh, mapped, switched off):", got, "library files:", size)
ok = got[0] == 0 and got[1] > 0 and got[2] == 0
print("ok" if ok else "FAILED")
sys.exit(0 if ok else 1)
