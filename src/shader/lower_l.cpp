// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// buffer instructions. See lower_impl.hpp.
#include <algorithm>
#include <string>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

void Lower::buffer(const Inst& inst, std::size_t b, std::size_t i) {
    const auto& f = std::get<BufferFields>(inst.fields);
    auto name = inst.name();
    if (name == "buffer_gl0_inv" || name == "buffer_gl1_inv") {
        return;
    }
    if (f.lds || f.tfe) {
        fail("a buffer access to LDS or with tfe");
    }
    if (f.format && !name.starts_with("tbuffer_load_format_")) {
        fail(name.data());
    }
    Site site{b, i};
    std::optional<V> g = guard(b, i);
    std::size_t c = cur;
    bool store = name.starts_with("buffer_store_");
    std::optional<std::uint32_t> atomic;
    if (name.starts_with("buffer_atomic_")) {
        auto rest = name.substr(14);
        if (rest == "add") {
            atomic = 0;
        } else if (rest == "and") {
            atomic = 1;
        } else if (rest == "or") {
            atomic = 2;
        } else if (rest == "xor") {
            atomic = 3;
        } else if (rest == "smin") {
            atomic = 4;
        } else if (rest == "smax") {
            atomic = 5;
        } else if (rest == "umin") {
            atomic = 6;
        } else if (rest == "umax") {
            atomic = 7;
        } else if (rest == "swap") {
            atomic = 8;
        } else if (rest == "cmpswap") {
            atomic = UINT32_MAX;
        } else {
            fail(name.data());
        }
    }
    std::optional<std::pair<std::uint32_t, bool>> narrow;
    if (name == "buffer_load_ubyte" || name == "buffer_store_byte") {
        narrow = std::pair{8u, false};
    } else if (name == "buffer_load_sbyte") {
        narrow = std::pair{8u, true};
    } else if (name == "buffer_load_ushort" || name == "buffer_store_short") {
        narrow = std::pair{16u, false};
    } else if (name == "buffer_load_sshort") {
        narrow = std::pair{16u, true};
    }
    bool d16 = name.starts_with("buffer_") && name.find("_format_d16_") != std::string_view::npos;
    std::string plain(name);
    for (std::size_t at; (at = plain.find("_format_d16_")) != std::string::npos;) {
        plain.replace(at, 12, "_format_");
    }
    std::optional<std::string_view> rest;
    std::string_view pv = plain;
    if (pv.starts_with("buffer_load_")) {
        rest = pv.substr(12);
    } else if (pv.starts_with("buffer_store_")) {
        rest = pv.substr(13);
    } else if (pv.starts_with("tbuffer_load_")) {
        rest = pv.substr(13);
    }
    bool typed;
    std::uint32_t count;
    if (atomic || narrow) {
        typed = false;
        count = 1;
    } else if (rest == "dword") {
        typed = false;
        count = 1;
    } else if (rest == "dwordx2") {
        typed = false;
        count = 2;
    } else if (rest == "dwordx3") {
        typed = false;
        count = 3;
    } else if (rest == "dwordx4") {
        typed = false;
        count = 4;
    } else if (rest == "format_x") {
        typed = true;
        count = 1;
    } else if (rest == "format_xy") {
        typed = true;
        count = 2;
    } else if (rest == "format_xyz") {
        typed = true;
        count = 3;
    } else if (rest == "format_xyzw") {
        typed = true;
        count = 4;
    } else {
        fail(name.data());
    }
    if (f.addr.kind != Operand::Vector) {
        fail("a buffer address");
    }
    std::uint8_t areg = f.addr.a;
    auto vaddr = [&](std::uint8_t k) {
        auto v = inst.read(Reg::v(static_cast<std::uint8_t>(areg + k)));
        if (!v) {
            fail("a buffer address");
        }
        return i32_of(*v);
    };
    std::optional<V> index;
    if (f.idxen) {
        index = vaddr(0);
    }
    V off = s.m.i32(f.offset);
    if (f.offen) {
        V o = vaddr(f.idxen ? 1 : 0);
        off = s.m.bin(c, Bin::Add, off, o);
    }
    if (!((f.soffset.kind == Operand::Scalar && f.soffset.a == NULL_CODE) || (f.soffset.kind == Operand::Const && f.soffset.a == 128))) {
        V so = operand(inst, f.soffset);
        off = s.m.bin(c, Bin::Add, off, so);
    }
    auto dit = descriptors_res.find(site);
    if (dit == descriptors_res.end()) {
        fail("an untracked buffer");
    }
    const Source& src = *dit->second;
    const UsedResource* used = used_kind(src);
    auto [base, extra] = built_offset(src);
    if (base != 0) {
        V bk = s.m.i32(base);
        off = s.m.bin(c, Bin::Add, off, bk);
    }
    if (extra) {
        off = s.m.bin(c, Bin::Add, off, *extra);
    }
    if (auto stride = slot_stride(src); stride && index && !typed && !used) {
        V at = s.m.bin(c, Bin::Mul, *index, *stride);
        off = s.m.bin(c, Bin::Add, at, off);
        index.reset();
    }
    if (index && used && !typed && used->kind != 2 && used->stride >= 4 && used->stride % 4 == 0) {
        V k = s.m.i32(used->stride);
        V at = s.m.bin(c, Bin::Mul, *index, k);
        off = s.m.bin(c, Bin::Add, at, off);
        index.reset();
    }
    if (used && used->kind == 2 && !typed) {
        V h = handle(site, src, false, {13, 0x10000});
        V byte = off;
        if (index) {
            V sixteen = s.m.i32(16);
            V rows = s.m.bin(c, Bin::Mul, *index, sixteen);
            byte = s.m.bin(c, Bin::Add, rows, off);
        }
        if (!f.data || f.data->kind != Operand::Vector) {
            fail("a buffer result");
        }
        std::uint8_t reg = f.data->a;
        for (std::uint32_t k = 0; k < count; ++k) {
            V four = s.m.i32(4 * k);
            V a = s.m.bin(c, Bin::Add, byte, four);
            V xv = cbuffer_dword(h, a);
            write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), xv, g);
        }
        return;
    }
    bool uav = store || atomic || (used && used->kind == 3);
    std::uint32_t u = uav ? 0x1000 : 0;
    V iu = s.m.undef(s.t.i32);
    if (narrow) {
        auto [bits, is_signed] = *narrow;
        V byte;
        if (!index) {
            byte = off;
        } else if (used && used->stride == 1) {
            byte = s.m.bin(c, Bin::Add, *index, off);
        } else {
            fail("a byte or 16-bit buffer access by index");
        }
        V h = handle(site, src, false, {11 | u, 0});
        V k3 = s.m.i32(3);
        V within = s.m.bin(c, Bin::And, byte, k3);
        V aligned = s.m.bin(c, Bin::Sub, byte, within);
        V shift = s.m.bin(c, Bin::Shl, within, k3);
        V four = s.m.i32(4);
        V next = s.m.bin(c, Bin::Add, aligned, four);
        V ones = s.m.i32(bits == 8 ? 0xff : 0xffff);
        V zero = s.m.i32(0);
        V k8 = s.m.i32(8);
        V straddles = s.m.cmp(c, pred::EQ, within, k3);
        if (!store) {
            V mask = s.m.i8(1);
            V align = s.m.i32(4);
            auto dword = [&](V at) {
                V r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {h, at, iu, mask, align});
                V fv = s.m.extract(c, r, 0);
                return as_int(fv);
            };
            V low = dword(aligned);
            V xv = s.m.bin(c, Bin::LShr, low, shift);
            if (bits == 16) {
                V high = dword(next);
                high = s.m.bin(c, Bin::Shl, high, k8);
                high = s.m.select(c, straddles, high, zero);
                xv = s.m.bin(c, Bin::Or, xv, high);
            }
            xv = s.m.bin(c, Bin::And, xv, ones);
            if (is_signed) {
                V up = s.m.i32(32 - bits);
                xv = s.m.bin(c, Bin::Shl, xv, up);
                xv = s.m.bin(c, Bin::AShr, xv, up);
            }
            if (!f.data || f.data->kind != Operand::Vector) {
                fail("a buffer result");
            }
            write(inst, Reg::v(f.data->a), xv, g);
            return;
        }
        if (!f.data) {
            fail("buffer store data");
        }
        V v = vgprs(inst, *f.data, 1)[0];
        v = s.m.bin(c, Bin::And, v, ones);
        V all = s.m.i32(UINT32_MAX);
        V clear = s.m.bin(c, Bin::Shl, ones, shift);
        V keep = s.m.bin(c, Bin::Xor, clear, all);
        V set = s.m.bin(c, Bin::Shl, v, shift);
        V keep_next = s.m.i32(0xffffff00u);
        V set_next = s.m.bin(c, Bin::LShr, v, k8);
        V andk = s.m.i32(1);
        V ork = s.m.i32(2);
        Ty i32t = s.t.i32;
        auto at = begin_guarded(g);
        for (const auto& [op, xv] : {std::pair{andk, keep}, std::pair{ork, set}}) {
            s.op(cur, "atomicBinOp.i32", Attr::NoUnwind, i32t, 78, {h, op, aligned, iu, iu, xv});
        }
        if (bits == 16) {
            auto second = begin_guarded(straddles);
            for (const auto& [op, xv] : {std::pair{andk, keep_next}, std::pair{ork, set_next}}) {
                s.op(cur, "atomicBinOp.i32", Attr::NoUnwind, i32t, 78, {h, op, next, iu, iu, xv});
            }
            end_guarded(second, {});
        }
        end_guarded(at, {});
        return;
    }
    if (atomic) {
        std::uint32_t op = *atomic;
        if (!f.data) {
            fail("buffer atomic data");
        }
        std::vector<V> vs = vgprs(inst, *f.data, op == UINT32_MAX ? 2 : 1);
        V h;
        std::array<V, 3> coords;
        if (index && used && used->stride == 1) {
            V byte = s.m.bin(c, Bin::Add, *index, off);
            h = handle(site, src, false, {11 | u, 0});
            coords = {byte, iu, iu};
        } else if (index) {
            std::uint32_t stride = used ? used->stride : 0;
            if (stride < 4 || stride % 4 != 0) {
                fail("a structured buffer of unknown stride");
            }
            h = handle(site, src, false, {12 | u, stride});
            coords = {*index, off, iu};
        } else {
            h = handle(site, src, false, {11 | u, 0});
            coords = {off, iu, iu};
        }
        if (f.data->kind != Operand::Vector) {
            fail("buffer atomic data");
        }
        std::uint8_t reg = f.data->a;
        bool returns = f.cache.glc;
        V old;
        auto rv = inst.read(Reg::v(reg));
        if (rv && returns) {
            old = i32_of(*rv);
        } else {
            old = s.m.i32(0);
        }
        auto at = begin_guarded(g);
        Ty i32t = s.t.i32;
        V r;
        if (op == UINT32_MAX) {
            std::vector<V> args{h};
            args.insert(args.end(), coords.begin(), coords.end());
            args.push_back(vs[1]);
            args.push_back(vs[0]);
            r = s.op(cur, "atomicCompareExchange.i32", Attr::NoUnwind, i32t, 79, args);
        } else {
            std::vector<V> args{h};
            args.push_back(s.m.i32(op));
            args.insert(args.end(), coords.begin(), coords.end());
            args.push_back(vs[0]);
            r = s.op(cur, "atomicBinOp.i32", Attr::NoUnwind, i32t, 78, args);
        }
        r = end_guarded(at, {{r, old}})[0];
        if (returns) {
            write(inst, Reg::v(reg), r, std::nullopt);
        }
        return;
    }
    if (store) {
        std::uint32_t nregs = d16 ? (count + 1) / 2 : count;
        if (!f.data) {
            fail("buffer store data");
        }
        std::vector<V> vs = vgprs(inst, *f.data, static_cast<std::uint8_t>(nregs));
        if (d16) {
            std::vector<V> wide;
            for (std::uint32_t k = 0; k < count; ++k) {
                V xv = vs[k / 2];
                if (k % 2 == 1) {
                    V k16 = s.m.i32(16);
                    xv = s.m.bin(c, Bin::LShr, xv, k16);
                }
                V fv = s.op(c, "legacyF16ToF32", Attr::ReadNone, s.t.f32, 131, {xv});
                wide.push_back(as_int(fv));
            }
            vs = std::move(wide);
        }
        std::vector<V> args;
        std::uint32_t op;
        const char* nm;
        if (typed) {
            if (count != 4 || f.offen || f.offset != 0) {
                fail("a typed buffer store of some channels or at an offset");
            }
            args.push_back(handle(site, src, false, {10 | u, 0x409}));
            V zero = s.m.i32(0);
            args.push_back(index ? *index : zero);
            args.push_back(iu);
            for (V xv : vs) {
                args.push_back(as_float(xv));
            }
            op = 69;
            nm = "bufferStore.f32";
        } else {
            if (index && used && used->stride == 1) {
                V byte = s.m.bin(c, Bin::Add, *index, off);
                args.push_back(handle(site, src, false, {11 | u, 0}));
                args.push_back(byte);
                args.push_back(iu);
            } else if (index) {
                std::uint32_t stride = used ? used->stride : 0;
                if (stride < 4 || stride % 4 != 0) {
                    fail("a structured buffer of unknown stride");
                }
                args.push_back(handle(site, src, false, {12 | u, stride}));
                args.push_back(*index);
                args.push_back(off);
            } else {
                args.push_back(handle(site, src, false, {11 | u, 0}));
                args.push_back(off);
                args.push_back(iu);
            }
            args.insert(args.end(), vs.begin(), vs.end());
            args.resize(3 + 4, iu);
            op = 140;
            nm = "rawBufferStore.i32";
        }
        args.push_back(s.m.i8(typed ? 0xF : static_cast<std::uint8_t>((1u << count) - 1)));
        if (!typed) {
            args.push_back(s.m.i32(4));
        }
        auto at = begin_guarded(g);
        s.op(cur, nm, Attr::NoUnwind, s.t.void_, op, args);
        end_guarded(at, {});
        return;
    }
    if (f.format) {
        if (index) {
            fail("a typed-format load by index");
        }
        V h = handle(site, src, false, {11 | u, 0});
        if (!f.data || f.data->kind != Operand::Vector) {
            fail("a buffer result");
        }
        std::uint8_t reg = f.data->a;
        auto xs = format_load(h, off, *f.format, count);
        for (std::size_t k = 0; k < xs.size(); ++k) {
            write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), xs[k], g);
        }
        return;
    }
    V r;
    if (typed) {
        if (f.offen || off.is(V::Inst) || f.offset != 0) {
            fail("a typed buffer load at an offset");
        }
        if (uav) {
            s.flags |= dxil::flags::TYPED_UAV_LOADS;
        }
        V h = handle(site, src, false, {10 | u, 0x409});
        V zero = s.m.i32(0);
        V idx = index ? *index : zero;
        r = s.op(c, "bufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 68, {h, idx, iu});
    } else {
        V mask = s.m.i8(static_cast<std::uint8_t>((1u << count) - 1));
        V align = s.m.i32(4);
        if (index && used && used->stride == 1) {
            V byte = s.m.bin(c, Bin::Add, *index, off);
            V h = handle(site, src, false, {11 | u, 0});
            r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {h, byte, iu, mask, align});
        } else if (index) {
            std::uint32_t stride = used ? used->stride : 0;
            if (stride < 4 || stride % 4 != 0) {
                fail("a structured buffer of unknown stride");
            }
            V h = handle(site, src, false, {12 | u, stride});
            r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {h, *index, off, mask, align});
        } else {
            V h = handle(site, src, false, {11 | u, 0});
            r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {h, off, iu, mask, align});
        }
    }
    if (!f.data || f.data->kind != Operand::Vector) {
        fail("a buffer result");
    }
    std::uint8_t reg = f.data->a;
    if (d16) {
        V k16 = s.m.i32(16);
        Ty i32t = s.t.i32;
        for (std::uint32_t k = 0; k < (count + 1) / 2; ++k) {
            std::optional<V> xv;
            std::uint32_t lo = 2 * k, hi = std::min(2 * k + 2, count);
            for (std::uint32_t ch = hi; ch-- > lo;) {
                V fv = s.m.extract(c, r, ch);
                V h = s.op(c, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {fv});
                if (xv) {
                    V hs = s.m.bin(c, Bin::Shl, *xv, k16);
                    xv = s.m.bin(c, Bin::Or, h, hs);
                } else {
                    xv = h;
                }
            }
            write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), *xv, g);
        }
        return;
    }
    for (std::uint32_t k = 0; k < count; ++k) {
        V xv = s.m.extract(c, r, k);
        xv = as_int(xv);
        write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), xv, g);
    }
}

}   
