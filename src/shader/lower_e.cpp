// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// scalar ALU instructions. See lower_impl.hpp.
#include <algorithm>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

Why Lower::sop(const Inst& inst) {
    const auto& f = std::get<SopFields>(inst.fields);
    auto name = inst.name();
    auto scc = inst.write(Reg::scc());
    std::optional<std::uint8_t> dst_code;
    if (f.dst && f.dst->kind == Operand::Scalar) {
        dst_code = f.dst->a;
    }
    std::optional<Value> lo;
    if (dst_code) {
        lo = inst.write(Reg::s(*dst_code));
    }
    if (inst.write(Reg::s(ir::EXEC)) || is_mask(lo)) {
        return LANE_MASK;
    }
    if (name == "s_setreg_imm32_b32" || name == "s_setreg_b32" || name == "s_swappc_b64" || name == "s_version" || name.starts_with("s_waitcnt")) {
        return "a register the emitter does not model";
    }
    if (name == "s_getpc_b64") {
        V z = s.m.i32(0);
        V at = s.m.i32(static_cast<std::uint32_t>(inst.machine.offset) + 4);
        std::optional<Value> hi;
        if (dst_code) {
            hi = inst.write(Reg::s(static_cast<std::uint8_t>(*dst_code + 1)));
        }
        set(lo, Val::i32(at));
        set(hi, Val::i32(z));
        return nullptr;
    }
    std::size_t c = cur;
    auto src32 = [&](std::size_t k) -> V {
        if (k == 0 && (name == "s_addk_i32" || name == "s_mulk_i32")) {
            if (!f.dst) {
                fail("a missing operand");
            }
            return operand(inst, *f.dst);
        }
        if (k >= f.src.size()) {
            fail("a missing operand");
        }
        return operand(inst, f.src[k]);
    };
    auto sext16 = [&]() {
        return s.m.i32(static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(f.imm.value_or(0)))));
    };
    auto half = [&](std::size_t k, std::uint8_t h) -> V {
        if (k < f.src.size()) {
            const Operand& op = f.src[k];
            if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
                auto v = inst.read(Reg::s(static_cast<std::uint8_t>(op.a + h)));
                if (!v) {
                    fail("an unread operand");
                }
                return i32_of(*v);
            }
            if (h == 0) {
                return operand(inst, op);
            }
            if (op.kind == Operand::Const) {
                auto xv = resource::inline_constant(op.a);
                if (!xv) {
                    fail("a special operand");
                }
                return s.m.i32(static_cast<std::int32_t>(*xv) < 0 ? UINT32_MAX : 0);
            }
        }
        return s.m.i32(0);
    };
    if (name.starts_with("s_bitcmp")) {
        auto op = name.substr(8);
        V bit = src32(1);
        V k31 = s.m.i32(31);
        V idx = s.m.bin(c, Bin::And, bit, k31);
        V word;
        if (op.ends_with("_b64")) {
            V a = half(0, 0);
            V b = half(0, 1);
            V k32 = s.m.i32(32);
            V upper = s.m.bin(c, Bin::And, bit, k32);
            V z = s.m.i32(0);
            upper = s.m.cmp(c, pred::NE, upper, z);
            word = s.m.select(c, upper, b, a);
        } else {
            word = src32(0);
        }
        V shifted = s.m.bin(c, Bin::LShr, word, idx);
        V one = s.m.i32(1);
        V tested = s.m.bin(c, Bin::And, shifted, one);
        V want = s.m.i32(op.starts_with('1') ? 1 : 0);
        V r = s.m.cmp(c, pred::EQ, tested, want);
        set(scc, Val::bit(r));
        return nullptr;
    }
    if (name.starts_with("s_cmp_") && name.ends_with("_u64")) {
        auto op = name.substr(6);
        std::optional<V> r;
        for (std::uint8_t h = 0; h < 2; ++h) {
            V a = half(0, h);
            V b = half(1, h);
            V e = s.m.cmp(c, pred::EQ, a, b);
            r = r ? s.m.bin(c, Bin::And, *r, e) : e;
        }
        V rv = *r;
        if (op.starts_with("lg")) {
            V t = s.m.boolean(true);
            rv = s.m.bin(c, Bin::Xor, rv, t);
        }
        set(scc, Val::bit(rv));
        return nullptr;
    }
    if (name.starts_with("s_cmp_") || name.starts_with("s_cmpk_")) {
        auto op = name.starts_with("s_cmp_") ? name.substr(6) : name.substr(7);
        V a, b;
        if (name.starts_with("s_cmpk_")) {
            b = op.ends_with("_i32") ? sext16() : s.m.i32(f.imm.value_or(0));
            a = src32(0);
        } else {
            if (op.ends_with("64")) {
                fail("a 64-bit scalar compare");
            }
            a = src32(0);
            b = src32(1);
        }
        bool is_signed = op.ends_with("_i32");
        auto two = op.substr(0, 2);
        std::uint8_t pr;
        if (two == "eq") {
            pr = pred::EQ;
        } else if (two == "lg") {
            pr = pred::NE;
        } else if (two == "gt") {
            pr = is_signed ? pred::SGT : pred::IUGT;
        } else if (two == "ge") {
            pr = is_signed ? pred::SGE : pred::IUGE;
        } else if (two == "lt") {
            pr = is_signed ? pred::SLT : pred::IULT;
        } else if (two == "le") {
            pr = is_signed ? pred::SLE : pred::IULE;
        } else {
            fail("a scalar compare");
        }
        V r = s.m.cmp(c, pr, a, b);
        set(scc, Val::bit(r));
        return nullptr;
    }
    bool wide = name.ends_with("_b64") || name.ends_with("_u64") || name.ends_with("_i64");
    if (wide) {
        std::optional<Value> hi;
        if (dst_code) {
            hi = inst.write(Reg::s(static_cast<std::uint8_t>(*dst_code + 1)));
        }
        auto nonzero = [&](V a, V b) {
            V o = s.m.bin(c, Bin::Or, a, b);
            V z = s.m.i32(0);
            return s.m.cmp(c, pred::NE, o, z);
        };
        if (name == "s_bcnt1_i32_b64") {
            V a = half(0, 0);
            V b = half(0, 1);
            Ty i32t = s.t.i32;
            V na = s.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 31, {a});
            V nb = s.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 31, {b});
            V r = s.m.bin(c, Bin::Add, na, nb);
            set(lo, Val::i32(r));
            V z = s.m.i32(0);
            V sv = s.m.cmp(c, pred::NE, r, z);
            set(scc, Val::bit(sv));
            return nullptr;
        }
        if (name == "s_ff1_i32_b64") {
            V a = half(0, 0);
            V b = half(0, 1);
            Ty i32t = s.t.i32;
            V fa = s.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 32, {a});
            V fb = s.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 32, {b});
            V z = s.m.i32(0);
            V k32 = s.m.i32(32);
            V fb32 = s.m.bin(c, Bin::Add, fb, k32);
            V none = s.m.cmp(c, pred::EQ, b, z);
            fb = s.m.select(c, none, fb, fb32);
            V low = s.m.cmp(c, pred::NE, a, z);
            V r = s.m.select(c, low, fa, fb);
            set(lo, Val::i32(r));
            return nullptr;
        }
        if (name == "s_and_b64" || name == "s_or_b64" || name == "s_xor_b64" || name == "s_andn2_b64" || name == "s_orn2_b64" || name == "s_nand_b64" ||
            name == "s_nor_b64" || name == "s_xnor_b64" || name == "s_not_b64") {
            V ones = s.m.i32(UINT32_MAX);
            std::vector<V> r;
            for (std::uint8_t h = 0; h < 2; ++h) {
                V a = half(0, h);
                V xv;
                if (name == "s_not_b64") {
                    xv = s.m.bin(c, Bin::Xor, a, ones);
                } else {
                    V b = half(1, h);
                    if (name.find("n2") != std::string_view::npos) {
                        b = s.m.bin(c, Bin::Xor, b, ones);
                    }
                    auto three = name.substr(2, 3);
                    Bin op = (three == "and" || three == "nan") ? Bin::And : (three == "or_" || three == "orn" || three == "nor") ? Bin::Or : Bin::Xor;
                    xv = s.m.bin(c, op, a, b);
                    if (name == "s_nand_b64" || name == "s_nor_b64" || name == "s_xnor_b64") {
                        xv = s.m.bin(c, Bin::Xor, xv, ones);
                    }
                }
                r.push_back(xv);
            }
            set(lo, Val::i32(r[0]));
            set(hi, Val::i32(r[1]));
            V sv = nonzero(r[0], r[1]);
            set(scc, Val::bit(sv));
            return nullptr;
        }
        if (name == "s_lshl_b64" || name == "s_lshr_b64" || name == "s_ashr_i64") {
            V a = half(0, 0);
            V b = half(0, 1);
            V n = src32(1);
            Ty i64t = s.m.int_(64);
            s.flags |= dxil::flags::INT64;
            a = s.m.cast(c, Cast::ZExt, a, i64t);
            b = s.m.cast(c, Cast::ZExt, b, i64t);
            V k32 = s.m.int_const(i64t, 32);
            b = s.m.bin(c, Bin::Shl, b, k32);
            V xv = s.m.bin(c, Bin::Or, a, b);
            V k63 = s.m.i32(63);
            n = s.m.bin(c, Bin::And, n, k63);
            n = s.m.cast(c, Cast::ZExt, n, i64t);
            Bin op = name == "s_lshl_b64" ? Bin::Shl : name == "s_lshr_b64" ? Bin::LShr : Bin::AShr;
            V r = s.m.bin(c, op, xv, n);
            V rl = s.m.cast(c, Cast::Trunc, r, s.t.i32);
            V rh = s.m.bin(c, Bin::LShr, r, k32);
            rh = s.m.cast(c, Cast::Trunc, rh, s.t.i32);
            set(lo, Val::i32(rl));
            set(hi, Val::i32(rh));
            V sv = nonzero(rl, rh);
            set(scc, Val::bit(sv));
            return nullptr;
        }
        if (name == "s_mov_b64") {
            std::optional<Value> outs[2] = {lo, hi};
            for (std::uint8_t h = 0; h < 2; ++h) {
                Val val;
                try {
                    val = Val::i32(half(0, h));
                } catch (const WhyError& e) {
                    val = Val::missing(e.why);
                }
                set(outs[h], val);
            }
            return nullptr;
        }
        if (name == "s_cselect_b64") {
            V cond = scc_in(inst);
            std::optional<Value> outs[2] = {lo, hi};
            for (std::uint8_t h = 0; h < 2; ++h) {
                V a = half(0, h);
                V b = half(1, h);
                V r = s.m.select(c, cond, a, b);
                set(outs[h], Val::i32(r));
            }
            return nullptr;
        }
        fail(name.data());
    }
    V r;
    std::optional<V> carry;
    if (name == "s_mov_b32") {
        r = src32(0);
    } else if (name == "s_movk_i32") {
        r = sext16();
    } else if (name == "s_cselect_b32") {
        V cond = scc_in(inst);
        V a = src32(0);
        V b = src32(1);
        r = s.m.select(c, cond, a, b);
    } else if (name == "s_brev_b32") {
        V a = src32(0);
        r = s.op(c, "unary.i32", Attr::ReadNone, s.t.i32, 30, {a});
    } else if (name == "s_not_b32") {
        V a = src32(0);
        V ones = s.m.i32(UINT32_MAX);
        r = s.m.bin(c, Bin::Xor, a, ones);
    } else if (name == "s_add_u32" || name == "s_add_i32" || name == "s_addk_i32") {
        V a = src32(0);
        V b = name == "s_addk_i32" ? sext16() : src32(1);
        r = s.m.bin(c, Bin::Add, a, b);
        if (name == "s_add_u32") {
            carry = s.m.cmp(c, pred::IULT, r, a);
        }
    } else if (name == "s_sub_u32" || name == "s_sub_i32") {
        V a = src32(0);
        V b = src32(1);
        r = s.m.bin(c, Bin::Sub, a, b);
        if (name == "s_sub_u32") {
            carry = s.m.cmp(c, pred::IULT, a, b);
        }
    } else if (name == "s_addc_u32" || name == "s_subb_u32") {
        V a = src32(0);
        V b = src32(1);
        V cin = scc_in(inst);
        auto [rr, cc] = add_sub_carry(c, name == "s_addc_u32", a, b, cin);
        r = rr;
        carry = cc;
    } else if (name == "s_mul_i32" || name == "s_mulk_i32") {
        V a = src32(0);
        V b = name == "s_mulk_i32" ? sext16() : src32(1);
        r = s.m.bin(c, Bin::Mul, a, b);
    } else if (name == "s_mul_hi_u32" || name == "s_mul_hi_i32") {
        V a = src32(0);
        V b = src32(1);
        r = mul_hi(a, b, name.ends_with("i32"));
    } else if (name == "s_flbit_i32_b32" || name == "s_flbit_i32") {
        V a = src32(0);
        r = s.op(c, "unaryBits.i32", Attr::ReadNone, s.t.i32, name == "s_flbit_i32_b32" ? 33 : 34, {a});
    } else if (name == "s_ff1_i32_b32" || name == "s_bcnt1_i32_b32") {
        V a = src32(0);
        r = s.op(c, "unaryBits.i32", Attr::ReadNone, s.t.i32, name == "s_ff1_i32_b32" ? 32 : 31, {a});
    } else if (name == "s_bfe_u32" || name == "s_bfe_i32") {
        V a = src32(0);
        V b = src32(1);
        V k16 = s.m.i32(16);
        V k127 = s.m.i32(0x7F);
        V w = s.m.bin(c, Bin::LShr, b, k16);
        w = s.m.bin(c, Bin::And, w, k127);
        r = bfe(a, b, w, name == "s_bfe_i32");
    } else if (name == "s_and_b32" || name == "s_or_b32" || name == "s_xor_b32" || name == "s_andn2_b32" || name == "s_orn2_b32" || name == "s_nand_b32" ||
               name == "s_nor_b32" || name == "s_xnor_b32") {
        V a = src32(0);
        V b = src32(1);
        V ones = s.m.i32(UINT32_MAX);
        if (name.find("n2") != std::string_view::npos) {
            b = s.m.bin(c, Bin::Xor, b, ones);
        }
        auto three = name.substr(2, 3);
        Bin op = (three == "and" || three == "nan") ? Bin::And : (three == "or_" || three == "orn" || three == "nor") ? Bin::Or : Bin::Xor;
        r = s.m.bin(c, op, a, b);
        if (name == "s_nand_b32" || name == "s_nor_b32" || name == "s_xnor_b32") {
            r = s.m.bin(c, Bin::Xor, r, ones);
        }
    } else if (name == "s_lshl_b32" || name == "s_lshr_b32" || name == "s_ashr_i32") {
        V a = src32(0);
        V b = src32(1);
        V k31 = s.m.i32(31);
        b = s.m.bin(c, Bin::And, b, k31);
        Bin op = name == "s_lshl_b32" ? Bin::Shl : name == "s_lshr_b32" ? Bin::LShr : Bin::AShr;
        r = s.m.bin(c, op, a, b);
    } else if (name == "s_lshl1_add_u32" || name == "s_lshl2_add_u32" || name == "s_lshl3_add_u32" || name == "s_lshl4_add_u32") {
        V a = src32(0);
        V b = src32(1);
        V k = s.m.i32(static_cast<std::uint32_t>(name[6] - '0'));
        V t = s.m.bin(c, Bin::Shl, a, k);
        r = s.m.bin(c, Bin::Add, t, b);
    } else if (name == "s_min_u32" || name == "s_max_u32" || name == "s_min_i32" || name == "s_max_i32") {
        V a = src32(0);
        V b = src32(1);
        std::uint8_t pr = name == "s_min_u32" ? pred::IULT : name == "s_max_u32" ? pred::IUGT : name == "s_min_i32" ? pred::SLT : pred::SGT;
        V t = s.m.cmp(c, pr, a, b);
        r = s.m.select(c, t, a, b);
    } else if (name == "s_abs_i32") {
        V a = src32(0);
        V z = s.m.i32(0);
        V neg = s.m.cmp(c, pred::SLT, a, z);
        V n = s.m.bin(c, Bin::Sub, z, a);
        r = s.m.select(c, neg, n, a);
    } else if (name == "s_bitset0_b32" || name == "s_bitset1_b32") {
        if (!f.dst) {
            fail("a missing operand");
        }
        V old = operand(inst, *f.dst);
        V a = src32(0);
        V k31 = s.m.i32(31);
        V one = s.m.i32(1);
        a = s.m.bin(c, Bin::And, a, k31);
        V bit = s.m.bin(c, Bin::Shl, one, a);
        if (name == "s_bitset1_b32") {
            r = s.m.bin(c, Bin::Or, old, bit);
        } else {
            V ones = s.m.i32(UINT32_MAX);
            V nb = s.m.bin(c, Bin::Xor, bit, ones);
            r = s.m.bin(c, Bin::And, old, nb);
        }
    } else if (name == "s_bfm_b32") {
        V a = src32(0);
        V b = src32(1);
        V k31 = s.m.i32(31);
        V one = s.m.i32(1);
        a = s.m.bin(c, Bin::And, a, k31);
        b = s.m.bin(c, Bin::And, b, k31);
        V m = s.m.bin(c, Bin::Shl, one, a);
        m = s.m.bin(c, Bin::Sub, m, one);
        r = s.m.bin(c, Bin::Shl, m, b);
    } else {
        fail(name.data());
    }
    set(lo, Val::i32(r));
    if (scc) {
        Val val;
        if (carry) {
            val = Val::bit(*carry);
        } else if (name == "s_mov_b32" || name == "s_movk_i32" || name == "s_cselect_b32" || name == "s_brev_b32" || name == "s_mul_i32" ||
                   name == "s_mulk_i32") {
            return nullptr;
        } else if (name.starts_with("s_add") || name.starts_with("s_sub")) {
            val = Val::missing("the carry of a scalar operation");
        } else {
            V z = s.m.i32(0);
            val = Val::bit(s.m.cmp(c, pred::NE, r, z));
        }
        vals[*scc] = val;
    }
    return nullptr;
}

std::pair<V, V> Lower::add_sub_carry(std::size_t c, bool add, V a, V b, std::optional<V> cin) {
    Bin op = add ? Bin::Add : Bin::Sub;
    V r = s.m.bin(c, op, a, b);
    V carry = add ? s.m.cmp(c, pred::IULT, r, a) : s.m.cmp(c, pred::IULT, a, b);
    if (cin) {
        V one = s.m.cast(c, Cast::ZExt, *cin, s.t.i32);
        V t = r;
        r = s.m.bin(c, op, t, one);
        V edge = s.m.i32(add ? UINT32_MAX : 0);
        V at_edge = s.m.cmp(c, pred::EQ, t, edge);
        V more = s.m.bin(c, Bin::And, *cin, at_edge);
        carry = s.m.bin(c, Bin::Or, carry, more);
    }
    return {r, carry};
}

V Lower::scc_in(const Inst& inst) {
    auto v = inst.read(Reg::scc());
    if (!v) {
        fail("an unread SCC");
    }
    Val g = get(*v);
    if (g.kind != Val::Bit) {
        fail("SCC without a value");
    }
    return g.v;
}

}   
