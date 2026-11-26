# SPDX-License-Identifier: GPL-3.0-or-later
"""Milestone 6: synthetic PE32+ images and import libraries for the PE
readers (victory-pe and the port's src/pe), written to <out dir> with a
list of them (list.txt). A well-formed image is built from parts, then
broken one way at a time (headers, sections, data directories, imports,
delay imports, exports, TLS, CodeView, names that are not UTF-8); archives
likewise (GNU, COFF and BSD layouts, thin, each import-object name type).
Nothing here comes from a real binary.

Usage: python -I tools/gen_pe_fixtures.py <out dir>
"""
import os
import struct
import sys

IMAGE_BASE = 0x140000000


def u16(v):
    return struct.pack('<H', v & 0xFFFF)


def u32(v):
    return struct.pack('<I', v & 0xFFFFFFFF)


def u64(v):
    return struct.pack('<Q', v & 0xFFFFFFFFFFFFFFFF)


class Image:
    """A PE32+ image from parts: sections of bytes, data directories, then
    patches by name (offsets kept in `at`)."""

    def __init__(self):
        self.lfanew = 0x80
        self.machine = 0x8664
        self.characteristics = 0x0022
        self.opt_magic = 0x20B
        self.opt_size = 240
        self.rva_count = 16
        self.dirs = [(0, 0)] * 16
        self.sections = []   
        self.subsystem = 3
        self.dll_chars = 0x8160
        self.entry = 0x1000
        self.at = {}

    def build(self):
        file_align = 0x200
        headers = 0x400
        out = bytearray(headers)
        out[0:2] = b'MZ'
        out[0x3C:0x40] = u32(self.lfanew)
        nt = self.lfanew
        out[nt:nt + 4] = b'PE\0\0'
        fh = nt + 4
        out[fh:fh + 20] = u16(self.machine) + u16(len(self.sections)) + u32(0x5F000000) + u32(0) + u32(0) + u16(self.opt_size) + u16(
            self.characteristics)
        opt = fh + 20
        size_of_image = 0x1000
        for s in self.sections:
            size_of_image = max(size_of_image, (s[1] + max(s[2], 1) + 0xFFF) & ~0xFFF)
        o = bytearray(240)
        o[0:2] = u16(self.opt_magic)
        o[16:20] = u32(self.entry)
        o[24:32] = u64(IMAGE_BASE)
        o[32:36] = u32(0x1000)
        o[36:40] = u32(file_align)
        o[56:60] = u32(size_of_image)
        o[60:64] = u32(headers)
        o[68:70] = u16(self.subsystem)
        o[70:72] = u16(self.dll_chars)
        o[72:80] = u64(0x100000)
        o[80:88] = u64(0x1000)
        o[108:112] = u32(self.rva_count)
        for i, (rva, size) in enumerate(self.dirs):
            o[112 + 8 * i:120 + 8 * i] = u32(rva) + u32(size)
        out[opt:opt + 240] = o
        sec = opt + self.opt_size if self.opt_size >= 112 else opt + 240
        raw = headers
        body = bytearray()
        for name, rva, vsize, data, chars in self.sections:
            size = (len(data) + file_align - 1) // file_align * file_align
            hdr = name.ljust(8, b'\0')[:8] + u32(vsize) + u32(rva) + u32(size) + u32(raw if size else 0) + bytes(12) + u32(chars)
            if sec + 40 <= headers:
                out[sec:sec + 40] = hdr
            sec += 40
            body += data.ljust(size, b'\0')
            raw += size
        return bytes(out + body)


class Section:
    """A section's bytes with named places."""

    def __init__(self, rva):
        self.rva = rva
        self.data = bytearray()
        self.at = {}

    def here(self, name=None, align=8):
        while len(self.data) % align:
            self.data.append(0)
        if name:
            self.at[name] = self.rva + len(self.data)
        return self.rva + len(self.data)

    def put(self, b, name=None, align=8):
        r = self.here(name, align)
        self.data += b
        return r

    def patch(self, rva, b):
        o = rva - self.rva
        self.data[o:o + len(b)] = b


def standard(**opts):
    """The well-formed image: .text, .rdata (imports, delay imports, exports,
    TLS, debug), .data; `opts` change parts before building."""
    img = Image()
    text = Section(0x1000)
    text.put(b'\xC3' * 0x40, 'code')
    rd = Section(0x2000)

    names = {}
    dll_names = opts.get('dlls', [b'kernel32.dll', b'XGameRuntime.dll'])
    imports = opts.get('imports', [[(5, b'Sleep'), (7, b'ExitProcess'), 'o12'], [(0, b'XGameRuntimeInitialize')]])
    for d in dll_names:
        names[d] = rd.put(d + b'\0', align=2)
    hint_names = []
    for module in imports:
        hn = []
        for e in module:
            if isinstance(e, str):
                hn.append(None)
            else:
                hn.append(rd.put(u16(e[0]) + e[1] + b'\0', align=2))
        hint_names.append(hn)

    ilts, iats = [], []
    for k, module in enumerate(imports):
        entries = b''
        for e, hn in zip(module, hint_names[k]):
            if isinstance(e, str):
                entries += u64(0x8000000000000000 | int(e[1:]))
            else:
                entries += u64(hn)
        terminator = b'' if opts.get('no_thunk_null') and k == len(imports) - 1 else u64(0)
        ilts.append(rd.put(entries + terminator))
        iats.append(rd.put(entries + u64(0)))

    desc_rva = rd.here('imports', 4)
    for k in range(len(imports)):
        oft = 0 if opts.get('no_oft') and k == 0 else ilts[k]
        iat = opts.get('iat_override', iats[k]) if k == 0 else iats[k]
        rd.put(u32(oft) + u32(0) + u32(0) + u32(opts.get('name_override', names[dll_names[k]]) if k == 0 else names[dll_names[k]]) + u32(iat), align=4)
    rd.here('imports_end', 4)
    if not opts.get('no_import_null'):
        rd.put(bytes(20), align=4)
    import_size = len(imports) * 20 + 20

    dname = rd.put(b'delayed.dll\0', align=2)
    dhn = rd.put(u16(3) + b'LateCall\0', align=2)
    dint = rd.put(u64(dhn) + u64(0x8000000000000009) + u64(0))
    diat = rd.put(bytes(24))
    delay_rva = rd.here('delay', 4)
    rd.put(u32(1) + u32(opts.get('delay_name', dname)) + u32(0) + u32(diat) + u32(opts.get('delay_int', dint)) + u32(0) + u32(0) + u32(0), align=4)
    rd.here('delay_end', 4)
    rd.put(bytes(32), align=4)

    exp_names = opts.get('export_names', [b'alpha', b'beta', b'gamma', b'fwd_name', b'fwd_ord'])
    exp_rva = rd.here('exports', 4)
    rd.put(bytes(40), align=4)
    dll_name = rd.put(b'synthetic.dll\0', align=1)
    fwd1 = rd.put(opts.get('fwd1', b'other.Target') + b'\0', align=1)
    fwd2 = rd.put(opts.get('fwd2', b'other.#42') + b'\0', align=1)
    name_rvas = [rd.put(n + b'\0', align=1) for n in exp_names]
    funcs = opts.get('export_funcs', [0x1000, 0, 0x1010, fwd1, fwd2, 0x1020])
    addr_table = rd.put(b''.join(u32(f) for f in funcs), align=4)
    ordinals = opts.get('export_ordinals', [0, 2, 5, 3, 4])
    order = sorted(range(len(exp_names)), key=lambda i: exp_names[i])
    name_table = rd.put(b''.join(u32(name_rvas[i]) for i in order), align=4)
    ord_table = rd.put(b''.join(u16(ordinals[i]) for i in order), align=2)
    exp_end = rd.here(None, 1)
    base = opts.get('export_base', 4)
    nfuncs = opts.get('export_nfuncs', len(funcs))
    nnames = opts.get('export_nnames', len(exp_names))
    rd.patch(exp_rva, u32(0) + u32(0) + u16(0) + u16(0) + u32(dll_name) + u32(base) + u32(nfuncs) + u32(nnames) +
             u32(opts.get('export_addr', addr_table)) + u32(opts.get('export_names_rva', name_table)) +
             u32(opts.get('export_ords_rva', ord_table)))
    exp_size = opts.get('export_size', exp_end - exp_rva)

    callbacks = opts.get('tls_callbacks', [IMAGE_BASE + 0x1008, IMAGE_BASE + 0x1018])
    cb_rva = rd.put(b''.join(u64(c) for c in callbacks) + (b'' if opts.get('tls_no_null') else u64(0)))
    tls_rva = rd.here('tls', 8)
    rd.put(u64(IMAGE_BASE + 0x3000) + u64(IMAGE_BASE + 0x3010) + u64(IMAGE_BASE + 0x3020) + u64(opts.get('tls_cb_va', IMAGE_BASE + cb_rva)) +
           u32(0) + u32(0))

    cv = b'RSDS' + bytes(range(16)) + u32(3) + opts.get('pdb', b'C:\\build\\synthetic.pdb') + (b'' if opts.get('pdb_no_nul') else b'\0')
    cv_rva = rd.put(opts.get('cv_override', cv), align=4)
    debug_rva = rd.here('debug', 4)
    extra = opts.get('debug_entries', [])
    for e in [(2, None)] + extra:
        typ, sz = e
        rd.put(u32(0) + u32(0) + u16(0) + u16(0) + u32(typ) + u32(sz if sz is not None else len(cv)) + u32(cv_rva) +
               u32(0x400 + 0x200 + (cv_rva - 0x2000) if not opts.get('cv_bad_ptr') else 0x7FFFFFF0), align=4)
    debug_size = opts.get('debug_size', 28 * (1 + len(extra)))
    rd.here('debug_end', 4)
    rd.put(bytes(opts.get('tail_pad', 0)), align=1)
    data = Section(0x3000)
    data.put(bytes(0x40))
    vsize = opts.get('rdata_vsize', len(rd.data))
    if 'rdata_end_at' in opts:
        place, extra_bytes = opts['rdata_end_at']
        vsize = rd.at[place] - 0x2000 + extra_bytes
    img.sections = [
        (opts.get('text_name', b'.text'), 0x1000, 0x40, bytes(text.data), 0x60000020),
        (opts.get('rdata_name', b'.rdata'), 0x2000, vsize, bytes(rd.data), 0x40000040),
        (b'.data', 0x3000, 0x40, bytes(data.data), 0xC0000040),
    ]
    img.dirs = list(img.dirs)
    img.dirs[1] = (opts.get('import_rva', desc_rva), import_size)
    img.dirs[0] = (opts.get('export_dir_rva', exp_rva), exp_size)
    img.dirs[9] = (opts.get('tls_dir_rva', tls_rva), 40)
    img.dirs[6] = (debug_rva, debug_size)
    img.dirs[13] = (opts.get('delay_rva', delay_rva), 64)
    img.dirs[3] = (0x1000, opts.get('exception_size', 0x10))
    img.dirs[5] = (0x3000, 0x10)
    img.dirs[10] = (0x3010, opts.get('load_config_size', 0x10))
    for k, v in opts.get('dirs', {}).items():
        img.dirs[k] = v
    for k in ('lfanew', 'machine', 'characteristics', 'opt_magic', 'opt_size', 'rva_count', 'subsystem', 'entry'):
        if k in opts:
            setattr(img, k, opts[k])
    if 'sections' in opts:
        img.sections = opts['sections'](img.sections)
    b = bytearray(img.build())
    for at, patch in opts.get('patches', []):
        b[at:at + len(patch)] = patch
    if 'truncate' in opts:
        b = b[:opts['truncate']]
    return bytes(b)


def images():
    out = {}
    out['ok'] = standard()
    out['dll'] = standard(characteristics=0x2022, entry=0)
    out['tiny-0'] = b''
    out['tiny-15'] = b'MZ' + bytes(13)
    out['tiny-63'] = b'MZ' + bytes(61)
    out['mz-garbage'] = b'MZ' + bytes(58) + u32(0x10000) + bytes(60)
    out['elf'] = b'\x7fELF\x02' + bytes(60)
    out['archive-magic'] = b'!<arch>\n' + bytes(60)
    out['coff-object'] = u16(0x8664) + bytes(62)
    out['coff-import'] = b'\0\0\xff\xff\0\0' + bytes(58)
    out['anon-object'] = b'\0\0\xff\xff\x02\0' + bytes(58)
    out['bad-pe-magic'] = standard(patches=[(0x80, b'PX')])
    out['pe32'] = standard(opt_magic=0x10B)
    out['rom-magic'] = standard(opt_magic=0x107)
    out['arm64'] = standard(machine=0xAA64)
    out['i386-in-pe64'] = standard(machine=0x14C)
    out['opt-too-small'] = standard(opt_size=100)
    out['opt-too-big'] = standard(opt_size=0xFFFF)
    out['rva-count-big'] = standard(rva_count=100)
    out['rva-count-6'] = standard(rva_count=6)
    out['rva-count-0'] = standard(rva_count=0)
    out['lfanew-odd'] = standard(lfanew=0x81)
    out['lfanew-past-end'] = standard(lfanew=0x7FFFFFF0)
    out['sections-many'] = standard(patches=[(0x86, u16(0xFFFF))])
    out['sections-none'] = standard(patches=[(0x86, u16(0))])
    out['truncated-200'] = standard(truncate=0x200)
    out['truncated-headers'] = standard(truncate=0x400)
    out['truncated-rdata'] = standard(truncate=0x800)
    out['import-outside'] = standard(import_rva=0x9000)
    out['import-in-text'] = standard(import_rva=0x1010)
    out['import-no-null'] = standard(no_import_null=True)
    out['import-no-oft'] = standard(no_oft=True)
    out['import-thunk-no-null'] = standard(no_thunk_null=True)
    out['import-empty-name'] = standard(imports=[[(1, b'')], [(0, b'X')]])
    out['import-ordinals-only'] = standard(imports=[['o1', 'o65535'], ['o7']])
    out['import-utf8'] = standard(dlls=[b'k\xe9rnel.dll', b'\xf0\x9f\x98\x80.dll'],
                                  imports=[[(1, b'na\xc3\xafve'), (2, b'bad\xc3'), (3, b'\xe0\x80x'), (4, b'\xed\xa0\x80'), (5, b'\xf4\x90\x80\x80'),
                                            (6, b'\xf0\x9f\x98')], [(0, b'\xff\xfe')]])
    out['iat-outside'] = standard(iat_override=0xFFFFFFF8)
    out['iat-at-end'] = standard(iat_override=0x3FF8)
    out['iat-zero'] = standard(iat_override=0)
    out['import-name-outside'] = standard(name_override=0x9000)
    out['import-name-in-text'] = standard(name_override=0x1004)
    out['delay-outside'] = standard(delay_rva=0x8000)
    out['export-outside'] = standard(export_dir_rva=0x8000)
    out['export-size-big'] = standard(export_size=0x10000)
    out['export-size-small'] = standard(export_size=20)
    out['export-base-big'] = standard(export_base=0x10000)
    out['export-base-max'] = standard(export_base=0xFFFF)
    out['export-nfuncs-big'] = standard(export_nfuncs=0x10001)
    out['export-addr-out'] = standard(export_addr=0x2FF0)
    out['export-no-ords'] = standard(export_ords_rva=0)
    out['export-names-out'] = standard(export_names_rva=0x5000)
    out['export-ord-out'] = standard(export_ordinals=[0, 2, 5, 3, 40])
    out['export-dup-names'] = standard(export_ordinals=[0, 0, 5, 3, 4])
    out['export-fwd-nodot'] = standard(fwd1=b'otherTarget')
    out['export-fwd-hash'] = standard(fwd2=b'other.#')
    out['export-fwd-hash-alpha'] = standard(fwd2=b'other.#4x')
    out['export-fwd-ord-big'] = standard(fwd2=b'other.#70000')
    out['export-fwd-empty'] = standard(fwd1=b'other.')
    out['export-fwd-dots'] = standard(fwd1=b'a.b.c')
    out['export-gaps'] = standard(export_funcs=[0, 0, 0x1010, 0, 0, 0x1020])
    out['export-no-names'] = standard(export_nnames=0)
    out['tls-outside'] = standard(tls_dir_rva=0x8000)
    out['tls-no-callbacks'] = standard(tls_cb_va=0)
    out['tls-cb-below-base'] = standard(tls_cb_va=0x1000)
    out['tls-cb-far'] = standard(tls_cb_va=IMAGE_BASE + 0x100000000)
    out['tls-cb-outside'] = standard(tls_cb_va=IMAGE_BASE + 0x8000)
    out['tls-cb-bad-entry'] = standard(tls_callbacks=[IMAGE_BASE + 0x1008, 0x10])
    out['tls-many'] = standard(tls_callbacks=[IMAGE_BASE + 0x1000 + k for k in range(1100)])
    out['debug-size-odd'] = standard(debug_size=30)
    out['debug-two'] = standard(debug_entries=[(2, None)])
    out['debug-other-type'] = standard(debug_entries=[(13, 4)])
    out['cv-bad-ptr'] = standard(cv_bad_ptr=True)
    out['cv-not-rsds'] = standard(cv_override=b'NB10' + bytes(20))
    out['cv-short-guid'] = standard(cv_override=b'RSDS' + bytes(10))
    out['cv-no-age'] = standard(cv_override=b'RSDS' + bytes(18))
    out['cv-no-nul'] = standard(pdb_no_nul=True, debug_entries=[])
    out['cv-utf8'] = standard(pdb=b'C:\\b\xc3\xa9\xff.pdb')
    out['reloc-stripped'] = standard(characteristics=0x0023)
    out['no-exception'] = standard(exception_size=0)
    out['no-load-config'] = standard(load_config_size=0)
    out['section-name-8'] = standard(text_name=b'.textbss')
    out['section-name-bytes'] = standard(text_name=b'.t\xffx\xe2\x82')
    out['section-raw-far'] = standard(sections=lambda s: [(s[0][0], s[0][1], s[0][2], s[0][3], s[0][4])] + s[1:],
                                      patches=[(0x80 + 24 + 240 + 20, u32(0xFFFFFFF0))])
    out['section-vsize-0'] = standard(sections=lambda s: [(s[0][0], s[0][1], 0, s[0][3], s[0][4])] + s[1:])
    out['section-overlap'] = standard(sections=lambda s: [(b'.dup', 0x2000, 0x10, bytes(0x10), 0x40000040)] + s)
    out['import-null-cut'] = standard(no_import_null=True, rdata_end_at=('imports_end', 0))
    out['import-null-half'] = standard(rdata_end_at=('imports_end', 10))
    out['delay-null-cut'] = standard(rdata_end_at=('delay_end', 0))
    out['delay-name-out'] = standard(delay_name=0x9000)
    out['delay-int-out'] = standard(delay_int=0x9000)
    out['delay-int-text'] = standard(delay_int=0x1000)
    out['tls-truncated'] = standard(rdata_end_at=('tls', 16), dirs={6: (0, 0)})
    out['debug-size-odd-room'] = standard(debug_size=30, tail_pad=64)
    out['debug-cut'] = standard(rdata_end_at=('debug_end', -10))
    out['cv-sig-short'] = standard(cv_override=b'RS')
    out['nt-headers-short'] = standard(truncate=0x80 + 128)
    out['nt-headers-sig-only'] = standard(truncate=0x80 + 4)
    out['export-name-ptr-out'] = standard(export_names_rva=0x2000)
    for n in range(0x180, 0xA00, 0x40):
        out[f'cut-{n:x}'] = standard(truncate=n)
    return out


def member(name, data, date=b'0', mode=b'644', size=None, terminator=b'`\n'):
    h = name.ljust(16, b' ')[:16] + date.ljust(12, b' ') + b'0'.ljust(6, b' ') + b'0'.ljust(6, b' ') + mode.ljust(8, b' ')
    h += (size if size is not None else str(len(data)).encode()).ljust(10, b' ')[:10] + terminator
    body = data + (b'\n' if len(data) % 2 else b'')
    return h + body


def import_object(symbol, dll, name_type=1, typ=0, ordinal=0, export=None, version=0, sig2=0xFFFF, machine=0x8664):
    data = symbol + b'\0' + dll + b'\0' + (export + b'\0' if export is not None else b'')
    return u16(0) + u16(sig2) + u16(version) + u16(machine) + u32(0) + u32(len(data)) + u16(ordinal) + u16(typ | name_type << 2) + data


def objects():
    return [
        import_object(b'Sleep', b'KERNEL32.dll'),
        import_object(b'_GetTickCount@0', b'KERNEL32.dll', name_type=3),
        import_object(b'?Decorated@@YAXXZ', b'other.dll', name_type=2),
        import_object(b'@Fast@8', b'other.dll', name_type=3),
        import_object(b'ByOrdinal', b'ord.dll', name_type=0, ordinal=17),
        import_object(b'Shown', b'exp.dll', name_type=4, export=b'RealName'),
        import_object(b'Shown2', b'exp.dll', name_type=4, export=b''),
        import_object(b'DataItem', b'KERNEL32.dll', typ=1),
        import_object(b'ConstItem', b'KERNEL32.dll', typ=2),
        import_object(b'BadType', b'KERNEL32.dll', typ=3),
        import_object(b'BadName', b'KERNEL32.dll', name_type=5),
        import_object(b'Sleep', b'KERNEL32.dll'),
        import_object(b'Ver', b'KERNEL32.dll', version=1),
        import_object(b'Sig', b'KERNEL32.dll', sig2=0xFFFE),
        import_object(b'N\xe9', b'u\xff.dll'),
        u16(0) + u16(0xFFFF) + u16(0) + u16(0x8664) + u32(0) + u32(100) + u16(0) + u16(4) + b'short',
        u16(0) + u16(0xFFFF) + u16(0) + u16(0x8664) + u32(0) + u32(6) + u16(0) + u16(4) + b'NoDll\0',
        u16(0) + u16(0xFFFF) + u16(0) + u16(0x8664) + u32(0) + u32(12) + u16(0) + u16(16) + b'Sym\0dll.dll\0',
        b'\0\0\xff\xff',
        u16(0x8664) + bytes(40),
    ]


def archives():
    obj = objects()
    out = {}
    longnames = b'a-very-long-member-name.obj/\nanother-long-name.obj/\n'
    gnu = b'!<arch>\n' + member(b'/', u32(0)) + member(b'//', longnames)
    for k, o in enumerate(obj):
        gnu += member(b'/0' if k % 2 else f'm{k}.o/'.encode(), o)
    out['gnu'] = gnu
    coff = b'!<arch>\n' + member(b'/', u32(0)) + member(b'/', u32(0)) + member(b'//', longnames) + member(b'/<ECSYMBOLS>/', b'') + b''.join(
        member(f'obj{k}/'.encode(), o) for k, o in enumerate(obj))
    out['coff'] = coff
    out['coff-no-names'] = b'!<arch>\n' + member(b'/', u32(0)) + member(b'/', u32(0)) + b''.join(member(b'x.obj/', o) for o in obj)
    out['sym64'] = b'!<arch>\n' + member(b'/SYM64/', u64(0)) + member(b'//', longnames) + b''.join(member(b'/29', o) for o in obj)
    out['names-first'] = b'!<arch>\n' + member(b'//', longnames) + b''.join(member(b'/0', o) for o in obj)
    out['bsd'] = b'!<arch>\n' + member(b'__.SYMDEF', u32(0)) + b''.join(
        member(b'#1/12', b'bsdname.o\0\0\0' + o) for o in obj)
    out['bsd-sorted'] = b'!<arch>\n' + member(b'__.SYMDEF SORTED', u32(0)) + member(b'#1/3', b'abc' + obj[0])
    out['plain'] = b'!<arch>\n' + b''.join(member(b'p.o/', o) for o in obj)
    out['empty'] = b'!<arch>\n'
    out['thin'] = b'!<thin>\n' + member(b'/', u32(0)) + member(b'//', longnames) + member(b'/0', obj[0])[:60] + member(b'x.o/', obj[1])[:60]
    out['bad-magic'] = b'!<arxh>\n' + member(b'x/', obj[0])
    out['short-magic'] = b'!<ar'
    out['bad-terminator'] = b'!<arch>\n' + member(b'x/', obj[0], terminator=b'XX')
    out['bad-size'] = b'!<arch>\n' + member(b'x/', obj[0], size=b'12a')
    out['space-size'] = b'!<arch>\n' + member(b'x/', obj[0], size=b' 12')
    out['huge-size'] = b'!<arch>\n' + member(b'x/', obj[0], size=b'9999999999')
    out['bad-longname'] = b'!<arch>\n' + member(b'//', b'abc') + member(b'/99', obj[0])
    out['longname-no-slash'] = b'!<arch>\n' + member(b'//', b'name\n') + member(b'/0', obj[0])
    out['longname-nul'] = b'!<arch>\n' + member(b'//', b'name\0') + member(b'/0', obj[0])
    out['bsd-too-long'] = b'!<arch>\n' + member(b'#1/99', b'abc')
    out['truncated-member'] = (b'!<arch>\n' + member(b'x/', obj[0]))[:-10]
    out['truncated-header'] = b'!<arch>\n' + member(b'x/', obj[0])[:30]
    out['odd-padding'] = b'!<arch>\n' + member(b'a/', obj[0] + b'z') + member(b'b/', obj[1])
    return out


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    paths = []
    for name, data in sorted(images().items()):
        p = os.path.join(out, f'img-{name}.bin')
        open(p, 'wb').write(data)
        paths.append(p)
    for name, data in sorted(archives().items()):
        p = os.path.join(out, f'ar-{name}.lib')
        open(p, 'wb').write(data)
        paths.append(p)
    open(os.path.join(out, 'list.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(paths) + '\n')
    print(f'{len(paths)} fixtures in {out}')


if __name__ == '__main__':
    main()
