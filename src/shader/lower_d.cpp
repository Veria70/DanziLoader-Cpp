// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// thread ids, the mesh prologue, lane reads, LDS, and the dispatch of one
// instruction. See lower_impl.hpp.
#include <algorithm>
#include <bit>
#include <cstdlib>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

Val Lower::thread_id(std::uint8_t c) {
    if (!console_lanes) {
        return compute_id("threadIdInGroup.i32", 95, c);
    }
    auto [tx, ty, tz] = *s.threads;
    (void)tz;
    std::size_t e = s.entry;
    Ty i32t = s.t.i32;
    V f;
    if (halves) {
        f = console_thread();
        s.flags |= dxil::flags::WAVE_OPS;
    } else {
        V flat = s.op(e, "flattenedThreadIdInGroup.i32", Attr::ReadNone, i32t, 96, {});
        V first = s.op(e, "waveReadLaneFirst.i32", Attr::NoUnwind, i32t, 118, {flat});
        std::uint32_t wave = *s.wave_size;
        V mask = s.m.i32(~(wave - 1));
        V base = s.m.bin(e, Bin::And, first, mask);
        V lane = lane_index();
        f = s.m.bin(e, Bin::Add, base, lane);
        s.flags |= dxil::flags::WAVE_OPS;
    }
    V xk = s.m.i32(tx);
    V yk = s.m.i32(ty);
    V xy = s.m.i32(tx * ty);
    switch (c) {
        case 0: return Val::i32(s.m.bin(e, Bin::URem, f, xk));
        case 1: {
            V t = s.m.bin(e, Bin::UDiv, f, xk);
            return Val::i32(s.m.bin(e, Bin::URem, t, yk));
        }
        default: return Val::i32(s.m.bin(e, Bin::UDiv, f, xy));
    }
}

void Lower::mesh_prologue(container::Mesh mesh) {
    std::uint32_t threads = (*s.threads)[0];
    if (mesh.topology != 2) {
        fail("a mesh shader not drawing triangles");
    }
    if (threads > 128 || mesh.max_vertices > 256 || mesh.max_primitives > 256) {
        fail("a mesh shader beyond the host's limits");
    }
    auto fits = [&](const container::Mesh& m) {
        return container::mesh_output_bytes(s.outputs, s.prims, m.max_vertices, m.max_primitives) <= container::MESH_OUTPUT_LIMIT;
    };
    if (!fits(mesh)) {
        mesh.max_vertices = std::min(mesh.max_vertices, threads);
        mesh.max_primitives = std::min(mesh.max_primitives, threads);
        char* v = nullptr;
        std::size_t n = 0;
        bool strict = _dupenv_s(&v, &n, "VICTORY_MESH_OUTPUT_STRICT") == 0 && v;
        std::free(v);
        if (!fits(mesh) && strict) {
            fail("a mesh shader whose outputs pass the host's 32 KiB");
        }
        s.mesh = mesh;
    }
    std::size_t e = s.entry;
    Ty void_t = s.t.void_;
    V vc = s.m.i32(mesh.max_vertices);
    V pc = s.m.i32(mesh.max_primitives);
    s.op(e, "setMeshOutputCounts", Attr::NoUnwind, void_t, 168, {vc, pc});
    V flat = mesh_thread();
    V z = s.m.i32(0);
    std::uint32_t passes = (mesh.max_primitives + std::max(threads, 1u) - 1) / std::max(threads, 1u);
    for (std::uint32_t pass = 0; pass < passes; ++pass) {
        V index;
        if (pass == 0) {
            index = flat;
        } else {
            V past = s.m.i32(pass * threads);
            index = s.m.bin(cur, Bin::Add, flat, past);
        }
        std::optional<V> g;
        if ((pass + 1) * threads > mesh.max_primitives) {
            g = s.m.cmp(cur, pred::IULT, index, pc);
        }
        auto at = begin_guarded(g);
        s.op(cur, "emitIndices", Attr::NoUnwind, void_t, 169, {index, z, z, z});
        end_guarded(at, {});
    }
}

V Lower::mesh_thread() {
    if (flat_) {
        return *flat_;
    }
    V f = s.op(s.entry, "flattenedThreadIdInGroup.i32", Attr::ReadNone, s.t.i32, 96, {});
    flat_ = f;
    return f;
}

std::uint32_t Lower::wave() const { return x.native->wave64 ? 64 : 32; }

V Lower::lane_index() {
    if (s.mesh) {
        V f = mesh_thread();
        V m = s.m.i32(wave() - 1);
        return s.m.bin(s.entry, Bin::And, f, m);
    }
    return s.op(cur, "waveGetLaneIndex", Attr::ReadOnly, s.t.i32, 111, {});
}

V Lower::quad_pick(V xv, V srcv) {
    s.flags |= dxil::flags::WAVE_OPS;
    std::size_t c = cur;
    V three = s.m.i32(3);
    V q = s.m.bin(c, Bin::And, srcv, three);
    V r = xv;
    for (std::uint32_t n = 0; n < 4; ++n) {
        V kn = s.m.i32(n);
        V read = s.op(c, "quadReadLaneAt.i32", Attr::NoUnwind, s.t.i32, 122, {xv, kn});
        V is = s.m.cmp(c, pred::EQ, q, kn);
        r = s.m.select(c, is, read, r);
    }
    return r;
}

std::pair<V, V> Lower::first_lane(V xv, std::optional<V> g) {
    std::size_t c = cur;
    Ty i32t = s.t.i32;
    s.flags |= dxil::flags::WAVE_OPS;
    V first = s.op(c, "waveReadLaneFirst.i32", Attr::NoUnwind, i32t, 118, {xv});
    if (!g) {
        return {first, s.m.boolean(true)};
    }
    Ty four = s.m.named_struct("dx.types.fouri32", {i32t, i32t, i32t, i32t});
    V r = s.op(c, "waveActiveBallot", Attr::NoUnwind, four, 116, {*g});
    V lo = s.m.extract(c, r, 0);
    V hi = s.m.extract(c, r, 1);
    V z = s.m.i32(0);
    V k32 = s.m.i32(32);
    V flo = s.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 32, {lo});
    V fhi = s.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 32, {hi});
    fhi = s.m.bin(c, Bin::Add, fhi, k32);
    V in_lo = s.m.cmp(c, pred::NE, lo, z);
    V in_hi = s.m.cmp(c, pred::NE, hi, z);
    V any = s.m.bin(c, Bin::Or, in_lo, in_hi);
    V lane = s.m.select(c, in_lo, flo, fhi);
    V me = s.op(c, "waveGetLaneIndex", Attr::ReadOnly, i32t, 111, {});
    lane = s.m.select(c, any, lane, me);
    V at = read_lane(xv, lane);
    return {s.m.select(c, any, at, first), any};
}

V Lower::read_lane(V xv, V lane) { return s.op(cur, "waveReadLaneAt.i32", Attr::NoUnwind, s.t.i32, 117, {xv, lane}); }

V Lower::float_of(Value v) {
    if (auto it = floats.find(v); it != floats.end()) {
        return it->second;
    }
    V xv = i32_of(v);
    return as_float(xv);
}

std::vector<V> Lower::vgprs(const Inst& inst, const Operand& op, std::uint8_t n) {
    if (op.kind != Operand::Vector) {
        fail("a vector operand");
    }
    std::vector<V> out;
    out.reserve(n);
    for (std::uint8_t k = 0; k < n; ++k) {
        auto v = inst.read(Reg::v(static_cast<std::uint8_t>(op.a + k)));
        if (!v) {
            fail("an unread operand");
        }
        out.push_back(i32_of(*v));
    }
    return out;
}

namespace {

std::string_view trim_suffix(std::string_view s, std::string_view suffix) {
    while (s.ends_with(suffix)) {
        s.remove_suffix(suffix.size());
    }
    return s;
}

std::uint32_t ds_width(std::string_view n) {
    auto at = n.rfind('_');
    std::string_view last = at == std::string_view::npos ? n : n.substr(at + 1);
    if (last == "b64") {
        return 2;
    }
    if (last == "b96") {
        return 3;
    }
    if (last == "b128") {
        return 4;
    }
    return 1;
}

}   

void Lower::ds(const Inst& inst, std::size_t b, std::size_t i) {
    const auto& f = std::get<DsFields>(inst.fields);
    if (f.gds) {
        fail("GDS");
    }
    auto name = inst.name();
    std::optional<V> g = guard(b, i);
    std::size_t c = cur;
    std::uint32_t offset = std::uint32_t{f.offset0} | std::uint32_t{f.offset1} << 8;
    std::optional<std::uint8_t> dst_reg;
    if (f.dst && f.dst->kind == Operand::Vector) {
        dst_reg = f.dst->a;
    }
    if (name == "ds_swizzle_b32") {
        bool quad = (offset & 0x8000) != 0 || ((offset & 0x1C) == 0x1C && (offset & 0x380) == 0 && (offset >> 10 & 0x1F) < 4);
        bool mesh_quad = s.mesh.has_value() && quad;
        if (!mesh_quad) {
            cross_lane(quad);
        }
        if (!f.addr) {
            fail("a swizzle source");
        }
        V xv = vgprs(inst, *f.addr, 1)[0];
        V lane = lane_index();
        V srcv;
        if (offset & 0x8000) {
            V three = s.m.i32(3);
            V one = s.m.i32(1);
            V sel = s.m.bin(c, Bin::And, lane, three);
            V sh = s.m.bin(c, Bin::Shl, sel, one);
            V pattern = s.m.i32(offset & 0xFF);
            V pick = s.m.bin(c, Bin::LShr, pattern, sh);
            pick = s.m.bin(c, Bin::And, pick, three);
            V not3 = s.m.i32(~3u);
            V base = s.m.bin(c, Bin::And, lane, not3);
            srcv = s.m.bin(c, Bin::Or, base, pick);
        } else {
            V andk = s.m.i32((offset & 0x1F) | ~0x1Fu);
            V ork = s.m.i32(offset >> 5 & 0x1F);
            V xork = s.m.i32(offset >> 10 & 0x1F);
            V t = s.m.bin(c, Bin::And, lane, andk);
            t = s.m.bin(c, Bin::Or, t, ork);
            srcv = s.m.bin(c, Bin::Xor, t, xork);
        }
        V r = mesh_quad ? quad_pick(xv, srcv) : read_lane(xv, srcv);
        if (!dst_reg) {
            fail("a swizzle result");
        }
        write(inst, Reg::v(*dst_reg), r, g);
        return;
    }
    if (name == "ds_bpermute_b32") {
        cross_lane(false);
        if (exchange && !exchange->small) {
            fail("a lane permute across two host waves");
        }
        if (!f.addr) {
            fail("a permute address");
        }
        V at = vgprs(inst, *f.addr, 1)[0];
        if (f.data.empty()) {
            fail("a permute source");
        }
        V xv = vgprs(inst, f.data[0], 1)[0];
        V o = s.m.i32(offset);
        at = s.m.bin(c, Bin::Add, at, o);
        V two = s.m.i32(2);
        V srcv = s.m.bin(c, Bin::LShr, at, two);
        V last = s.m.i32(wave() - 1);
        srcv = s.m.bin(c, Bin::And, srcv, last);
        if (g) {
            V z = s.m.i32(0);
            xv = s.m.select(c, *g, xv, z);
        }
        V r = read_lane(xv, srcv);
        if (!dst_reg) {
            fail("a permute result");
        }
        write(inst, Reg::v(*dst_reg), r, g);
        return;
    }
    if (s.lds_dwords == 0) {
        fail("LDS the program does not allocate");
    }
    if (!f.addr) {
        fail("an LDS address");
    }
    V a = vgprs(inst, *f.addr, 1)[0];
    auto dword = [&](std::uint32_t bytes) -> V {
        std::size_t cc = cur;
        V o = s.m.i32(bytes);
        V at = s.m.bin(cc, Bin::Add, a, o);
        V two = s.m.i32(2);
        V index = s.m.bin(cc, Bin::LShr, at, two);
        V lds = s.lds();
        return s.m.element(cc, lds, index);
    };
    auto pair_of = [&](std::string_view n) -> std::vector<std::pair<std::uint32_t, std::uint32_t>> {
        std::uint32_t w = ds_width(n);
        std::uint32_t scale = 4 * w * (n.find("st64") != std::string_view::npos ? 64 : 1);
        return {{std::uint32_t{f.offset0} * scale, w}, {std::uint32_t{f.offset1} * scale, w}};
    };
    std::string_view base = trim_suffix(trim_suffix(name, "_hi"), "_d16");
    std::optional<std::uint32_t> sub;
    if (base == "ds_read_u8" || base == "ds_read_i8" || base == "ds_write_b8") {
        sub = 8;
    } else if (base == "ds_read_u16" || base == "ds_read_i16" || base == "ds_write_b16") {
        sub = 16;
    }
    if (sub && !(base == name || (name.ends_with("_d16") && base.starts_with("ds_read")) || name.ends_with("_d16_hi"))) {
        sub.reset();
    }
    if (sub) {
        std::uint32_t bits = *sub;
        V pp = dword(offset);
        V o = s.m.i32(offset);
        V at = s.m.bin(c, Bin::Add, a, o);
        V three = s.m.i32(3);
        V byte = s.m.bin(c, Bin::And, at, three);
        V sh = s.m.bin(c, Bin::Shl, byte, three);
        V mask = s.m.i32((1u << bits) - 1);
        if (name.starts_with("ds_read")) {
            if (!dst_reg) {
                fail("an LDS read result");
            }
            std::uint8_t reg = *dst_reg;
            V w = s.m.load(c, pp);
            V xv = s.m.bin(c, Bin::LShr, w, sh);
            if (base.find("_i") != std::string_view::npos) {
                V k = s.m.i32(32 - bits);
                xv = s.m.bin(c, Bin::Shl, xv, k);
                xv = s.m.bin(c, Bin::AShr, xv, k);
            } else {
                xv = s.m.bin(c, Bin::And, xv, mask);
            }
            if (base != name) {
                V old;
                if (auto v = inst.read(Reg::v(reg))) {
                    old = i32_of(*v);
                } else {
                    old = s.m.i32(0);
                }
                V half = s.m.i32(0xFFFF);
                xv = s.m.bin(c, Bin::And, xv, half);
                V keep;
                if (name.ends_with("_hi")) {
                    V k16 = s.m.i32(16);
                    xv = s.m.bin(c, Bin::Shl, xv, k16);
                    keep = half;
                } else {
                    keep = s.m.i32(0xFFFF0000u);
                }
                old = s.m.bin(c, Bin::And, old, keep);
                xv = s.m.bin(c, Bin::Or, old, xv);
            }
            write(inst, Reg::v(reg), xv, g);
        } else {
            if (f.data.empty()) {
                fail("an LDS write");
            }
            V xv = vgprs(inst, f.data[0], 1)[0];
            if (name != base) {
                V k16 = s.m.i32(16);
                xv = s.m.bin(c, Bin::LShr, xv, k16);
            }
            xv = s.m.bin(c, Bin::And, xv, mask);
            xv = s.m.bin(c, Bin::Shl, xv, sh);
            V m = s.m.bin(c, Bin::Shl, mask, sh);
            V ones = s.m.i32(UINT32_MAX);
            V keep = s.m.bin(c, Bin::Xor, m, ones);
            auto at2 = begin_guarded(g);
            s.m.atomic(cur, Rmw::And, pp, keep);
            s.m.atomic(cur, Rmw::Or, pp, xv);
            end_guarded(at2, {});
        }
        return;
    }
    if (name.starts_with("ds_read")) {
        auto rest = name.substr(7);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> elements;
        if (rest.starts_with('2')) {
            elements = pair_of(name);
        } else if (rest == "_b32" || rest == "_b64" || rest == "_b96" || rest == "_b128") {
            elements = {{offset, ds_width(name)}};
        } else {
            fail("an LDS read of bytes or halves");
        }
        if (!dst_reg) {
            fail("an LDS read result");
        }
        std::uint8_t reg = *dst_reg;
        std::uint8_t n = 0;
        for (const auto& [off, w] : elements) {
            for (std::uint32_t k = 0; k < w; ++k) {
                V pp = dword(off + 4 * k);
                V xv = s.m.load(cur, pp);
                write(inst, Reg::v(static_cast<std::uint8_t>(reg + n)), xv, g);
                ++n;
            }
        }
        return;
    }
    if (name.starts_with("ds_write")) {
        auto rest = name.substr(8);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> elements;
        if (rest.starts_with('2')) {
            elements = pair_of(name);
        } else if (rest == "_b32" || rest == "_b64" || rest == "_b96" || rest == "_b128") {
            elements = {{offset, ds_width(name)}};
        } else {
            fail("an LDS write of bytes or halves");
        }
        std::vector<std::pair<std::uint32_t, V>> stores;
        for (std::size_t e = 0; e < elements.size(); ++e) {
            auto [off, w] = elements[e];
            if (e >= f.data.size()) {
                fail("an LDS write");
            }
            auto vs = vgprs(inst, f.data[e], static_cast<std::uint8_t>(w));
            for (std::size_t k = 0; k < vs.size(); ++k) {
                stores.emplace_back(off + 4 * static_cast<std::uint32_t>(k), vs[k]);
            }
        }
        auto at = begin_guarded(g);
        for (const auto& [off, xv] : stores) {
            V pp = dword(off);
            s.m.store(cur, pp, xv);
        }
        end_guarded(at, {});
        return;
    }
    if (!name.starts_with("ds_")) {
        fail("an LDS operation");
    }
    std::string op(name.substr(3));
    for (std::size_t at; (at = op.find("_rtn")) != std::string::npos;) {
        op.erase(at, 4);
    }
    std::optional<Rmw> rmw;
    if (op == "add_u32") {
        rmw = Rmw::Add;
    } else if (op == "sub_u32") {
        rmw = Rmw::Sub;
    } else if (op == "and_b32") {
        rmw = Rmw::And;
    } else if (op == "or_b32") {
        rmw = Rmw::Or;
    } else if (op == "xor_b32") {
        rmw = Rmw::Xor;
    } else if (op == "min_i32") {
        rmw = Rmw::Min;
    } else if (op == "max_i32") {
        rmw = Rmw::Max;
    } else if (op == "min_u32") {
        rmw = Rmw::UMin;
    } else if (op == "max_u32") {
        rmw = Rmw::UMax;
    } else if (op == "wrxchg_b32") {
        rmw = Rmw::Xchg;
    } else if (op == "cmpst_b32" || op == "max_f32" || op == "min_f32") {
    } else {
        fail("an LDS operation");
    }
    if (f.data.empty()) {
        fail("an LDS atomic");
    }
    V v0 = vgprs(inst, f.data[0], 1)[0];
    std::optional<V> v1;
    if (f.data.size() > 1) {
        v1 = vgprs(inst, f.data[1], 1)[0];
    }
    std::optional<Value> old_v;
    if (dst_reg) {
        old_v = inst.read(Reg::v(*dst_reg));
    }
    V outside;
    if (old_v) {
        std::optional<V> r;
        try {
            r = i32_of(*old_v);
        } catch (const WhyError&) {
        }
        V z = s.m.i32(0);
        outside = r ? *r : z;
    } else {
        outside = s.m.i32(0);
    }
    V pp = dword(offset);
    if (op == "max_f32" || op == "min_f32") {
        bool max = op == "max_f32";
        auto at = begin_guarded(g);
        V r = lds_float_min_max(pp, v0, max);
        r = end_guarded(at, {{r, outside}})[0];
        if (dst_reg) {
            write(inst, Reg::v(*dst_reg), r, std::nullopt);
        }
        return;
    }
    auto at = begin_guarded(g);
    V r;
    if (rmw) {
        r = s.m.atomic(cur, *rmw, pp, v0);
    } else {
        if (!v1) {
            fail("an LDS compare");
        }
        r = s.m.cmpxchg(cur, pp, v0, *v1);
    }
    r = end_guarded(at, {{r, outside}})[0];
    if (dst_reg) {
        write(inst, Reg::v(*dst_reg), r, std::nullopt);
    }
}

V Lower::lds_float_min_max(V pp, V data, bool max) {
    std::size_t pre = cur;
    V init = s.m.load(pre, pp);
    std::size_t lp = s.m.new_block();
    std::size_t ex = s.m.new_block();
    s.m.terminate(pre, Term::br(lp));
    V old = s.m.phi(lp, s.t.i32);
    cur = lp;
    V now = lds_float_pick(old, data, max);
    V prev = s.m.cmpxchg(lp, pp, old, now);
    V done = s.m.cmp(lp, pred::EQ, prev, old);
    s.m.set_phi(old, {{init, pre}, {prev, lp}});
    s.m.terminate(lp, Term::cond_br(done, ex, lp));
    cur = ex;
    return old;
}

V Lower::lds_float_pick(V mem, V data, bool max) {
    std::size_t c = cur;
    V abs_mask = s.m.i32(0x7FFFFFFF);
    V inf = s.m.i32(0x7F800000);
    V quiet = s.m.i32(0x400000);
    V zero = s.m.i32(0);
    auto nan = [&](V xv) {
        V m = s.m.bin(c, Bin::And, xv, abs_mask);
        return s.m.cmp(c, pred::IUGT, m, inf);
    };
    auto snan = [&](V xv) {
        V n = nan(xv);
        V q = s.m.bin(c, Bin::And, xv, quiet);
        V sig = s.m.cmp(c, pred::EQ, q, zero);
        return s.m.bin(c, Bin::And, n, sig);
    };
    auto key = [&](V xv) {
        V k31 = s.m.i32(31);
        V sign = s.m.bin(c, Bin::AShr, xv, k31);
        V flip = s.m.bin(c, Bin::And, sign, abs_mask);
        return s.m.bin(c, Bin::Xor, xv, flip);
    };
    V dk = key(data);
    V mk = key(mem);
    V take = s.m.cmp(c, max ? pred::SGT : pred::SLT, dk, mk);
    V r = s.m.select(c, take, data, mem);
    V dn = nan(data);
    V mn = nan(mem);
    r = s.m.select(c, mn, data, r);
    V both = s.m.bin(c, Bin::And, dn, mn);
    V t = s.m.boolean(true);
    V not_both = s.m.bin(c, Bin::Xor, both, t);
    V only_data = s.m.bin(c, Bin::And, dn, not_both);
    r = s.m.select(c, only_data, mem, r);
    V qm = s.m.bin(c, Bin::Or, mem, quiet);
    V sm = snan(mem);
    r = s.m.select(c, sm, qm, r);
    V qd = s.m.bin(c, Bin::Or, data, quiet);
    V sd = snan(data);
    return s.m.select(c, sd, qd, r);
}

void Lower::inst(std::size_t b, std::size_t i) {
    const Inst& in = p().blocks[b].insts[i];
    std::size_t offset = in.machine.offset;
    auto name = in.name();
    local_ok = name.starts_with("s_bcnt1_i32_") || name.starts_with("s_ff1_i32_") || name.starts_with("v_mbcnt_");
    switch (mask_data) {
        case MaskData::Console: mask_ok = true; break;
        case MaskData::Host:
            mask_ok = name.starts_with("s_bcnt1_i32_") || name.starts_with("s_ff1_i32_") || name.starts_with("v_mbcnt_") || name == "s_cmp_eq_u32" ||
                      name == "s_cmp_lg_u32" || name == "s_cmp_eq_u64" || name == "s_cmp_lg_u64";
            break;
        case MaskData::None: mask_ok = uniform_test(in); break;
    }
    bool is_ds = std::holds_alternative<DsFields>(in.fields);
    bool effect = std::holds_alternative<ExpFields>(in.fields) || (std::holds_alternative<FlatFields>(in.fields) && !name.starts_with("global_load_")) ||
                  name.find("store") != std::string_view::npos || name.find("atomic") != std::string_view::npos || name == "s_barrier" ||
                  (is_ds && !(name.starts_with("ds_read") || name.starts_with("ds_swizzle") || name.find("permute") != std::string_view::npos));
    if (split_lds && is_ds && !(name.starts_with("ds_swizzle") || name.find("permute") != std::string_view::npos) && (!lds_meet || lds_meet->first[b])) {
        V mode = s.m.i32(meeting_mode());
        s.op(cur, "barrier", Attr::NoDuplicate, s.t.void_, 80, {mode});
    }


    auto failed = [&](Why why) {
        if (effect) {
            stop_unsupported(offset, why);
        }
        for (const auto& [_, v] : in.writes) {
            vals[v] = Val::missing(why);
        }
    };
    try {
        if (std::holds_alternative<SopFields>(in.fields)) {
            if (Why why = sop(in)) {
                failed(why);
            }
        } else if (std::holds_alternative<SoppFields>(in.fields)) {
            if (name == "s_barrier" && no_barrier.contains(b)) {
            } else if (name == "s_barrier") {
                std::uint32_t mode = writes_uavs ? (1 | 2 | 8) : (1 | 8);
                bool waits;
                switch (::danzi::shader::lower::group_sync()) {
                    case 0: waits = false; break;
                    case 1: waits = !fence_only.contains(b); break;
                    default: waits = true; break;
                }
                if (!waits) {
                    mode &= ~1u;
                }
                V m = s.m.i32(mode);
                s.op(cur, "barrier", Attr::NoDuplicate, s.t.void_, 80, {m});
            }
        } else if (std::holds_alternative<SmemFields>(in.fields)) {
            if (Why why = smem(in, Site{b, i})) {
                failed(why);
            }
        } else if (std::holds_alternative<Valu>(in.fields) && name.starts_with("v_interp_")) {
            interp(in, Site{b, i});
        } else if (std::holds_alternative<Valu>(in.fields)) {
            valu(in, b, i);
        } else if (std::holds_alternative<VintrpFields>(in.fields)) {
            interp(in, Site{b, i});
        } else if (std::holds_alternative<ImageFields>(in.fields) && name.starts_with("image_bvh")) {
            try {
                bvh(in, b, i);
            } catch (const WhyError& e) {
                stop_unsupported(offset, e.why);
            }
            return;
        } else if (std::holds_alternative<ImageFields>(in.fields)) {
            image(in, b, i);
        } else if (std::holds_alternative<BufferFields>(in.fields)) {
            buffer(in, b, i);
        } else if (std::holds_alternative<ExpFields>(in.fields)) {
            export_(b, i);
        } else if (is_ds) {
            ds(in, b, i);
        } else if (std::holds_alternative<FlatFields>(in.fields) && name.starts_with("global_")) {
            flat(in, b, i);
        } else if (std::holds_alternative<FlatFields>(in.fields)) {
            fail("flat memory");
        }
    } catch (const WhyError& e) {
        failed(e.why);
    }
}

void Lower::lds_meeting() {
    V mode = s.m.i32(meeting_mode());
    s.op(cur, "barrier", Attr::NoDuplicate, s.t.void_, 80, {mode});
}

bool Lower::dead(Value v, std::vector<Value>& seen) const {
    if (stale.contains(v)) {
        return true;
    }
    if (mask_data != MaskData::Console && (st().masks.contains(v) || st().highs.contains(v))) {
        return true;
    }
    auto it = phi_at.find(v);
    if (it == phi_at.end()) {
        return false;
    }
    if (std::find(seen.begin(), seen.end(), v) != seen.end()) {
        return true;
    }
    seen.push_back(v);
    auto [b, i] = it->second;
    for (Value a : p().blocks[b].phis[i].args) {
        if (!dead(a, seen)) {
            return false;
        }
    }
    return true;
}

bool Lower::uniform_test(const Inst& inst) const {
    const ir::Program& prog = p();
    auto writer = [&](Value v) -> const Inst* {
        auto it = def_block.find(v);
        if (it == def_block.end()) {
            return nullptr;
        }
        for (const Inst& i : prog.blocks[it->second].insts) {
            for (const auto& w : i.writes) {
                if (w.second == v) {
                    return &i;
                }
            }
        }
        return nullptr;
    };
    auto scalar_of = [](const Inst& i, const Operand& op) -> std::optional<Value> {
        if (op.kind == Operand::Scalar && op.a != NULL_CODE && op.a != ir::EXEC) {
            return i.read(Reg::s(op.a));
        }
        return std::nullopt;
    };
    if (!inst.name().starts_with("s_bcnt1_i32_")) {
        return false;
    }
    auto* f = std::get_if<SopFields>(&inst.fields);
    if (!f || f->src.empty()) {
        return false;
    }
    auto sv = scalar_of(inst, f->src[0]);
    const Inst* xr = sv ? writer(*sv) : nullptr;
    if (!xr) {
        return false;
    }
    auto* xf = std::get_if<SopFields>(&xr->fields);
    if (!xf) {
        return false;
    }
    if (!(xr->name() == "s_xor_b64" || xr->name() == "s_xor_b32") ||
        !std::any_of(xf->src.begin(), xf->src.end(), [](const Operand& o) { return o.kind == Operand::Scalar && o.a == ir::EXEC; })) {
        return false;
    }
    const Inst* cmp = nullptr;
    for (const Operand& o : xf->src) {
        if (auto v = scalar_of(*xr, o)) {
            cmp = writer(*v);
            break;
        }
    }
    if (!cmp) {
        return false;
    }
    auto* cf = std::get_if<Valu>(&cmp->fields);
    if (!cf) {
        return false;
    }
    if (!(cmp->name() == "v_cmp_eq_u32" || cmp->name() == "v_cmp_eq_i32") || cf->sdwa || cf->dpp) {
        return false;
    }
    std::optional<Value> lanes;
    for (const Operand& o : cf->src) {
        if (o.kind == Operand::Vector) {
            lanes = cmp->read(Reg::v(o.a));
            if (lanes) {
                break;
            }
        }
    }
    const Inst* fl = nullptr;
    for (const Operand& o : cf->src) {
        if (auto v = scalar_of(*cmp, o)) {
            fl = writer(*v);
            break;
        }
    }
    if (!lanes || !fl || fl->name() != "v_readfirstlane_b32") {
        return false;
    }
    auto* g = std::get_if<Valu>(&fl->fields);
    return g && !g->src.empty() && g->src[0].kind == Operand::Vector && fl->read(Reg::v(g->src[0].a)) == lanes;
}

bool Lower::is_mask(std::optional<Value> v) const { return v && st().masks.contains(*v); }

}   
