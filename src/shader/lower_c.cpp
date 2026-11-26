// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// scalar loads by address, lane masks as data, operands, float helpers,
// lane booleans, the exchange between two host waves, and results. See
// lower_impl.hpp.
#include <algorithm>
#include <bit>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

namespace {
std::uint32_t fbits(float f) { return std::bit_cast<std::uint32_t>(f); }
}   

void Lower::memory_smem(const Inst& inst) {
    auto* f = std::get_if<SmemFields>(&inst.fields);
    if (!f || !f->data || f->data->kind != Operand::Scalar || f->base.kind != Operand::Scalar) {
        fail("a scalar load from memory");
    }
    std::uint8_t code = f->data->a, base = f->base.a;
    std::int32_t offset = f->offset;
    Operand soffset = f->soffset;
    auto name = inst.name();
    std::uint8_t count;
    if (name == "s_load_dword") {
        count = 1;
    } else if (name == "s_load_dwordx2") {
        count = 2;
    } else if (name == "s_load_dwordx4") {
        count = 4;
    } else if (name == "s_load_dwordx8") {
        count = 8;
    } else if (name == "s_load_dwordx16") {
        count = 16;
    } else {
        fail("a scalar memory access by address");
    }
    auto word = [&](std::uint8_t k) -> V {
        auto v = inst.read(Reg::s(static_cast<std::uint8_t>(base + k)));
        if (!v) {
            fail("an address not in SGPRs");
        }
        return i32_of(*v);
    };
    V lo = word(0);
    V hi = word(1);
    std::size_t c = cur;
    Ty i32t = s.t.i32, res = s.t.res_ret;
    if (!(soffset.kind == Operand::Scalar && soffset.a == NULL_CODE)) {
        V so = operand(inst, soffset);
        V sum = s.m.bin(c, Bin::Add, lo, so);
        V carry = s.m.cmp(c, pred::IULT, sum, lo);
        carry = s.m.cast(c, Cast::ZExt, carry, i32t);
        hi = s.m.bin(c, Bin::Add, hi, carry);
        lo = sum;
    }
    V iu = s.m.undef(i32t);
    V align = s.m.i32(4);
    for (std::uint8_t first = 0; first < count; first = static_cast<std::uint8_t>(first + 4)) {
        std::uint8_t n = std::min<std::uint8_t>(static_cast<std::uint8_t>(count - first), 4);
        auto [l2, h2] = address_plus(lo, hi, offset + 4 * std::int32_t{first});
        auto [h, byte] = memory(l2, h2, std::nullopt);
        V mask = s.m.i8(static_cast<std::uint8_t>((1u << n) - 1));
        V r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, res, 139, {h, byte, iu, mask, align});
        for (std::uint8_t k = 0; k < n; ++k) {
            V xv = s.m.extract(c, r, k);
            xv = as_int(xv);
            write(inst, Reg::s(static_cast<std::uint8_t>(code + first + k)), xv, std::nullopt);
        }
    }
}

V Lower::mask_dword(Value v) {
    Value lo = v;
    bool high = false;
    if (auto it = st().highs.find(v); it != st().highs.end()) {
        lo = it->second;
        high = true;
    }
    auto mit = st().masks.find(lo);
    if (mit == st().masks.end()) {
        fail(LANE_MASK);
    }
    Bool m = mit->second;
    std::optional<std::size_t> made;
    if (auto it = def_block.find(lo); it != def_block.end()) {
        made = it->second;
    }
    bool whole = exchange && !local && (!made || all_lanes(*made));
    if (!st().active.contains(lo) && !whole) {
        if (exchange && !local) {
            need(made.value_or(0));
        }
        fail(MASK_ELSEWHERE);
    }
    std::pair<V, V> xy;
    if (auto it = ballots.find(lo); it != ballots.end()) {
        xy = it->second;
    } else {
        std::size_t at;
        if (!made) {
            at = s.entry;
        } else if (*made == cur_ir) {
            at = cur;
        } else {
            if (!tail[*made]) {
                fail("a lane mask written after its use");
            }
            at = *tail[*made];
        }
        if (!balloting.insert(lo).second) {
            fail("a lane mask whose lanes depend on its own value");
        }
        V bit;
        try {
            bit = boolean(m, at);
        } catch (...) {
            balloting.erase(lo);
            throw;
        }
        balloting.erase(lo);
        Ty i32t = s.t.i32;
        Ty four = s.m.named_struct("dx.types.fouri32", {i32t, i32t, i32t, i32t});
        V r = s.op(at, "waveActiveBallot", Attr::NoUnwind, four, 116, {bit});
        s.flags |= dxil::flags::WAVE_OPS;
        V x0 = s.m.extract(at, r, 0);
        V x1 = s.m.extract(at, r, 1);
        xy = {x0, x1};
        if (exchange) {
            if (local) {
                if (!local_ok && !local_any) {
                    fail("a lane mask read as data in a half-wave");
                }
            } else if (!exchange->small && !(!made || all_lanes(*made))) {
                need(made.value_or(0));
                fail("a lane mask exchanged where not every lane runs");
            }
            xy = exchange_halves(at, xy.first);
        }
        ballots[lo] = xy;
    }
    return high ? xy.second : xy.first;
}

V Lower::operand(const Inst& inst, const Operand& op) {
    switch (op.kind) {
        case Operand::Scalar: {
            if (op.a == NULL_CODE) {
                return s.m.i32(0);
            }
            auto v = inst.read(Reg::s(op.a));
            if (!v) {
                fail("an unread operand");
            }
            return i32_of(*v);
        }
        case Operand::Vector: {
            auto v = inst.read(Reg::v(op.a));
            if (!v) {
                fail("an unread operand");
            }
            return i32_of(*v);
        }
        case Operand::Const: {
            auto c = resource::inline_constant(op.a);
            if (!c) {
                fail("a special operand (vccz, execz, scc, lds_direct)");
            }
            return s.m.i32(*c);
        }
        case Operand::Literal: return s.m.i32(op.literal);
        default: fail("an attribute operand");
    }
}

V Lower::f16_operand(const Inst& inst, const Operand& op) {
    if (op.kind == Operand::Const && op.a >= 240 && op.a <= 248) {
        return s.m.i32(inline_f16(op.a));
    }
    return operand(inst, op);
}

V Lower::as_float(V xv) { return s.m.cast(cur, Cast::Bitcast, xv, s.t.f32); }
V Lower::as_int(V f) { return s.m.cast(cur, Cast::Bitcast, f, s.t.i32); }

std::optional<std::pair<std::uint8_t, bool>> Lower::sdwa_src(const Inst& inst, std::size_t k) {
    auto* v = std::get_if<Valu>(&inst.fields);
    if (!v || !v->sdwa || k >= 2) {
        return std::nullopt;
    }
    std::uint8_t sel = v->sdwa->src_sel[k];
    bool sext = (v->sdwa->sext >> k & 1) != 0;
    if (sel < 6) {
        return std::pair{sel, sext};
    }
    return std::nullopt;
}

V Lower::select_bits(V xv, std::uint8_t sel, bool sext) {
    std::size_t c = cur;
    std::uint32_t width, at;
    if (sel < 4) {
        width = 8;
        at = 8u * sel;
    } else {
        width = 16;
        at = 16u * (sel - 4u);
    }
    V left = s.m.i32(32 - width - at);
    V t = s.m.bin(c, Bin::Shl, xv, left);
    V right = s.m.i32(32 - width);
    return s.m.bin(c, sext ? Bin::AShr : Bin::LShr, t, right);
}

V Lower::fsrc(const Inst& inst, std::size_t k) {
    const auto& v = std::get<Valu>(inst.fields);
    if (k >= v.src.size()) {
        fail("a missing operand");
    }
    Operand op = v.src[k];
    auto selected = sdwa_src(inst, k);
    V f;
    if (selected) {
        V xv = operand(inst, op);
        xv = select_bits(xv, selected->first, selected->second);
        f = as_float(xv);
    } else if (k == 0 && src0) {
        f = as_float(*src0);
    } else if ((op.kind == Operand::Vector || op.kind == Operand::Scalar) && !(op.kind == Operand::Scalar && op.a == NULL_CODE)) {
        Reg r = op.kind == Operand::Vector ? Reg::v(op.a) : Reg::s(op.a);
        auto val = inst.read(r);
        if (!val) {
            fail("an unread operand");
        }
        if (auto it = floats.find(*val); it != floats.end()) {
            f = it->second;
        } else {
            V xv = i32_of(*val);
            f = as_float(xv);
        }
    } else {
        V xv = operand(inst, op);
        f = as_float(xv);
    }
    if (v.abs >> k & 1) {
        f = unary(6, f);
    }
    if (v.neg >> k & 1) {
        V z = s.m.f32(0x80000000u);
        f = s.m.bin(cur, Bin::Sub, z, f);
    }
    return f;
}

V Lower::src(const Inst& inst, std::size_t k) {
    const auto& v = std::get<Valu>(inst.fields);
    if (k >= v.src.size()) {
        fail("a missing operand");
    }
    Operand op = v.src[k];
    if ((v.abs >> k & 1) || (v.neg >> k & 1)) {
        fail("integer operand modifiers");
    }
    if (src0 && k == 0) {
        return *src0;
    }
    V xv = operand(inst, op);
    if (auto sel = sdwa_src(inst, k)) {
        return select_bits(xv, sel->first, sel->second);
    }
    return xv;
}

V Lower::unary(std::uint32_t op, V f) { return s.op(cur, "unary.f32", Attr::ReadNone, s.t.f32, op, {f}); }
V Lower::binary(std::uint32_t op, V a, V b) { return s.op(cur, "binary.f32", Attr::ReadNone, s.t.f32, op, {a, b}); }

V Lower::max_min(bool max, V a, V b) {
    std::size_t c = cur;
    V r = binary(max ? 35 : 36, a, b);
    V ia = as_int(a);
    V ib = as_int(b);
    V either = s.m.bin(c, Bin::Or, ia, ib);
    V magnitude = s.m.i32(0x7FFFFFFF);
    either = s.m.bin(c, Bin::And, either, magnitude);
    V zero = s.m.i32(0);
    V zeros = s.m.cmp(c, pred::EQ, either, zero);
    V sign = s.m.bin(c, max ? Bin::And : Bin::Or, ia, ib);
    V sgn = as_float(sign);
    return s.m.select(c, zeros, sgn, r);
}

V Lower::fmad(V a, V b, V c) { return s.op(cur, "tertiary.f32", Attr::ReadNone, s.t.f32, 46, {a, b, c}); }

V Lower::f16_toward_zero(V f) {
    std::size_t c = cur;
    Ty i32t = s.t.i32;
    V h = s.op(c, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {f});
    V back = s.op(c, "legacyF16ToF32", Attr::ReadNone, s.t.f32, 131, {h});
    V fa = unary(6, f);
    V ba = unary(6, back);
    V away = s.m.cmp(c, pred::OGT, ba, fa);
    V one = s.m.i32(1);
    V stepped = s.m.bin(c, Bin::Sub, h, one);
    return s.m.select(c, away, stepped, h);
}

V Lower::f16_nearest(V f) {
    std::size_t c = cur;
    Ty f32t = s.t.f32;
    V lo = f16_toward_zero(f);
    V one = s.m.i32(1);
    V zero = s.m.i32(0);
    V hi = s.m.bin(c, Bin::Add, lo, one);
    auto size = [&](V h) {
        V w = s.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {h});
        return unary(6, w);
    };
    V below = size(lo);
    V above = size(hi);
    V magnitude = s.m.i32(0x7FFF);
    V infinity = s.m.i32(0x7C00);
    V m = s.m.bin(c, Bin::And, hi, magnitude);
    V top = s.m.cmp(c, pred::EQ, m, infinity);
    V bound = s.m.f32(fbits(65536.0f));
    above = s.m.select(c, top, bound, above);
    V fa = unary(6, f);
    V under = s.m.bin(c, Bin::Sub, fa, below);
    V over = s.m.bin(c, Bin::Sub, above, fa);
    V nearer = s.m.cmp(c, pred::OLT, over, under);
    V tie = s.m.cmp(c, pred::OEQ, over, under);
    V odd = s.m.bin(c, Bin::And, lo, one);
    odd = s.m.cmp(c, pred::NE, odd, zero);
    tie = s.m.bin(c, Bin::And, tie, odd);
    V up = s.m.bin(c, Bin::Or, nearer, tie);
    return s.m.select(c, up, hi, lo);
}

V Lower::bfe(V a, V off, V width, bool is_signed) {
    std::size_t c = cur;
    V k31 = s.m.i32(31);
    V k32 = s.m.i32(32);
    V zero = s.m.i32(0);
    V one = s.m.i32(1);
    V ones = s.m.i32(UINT32_MAX);
    off = s.m.bin(c, Bin::And, off, k31);
    V shifted = s.m.bin(c, is_signed ? Bin::AShr : Bin::LShr, a, off);
    V full = s.m.cmp(c, pred::IUGE, width, k32);
    V w = s.m.bin(c, Bin::And, width, k31);
    V mask = s.m.bin(c, Bin::Shl, one, w);
    mask = s.m.bin(c, Bin::Sub, mask, one);
    mask = s.m.select(c, full, ones, mask);
    V r = s.m.bin(c, Bin::And, shifted, mask);
    if (!is_signed) {
        return r;
    }
    V left = s.m.bin(c, Bin::Sub, k32, w);
    left = s.m.bin(c, Bin::And, left, k31);
    V t = s.m.bin(c, Bin::Shl, r, left);
    t = s.m.bin(c, Bin::AShr, t, left);
    V is0 = s.m.cmp(c, pred::EQ, w, zero);
    V keep = s.m.bin(c, Bin::Or, full, is0);
    return s.m.select(c, keep, r, t);
}

V Lower::is_nan_f32(V f) {
    std::size_t c = cur;
    V bits = as_int(f);
    V mask = s.m.i32(0x7FFFFFFF);
    V abs = s.m.bin(c, Bin::And, bits, mask);
    V inf = s.m.i32(0x7F800000);
    return s.m.cmp(c, pred::IUGT, abs, inf);
}

V Lower::med3(V a, V b, V third) {
    std::size_t c = cur;
    V lo = binary(36, a, b);
    V hi = binary(35, a, b);
    V t = binary(36, hi, third);
    V median = binary(35, lo, t);
    V least = binary(36, lo, third);
    V nan_a = is_nan_f32(a);
    V nan_b = is_nan_f32(b);
    V nan_3 = is_nan_f32(third);
    V nan_ab = s.m.bin(c, Bin::Or, nan_a, nan_b);
    V nan = s.m.bin(c, Bin::Or, nan_ab, nan_3);
    return s.m.select(c, nan, least, median);
}

V Lower::fract(V a, float threshold, float below_one) {
    std::size_t c = cur;
    V r = unary(22, a);
    V limit = s.m.f32(fbits(threshold));
    V k = s.m.f32(fbits(below_one));
    V over = s.m.cmp(c, pred::OGE, r, limit);
    return s.m.select(c, over, k, r);
}

V Lower::trig_turns(V xv, bool cosine) {
    std::size_t c = cur;
    V zero = s.m.f32(0);
    V whole = unary(26, xv);
    V part = s.m.bin(c, Bin::Sub, xv, whole);
    V is_zero = s.m.cmp(c, pred::OEQ, xv, zero);
    part = s.m.select(c, is_zero, xv, part);
    V k = s.m.f32(fbits(TWO_PI));
    V angle = s.m.bin(c, Bin::Mul, part, k);
    V r = unary(cosine ? 12 : 13, angle);
    V at = s.m.f32(fbits(cosine ? 0.25f : 0.5f));
    V size = unary(6, part);
    V exact = s.m.cmp(c, pred::OEQ, size, at);
    return s.m.select(c, exact, zero, r);
}

V Lower::mul_hi(V a, V b, bool is_signed) {
    std::size_t c = cur;
    Ty i64t = s.m.int_(64);
    Cast ext = is_signed ? Cast::SExt : Cast::ZExt;
    V a64 = s.m.cast(c, ext, a, i64t);
    V b64 = s.m.cast(c, ext, b, i64t);
    V m = s.m.bin(c, Bin::Mul, a64, b64);
    V k32 = s.m.int_const(i64t, 32);
    V hi = s.m.bin(c, Bin::LShr, m, k32);
    s.flags |= dxil::flags::INT64;
    return s.m.cast(c, Cast::Trunc, hi, s.t.i32);
}

V Lower::legacy_mul(V a, V b) {
    std::size_t c = cur;
    V m = s.m.bin(c, Bin::Mul, a, b);
    V zero = s.m.f32(0);
    V az = s.m.cmp(c, pred::OEQ, a, zero);
    V bz = s.m.cmp(c, pred::OEQ, b, zero);
    V z = s.m.bin(c, Bin::Or, az, bz);
    return s.m.select(c, z, zero, m);
}


V Lower::boolean(Bool b, std::size_t at) {
    if (b == structure::TRUE_ || b == structure::FALSE_) {
        return s.m.boolean(b == structure::TRUE_);
    }
    if (auto it = bools.find({at, b.id}); it != bools.end()) {
        return it->second;
    }
    auto node = st().bools.node(b);
    V a = atom(node->atom, at);
    Bool lo = node->lo, hi = node->hi;
    V v;
    if (lo == structure::FALSE_ && hi == structure::TRUE_) {
        v = a;
    } else if (lo == structure::TRUE_ && hi == structure::FALSE_) {
        V t = s.m.boolean(true);
        v = s.m.bin(at, Bin::Xor, a, t);
    } else if (lo == structure::FALSE_) {
        V h = boolean(hi, at);
        v = s.m.bin(at, Bin::And, a, h);
    } else if (hi == structure::TRUE_) {
        V l = boolean(lo, at);
        v = s.m.bin(at, Bin::Or, a, l);
    } else {
        V l = boolean(lo, at);
        V h = boolean(hi, at);
        v = s.m.select(at, a, h, l);
    }
    bools[{at, b.id}] = v;
    return v;
}

V Lower::atom(Atom a, std::size_t at) {
    switch (a.kind) {
        case Atom::Lane: {
            Value v{static_cast<std::uint32_t>(a.payload)};
            if (auto it = lane_bits.find(v); it != lane_bits.end()) {
                return it->second;
            }
            auto f = lane_fail.find(v);
            fail(f != lane_fail.end() ? f->second : "a lane mask from a carry");
        }
        case Atom::Uniform: {
            Val g = get(Value{static_cast<std::uint32_t>(a.payload)});
            if (g.kind == Val::Bit) {
                return g.v;
            }
            if (g.kind == Val::I32) {
                V z = s.m.i32(0);
                return s.m.cmp(at, pred::NE, g.v, z);
            }
            fail(g.why);
        }
        case Atom::Phi: {
            Value id{static_cast<std::uint32_t>(a.payload)};
            if (auto it = mask_phis.find(id); it != mask_phis.end()) {
                return it->second;
            }
            auto pit = phi_at.find(id);
            if (pit == phi_at.end()) {
                fail("a lane-mask join without a phi");
            }
            auto [b, i] = pit->second;
            if (!block[b]) {
                fail("an unreachable join");
            }
            V v = s.m.phi(*block[b], s.t.i1);
            mask_phis[id] = v;
            pending_masks.emplace_back(v, b, i);
            return v;
        }
        case Atom::Quad: return s.m.boolean(true);
        case Atom::Opaque: {
            if (!console_lanes) {
                fail("an opaque lane mask");
            }
            Value v{static_cast<std::uint32_t>(a.payload)};
            std::optional<Val> hi;
            if (auto h = pair_high(v)) {
                hi = get(*h);
            }
            Val lo = get(v);
            auto word = [](const Val& w) -> V {
                if (w.kind != Val::I32) {
                    fail("an opaque lane mask");
                }
                return w.v;
            };
            V l = word(lo);
            std::optional<V> h;
            if (hi) {
                h = word(*hi);
            }
            return lane_bit(at, l, h);
        }
        case Atom::Constant: {
            if (!console_lanes) {
                fail("a constant lane mask");
            }
            V lo = s.m.i32(static_cast<std::uint32_t>(a.payload));
            V hi = s.m.i32(static_cast<std::uint32_t>(a.payload >> 32));
            return lane_bit(at, lo, hi);
        }
    }
    fail("an atom");
}

std::optional<V> Lower::wave_base() {
    if (!halves) {
        return std::nullopt;
    }
    std::uint32_t slot = *halves;
    if (wave_base_) {
        return wave_base_;
    }
    std::size_t e = s.entry;
    Ty i32t = s.t.i32;
    V lds = s.lds();
    V k = s.m.i32(slot);
    V ptr = s.m.element(e, lds, k);
    V zero = s.m.i32(0);
    s.m.store(e, ptr, zero);
    group_sync(e);
    V lane = s.op(e, "waveGetLaneIndex", Attr::ReadOnly, i32t, 111, {});
    V first = s.m.cmp(e, pred::EQ, lane, zero);
    V k32 = s.m.i32(32);
    V add = s.m.select(e, first, k32, zero);
    V ptr2 = s.m.element(e, lds, k);
    V old = s.m.atomic(e, Rmw::Add, ptr2, add);
    V base = s.op(e, "waveReadLaneFirst.i32", Attr::NoUnwind, i32t, 118, {old});
    s.flags |= dxil::flags::WAVE_OPS;
    wave_base_ = base;
    return base;
}

V Lower::console_thread() {
    std::size_t e = s.entry;
    Ty i32t = s.t.i32;
    V lane = s.op(e, "waveGetLaneIndex", Attr::ReadOnly, i32t, 111, {});
    if (auto base = wave_base()) {
        return s.m.bin(e, Bin::Add, *base, lane);
    }
    V flat = s.op(e, "flattenedThreadIdInGroup.i32", Attr::ReadNone, i32t, 96, {});
    V first = s.op(e, "waveReadLaneFirst.i32", Attr::NoUnwind, i32t, 118, {flat});
    std::uint32_t wave = s.wave_size.value_or(32);
    V mask = s.m.i32(~(wave - 1));
    V base = s.m.bin(e, Bin::And, first, mask);
    return s.m.bin(e, Bin::Add, base, lane);
}

V Lower::console_lane() {
    if (!exchange) {
        return lane_index();
    }
    if (clane) {
        return *clane;
    }
    std::size_t e = s.entry;
    V f = console_thread();
    V k63 = s.m.i32(63);
    V cl = s.m.bin(e, Bin::And, f, k63);
    s.flags |= dxil::flags::WAVE_OPS;
    clane = cl;
    return cl;
}

void Lower::group_sync(std::size_t blk) {
    V mode = s.m.i32(meeting_mode());
    s.op(blk, "barrier", Attr::NoDuplicate, s.t.void_, 80, {mode});
}

std::pair<V, V> Lower::exchange_halves(std::size_t at, V mine) {
    if (!exchange || exchange->small) {
        V z = s.m.i32(0);
        return {mine, z};
    }
    std::uint32_t slot = exchange->first;
    if (local) {
        V cl = console_lane();
        V k32 = s.m.i32(32);
        V z = s.m.i32(0);
        V upper = s.m.cmp(at, pred::IUGE, cl, k32);
        V lo = s.m.select(at, upper, z, mine);
        V hi = s.m.select(at, upper, mine, z);
        return {lo, hi};
    }
    V pr = exchange_pair(slot);
    V cl = console_lane();
    V five = s.m.i32(5);
    V half = s.m.bin(at, Bin::LShr, cl, five);
    V index = s.m.bin(at, Bin::Add, pr, half);
    V lds = s.lds();
    V ptr = s.m.element(at, lds, index);
    s.m.store(at, ptr, mine);
    group_sync(at);
    V one = s.m.i32(1);
    V k1 = s.m.bin(at, Bin::Add, pr, one);
    V p0 = s.m.element(at, lds, pr);
    V p1 = s.m.element(at, lds, k1);
    V lo = s.m.load(at, p0);
    V hi = s.m.load(at, p1);
    group_sync(at);
    return {lo, hi};
}

V Lower::exchange_pair(std::uint32_t slot) {
    if (pair) {
        return *pair;
    }
    std::size_t e = s.entry;
    V thread = console_thread();
    V six = s.m.i32(6);
    V wave = s.m.bin(e, Bin::LShr, thread, six);
    V one = s.m.i32(1);
    V twice = s.m.bin(e, Bin::Shl, wave, one);
    V base = s.m.i32(slot);
    V v = s.m.bin(e, Bin::Add, base, twice);
    pair = v;
    return v;
}

V Lower::scc_some(Value scc) {
    auto it = st().scc_some.find(scc);
    if (it == st().scc_some.end()) {
        fail(LANE_MASK);
    }
    Bool m = it->second;
    if (!console_lanes || local || (!exchange && halves)) {
        fail(LANE_MASK);
    }
    std::optional<std::size_t> made;
    if (auto d = def_block.find(scc); d != def_block.end()) {
        made = d->second;
    }
    std::size_t at;
    if (!made) {
        at = s.entry;
    } else if (*made == cur_ir) {
        at = cur;
    } else {
        if (!tail[*made]) {
            fail("a lane mask written after its use");
        }
        at = *tail[*made];
    }
    V bit = boolean(m, at);
    Ty i1t = s.t.i1, i32t = s.t.i32;
    V some = s.op(at, "waveAnyTrue", Attr::NoUnwind, i1t, 113, {bit});
    s.flags |= dxil::flags::WAVE_OPS;
    if (!(exchange && !exchange->small)) {
        return some;
    }
    if (!(!made || all_lanes(*made))) {
        need(made.value_or(0));
        fail("a lane mask exchanged where not every lane runs");
    }
    some = s.m.cast(at, Cast::ZExt, some, i32t);
    auto [lo, hi] = exchange_halves(at, some);
    V both = s.m.bin(at, Bin::Or, lo, hi);
    V z = s.m.i32(0);
    return s.m.cmp(at, pred::NE, both, z);
}

V Lower::wave_wide(std::size_t b, Bool when) {
    static constexpr Why WHY = "a wave-wide branch where not every lane runs";
    if (b >= st().wave_any.size() || !st().wave_any[b]) {
        fail("a wave-wide branch on a scalar condition");
    }
    bool any = *st().wave_any[b];
    if (!exchange && !halves && console_lanes && mask_data == MaskData::Console) {
        std::size_t c = cur;
        V mine = boolean(when, c);
        Ty i1t = s.t.i1;
        s.flags |= dxil::flags::WAVE_OPS;
        return any ? s.op(c, "waveAnyTrue", Attr::NoUnwind, i1t, 113, {mine}) : s.op(c, "waveAllTrue", Attr::NoUnwind, i1t, 114, {mine});
    }
    if (local || !(exchange && !exchange->small)) {
        fail(WHY);
    }
    if (!all_lanes(b)) {
        need(b);
        fail(WHY);
    }
    std::size_t c = cur;
    V mine = boolean(when, c);
    V set;
    if (any) {
        set = mine;
    } else {
        V t = s.m.boolean(true);
        set = s.m.bin(c, Bin::Xor, mine, t);
    }
    Ty i1t = s.t.i1, i32t = s.t.i32;
    V some = s.op(c, "waveAnyTrue", Attr::NoUnwind, i1t, 113, {set});
    s.flags |= dxil::flags::WAVE_OPS;
    some = s.m.cast(c, Cast::ZExt, some, i32t);
    auto [lo, hi] = exchange_halves(c, some);
    std::size_t cc = cur;
    V both = s.m.bin(cc, Bin::Or, lo, hi);
    V z = s.m.i32(0);
    return s.m.cmp(cc, any ? pred::NE : pred::EQ, both, z);
}

bool Lower::all_lanes(std::size_t b) const { return !exchange || (b < exchange->all.size() && exchange->all[b]); }

V Lower::lane_bit(std::size_t at, V lo, std::optional<V> hi) {
    s.flags |= dxil::flags::WAVE_OPS;
    V lane = exchange ? console_lane() : s.op(at, "waveGetLaneIndex", Attr::ReadOnly, s.t.i32, 111, {});
    V k31 = s.m.i32(31);
    V k32 = s.m.i32(32);
    V one = s.m.i32(1);
    V z = s.m.i32(0);
    V word = lo;
    if (hi) {
        V low = s.m.cmp(at, pred::IULT, lane, k32);
        word = s.m.select(at, low, lo, *hi);
    }
    V sh = s.m.bin(at, Bin::And, lane, k31);
    V t = s.m.bin(at, Bin::LShr, word, sh);
    t = s.m.bin(at, Bin::And, t, one);
    return s.m.cmp(at, pred::NE, t, z);
}

std::optional<Value> Lower::pair_high(Value lo) const {
    if (!x.native->wave64) {
        return std::nullopt;
    }
    const ir::Program& prog = p();
    auto pick = [](Reg reg) -> std::optional<Reg> {
        if (reg.kind == Reg::S) {
            return Reg::s(static_cast<std::uint8_t>(reg.n + 1));
        }
        return std::nullopt;
    };
    for (const auto& [reg, v] : prog.inputs) {
        if (v == lo) {
            auto h = pick(reg);
            if (!h) {
                return std::nullopt;
            }
            for (const auto& [r2, v2] : prog.inputs) {
                if (r2 == *h) {
                    return v2;
                }
            }
            return std::nullopt;
        }
    }
    if (auto it = phi_at.find(lo); it != phi_at.end()) {
        auto [b, i] = it->second;
        auto h = pick(prog.blocks[b].phis[i].reg);
        if (!h) {
            return std::nullopt;
        }
        for (const ir::Phi& q : prog.blocks[b].phis) {
            if (q.reg == *h) {
                return q.value;
            }
        }
        return std::nullopt;
    }
    auto d = def_block.find(lo);
    if (d == def_block.end()) {
        return std::nullopt;
    }
    for (const Inst& inst : prog.blocks[d->second].insts) {
        for (const auto& [reg, w] : inst.writes) {
            if (w == lo) {
                auto h = pick(reg);
                if (!h) {
                    return std::nullopt;
                }
                return inst.write(*h);
            }
        }
    }
    return std::nullopt;
}

std::optional<V> Lower::guard(std::size_t b, std::size_t i) {
    const auto& g = st().guards[b][i];
    if (!g) {
        return std::nullopt;
    }
    return boolean(*g, cur);
}


void Lower::set(std::optional<Value> v, Val val) {
    if (v) {
        vals[*v] = val;
    }
}

void Lower::write(const Inst& inst, Reg reg, V xv, std::optional<V> g) {
    if (auto d = sdwa_dst(inst); d && reg.kind == Reg::V) {
        xv = place_bits(inst, reg, xv, d->first, d->second);
    }
    write_placed(inst, reg, xv, g);
}

V Lower::old_or_zero(const Inst& inst, Reg reg) {
    auto o = inst.read(reg);
    if (o) {
        try {
            return i32_of(*o);
        } catch (const WhyError&) {
        }
    }
    return s.m.i32(0);
}

void Lower::write_placed(const Inst& inst, Reg reg, V xv, std::optional<V> g) {
    auto now = inst.write(reg);
    if (!now) {
        return;
    }
    if (g && reg.kind == Reg::V) {
        V old = old_or_zero(inst, reg);
        V v = s.m.select(cur, *g, xv, old);
        s.m.tag(v, Tag::Guard);
        xv = v;
    }
    vals[*now] = Val::i32(xv);
}

std::optional<std::pair<std::uint8_t, std::uint8_t>> Lower::sdwa_dst(const Inst& inst) {
    auto* v = std::get_if<Valu>(&inst.fields);
    if (!v || !v->sdwa) {
        return std::nullopt;
    }
    if (v->sdwa->dst_sel < 6) {
        return std::pair{v->sdwa->dst_sel, v->sdwa->dst_unused};
    }
    return std::nullopt;
}

V Lower::place_bits(const Inst& inst, Reg reg, V xv, std::uint8_t sel, std::uint8_t unused) {
    std::size_t c = cur;
    std::uint32_t width, at;
    if (sel < 4) {
        width = 8;
        at = 8u * sel;
    } else {
        width = 16;
        at = 16u * (sel - 4u);
    }
    V left = s.m.i32(32 - width);
    V t = s.m.bin(c, Bin::Shl, xv, left);
    V right = s.m.i32(32 - width - at);
    if (unused == 1) {
        return s.m.bin(c, Bin::AShr, t, right);
    }
    V field = s.m.bin(c, Bin::LShr, t, right);
    if (unused != 2) {
        return field;
    }
    V old = old_or_zero(inst, reg);
    V keep = s.m.i32(~(static_cast<std::uint32_t>((std::uint64_t{1} << width) - 1) << at));
    old = s.m.bin(c, Bin::And, old, keep);
    return s.m.bin(c, Bin::Or, old, field);
}

void Lower::write_f(const Inst& inst, V f, std::optional<V> g) {
    const auto& v = std::get<Valu>(inst.fields);
    if (v.omod != 0) {
        static const float factor[] = {1.0f, 2.0f, 4.0f, 0.5f};
        V k = s.m.f32(fbits(factor[v.omod]));
        f = s.m.bin(cur, Bin::Mul, f, k);
    }
    if (v.clamp) {
        f = unary(7, f);
    }
    if (!v.dst || v.dst->kind != Operand::Vector) {
        fail("a float result outside a VGPR");
    }
    std::uint8_t reg = v.dst->a;
    V xv = as_int(f);
    write(inst, Reg::v(reg), xv, g);
    if (!g && !sdwa_dst(inst)) {
        if (auto now = inst.write(Reg::v(reg))) {
            floats[*now] = f;
        }
    }
}

void Lower::write_i(const Inst& inst, V xv, std::optional<V> g) {
    const auto& v = std::get<Valu>(inst.fields);
    if (v.clamp || v.omod != 0) {
        fail("integer output modifiers");
    }
    if (v.dst && v.dst->kind == Operand::Vector) {
        write(inst, Reg::v(v.dst->a), xv, g);
    } else if (v.dst && v.dst->kind == Operand::Scalar) {
        write(inst, Reg::s(v.dst->a), xv, std::nullopt);
    } else {
        fail("a result without a register");
    }
}

std::optional<std::pair<std::size_t, std::size_t>> Lower::begin_guarded(std::optional<V> g) {
    if (!g) {
        return std::nullopt;
    }
    std::size_t then = s.m.new_block();
    std::size_t join = s.m.new_block();
    s.m.terminate(cur, Term::cond_br(*g, then, join));
    std::size_t head = cur;
    cur = then;
    return std::pair{head, join};
}

std::vector<V> Lower::end_guarded(std::optional<std::pair<std::size_t, std::size_t>> at, const std::vector<std::pair<V, V>>& values) {
    std::vector<V> out;
    if (!at) {
        for (const auto& v : values) {
            out.push_back(v.first);
        }
        return out;
    }
    auto [head, join] = *at;
    std::size_t then = cur;
    s.m.terminate(then, Term::br(join));
    cur = join;
    for (const auto& [inside, outside] : values) {
        Ty ty = s.m.type_of(inside);
        V phi = s.m.phi(join, ty);
        s.m.set_phi(phi, {{inside, then}, {outside, head}});
        out.push_back(phi);
    }
    return out;
}

void Lower::cross_lane(bool quad) {
    if (s.mesh) {
        fail("a cross-lane operation in a mesh shader");
    }
    if (!s.threads && !(quad && s.pixel)) {
        fail("a cross-lane operation outside compute");
    }
    s.flags |= dxil::flags::WAVE_OPS;
}

}   
