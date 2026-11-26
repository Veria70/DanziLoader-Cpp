# SPDX-License-Identifier: GPL-3.0-or-later
"""M6: keyboard-and-mouse play (`kbm`), the port's against the reference's.

Makes scripts of random steps (keys, mouse motion, wheel, pads, scripted
input, layouts, settings, key maps, environment values, `[controls]`
tables, good and bad), runs each through the reference's
`victory_core::kbm` (build/kbm-oracle, `kbm-oracle <script> <out>`) and the
port's (`kbm-check <script> <out>`), and compares the outputs line by line.

Usage: python -I tools/check_kbm.py <kbm-oracle> <kbm-check> <work dir> [scripts] [steps] [seed]
"""
import pathlib
import random
import struct
import subprocess
import sys

oracle, port = str(pathlib.Path(sys.argv[1]).resolve()), str(pathlib.Path(sys.argv[2]).resolve())
work = pathlib.Path(sys.argv[3])
scripts = int(sys.argv[4]) if len(sys.argv) > 4 else 20
steps = int(sys.argv[5]) if len(sys.argv) > 5 else 400
seed = int(sys.argv[6]) if len(sys.argv) > 6 else 1
work.mkdir(parents=True, exist_ok=True)


def hx(s):
    return 'x' + s.encode('utf-8').hex()


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def f32h(x):
    return '%08x' % f32(x)


def f64h(x):
    return '%016x' % struct.unpack('<Q', struct.pack('<d', x))[0]


KEY_NAMES = ['MouseLeft', 'MouseRight', 'MouseMiddle', 'Mouse4', 'Mouse5', 'WheelUp', 'WheelDown', 'Backspace', 'Tab',
             'Enter', 'Pause', 'CapsLock', 'Esc', 'Space', 'PageUp', 'PageDown', 'End', 'Home', 'Left', 'Up', 'Right',
             'Down', 'Insert', 'Delete', 'NumMultiply', 'NumAdd', 'NumSubtract', 'NumDecimal', 'NumDivide', 'LShift',
             'RShift', 'LCtrl', 'RCtrl', 'LAlt', 'RAlt', 'Semicolon', 'Equals', 'Comma', 'Minus', 'Period', 'Slash',
             'Grave', 'LBracket', 'Backslash', 'RBracket', 'Quote', 'Shift', 'Ctrl', 'Alt']
OUTPUT_NAMES = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'LS', 'RS', 'View', 'Menu', 'DpadUp', 'DpadDown',
                'DpadLeft', 'DpadRight', 'LeftUp', 'LeftDown', 'LeftLeft', 'LeftRight', 'RightUp', 'RightDown',
                'RightLeft', 'RightRight', 'Walk', 'ToggleVehicle', 'ReleaseCursor']
ODD = ['', ' ', '\t', '　', ' ', '\u0085', '​', '́', 'é', 'Ä', '日本', '\U0001F600', '"', '\\', '\n',
       '\r', '\x00', '\x1b', '\x7f', ' ', '#', '=', ',', '[', ']', '﻿', '­', '퟿']
FIELDS = ['enabled', 'mouse_sensitivity', 'mouse_y_scale', 'mouse_invert_y', 'mouse_capture', 'mouse_carry',
          'stick_deadzone', 'stick_curve', 'walk_speed', 'on_foot', 'vehicle']
DATETIMES = ['1979-05-27T07:32:00Z', '1979-05-27T00:32:00.999999-07:00', '1979-05-27T07:32:00', '1979-05-27',
             '07:32:00', '00:32:00.5']
FLOATS = [2.5, 0.24, 1.5, 0.0, -0.0, 0.01, 100.0, 100.00001, 1e-7, 1e300, float('inf'), float('-inf'), float('nan'),
          0.009999999, 0.05, 5.0, 0.2, 0.19999999, 1e16, 123456789.0, 0.1 + 0.2, 0.9, 0.90000001, 1.0, -1.0, 3.0,
          0.049999999, 5.0000001, 1e-45, 3.4028235e38, 3.5e38, 2.0 ** -149, 0.123456789012345]
INTS = [0, 1, -1, 5, 100, 101, 2 ** 24 + 1, 2 ** 53 + 1, 2 ** 63 - 1, -2 ** 63, 3, 2, -0]
FLOAT_TEXT = ['2.5', ' 0.01 ', '100', '100.0000001', '1e2', '1e-2', 'inf', 'NaN', '+3', '.5', '5.', '1_0', '0x10', '',
              '  ', '１', '-1', '0', '1E1', '1e', '.', '+', '-inf', 'infinity', '1e40', '0.0099999999', '50 ',
              '　2', '3 ', 'nan(1)', '2.5f', '1,5', '99.99999999999']
FLAG_TEXT = ['1', '0', 'true', 'FALSE', 'On', 'off', 'yes', 'No', '', ' ', ' 1 ', '2', 'maybe', '　yes', 'tru',
             'ja', ' on ', 'Yes\n']


def rnd_name(r):
    k = r.random()
    if k < 0.35:
        n = r.choice(KEY_NAMES)
    elif k < 0.55:
        n = r.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcxyz')
    elif k < 0.7:
        n = r.choice(['F', 'f', 'Num', 'num', 'NUM']) + r.choice(['0', '1', '9', '10', '12', '24', '25', '+5', '+',
                                                                  '-0', '007', '255', '256', '', 'x', '1 ', 'é'])
    else:
        n = ''.join(r.choice(ODD + list('AFNumWheel')) for _ in range(r.randint(0, 4)))
    if r.random() < 0.3:
        n = ''.join(ch.lower() if r.random() < 0.5 else ch.upper() for ch in n)
    if r.random() < 0.2:
        n = r.choice(ODD[:7]) + n + r.choice(ODD[:7])
    return n


def rnd_output(r):
    k = r.random()
    if k < 0.75:
        n = r.choice(OUTPUT_NAMES)
        if r.random() < 0.3:
            n = n.upper() if r.random() < 0.5 else n.lower()
    elif k < 0.85:
        n = ''
    else:
        n = r.choice(['RightTrigger_typo', 'Nonsense', 'A B', 'é', 'Dpad', 'Left'])
    if r.random() < 0.2:
        n = r.choice(ODD[:5]) + n + r.choice(ODD[:5])
    return n


def rnd_float32(r):
    k = r.random()
    if k < 0.15:
        return r.choice([0.0, -0.0, 1.0, -1.0, 0.2, -0.2, 0.1, 0.5, 0.50000006, 0.2000001, 0.19999999])
    if k < 0.2:
        return r.choice([float('nan'), float('inf'), float('-inf'), 2.0, -3.0])
    if k < 0.6:
        return 0.0
    return r.uniform(-1.3, 1.3)


def rnd_pad(r, none_ok=True):
    if none_ok and r.random() < 0.4:
        return '-'
    buttons = 0 if r.random() < 0.5 else r.getrandbits(16) & r.getrandbits(16)
    vals = [f32h(rnd_float32(r)) for _ in range(6)]
    return '%x,' % buttons + ','.join(vals)


def rnd_value(r, depth=0):
    k = r.random()
    if k < 0.25:
        return 's:' + hx(rnd_output(r) if r.random() < 0.6 else rnd_name(r))
    if k < 0.4:
        return 'i:%d' % r.choice(INTS + [r.randint(-1000, 1000)])
    if k < 0.6:
        return 'f:' + f64h(r.choice(FLOATS + [r.uniform(-10, 200)]))
    if k < 0.75:
        return 'b:%d' % r.randint(0, 1)
    if k < 0.8:
        return 'd:' + hx(r.choice(DATETIMES))
    if k < 0.92 and depth < 2:
        return '[ ' + ' '.join(rnd_value(r, depth + 1) for _ in range(r.randint(0, 3))) + ' ]'
    if depth < 2:
        return rnd_table(r, depth + 1)
    return 'b:1'


def rnd_binding(r):
    k = r.random()
    if k < 0.5:
        return 's:' + hx(rnd_output(r))
    if k < 0.8:
        return '[ ' + ' '.join('s:' + hx(rnd_output(r)) for _ in range(r.randint(0, 3))) + ' ]'
    if k < 0.85:
        return 'd:' + hx(r.choice(DATETIMES))
    return rnd_value(r, 1)


def rnd_layout_table(r):
    items = []
    for _ in range(r.randint(0, 5)):
        items.append('k:' + hx(rnd_name(r)) + ' ' + rnd_binding(r))
    return '{ ' + ' '.join(items) + ' }'


def rnd_table(r, depth=0):
    items = []
    for _ in range(r.randint(0, 3)):
        items.append('k:' + hx(rnd_name(r)) + ' ' + rnd_value(r, depth + 1))
    return '{ ' + ' '.join(items) + ' }'


def rnd_controls_table(r):
    items = []
    bad = r.random() < 0.3
    for f in FIELDS:
        if r.random() < 0.4:
            continue
        if f in ('on_foot', 'vehicle'):
            v = rnd_layout_table(r) if r.random() < 0.9 else rnd_value(r)
        elif f in ('enabled', 'mouse_invert_y', 'mouse_capture', 'mouse_carry'):
            v = 'b:%d' % r.randint(0, 1) if not bad or r.random() < 0.7 else rnd_value(r)
        else:
            if bad and r.random() < 0.4:
                v = rnd_value(r)
            elif r.random() < 0.3:
                v = 'i:%d' % r.choice([0, 1, 2, 3, 5, 100, -1, 101])
            else:
                lo, hi = {'mouse_sensitivity': (0.01, 100), 'mouse_y_scale': (0.01, 100), 'stick_deadzone': (0, 0.9),
                          'stick_curve': (0.2, 5), 'walk_speed': (0.05, 1)}[f]
                v = 'f:' + f64h(r.uniform(lo, hi) if r.random() < 0.8 else r.choice(FLOATS))
        items.append('k:' + hx(f) + ' ' + v)
    if bad and r.random() < 0.5:
        items.append('k:' + hx(r.choice(['mouse_sensitivty', 'Enabled', 'x', 'controls', '', 'on_foot ', 'zzz', 'a.b']))
                     + ' ' + rnd_value(r))
    r.shuffle(items)
    return 'parse { ' + ' '.join(items) + ' }'


def rnd_keymap(r):
    lines = []
    for _ in range(r.randint(0, 8)):
        k = r.random()
        if k < 0.55:
            outs = ','.join(rnd_output(r) for _ in range(r.randint(0, 3)))
            line = rnd_name(r) + r.choice(['=', ' = ', '=\t']) + outs
        elif k < 0.7:
            line = '[' + r.choice(['on_foot', 'OnFoot', 'foot', 'vehicle', ' Vehicle ', 'all', 'both', 'oops', '', 'x]'])
            line += r.choice([']', ']', '', ' ]'])
        elif k < 0.8:
            line = r.choice(['', '   ', '# comment', 'no equals here', '[', ']', '=', '==', '#=A'])
        else:
            line = ''.join(r.choice(ODD + list('AW=,#[] ')) for _ in range(r.randint(0, 6)))
        if r.random() < 0.2:
            line += ' # ' + rnd_output(r)
        lines.append(line)
    sep = r.choice(['\n', '\r\n', '\n'])
    text = sep.join(lines)
    if r.random() < 0.5:
        text += sep
    return 'keymap ' + hx(text)


def rnd_env(r):
    specs = []
    names = ['VICTORY_KBM', 'VICTORY_MOUSE_SENS', 'VICTORY_MOUSE_INVERT_Y', 'VICTORY_KEYMAP']
    for n in names:
        if r.random() < 0.5:
            continue
        if n == 'VICTORY_MOUSE_SENS':
            v = r.choice(FLOAT_TEXT) if r.random() < 0.7 else repr(r.uniform(-1, 120))
        elif n == 'VICTORY_KEYMAP':
            v = r.choice(['keys.txt', ' keys.txt ', '', '  ', 'missing.txt', 'bad.txt', 'é.txt'])
        else:
            v = r.choice(FLAG_TEXT)
        specs.append('get:%s=%s' % (hx(n), hx(v)))
    if r.random() < 0.3:
        specs.append('get:%s=%s' % (hx('VICTORY_OTHER'), hx('1')))
    km = rnd_keymap(r).split(' ', 1)[1]
    specs.append('file:%s=%s' % (hx('keys.txt'), km))
    specs.append('file:%s=%s' % (hx('é.txt'), hx('Space=Y')))
    specs.append('ferr:%s=%s' % (hx('bad.txt'), hx('Access is denied. (os error 5)')))
    r.shuffle(specs)
    return 'env ' + ' '.join(specs)


def rnd_keys(r):
    pool = [0x57, 0x41, 0x53, 0x44, 0x20, 0xA0, 0xA1, 0xA2, 0x47, 0x5A, 0xA4, 0xA5, 0x01, 0x02, 0x04, 0x05, 0x06,
            0x0D, 0x1B, 0x08, 0x09, 0x50, 0x45, 0x46, 0x51, 0x52, 0x43, 0x26, 0x28, 0x25, 0x27, 0x0E, 0x0F, 0x48, 0x4A]
    n = r.choice([0, 0, 1, 1, 2, 3, 4])
    keys = [r.choice(pool) if r.random() < 0.95 else r.randint(0, 255) for _ in range(n)]
    return ','.join('%x' % k for k in keys) if keys else '-'


def rnd_settings(r, out):
    out.append('settings')
    for f in ['enabled', 'invert_y', 'capture', 'carry']:
        if r.random() < 0.2:
            out.append('sset %s %d' % (f, r.randint(0, 1)))
    for f, lo, hi in [('sensitivity', 0.01, 100), ('y_scale', 0.01, 100), ('deadzone', 0, 0.9), ('curve', 0.2, 5),
                      ('walk_speed', 0.05, 1)]:
        if r.random() < 0.3:
            v = r.uniform(lo, hi) if r.random() < 0.9 else r.choice([lo, hi, 1.0, 0.0])
            out.append('sset %s %s' % (f, f32h(v)))


def script(r, n):
    out = []
    now = 0
    for i in range(n):
        k = r.random()
        if k < 0.02:
            rnd_settings(r, out)
            out.append(r.choice(['new', 'cs']))
        elif k < 0.04:
            out.append(rnd_controls_table(r))
            if r.random() < 0.5:
                out.append('cs')
        elif k < 0.05:
            out.append(rnd_keymap(r))
            out.append('cs')
        elif k < 0.06:
            out.append(rnd_env(r))
            out.append('cs')
        elif k < 0.25:
            out.append('feed ' + rnd_keys(r))
        elif k < 0.37:
            big = r.random() < 0.03
            dx = r.choice([2 ** 62, -2 ** 62, 10 ** 12]) if big else int(r.gauss(0, 30))
            dy = int(r.gauss(0, 20))
            out.append('add %d %d' % (dx, dy))
        elif k < 0.41:
            out.append('wheel %d' % r.choice([1, -1, 2, -3, 0, 12, -40, 1000]))
        elif k < 0.5:
            out.append('peek ' + rnd_pad(r))
        elif k < 0.75:
            now += r.choice([0, 500, 1999, 2000, 8000, 16667, 33333, 300000, 1500000])
            t = now if r.random() < 0.95 else max(0, now - r.randint(0, 50000))
            out.append('take %d %s' % (t, rnd_pad(r)))
        elif k < 0.82:
            out.append('script ' + rnd_pad(r, none_ok=False))
        elif k < 0.86:
            out.append('merged %s %s' % (rnd_pad(r, False), rnd_pad(r, False)))
        elif k < 0.92:
            out.append('state')
        elif k < 0.94:
            out.append('clear')
        elif k < 0.96:
            out.append('keycode ' + hx(rnd_name(r)))
        elif k < 0.97:
            out.append('keyname %x' % r.randint(0, 255))
        elif k < 0.98:
            base = r.choice(['Game', '', 'Grand Theft', 'Game - click to capture the mouse (Alt frees it)',
                             ' - click to capture the mouse (Alt frees it)', 'é - click'])
            out.append('caption %s %d' % (hx(base), r.randint(0, 1)))
        else:
            out.append('dump')
    return out


def fixed():
    """Every key name and code, every output name, each field's bad types."""
    out = []
    for n in KEY_NAMES + OUTPUT_NAMES + ['F%d' % i for i in range(27)] + ['Num%d' % i for i in range(12)]:
        for v in (n, n.lower(), n.upper(), ' %s\t' % n):
            out.append('keycode ' + hx(v))
    for v in range(256):
        out.append('keyname %x' % v)
    values = (['s:' + hx(s) for s in ['x', 'yes', 'a"b\\c\n\t\r\x00\x01\x7f', 'é́​\U0001F600\u0085­﻿',
                                      ' 　 ', '']] +
              ['i:%d' % i for i in INTS] + ['f:' + f64h(f) for f in FLOATS] + ['b:0', 'b:1'] +
              ['d:' + hx(d) for d in DATETIMES] + ['[ ]', '[ s:' + hx('A') + ' ]', '[ i:1 ]', '{ }',
                                                   '{ k:' + hx('A') + ' s:' + hx('B') + ' }'])
    for f in FIELDS:
        for v in values:
            out.append('parse { k:%s %s }' % (hx(f), v))
            out.append('parse { k:%s { k:%s %s } }' % (hx(f), hx('Space'), v))
    for k in ['unknown', 'Enabled', '', 'a.b', 'é', 'mouse_sensitivty']:
        out.append('parse { k:%s b:1 }' % hx(k))
    for t in FLOAT_TEXT:
        out.append('env get:%s=%s' % (hx('VICTORY_MOUSE_SENS'), hx(t)))
    for t in FLAG_TEXT:
        out.append('env get:%s=%s get:%s=%s' % (hx('VICTORY_KBM'), hx(t), hx('VICTORY_MOUSE_INVERT_Y'), hx(t)))
    return out


failed = 0
total = 0
rng = random.Random(seed)
jobs = [('fixed', fixed())] + [('fuzz%03d' % i, script(rng, steps)) for i in range(scripts)]
for name, lines in jobs:
    path = work / (name + '.txt')
    path.write_bytes(('\n'.join(lines) + '\n').encode())
    ref, cpp = work / (name + '-ref.out'), work / (name + '-cpp.out')

    a = subprocess.run([oracle, str(path), str(ref)], stderr=subprocess.DEVNULL)
    b = subprocess.run([port, str(path), str(cpp)], stderr=subprocess.DEVNULL)
    ra = ref.read_bytes().decode().split('\n') if ref.exists() else []
    rb = cpp.read_bytes().decode().split('\n') if cpp.exists() else []
    steps_run = sum(1 for l in ra if l.startswith('#'))
    total += steps_run
    diffs = [i for i in range(max(len(ra), len(rb))) if (ra[i] if i < len(ra) else None) != (rb[i] if i < len(rb) else None)]
    ok = a.returncode == 0 and b.returncode == 0 and not diffs and steps_run > 0
    print('%s: %d steps, %d differing lines, %s' % (name, steps_run, len(diffs), 'ok' if ok else 'FAILED'))
    for i in diffs[:5]:
        ctx = [l for l in ra[:i + 1] if l.startswith('#')][-1:] if i < len(ra) else []
        print('  line %d %s\n    ref: %s\n    cpp: %s' % (i + 1, ctx, ra[i] if i < len(ra) else None, rb[i] if i < len(rb) else None))
    failed += not ok
print('%d steps in %d scripts, %d failed' % (total, len(jobs), failed))
sys.exit(1 if failed else 0)
