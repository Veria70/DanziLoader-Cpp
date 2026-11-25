# SPDX-License-Identifier: GPL-3.0-or-later


import hashlib
import os
import shutil
import subprocess
import sys

ref, out = sys.argv[1], sys.argv[2]
CRATES = ['victory-core', 'victory-formats', 'victory-pipeline', 'victory-shader', 'victory-gpu']

if os.path.isdir(out):
    shutil.rmtree(out)
os.makedirs(os.path.join(out, 'crates'))

lines = []
commit = subprocess.run(['git', '-C', ref, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
status = subprocess.run(['git', '-C', ref, 'status', '--short'], capture_output=True, text=True).stdout.strip()
lines.append(f'reference: {ref}')
lines.append(f'commit: {commit}')
lines.append('working tree: ' + (status.replace('\n', '; ') or 'clean'))

for crate in CRATES:
    src = os.path.join(ref, 'crates', crate)
    dst = os.path.join(out, 'crates', crate)
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns('target'))
    for root, _, files in os.walk(dst):
        for f in sorted(files):
            p = os.path.join(root, f)
            h = hashlib.sha256(open(p, 'rb').read()).hexdigest()
            lines.append(f'{h}  {os.path.relpath(p, out).replace(os.sep, "/")}')


manifest = open(os.path.join(ref, 'Cargo.toml'), encoding='utf-8').read()
manifest = manifest.replace('members = ["crates/*", "apps/*", "xtask"]', 'members = ["crates/*", "oracle"]', 1)
assert 'members = ["crates/*", "oracle"]' in manifest
open(os.path.join(out, 'Cargo.toml'), 'w', encoding='utf-8', newline='\n').write(manifest)
shutil.copy(os.path.join(ref, 'Cargo.lock'), os.path.join(out, 'Cargo.lock'))
shutil.copy(os.path.join(ref, 'rust-toolchain.toml'), os.path.join(out, 'rust-toolchain.toml'))

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
shutil.copytree(os.path.join(here, 'oracle-m4'), os.path.join(out, 'oracle'), ignore=shutil.ignore_patterns('target'))
open(os.path.join(out, 'MANIFEST.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
print(f'{len(lines) - 3} files from {len(CRATES)} crates, commit {commit[:10]}')
