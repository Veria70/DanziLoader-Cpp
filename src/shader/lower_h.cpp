// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// 64-bit sources, the f64 to f32 conversion, 16-bit and packed 16-bit
// operations. See lower_impl.hpp.
#include <algorithm>
#include <bit>
#include <numbers>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

namespace {

std::uint32_t fb(float f) { return std::bit_cast<std::uint32_t>(f); }

bool among(std::string_view name, std::initializer_list<std::string_view> list) { return std::find(list.begin(), list.end(), name) != list.end(); }

constexpr std::initializer_list<std::string_view> FLOAT2 = {"v_add_f16", "v_sub_f16", "v_subrev_f16", "v_mul_f16", "v_max_f16", "v_min_f16"};
constexpr std::initializer_list<std::string_view> FLOAT3 = {"v_fma_f16", "v_mad_f16", "v_fmac_f16", "v_mac_f16", "v_max3_f16", "v_min3_f16", "v_med3_f16"};
constexpr std::initializer_list<std::string_view> FLOAT1 = {"v_rcp_f16",   "v_sqrt_f16",  "v_rsq_f16",  "v_log_f16",   "v_exp_f16",
                                                            "v_fract_f16", "v_trunc_f16", "v_floor_f16", "v_ceil_f16", "v_rndne_f16",
                                                            "v_sin_f16",   "v_cos_f16",   "v_cvt_f16_u16", "v_cvt_f16_i16"};
constexpr std::initializer_list<std::string_view> INT16 = {"v_add_nc_u16",  "v_sub_nc_u16",  "v_subrev_nc_u16", "v_mul_lo_u16",       "v_lshlrev_b16",
                                                           "v_lshrrev_b16", "v_ashrrev_i16", "v_max_u16",       "v_max_i16",          "v_min_u16",
                                                           "v_min_i16",     "v_cvt_u16_f16", "v_cvt_i16_f16",   "v_cvt_norm_i16_f16", "v_cvt_norm_u16_f16"};

}   

V Lower::src64(const Inst& inst, std::size_t k) {
    const auto& v = std::get<Valu>(inst.fields);
    if ((v.abs >> k & 1) || (v.neg >> k & 1) || v.sdwa || v.dpp) {
        fail("64-bit operand modifiers");
    }
    std::size_t c = cur;
    Ty i64t = s.m.int_(64);
    s.flags |= dxil::flags::INT64;
    if (k >= v.src.size()) {
        fail("a missing operand");
    }
    Operand op = v.src[k];
    auto pair_of = [&](Reg r, Reg r1) {
        auto a = inst.read(r);
        if (!a) {
            fail("an unread operand");
        }
        V lo = i32_of(*a);
        auto b = inst.read(r1);
        if (!b) {
            fail("an unread operand");
        }
        V hi = i32_of(*b);
        return join64(lo, hi);
    };
    if (op.kind == Operand::Vector) {
        return pair_of(Reg::v(op.a), Reg::v(static_cast<std::uint8_t>(op.a + 1)));
    }
    if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
        return pair_of(Reg::s(op.a), Reg::s(static_cast<std::uint8_t>(op.a + 1)));
    }
    if (op.kind == Operand::Const && op.a >= 128 && op.a <= 208) {
        auto xv = resource::inline_constant(op.a);
        if (!xv) {
            fail("a special operand");
        }
        return s.m.int_const(i64t, static_cast<std::int64_t>(static_cast<std::int32_t>(*xv)));
    }
    V xv = operand(inst, op);
    return s.m.cast(c, Cast::ZExt, xv, i64t);
}

std::pair<V, V> Lower::f64_pair(const Inst& inst, std::size_t k) {
    const auto& v = std::get<Valu>(inst.fields);
    if (v.sdwa || v.dpp) {
        fail("64-bit operand modifiers");
    }
    if (k >= v.src.size()) {
        fail("a missing operand");
    }
    Operand op = v.src[k];
    auto pair_of = [&](Reg r, Reg r1) {
        auto a = inst.read(r);
        if (!a) {
            fail("an unread operand");
        }
        V lo = i32_of(*a);
        auto b = inst.read(r1);
        if (!b) {
            fail("an unread operand");
        }
        V hi = i32_of(*b);
        return std::pair{lo, hi};
    };
    V lo, hi;
    if (op.kind == Operand::Vector) {
        std::tie(lo, hi) = pair_of(Reg::v(op.a), Reg::v(static_cast<std::uint8_t>(op.a + 1)));
    } else if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
        std::tie(lo, hi) = pair_of(Reg::s(op.a), Reg::s(static_cast<std::uint8_t>(op.a + 1)));
    } else if (op.kind == Operand::Const && op.a == 128) {
        lo = s.m.i32(0);
        hi = s.m.i32(0);
    } else if (op.kind == Operand::Const && op.a >= 240 && op.a <= 248) {
        static const double INLINE_F64[9] = {0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0, 1.0 / (2.0 * std::numbers::pi)};
        std::uint64_t bits = std::bit_cast<std::uint64_t>(INLINE_F64[op.a - 240]);
        lo = s.m.i32(static_cast<std::uint32_t>(bits));
        hi = s.m.i32(static_cast<std::uint32_t>(bits >> 32));
    } else if (op.kind == Operand::Literal) {
        lo = s.m.i32(0);
        hi = s.m.i32(op.literal);
    } else {
        fail("a 64-bit float operand");
    }
    std::size_t c = cur;
    if (v.abs >> k & 1) {
        V mask = s.m.i32(0x7FFFFFFF);
        hi = s.m.bin(c, Bin::And, hi, mask);
    }
    if (v.neg >> k & 1) {
        V sign = s.m.i32(0x80000000u);
        hi = s.m.bin(c, Bin::Xor, hi, sign);
    }
    return {lo, hi};
}

V Lower::f64_to_f32_bits(V lo, V hi) {
    std::size_t c = cur;
    auto& m = s.m;
    V k0 = m.i32(0);
    V k1 = m.i32(1);
    V sign_mask = m.i32(0x80000000u);
    V sign = m.bin(c, Bin::And, hi, sign_mask);
    V k20 = m.i32(20);
    V k7ff = m.i32(0x7FF);
    V exp64 = m.bin(c, Bin::LShr, hi, k20);
    exp64 = m.bin(c, Bin::And, exp64, k7ff);
    V frac_mask = m.i32(0x000FFFFF);
    V frac_hi = m.bin(c, Bin::And, hi, frac_mask);
    V frac_any = m.bin(c, Bin::Or, frac_hi, lo);
    V frac_nz = m.cmp(c, pred::NE, frac_any, k0);
    V k3 = m.i32(3);
    V k29 = m.i32(29);
    V m23_hi = m.bin(c, Bin::Shl, frac_hi, k3);
    V m23_lo = m.bin(c, Bin::LShr, lo, k29);
    V m23 = m.bin(c, Bin::Or, m23_hi, m23_lo);
    V rem_mask = m.i32(0x1FFFFFFF);
    V rem = m.bin(c, Bin::And, lo, rem_mask);
    V half_rem = m.i32(0x10000000);
    V gt_half = m.cmp(c, pred::IUGT, rem, half_rem);
    V eq_half = m.cmp(c, pred::EQ, rem, half_rem);
    V m23_odd_bit = m.bin(c, Bin::And, m23, k1);
    V m23_odd = m.cmp(c, pred::NE, m23_odd_bit, k0);
    V tie_up = m.bin(c, Bin::And, eq_half, m23_odd);
    V norm_round = m.bin(c, Bin::Or, gt_half, tie_up);
    V norm_inc = m.select(c, norm_round, k1, k0);
    V k896 = m.i32(896);
    V k23 = m.i32(23);
    V exp32 = m.bin(c, Bin::Sub, exp64, k896);
    V exp32_sh = m.bin(c, Bin::Shl, exp32, k23);
    V norm_body = m.bin(c, Bin::Add, exp32_sh, m23);
    norm_body = m.bin(c, Bin::Add, norm_body, norm_inc);
    V norm_res = m.bin(c, Bin::Or, sign, norm_body);
    V k897 = m.i32(897);
    V k32 = m.i32(32);
    V k31 = m.i32(31);
    V imp = m.i32(0x00800000);
    V sig24 = m.bin(c, Bin::Or, imp, m23);
    V raw_shift = m.bin(c, Bin::Sub, k897, exp64);
    V lt1 = m.cmp(c, pred::IULT, raw_shift, k1);
    V shift = m.select(c, lt1, k1, raw_shift);
    V gt23 = m.cmp(c, pred::IUGT, shift, k23);
    shift = m.select(c, gt23, k23, shift);
    V sub_m = m.bin(c, Bin::LShr, sig24, shift);
    V inv_shift = m.bin(c, Bin::Sub, k32, shift);
    V lost = m.bin(c, Bin::Shl, sig24, inv_shift);
    V round_bit = m.bin(c, Bin::LShr, lost, k31);
    V round_on = m.cmp(c, pred::NE, round_bit, k0);
    V low31 = m.i32(0x7FFFFFFF);
    V lost_lo = m.bin(c, Bin::And, lost, low31);
    V sticky = m.bin(c, Bin::Or, lost_lo, rem);
    V sticky_on = m.cmp(c, pred::NE, sticky, k0);
    V sub_odd_bit = m.bin(c, Bin::And, sub_m, k1);
    V sub_odd = m.cmp(c, pred::NE, sub_odd_bit, k0);
    V sub_tie = m.bin(c, Bin::Or, sticky_on, sub_odd);
    V sub_round = m.bin(c, Bin::And, round_on, sub_tie);
    V sub_inc = m.select(c, sub_round, k1, k0);
    V sub_body = m.bin(c, Bin::Add, sub_m, sub_inc);
    V sub_res = m.bin(c, Bin::Or, sign, sub_body);
    V k873 = m.i32(873);
    V eq873 = m.cmp(c, pred::EQ, exp64, k873);
    V round_min = m.bin(c, Bin::And, eq873, frac_nz);
    V min_bit = m.select(c, round_min, k1, k0);
    V under_res = m.bin(c, Bin::Or, sign, min_bit);
    V is_exp_all = m.cmp(c, pred::EQ, exp64, k7ff);
    V is_nan = m.bin(c, Bin::And, is_exp_all, frac_nz);
    V inf_bits = m.i32(0x7F800000);
    V qnan_bits = m.i32(0x7FC00000);
    V nan_body = m.bin(c, Bin::Or, qnan_bits, m23);
    V over_body = m.select(c, is_nan, nan_body, inf_bits);
    V over_res = m.bin(c, Bin::Or, sign, over_body);
    V k874 = m.i32(874);
    V k1151 = m.i32(1151);
    V is_under = m.cmp(c, pred::IULT, exp64, k874);
    V is_sub = m.cmp(c, pred::IULT, exp64, k897);
    V is_over = m.cmp(c, pred::IUGE, exp64, k1151);
    V r = m.select(c, is_over, over_res, norm_res);
    r = m.select(c, is_sub, sub_res, r);
    return m.select(c, is_under, under_res, r);
}

V Lower::join64(V lo, V hi) {
    std::size_t c = cur;
    Ty ty = s.m.int_(64);
    lo = s.m.cast(c, Cast::ZExt, lo, ty);
    hi = s.m.cast(c, Cast::ZExt, hi, ty);
    V shift = s.m.int_const(ty, 32);
    hi = s.m.bin(c, Bin::Shl, hi, shift);
    return s.m.bin(c, Bin::Or, lo, hi);
}

std::pair<V, V> Lower::split64(V xv) {
    std::size_t c = cur;
    Ty i64t = s.m.int_(64);
    V k32 = s.m.int_const(i64t, 32);
    V lo = s.m.cast(c, Cast::Trunc, xv, s.t.i32);
    V hi = s.m.bin(c, Bin::LShr, xv, k32);
    hi = s.m.cast(c, Cast::Trunc, hi, s.t.i32);
    return {lo, hi};
}

V Lower::half_src(const Inst& inst, std::size_t k) {
    const auto& v = std::get<Valu>(inst.fields);
    if (k >= v.src.size()) {
        fail("a missing operand");
    }
    Operand op = v.src[k];
    bool high = (v.opsel >> k & 1) != 0;
    if (v.sdwa) {
        std::uint8_t sel = k < 2 ? v.sdwa->src_sel[k] : 6;
        if (sel == 5) {
            high = true;
        } else if (sel != 4 && sel != 6) {
            fail("a byte select on a 16-bit operation");
        }
    }
    V xv = (src0 && k == 0) ? *src0 : f16_operand(inst, op);
    std::size_t c = cur;
    if (high) {
        V k16 = s.m.i32(16);
        xv = s.m.bin(c, Bin::LShr, xv, k16);
    }
    V m = s.m.i32(0xFFFF);
    return s.m.bin(c, Bin::And, xv, m);
}

V Lower::half_fsrc(const Inst& inst, std::size_t k) {
    const auto& v = std::get<Valu>(inst.fields);
    V h = half_src(inst, k);
    V f = s.op(cur, "legacyF16ToF32", Attr::ReadNone, s.t.f32, 131, {h});
    if (v.abs >> k & 1) {
        f = unary(6, f);
    }
    if (v.neg >> k & 1) {
        V z = s.m.f32(0x80000000u);
        f = s.m.bin(cur, Bin::Sub, z, f);
    }
    return f;
}

void Lower::write_half(const Inst& inst, V xv, std::optional<V> g) {
    const auto& v = std::get<Valu>(inst.fields);
    if (!v.dst || v.dst->kind != Operand::Vector) {
        fail("a 16-bit result outside a VGPR");
    }
    std::uint8_t reg = v.dst->a;
    bool high = (v.opsel >> 3 & 1) != 0;
    bool keep = true;
    if (v.sdwa) {
        std::uint8_t sel = v.sdwa->dst_sel, unused = v.sdwa->dst_unused;
        if (sel == 4 && unused == 2) {
        } else if (sel == 5 && unused == 2) {
            high = true;
        } else if (sel == 4 || sel == 6) {
            keep = false;
        } else if (sel == 5) {
            high = true;
            keep = false;
        } else {
            fail("an SDWA destination of a 16-bit operation");
        }
    }
    std::size_t c = cur;
    std::optional<V> got;
    if (auto o = inst.read(Reg::v(reg))) {
        try {
            got = i32_of(*o);
        } catch (const WhyError&) {
        }
    }
    V old = got && keep ? *got : s.m.i32(0);
    V k16 = s.m.i32(16);
    V low = s.m.i32(0xFFFF);
    xv = s.m.bin(c, Bin::And, xv, low);
    V r;
    if (high) {
        V o = s.m.bin(c, Bin::And, old, low);
        V xs = s.m.bin(c, Bin::Shl, xv, k16);
        r = s.m.bin(c, Bin::Or, o, xs);
    } else {
        V kk = s.m.i32(0xFFFF0000u);
        V o = s.m.bin(c, Bin::And, old, kk);
        r = s.m.bin(c, Bin::Or, o, xv);
    }
    write_placed(inst, Reg::v(reg), r, g);
}

bool Lower::valu16(const Inst& inst, std::string_view name, std::optional<V> g) {
    if (!among(name, FLOAT1) && !among(name, FLOAT2) && !among(name, FLOAT3) && !among(name, INT16) && name != "v_cvt_f16_f32" &&
        name != "v_cvt_f32_f16") {
        return false;
    }
    valu16_op(inst, name, g);
    return true;
}

void Lower::valu16_op(const Inst& inst, std::string_view name, std::optional<V> g) {
    const auto& v = std::get<Valu>(inst.fields);
    std::size_t c = cur;
    Ty i32t = s.t.i32, f32t = s.t.f32;
    if (name == "v_cvt_f32_f16") {
        V f = half_fsrc(inst, 0);
        write_f(inst, f, g);
        return;
    }
    auto float_result = [&](V f) {
        if (v.omod != 0) {
            static const float factor[] = {1.0f, 2.0f, 4.0f, 0.5f};
            V k = s.m.f32(fb(factor[v.omod]));
            f = s.m.bin(cur, Bin::Mul, f, k);
        }
        if (v.clamp) {
            f = unary(7, f);
        }
        V h = f16_nearest_on() ? f16_nearest(f) : s.op(cur, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {f});
        write_half(inst, h, g);
    };
    if (name == "v_cvt_f16_f32") {
        float_result(fsrc(inst, 0));
        return;
    }
    if (among(name, FLOAT2)) {
        V a = half_fsrc(inst, 0);
        V b = half_fsrc(inst, 1);
        V r;
        if (name == "v_add_f16") {
            r = s.m.bin(c, Bin::Add, a, b);
        } else if (name == "v_sub_f16") {
            r = s.m.bin(c, Bin::Sub, a, b);
        } else if (name == "v_subrev_f16") {
            r = s.m.bin(c, Bin::Sub, b, a);
        } else if (name == "v_mul_f16") {
            r = s.m.bin(c, Bin::Mul, a, b);
        } else if (name == "v_max_f16") {
            r = max_min(true, a, b);
        } else {
            r = max_min(false, a, b);
        }
        float_result(r);
        return;
    }
    if (among(name, FLOAT3)) {
        V a = half_fsrc(inst, 0);
        V b = half_fsrc(inst, 1);
        V acc;
        if (name == "v_fmac_f16" || name == "v_mac_f16") {
            if (!v.dst || v.dst->kind != Operand::Vector) {
                fail("an accumulator");
            }
            auto av = inst.read(Reg::v(v.dst->a));
            if (!av) {
                fail("an accumulator");
            }
            V xv = i32_of(*av);
            if (v.opsel >> 3 & 1) {
                V k16 = s.m.i32(16);
                xv = s.m.bin(c, Bin::LShr, xv, k16);
            }
            V m = s.m.i32(0xFFFF);
            xv = s.m.bin(c, Bin::And, xv, m);
            acc = s.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {xv});
        } else {
            acc = half_fsrc(inst, 2);
        }
        V r;
        if (name == "v_fma_f16" || name == "v_fmac_f16" || name == "v_mad_f16" || name == "v_mac_f16") {
            r = fmad(a, b, acc);
        } else if (name == "v_max3_f16") {
            V t = max_min(true, a, b);
            r = max_min(true, t, acc);
        } else if (name == "v_min3_f16") {
            V t = max_min(false, a, b);
            r = max_min(false, t, acc);
        } else {
            r = med3(a, b, acc);
        }
        float_result(r);
        return;
    }
    if (among(name, FLOAT1)) {
        V r;
        if (name == "v_cvt_f16_u16" || name == "v_cvt_f16_i16") {
            V xv = half_src(inst, 0);
            Cast op = Cast::UiToFp;
            if (name == "v_cvt_f16_i16") {
                V k16 = s.m.i32(16);
                V t = s.m.bin(c, Bin::Shl, xv, k16);
                xv = s.m.bin(c, Bin::AShr, t, k16);
                op = Cast::SiToFp;
            }
            r = s.m.cast(c, op, xv, f32t);
        } else if (name == "v_rcp_f16") {
            V a = half_fsrc(inst, 0);
            V one = s.m.f32(fb(1.0f));
            r = s.m.bin(c, Bin::SDiv, one, a);
        } else {
            V a = half_fsrc(inst, 0);
            std::uint32_t op = name == "v_sqrt_f16"    ? 24
                               : name == "v_rsq_f16"   ? 25
                               : name == "v_log_f16"   ? 23
                               : name == "v_exp_f16"   ? 21
                               : name == "v_fract_f16" ? 22
                               : name == "v_trunc_f16" ? 29
                               : name == "v_floor_f16" ? 27
                               : name == "v_ceil_f16"  ? 28
                               : name == "v_rndne_f16" ? 26
                               : name == "v_sin_f16"   ? 13
                                                       : 12;
            if (op == 12 || op == 13) {
                r = trig_turns(a, op == 12);
            } else if (op == 22) {
                r = fract(a, 1.0f - 1.0f / 4096.0f, 1.0f - 1.0f / 2048.0f);
            } else {
                r = unary(op, a);
            }
        }
        float_result(r);
        return;
    }
    if (name == "v_cvt_u16_f16" || name == "v_cvt_i16_f16") {
        if (v.clamp || v.omod != 0) {
            fail("modifiers on a 16-bit integer operation");
        }
        V a = half_fsrc(inst, 0);
        float lo_f = name == "v_cvt_u16_f16" ? 0.0f : -32768.0f;
        float hi_f = name == "v_cvt_u16_f16" ? 65535.0f : 32767.0f;
        V lo = s.m.f32(fb(lo_f));
        V hi = s.m.f32(fb(hi_f));
        V t = binary(35, a, lo);
        t = binary(36, t, hi);
        V r = s.m.cast(c, Cast::FpToSi, t, i32t);
        V nan = s.m.cmp(c, pred::UNO, a, a);
        V z = s.m.i32(0);
        r = s.m.select(c, nan, z, r);
        write_half(inst, r, g);
        return;
    }
    if (name == "v_cvt_norm_i16_f16" || name == "v_cvt_norm_u16_f16") {
        if (v.clamp || v.omod != 0) {
            fail("modifiers on a 16-bit integer operation");
        }
        V r = cvt_norm16(inst, name == "v_cvt_norm_i16_f16");
        write_half(inst, r, g);
        return;
    }
    if (v.clamp || v.omod != 0 || v.abs != 0 || v.neg != 0) {
        fail("modifiers on a 16-bit integer operation");
    }
    V a = half_src(inst, 0);
    V b = half_src(inst, 1);
    V k16 = s.m.i32(16);
    auto sext = [&](V t) {
        V u = s.m.bin(c, Bin::Shl, t, k16);
        return s.m.bin(c, Bin::AShr, u, k16);
    };
    if (name.ends_with("_i16")) {
        a = sext(a);
        b = sext(b);
    }
    V k15 = s.m.i32(15);
    V r;
    if (name == "v_add_nc_u16") {
        r = s.m.bin(c, Bin::Add, a, b);
    } else if (name == "v_sub_nc_u16") {
        r = s.m.bin(c, Bin::Sub, a, b);
    } else if (name == "v_subrev_nc_u16") {
        r = s.m.bin(c, Bin::Sub, b, a);
    } else if (name == "v_mul_lo_u16") {
        r = s.m.bin(c, Bin::Mul, a, b);
    } else if (name == "v_lshlrev_b16" || name == "v_lshrrev_b16" || name == "v_ashrrev_i16") {
        V sh = s.m.bin(c, Bin::And, a, k15);
        V xv = name == "v_ashrrev_i16" ? sext(b) : b;
        Bin op = name == "v_lshlrev_b16" ? Bin::Shl : name == "v_lshrrev_b16" ? Bin::LShr : Bin::AShr;
        r = s.m.bin(c, op, xv, sh);
    } else {
        std::uint8_t pr = name == "v_max_u16" ? pred::IUGT : name == "v_min_u16" ? pred::IULT : name == "v_max_i16" ? pred::SGT : pred::SLT;
        V t = s.m.cmp(c, pr, a, b);
        r = s.m.select(c, t, a, b);
    }
    write_half(inst, r, g);
}

V Lower::cvt_norm16(const Inst& inst, bool is_signed) {
    const auto& v = std::get<Valu>(inst.fields);
    std::size_t c = cur;
    auto& m = s.m;
    V h = half_src(inst, 0);
    if (v.abs & 1) {
        V mm = m.i32(0x7FFF);
        h = m.bin(c, Bin::And, h, mm);
    }
    if (v.neg & 1) {
        V mm = m.i32(0x8000);
        h = m.bin(c, Bin::Xor, h, mm);
    }
    std::uint32_t scale = is_signed ? 0x7FFF : 0xFFFF;
    V k0 = m.i32(0);
    V k1 = m.i32(1);
    V k10 = m.i32(10);
    V mag_mask = m.i32(0x7FFF);
    V mag = m.bin(c, Bin::And, h, mag_mask);
    V k15 = m.i32(15);
    V neg = m.bin(c, Bin::LShr, h, k15);
    neg = m.cmp(c, pred::NE, neg, k0);
    V ex = m.bin(c, Bin::LShr, mag, k10);
    V man_mask = m.i32(0x3FF);
    V man = m.bin(c, Bin::And, mag, man_mask);
    V implicit = m.i32(0x400);
    V normal = m.bin(c, Bin::Or, man, implicit);
    V sub = m.cmp(c, pred::EQ, ex, k0);
    V mv = m.select(c, sub, man, normal);
    V k25 = m.i32(25);
    V s_norm = m.bin(c, Bin::Sub, k25, ex);
    V k24 = m.i32(24);
    V sh = m.select(c, sub, k24, s_norm);
    V ks = m.i32(scale);
    V p = m.bin(c, Bin::Mul, mv, ks);
    V q = m.bin(c, Bin::LShr, p, sh);
    V one_s = m.bin(c, Bin::Shl, k1, sh);
    V low_mask = m.bin(c, Bin::Sub, one_s, k1);
    V rem = m.bin(c, Bin::And, p, low_mask);
    V half = m.bin(c, Bin::LShr, one_s, k1);
    V above = m.cmp(c, pred::IUGT, rem, half);
    V tie = m.cmp(c, pred::EQ, rem, half);
    V odd = m.bin(c, Bin::And, q, k1);
    odd = m.cmp(c, pred::NE, odd, k0);
    V tie_up = m.bin(c, Bin::And, tie, odd);
    V up = m.bin(c, Bin::Or, above, tie_up);
    V r = m.select(c, up, k1, k0);
    r = m.bin(c, Bin::Add, q, r);
    V one_h = m.i32(0x3C00);
    V big = m.cmp(c, pred::IUGE, mag, one_h);
    r = m.select(c, big, ks, r);
    V inf = m.i32(0x7C00);
    V nan = m.cmp(c, pred::IUGT, mag, inf);
    r = m.select(c, nan, k0, r);
    if (is_signed) {
        V n = m.bin(c, Bin::Sub, k0, r);
        r = m.select(c, neg, n, r);
    } else {
        r = m.select(c, neg, k0, r);
    }
    V m16 = m.i32(0xFFFF);
    return m.bin(c, Bin::And, r, m16);
}

V Lower::packed_src(const Inst& inst, std::size_t k, bool high, bool is_float) {
    const auto& v = std::get<Valu>(inst.fields);
    if (k >= v.src.size()) {
        fail("a missing operand");
    }
    Operand op = v.src[k];
    bool sel = ((high ? v.opsel_hi : v.opsel) >> k & 1) != 0;
    V xv = is_float ? f16_operand(inst, op) : operand(inst, op);
    std::size_t c = cur;
    if (sel) {
        V k16 = s.m.i32(16);
        xv = s.m.bin(c, Bin::LShr, xv, k16);
    }
    V m = s.m.i32(0xFFFF);
    return s.m.bin(c, Bin::And, xv, m);
}

void Lower::packed16(const Inst& inst, std::string_view op, std::optional<V> g) {
    const auto& v = std::get<Valu>(inst.fields);
    bool is_float = op.ends_with("_f16");
    std::size_t n;
    if (op == "fma_f16" || op == "mad_u16" || op == "mad_i16") {
        n = 3;
    } else if (among(op, {"add_f16", "mul_f16", "min_f16", "max_f16", "add_u16", "sub_u16", "add_i16", "sub_i16", "mul_lo_u16", "lshlrev_b16",
                          "lshrrev_b16", "ashrrev_i16", "max_u16", "min_u16", "max_i16", "min_i16"})) {
        n = 2;
    } else {
        fail("a packed 16-bit operation");
    }
    if (v.omod != 0 || (!is_float && (v.clamp || v.neg != 0 || v.neg_hi != 0))) {
        fail("modifiers on a packed integer operation");
    }
    std::size_t c = cur;
    Ty i32t = s.t.i32, f32t = s.t.f32;
    std::vector<V> halves_v;
    for (bool high : {false, true}) {
        std::vector<V> xs;
        for (std::size_t k = 0; k < n; ++k) {
            V h = packed_src(inst, k, high, is_float);
            if (is_float) {
                V f = s.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {h});
                std::uint8_t neg = high ? v.neg_hi : v.neg;
                if (neg >> k & 1) {
                    V z = s.m.f32(0x80000000u);
                    f = s.m.bin(c, Bin::Sub, z, f);
                }
                xs.push_back(f);
            } else {
                xs.push_back(h);
            }
        }
        V r;
        if (is_float) {
            V f;
            if (op == "add_f16") {
                f = s.m.bin(c, Bin::Add, xs[0], xs[1]);
            } else if (op == "mul_f16") {
                f = s.m.bin(c, Bin::Mul, xs[0], xs[1]);
            } else if (op == "min_f16") {
                f = max_min(false, xs[0], xs[1]);
            } else if (op == "max_f16") {
                f = max_min(true, xs[0], xs[1]);
            } else {
                f = fmad(xs[0], xs[1], xs[2]);
            }
            if (v.clamp) {
                f = unary(7, f);
            }
            r = s.op(c, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {f});
        } else {
            V k16 = s.m.i32(16);
            auto sext = [&](V t) {
                V u = s.m.bin(c, Bin::Shl, t, k16);
                return s.m.bin(c, Bin::AShr, u, k16);
            };
            if (op.ends_with("_i16")) {
                for (V& xv : xs) {
                    xv = sext(xv);
                }
            }
            V k15 = s.m.i32(15);
            if (op == "add_u16" || op == "add_i16") {
                r = s.m.bin(c, Bin::Add, xs[0], xs[1]);
            } else if (op == "sub_u16" || op == "sub_i16") {
                r = s.m.bin(c, Bin::Sub, xs[0], xs[1]);
            } else if (op == "mul_lo_u16") {
                r = s.m.bin(c, Bin::Mul, xs[0], xs[1]);
            } else if (op == "mad_u16" || op == "mad_i16") {
                V mm = s.m.bin(c, Bin::Mul, xs[0], xs[1]);
                r = s.m.bin(c, Bin::Add, mm, xs[2]);
            } else if (op == "lshlrev_b16" || op == "lshrrev_b16" || op == "ashrrev_i16") {
                V sh = s.m.bin(c, Bin::And, xs[0], k15);
                Bin o = op == "lshlrev_b16" ? Bin::Shl : op == "lshrrev_b16" ? Bin::LShr : Bin::AShr;
                V xv = op == "ashrrev_i16" ? sext(xs[1]) : xs[1];
                r = s.m.bin(c, o, xv, sh);
            } else {
                std::uint8_t pr = op == "max_u16" ? pred::IUGT : op == "min_u16" ? pred::IULT : op == "max_i16" ? pred::SGT : pred::SLT;
                V t = s.m.cmp(c, pr, xs[0], xs[1]);
                r = s.m.select(c, t, xs[0], xs[1]);
            }
        }
        V m = s.m.i32(0xFFFF);
        halves_v.push_back(s.m.bin(c, Bin::And, r, m));
    }
    V k16 = s.m.i32(16);
    V hi = s.m.bin(c, Bin::Shl, halves_v[1], k16);
    V r = s.m.bin(c, Bin::Or, halves_v[0], hi);
    if (!v.dst || v.dst->kind != Operand::Vector) {
        fail("a packed result outside a VGPR");
    }
    write(inst, Reg::v(v.dst->a), r, g);
}

}   
