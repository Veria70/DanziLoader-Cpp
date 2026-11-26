// SPDX-License-Identifier: GPL-3.0-or-later
// Whole-wave sections (`emit/wave.rs`; DanziLoader / victory-perf, see
// NOTICE.md). See wave.hpp.
#include "wave.hpp"

#include <algorithm>
#include <bit>
#include <vector>

#include "resource.hpp"

namespace danzi::shader::wave {

using ir::Reg;
using ir::Value;

namespace {

std::optional<Op> op_of(std::string_view name) {
    if (name == "v_add_nc_u32" || name == "v_add_co_u32") {
        return Op::AddU;
    }
    if (name == "v_add_f32") {
        return Op::AddF;
    }
    if (name == "v_add_f16") {
        return Op::AddH;
    }
    if (name == "v_min_u32") {
        return Op::MinU;
    }
    if (name == "v_max_u32") {
        return Op::MaxU;
    }
    if (name == "v_min_i32") {
        return Op::MinI;
    }
    if (name == "v_max_i32") {
        return Op::MaxI;
    }
    if (name == "v_min_f32") {
        return Op::MinF;
    }
    if (name == "v_max_f32") {
        return Op::MaxF;
    }
    if (name == "v_or_b32") {
        return Op::Or;
    }
    if (name == "v_and_b32") {
        return Op::And;
    }
    if (name == "v_xor_b32") {
        return Op::Xor;
    }
    return std::nullopt;
}

bool identity(Op op, std::uint32_t k) {
    switch (op) {
        case Op::AddU:
        case Op::AddH:
        case Op::Or:
        case Op::Xor:
        case Op::MaxU: return k == 0;
        case Op::AddF: return k == 0 || k == 0x80000000u;
        case Op::MinU:
        case Op::And: return k == UINT32_MAX;
        case Op::MinI: return k == 0x7FFFFFFFu;
        case Op::MaxI: return k == 0x80000000u;
        case Op::MinF: return k == 0x7F800000u;
        case Op::MaxF: return k == 0xFF800000u;
    }
    return false;
}

bool counts(Op op) { return op == Op::AddU || op == Op::AddF || op == Op::AddH || op == Op::Xor; }

bool buddies(std::uint64_t a, std::uint64_t b) {
    int n = std::popcount(a);
    if (n != std::popcount(b) || !std::has_single_bit(static_cast<unsigned>(n)) || n == 64) {
        return false;
    }
    auto run = [&](std::uint64_t s) -> std::optional<std::uint32_t> {
        int first = std::countr_zero(s);
        if (first % n == 0 && (s >> first) == (std::uint64_t{1} << n) - 1) {
            return static_cast<std::uint32_t>(first);
        }
        return std::nullopt;
    };
    auto x = run(a), y = run(b);
    if (!x || !y) {
        return false;
    }
    std::uint32_t lo = std::min(*x, *y), hi = std::max(*x, *y);
    return lo % (2 * static_cast<std::uint32_t>(n)) == 0 && hi - lo == static_cast<std::uint32_t>(n);
}


struct Sym {
    enum Kind : std::uint8_t { Unknown, K, Own, Uni, Red, Red64F } kind = Unknown;
    std::uint32_t k = 0;

    Value a{};

    Value b{};

    Value c{};

    std::optional<Op> op;
    std::uint32_t ident = 0;
    std::uint64_t lanes = 0;
    bool high = false;

    static Sym unknown() { return {}; }
    static Sym konst(std::uint32_t x) {
        Sym s;
        s.kind = K;
        s.k = x;
        return s;
    }
    static Sym own(Value v) {
        Sym s;
        s.kind = Own;
        s.a = v;
        return s;
    }
    static Sym uni(Value v) {
        Sym s;
        s.kind = Uni;
        s.a = v;
        return s;
    }
    static Sym red(std::optional<Op> op, Value src, Value mask, std::uint32_t ident, std::uint64_t lanes) {
        Sym s;
        s.kind = Red;
        s.op = op;
        s.a = src;
        s.b = mask;
        s.ident = ident;
        s.lanes = lanes;
        return s;
    }
    static Sym red64(Value lo, Value hi, Value mask, std::uint64_t lanes, bool high) {
        Sym s;
        s.kind = Red64F;
        s.a = lo;
        s.b = hi;
        s.c = mask;
        s.lanes = lanes;
        s.high = high;
        return s;
    }
    bool operator==(const Sym& o) const {
        if (kind != o.kind) {
            return false;
        }
        switch (kind) {
            case Unknown: return true;
            case K: return k == o.k;
            case Own:
            case Uni: return a == o.a;
            case Red: return op == o.op && a == o.a && b == o.b && ident == o.ident && lanes == o.lanes;
            case Red64F: return a == o.a && b == o.b && c == o.c && lanes == o.lanes && high == o.high;
        }
        return false;
    }
};

using Lanes = std::vector<Sym>;
using ValMap = std::unordered_map<Value, Lanes, structure::ValueHash>;
using UniMap = std::unordered_map<Value, Sym, structure::ValueHash>;


struct DppLane {
    std::uint32_t src;
    bool zero, on;
};
std::optional<DppLane> dpp_lane(const Dpp& d, std::uint32_t l) {
    if (d.kind == Dpp::Lanes) {
        return DppLane{(l & ~7u) | (d.sel >> (3 * (l & 7)) & 7), false, true};
    }
    std::uint32_t row = l & ~15u, pos = l & 15, n = d.ctrl & 15u;
    bool enabled = (d.row_mask >> (l >> 4 & 3) & 1) != 0 && (d.bank_mask >> (l >> 2 & 3) & 1) != 0;
    std::uint32_t src;
    bool inside;
    std::uint16_t ctrl = d.ctrl;
    if (ctrl <= 0xFF) {
        src = (l & ~3u) | (static_cast<std::uint32_t>(ctrl) >> (2 * (l & 3)) & 3);
        inside = true;
    } else if (ctrl >= 0x101 && ctrl <= 0x10F) {
        src = l + n;
        inside = pos + n < 16;
    } else if (ctrl >= 0x111 && ctrl <= 0x11F) {
        src = l - n;
        inside = pos >= n;
    } else if (ctrl >= 0x121 && ctrl <= 0x12F) {
        src = row | ((pos - n) & 15);
        inside = true;
    } else if (ctrl == 0x140) {
        src = l ^ 15;
        inside = true;
    } else if (ctrl == 0x141) {
        src = l ^ 7;
        inside = true;
    } else if (ctrl >= 0x150 && ctrl <= 0x15F) {
        src = row | n;
        inside = true;
    } else if (ctrl >= 0x160 && ctrl <= 0x16F) {
        src = l ^ n;
        inside = true;
    } else {
        return std::nullopt;
    }
    if (inside) {
        return DppLane{src, false, enabled};
    }
    if (d.bound_ctrl) {
        return DppLane{l, true, enabled};
    }
    return DppLane{l, false, false};
}

Sym combine(Op op, const Sym& a, const Sym& b) {
    if (a.kind == Sym::K && identity(op, a.k)) {
        return b;
    }
    if (b.kind == Sym::K && identity(op, b.k)) {
        return a;
    }
    if (a.kind == Sym::Red && b.kind == Sym::Red && a.a == b.a && a.b == b.b && a.ident == b.ident && identity(op, a.ident) &&
        (!a.op || *a.op == op) && (!b.op || *b.op == op) && (!counts(op) || (a.lanes & b.lanes) == 0) && (op != Op::AddH || buddies(a.lanes, b.lanes))) {
        return Sym::red(op, a.a, a.b, a.ident, a.lanes | b.lanes);
    }
    return Sym::unknown();
}

std::optional<std::uint64_t> constant(const ir::Inst& inst) {
    auto* f = std::get_if<SopFields>(&inst.fields);
    if (!f || f->src.empty()) {
        return std::nullopt;
    }
    bool wide = inst.name() == "s_mov_b64";
    const Operand& s = f->src[0];
    if (s.kind == Operand::Scalar && s.a == NULL_CODE) {
        return 0;
    }
    if (s.kind == Operand::Const && s.a >= 128 && s.a <= 208) {
        auto x = resource::inline_constant(s.a);
        if (!x) {
            return std::nullopt;
        }
        return wide ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(*x))) : std::uint64_t{*x};
    }
    if (s.kind == Operand::Const && !wide) {
        auto x = resource::inline_constant(s.a);
        return x ? std::optional<std::uint64_t>(*x) : std::nullopt;
    }
    if (s.kind == Operand::Literal) {
        return std::uint64_t{s.literal};
    }
    return std::nullopt;
}

std::optional<Shape> single(const Sym& s, std::uint64_t all) {
    Shape sh;
    switch (s.kind) {
        case Sym::K:
            sh.kind = Shape::K;
            sh.k = s.k;
            return sh;
        case Sym::Own:
        case Sym::Uni:
            sh.kind = Shape::Value;
            sh.a = s.a;
            return sh;
        case Sym::Red:
            if (s.op && s.lanes == all) {
                sh.kind = Shape::Active;
                sh.op = *s.op;
                sh.a = s.a;
                sh.b = s.b;
                sh.ident = s.ident;
                return sh;
            }
            return std::nullopt;
        case Sym::Red64F:
            if (s.lanes == all) {
                sh.kind = Shape::ActiveF64;
                sh.a = s.a;
                sh.b = s.b;
                sh.c = s.c;
                sh.flag = s.high;
                return sh;
            }
            return std::nullopt;
        default: return std::nullopt;
    }
}

std::optional<Shape> per_lane(const Lanes& lanes, std::uint64_t all) {
    const Sym& first = lanes[0];
    auto own = [](std::size_t l, const Sym& s) -> std::optional<std::tuple<Value, Value, std::uint32_t>> {
        if (s.kind == Sym::Red && !s.op && s.lanes == std::uint64_t{1} << l) {
            return std::tuple{s.a, s.b, s.ident};
        }
        return std::nullopt;
    };
    if (auto m = own(0, first)) {
        bool all_own = true;
        for (std::size_t l = 0; l < lanes.size(); ++l) {
            if (own(l, lanes[l]) != m) {
                all_own = false;
                break;
            }
        }
        if (all_own) {
            Shape sh;
            sh.kind = Shape::Masked;
            sh.a = std::get<0>(*m);
            sh.b = std::get<1>(*m);
            sh.ident = std::get<2>(*m);
            return sh;
        }
    }
    if (std::all_of(lanes.begin(), lanes.end(), [&](const Sym& s) { return s == first; })) {
        return single(first, all);
    }
    std::optional<std::tuple<Value, Value, std::uint32_t, bool>> meta;
    std::optional<Op> op_found;
    for (std::size_t l = 0; l < lanes.size(); ++l) {
        const Sym& s = lanes[l];
        std::uint64_t below = (std::uint64_t{1} << l) - 1;
        if (s.kind == Sym::K && l == 0) {
            continue;
        }
        if (s.kind != Sym::Red) {
            return std::nullopt;
        }
        std::uint64_t set = s.lanes;
        bool inclusive = set == (below | (std::uint64_t{1} << l));
        if (!inclusive && set != below) {
            return std::nullopt;
        }
        if (l == 0 && !inclusive) {
            return std::nullopt;
        }
        if (!s.op && std::popcount(set) != 1) {
            return std::nullopt;
        }
        auto lane_meta = std::tuple{s.a, s.b, s.ident, inclusive};
        if (!meta) {
            meta = lane_meta;
        } else if (*meta != lane_meta) {
            return std::nullopt;
        }
        if (s.op) {
            if (!op_found) {
                op_found = s.op;
            } else if (*op_found != *s.op) {
                return std::nullopt;
            }
        }
    }
    if (!meta || !op_found) {
        return std::nullopt;
    }
    auto [src, mask, ident, inclusive] = *meta;
    Op op = *op_found;
    if (!identity(op, ident)) {
        return std::nullopt;
    }
    if (lanes[0].kind == Sym::K) {
        if (inclusive || lanes[0].k != ident) {
            return std::nullopt;
        }
    } else if (lanes[0].kind == Sym::Red) {
        if (!inclusive) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    Shape sh;
    sh.kind = Shape::Prefix;
    sh.op = op;
    sh.a = src;
    sh.b = mask;
    sh.ident = ident;
    sh.flag = inclusive;
    return sh;
}

std::unordered_map<Value, Shape, structure::ValueHash> shapes(const ValMap& vec, const UniMap& uni, std::uint32_t wave) {
    std::uint64_t all = wave == 64 ? UINT64_MAX : std::uint64_t{UINT32_MAX};
    std::unordered_map<Value, Shape, structure::ValueHash> out;
    for (const auto& [v, s] : uni) {
        if (auto sh = single(s, all)) {
            out[v] = *sh;
        }
    }
    for (const auto& [v, lanes] : vec) {
        if (auto sh = per_lane(lanes, all)) {
            out[v] = *sh;
        }
    }
    return out;
}

std::optional<std::uint32_t> lane_const(const Operand* op) {
    if (!op) {
        return std::nullopt;
    }
    if (op->kind == Operand::Const) {
        return resource::inline_constant(op->a);
    }
    if (op->kind == Operand::Literal) {
        return op->literal;
    }
    return std::nullopt;
}

std::unordered_map<Value, Shape, structure::ValueHash> simulate(const std::vector<ir::Inst>& insts, std::size_t from, std::size_t to,
                                                                std::uint32_t wave, Value saved) {
    std::uint64_t all = wave == 64 ? UINT64_MAX : std::uint64_t{UINT32_MAX};
    std::uint64_t exec = all;
    ValMap vec;
    UniMap uni;
    std::vector<Value> masked{saved};
    auto is_masked = [&](Value v) { return std::find(masked.begin(), masked.end(), v) != masked.end(); };
    for (std::size_t at = from; at < to; ++at) {
        const ir::Inst& inst = insts[at];
        auto name = inst.name();
        if (inst.write(Reg::s(ir::EXEC))) {
            std::optional<std::uint64_t> lo;
            if (name.starts_with("s_mov_b")) {
                lo = constant(inst);
            }
            if (!lo) {
                return shapes(vec, uni, wave);
            }
            exec = (name == "s_mov_b64" ? *lo : ((exec & ~std::uint64_t{0xFFFFFFFF}) | (*lo & 0xFFFFFFFF))) & all;
            continue;
        }
        if (inst.write(Reg::s(ir::EXEC + 1))) {
            std::optional<std::uint64_t> k;
            if (name.starts_with("s_mov_b")) {
                k = constant(inst);
            }
            if (!k) {
                return shapes(vec, uni, wave);
            }
            exec = ((exec & 0xFFFFFFFF) | ((*k & 0xFFFFFFFF) << 32)) & all;
            continue;
        }
        if (auto* f = std::get_if<SopFields>(&inst.fields)) {
            if (name.starts_with("s_and_b")) {
                bool any = false;
                for (const Operand& o : f->src) {
                    if (o.kind == Operand::Scalar) {
                        auto r = inst.read(Reg::s(o.a));
                        if (r && is_masked(*r)) {
                            any = true;
                            break;
                        }
                    }
                }
                if (any) {
                    for (const auto& w : inst.writes) {
                        if (w.first.kind == Reg::S) {
                            masked.push_back(w.second);
                            break;
                        }
                    }
                }
            }
            if (name == "s_mov_b32" && f->dst && f->dst->kind == Operand::Scalar && !f->src.empty() && f->dst->a != NULL_CODE) {
                if (auto w = inst.write(Reg::s(f->dst->a))) {
                    const Operand& s0 = f->src[0];
                    Sym sym;
                    if (s0.kind == Operand::Scalar && s0.a != NULL_CODE) {
                        if (auto x = inst.read(Reg::s(s0.a))) {
                            auto it = uni.find(*x);
                            sym = it != uni.end() ? it->second : Sym::uni(*x);
                        }
                    } else if (s0.kind == Operand::Const) {
                        auto c = resource::inline_constant(s0.a);
                        sym = c ? Sym::konst(*c) : Sym::unknown();
                    } else if (s0.kind == Operand::Literal) {
                        sym = Sym::konst(s0.literal);
                    }
                    uni[*w] = sym;
                }
            }
            for (const auto& w : inst.writes) {
                uni.try_emplace(w.second, Sym::unknown());
            }
            continue;
        }
        auto* v = std::get_if<Valu>(&inst.fields);
        if (!v) {
            for (const auto& w : inst.writes) {
                vec[w.second] = Lanes(wave, Sym::unknown());
            }
            continue;
        }
        auto lanes = [&](const Operand& op) -> Lanes {
            auto one = [&](const Sym& s) { return Lanes(wave, s); };
            if (op.kind == Operand::Vector) {
                auto x = inst.read(Reg::v(op.a));
                if (!x) {
                    return one(Sym::unknown());
                }
                auto it = vec.find(*x);
                if (it != vec.end()) {
                    return it->second;
                }
                return one(Sym::own(*x));
            }
            if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
                auto x = inst.read(Reg::s(op.a));
                if (!x) {
                    return one(Sym::unknown());
                }
                auto it = uni.find(*x);
                return one(it != uni.end() ? it->second : Sym::uni(*x));
            }
            if (op.kind == Operand::Const) {
                auto c = resource::inline_constant(op.a);
                return one(c ? Sym::konst(*c) : Sym::unknown());
            }
            if (op.kind == Operand::Literal) {
                return one(Sym::konst(op.literal));
            }
            return one(Sym::unknown());
        };
        bool modifiers = v->abs != 0 || v->neg != 0 || v->clamp || v->omod != 0 || v->sdwa.has_value();
        if (name == "v_readlane_b32") {
            if (!v->dst || v->dst->kind != Operand::Scalar || v->src.size() < 2) {
                continue;
            }
            auto l = lane_const(&v->src[1]);
            Lanes x = lanes(v->src[0]);
            if (auto w = inst.write(Reg::s(v->dst->a))) {
                Sym s = l && *l < x.size() ? x[*l] : Sym::unknown();
                uni[*w] = s;
            }
            continue;
        }
        if (name == "v_add_f64" && !modifiers && !v->dpp && v->dst && v->dst->kind == Operand::Vector) {
            std::uint8_t dreg = v->dst->a;
            auto dst_lo = inst.write(Reg::v(dreg));
            auto dst_hi = inst.write(Reg::v(static_cast<std::uint8_t>(dreg + 1)));
            if (dst_lo && dst_hi) {
                auto pair_lanes = [&](const Operand* op) -> std::optional<std::pair<Lanes, Lanes>> {
                    if (!op) {
                        return std::nullopt;
                    }
                    if (op->kind == Operand::Vector) {
                        Lanes a = lanes(Operand::vector(op->a, 1));
                        Lanes b = lanes(Operand::vector(static_cast<std::uint8_t>(op->a + 1), 1));
                        return std::pair{std::move(a), std::move(b)};
                    }
                    if (op->kind == Operand::Scalar && op->a != NULL_CODE) {
                        Lanes a = lanes(Operand::scalar(op->a, 1));
                        Lanes b = lanes(Operand::scalar(static_cast<std::uint8_t>(op->a + 1), 1));
                        return std::pair{std::move(a), std::move(b)};
                    }
                    return std::nullopt;
                };
                Lanes old_lo = lanes(Operand::vector(dreg, 1));
                Lanes old_hi = lanes(Operand::vector(static_cast<std::uint8_t>(dreg + 1), 1));
                auto f64_term = [](const Sym& lo_s, const Sym& hi_s) -> std::optional<std::tuple<Value, Value, Value, std::uint64_t>> {
                    if (lo_s.kind == Sym::Red && !lo_s.op && lo_s.ident == 0 && hi_s.kind == Sym::Red && !hi_s.op && hi_s.ident == 0 &&
                        lo_s.b == hi_s.b && lo_s.lanes == hi_s.lanes) {
                        return std::tuple{lo_s.a, hi_s.a, lo_s.b, lo_s.lanes};
                    }
                    if (lo_s.kind == Sym::Red64F && !lo_s.high && hi_s.kind == Sym::Red64F && hi_s.high && lo_s.a == hi_s.a && lo_s.b == hi_s.b &&
                        lo_s.c == hi_s.c && lo_s.lanes == hi_s.lanes) {
                        return std::tuple{lo_s.a, lo_s.b, lo_s.c, lo_s.lanes};
                    }
                    return std::nullopt;
                };
                Lanes out_lo(wave, Sym::unknown()), out_hi(wave, Sym::unknown());
                auto pa = pair_lanes(v->src.size() > 0 ? &v->src[0] : nullptr);
                auto pb = pair_lanes(v->src.size() > 1 ? &v->src[1] : nullptr);
                if (pa && pb) {
                    for (std::size_t l = 0; l < wave; ++l) {
                        Sym rl, rh;
                        auto ta = f64_term(pa->first[l], pa->second[l]);
                        auto tb = f64_term(pb->first[l], pb->second[l]);
                        if (ta && tb && std::get<0>(*ta) == std::get<0>(*tb) && std::get<1>(*ta) == std::get<1>(*tb) &&
                            std::get<2>(*ta) == std::get<2>(*tb) && (std::get<3>(*ta) & std::get<3>(*tb)) == 0) {
                            std::uint64_t ls = std::get<3>(*ta) | std::get<3>(*tb);
                            rl = Sym::red64(std::get<0>(*ta), std::get<1>(*ta), std::get<2>(*ta), ls, false);
                            rh = Sym::red64(std::get<0>(*ta), std::get<1>(*ta), std::get<2>(*ta), ls, true);
                        }
                        bool on = (exec >> l & 1) != 0;
                        out_lo[l] = on ? rl : old_lo[l];
                        out_hi[l] = on ? rh : old_hi[l];
                    }
                }
                vec[*dst_lo] = std::move(out_lo);
                vec[*dst_hi] = std::move(out_hi);
                continue;
            }
        }
        if (!v->dst || v->dst->kind != Operand::Vector) {
            for (const auto& w : inst.writes) {
                uni[w.second] = Sym::unknown();
            }
            continue;
        }
        std::uint8_t dreg = v->dst->a;
        auto dst = inst.write(Reg::v(dreg));
        if (!dst) {
            continue;
        }
        Lanes old = lanes(Operand::vector(dreg, 1));
        std::optional<Lanes> src0;
        if (!v->src.empty()) {
            src0 = lanes(v->src[0]);
        }
        std::uint64_t writes = exec;
        std::optional<Lanes> fetched;
        if (v->dpp && src0) {
            if (exec != all) {
                vec[*dst] = Lanes(wave, Sym::unknown());
                continue;
            }
            Lanes f;
            f.reserve(wave);
            bool ok = true;
            for (std::uint32_t l = 0; l < wave; ++l) {
                auto dl = dpp_lane(*v->dpp, l);
                if (dl && dl->src < wave) {
                    f.push_back(dl->zero ? Sym::konst(0) : (*src0)[dl->src]);
                    if (!dl->on) {
                        writes &= ~(std::uint64_t{1} << l);
                    }
                } else {
                    ok = false;
                }
            }
            if (ok) {
                fetched = std::move(f);
            }
        } else if (!v->dpp && src0) {
            fetched = *src0;
        }
        auto src = [&](std::size_t k) -> std::optional<Lanes> {
            if (k < v->src.size()) {
                return lanes(v->src[k]);
            }
            return std::nullopt;
        };
        Lanes result;
        if (modifiers || !fetched) {
            result.assign(wave, Sym::unknown());
        } else if (name == "v_mov_b32") {
            result = std::move(*fetched);
        } else if (name == "v_bfrev_b32") {
            for (const Sym& s : *fetched) {
                if (s.kind == Sym::K) {
                    std::uint32_t r = 0;
                    for (int i = 0; i < 32; ++i) {
                        r |= ((s.k >> i) & 1u) << (31 - i);
                    }
                    result.push_back(Sym::konst(r));
                } else {
                    result.push_back(Sym::unknown());
                }
            }
        } else if (name == "v_cndmask_b32") {
            const Lanes& a = *fetched;
            std::optional<Value> mask;
            if (v->src.size() > 2 && v->src[2].kind == Operand::Scalar) {
                mask = inst.read(Reg::s(v->src[2].a));
            }
            Lanes b = src(1).value_or(Lanes{});
            for (std::size_t l = 0; l < wave; ++l) {
                if (a[l].kind == Sym::K && l < b.size() && b[l].kind == Sym::Own && mask && is_masked(*mask)) {
                    result.push_back(Sym::red(std::nullopt, b[l].a, *mask, a[l].k, std::uint64_t{1} << l));
                } else {
                    result.push_back(Sym::unknown());
                }
            }
        } else if (name == "v_permlanex16_b32") {
            const Lanes& a = *fetched;
            auto s1 = lane_const(v->src.size() > 1 ? &v->src[1] : nullptr);
            auto s2 = lane_const(v->src.size() > 2 ? &v->src[2] : nullptr);
            if (s1 && s2) {
                std::uint64_t s = std::uint64_t{*s1} | std::uint64_t{*s2} << 32;
                for (std::uint32_t l = 0; l < wave; ++l) {
                    std::uint32_t other = (l & ~31u) | ((l & 16) ^ 16);
                    auto pick = static_cast<std::uint32_t>(s >> (4 * (l & 15)) & 15);
                    result.push_back(a[other | pick]);
                }
            } else {
                result.assign(wave, Sym::unknown());
            }
        } else if (name == "v_sub_nc_u32") {
            const Lanes& a = *fetched;
            Lanes b = src(1).value_or(Lanes{});
            for (std::size_t l = 0; l < wave; ++l) {
                const Sym& x = a[l];
                if (x.kind == Sym::Red && (!x.op || *x.op == Op::AddU) && x.ident == 0 && l < b.size() && b[l].kind == Sym::Red &&
                    (!b[l].op || *b[l].op == Op::AddU) && b[l].ident == 0 && x.a == b[l].a && x.b == b[l].b && (x.lanes & b[l].lanes) == b[l].lanes) {
                    std::uint64_t rest = x.lanes & ~b[l].lanes;
                    result.push_back(rest == 0 ? Sym::konst(0) : Sym::red(Op::AddU, x.a, x.b, 0, rest));
                } else {
                    result.push_back(Sym::unknown());
                }
            }
        } else {
            const Lanes& a = *fetched;
            auto op = op_of(name);
            auto b = src(1);
            if (op && b) {
                for (std::size_t l = 0; l < wave; ++l) {
                    result.push_back(combine(*op, a[l], (*b)[l]));
                }
            } else {
                result.assign(wave, Sym::unknown());
            }
        }
        Lanes merged(wave);
        for (std::size_t l = 0; l < wave; ++l) {
            merged[l] = (writes >> l & 1) ? result[l] : old[l];
        }
        vec[*dst] = std::move(merged);
    }
    return shapes(vec, uni, wave);
}

std::uint8_t saved_code(const ir::Inst& inst) {
    if (auto* f = std::get_if<SopFields>(&inst.fields); f && f->dst && f->dst->kind == Operand::Scalar) {
        return f->dst->a;
    }
    return NULL_CODE;
}

}   

std::unordered_map<std::size_t, Section> sections(const ir::Program& p, std::size_t block) {
    const auto& insts = p.blocks[block].insts;
    std::uint32_t wave = p.wave64 ? 64 : 32;
    std::unordered_map<std::size_t, Section> out;
    std::size_t i = 0;
    while (i < insts.size()) {
        const ir::Inst& inst = insts[i];
        auto name = inst.name();
        std::optional<Operand> all;
        if (name == "s_or_saveexec_b64" || name == "s_or_saveexec_b32") {
            all = Operand::constant(193);
        } else if (name == "s_nand_saveexec_b64" || name == "s_nand_saveexec_b32") {
            all = Operand::constant(128);
        }
        bool start = false;
        auto* f = std::get_if<SopFields>(&inst.fields);
        if (all && f && !f->src.empty() && f->src[0] == *all) {
            start = true;
        }
        std::optional<Value> saved;
        if (start && f->dst && f->dst->kind == Operand::Scalar) {
            saved = inst.write(Reg::s(f->dst->a));
        }
        if (!saved) {
            ++i;
            continue;
        }
        std::optional<std::size_t> end;
        for (std::size_t j = i + 1; j < insts.size(); ++j) {
            if (insts[j].write(Reg::s(ir::EXEC)) && insts[j].read(Reg::s(saved_code(inst))) == saved) {
                end = j;
                break;
            }
        }
        if (!end) {
            break;
        }
        Section sec;
        sec.end = *end;
        sec.shapes = simulate(insts, i + 1, *end, wave, *saved);
        out.emplace(i, std::move(sec));
        i = *end + 1;
    }
    return out;
}

}   
