# SPDX-License-Identifier: GPL-3.0-or-later
"""Milestone 7: the command line. Runs the reference's `victory` (built
unchanged from the snapshot) and the port's on the same cases, one after the
other in the same fresh folders, and compares stdout, stderr and exit codes.
Inputs are synthetic (the M6 fixtures) or the host's own files (an SDK
import library, System32 DLLs); paths, cache and data folders are the same
for both runs. Usage errors (clap's wording) compare by exit code only.

Usage: python -I tools/check_cli.py <reference victory.exe> <port victory.exe> <fixture dir> <work dir> [title exe]
"""
import glob
import os
import pathlib
import re
import shutil
import subprocess
import sys

ref, port, built, work = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), pathlib.Path(sys.argv[4]).resolve()
work.mkdir(parents=True, exist_ok=True)

fixtures = work / 'fixtures'
fixtures.mkdir(exist_ok=True)
for f in ['fixture_title.exe', 'fixture_dll.dll', 'fixture_host.dll']:
    shutil.copy(built / f, fixtures / f)

title = fixtures / 'fixture_title.exe'
dll = fixtures / 'fixture_dll.dll'
sdk = sorted(glob.glob(r'C:\Program Files (x86)\Windows Kits\10\Lib\*\um\x64\kernel32.Lib'))
system = pathlib.Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'

configs = {
    'good': "[paths]\ngame_dir = 'G:\\\\nowhere\\\\game'\ntest_titles = ['T:\\\\a', 'T:\\\\b']\n",
    'cache': "[paths]\ncache_dir = 'CACHE'\ndata_dir = 'DATA'\n",
    'empty': "",
    'unknown': "[paths]\ngame = 'x'\n",
    'top': "[other]\nx = 1\n",
    'type': "[paths]\ngame_dir = 5\n",
    'syntax': "[paths\n",
}

cases = [
    ('inspect', ['inspect', str(title)], {}),
    ('inspect-imports', ['inspect', str(title), '--imports'], {}),
    ('inspect-exports', ['inspect', str(dll), '--exports', '--imports'], {}),
    ('inspect-system', ['inspect', str(system / 'kernel32.dll'), '--exports'], {}),
    ('inspect-notpe', ['inspect', str(fixtures / 'notpe.bin')], {}),
    ('inspect-missing', ['inspect', str(fixtures / 'missing.exe')], {}),
    ('census', ['census', str(fixtures)], {}),
    ('census-strict', ['census', str(fixtures), '--strict', '--missing'], {}),
    ('census-system', ['census', str(system / 'kernel32.dll'), str(system / 'user32.dll'), str(system / 'd3d12.dll'), '--missing'], {}),
    ('config-none', ['config'], {}),
    ('run', ['run', str(title)], {}),
    ('run-strict', ['run', str(title), '--strict', '--unimplemented', 'zero'], {}),
    ('run-s', ['run', str(title), '--console', 'series-s', '--unimplemented=trap'], {}),
    ('run-missing', ['run', str(fixtures / 'missing.exe')], {}),
    ('run-execute', ['run', str(title), '--execute'], {}),
    ('run-execute-zero', ['run', str(title), '--execute', '--unimplemented', 'zero', '--strict'], {}),
]


if len(sys.argv) > 5:
    game = pathlib.Path(sys.argv[5])
    cases += [
        ('title-inspect', ['inspect', str(game), '--imports', '--exports'], {}),
        ('title-run', ['run', str(game)], {}),
        ('title-run-s', ['run', str(game), '--console', 'series-s', '--strict'], {}),
        ('title-census', ['census', str(game.parent), '--missing'], {}),
    ]
if sdk:
    cases += [('lib', ['lib-exports', sdk[-1]], {}), ('lib-list', ['lib-exports', sdk[-1], '--list'], {})]
for name in configs:
    cases.append((f'config-{name}', ['config'], {'VICTORY_CONFIG': f'{name}.toml'}))
cases += [
    ('config-env', ['config'], {'VICTORY_GAME_DIR': 'from-env', 'VICTORY_TEST_TITLES': 'x;"y;z";;w'}),
    ('config-absent', ['config'], {'VICTORY_CONFIG': 'absent.toml'}),
    ('config-badutf8', ['config'], {'VICTORY_CONFIG': 'badutf8.toml'}),
]
usage = [
    ('usage-none', []),
    ('usage-unknown', ['frobnicate']),
    ('usage-inspect', ['inspect']),
    ('usage-flag', ['inspect', str(title), '--bogus']),
    ('usage-console', ['run', str(title), '--console', 'series-z']),
]


def run(exe, args, env_extra, cwd):
    env = dict(os.environ)
    for k in ['VICTORY_CONFIG', 'VICTORY_GAME_DIR', 'VICTORY_TEST_TITLES', 'VICTORY_LOG', 'RUST_LOG']:
        env.pop(k, None)
    env['VICTORY_CACHE_DIR'] = str(work / 'cache')
    env['VICTORY_DATA_DIR'] = str(work / 'data')
    env['NO_COLOR'] = '1'
    env.update(env_extra)
    r = subprocess.run([exe] + args, cwd=cwd, env=env, capture_output=True, timeout=600)
    return r.returncode, normal(r.stdout), normal(r.stderr)


def normal(raw):
    """Log lines without their time; addresses the system
    randomises per process (heap-placed images) as 0xADDR."""
    out = []
    for line in raw.replace(b'\r\n', b'\n').split(b'\n'):
        head, _, rest = line.partition(b' ')
        if head.endswith(b'Z') and head[:1].isdigit():
            line = rest
        out.append(re.sub(rb'0x[0-9a-f]{9,}', b'0xADDR', line))
    return b'\n'.join(out)


def fresh():
    d = work / 'cwd'
    shutil.rmtree('\\\\?\\' + str(d), ignore_errors=True)
    for k in ['cache', 'data']:
        shutil.rmtree('\\\\?\\' + str(work / k), ignore_errors=True)
    d.mkdir(parents=True)
    for name, text in configs.items():
        (d / f'{name}.toml').write_bytes(text.encode())
    (d / 'badutf8.toml').write_bytes(b'[paths]' + bytes([10]) + b'game_dir = ' + bytes([39, 0xff, 39, 10]))
    return d


(fixtures / 'notpe.bin').write_bytes(b'MZ but not a PE image')
failed = 0
for name, args, env in cases:
    a = run(ref, args, env, fresh())
    b = run(port, args, env, fresh())
    ok = a == b
    print(f'{name}: {"ok" if ok else "FAILED"} (exit {a[0]}, {len(a[1].splitlines())} lines)')
    if not ok:
        failed += 1
        for k, (x, y) in enumerate(zip(a, b)):
            if x != y:
                (work / f'{name}-{k}-ref.txt').write_bytes(x if isinstance(x, bytes) else str(x).encode())
                (work / f'{name}-{k}-cpp.txt').write_bytes(y if isinstance(y, bytes) else str(y).encode())
for name, args in usage:
    a = run(ref, args, {}, fresh())
    b = run(port, args, {}, fresh())
    ok = a[0] == b[0]
    print(f'{name}: {"ok" if ok else "FAILED"} (exit {a[0]} / {b[0]})')
    failed += not ok
sys.exit(1 if failed else 0)
