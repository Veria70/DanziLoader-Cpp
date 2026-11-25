# SPDX-License-Identifier: GPL-3.0-or-later


import os
import random
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
MB = 1 << 20


def fixture(name, about, env, quiet_all=False):
    r = random.Random('windows')
    base = 0x202300000000
    windows, size, rounds, writes = 32, 4 * MB, 20, 64
    lines = [f'# {about}'] + [f'env {k} {v}' for k, v in env]


    if quiet_all:
        lines.append('quiet 1')
    lines += [f'region W watched {base:#x} {windows * size:#x}', f'write W 0 {windows * size:#x} 1']
    for i in range(windows):

        lines.append(f'find w{i} W {i * size + r.randrange(size - 256):#x} 0x100')
    for i in range(windows):
        lines.append(f'refresh w{i} 0 {size:#x}')
    lines.append('card_submit')
    lines.append('quiet 1')
    for k in range(rounds):
        for j in range(writes):
            lines.append(f'write W {r.randrange(windows * size - 64):#x} 32 {k * 1000 + j + 2}')
        for i in range(windows):
            lines.append(f'refresh w{i} 0 {size:#x}')
        lines.append('card_submit')
    if not quiet_all:
        lines += ['quiet 0', 'check']
    with open(os.path.join(out, name + '.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


fixture('w1-windows-on-card', '32 windows of 4 MiB, 64 title writes a round: windows on the card (default)', [])
fixture('w2-windows-pc-memory', 'w1 with VICTORY_CARD_WINDOWS_MB=0: windows in the PC memory', [('VICTORY_CARD_WINDOWS_MB', 0)])
fixture('w3-windows-rebar', 'w1 with VICTORY_REBAR=1: windows in CPU-visible card memory', [('VICTORY_REBAR', 1)], quiet_all=True)
fixture('w4-windows-in-place', 'w1 with VICTORY_INPLACE=1: windows in place (the title memory itself)', [('VICTORY_INPLACE', 1)])
