// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// integer vector operations, compares and the lane bits they write. See
// lower_impl.hpp.
#include <algorithm>
#include <bit>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

V Lower::integer(const Inst& inst, std::string_view name) {
    std::size_t c = cur;
    auto sv = [&](std::size_t k) { return src(inst, k); };
    auto k = [&](std::uint32_t xv) { return s.m.i32(xv); };
    auto m31 = [&](V xv) {
        V k31 = s.m.i32(31);
        return s.m.bin(c, Bin::And, xv, k31);
    };
    if (name == "v_and_b32" || name == "v_or_b32" || name == "v_xor_b32" || name == "v_add_nc_u32" || name == "v_sub_nc_u32" ||
        name == "v_subrev_nc_u32" || name == "v_mul_lo_u32") {
        V a = sv(0);
        V b = sv(1);
        if (name == "v_and_b32") {
            return s.m.bin(c, Bin::And, a, b);
        }
        if (name == "v_or_b32") {
            return s.m.bin(c, Bin::Or, a, b);
        }
        if (name == "v_xor_b32") {
            return s.m.bin(c, Bin::Xor, a, b);
        }
        if (name == "v_add_nc_u32") {
            return s.m.bin(c, Bin::Add, a, b);
        }
        if (name == "v_sub_nc_u32") {
            return s.m.bin(c, Bin::Sub, a, b);
        }
        if (name == "v_subrev_nc_u32") {
            return s.m.bin(c, Bin::Sub, b, a);
        }
        return s.m.bin(c, Bin::Mul, a, b);
    }
    if (name == "v_xnor_b32") {
        V a = sv(0);
        V b = sv(1);
        V xv = s.m.bin(c, Bin::Xor, a, b);
        V ones = k(UINT32_MAX);
        return s.m.bin(c, Bin::Xor, xv, ones);
    }
    if (name == "v_not_b32") {
        V a = sv(0);
        V ones = k(UINT32_MAX);
        return s.m.bin(c, Bin::Xor, a, ones);
    }
    if (name == "v_lshlrev_b32" || name == "v_lshrrev_b32" || name == "v_ashrrev_i32") {
        V sh = sv(0);
        V a = sv(1);
        sh = m31(sh);
        Bin op = name == "v_lshlrev_b32" ? Bin::Shl : name == "v_lshrrev_b32" ? Bin::LShr : Bin::AShr;
        return s.m.bin(c, op, a, sh);
    }
    if (name == "v_lshl_add_u32" || name == "v_lshl_or_b32" || name == "v_add_lshl_u32" || name == "v_and_or_b32" || name == "v_or3_b32" ||
        name == "v_xad_u32" || name == "v_add3_u32" || name == "v_xor3_b32") {
        V a = sv(0);
        V b = sv(1);
        V d = sv(2);
        if (name == "v_lshl_add_u32" || name == "v_lshl_or_b32") {
            b = m31(b);
            V t = s.m.bin(c, Bin::Shl, a, b);
            return s.m.bin(c, name == "v_lshl_add_u32" ? Bin::Add : Bin::Or, t, d);
        }
        if (name == "v_add_lshl_u32") {
            V t = s.m.bin(c, Bin::Add, a, b);
            d = m31(d);
            return s.m.bin(c, Bin::Shl, t, d);
        }
        Bin o1, o2;
        if (name == "v_and_or_b32") {
            o1 = Bin::And;
            o2 = Bin::Or;
        } else if (name == "v_or3_b32") {
            o1 = Bin::Or;
            o2 = Bin::Or;
        } else if (name == "v_xad_u32") {
            o1 = Bin::Xor;
            o2 = Bin::Add;
        } else if (name == "v_add3_u32") {
            o1 = Bin::Add;
            o2 = Bin::Add;
        } else {
            o1 = Bin::Xor;
            o2 = Bin::Xor;
        }
        V t = s.m.bin(c, o1, a, b);
        return s.m.bin(c, o2, t, d);
    }
    if (name == "v_mul_u32_u24" || name == "v_mul_i32_i24") {
        V a = sv(0);
        V b = sv(1);
        V sh = k(8);
        Bin op = name == "v_mul_i32_i24" ? Bin::AShr : Bin::LShr;
        a = s.m.bin(c, Bin::Shl, a, sh);
        a = s.m.bin(c, op, a, sh);
        b = s.m.bin(c, Bin::Shl, b, sh);
        b = s.m.bin(c, op, b, sh);
        return s.m.bin(c, Bin::Mul, a, b);
    }
    if (name == "v_bfe_u32" || name == "v_bfe_i32") {
        V a = sv(0);
        V off = sv(1);
        V w = sv(2);
        w = m31(w);
        return bfe(a, off, w, name == "v_bfe_i32");
    }
    if (name == "v_bfi_b32") {
        V m = sv(0);
        V a = sv(1);
        V b = sv(2);
        V ones = k(UINT32_MAX);
        V nm = s.m.bin(c, Bin::Xor, m, ones);
        V xv = s.m.bin(c, Bin::And, m, a);
        V y = s.m.bin(c, Bin::And, nm, b);
        return s.m.bin(c, Bin::Or, xv, y);
    }
    if (name == "v_max_i32" || name == "v_min_i32" || name == "v_max_u32" || name == "v_min_u32") {
        V a = sv(0);
        V b = sv(1);
        std::uint8_t p = name == "v_max_i32" ? pred::SGT : name == "v_min_i32" ? pred::SLT : name == "v_max_u32" ? pred::IUGT : pred::IULT;
        V t = s.m.cmp(c, p, a, b);
        return s.m.select(c, t, a, b);
    }
    if (name == "v_mul_hi_u32" || name == "v_mul_hi_i32") {
        V a = sv(0);
        V b = sv(1);
        return mul_hi(a, b, name == "v_mul_hi_i32");
    }
    if (name == "v_alignbit_b32" || name == "v_alignbyte_b32") {
        V a = sv(0);
        V b = sv(1);
        V sh = sv(2);
        if (name == "v_alignbyte_b32") {
            V three = k(3);
            V t = s.m.bin(c, Bin::And, sh, three);
            V k3 = k(3);
            sh = s.m.bin(c, Bin::Shl, t, k3);
        } else {
            sh = m31(sh);
        }
        V k32 = k(32);
        V lo = s.m.bin(c, Bin::LShr, b, sh);
        V left = s.m.bin(c, Bin::Sub, k32, sh);
        left = m31(left);
        V hi = s.m.bin(c, Bin::Shl, a, left);
        V zero = k(0);
        V none = s.m.cmp(c, pred::EQ, sh, zero);
        hi = s.m.select(c, none, zero, hi);
        return s.m.bin(c, Bin::Or, lo, hi);
    }
    if (name == "v_bfrev_b32") {
        V a = sv(0);
        return s.op(c, "unary.i32", Attr::ReadNone, s.t.i32, 30, {a});
    }
    if (name == "v_bcnt_u32_b32") {
        V a = sv(0);
        V b = sv(1);
        V n = s.op(c, "unaryBits.i32", Attr::ReadNone, s.t.i32, 31, {a});
        return s.m.bin(c, Bin::Add, n, b);
    }
    if (name == "v_mad_u32_u24" || name == "v_mad_i32_i24") {
        V a = sv(0);
        V b = sv(1);
        V d = sv(2);
        V sh = k(8);
        Bin op = name == "v_mad_i32_i24" ? Bin::AShr : Bin::LShr;
        a = s.m.bin(c, Bin::Shl, a, sh);
        a = s.m.bin(c, op, a, sh);
        b = s.m.bin(c, Bin::Shl, b, sh);
        b = s.m.bin(c, op, b, sh);
        V m = s.m.bin(c, Bin::Mul, a, b);
        return s.m.bin(c, Bin::Add, m, d);
    }
    if (name == "v_min3_i32" || name == "v_max3_i32" || name == "v_min3_u32" || name == "v_max3_u32" || name == "v_med3_i32" || name == "v_med3_u32") {
        V a = sv(0);
        V b = sv(1);
        V d = sv(2);
        bool is_signed = name.ends_with("i32");
        std::uint8_t lt = is_signed ? pred::SLT : pred::IULT, gt = is_signed ? pred::SGT : pred::IUGT;
        auto pick = [&](std::uint8_t p, V xv, V y) {
            V t = s.m.cmp(c, p, xv, y);
            return s.m.select(c, t, xv, y);
        };
        auto four = name.substr(2, 4);
        if (four == "min3") {
            V t = pick(lt, a, b);
            return pick(lt, t, d);
        }
        if (four == "max3") {
            V t = pick(gt, a, b);
            return pick(gt, t, d);
        }
        V lo = pick(lt, a, b);
        V hi = pick(gt, a, b);
        V t = pick(lt, hi, d);
        return pick(gt, lo, t);
    }
    if (name == "v_perm_b32") {
        const auto& v = std::get<Valu>(inst.fields);
        std::optional<std::uint32_t> sel;
        if (v.src.size() > 2 && v.src[2].kind == Operand::Literal) {
            sel = v.src[2].literal;
        } else if (v.src.size() > 2 && v.src[2].kind == Operand::Const) {
            sel = resource::inline_constant(v.src[2].a);
            if (!sel) {
                fail(name.data());
            }
        }
        if (!sel) {
            V a = sv(0);
            V b = sv(1);
            V svv = sv(2);
            V r = k(0);
            for (std::uint32_t i = 0; i < 4; ++i) {
                V sh = k(8 * i);
                V ff = k(0xFF);
                V se = s.m.bin(c, Bin::LShr, svv, sh);
                se = s.m.bin(c, Bin::And, se, ff);
                V four = k(4);
                V zero = k(0);
                V high = s.m.bin(c, Bin::And, se, four);
                high = s.m.cmp(c, pred::NE, high, zero);
                V word = s.m.select(c, high, a, b);
                V three = k(3);
                V index = s.m.bin(c, Bin::And, se, three);
                V bits = s.m.bin(c, Bin::Shl, index, three);
                V xv = s.m.bin(c, Bin::LShr, word, bits);
                V byte = s.m.bin(c, Bin::And, xv, ff);
                struct Sign {
                    std::uint32_t n;
                    V src;
                    std::uint32_t bit;
                };
                for (const Sign& sg : {Sign{8, b, 15}, Sign{9, b, 31}, Sign{10, a, 15}, Sign{11, a, 31}}) {
                    V left = k(31 - sg.bit);
                    V t = s.m.bin(c, Bin::Shl, sg.src, left);
                    V k31 = k(31);
                    t = s.m.bin(c, Bin::AShr, t, k31);
                    t = s.m.bin(c, Bin::And, t, ff);
                    V kn = k(sg.n);
                    V is = s.m.cmp(c, pred::EQ, se, kn);
                    byte = s.m.select(c, is, t, byte);
                }
                V k12 = k(12);
                V z = k(0);
                V is12 = s.m.cmp(c, pred::EQ, se, k12);
                byte = s.m.select(c, is12, z, byte);
                V above = s.m.cmp(c, pred::IUGT, se, k12);
                byte = s.m.select(c, above, ff, byte);
                V at = k(8 * i);
                byte = s.m.bin(c, Bin::Shl, byte, at);
                r = s.m.bin(c, Bin::Or, r, byte);
            }
            return r;
        }
        V a = sv(0);
        V b = sv(1);
        V r = k(0);
        for (std::uint32_t i = 0; i < 4; ++i) {
            std::uint32_t pick = *sel >> (8 * i) & 0xFF;
            V byte;
            if (pick <= 7) {
                V srcv = pick < 4 ? b : a;
                std::uint32_t n = pick < 4 ? pick : pick - 4;
                V sh = k(8 * n);
                V t = s.m.bin(c, Bin::LShr, srcv, sh);
                V ff = k(0xFF);
                byte = s.m.bin(c, Bin::And, t, ff);
            } else if (pick <= 11) {
                V srcv = pick < 10 ? b : a;
                std::uint32_t bit = pick % 2 == 0 ? 15 : 31;
                V left = k(31 - bit);
                V t = s.m.bin(c, Bin::Shl, srcv, left);
                V k31 = k(31);
                t = s.m.bin(c, Bin::AShr, t, k31);
                V ff = k(0xFF);
                byte = s.m.bin(c, Bin::And, t, ff);
            } else if (pick == 12) {
                continue;
            } else {
                byte = k(0xFF);
            }
            V at = k(8 * i);
            byte = s.m.bin(c, Bin::Shl, byte, at);
            r = s.m.bin(c, Bin::Or, r, byte);
        }
        return r;
    }
    if (name == "v_bfm_b32") {
        V a = sv(0);
        V b = sv(1);
        V one = k(1);
        a = m31(a);
        b = m31(b);
        V m = s.m.bin(c, Bin::Shl, one, a);
        m = s.m.bin(c, Bin::Sub, m, one);
        return s.m.bin(c, Bin::Shl, m, b);
    }
    if (name == "v_cvt_pk_u8_f32") {
        V f = fsrc(inst, 0);
        V b = sv(1);
        V d = sv(2);
        f = unary(26, f);
        V zero = s.m.f32(0);
        V max = s.m.f32(std::bit_cast<std::uint32_t>(255.0f));
        f = binary(35, f, zero);
        f = binary(36, f, max);
        V xv = s.m.cast(c, Cast::FpToUi, f, s.t.i32);
        V three = k(3);
        V k3 = k(3);
        V sh = s.m.bin(c, Bin::And, b, three);
        sh = s.m.bin(c, Bin::Shl, sh, k3);
        xv = s.m.bin(c, Bin::Shl, xv, sh);
        V ff = k(0xFF);
        V hole = s.m.bin(c, Bin::Shl, ff, sh);
        V ones = k(UINT32_MAX);
        hole = s.m.bin(c, Bin::Xor, hole, ones);
        d = s.m.bin(c, Bin::And, d, hole);
        return s.m.bin(c, Bin::Or, d, xv);
    }
    if (name == "v_ffbl_b32") {
        V a = sv(0);
        return s.op(c, "unaryBits.i32", Attr::ReadNone, s.t.i32, 32, {a});
    }
    if (name == "v_mbcnt_lo_u32_b32" || name == "v_mbcnt_hi_u32_b32") {
        V a = sv(0);
        V b = sv(1);
        if (mask_data == MaskData::Host) {
            const auto& v = std::get<Valu>(inst.fields);
            bool host = false;
            if (!v.src.empty()) {
                const Operand& o = v.src[0];
                if (o.kind == Operand::Const && o.a == 193) {
                    host = true;
                } else if (o.kind == Operand::Scalar) {
                    auto m = inst.read(Reg::s(o.a));
                    host = m && (st().masks.contains(*m) || st().highs.contains(*m));
                }
            }
            if (!host) {
                fail("v_mbcnt of a mask that is no ballot (console lane layout)");
            }
            s.flags |= dxil::flags::WAVE_OPS;
        } else if (!s.mesh) {
            cross_lane(false);
        }
        V lane = console_lane();
        V below;
        if (name == "v_mbcnt_lo_u32_b32") {
            below = lane;
        } else {
            V k32 = k(32);
            V z = k(0);
            V t = s.m.bin(c, Bin::Sub, lane, k32);
            V low = s.m.cmp(c, pred::IULT, lane, k32);
            below = s.m.select(c, low, z, t);
        }
        V one = k(1);
        V k32 = k(32);
        V ones = k(UINT32_MAX);
        V sh = m31(below);
        V m = s.m.bin(c, Bin::Shl, one, sh);
        m = s.m.bin(c, Bin::Sub, m, one);
        V full = s.m.cmp(c, pred::IUGE, below, k32);
        m = s.m.select(c, full, ones, m);
        V xv = s.m.bin(c, Bin::And, a, m);
        V n = s.op(c, "unaryBits.i32", Attr::ReadNone, s.t.i32, 31, {xv});
        return s.m.bin(c, Bin::Add, n, b);
    }
    fail(name.data());
}

void Lower::compare(const Inst& inst, std::string_view rest) {
    auto us = rest.rfind('_');
    if (us == std::string_view::npos) {
        fail("a compare");
    }
    std::string_view op = rest.substr(0, us), ty = rest.substr(us + 1);
    std::size_t c = cur;
    V r;
    if ((ty == "f32" || ty == "f16") && op == "class") {
        bool f16 = ty == "f16";
        V xv;
        if (f16) {
            const auto& v = std::get<Valu>(inst.fields);
            V h = half_src(inst, 0);
            if (v.abs & 1) {
                V m = s.m.i32(0x7FFF);
                h = s.m.bin(c, Bin::And, h, m);
            }
            if (v.neg & 1) {
                V sg = s.m.i32(0x8000);
                h = s.m.bin(c, Bin::Xor, h, sg);
            }
            xv = h;
        } else {
            V f = fsrc(inst, 0);
            xv = as_int(f);
        }
        V mask = src(inst, 1);
        std::uint32_t exp_sh = f16 ? 10 : 23, sign_sh = f16 ? 15 : 31, exp_mask = f16 ? 0x1F : 0xFF, mant_mask = f16 ? 0x03FF : 0x7FFFFF,
                      quiet_bit = f16 ? 0x0200 : 0x400000;
        V k_exp = s.m.i32(exp_sh);
        V k_sign = s.m.i32(sign_sh);
        V k_emax = s.m.i32(exp_mask);
        V mant = s.m.i32(mant_mask);
        V quiet = s.m.i32(quiet_bit);
        V e = s.m.bin(c, Bin::LShr, xv, k_exp);
        e = s.m.bin(c, Bin::And, e, k_emax);
        V m = s.m.bin(c, Bin::And, xv, mant);
        V neg = s.m.bin(c, Bin::LShr, xv, k_sign);
        V zero = s.m.i32(0);
        V e0 = s.m.cmp(c, pred::EQ, e, zero);
        V emax = s.m.cmp(c, pred::EQ, e, k_emax);
        V m0 = s.m.cmp(c, pred::EQ, m, zero);
        V q = s.m.bin(c, Bin::And, xv, quiet);
        q = s.m.cmp(c, pred::NE, q, zero);
        V normal = s.m.i32(8);
        V inf = s.m.i32(9);
        V zero_c = s.m.i32(6);
        V denorm = s.m.i32(7);
        V t = s.m.select(c, m0, zero_c, denorm);
        t = s.m.select(c, e0, t, normal);
        t = s.m.select(c, emax, inf, t);
        V eleven = s.m.i32(11);
        V mirrored = s.m.bin(c, Bin::Sub, eleven, t);
        V is_neg = s.m.cmp(c, pred::NE, neg, zero);
        t = s.m.select(c, is_neg, mirrored, t);
        V one = s.m.i32(1);
        V snan = s.m.i32(0);
        V nan_class = s.m.select(c, q, one, snan);
        V yes = s.m.boolean(true);
        V nan = s.m.bin(c, Bin::Xor, m0, yes);
        nan = s.m.bin(c, Bin::And, nan, emax);
        t = s.m.select(c, nan, nan_class, t);
        V bit = s.m.bin(c, Bin::LShr, mask, t);
        bit = s.m.bin(c, Bin::And, bit, one);
        r = s.m.cmp(c, pred::NE, bit, zero);
    } else if (ty == "f32" || ty == "f16") {
        V a, b;
        if (ty == "f16") {
            a = half_fsrc(inst, 0);
            b = half_fsrc(inst, 1);
        } else {
            a = fsrc(inst, 0);
            b = fsrc(inst, 1);
        }
        if (op == "lg") {
            V lt = s.m.cmp(c, pred::OLT, a, b);
            V gt = s.m.cmp(c, pred::OGT, a, b);
            r = s.m.bin(c, Bin::Or, lt, gt);
        } else {
            std::uint8_t p;
            if (op == "f") {
                p = pred::FALSE_;
            } else if (op == "lt") {
                p = pred::OLT;
            } else if (op == "eq") {
                p = pred::OEQ;
            } else if (op == "le") {
                p = pred::OLE;
            } else if (op == "gt") {
                p = pred::OGT;
            } else if (op == "ge") {
                p = pred::OGE;
            } else if (op == "o") {
                p = pred::ORD;
            } else if (op == "u") {
                p = pred::UNO;
            } else if (op == "nge") {
                p = pred::ULT;
            } else if (op == "nlg") {
                p = pred::UEQ;
            } else if (op == "ngt") {
                p = pred::ULE;
            } else if (op == "nle") {
                p = pred::UGT;
            } else if (op == "neq") {
                p = pred::UNE;
            } else if (op == "nlt") {
                p = pred::UGE;
            } else if (op == "tru") {
                p = pred::TRUE_;
            } else {
                fail(inst.name().data());
            }
            r = s.m.cmp(c, p, a, b);
        }
    } else if (ty == "u64" || ty == "i64") {
        V a = src64(inst, 0);
        V b = src64(inst, 1);
        bool is_signed = ty == "i64";
        std::uint8_t p;
        if (op == "eq") {
            p = pred::EQ;
        } else if (op == "ne" || op == "lg") {
            p = pred::NE;
        } else if (op == "lt") {
            p = is_signed ? pred::SLT : pred::IULT;
        } else if (op == "le") {
            p = is_signed ? pred::SLE : pred::IULE;
        } else if (op == "gt") {
            p = is_signed ? pred::SGT : pred::IUGT;
        } else if (op == "ge") {
            p = is_signed ? pred::SGE : pred::IUGE;
        } else {
            fail("a 64-bit compare");
        }
        r = s.m.cmp(c, p, a, b);
    } else if (ty == "u32" || ty == "i32" || ty == "u16" || ty == "i16") {
        V a, b;
        if (ty.ends_with("16")) {
            a = half_src(inst, 0);
            b = half_src(inst, 1);
            if (ty == "i16") {
                V k16 = s.m.i32(16);
                for (V* xp : {&a, &b}) {
                    V t = s.m.bin(c, Bin::Shl, *xp, k16);
                    *xp = s.m.bin(c, Bin::AShr, t, k16);
                }
            }
        } else {
            a = src(inst, 0);
            b = src(inst, 1);
        }
        bool is_signed = ty == "i32" || ty == "i16";
        std::uint8_t p;
        if (op == "eq") {
            p = pred::EQ;
        } else if (op == "ne" || op == "lg") {
            p = pred::NE;
        } else if (op == "lt") {
            p = is_signed ? pred::SLT : pred::IULT;
        } else if (op == "le") {
            p = is_signed ? pred::SLE : pred::IULE;
        } else if (op == "gt") {
            p = is_signed ? pred::SGT : pred::IUGT;
        } else if (op == "ge") {
            p = is_signed ? pred::SGE : pred::IUGE;
        } else if (op == "f" || op == "t") {
            lane(inst, op == "t");
            return;
        } else {
            fail("an integer compare");
        }
        r = s.m.cmp(c, p, a, b);
    } else {
        fail(inst.name().data());
    }
    lane_bits_for(inst, r);
}

void Lower::lane(const Inst& inst, std::optional<bool> value) {
    V b = s.m.boolean(value == std::optional<bool>(true));
    lane_bits_for(inst, b);
}

void Lower::lane_bits_for(const Inst& inst, V r) {
    auto* v = std::get_if<Valu>(&inst.fields);
    if (!v) {
        return;
    }
    std::optional<std::uint8_t> code;
    if (inst.name().starts_with("v_cmpx")) {
        code = ir::EXEC;
    } else if (v->dst && v->dst->kind == Operand::Scalar) {
        code = v->dst->a;
    }
    if (code) {
        if (auto lo = inst.write(Reg::s(*code))) {
            lane_bits[*lo] = r;
        }
    }
    for (const auto& [_, w] : inst.writes) {
        vals.try_emplace(w, Val::missing("a lane mask read as data"));
    }
}

}   
