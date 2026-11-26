# SPDX-License-Identifier: GPL-3.0-or-later
"""M6.6 and on: every runtime scenario (tests/m6/hle/*.txt) run by the
reference's runtime (`m6-oracle hle-run`) and by the port's (`hle-run`),
each in its own process and its own empty working folder: the same
results, buffers, last errors and files.

Usage: python -I tools/check_hle.py <m6-oracle> <hle-run> <scenario dir> <work dir>
"""
import os
import re
import pathlib
import shutil
import subprocess
import sys

oracle, port = str(pathlib.Path(sys.argv[1]).resolve()), str(pathlib.Path(sys.argv[2]).resolve())
scenarios, work = pathlib.Path(sys.argv[3]).resolve(), pathlib.Path(sys.argv[4]).resolve()
work.mkdir(parents=True, exist_ok=True)


def fresh(d):
    """An empty folder for one run: relative paths in a scenario land there."""

    shutil.rmtree('\\\\?\\' + str(d), ignore_errors=True)
    d.mkdir(parents=True)
    return d


env = dict(os.environ)
env['NO_COLOR'] = '1'
for k in ['VICTORY_LOG', 'RUST_LOG']:
    env.pop(k, None)


def log_lines(raw):
    out = []
    for line in raw.replace(b"\r\n", b"\n").split(b"\n"):
        if not line:
            continue
        head, _, rest = line.partition(b" ")
        out.append(rest if head.endswith(b"Z") else line)


    out = [re.sub(rb'used_mb=[0-9]+', b'used_mb=N', l) for l in out]
    return b"".join(l + b"\n" for l in sorted(out))


failed = False
for s in sorted(scenarios.glob('*.txt')):
    ref, cpp = work / f'{s.stem}-ref.txt', work / f'{s.stem}-cpp.txt'
    rr = subprocess.run([oracle, 'hle-run', str(s), str(ref)], cwd=fresh(work / f'{s.stem}-ref.d'), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    rc = subprocess.run([port, str(s), str(cpp)], cwd=fresh(work / f'{s.stem}-cpp.d'), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    la, lb = log_lines(rr.stderr), log_lines(rc.stderr)
    (work / f'{s.stem}-ref.log').write_bytes(la)
    (work / f'{s.stem}-cpp.log').write_bytes(lb)
    a = ref.read_text(encoding='utf-8') if ref.exists() else ''
    b = cpp.read_text(encoding='utf-8') if cpp.exists() else ''
    calls = a.count('  -> ')
    ok = rr.returncode == 0 and rc.returncode == 0 and a == b and calls > 0
    logs = la == lb
    lines = la.count(b"\n")
    print(f'{s.stem}: {calls} calls, {"ok" if ok else "FAILED"}, {lines} log lines {"alike" if logs else "DIFFER"}')
    ok = ok and logs
    failed |= not ok
sys.exit(1 if failed else 0)
