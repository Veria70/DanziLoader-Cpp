# SPDX-License-Identifier: GPL-3.0-or-later
"""M6.4: the synthetic title (tests/m6/fixture) run by the reference's
loader (`m6-oracle run`) and by the port's (`loader-run`), each in its own
process with its own cache folder: the binding reports, every line the
title logs (TLS callbacks, DllMain notices, its thread, static TLS values,
imports by name and by ordinal, an unimplemented import) and the notices
at process exit must be the same. Then with unimplemented imports
trapping: the same log up to the trap, and the same crash log (but for its
first line, the time and process id).

Usage: python -I tools/check_loader.py <m6-oracle> <loader-run> <fixture dir> <work dir>
"""
import pathlib
import shutil
import subprocess
import sys

oracle, port, fixture, work = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4])
shutil.rmtree(work, ignore_errors=True)
work.mkdir(parents=True)
failed = False
for mode in ('', 'trap', 'crash1', 'crash2'):
    outs = {}
    for side, exe, cmd in (('ref', oracle, ['run']), ('cpp', port, [])):
        out = work / f'{side}{"-" + mode if mode else ""}.txt'
        args = [exe] + cmd + [fixture, 'fixture_title.exe', str(work / f'cache-{side}'), str(out)] + ([mode] if mode else [])
        r = subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        crash = out.parent / (out.name + '.crash')
        lines = crash.read_text(encoding='utf-8').splitlines()[1:] if mode and crash.exists() else []
        if mode.startswith('crash'):


            lines = [l for l in lines if l.startswith('VIctory:') or 'victory-image-' in l]
        outs[side] = (r.returncode, out.read_text(encoding='utf-8'), '\n'.join(lines))
    name = mode or 'run'
    (rc_r, log_r, crash_r), (rc_c, log_c, crash_c) = outs['ref'], outs['cpp']
    problems = []
    if (rc_r == 0) != (rc_c == 0):
        problems.append(f'exit {rc_r} against {rc_c}')
    if log_r != log_c:
        problems.append('the logs differ')
    if crash_r != crash_c:
        problems.append('the crash logs differ')
    if 'exit 42' not in log_r and not mode:
        problems.append('the title did not finish')
    if mode.startswith('crash') and 'VIctory: unhandled exception' not in crash_r:
        problems.append('no report in the crash log')
    if mode == 'trap' and 'unimplemented import fixture_host.dll!Unimplemented called' not in crash_r:
        problems.append('no trap in the crash log')
    print(f'{name}: {len(log_r.splitlines())} lines, {"ok" if not problems else "FAILED: " + ", ".join(problems)}')
    failed |= bool(problems)
sys.exit(1 if failed else 0)
