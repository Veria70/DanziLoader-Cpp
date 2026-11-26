# SPDX-License-Identifier: GPL-3.0-or-later
"""The port's Opus decoder (bundled libopus 1.6.1, src/hle/apu.cpp) against
the reference's (`opus-pure` 0.2.4, built by build/opus-oracle). The oracle
makes a corpus of packet streams (its encoder over synthetic signals and
settings, then mutations and noise) and decodes it as `xapu` does;
`opus-check` decodes the same with libopus.

Required: the same frame count and the same accept/reject decision for every
packet. Samples are measured, not required to be identical: `opus-pure`
matches libopus bit for bit in SILK and within float rounding in CELT,
except where its documentation says it differs (concealment, decoding to the
other channel count, the cross-fade at a mode switch).

Usage: python -I tools/check_opus.py <opus-oracle> <opus-check> <work dir> [seed ...]
"""
import pathlib
import subprocess
import sys

oracle, check = str(pathlib.Path(sys.argv[1]).resolve()), str(pathlib.Path(sys.argv[2]).resolve())
work = pathlib.Path(sys.argv[3]).resolve()
seeds = sys.argv[4:] or ['1']
work.mkdir(parents=True, exist_ok=True)
failed = False
for seed in seeds:
    corpus, ref = work / f'corpus{seed}.bin', work / f'ref{seed}.bin'
    made = subprocess.run([oracle, str(corpus), str(ref), seed], capture_output=True, text=True, check=True)
    print(f'seed {seed}: {made.stdout.strip()}')
    r = subprocess.run([check, str(corpus), str(ref)], capture_output=True, text=True)
    print(r.stdout.strip())
    failed |= r.returncode != 0
sys.exit(1 if failed else 0)
