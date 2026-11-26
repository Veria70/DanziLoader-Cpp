# SPDX-License-Identifier: GPL-3.0-or-later
"""Differential test of the BCPack port: compares the reference's results
(`build/bcpack-oracle`, the unchanged `victory-formats/src/bcpack.rs`;
DanziLoader / victory-perf, see NOTICE.md) with the port's
(`bcpack-check run`) on the same corpus, case by case: success or the error's
text, and every byte of the destination (blocks written before an error
included).

Usage: python -I tools/check_bcpack.py <corpus> <oracle results> <port results>
"""
import collections
import struct
import sys


def corpus_cases(data):
    """(kind, bc, table) per case."""
    at = 0
    (tables,) = struct.unpack_from('<I', data, at)
    at += 4
    for _ in range(tables):
        kind = data[at]
        at += 1
        if kind == 1:
            (n,) = struct.unpack_from('<I', data, at)
            at += 4 + n
    (count,) = struct.unpack_from('<I', data, at)
    at += 4
    cases = []
    for _ in range(count):
        kind = data[at]
        at += 1
        if kind == 0:
            table, bc, _fill, _dst, n = struct.unpack_from('<IBBII', data, at)
            at += 14 + n
            cases.append((0, bc, table))
        else:
            (n,) = struct.unpack_from('<I', data, at)
            at += 4 + n
            cases.append((1, None, None))
    assert at == len(data), 'trailing corpus bytes'
    return cases


def results(data):
    """(kind, failed, text, dst) per case."""
    at = 0
    out = []
    while at < len(data):
        kind, failed, n = struct.unpack_from('<BBH', data, at)
        at += 4
        text = data[at:at + n].decode('utf-8')
        at += n
        dst = None
        if kind == 0:
            (m,) = struct.unpack_from('<I', data, at)
            at += 4
            dst = data[at:at + m]
            at += m
        out.append((kind, failed, text, dst))
    return out


def main():
    corpus = open(sys.argv[1], 'rb').read()
    want = results(open(sys.argv[2], 'rb').read())
    got = results(open(sys.argv[3], 'rb').read())
    cases = corpus_cases(corpus)
    if not (len(cases) == len(want) == len(got)):
        print(f'case counts differ: corpus {len(cases)}, oracle {len(want)}, port {len(got)}')
        return 1
    diffs = 0
    ok = collections.Counter()
    total = collections.Counter()
    for k, (case, w, g) in enumerate(zip(cases, want, got)):
        label = 'parse' if case[0] == 1 else f'bc{case[1]}'
        total[label] += 1
        ok[label] += w[1] == 0
        if w != g:
            diffs += 1
            if diffs <= 20:
                where = ''
                if w[3] is not None and g[3] is not None and w[3] != g[3]:
                    first = next((i for i, (a, b) in enumerate(zip(w[3], g[3])) if a != b), min(len(w[3]), len(g[3])))
                    where = f', dst differs from byte {first}'
                print(f'case {k} ({label}, table {case[2]}): oracle {w[1]} {w[2]!r}, port {g[1]} {g[2]!r}{where}')
    print(f'{len(cases)} cases, {sum(ok.values())} ok in the reference, {diffs} differences')
    for label in sorted(total, key=lambda s: (s == 'parse', len(s), s)):
        print(f'  {label:6} {total[label]:6} cases, {ok[label]:6} ok')
    return 1 if diffs else 0


if __name__ == '__main__':
    sys.exit(main())
