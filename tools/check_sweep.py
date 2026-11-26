# SPDX-License-Identifier: GPL-3.0-or-later


import os
import re
import subprocess
import sys

driver, work = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scenario = os.path.join(here, 'tests', 'm4-port', 'sweep.txt')
out = os.path.join(work, 'sweep')
subprocess.run([driver, scenario, out], check=True)
log = open(os.path.join(out, 'log.txt'), encoding='utf-8').read()
m = re.search(r'windows (\d+) staging (\d+) textures (\d+)', log)
windows, staging, textures = (int(x) for x in m.groups())
print(f'let go: windows {windows}, staging buffers {staging}, host copies of textures {textures}')
ok = windows >= 1 and staging >= 1 and textures >= 1 and 'error' not in log
print('ok' if ok else 'FAILED')
sys.exit(0 if ok else 1)
