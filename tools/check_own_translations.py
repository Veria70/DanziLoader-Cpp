# SPDX-License-Identifier: GPL-3.0-or-later
"""M5.10: milestone 4's scenarios with the port's own translations. Each
scenario that keeps translated shaders (`shader_cache`) runs with a fresh
cache on each side: the reference translates with its translator, the port
with its own (no build file: the port keys its entries with its own
fingerprint, so nothing the reference made is read). The logs must match,
then match again with the port's cache warm (every shader read back from
the entries the port wrote). The port must have written as many entries
as the reference.

Usage: python -I tools/check_own_translations.py <title-driver-ref> <title-driver-cpp> <scenario dir> <work dir>
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys

CACHE_LINE = re.compile(r"^shader_cache\s+\S+(\s+\S+)?\s*$", re.M)


def entries(cache):
    return sorted(p.relative_to(cache).as_posix() for p in cache.rglob("*")
                  if p.is_file() and len(p.parent.name) == 2 and not p.name.startswith(".tmp"))


def run(exe, scenario, out):
    shutil.rmtree(out, ignore_errors=True)
    r = subprocess.run([exe, str(scenario), str(out)], env=dict(os.environ),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = out / "log.txt"

    text = log.read_text() if log.exists() else ""
    return r.returncode, re.sub(r"^(#\d+ shader_cache) .*$", r"\1", text, flags=re.M)


def main():
    ref, cpp, scenarios, work = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]), pathlib.Path(sys.argv[4])
    work.mkdir(parents=True, exist_ok=True)
    failed = False
    ran = 0
    for scenario in sorted(scenarios.glob("*.txt")):
        text = scenario.read_text()
        if not CACHE_LINE.search(text):
            continue
        ran += 1
        name = scenario.stem
        ref_cache, cpp_cache = work / f"{name}-ref-cache", work / f"{name}-cpp-cache"
        for c in (ref_cache, cpp_cache):
            shutil.rmtree(c, ignore_errors=True)
        ref_text = CACHE_LINE.sub(f"shader_cache {ref_cache.as_posix()} {ref_cache.as_posix()}/build.bin", text)
        cpp_text = CACHE_LINE.sub(f"shader_cache {cpp_cache.as_posix()}", text)
        ref_s, cpp_s = work / f"{name}-ref.txt", work / f"{name}-cpp.txt"
        ref_s.write_text(ref_text)
        cpp_s.write_text(cpp_text)
        rc_r, log_r = run(ref, ref_s, work / f"{name}-ref")
        rc_c, log_c = run(cpp, cpp_s, work / f"{name}-cpp")
        rc_w, log_w = run(cpp, cpp_s, work / f"{name}-cpp-warm")
        n_ref = len([e for e in entries(ref_cache) if not e.endswith("build.bin")])
        n_cpp = len(entries(cpp_cache))
        problems = []
        if rc_r != rc_c:
            problems.append(f"exit {rc_r} against {rc_c}")
        if log_r != log_c:
            problems.append("the logs differ (cold)")
        if log_r != log_w:
            problems.append("the logs differ (warm)")
        if n_ref != n_cpp:
            problems.append(f"{n_ref} entries against {n_cpp}")
        if not log_r:
            problems.append("no log")
        print(f"{name}: {n_cpp} shaders translated by the port, "
              f"{'ok' if not problems else 'FAILED: ' + ', '.join(problems)}")
        failed |= bool(problems)
    if ran == 0:
        print("no scenario keeps translated shaders")
        failed = True
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
