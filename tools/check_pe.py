# SPDX-License-Identifier: GPL-3.0-or-later
"""M6.1 and M6.2: the synthetic PE fixtures (tools/gen_pe_fixtures.py) read
by the reference's victory-pe (m6-oracle) and by the port (pe-check), and
prepared by each side's loader (`prepare`): the same text. M6.3: the
repository policy (`policy`) on synthetic folders, one of them marked.
M6.5: the runtime's table (`hle-registry`) and how its resolver classifies
every import of the fixtures (`hle-classify`).

Usage: python -I tools/check_pe.py <m6-oracle> <pe-check> <work dir>
"""
import pathlib
import subprocess
import sys

oracle, port, work = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
here = pathlib.Path(__file__).resolve().parent
work.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, '-I', str(here / 'gen_pe_fixtures.py'), str(work / 'fix')], check=True)
lst = work / 'fix' / 'list.txt'
failed = False
for mode in ('pe', 'prepare', 'hle-classify'):
    subprocess.run([oracle, mode, str(lst), str(work / f'ref-{mode}.txt')], check=True)
    subprocess.run([port, mode, str(lst), str(work / f'cpp-{mode}.txt')], check=True)
    ref = (work / f'ref-{mode}.txt').read_text(encoding='utf-8').splitlines()
    cpp = (work / f'cpp-{mode}.txt').read_text(encoding='utf-8').splitlines()
    bad = next((i for i, (a, b) in enumerate(zip(ref, cpp)) if a != b), None)
    if bad is not None:
        print(f'{mode}: first difference at line {bad + 1}:')
        print(f'  ref {ref[bad]}')
        print(f'  cpp {cpp[bad]}')
        failed = True
    elif len(ref) != len(cpp):
        print(f'{mode}: {len(ref)} lines against {len(cpp)}')
        failed = True
    else:
        print(f'{mode}: {sum(1 for l in ref if l.startswith("== "))} fixtures, {len(ref)} lines alike')

pol = work / 'pol'
(pol / 'marked' / 'sub').mkdir(parents=True, exist_ok=True)
(pol / 'marked' / '.victory-root').write_text('marker')
cases = [pol / 'free' / 'new' / 'dir', pol / 'marked', pol / 'marked' / 'sub' / 'x', str(pol / 'marked') + '/../free',
         str(pol / 'free') + '/../marked/y', str(pol / 'marked' / 'sub') + '/./a/../b', 'relative/dir', 'Z:/no/such/drive']
(pol / 'list.txt').write_text(''.join(str(c) + chr(10) for c in cases), encoding='utf-8')
subprocess.run([oracle, 'policy', str(pol / 'list.txt'), str(pol / 'ref.txt')], check=True)
subprocess.run([port, 'policy', str(pol / 'list.txt'), str(pol / 'cpp.txt')], check=True)
if (pol / 'ref.txt').read_text(encoding='utf-8') != (pol / 'cpp.txt').read_text(encoding='utf-8'):
    print('policy: the decisions or messages differ')
    failed = True
else:
    print(f'policy: {len(cases)} paths alike')
subprocess.run([oracle, 'hle-registry', str(work / 'reg-ref.txt')], check=True)
subprocess.run([port, 'hle-registry', str(work / 'reg-cpp.txt')], check=True)
if (work / 'reg-ref.txt').read_bytes() != (work / 'reg-cpp.txt').read_bytes():
    print('hle-registry: the tables differ')
    failed = True
else:
    print('hle-registry: alike')
sys.exit(1 if failed else 0)
