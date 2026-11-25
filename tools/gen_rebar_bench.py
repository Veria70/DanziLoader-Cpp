# SPDX-License-Identifier: GPL-3.0-or-later


import os
import random
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
KB = 1024


def fixture(name, about, relay, buffers, size, rounds, writes):
    r = random.Random(name)
    base = 0x201D00000000
    lines = [f'# {about}', 'env VICTORY_REBAR 2', f'region W watched {base:#x} {buffers * size + 0x10000:#x}',
             f'write W 0 {buffers * size:#x} 1']
    names = [f's{i}' for i in range(buffers)]
    for i, n in enumerate(names):
        lines.append(f'card_shadow {n} W {i * size:#x} {size:#x}')
    for n in names:
        lines.append(f'refresh {n} 0 {size:#x}')
    lines.append('quiet 1')
    for k in range(rounds):
        for _ in range(2):
            lines.append(f'write W {r.randrange(buffers * size - 64):#x} 32 {k * 100 + 3}')
        for n in names:
            lines.append(f'refresh {n} 0 {size:#x}')
        if relay:
            for n in names:
                lines.append(f'merge_under {n} 0 {size:#x} 64')
        lines.append(f'prepare {k + 2}')
        for n in names:
            lines.append(f'gpu_declare {n} 0 {size:#x}')
        lines.append('snapshots')
        for n in names:
            for w in range(writes):
                lines.append(f'card_write {n} {r.randrange(size - 64):#x} {r.randrange(4, 64):#x} {k * 1000 + w + 1}')
        ranges = ' '.join(f'{n} 0 {size:#x}' for n in names)
        if relay:
            lines.append(f'card_send {ranges}')
            if k > 0:
                lines.append('card_land 1')
        else:
            lines.append(f'card_submit {ranges}')
    if relay:
        lines.append('card_land')
    lines += ['quiet 0', 'check']
    with open(os.path.join(out, name + '.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


fixture('e1-rebar-4x256kb', 'VICTORY_REBAR=2: four 256 KiB title buffers, 8 small shader writes each a round, waited for',
        False, 4, 256 * KB, 10, 8)
fixture('e2-rebar-4x256kb-relay', 'e1 sent ahead, one in the air', True, 4, 256 * KB, 10, 8)
