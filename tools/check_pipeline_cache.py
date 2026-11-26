# SPDX-License-Identifier: GPL-3.0-or-later


import os
import re
import shutil
import subprocess
import sys

driver, work = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scenario = os.path.join(here, 'tests', 'm4-port', 'pipeline-cache.txt')
shutil.rmtree('c:/tmp/m4-pipeline-cache-port', ignore_errors=True)
counts = []
for run in (1, 2):
    out = os.path.join(work, f'pipeline-cache-{run}')
    subprocess.run([driver, scenario, out], check=True)
    log = open(os.path.join(out, 'log.txt'), encoding='utf-8').read()
    m = re.search(r'made (\d+) loaded (\d+) early (\d+)', log)
    counts.append(tuple(int(x) for x in m.groups()))
print('first run (made, loaded, early):', counts[0])
print('second run (made, loaded, early):', counts[1])
ok = counts[0][0] >= 2 and counts[0][1] == 0 and counts[1][1] >= 2 and counts[1][0] == 0 and counts[1][2] >= 2
print('ok' if ok else 'FAILED')
sys.exit(0 if ok else 1)
