// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// global loads, wave sections' values, the walk over the blocks, inputs,
// values, user data and title memory by address. See lower_impl.hpp.
#include <algorithm>
#include <bit>
#include <cstring>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

std::optional<std::uint8_t> interp_mode(Barycentric kind) {
    switch (kind) {
        case Barycentric::PerspectiveCenter: return std::uint8_t{2};
        case Barycentric::PerspectiveCentroid: return std::uint8_t{3};
        case Barycentric::PerspectiveSample: return std::uint8_t{6};
        case Barycentric::LinearCenter: return std::uint8_t{4};
        case Barycentric::LinearCentroid: return std::uint8_t{5};
        case Barycentric::LinearSample: return std::uint8_t{7};
        default: return std::nullopt;
    }
}

void Lower::flat(const Inst& inst, std::size_t b, std::size_t i) {
    const auto& f = std::get<FlatFields>(inst.fields);
    auto name = inst.name();
    if (auto it = scalar.find(Site{b, i}); it != scalar.end() && it->second.param == resource::MEMORY) {
        memory_flat(inst, b, i);
        return;
    }
    if (!name.starts_with("global_load_")) {
        fail("a global memory write");
    }
    auto rest = name.substr(12);
    std::uint32_t n;
    std::optional<std::pair<std::uint32_t, bool>> sub;
    if (rest == "dword") {
        n = 1;
    } else if (rest == "dwordx2") {
        n = 2;
    } else if (rest == "dwordx3") {
        n = 3;
    } else if (rest == "dwordx4") {
        n = 4;
    } else if (rest == "ubyte") {
        n = 1;
        sub = std::pair{8u, false};
    } else if (rest == "sbyte") {
        n = 1;
        sub = std::pair{8u, true};
    } else if (rest == "ushort") {
        n = 1;
        sub = std::pair{16u, false};
    } else if (rest == "sshort") {
        n = 1;
        sub = std::pair{16u, true};
    } else {
        fail(name.data());
    }
    auto it = scalar.find(Site{b, i});
    if (it == scalar.end()) {
        fail("an untracked global address");
    }
    Address a = it->second;
    std::optional<V> g = guard(b, i);
    if (!f.dst || f.dst->kind != Operand::Vector) {
        fail("a global load result");
    }
    std::uint8_t reg = f.dst->a;
    std::size_t c = cur;
    V off = s.m.i32(a.offset);
    if (a.dynamic) {
        V d = i32_of(*a.dynamic);
        off = s.m.bin(c, Bin::Add, off, d);
    }
    std::optional<V> h;
    if (a.param != resource::CODE) {
        Source src;
        src.kind = Source::Built;
        src.address = a;
        if (auto bias = root_bias(a.param)) {
            off = s.m.bin(c, Bin::Add, off, *bias);
        }
        h = handle(Site{b, i}, src, false, {11, 0});
    }
    auto dword = [&](V byte) -> V {
        if (!h) {
            return code_dword(byte);
        }
        std::size_t cc = cur;
        V u = s.m.undef(s.t.i32);
        V mask = s.m.i8(1);
        V al = s.m.i32(4);
        V r = s.op(cc, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {*h, byte, u, mask, al});
        V xx = s.m.extract(cc, r, 0);
        return as_int(xx);
    };
    for (std::uint32_t k = 0; k < n; ++k) {
        V xv;
        if (!sub) {
            V four = s.m.i32(4 * k);
            V at = s.m.bin(c, Bin::Add, off, four);
            xv = dword(at);
        } else {
            auto [width, is_signed] = *sub;
            V not3 = s.m.i32(~3u);
            V three = s.m.i32(3);
            V at = s.m.bin(c, Bin::And, off, not3);
            V w = dword(at);
            V byte = s.m.bin(c, Bin::And, off, three);
            V sh = s.m.bin(c, Bin::Shl, byte, three);
            V xs = s.m.bin(c, Bin::LShr, w, sh);
            V left = s.m.i32(32 - width);
            xs = s.m.bin(c, Bin::Shl, xs, left);
            xv = s.m.bin(c, is_signed ? Bin::AShr : Bin::LShr, xs, left);
        }
        write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), xv, g);
    }
}

V Lower::shape(const wave::Shape& sh, bool own) {
    using wave::Op;
    Op op;
    Value src, mask;
    std::uint32_t ident;
    std::optional<bool> prefix;
    switch (sh.kind) {
        case wave::Shape::K: return s.m.i32(sh.k);
        case wave::Shape::Value: return i32_of(sh.a);
        case wave::Shape::Active:
            op = sh.op;
            src = sh.a;
            mask = sh.b;
            ident = sh.ident;
            break;
        case wave::Shape::ActiveF64: return active_f64_sum(sh.a, sh.b, sh.c, sh.flag);
        case wave::Shape::Masked: {
            std::size_t c = cur;
            auto it = st().masks.find(sh.b);
            if (it == st().masks.end()) {
                fail("a wave reduction's untracked mask");
            }
            V on = boolean(it->second, c);
            V xv = i32_of(sh.a);
            V id = s.m.i32(sh.ident);
            return s.m.select(c, on, xv, id);
        }
        case wave::Shape::Prefix:
            op = sh.op;
            src = sh.a;
            mask = sh.b;
            ident = sh.ident;
            prefix = sh.flag;
            break;
        default: fail("a wave shape");
    }
    if (mask_data == MaskData::None &&
        (prefix || !(op == Op::MinU || op == Op::MaxU || op == Op::MinI || op == Op::MaxI || op == Op::MinF || op == Op::MaxF))) {
        fail("a wave reduction in a mesh shader");
    }
    std::size_t c = cur;
    auto it = st().masks.find(mask);
    if (it == st().masks.end()) {
        fail("a wave reduction's untracked mask");
    }
    V on = boolean(it->second, c);
    V xv = i32_of(src);
    V id = s.m.i32(ident);
    xv = s.m.select(c, on, xv, id);
    s.flags |= dxil::flags::WAVE_OPS;
    if (op == Op::AddH) {
        if (prefix) {
            fail("a wave prefix of an operation other than a sum");
        }
        return half_sum(xv);
    }
    bool is_float = op == Op::AddF || op == Op::MinF || op == Op::MaxF;
    Ty i32t = s.t.i32, f32t = s.t.f32;
    V arg = is_float ? as_float(xv) : xv;
    Ty ty = is_float ? f32t : i32t;
    std::string sfx = is_float ? "f32" : "i32";
    V r;
    if (prefix) {
        if (!(op == Op::AddU || op == Op::AddF)) {
            fail("a wave prefix of an operation other than a sum");
        }
        V k0 = s.m.i8(0);
        V k1 = s.m.i8(1);
        r = s.op(c, "wavePrefixOp." + sfx, Attr::NoUnwind, ty, 121, {arg, k0, k1});
        if (*prefix) {
            r = s.m.bin(c, Bin::Add, r, arg);
        }
    } else if (op == Op::Or || op == Op::And || op == Op::Xor) {
        V k = s.m.i8(op == Op::And ? 0 : op == Op::Or ? 1 : 2);
        r = s.op(c, "waveActiveBit.i32", Attr::NoUnwind, i32t, 120, {arg, k});
    } else {
        std::uint8_t code, uns;
        switch (op) {
            case Op::AddU:
                code = 0;
                uns = 1;
                break;
            case Op::AddF:
                code = 0;
                uns = 0;
                break;
            case Op::MinU:
                code = 2;
                uns = 1;
                break;
            case Op::MaxU:
                code = 3;
                uns = 1;
                break;
            case Op::MinI:
            case Op::MinF:
                code = 2;
                uns = 0;
                break;
            default:
                code = 3;
                uns = 0;
                break;
        }
        V k = s.m.i8(code);
        V u = s.m.i8(uns);
        r = s.op(c, "waveActiveOp." + sfx, Attr::NoUnwind, ty, 119, {arg, k, u});
    }
    if (local || (exchange && exchange->small)) {
    } else if (own && !prefix && op == Op::Or && !all_lanes(cur_ir)) {
    } else if (exchange) {
        if (!all_lanes(cur_ir)) {
            need(cur_ir);
            fail("a wave reduction where not every lane runs");
        }
        auto bits = [&](V v) { return is_float ? as_int(v) : v; };
        auto back = [&](V v) { return is_float ? as_float(v) : v; };
        if (prefix) {
            V k0 = s.m.i8(0);
            V ku = s.m.i8(is_float ? 0 : 1);
            V total = s.op(c, "waveActiveOp." + sfx, Attr::NoUnwind, ty, 119, {arg, k0, ku});
            V tb = bits(total);
            std::size_t cc = cur;
            auto [lo, _] = exchange_halves(cc, tb);
            lo = back(lo);
            V cl = console_lane();
            V k32 = s.m.i32(32);
            V upper = s.m.cmp(cc, pred::IUGE, cl, k32);
            V zero = is_float ? s.m.f32(0) : s.m.i32(0);
            V add = s.m.select(cc, upper, lo, zero);
            r = s.m.bin(cc, Bin::Add, r, add);
        } else {
            V rb = bits(r);
            std::size_t cc = cur;
            auto [lo, hi] = exchange_halves(cc, rb);
            lo = back(lo);
            hi = back(hi);
            switch (op) {
                case Op::AddU:
                case Op::AddF: r = s.m.bin(cc, Bin::Add, lo, hi); break;
                case Op::Or: r = s.m.bin(cc, Bin::Or, lo, hi); break;
                case Op::And: r = s.m.bin(cc, Bin::And, lo, hi); break;
                case Op::Xor: r = s.m.bin(cc, Bin::Xor, lo, hi); break;
                default: {
                    std::uint32_t code;
                    switch (op) {
                        case Op::MaxF: code = 35; break;
                        case Op::MinF: code = 36; break;
                        case Op::MinU: code = 40; break;
                        case Op::MaxU: code = 39; break;
                        case Op::MinI: code = 38; break;
                        default: code = 37; break;
                    }
                    r = s.op(cc, "binary." + sfx, Attr::ReadNone, ty, code, {lo, hi});
                }
            }
        }
    }
    return is_float ? as_int(r) : r;
}

V Lower::active_f64_sum(Value lo_v, Value hi_v, Value mask, bool high) {
    if (mask_data == MaskData::None) {
        fail("a wave reduction in a mesh shader");
    }
    std::size_t c = cur;
    auto it = st().masks.find(mask);
    if (it == st().masks.end()) {
        fail("a wave reduction's untracked mask");
    }
    V on = boolean(it->second, c);
    V lo = i32_of(lo_v);
    V hi = i32_of(hi_v);
    V z = s.m.i32(0);
    lo = s.m.select(c, on, lo, z);
    hi = s.m.select(c, on, hi, z);
    s.flags |= dxil::flags::WAVE_OPS | dxil::flags::DOUBLES;
    Ty f64t = s.m.double_();
    Ty i32t = s.t.i32;
    Ty split_ty = s.m.named_struct("dx.types.splitdouble", {i32t, i32t});
    V arg = s.op(c, "makeDouble.f64", Attr::ReadNone, f64t, 101, {lo, hi});
    V k0 = s.m.i8(0);
    V k0b = s.m.i8(0);
    V sum = s.op(c, "waveActiveOp.f64", Attr::NoUnwind, f64t, 119, {arg, k0, k0b});
    V split = s.op(c, "splitDouble.f64", Attr::ReadNone, split_ty, 102, {sum});
    V r_lo = s.m.extract(c, split, 0);
    V r_hi = s.m.extract(c, split, 1);
    if (exchange) {
        if (!all_lanes(cur_ir)) {
            need(cur_ir);
            fail("a wave reduction where not every lane runs");
        }
        auto [lo0, lo1] = exchange_halves(cur, r_lo);
        auto [hi0, hi1] = exchange_halves(cur, r_hi);
        std::size_t cc = cur;
        V d0 = s.op(cc, "makeDouble.f64", Attr::ReadNone, f64t, 101, {lo0, hi0});
        V d1 = s.op(cc, "makeDouble.f64", Attr::ReadNone, f64t, 101, {lo1, hi1});
        V total = s.m.bin(cc, Bin::Add, d0, d1);
        V sp = s.op(cc, "splitDouble.f64", Attr::ReadNone, split_ty, 102, {total});
        r_lo = s.m.extract(cc, sp, 0);
        r_hi = s.m.extract(cc, sp, 1);
    }
    return high ? r_hi : r_lo;
}

V Lower::add_halves(V a, V b) {
    std::size_t c = cur;
    Ty f32t = s.t.f32;
    V low = s.m.i32(0xFFFF);
    auto wide = [&](V xv) {
        V h = s.m.bin(c, Bin::And, xv, low);
        return s.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {h});
    };
    V fa = wide(a);
    V fb = wide(b);
    V f = s.m.bin(c, Bin::Add, fa, fb);
    return f16_nearest(f);
}

V Lower::half_sum_here(V xv) {
    std::size_t c = cur;
    Ty i32t = s.t.i32, f32t = s.t.f32;
    s.flags |= dxil::flags::WAVE_OPS;
    V low = s.m.i32(0xFFFF);
    V h = s.m.bin(c, Bin::And, xv, low);
    V f = s.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {h});
    V k = s.m.i8(0);
    V sum = s.op(c, "waveActiveOp.f32", Attr::NoUnwind, f32t, 119, {f, k, k});
    return s.op(c, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {sum});
}

V Lower::half_sum(V xv) {
    static constexpr Why WHERE = "a wave sum of 16-bit floats where not every lane runs";
    if (!s.threads) {
        fail("a wave sum of 16-bit floats outside compute");
    }
    auto threads = *s.threads;
    if ((threads[0] * threads[1] * threads[2]) % 64 != 0) {
        fail("a wave sum of 16-bit floats in a wave that is not full");
    }
    if (local) {
        fail("a wave sum of 16-bit floats in a half-wave");
    }
    bool halves_;
    if (exchange && !exchange->small) {
        halves_ = true;
    } else if (!exchange && s.wave_size == std::optional<std::uint32_t>(64) && p().wave64) {
        halves_ = false;
    } else {
        fail("a wave sum of 16-bit floats on these waves");
    }
    bool every;
    if (halves_) {
        every = all_lanes(cur_ir);
    } else {
        auto all = all_lane_blocks(st());
        every = all && cur_ir < all->size() && (*all)[cur_ir];
    }
    if (!every) {
        if (halves_) {
            need(cur_ir);
            fail(WHERE);
        }
        return half_sum_here(xv);
    }
    std::size_t c = cur;
    Ty i32t = s.t.i32;
    V lane = s.op(c, "waveGetLaneIndex", Attr::ReadOnly, i32t, 111, {});
    V sum = xv;
    for (int step = 0; step < (halves_ ? 5 : 6); ++step) {
        V bit = s.m.i32(1u << step);
        V partner = s.m.bin(c, Bin::Xor, lane, bit);
        V theirs = read_lane(sum, partner);
        sum = add_halves(sum, theirs);
    }
    V high = s.m.i32(0xFFFF0000u);
    V low = s.m.i32(0xFFFF);
    V zero = s.m.i32(0);
    V first = read_lane(xv, zero);
    first = s.m.bin(c, Bin::And, first, high);
    sum = s.m.bin(c, Bin::And, sum, low);
    V mine = s.m.bin(c, Bin::Or, first, sum);
    if (!halves_) {
        return mine;
    }
    auto [lo, hi] = exchange_halves(c, mine);
    std::size_t cc = cur;
    V total = add_halves(lo, hi);
    total = s.m.bin(cc, Bin::And, total, low);
    V f2 = s.m.bin(cc, Bin::And, lo, high);
    return s.m.bin(cc, Bin::Or, f2, total);
}

Lowered Lower::run() {
    const ir::Program& prog = p();
    auto order = ir::reverse_postorder(prog.blocks);
    for (std::size_t n = 0; n < order.size(); ++n) {
        rpo[order[n]] = n;
    }
    for (std::size_t n = 0; n < order.size(); ++n) {
        block[order[n]] = n == 0 ? s.entry : s.m.new_block();
    }
    for (std::size_t b : order) {
        bool header = std::any_of(prog.blocks[b].preds.begin(), prog.blocks[b].preds.end(),
                                  [&](std::size_t q) { return block[q].has_value() && rpo[b] <= rpo[q]; });
        if (header) {
            V phi = s.m.phi(*block[b], s.t.i32);
            counters.emplace(b, std::pair{phi, std::vector<std::pair<V, std::size_t>>{}});
        }
    }
    cur = s.entry;
    find_sections();
    inputs();
    if (s.mesh) {
        try {
            mesh_prologue(*s.mesh);
        } catch (const WhyError& e) {
            stop_unsupported(0, e.why);
        }
        block[order[0]] = cur;
    }
    for (std::size_t b : order) {
        cur = *block[b];
        cur_ir = b;
        bool any = false, all = true;
        for (std::size_t q : prog.blocks[b].preds) {
            if (!block[q]) {
                continue;
            }
            any = true;
            if (!exported_out(q)) {
                all = false;
            }
        }
        exported[b] = any && all;
        for (std::size_t i = 0; i < prog.blocks[b].phis.size(); ++i) {
            const ir::Phi& phi = prog.blocks[b].phis[i];
            Val val;
            if (phi.reg.kind == Reg::S && (phi.reg.n == ir::EXEC || phi.reg.n == ir::EXEC + 1)) {
                val = Val::missing("a lane mask");
            } else if (auto it = bad.find(phi.value); it != bad.end()) {
                val = Val::missing(it->second);
            } else if (phi.reg.kind == Reg::Scc) {
                V v = s.m.phi(cur, s.t.i1);
                data_phis.emplace_back(v, b, i);
                val = Val::bit(v);
            } else {
                V v = s.m.phi(cur, s.t.i32);
                data_phis.emplace_back(v, b, i);
                val = Val::i32(v);
            }
            vals[phi.value] = val;
        }
        if (lds_meet && lds_meet->second[b].first) {
            lds_meeting();
        }
        std::size_t i = 0;
        while (i < prog.blocks[b].insts.size()) {
            inst(b, i);
            if (auto sb = sections.find(b); sb != sections.end()) {
                if (auto sec = sb->second.find(i); sec != sb->second.end()) {
                    std::size_t end = sec->second.end;
                    for (std::size_t j = i + 1; j < end; ++j) {
                        section_inst(b, j, i);
                    }
                    i = end;
                    continue;
                }
            }
            ++i;
        }
        mask_ok = mask_data == MaskData::Console;
        if (lds_meet && lds_meet->second[b].second) {
            lds_meeting();
        }
        tail[b] = cur;
        exit(b);
    }
    fill_phis();
    Lowered out;
    for (const auto& [_, u] : user_reads) {
        out.user_data.push_back(u);
    }
    s.samplers = static_cast<std::uint32_t>(samplers.size());
    for (const auto& r : s.root_cbvs) {
        out.root_cbvs.push_back(r.first);
    }
    for (const auto& r : s.root_srvs) {
        out.root_srvs.push_back(r.first);
    }
    for (const auto& r : s.root_uavs) {
        out.root_uavs.push_back(r.first);
    }
    out.cmp_samplers = s.cmp_samplers;
    out.wave_size = s.wave_size;
    out.dxil = s.finish();
    out.samplers = std::move(samplers);
    return out;
}

bool Lower::exported_out(std::size_t b) const { return exported[b] || has_done_export(b); }

bool Lower::has_done_export(std::size_t b) const {
    for (std::size_t i = 0; i < p().blocks[b].insts.size(); ++i) {
        auto it = exports.find(Site{b, i});
        if (it != exports.end() && it->second->done) {
            return true;
        }
    }
    return false;
}

void Lower::inputs() {
    std::unordered_map<Value, Input, ValueHash> io_inputs;
    for (const auto& [v, in] : x.io->inputs) {
        io_inputs[v] = in;
    }
    for (const auto& [reg, v] : p().inputs) {
        Val val;
        auto it = io_inputs.find(v);
        if (reg.kind == Reg::S && params.contains(v)) {
            continue;
        }
        if (reg.kind == Reg::V && it != io_inputs.end() && it->second.kind == Input::Bary) {
            continue;
        }
        if (reg.kind == Reg::S) {
            if (it != io_inputs.end()) {
                val = input(it->second);
            } else if (reg.n <= 105 && x.io->initialized_sgprs && reg.n >= *x.io->initialized_sgprs) {
                val = Val::i32(s.m.i32(0));
            } else if (reg.n >= ir::PARKED) {
                val = Val::i32(s.m.i32(0));
            } else {
                val = Val::missing("a hardware SGPR input");
            }
        } else if (reg.kind == Reg::Scc) {
            val = Val::missing("SCC on entry");
        } else {
            if (it != io_inputs.end()) {
                val = input(it->second);
            } else {
                val = Val::i32(s.m.i32(0));
            }
        }
        vals[v] = val;
    }
}

Val Lower::input(Input in) {
    std::uint8_t kind, col;
    switch (in.kind) {
        case Input::VertexId:
            kind = 1;
            col = 0;
            break;
        case Input::InstanceId:
            kind = 2;
            col = 0;
            break;
        case Input::Position:
            kind = 3;
            col = in.index;
            break;
        case Input::Attribute: {
            auto f = find_input(in.row, in.channel);
            if (!f) {
                return Val::missing("a vertex input the signature lacks");
            }
            return load_input(f->first, f->second);
        }
        case Input::Bary: return barycentric(in.bary, in.index);
        case Input::PrimitiveId: return Val::missing("the primitive id");
        case Input::FrontFace:
            kind = 13;
            col = 0;
            break;
        case Input::ThreadIdInGroup: return thread_id(in.index);
        case Input::GroupId: {
            Val id = compute_id("groupId.i32", 94, in.index);
            if (!one_dimensional()) {
                return id;
            }
            V base = host_word(dxil::USER_GROUP_BASE, 0, 0);
            return Val::i32(s.m.bin(s.entry, Bin::Add, id.v, base));
        }
        case Input::GroupInfo: return Val::missing("the thread-group info SGPR");
        case Input::MeshGroup: {
            Val g = compute_id("groupId.i32", 94, 0);
            V n = s.m.i32(x.mesh_stride);
            return Val::i32(s.m.bin(s.entry, Bin::Mul, g.v, n));
        }
        case Input::MergedWaveInfo: {
            V f = mesh_thread();
            V shift = s.m.i32(static_cast<std::uint32_t>(std::countr_zero(wave())));
            V w = s.m.bin(s.entry, Bin::LShr, f, shift);
            V k24 = s.m.i32(24);
            return Val::i32(s.m.bin(s.entry, Bin::Shl, w, k24));
        }
        default: return Val::missing("a pixel system input");
    }
    auto free_row = [&] {
        std::uint32_t m = 0;
        for (const auto& e : s.inputs) {
            if (e.row) {
                m = std::max(m, *e.row + 1);
            }
        }
        return m;
    };
    std::uint32_t fr = free_row();
    std::size_t id;
    auto pos = std::find_if(s.inputs.begin(), s.inputs.end(), [&](const SigElement& e) { return e.kind == kind; });
    if (pos != s.inputs.end()) {
        id = static_cast<std::size_t>(pos - s.inputs.begin());
        if (kind != 3 && !s.inputs[id].row) {
            s.inputs[id].row = fr;
        }
    } else if (kind >= 3) {
        return Val::missing("a pixel system input the signature lacks");
    } else {
        std::uint32_t row = free_row();
        static const char* const NAMES[] = {"", "SV_VertexID", "SV_InstanceID"};
        SigElement e;
        e.name = NAMES[kind];
        e.kind = kind;
        e.comp = 5;
        e.row = row;
        e.cols = 1;
        s.inputs.push_back(std::move(e));
        id = s.inputs.size() - 1;
    }
    return load_input(id, col);
}

bool Lower::one_dimensional() const {
    constexpr std::uint32_t MOST = 20;
    auto reads = [&](std::uint8_t c) {
        return std::any_of(x.io->inputs.begin(), x.io->inputs.end(),
                           [&](const auto& i) { return i.second.kind == Input::GroupId && i.second.index == c; });
    };
    std::uint32_t words = 0;
    for (const auto& e : x.native->layout) {
        words = std::max(words, std::uint32_t{e.user_data} + e.sgprs);
    }
    return x.native->io.kind == StageIo::Cs && reads(0) && !reads(1) && !reads(2) && words <= MOST && !is_spilled(*x.native);
}

Val Lower::compute_id(const char* name, std::uint32_t opcode, std::uint8_t c) {
    std::size_t entry = s.entry;
    V cv = s.m.i32(c);
    return Val::i32(s.op(entry, name, Attr::ReadNone, s.t.i32, opcode, {cv}));
}

std::optional<std::pair<std::size_t, std::uint8_t>> Lower::find_input(std::uint8_t row, std::uint8_t channel) const {
    for (std::size_t id = 0; id < s.inputs.size(); ++id) {
        const SigElement& e = s.inputs[id];
        if (!e.row) {
            continue;
        }
        if (*e.row == row && channel >= e.col && channel < e.col + e.cols) {
            return std::pair{id, static_cast<std::uint8_t>(channel - e.col)};
        }
    }
    return std::nullopt;
}

Val Lower::load_input(std::size_t id, std::uint8_t col) {
    if (s.inputs[id].half) {
        return Val::missing("a 16-bit input");
    }
    return load_any_input(id, col);
}

Val Lower::load_any_input(std::size_t id, std::uint8_t col) {
    SigElement& e = s.inputs[id];
    e.usage = static_cast<std::uint8_t>(e.usage | (1u << col));
    bool is_int = e.comp != 9, bit = e.comp == 1;
    Ty ty = bit ? s.t.i1 : is_int ? s.t.i32 : s.t.f32;
    const char* suffix = bit ? ".i1" : is_int ? ".i32" : ".f32";
    std::size_t entry = s.entry;
    V a0 = s.m.i32(static_cast<std::uint32_t>(id));
    V a1 = s.m.i32(0);
    V a2 = s.m.i8(col);
    V a3 = s.m.undef(s.t.i32);
    V v = s.op(entry, std::string("loadInput") + suffix, Attr::ReadNone, ty, 4, {a0, a1, a2, a3});
    if (bit) {
        return Val::i32(s.m.cast(entry, Cast::ZExt, v, s.t.i32));
    }
    if (is_int) {
        return Val::i32(v);
    }
    return Val::i32(s.m.cast(entry, Cast::Bitcast, v, s.t.i32));
}

Val Lower::get(Value v) {
    if (phi_at.contains(v)) {
        read_phis.insert(v);
    }
    bool is_mask_v = st().masks.contains(v) || st().highs.contains(v);
    auto it = vals.find(v);
    if ((it == vals.end() || it->second.kind == Val::Missing) && mask_ok && is_mask_v) {
        return Val::i32(mask_dword(v));
    }
    if (it != vals.end()) {
        if (it->second.kind == Val::Missing) {
            fail(it->second.why);
        }
        return it->second;
    }
    if (auto pit = params.find(v); pit != params.end()) {
        Val val = user(pit->second.first, pit->second.second);
        vals[v] = val;
        return val;
    }
    if (lazy_barycentric(v)) {
        return vals.at(v);
    }
    fail("a value defined after its use");
}

bool Lower::lazy_barycentric(Value v) {
    for (const auto& [w, in] : x.io->inputs) {
        if (in.kind == Input::Bary && w == v) {
            Val val = barycentric(in.bary, in.index);
            vals[v] = val;
            if (val.kind == Val::Missing) {
                fail(val.why);
            }
            return true;
        }
    }
    return false;
}

Val Lower::barycentric(Barycentric kind, std::uint8_t index) {
    auto mode = interp_mode(kind);
    if (!mode) {
        return Val::missing("pull-model barycentrics");
    }
    if (index > 1) {
        return Val::missing("pull-model barycentrics");
    }
    auto bary = [](const SigElement& e) { return e.kind == 28; };
    std::size_t id;
    auto pos = std::find_if(s.inputs.begin(), s.inputs.end(), [&](const SigElement& e) { return bary(e) && (e.interp == *mode || e.usage == 0); });
    if (pos != s.inputs.end()) {
        id = static_cast<std::size_t>(pos - s.inputs.begin());
    } else {
        auto count = static_cast<std::uint32_t>(std::count_if(s.inputs.begin(), s.inputs.end(), bary));
        if (count >= 2) {
            return Val::missing("barycentrics of a third interpolation mode");
        }
        SigElement e;
        e.name = "SV_Barycentrics";
        e.index = count;
        e.kind = 28;
        e.comp = 9;
        e.interp = *mode;
        e.cols = 3;
        s.inputs.push_back(std::move(e));
        id = s.inputs.size() - 1;
    }
    SigElement& e = s.inputs[id];
    if (e.col != 0 || e.cols < 3) {
        return Val::missing("a console barycentrics element without i and j");
    }
    e.row.reset();
    e.interp = *mode;
    s.flags |= dxil::flags::BARYCENTRICS;
    return load_any_input(id, static_cast<std::uint8_t>(index + 1));
}

V Lower::i32_of(Value v) {
    Val g = get(v);
    switch (g.kind) {
        case Val::I32: return g.v;
        case Val::Bit: return s.m.cast(cur, Cast::ZExt, g.v, s.t.i32);
        default: fail(g.why);
    }
}

Val Lower::user(std::size_t param, std::uint32_t dword) {
    const RootParam& rp = x.native->layout[param];
    switch (rp.root) {
        case 5:
        case 6: return Val::i32(s.m.i32(0));
        case 0:
        case 3: break;
        default: return Val::missing("a root descriptor");
    }
    std::uint32_t index = std::uint32_t{rp.user_data} + dword - user_first();
    user_reads[index] = UserData{index, rp.slot, rp.root, dword};
    return Val::i32(user_at(index));
}

std::uint32_t Lower::user_first() const { return is_spilled(*x.native) ? 16 : 0; }

V Lower::host_word(std::uint32_t root, std::uint8_t slot, std::uint32_t dword) {
    std::optional<std::uint32_t> known;
    for (const auto& [_, u] : user_reads) {
        if (u.root == root && u.slot == slot && u.dword == dword) {
            known = u.index;
            break;
        }
    }
    std::uint32_t index;
    if (known) {
        index = *known;
    } else {
        std::uint32_t first = user_first();
        std::uint32_t m = 0;
        for (const auto& e : x.native->layout) {
            m = std::max(m, std::uint32_t{e.user_data} + e.sgprs);
        }
        index = m > first ? m - first : 0;
        while (user_reads.contains(index)) {
            ++index;
        }
        user_reads[index] = UserData{index, slot, root, dword};
    }
    return user_at(index);
}

V Lower::user_at(std::uint32_t index) {
    s.user_data = std::max(s.user_data, index + 1);
    if (!user_data) {
        user_data = s.user_data_handle();
    }
    V h = *user_data;
    std::size_t entry = s.entry;
    V row = s.m.i32(index / 4);
    V r = s.op(entry, "cbufferLoadLegacy.i32", Attr::ReadOnly, s.t.cbuf_ret, 59, {h, row});
    return s.m.extract(entry, r, index % 4);
}

std::span<const std::pair<std::uint32_t, const Inst*>> Lower::readers(Value v) const {
    if (!readers_) {


        auto& r = readers_.emplace();
        for (const auto& blk : p().blocks) {
            for (const Inst& i : blk.insts) {
                std::size_t from = r.size();
                for (const auto& rd : i.reads) {
                    bool dup = false;
                    for (std::size_t k = from; k < r.size(); ++k) {
                        dup |= r[k].first == rd.second.id;
                    }
                    if (!dup) {
                        r.emplace_back(rd.second.id, &i);
                    }
                }
            }
        }
        std::stable_sort(r.begin(), r.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    }
    const auto& r = *readers_;
    auto lo = std::lower_bound(r.begin(), r.end(), v.id, [](const auto& x, std::uint32_t id) { return x.first < id; });
    auto hi = lo;
    while (hi != r.end() && hi->first == v.id) {
        ++hi;
    }
    return {lo, hi};
}

bool Lower::computed_with(Value v, std::uint32_t depth) const {
    {
        for (const auto& [_, ip] : readers(v)) {
            const Inst& i = *ip;
            auto name = i.name();
            bool moves = name == "s_mov_b32" || name == "s_mov_b64" || name == "v_mov_b32" || name == "v_readfirstlane_b32" ||
                         name == "v_readlane_b32" || name == "v_writelane_b32" || name == "v_cndmask_b32";
            bool alu = std::holds_alternative<SopFields>(i.fields) || std::holds_alternative<Valu>(i.fields);
            bool r;
            if (alu && moves) {
                r = depth > 0 && std::any_of(i.writes.begin(), i.writes.end(), [&](const auto& w) { return computed_with(w.second, depth - 1); });
            } else if (alu) {
                r = !(name.starts_with("v_cmp") || name.starts_with("s_cmp"));
            } else {
                r = false;
            }
            if (r) {
                return true;
            }
        }
    }
    return false;
}

std::optional<std::pair<V, V>> Lower::view_address(Site site) {
    auto it = scalar.find(site);
    if (it == scalar.end()) {
        return std::nullopt;
    }
    Address a = it->second;
    if (a.param >= x.native->layout.size()) {
        return std::nullopt;
    }
    const RootParam& rp = x.native->layout[a.param];
    if (rp.root != 3 || a.dynamic || a.offset % 32 != 0) {
        return std::nullopt;
    }
    std::uint32_t dword = a.offset / 4;
    const UsedResource* used = nullptr;
    for (const UsedResource& u : x.native->used) {
        if (u.root == 3 && u.user_data == rp.user_data && u.table_offset_dwords <= dword) {
            if (!used || u.table_offset_dwords >= used->table_offset_dwords) {
                used = &u;
            }
        }
    }
    if (!used) {
        return std::nullopt;
    }
    if (used->dwords != 4 || !(used->kind == 0 || used->kind == 3)) {
        return std::nullopt;
    }
    V lo = host_word(dxil::USER_VIEW_ADDRESS, rp.slot, dword);
    V hi = host_word(dxil::USER_VIEW_ADDRESS + 1, rp.slot, dword);
    return std::pair{lo, hi};
}

std::pair<V, V> Lower::address_plus(V lo, V hi, std::int32_t off) {
    if (off == 0) {
        return {lo, hi};
    }
    std::size_t c = cur;
    Ty i32t = s.t.i32;
    V k = s.m.i32(static_cast<std::uint32_t>(off));
    V sum = s.m.bin(c, Bin::Add, lo, k);
    V wrapped = s.m.cmp(c, off > 0 ? pred::IULT : pred::IUGT, sum, lo);
    V one = s.m.cast(c, Cast::ZExt, wrapped, i32t);
    V h2 = s.m.bin(c, off > 0 ? Bin::Add : Bin::Sub, hi, one);
    return {sum, h2};
}

std::pair<V, V> Lower::memory(V lo, V hi, std::optional<V> g) {
    using namespace dxil;
    std::size_t c = cur;
    Ty i32t = s.t.i32, res = s.t.res_ret;
    V k16 = s.m.i32(16);
    V k8 = s.m.i32(0xFF);
    V low = s.m.bin(c, Bin::LShr, lo, k16);
    V high = s.m.bin(c, Bin::And, hi, k8);
    high = s.m.bin(c, Bin::Shl, high, k16);
    V page = s.m.bin(c, Bin::Or, low, high);
    V last = s.m.i32(MEMORY_PAGES - 1);
    page = s.op(c, "binary.i32", Attr::ReadNone, i32t, 40, {page, last});
    s.m.tag(page, Tag::Clamp);
    V k3 = s.m.i32(3);
    V at = s.m.bin(c, Bin::Shl, page, k3);
    V directory = s.root_srv_handle(SLOT_MEMORY);
    V iu = s.m.undef(i32t);
    V both = s.m.i8(3);
    V align = s.m.i32(4);
    V entry = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, res, 139, {directory, at, iu, both, align});
    V copy = s.m.extract(c, entry, 0);
    V start = s.m.extract(c, entry, 1);
    copy = as_int(copy);
    start = as_int(start);
    V zero = s.m.i32(0);
    V missing = s.m.cmp(c, pred::EQ, copy, zero);
    if (g) {
        missing = s.m.bin(c, Bin::And, missing, *g);
    }
    V places = s.m.i32(MEMORY_FAULTS - 1);
    V place = s.m.bin(c, Bin::URem, page, places);
    V one = s.m.i32(1);
    place = s.m.bin(c, Bin::Add, place, one);
    V k2 = s.m.i32(2);
    place = s.m.bin(c, Bin::Shl, place, k2);
    place = s.m.select(c, missing, place, zero);
    V faults = s.root_uav_handle(SLOT_FAULTS);
    Ty void_t = s.t.void_;
    V mask = s.m.i8(1);
    s.op(c, "rawBufferStore.i32", Attr::NoUnwind, void_t, 140, {faults, place, iu, page, iu, iu, iu, mask, align});
    V base = host_word(USER_MEMORY_BASE, 0, 0);
    V slot = s.m.bin(c, Bin::Add, base, copy);
    V h = s.heap_handle(c, slot, false, true, {11 | 0x1000, 0});
    V k64k = s.m.i32(0xFFFF);
    V within = s.m.bin(c, Bin::And, lo, k64k);
    V byte = s.m.bin(c, Bin::Add, start, within);
    return {h, byte};
}

void Lower::memory_flat(const Inst& inst, std::size_t b, std::size_t i) {
    const auto& f = std::get<FlatFields>(inst.fields);
    if (f.lds || !(!f.saddr || (f.saddr->kind == Operand::Scalar && f.saddr->a == NULL_CODE))) {
        fail("a global address with a scalar base");
    }
    if (!f.addr || f.addr->kind != Operand::Vector) {
        fail("a global address not in VGPRs");
    }
    std::uint8_t areg = f.addr->a;
    auto word = [&](std::uint8_t k) -> V {
        auto v = inst.read(Reg::v(static_cast<std::uint8_t>(areg + k)));
        if (!v) {
            fail("a global address");
        }
        return i32_of(*v);
    };
    V lo0 = word(0);
    V hi0 = word(1);
    auto [lo, hi] = address_plus(lo0, hi0, f.offset);
    std::optional<V> g = guard(b, i);
    std::size_t c = cur;
    Ty i32t = s.t.i32, res = s.t.res_ret, void_t = s.t.void_;
    V iu = s.m.undef(i32t);
    V align = s.m.i32(4);
    auto name = inst.name();
    auto count = [&](std::string_view rest) -> std::uint8_t {
        if (rest == "dword") {
            return 1;
        }
        if (rest == "dwordx2") {
            return 2;
        }
        if (rest == "dwordx3") {
            return 3;
        }
        if (rest == "dwordx4") {
            return 4;
        }
        fail(name.data());
    };
    if (name.starts_with("global_load_")) {
        std::uint8_t n = count(name.substr(12));
        if (!f.dst || f.dst->kind != Operand::Vector) {
            fail("a global load result");
        }
        std::uint8_t reg = f.dst->a;
        auto [h, byte] = memory(lo, hi, g);
        V mask = s.m.i8(static_cast<std::uint8_t>((1u << n) - 1));
        V r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, res, 139, {h, byte, iu, mask, align});
        for (std::uint8_t k = 0; k < n; ++k) {
            V xv = s.m.extract(c, r, k);
            xv = as_int(xv);
            write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), xv, g);
        }
        return;
    }
    if (name.starts_with("global_store_")) {
        std::uint8_t n = count(name.substr(13));
        if (!f.data) {
            fail("global store data");
        }
        std::vector<V> vs = vgprs(inst, *f.data, n);
        auto [h, byte] = memory(lo, hi, g);
        std::vector<V> args{h, byte, iu};
        args.insert(args.end(), vs.begin(), vs.end());
        args.resize(3 + 4, iu);
        args.push_back(s.m.i8(static_cast<std::uint8_t>((1u << n) - 1)));
        args.push_back(align);
        auto at = begin_guarded(g);
        s.op(cur, "rawBufferStore.i32", Attr::NoUnwind, void_t, 140, args);
        end_guarded(at, {});
        return;
    }
    if (!name.starts_with("global_atomic_")) {
        fail(name.data());
    }
    std::string_view op = name.substr(14);
    if (!f.data) {
        fail("global atomic data");
    }
    V v = vgprs(inst, *f.data, 1)[0];
    std::optional<std::uint8_t> back;
    if (f.dst && f.dst->kind == Operand::Vector && f.cache.glc) {
        back = f.dst->a;
    }
    std::optional<Value> back_v;
    if (back) {
        back_v = inst.read(Reg::v(*back));
    }
    V old = back_v ? i32_of(*back_v) : s.m.i32(0);
    auto [h, byte] = memory(lo, hi, g);
    std::optional<std::pair<std::uint32_t, V>> simple;
    if (op == "add") {
        simple = std::pair{0u, v};
    } else if (op == "sub") {
        V zero = s.m.i32(0);
        simple = std::pair{0u, s.m.bin(c, Bin::Sub, zero, v)};
    } else if (op == "and") {
        simple = std::pair{1u, v};
    } else if (op == "or") {
        simple = std::pair{2u, v};
    } else if (op == "xor") {
        simple = std::pair{3u, v};
    } else if (op == "smin") {
        simple = std::pair{4u, v};
    } else if (op == "smax") {
        simple = std::pair{5u, v};
    } else if (op == "umin") {
        simple = std::pair{6u, v};
    } else if (op == "umax") {
        simple = std::pair{7u, v};
    } else if (op == "swap") {
        simple = std::pair{8u, v};
    } else if (op == "fmin" || op == "fmax") {
    } else {
        fail(name.data());
    }
    auto at = begin_guarded(g);
    V was;
    if (simple) {
        V code = s.m.i32(simple->first);
        was = s.op(cur, "atomicBinOp.i32", Attr::NoUnwind, i32t, 78, {h, code, byte, iu, iu, simple->second});
    } else {
        std::size_t head = cur;
        V mask = s.m.i8(1);
        V first = s.op(head, "rawBufferLoad.f32", Attr::ReadOnly, res, 139, {h, byte, iu, mask, align});
        first = s.m.extract(head, first, 0);
        first = as_int(first);
        std::size_t pass = s.m.new_block();
        std::size_t done = s.m.new_block();
        s.m.terminate(head, Term::br(pass));
        cur = pass;
        V seen = s.m.phi(pass, i32t);
        V n = s.m.phi(pass, i32t);
        Ty f32t = s.t.f32;
        V fv = as_float(v);
        V fseen = as_float(seen);
        std::uint32_t pick = op == "fmin" ? 36 : 35;
        V wanted = s.op(pass, "binary.f32", Attr::ReadNone, f32t, pick, {fseen, fv});
        wanted = as_int(wanted);
        V found = s.op(pass, "atomicCompareExchange.i32", Attr::NoUnwind, i32t, 79, {h, byte, iu, iu, seen, wanted});
        V one = s.m.i32(1);
        V n1 = s.m.bin(pass, Bin::Add, n, one);
        V lost = s.m.cmp(pass, pred::NE, found, seen);
        V limit = s.m.i32(1u << 16);
        V more = s.m.cmp(pass, pred::IULT, n1, limit);
        V again = s.m.bin(pass, Bin::And, lost, more);
        s.m.terminate(pass, Term::cond_br(again, pass, done));
        V zero = s.m.i32(0);
        s.m.set_phi(seen, {{first, head}, {found, pass}});
        s.m.set_phi(n, {{zero, head}, {n1, pass}});
        cur = done;
        was = found;
    }
    was = end_guarded(at, {{was, old}})[0];
    if (back) {
        write(inst, Reg::v(*back), was, std::nullopt);
    }
}

}   
