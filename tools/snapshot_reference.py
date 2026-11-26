# SPDX-License-Identifier: GPL-3.0-or-later


import hashlib
import os
import shutil
import subprocess
import sys

ref, out = sys.argv[1], sys.argv[2]
CRATES = ['victory-core', 'victory-formats', 'victory-pipeline', 'victory-shader', 'victory-gpu', 'victory-game', 'victory-pe', 'victory-loader', 'victory-hle']


os.makedirs(os.path.join(out, 'crates'), exist_ok=True)


def sync(src, dst):
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns('target'), dirs_exist_ok=True)
    for root, dirs, files in os.walk(dst):
        rel = os.path.relpath(root, dst)
        for f in files:
            if not os.path.exists(os.path.join(src, rel, f)):
                os.remove(os.path.join(root, f))


lines = []
commit = subprocess.run(['git', '-C', ref, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
status = subprocess.run(['git', '-C', ref, 'status', '--short'], capture_output=True, text=True).stdout.strip()
lines.append(f'reference: {ref}')
lines.append(f'commit: {commit}')
lines.append('working tree: ' + (status.replace('\n', '; ') or 'clean'))

for crate in CRATES:
    src = os.path.join(ref, 'crates', crate)
    dst = os.path.join(out, 'crates', crate)
    sync(src, dst)
    for root, _, files in os.walk(dst):
        for f in sorted(files):
            p = os.path.join(root, f)
            h = hashlib.sha256(open(p, 'rb').read()).hexdigest()
            lines.append(f'{h}  {os.path.relpath(p, out).replace(os.sep, "/")}')


manifest = open(os.path.join(ref, 'Cargo.toml'), encoding='utf-8').read()
manifest = manifest.replace('members = ["crates/*", "apps/*", "xtask"]', 'members = ["crates/*", "apps/victory", "oracle", "oracle-m5", "oracle-m6"]', 1)
assert 'members = ["crates/*", "apps/victory", "oracle", "oracle-m5", "oracle-m6"]' in manifest
open(os.path.join(out, 'Cargo.toml'), 'w', encoding='utf-8', newline='\n').write(manifest)
shutil.copy(os.path.join(ref, 'Cargo.lock'), os.path.join(out, 'Cargo.lock'))
shutil.copy(os.path.join(ref, 'rust-toolchain.toml'), os.path.join(out, 'rust-toolchain.toml'))

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sync(os.path.join(here, 'oracle-m4'), os.path.join(out, 'oracle'))

sync(os.path.join(here, 'oracle-m5'), os.path.join(out, 'oracle-m5'))

sync(os.path.join(here, 'oracle-m6'), os.path.join(out, 'oracle-m6'))

src = os.path.join(ref, 'apps', 'victory')
dst = os.path.join(out, 'apps', 'victory')
sync(src, dst)
for root, _, files in os.walk(dst):
    for f in sorted(files):
        p = os.path.join(root, f)
        h = hashlib.sha256(open(p, 'rb').read()).hexdigest()
        lines.append(f'{h}  {os.path.relpath(p, out).replace(os.sep, "/")}')
open(os.path.join(out, 'MANIFEST.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
print(f'{len(lines) - 3} files from {len(CRATES)} crates, commit {commit[:10]}')
