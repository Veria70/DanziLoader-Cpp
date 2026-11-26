# SPDX-License-Identifier: GPL-3.0-or-later
"""Milestone 7: the shader commands of `victory` (`shader-info`, `translate`,
`precompile`) against the reference's CLI, byte for byte.

    python -I tools/check_shader_cli.py <reference victory.exe> <port victory.exe>
        <work dir> <small dir> [--corpus DIR] [--sgd5 FILE] [--gpu] [--full]
        [--game DIR (read only: shaders.rpf, game_scarlett_master.exe)]
        [--only NAME,...]

Each case runs both executables with `NO_COLOR=1`, no config file (the work
dir is the current directory) and, per side, fresh `VICTORY_CACHE_DIR` /
`VICTORY_DATA_DIR` folders under the work dir. Compared: stdout, stderr (log
lines' leading timestamps stripped) and the exit code; for `precompile`
also the cache files each side wrote (relative name, size, SHA-256).
Lines that carry timings (`precompile`'s `shaders ... (12.3 s)` and its
progress lines) are compared with the seconds masked; such lines are listed
as "timing only". `--gpu` adds `precompile --pipelines` runs (sequential,
short); `--full` adds `translate` over the whole corpus (minutes), timed.

The corpus and game files are only read; everything written goes to the
work dir.
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

TIMESTAMP = re.compile(rb'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d+)?Z\s+', re.M)

TIMED = [
    re.compile(rb'^(shaders        .* failed \()[0-9.]+( s\))$', re.M),
    re.compile(rb'^(  \d+ shaders: .*\()[0-9]+( s\))$', re.M),
]


def args():
    a = sys.argv[1:]
    opts = {'--game': None, '--corpus': None, '--sgd5': None, '--gpu': False, '--full': False, '--only': None}
    pos = []
    i = 0
    while i < len(a):
        if a[i] in ('--gpu', '--full'):
            opts[a[i]] = True
        elif a[i] in opts:
            opts[a[i]] = a[i + 1]
            i += 1
        else:
            pos.append(a[i])
        i += 1
    if len(pos) != 4:
        sys.exit(__doc__)
    return [Path(p) for p in pos], opts


def mask(data):
    timed = []
    for pat in TIMED:
        for m in pat.finditer(data):
            timed.append(m.group(0).decode('utf-8', 'replace'))
        data = pat.sub(rb'\1#\2', data)
    return data, timed


def tree(root):
    out = {}
    if not root.exists():
        return out
    for p in sorted(root.rglob('*')):
        if p.is_file():
            b = p.read_bytes()
            out[p.relative_to(root).as_posix()] = (len(b), hashlib.sha256(b).hexdigest())
    return out


def run(exe, argv, env_extra, side_dir, cwd):
    env = dict(os.environ)
    for k in list(env):
        if k.upper().startswith('VICTORY_') or k.upper() == 'RUST_LOG':
            del env[k]
    env['NO_COLOR'] = '1'
    env['VICTORY_CACHE_DIR'] = str(side_dir / 'cache')
    env['VICTORY_DATA_DIR'] = str(side_dir / 'data')
    env.update(env_extra)
    t = time.perf_counter()
    r = subprocess.run([str(exe)] + argv, cwd=str(cwd), env=env, capture_output=True)
    secs = time.perf_counter() - t

    out, err = r.stdout, r.stderr
    for form in {str(side_dir), str(side_dir).replace('/', '\\')}:
        out = out.replace(form.encode(), b'<side>')
        err = err.replace(form.encode(), b'<side>')
    return r.returncode, out, TIMESTAMP.sub(b'', err), secs


def first_diff(a, b):
    al, bl = a.split(b'\n'), b.split(b'\n')
    for i in range(max(len(al), len(bl))):
        x = al[i] if i < len(al) else b'<none>'
        y = bl[i] if i < len(bl) else b'<none>'
        if x != y:
            return i + 1, x.decode('utf-8', 'replace'), y.decode('utf-8', 'replace')
    return None


def main():
    (ref, port, work, small), o = args()
    work.mkdir(parents=True, exist_ok=True)
    nowhere = work / 'empty-dir'
    nowhere.mkdir(exist_ok=True)
    not_shader = work / 'not-a-shader.txt'
    not_shader.write_bytes(b'hello, no shaders in here\n' * 100)
    S = str(small)
    cases = [
        ('shader-info', ['shader-info', S], {}),
        ('shader-info-verbose', ['shader-info', S, '--verbose'], {}),
        ('shader-info-disasm', ['shader-info', S, '--disasm'], {}),
        ('translate', ['translate', S], {}),
        ('translate-list', ['translate', S, '--list'], {}),
        ('translate-list-jobs1', ['translate', S, '--list', '--jobs', '1'], {}),
        ('translate-validate', ['translate', S, '--validate', '--list', '--jobs', '1'], {}),
        ('translate-wave32', ['translate', S, '--jobs', '1'], {'VICTORY_HOST_WAVE': '32', 'VICTORY_VIEW_LIMIT': '999999'}),
        ('translate-jobs0', ['translate', S, '--jobs', '0'], {}),
        ('precompile-limit50', ['precompile', S, '--limit', '50', '--jobs', '1'], {}),
        ('precompile', ['precompile', S], {}),
        ('err-no-paths', ['shader-info'], {}),
        ('err-no-paths-translate', ['translate'], {}),
        ('err-no-paths-precompile', ['precompile'], {}),
        ('err-missing-path', ['shader-info', str(work / 'does-not-exist')], {}),
        ('err-missing-path-translate', ['translate', str(work / 'does-not-exist')], {}),
        ('err-empty-folder', ['shader-info', str(nowhere)], {}),
        ('err-empty-folder-translate', ['translate', str(nowhere)], {}),
        ('err-empty-folder-precompile', ['precompile', str(nowhere)], {}),
        ('err-not-shader', ['shader-info', str(not_shader), '--verbose'], {}),
        ('err-not-shader-translate', ['translate', str(not_shader), '--list'], {}),
        ('err-bad-jobs', ['translate', S, '--jobs', 'x'], {}),
        ('err-jobs-no-value', ['translate', S, '--jobs'], {}),
        ('err-bad-only', ['precompile', S, '--only', 'gs'], {}),
        ('err-bad-limit', ['precompile', S, '--limit', '99999999999999999999999'], {}),
    ]
    for st in ('vs', 'ps', 'ms', 'cs'):
        cases.append((f'precompile-only-{st}', ['precompile', S, '--only', st, '--limit', '50'], {}))
    if o['--sgd5']:
        g = o['--sgd5']
        cases += [
            ('sgd5-shader-info', ['shader-info', g, '--verbose'], {}),
            ('sgd5-shader-info-disasm', ['shader-info', g, '--disasm'], {}),
            ('sgd5-translate-list', ['translate', g, '--list', '--jobs', '1'], {}),
            ('sgd5-translate', ['translate', g], {}),
            ('sgd5-precompile', ['precompile', g, '--jobs', '1'], {}),
        ]
    if o['--gpu']:
        cases += [
            ('precompile-pipelines', ['precompile', S, '--pipelines', '--only', 'cs', '--limit', '20', '--jobs', '1'], {}),
            ('translate-pipelines', ['translate', S, '--pipelines', '--list', '--jobs', '1'], {}),
        ]
    if o['--game']:
        g = o['--game']
        rpf, exe = str(Path(g) / 'shaders.rpf'), str(Path(g) / 'game_scarlett_master.exe')
        cases += [
            ('game-rpf-shader-info', ['shader-info', rpf], {}),
            ('game-rpf-shader-info-verbose', ['shader-info', rpf, '--verbose'], {}),
            ('game-exe-shader-info-verbose', ['shader-info', exe, '--verbose'], {}),
            ('game-exe-translate-list', ['translate', exe, '--list'], {}),
            ('game-rpf-translate', ['translate', rpf], {}),
        ]
    if o['--full'] and o['--corpus']:
        cases.append(('full-translate', ['translate', o['--corpus']], {}))
    if o['--only']:
        keep = set(o['--only'].split(','))
        cases = [c for c in cases if c[0] in keep]

    failures = 0
    for name, argv, env in cases:
        d = work / name
        if d.exists():
            shutil.rmtree(d)
        sides = {}
        for side, exe in (('ref', ref), ('port', port)):
            sd = d / side
            sd.mkdir(parents=True)
            code, out, err, secs = run(exe, argv, env, sd, work)
            (sd / 'stdout.txt').write_bytes(out)
            (sd / 'stderr.txt').write_bytes(err)
            sides[side] = (code, out, err, secs, tree(sd / 'cache'))
        rc, ro, re_, rs, rt = sides['ref']
        pc, po, pe, ps, pt = sides['port']
        ro_m, timed_o = mask(ro)
        po_m, _ = mask(po)
        re_m, timed_e = mask(re_)
        pe_m, _ = mask(pe)
        problems = []
        if rc != pc:
            problems.append(f'exit code {rc} vs {pc}')
        if ro_m != po_m:
            problems.append('stdout differs at line %d:\n      ref:  %r\n      port: %r' % first_diff(ro_m, po_m))
        if re_m != pe_m:
            problems.append('stderr differs at line %d:\n      ref:  %r\n      port: %r' % first_diff(re_m, pe_m))


        if sorted(rt.values()) != sorted(pt.values()):
            only_r = sorted(set(rt) - set(pt))
            only_p = sorted(set(pt) - set(rt))
            changed = sorted(k for k in set(rt) & set(pt) if rt[k] != pt[k])
            problems.append(f'cache files: {len(rt)} vs {len(pt)}; only ref {len(only_r)}, only port {len(only_p)}, differing {len(changed)}'
                            + (f' (e.g. {(only_r + only_p + changed)[0]})' if only_r or only_p or changed else ''))
        exact = ro == po and re_ == pe
        status = 'identical' if not problems and exact else ('identical (timing lines masked)' if not problems else 'DIFFERS')
        extra = f'  [{len(rt)} cache files, names {"same" if set(rt) == set(pt) else "differ"}, contents {"same" if sorted(rt.values()) == sorted(pt.values()) else "differ"}]' if rt or pt else ''
        print(f'{name:<32} {status}  exit {rc}/{pc}  {rs:.2f}s / {ps:.2f}s{extra}')
        if not problems and not exact:
            for line in timed_o + timed_e:
                print(f'    timing only: {line}')
        for p in problems:
            print('    ' + p)
        failures += bool(problems)
    print(f'{len(cases) - failures} of {len(cases)} cases match')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
