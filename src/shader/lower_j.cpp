// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// pixel input interpolation. See lower_impl.hpp.
#include <algorithm>
#include <bit>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

void Lower::interp(const Inst& inst, Site site) {
    auto it = interps.find(site);
    if (it == interps.end()) {
        fail("the first half of an interpolation");
    }
    const Interp& ip = *it->second;
    auto name = inst.name();
    Operand dst;
    std::optional<Operand> srco;
    if (auto* vf = std::get_if<VintrpFields>(&inst.fields)) {
        dst = vf->dst;
        srco = vf->src;
    } else {
        const auto& v = std::get<Valu>(inst.fields);
        if (v.abs != 0 || v.neg != 0) {
            fail("interpolation operand modifiers");
        }
        if (!v.dst) {
            fail("an interpolation");
        }
        dst = *v.dst;
        if (v.src.size() > 1) {
            srco = v.src[1];
        }
    }
    if (dst.kind != Operand::Vector) {
        fail("an interpolation");
    }
    std::uint8_t reg = dst.a;
    auto flt = [&](std::optional<Operand> op) {
        if (!op) {
            fail("an interpolation operand");
        }
        V xv = operand(inst, *op);
        return as_float(xv);
    };
    std::optional<V> add;
    io::Attribute attribute;
    Eval eval;
    switch (ip.kind) {
        case Interp::Interpolated: {
            attribute = ip.attribute;
            auto mode = interp_mode(ip.barycentric);
            if (!mode) {
                fail("pull-model interpolation");
            }
            if (per_vertex(attribute.row)) {
                std::optional<V> ij[2];
                for (std::uint8_t index = 0; index < 2; ++index) {
                    Val bv = barycentric(ip.barycentric, index);
                    if (bv.kind == Val::Missing) {
                        fail(bv.why);
                    }
                    ij[index] = as_float(bv.v);
                }
                eval.weights = true;
                eval.p0 = true;
                eval.i = ij[0];
                eval.j = ij[1];
            } else {
                eval = Eval::of_mode(*mode);
            }
            break;
        }
        case Interp::Vertex: {
            attribute = ip.attribute;
            if (ip.vertex == 2) {
                eval = Eval::of_mode(1);
            } else if (interpolated(attribute.row)) {
                V one = s.m.f32(std::bit_cast<std::uint32_t>(1.0f));
                eval.weights = true;
                eval.p0 = false;
                if (ip.vertex == 0) {
                    eval.i = one;
                } else {
                    eval.j = one;
                }
            } else {
                fail("a non-provoking vertex value");
            }
            break;
        }
        case Interp::Weighted: {
            attribute = ip.attribute;
            if (name.starts_with("v_interp_p1")) {
                eval.weights = true;
                eval.p0 = true;
                eval.i = flt(srco);
            } else {
                std::optional<Operand> old;
                if (auto* v = std::get_if<Valu>(&inst.fields)) {
                    if (v->src.size() > 2) {
                        old = v->src[2];
                    }
                } else {
                    old = dst;
                }
                add = flt(old);
                eval.weights = true;
                eval.p0 = false;
                eval.j = flt(srco);
            }
            break;
        }
    }
    std::optional<std::pair<bool, std::vector<std::pair<std::uint8_t, std::optional<std::uint8_t>>>>> packing;
    if (x.native->io.kind == StageIo::Ps) {
        for (const PsInput& pi : x.native->io.ps_inputs) {
            if (pi.row == attribute.row && pi.fp16) {
                std::vector<std::pair<std::uint8_t, std::optional<std::uint8_t>>> packs;
                if (pi.pack2) {
                    packs = input_packs(x.native->io, s.inputs);
                }
                packing = std::pair{pi.pack2, std::move(packs)};
                break;
            }
        }
    }
    bool f16 = name.ends_with("_f16");
    auto total = [&](std::uint8_t row, std::uint8_t ch) {
        V xv = eval_channel(row, ch, eval);
        xv = as_float(xv);
        if (add) {
            return s.m.bin(cur, Bin::Add, *add, xv);
        }
        return xv;
    };
    V xv;
    if (!packing) {
        if (f16 || attribute.high) {
            fail("a 16-bit read of a 32-bit input");
        }
        V t = total(attribute.row, attribute.channel);
        xv = as_int(t);
    } else {
        bool pack2 = packing->first;
        const auto& packs = packing->second;
        auto half = [&](std::uint8_t h) -> std::optional<V> {
            std::optional<std::pair<std::uint8_t, std::uint8_t>> srcp;
            if (pack2) {
                srcp = packed_half(s.inputs, packs, attribute.row, h);
            } else if (h % 2 == 0) {
                srcp = std::pair{attribute.row, static_cast<std::uint8_t>(h / 2)};
            }
            if (!srcp) {
                return std::nullopt;
            }
            return total(srcp->first, srcp->second);
        };
        auto to_f16 = [&](V f) { return s.op(cur, "legacyF32ToF16", Attr::ReadNone, s.t.i32, 130, {f}); };
        auto h = static_cast<std::uint8_t>(2 * attribute.channel + (attribute.high ? 1 : 0));
        std::size_t c = cur;
        if (name == "v_interp_p1ll_f16") {
            auto f = half(h);
            if (!f) {
                fail("a packed input half no element holds");
            }
            xv = as_int(*f);
        } else if (f16) {
            auto f = half(h);
            if (!f) {
                fail("a packed input half no element holds");
            }
            V lo = to_f16(*f);
            xv = low_half(inst, reg, lo);
        } else if (!eval.weights && eval.mode == 1) {
            auto h0 = static_cast<std::uint8_t>(2 * attribute.channel);
            V z = s.m.i32(0);
            auto fl = half(h0);
            V lo = fl ? to_f16(*fl) : z;
            auto fh = half(static_cast<std::uint8_t>(h0 + 1));
            V hi = fh ? to_f16(*fh) : z;
            V k16 = s.m.i32(16);
            hi = s.m.bin(c, Bin::Shl, hi, k16);
            xv = s.m.bin(c, Bin::Or, lo, hi);
        } else {
            fail("32-bit interpolation of a 16-bit input");
        }
    }
    write(inst, Reg::v(reg), xv, std::nullopt);
}

bool Lower::per_vertex(std::uint8_t row) const {
    for (const auto& [_, i] : x.io->interps) {
        switch (i.kind) {
            case Interp::Weighted:
                if (i.attribute.row == row) {
                    return true;
                }
                break;
            case Interp::Vertex:
                if (i.attribute.row == row && i.vertex != 2 && interpolated(row)) {
                    return true;
                }
                break;
            default: break;
        }
    }
    return false;
}

bool Lower::interpolated(std::uint8_t row) const {
    for (const auto& [_, i] : x.io->interps) {
        if ((i.kind == Interp::Interpolated || i.kind == Interp::Weighted) && i.attribute.row == row) {
            return true;
        }
    }
    return false;
}

V Lower::eval_channel(std::uint8_t row, std::uint8_t ch, const Eval& eval) {
    auto f = find_input(row, ch);
    if (!f) {
        fail("an input the signature lacks");
    }
    auto [id, col] = *f;
    std::uint8_t mode = eval.weights ? 1 : eval.mode;
    SigElement& e = s.inputs[id];
    if (e.interp != mode && e.usage != 0) {
        fail("one input interpolated two ways");
    }
    e.interp = mode;
    if (!eval.weights) {
        Val v = load_any_input(id, col);
        if (v.kind != Val::I32) {
            fail("an input");
        }
        return v.v;
    }
    V v0 = vertex_input(id, col, 0);
    std::size_t c = cur;
    std::optional<V> acc;
    if (eval.p0) {
        acc = v0;
    }
    std::pair<std::uint8_t, std::optional<V>> ws[2] = {{std::uint8_t{1}, eval.i}, {std::uint8_t{2}, eval.j}};
    for (const auto& [k, w] : ws) {
        if (!w) {
            continue;
        }
        V vk = vertex_input(id, col, k);
        V d = s.m.bin(c, Bin::Sub, vk, v0);
        acc = acc ? fmad(*w, d, *acc) : s.m.bin(c, Bin::Mul, *w, d);
    }
    if (!acc) {
        fail("an interpolation");
    }
    return as_int(*acc);
}

V Lower::vertex_input(std::size_t id, std::uint8_t col, std::uint8_t k) {
    SigElement& e = s.inputs[id];
    e.usage = static_cast<std::uint8_t>(e.usage | (1u << col));
    bool is_int = e.comp != 9;
    Ty ty = is_int ? s.t.i32 : s.t.f32;
    const char* suffix = is_int ? ".i32" : ".f32";
    s.flags |= dxil::flags::BARYCENTRICS;
    std::size_t entry = s.entry;
    V a0 = s.m.i32(static_cast<std::uint32_t>(id));
    V a1 = s.m.i32(0);
    V a2 = s.m.i8(col);
    V a3 = s.m.i8(k);
    V v = s.op(entry, std::string("attributeAtVertex") + suffix, Attr::ReadNone, ty, 137, {a0, a1, a2, a3});
    if (is_int) {
        return s.m.cast(entry, Cast::Bitcast, v, s.t.f32);
    }
    return v;
}

V Lower::low_half(const Inst& inst, std::uint8_t reg, V xv) {
    std::size_t c = cur;
    V old = old_or_zero(inst, Reg::v(reg));
    V high = s.m.i32(0xFFFF0000u);
    V low = s.m.i32(0xFFFF);
    old = s.m.bin(c, Bin::And, old, high);
    xv = s.m.bin(c, Bin::And, xv, low);
    return s.m.bin(c, Bin::Or, old, xv);
}

}   
