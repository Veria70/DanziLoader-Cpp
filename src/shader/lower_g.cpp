// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// vector ALU instructions, DPP. See lower_impl.hpp.
#include <algorithm>
#include <bit>
#include <cstdlib>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

namespace {
std::uint32_t fb(float f) { return std::bit_cast<std::uint32_t>(f); }
}   

void Lower::valu(const Inst& inst, std::size_t b, std::size_t i) {
    const auto& v = std::get<Valu>(inst.fields);
    auto name = inst.name();
    const auto& cmps = x.res->waterfall.compares;
    if (std::find(cmps.begin(), cmps.end(), Site{b, i}) != cmps.end()) {
        lane(inst, true);
        return;
    }
    if (name.starts_with("v_cmp_") || name.starts_with("v_cmpx_")) {
        auto rest = name.starts_with("v_cmp_") ? name.substr(6) : name.substr(7);
        try {
            if (v.dpp) {
                fail("a compare with DPP");
            }
            compare(inst, rest);
        } catch (const WhyError& e) {
            for (const auto& [_, w] : inst.writes) {
                lane_fail[w] = e.why;
            }
            throw;
        }
        return;
    }
    std::optional<V> g = guard(b, i);
    if (v.dpp) {
        auto [xv, enable] = dpp(inst, *v.dpp);
        if (g && enable) {
            g = s.m.bin(cur, Bin::And, *g, *enable);
        } else if (!g) {
            g = enable;
        }
        src0 = xv;
    }
    try {
        valu_op(inst, g);
    } catch (...) {
        src0.reset();
        throw;
    }
    src0.reset();
}

std::pair<V, std::optional<V>> Lower::dpp(const Inst& inst, const Dpp& d) {
    const auto& v = std::get<Valu>(inst.fields);
    if (v.src.empty() || v.src[0].kind != Operand::Vector) {
        fail("a DPP operand");
    }
    auto rv = inst.read(Reg::v(v.src[0].a));
    if (!rv) {
        fail("a DPP operand");
    }
    V xv = i32_of(*rv);
    bool quad = d.kind == Dpp::Row && d.ctrl <= 0xFF;
    bool mesh_quad = s.mesh.has_value() && quad;
    if (!mesh_quad) {
        cross_lane(quad);
    }
    std::size_t c = cur;
    V lane = lane_index();
    auto k = [&](std::uint32_t n) { return s.m.i32(n); };
    V sixteen_mask = k(~15u);
    V low = k(15);
    V row = s.m.bin(c, Bin::And, lane, sixteen_mask);
    V pos = s.m.bin(c, Bin::And, lane, low);
    V srcv;
    std::optional<V> inside;
    std::optional<std::pair<std::uint8_t, std::uint8_t>> masks;
    bool zero = false;
    if (d.kind == Dpp::Lanes) {
        V seven = k(7);
        V not7 = k(~7u);
        V three = k(3);
        V pp = s.m.bin(c, Bin::And, lane, seven);
        V sh = s.m.bin(c, Bin::Mul, pp, three);
        V sel = k(d.sel);
        V pick = s.m.bin(c, Bin::LShr, sel, sh);
        pick = s.m.bin(c, Bin::And, pick, seven);
        V base = s.m.bin(c, Bin::And, lane, not7);
        srcv = s.m.bin(c, Bin::Or, base, pick);
    } else {
        std::uint16_t ctrl = d.ctrl;
        std::uint32_t n = ctrl & 15u;
        if (ctrl <= 0xFF) {
            V three = k(3);
            V one = k(1);
            V not3 = k(~3u);
            V pp = s.m.bin(c, Bin::And, lane, three);
            V sh = s.m.bin(c, Bin::Shl, pp, one);
            V perm = k(ctrl);
            V pick = s.m.bin(c, Bin::LShr, perm, sh);
            pick = s.m.bin(c, Bin::And, pick, three);
            V base = s.m.bin(c, Bin::And, lane, not3);
            srcv = s.m.bin(c, Bin::Or, base, pick);
        } else if ((ctrl >= 0x101 && ctrl <= 0x10F) || (ctrl >= 0x111 && ctrl <= 0x11F)) {
            V nn = k(n);
            bool left = ctrl < 0x110;
            srcv = s.m.bin(c, left ? Bin::Add : Bin::Sub, lane, nn);
            V bound = left ? k(16 - n) : nn;
            inside = s.m.cmp(c, left ? pred::IULT : pred::IUGE, pos, bound);
        } else if (ctrl >= 0x121 && ctrl <= 0x12F) {
            V nn = k(n);
            V t = s.m.bin(c, Bin::Sub, pos, nn);
            t = s.m.bin(c, Bin::And, t, low);
            srcv = s.m.bin(c, Bin::Or, row, t);
        } else if (ctrl == 0x140 || ctrl == 0x141) {
            V m = k(ctrl == 0x140 ? 15 : 7);
            srcv = s.m.bin(c, Bin::Xor, lane, m);
        } else if (ctrl >= 0x150 && ctrl <= 0x15F) {
            V nn = k(n);
            srcv = s.m.bin(c, Bin::Or, row, nn);
        } else if (ctrl >= 0x160 && ctrl <= 0x16F) {
            V nn = k(n);
            srcv = s.m.bin(c, Bin::Xor, lane, nn);
        } else {
            fail("a DPP lane pattern");
        }
        if (d.row_mask != 0xF || d.bank_mask != 0xF) {
            masks = std::pair{d.row_mask, d.bank_mask};
        }
        zero = d.bound_ctrl;
    }
    V r = mesh_quad ? quad_pick(xv, srcv) : read_lane(xv, srcv);
    std::optional<V> enable;
    if (inside) {
        if (zero) {
            V z = k(0);
            r = s.m.select(c, *inside, r, z);
        } else {
            enable = inside;
        }
    }
    if (masks) {
        V four = k(4);
        V two = k(2);
        V three = k(3);
        V ri = s.m.bin(c, Bin::LShr, lane, four);
        ri = s.m.bin(c, Bin::And, ri, three);
        V bi = s.m.bin(c, Bin::LShr, lane, two);
        bi = s.m.bin(c, Bin::And, bi, three);
        V rm = k(masks->first);
        V bm = k(masks->second);
        V rb = s.m.bin(c, Bin::LShr, rm, ri);
        V bb = s.m.bin(c, Bin::LShr, bm, bi);
        V both = s.m.bin(c, Bin::And, rb, bb);
        V one = k(1);
        V z = k(0);
        both = s.m.bin(c, Bin::And, both, one);
        V on = s.m.cmp(c, pred::NE, both, z);
        enable = enable ? s.m.bin(c, Bin::And, *enable, on) : on;
    }
    return {r, enable};
}

void Lower::valu_op(const Inst& inst, std::optional<V> g) {
    const auto& v = std::get<Valu>(inst.fields);
    auto name = inst.name();
    std::size_t c = cur;
    auto f = [&](std::size_t k) { return fsrc(inst, k); };
    auto sv = [&](std::size_t k) { return src(inst, k); };
    auto flt = [&](V r) { write_f(inst, r, g); };
    auto in = [&](V r) { write_i(inst, r, g); };
    if (valu16(inst, name, g)) {
        return;
    }
    if (name.starts_with("v_pk_")) {
        packed16(inst, name.substr(5), g);
        return;
    }
    auto note_masks_missing = [&]() {
        for (const auto& [_, w] : inst.writes) {
            vals.try_emplace(w, Val::missing("a lane mask read as data"));
        }
    };
    if (name == "v_mad_u64_u32") {
        V a = sv(0);
        V b = sv(1);
        V xv = src64(inst, 2);
        Ty i64t = s.m.int_(64);
        s.flags |= dxil::flags::INT64;
        a = s.m.cast(c, Cast::ZExt, a, i64t);
        b = s.m.cast(c, Cast::ZExt, b, i64t);
        V product = s.m.bin(c, Bin::Mul, a, b);
        V r = s.m.bin(c, Bin::Add, product, xv);
        V carry = s.m.cmp(c, pred::IULT, r, xv);
        if (!v.dst || v.dst->kind != Operand::Vector) {
            fail("a 64-bit result");
        }
        std::uint8_t reg = v.dst->a;
        auto [lo, hi] = split64(r);
        write(inst, Reg::v(reg), lo, g);
        write(inst, Reg::v(static_cast<std::uint8_t>(reg + 1)), hi, g);
        if (v.carry && v.carry->kind == Operand::Scalar) {
            if (auto w = inst.write(Reg::s(v.carry->a))) {
                lane_bits[*w] = carry;
            }
        }
        note_masks_missing();
        return;
    }
    if (name == "v_lshlrev_b64" || name == "v_lshrrev_b64" || name == "v_ashrrev_i64") {
        V n = sv(0);
        V xv = src64(inst, 1);
        Ty i64t = s.m.int_(64);
        V k63 = s.m.i32(63);
        n = s.m.bin(c, Bin::And, n, k63);
        n = s.m.cast(c, Cast::ZExt, n, i64t);
        Bin op = name == "v_lshlrev_b64" ? Bin::Shl : name == "v_lshrrev_b64" ? Bin::LShr : Bin::AShr;
        V r = s.m.bin(c, op, xv, n);
        if (!v.dst || v.dst->kind != Operand::Vector) {
            fail("a 64-bit result");
        }
        std::uint8_t reg = v.dst->a;
        auto [lo, hi] = split64(r);
        write(inst, Reg::v(reg), lo, g);
        write(inst, Reg::v(static_cast<std::uint8_t>(reg + 1)), hi, g);
        return;
    }
    if (name == "v_cubeid_f32" || name == "v_cubesc_f32" || name == "v_cubetc_f32" || name == "v_cubema_f32") {
        V xv = f(0);
        V yv = f(1);
        V zv = f(2);
        V ax = unary(6, xv);
        V ay = unary(6, yv);
        V az = unary(6, zv);
        V zx = s.m.cmp(c, pred::OGE, az, ax);
        V zy = s.m.cmp(c, pred::OGE, az, ay);
        V zmaj = s.m.bin(c, Bin::And, zx, zy);
        V ymaj = s.m.cmp(c, pred::OGE, ay, ax);
        V zero = s.m.f32(0);
        auto neg = [&](V t) { return s.m.cmp(c, pred::OLT, t, zero); };
        auto minus = [&](V t) {
            V z = s.m.f32(0x80000000u);
            return s.m.bin(c, Bin::Sub, z, t);
        };
        auto pick = [&](V zf, V yf, V xf) {
            V t = s.m.select(c, ymaj, yf, xf);
            return s.m.select(c, zmaj, zf, t);
        };
        V r;
        if (name == "v_cubeid_f32") {
            V nz = neg(zv);
            V ny = neg(yv);
            V nx = neg(xv);
            V f5 = s.m.f32(fb(5.0f));
            V f4 = s.m.f32(fb(4.0f));
            V f3 = s.m.f32(fb(3.0f));
            V f2 = s.m.f32(fb(2.0f));
            V f1 = s.m.f32(fb(1.0f));
            V f0 = s.m.f32(fb(0.0f));
            V zf = s.m.select(c, nz, f5, f4);
            V yf = s.m.select(c, ny, f3, f2);
            V xf = s.m.select(c, nx, f1, f0);
            r = pick(zf, yf, xf);
        } else if (name == "v_cubesc_f32") {
            V nz = neg(zv);
            V nx = neg(xv);
            V mx = minus(xv);
            V mz = minus(zv);
            V zf = s.m.select(c, nz, mx, xv);
            V xf = s.m.select(c, nx, zv, mz);
            r = pick(zf, xv, xf);
        } else if (name == "v_cubetc_f32") {
            V ny = neg(yv);
            V my = minus(yv);
            V mz = minus(zv);
            V yf = s.m.select(c, ny, mz, zv);
            r = pick(my, yf, my);
        } else {
            V t = pick(zv, yv, xv);
            V two = s.m.f32(fb(2.0f));
            r = s.m.bin(c, Bin::Mul, t, two);
        }
        flt(r);
        return;
    }
    if (name == "v_movrels_b32" || name == "v_movreld_b32" || name == "v_movrelsd_b32") {
        auto m0v = inst.read(Reg::s(ir::M0));
        if (!m0v) {
            fail("M0");
        }
        V m0 = i32_of(*m0v);
        if (v.src.empty() || v.src[0].kind != Operand::Vector || !v.dst || v.dst->kind != Operand::Vector) {
            fail("an M0-relative move");
        }
        std::uint8_t sreg = v.src[0].a, dreg = v.dst->a;
        bool indexed_src = name != "v_movreld_b32";
        V value;
        if (indexed_src) {
            V r = s.m.i32(0);
            for (std::uint32_t k = 0; k <= 255u - sreg; ++k) {
                auto xr = inst.read(Reg::v(static_cast<std::uint8_t>(sreg + k)));
                if (!xr) {
                    break;
                }
                V xv = i32_of(*xr);
                V kk = s.m.i32(k);
                V is = s.m.cmp(c, pred::EQ, m0, kk);
                r = s.m.select(c, is, xv, r);
            }
            value = r;
        } else {
            value = sv(0);
        }
        if (name == "v_movrels_b32") {
            in(value);
            return;
        }
        for (std::uint32_t k = 0; k <= 255u - dreg; ++k) {
            Reg reg = Reg::v(static_cast<std::uint8_t>(dreg + k));
            if (!inst.write(reg)) {
                break;
            }
            V old;
            if (auto o = inst.read(reg)) {
                old = i32_of(*o);
            } else {
                old = s.m.i32(0);
            }
            V kk = s.m.i32(k);
            V is = s.m.cmp(c, pred::EQ, m0, kk);
            V xv = s.m.select(c, is, value, old);
            write_placed(inst, reg, xv, g);
        }
        return;
    }
    if (name == "v_swap_b32") {
        if (v.src.empty() || v.src[0].kind != Operand::Vector || !v.dst || v.dst->kind != Operand::Vector) {
            fail("v_swap_b32");
        }
        std::uint8_t sreg = v.src[0].a, dreg = v.dst->a;
        auto av = inst.read(Reg::v(dreg));
        if (!av) {
            fail("v_swap_b32");
        }
        V a = i32_of(*av);
        auto bv = inst.read(Reg::v(sreg));
        if (!bv) {
            fail("v_swap_b32");
        }
        V b = i32_of(*bv);
        write_placed(inst, Reg::v(dreg), b, g);
        if (dreg != sreg) {
            write_placed(inst, Reg::v(sreg), a, g);
        }
        return;
    }
    if (name == "v_ffbh_u32" || name == "v_ffbh_i32") {
        V a = sv(0);
        V r = s.op(c, "unaryBits.i32", Attr::ReadNone, s.t.i32, name == "v_ffbh_u32" ? 33 : 34, {a});
        in(r);
        return;
    }
    if (name == "v_mov_b32") {
        if (v.abs != 0 || v.neg != 0 || v.clamp || v.omod != 0) {
            flt(f(0));
            return;
        }
        in(sv(0));
        return;
    }
    if (name == "v_add_f32" || name == "v_sub_f32" || name == "v_subrev_f32" || name == "v_mul_f32" || name == "v_mul_legacy_f32" ||
        name == "v_max_f32" || name == "v_min_f32") {
        V a = f(0);
        V bb = f(1);
        V r;
        if (name == "v_add_f32") {
            r = s.m.bin(c, Bin::Add, a, bb);
        } else if (name == "v_sub_f32") {
            r = s.m.bin(c, Bin::Sub, a, bb);
        } else if (name == "v_subrev_f32") {
            r = s.m.bin(c, Bin::Sub, bb, a);
        } else if (name == "v_mul_f32") {
            r = s.m.bin(c, Bin::Mul, a, bb);
        } else if (name == "v_mul_legacy_f32") {
            r = legacy_mul(a, bb);
        } else if (name == "v_max_f32") {
            r = max_min(true, a, bb);
        } else {
            r = max_min(false, a, bb);
        }
        flt(r);
        return;
    }
    if (name == "v_fma_f32" || name == "v_fmaak_f32" || name == "v_fmamk_f32") {
        V a = f(0);
        V bb = f(1);
        V cc = f(2);
        flt(fmad(a, bb, cc));
        return;
    }
    if (name == "v_div_scale_f32") {
        V a = f(0);
        V den = f(1);
        V num = f(2);
        V ai = as_int(a);
        V ni = as_int(num);
        V di = as_int(den);
        V k23 = s.m.i32(23);
        V k255 = s.m.i32(0xFF);
        V kabs = s.m.i32(0x7FFFFFFF);
        V zero = s.m.i32(0);
        V k23_exp = s.m.i32(23);
        V k232_exp = s.m.i32(232);
        V k96 = s.m.i32(96);
        auto exp_of = [&](V t) {
            V sh = s.m.bin(c, Bin::LShr, t, k23);
            return s.m.bin(c, Bin::And, sh, k255);
        };
        V e1 = exp_of(ni);
        V e2 = exp_of(di);
        V n_abs = s.m.bin(c, Bin::And, ni, kabs);
        V d_abs = s.m.bin(c, Bin::And, di, kabs);
        V z1 = s.m.cmp(c, pred::EQ, n_abs, zero);
        V z2 = s.m.cmp(c, pred::EQ, d_abs, zero);
        V any_zero = s.m.bin(c, Bin::Or, z1, z2);
        V s1 = s.m.cmp(c, pred::EQ, e1, k255);
        V s2 = s.m.cmp(c, pred::EQ, e2, k255);
        V a12 = s.m.bin(c, Bin::Or, s1, s2);
        V special = s.m.bin(c, Bin::Or, a12, any_zero);
        V is_den = s.m.cmp(c, pred::EQ, ai, di);
        V diff = s.m.bin(c, Bin::Sub, e2, e1);
        V den_tiny = s.m.cmp(c, pred::IULE, e2, k23_exp);
        V den_huge = s.m.cmp(c, pred::IUGE, e2, k232_exp);
        V num_tiny_exp = s.m.cmp(c, pred::IULE, e1, k23_exp);
        V num_huge = s.m.cmp(c, pred::IUGE, e1, k232_exp);
        V num_gap = s.m.cmp(c, pred::SGE, diff, k96);
        V t1 = s.m.boolean(true);
        V not_den_huge = s.m.bin(c, Bin::Xor, den_huge, t1);
        num_gap = s.m.bin(c, Bin::And, num_gap, not_den_huge);
        V num_tiny = s.m.bin(c, Bin::Or, num_tiny_exp, num_gap);
        V up_raw = s.m.select(c, is_den, den_tiny, num_tiny);
        V dn_raw = s.m.select(c, is_den, den_huge, num_huge);
        V t2 = s.m.boolean(true);
        V not_special = s.m.bin(c, Bin::Xor, special, t2);
        V up = s.m.bin(c, Bin::And, up_raw, not_special);
        V dn = s.m.bin(c, Bin::And, dn_raw, not_special);
        V x_tiny = s.m.bin(c, Bin::Xor, den_tiny, num_tiny);
        V x_huge = s.m.bin(c, Bin::Xor, den_huge, num_huge);
        V uncancelled = s.m.bin(c, Bin::Or, x_tiny, x_huge);
        V flag_raw = s.m.select(c, is_den, uncancelled, up_raw);
        V flag = s.m.bin(c, Bin::And, flag_raw, not_special);
        V k_up = s.m.f32(0x5F800000);
        V k_dn = s.m.f32(0x1F800000);
        V a_up = s.m.bin(c, Bin::Mul, a, k_up);
        V a_dn = s.m.bin(c, Bin::Mul, a, k_dn);
        V r = s.m.select(c, dn, a_dn, a);
        r = s.m.select(c, up, a_up, r);
        V qnan = s.m.f32(0x7FC00000);
        r = s.m.select(c, any_zero, qnan, r);
        flt(r);
        if (v.carry && v.carry->kind == Operand::Scalar) {
            if (auto w = inst.write(Reg::s(v.carry->a))) {
                lane_bits[*w] = flag;
            }
        }
        note_masks_missing();
        return;
    }
    if (name == "v_div_fmas_f32") {
        V a = f(0);
        V bb = f(1);
        V cc = f(2);
        std::optional<Bool> m;
        if (auto vc = inst.read(Reg::s(ir::VCC))) {
            if (auto it = st().masks.find(*vc); it != st().masks.end()) {
                m = it->second;
            }
        }
        if (!m) {
            fail("v_div_fmas_f32 without VCC");
        }
        V vcc = boolean(*m, c);
        V tmp = fmad(a, bb, cc);
        V ci = as_int(cc);
        V k23 = s.m.i32(23);
        V k255 = s.m.i32(0xFF);
        V k128 = s.m.i32(128);
        V sh = s.m.bin(c, Bin::LShr, ci, k23);
        V ex = s.m.bin(c, Bin::And, sh, k255);
        V exp_ge = s.m.cmp(c, pred::IUGE, ex, k128);
        V k_up = s.m.f32(0x5F800000);
        V k_dn = s.m.f32(0x1F800000);
        V scale = s.m.select(c, exp_ge, k_up, k_dn);
        V scaled = s.m.bin(c, Bin::Mul, tmp, scale);
        flt(s.m.select(c, vcc, scaled, tmp));
        return;
    }
    if (name == "v_div_fixup_f32") {
        V q = f(0);
        V den = f(1);
        V num = f(2);
        V qi = as_int(q);
        V di = as_int(den);
        V ni = as_int(num);
        V ksign = s.m.i32(0x80000000u);
        V kabs = s.m.i32(0x7FFFFFFF);
        V kinf = s.m.i32(0x7F800000);
        V kqnan_bit = s.m.i32(0x00400000);
        V kneg_qnan = s.m.i32(0xFFC00000u);
        V zero = s.m.i32(0);
        V sx = s.m.bin(c, Bin::Xor, ni, di);
        V sign = s.m.bin(c, Bin::And, sx, ksign);
        V q_abs = s.m.bin(c, Bin::And, qi, kabs);
        V d_abs = s.m.bin(c, Bin::And, di, kabs);
        V n_abs = s.m.bin(c, Bin::And, ni, kabs);
        V q_nan = s.m.cmp(c, pred::IUGT, q_abs, kinf);
        V d_nan = s.m.cmp(c, pred::IUGT, d_abs, kinf);
        V n_nan = s.m.cmp(c, pred::IUGT, n_abs, kinf);
        V d_inf = s.m.cmp(c, pred::EQ, d_abs, kinf);
        V n_inf = s.m.cmp(c, pred::EQ, n_abs, kinf);
        V d_zero = s.m.cmp(c, pred::EQ, d_abs, zero);
        V n_zero = s.m.cmp(c, pred::EQ, n_abs, zero);
        V both_inf = s.m.bin(c, Bin::And, n_inf, d_inf);
        V both_zero = s.m.bin(c, Bin::And, n_zero, d_zero);
        V invalid = s.m.bin(c, Bin::Or, both_inf, both_zero);
        V mi = s.m.bin(c, Bin::Or, n_inf, d_zero);
        V make_inf = s.m.bin(c, Bin::Or, mi, q_nan);
        V make_zero = s.m.bin(c, Bin::Or, d_inf, n_zero);
        V signed_q = s.m.bin(c, Bin::Or, sign, q_abs);
        V signed_inf = s.m.bin(c, Bin::Or, sign, kinf);
        V n_qnan = s.m.bin(c, Bin::Or, ni, kqnan_bit);
        V d_qnan = s.m.bin(c, Bin::Or, di, kqnan_bit);
        V r = s.m.select(c, make_zero, sign, signed_q);
        r = s.m.select(c, make_inf, signed_inf, r);
        r = s.m.select(c, invalid, kneg_qnan, r);
        r = s.m.select(c, d_nan, d_qnan, r);
        r = s.m.select(c, n_nan, n_qnan, r);
        flt(as_float(r));
        return;
    }
    if (name == "v_fmac_f32" || name == "v_mac_f32" || name == "v_mac_legacy_f32") {
        V a = f(0);
        V bb = f(1);
        if (!v.dst || v.dst->kind != Operand::Vector) {
            fail("an accumulator");
        }
        auto accv = inst.read(Reg::v(v.dst->a));
        if (!accv) {
            fail("an accumulator");
        }
        V acc;
        if (auto it = floats.find(*accv); it != floats.end()) {
            acc = it->second;
        } else {
            V xi = i32_of(*accv);
            acc = as_float(xi);
        }
        V r;
        if (name == "v_fmac_f32") {
            r = fmad(a, bb, acc);
        } else if (name == "v_mac_f32") {
            V m = s.m.bin(c, Bin::Mul, a, bb);
            r = s.m.bin(c, Bin::Add, m, acc);
        } else {
            V m = legacy_mul(a, bb);
            r = s.m.bin(c, Bin::Add, m, acc);
        }
        flt(r);
        return;
    }
    if (name == "v_mad_f32" || name == "v_madak_f32" || name == "v_madmk_f32" || name == "v_mad_legacy_f32") {
        V a = f(0);
        V bb = f(1);
        V cc = f(2);
        V m = name == "v_mad_legacy_f32" ? legacy_mul(a, bb) : s.m.bin(c, Bin::Mul, a, bb);
        flt(s.m.bin(c, Bin::Add, m, cc));
        return;
    }
    if (name == "v_max3_f32" || name == "v_min3_f32" || name == "v_med3_f32") {
        V a = f(0);
        V bb = f(1);
        V cc = f(2);
        V r;
        if (name == "v_max3_f32") {
            V t = max_min(true, a, bb);
            r = max_min(true, t, cc);
        } else if (name == "v_min3_f32") {
            V t = max_min(false, a, bb);
            r = max_min(false, t, cc);
        } else {
            r = med3(a, bb, cc);
        }
        flt(r);
        return;
    }
    if (name == "v_rcp_f32" || name == "v_rcp_iflag_f32") {
        V a = f(0);
        V one = s.m.f32(fb(1.0f));
        flt(s.m.bin(c, Bin::SDiv, one, a));
        return;
    }
    if (name == "v_rsq_f32" || name == "v_sqrt_f32" || name == "v_log_f32" || name == "v_exp_f32" || name == "v_fract_f32" || name == "v_trunc_f32" ||
        name == "v_floor_f32" || name == "v_ceil_f32" || name == "v_rndne_f32" || name == "v_sin_f32" || name == "v_cos_f32") {
        V a = f(0);
        std::uint32_t op = name == "v_rsq_f32"     ? 25
                           : name == "v_sqrt_f32"  ? 24
                           : name == "v_log_f32"   ? 23
                           : name == "v_exp_f32"   ? 21
                           : name == "v_fract_f32" ? 22
                           : name == "v_trunc_f32" ? 29
                           : name == "v_floor_f32" ? 27
                           : name == "v_ceil_f32"  ? 28
                           : name == "v_rndne_f32" ? 26
                           : name == "v_sin_f32"   ? 13
                                                   : 12;
        V r;
        if (op == 12 || op == 13) {
            r = trig_turns(a, op == 12);
        } else if (op == 22) {
            r = fract(a, 1.0f, std::bit_cast<float>(0x3F7FFFFFu));
        } else {
            r = unary(op, a);
        }
        flt(r);
        return;
    }
    if (name == "v_cvt_i32_f32" || name == "v_cvt_u32_f32" || name == "v_cvt_flr_i32_f32" || name == "v_cvt_rpi_i32_f32") {
        V a = f(0);
        if (name == "v_cvt_rpi_i32_f32") {
            V h = s.m.f32(fb(0.5f));
            a = s.m.bin(c, Bin::Add, a, h);
        }
        if (name != "v_cvt_i32_f32" && name != "v_cvt_u32_f32") {
            a = unary(27, a);
        }
        in(s.m.cast(c, name == "v_cvt_u32_f32" ? Cast::FpToUi : Cast::FpToSi, a, s.t.i32));
        return;
    }
    if (name == "v_permlane16_b32" || name == "v_permlanex16_b32") {
        cross_lane(false);
        V xv = sv(0);
        V s1 = sv(1);
        V s2 = sv(2);
        Ty i32t = s.t.i32;
        V lane = s.op(c, "waveGetLaneIndex", Attr::ReadOnly, i32t, 111, {});
        V k15 = s.m.i32(15);
        V k8 = s.m.i32(8);
        V k7 = s.m.i32(7);
        V k2 = s.m.i32(2);
        V k16 = s.m.i32(16);
        V not31 = s.m.i32(~31u);
        V not15 = s.m.i32(~15u);
        V low = s.m.bin(c, Bin::And, lane, k15);
        V hi_word = s.m.cmp(c, pred::IUGE, low, k8);
        V word = s.m.select(c, hi_word, s2, s1);
        V n = s.m.bin(c, Bin::And, low, k7);
        V sh = s.m.bin(c, Bin::Shl, n, k2);
        V pick = s.m.bin(c, Bin::LShr, word, sh);
        pick = s.m.bin(c, Bin::And, pick, k15);
        V row;
        if (name == "v_permlanex16_b32") {
            V half = s.m.bin(c, Bin::And, lane, not31);
            V other = s.m.bin(c, Bin::And, lane, k16);
            other = s.m.bin(c, Bin::Xor, other, k16);
            row = s.m.bin(c, Bin::Or, half, other);
        } else {
            row = s.m.bin(c, Bin::And, lane, not15);
        }
        V from = s.m.bin(c, Bin::Or, row, pick);
        in(read_lane(xv, from));
        return;
    }
    if (name == "v_cvt_f32_f64") {
        auto [lo, hi] = f64_pair(inst, 0);
        V bits = f64_to_f32_bits(lo, hi);
        flt(as_float(bits));
        return;
    }
    if (name == "v_cvt_f32_i32" || name == "v_cvt_f32_u32" || name.starts_with("v_cvt_f32_ubyte")) {
        if (name.starts_with("v_cvt_f32_ubyte") && !(name == "v_cvt_f32_ubyte0" || name == "v_cvt_f32_ubyte1" || name == "v_cvt_f32_ubyte2" ||
                                                     name == "v_cvt_f32_ubyte3")) {
            in(integer(inst, name));
            return;
        }
        V a = sv(0);
        if (name.starts_with("v_cvt_f32_ubyte")) {
            V sh = s.m.i32(8u * static_cast<std::uint32_t>(name[15] - '0'));
            V ff = s.m.i32(0xff);
            a = s.m.bin(c, Bin::LShr, a, sh);
            a = s.m.bin(c, Bin::And, a, ff);
        }
        flt(s.m.cast(c, name == "v_cvt_f32_i32" ? Cast::SiToFp : Cast::UiToFp, a, s.t.f32));
        return;
    }
    if (name == "v_fma_mix_f32" || name == "v_fma_mixlo_f16" || name == "v_fma_mixhi_f16") {
        std::vector<V> xs;
        for (std::size_t i = 0; i < 3; ++i) {
            if (i >= v.src.size()) {
                fail("a missing operand");
            }
            const Operand& op = v.src[i];
            V xv;
            if (v.opsel_hi >> i & 1) {
                V t = f16_operand(inst, op);
                V h = t;
                if (v.opsel >> i & 1) {
                    V k16 = s.m.i32(16);
                    h = s.m.bin(c, Bin::LShr, t, k16);
                }
                xv = s.op(c, "legacyF16ToF32", Attr::ReadNone, s.t.f32, 131, {h});
            } else {
                V t = operand(inst, op);
                xv = as_float(t);
            }
            if (v.neg_hi >> i & 1) {
                xv = unary(6, xv);
            }
            if (v.neg >> i & 1) {
                V z = s.m.f32(0x80000000u);
                xv = s.m.bin(c, Bin::Sub, z, xv);
            }
            xs.push_back(xv);
        }
        V r = fmad(xs[0], xs[1], xs[2]);
        if (name == "v_fma_mix_f32") {
            flt(r);
            return;
        }
        if (v.clamp) {
            r = unary(7, r);
        }
        if (!v.dst || v.dst->kind != Operand::Vector) {
            fail("a mixed-precision result");
        }
        std::uint8_t reg = v.dst->a;
        V old;
        if (auto o = inst.read(Reg::v(reg))) {
            old = i32_of(*o);
        } else {
            old = s.m.i32(0);
        }
        V h = s.op(c, "legacyF32ToF16", Attr::ReadNone, s.t.i32, 130, {r});
        std::uint32_t keepk;
        if (name == "v_fma_mixhi_f16") {
            V k16 = s.m.i32(16);
            h = s.m.bin(c, Bin::Shl, h, k16);
            keepk = 0xFFFF;
        } else {
            keepk = 0xFFFF0000u;
        }
        V keep = s.m.i32(keepk);
        old = s.m.bin(c, Bin::And, old, keep);
        V xv = s.m.bin(c, Bin::Or, old, h);
        write(inst, Reg::v(reg), xv, g);
        return;
    }
    if (name == "v_cvt_pkrtz_f16_f32") {
        V a = f(0);
        V bb = f(1);
        V lo = f16_toward_zero(a);
        V hi = f16_toward_zero(bb);
        V sh = s.m.i32(16);
        hi = s.m.bin(c, Bin::Shl, hi, sh);
        in(s.m.bin(c, Bin::Or, lo, hi));
        return;
    }
    if (name == "v_cvt_f16_f32" && v.opsel == 0) {
        V a = f(0);
        V r = s.op(c, "legacyF32ToF16", Attr::ReadNone, s.t.i32, 130, {a});
        if (!v.dst || v.dst->kind != Operand::Vector) {
            fail("a 16-bit result outside a VGPR");
        }
        r = low_half(inst, v.dst->a, r);
        in(r);
        return;
    }
    if (name == "v_pack_b32_f16") {
        std::vector<V> halves_v;
        for (std::size_t k = 0; k < 2; ++k) {
            if (k >= v.src.size()) {
                fail("a missing operand");
            }
            V xv = f16_operand(inst, v.src[k]);
            if (v.opsel >> k & 1) {
                V k16 = s.m.i32(16);
                xv = s.m.bin(c, Bin::LShr, xv, k16);
            }
            V mask = s.m.i32((v.abs >> k & 1) ? 0x7FFF : 0xFFFF);
            xv = s.m.bin(c, Bin::And, xv, mask);
            if (v.neg >> k & 1) {
                V sign = s.m.i32(0x8000);
                xv = s.m.bin(c, Bin::Xor, xv, sign);
            }
            halves_v.push_back(xv);
        }
        V k16 = s.m.i32(16);
        V hi = s.m.bin(c, Bin::Shl, halves_v[1], k16);
        in(s.m.bin(c, Bin::Or, halves_v[0], hi));
        return;
    }
    if (name == "v_cvt_f32_f16" && v.opsel == 0) {
        V a = sv(0);
        flt(s.op(c, "legacyF16ToF32", Attr::ReadNone, s.t.f32, 131, {a}));
        return;
    }
    if (name == "v_cndmask_b32") {
        static constexpr Why UNTRACKED = "a select on an untracked mask";
        std::optional<Value> mask;
        if (v.src.size() > 2 && v.src[2].kind == Operand::Scalar) {
            mask = inst.read(Reg::s(v.src[2].a));
        }
        std::optional<Bool> mb;
        if (mask) {
            if (auto it = st().masks.find(*mask); it != st().masks.end()) {
                mb = it->second;
            }
        }
        V cond;
        if (mb) {
            cond = boolean(*mb, c);
        } else if (mask_data == MaskData::Console) {
            if (local) {
                fail("a constant lane mask in a half-wave");
            }
            if (!mask) {
                fail(UNTRACKED);
            }
            V lo;
            try {
                lo = i32_of(*mask);
            } catch (const WhyError&) {
                fail(UNTRACKED);
            }
            std::optional<Value> high;
            if (v.src.size() > 2 && v.src[2].kind == Operand::Scalar && x.native->wave64) {
                high = inst.read(Reg::s(static_cast<std::uint8_t>(v.src[2].a + 1)));
                if (!high) {
                    fail(UNTRACKED);
                }
            }
            std::optional<V> hi;
            if (high) {
                try {
                    hi = i32_of(*high);
                } catch (const WhyError&) {
                    fail(UNTRACKED);
                }
            }
            cross_lane(false);
            cond = lane_bit(c, lo, hi);
        } else {
            fail(UNTRACKED);
        }
        if (v.abs != 0 || v.neg != 0) {
            V a = f(0);
            V bb = f(1);
            flt(s.m.select(c, cond, bb, a));
            return;
        }
        V a = sv(0);
        V bb = sv(1);
        in(s.m.select(c, cond, bb, a));
        return;
    }
    if (name == "v_add_co_u32" || name == "v_sub_co_u32" || name == "v_subrev_co_u32" || name == "v_add_co_ci_u32" || name == "v_sub_co_ci_u32" ||
        name == "v_subrev_co_ci_u32") {
        V a = sv(0);
        V bb = sv(1);
        if (name.find("subrev") != std::string_view::npos) {
            std::swap(a, bb);
        }
        std::optional<V> cin;
        if (name.ends_with("_ci_u32")) {
            std::optional<Value> m;
            if (v.src.size() > 2 && v.src[2].kind == Operand::Scalar) {
                m = inst.read(Reg::s(v.src[2].a));
            }
            std::optional<Bool> mb;
            if (m) {
                if (auto it = st().masks.find(*m); it != st().masks.end()) {
                    mb = it->second;
                }
            }
            if (!mb) {
                fail("a carry-in from an untracked mask");
            }
            cin = boolean(*mb, c);
        }
        auto [r, carry] = add_sub_carry(c, name.starts_with("v_add"), a, bb, cin);
        in(r);
        if (v.carry && v.carry->kind == Operand::Scalar) {
            if (auto w = inst.write(Reg::s(v.carry->a))) {
                lane_bits[*w] = carry;
            }
        }
        note_masks_missing();
        return;
    }
    if (name == "v_readlane_b32") {
        V a = sv(0);
        V lane = sv(1);
        if (mask_data == MaskData::Host && !lane.is(V::Const)) {
            s.flags |= dxil::flags::WAVE_OPS;
        } else {
            cross_lane(false);
        }
        if (exchange) {
            std::uint32_t slot = exchange->first;
            if (local && local_any) {
                V k31 = s.m.i32(31);
                V here = s.m.bin(cur, Bin::And, lane, k31);
                in(read_lane(a, here));
                return;
            }
            if (local) {
                fail("a lane read in a half-wave");
            }
            if (exchange->small) {
                in(read_lane(a, lane));
                return;
            }
            if (!all_lanes(cur_ir)) {
                need(cur_ir);
                fail("a lane read where not every lane runs");
            }
            V cl = console_lane();
            V is = s.m.cmp(cur, pred::EQ, cl, lane);
            V k = exchange_pair(slot);
            auto at = begin_guarded(is);
            V lds = s.lds();
            V ptr = s.m.element(cur, lds, k);
            s.m.store(cur, ptr, a);
            end_guarded(at, {});
            std::size_t c2 = cur;
            group_sync(c2);
            V ptr2 = s.m.element(c2, lds, k);
            V r = s.m.load(c2, ptr2);
            group_sync(c2);
            in(r);
            return;
        }
        in(read_lane(a, lane));
        return;
    }
    if (name == "v_writelane_b32") {
        V a = sv(0);
        V lane = sv(1);
        if (!v.dst || v.dst->kind != Operand::Vector) {
            fail("a lane write");
        }
        std::uint8_t reg = v.dst->a;
        V old;
        if (auto o = inst.read(Reg::v(reg))) {
            old = i32_of(*o);
        } else {
            old = s.m.i32(0);
        }
        cross_lane(false);
        if (local && !local_any) {
            fail("a lane write in a half-wave");
        }
        V me = console_lane();
        V is = s.m.cmp(c, pred::EQ, me, lane);
        in(s.m.select(c, is, a, old));
        return;
    }
    if (name == "v_readfirstlane_b32") {
        const auto& reads = x.res->waterfall.reads;
        if (!inst.writes.empty() && std::find(reads.begin(), reads.end(), inst.writes[0].second) != reads.end()) {
            in(sv(0));
            return;
        }
        if (!v.src.empty() && v.src[0].kind == Operand::Vector) {
            auto xr = inst.read(Reg::v(v.src[0].a));
            if (xr && own_bits.alike.contains(*xr)) {
                in(sv(0));
                return;
            }
        }
        V a = sv(0);
        auto [mine, any] = first_lane(a, g);
        bool alike = true;
        if (!v.src.empty() && v.src[0].kind == Operand::Vector) {
            auto xr = inst.read(Reg::v(v.src[0].a));
            alike = xr && uniform.contains(*xr);
        }
        bool halves_x = exchange && !exchange->small && !local;
        V r;
        if (halves_x && !alike) {
            if (!all_lanes(cur_ir)) {
                static const bool own = [] {
                    char* e = nullptr;
                    std::size_t n = 0;
                    bool on = _dupenv_s(&e, &n, "VICTORY_FIRSTLANE_OWN") == 0 && e && std::string_view(e) == "1";
                    std::free(e);
                    return on;
                }();
                if (own) {
                    in(mine);
                    return;
                }
                need(cur_ir);
                fail("a first-lane read where not every lane runs");
            }
            std::size_t cc = cur;
            if (!g && (dxil::shader_opt() & bitcode::opt::FIRST) != 0) {
                r = exchange_halves(cc, mine).first;
            } else {
                Ty i32t = s.t.i32;
                V has = s.m.cast(cc, Cast::ZExt, any, i32t);
                auto [lo_has, hi_has] = exchange_halves(cc, has);
                auto [lo, hi] = exchange_halves(cc, mine);
                V z = s.m.i32(0);
                V in_lo = s.m.cmp(cc, pred::NE, lo_has, z);
                V in_hi = s.m.cmp(cc, pred::NE, hi_has, z);
                hi = s.m.select(cc, in_hi, hi, lo);
                r = s.m.select(cc, in_lo, lo, hi);
            }
        } else {
            r = mine;
        }
        in(r);
        return;
    }
    in(integer(inst, name));
}

}   
