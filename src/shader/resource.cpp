// SPDX-License-Identifier: GPL-3.0-or-later
// Resource tracking (`resource.rs`; DanziLoader / victory-perf, see
// NOTICE.md). See resource.hpp. The reference's `Result<_, &str>` is `Res`
// here (a value or the reason it failed), its `Option` std::optional.
#include "resource.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>

#include "error.hpp"

namespace danzi::shader::resource {

using ir::Reg;
using ir::Value;
using structure::Bool;
using structure::LaneExit;

namespace {

template <class T>
struct Res {
    T value{};
    const char* why = nullptr;
    Res() = default;
    Res(T v) : value(std::move(v)) {}
    bool ok() const { return why == nullptr; }
};
struct Fail {
    const char* why;
    template <class T>
    operator Res<T>() const {
        Res<T> r;
        r.why = why;
        return r;
    }
};

using Status = const char*;

#define TRY(name, expr)               \
    auto name##_r = (expr);           \
    if (!name##_r.ok()) {             \
        return Fail{name##_r.why};    \
    }                                 \
    auto& name = name##_r.value


constexpr std::string_view RAW[5] = {
    "an address not from a root parameter",
    "an address from a root parameter that is no address",
    "an address in an SGPR that is no root parameter",
    "an address loaded from memory",
    "an address from a phi",
};

constexpr std::size_t MAX_ROUNDS = 64, MAX_JOINS = 8, MAX_COPIES = 64;


struct Term {
    bool is_const = false;
    std::uint32_t c = 0;
    Value v{};
    static Term constant(std::uint32_t x) { return {true, x, {}}; }
    static Term value(Value x) { return {false, 0, x}; }
    bool operator==(const Term& o) const { return is_const == o.is_const && (is_const ? c == o.c : v == o.v); }
};


struct Sum {
    std::optional<Value> base;
    std::uint32_t offset = 0;
    std::optional<Value> dynamic;
};

struct Moved {
    bool is_const;
    std::uint32_t c;
    Value v;
    bool operator==(const Moved& o) const { return is_const == o.is_const && (is_const ? c == o.c : v == o.v); }
};

using Known = std::vector<std::pair<Bool, bool>>;

std::optional<std::uint32_t> lane_of(const Operand& op) {
    if (op.kind == Operand::Const) {
        return inline_constant(op.a);
    }
    if (op.kind == Operand::Literal) {
        return op.literal;
    }
    return std::nullopt;
}


std::optional<Moved> copy_source(const ir::Inst& inst, std::size_t k) {
    auto* f = std::get_if<SopFields>(&inst.fields);
    if (!f) {
        return std::nullopt;
    }
    auto name = inst.name();
    if (name == "s_movk_i32") {
        if (!f->imm) {
            return std::nullopt;
        }
        return Moved{true, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(*f->imm))), {}};
    }
    if (f->src.empty()) {
        return std::nullopt;
    }
    const Operand& src = f->src[0];
    bool mov = name == "s_mov_b32" || name == "s_mov_b64";
    if (mov && src.kind == Operand::Scalar && src.a != NULL_CODE) {
        auto v = inst.read(Reg::s(static_cast<std::uint8_t>(src.a + k)));
        return v ? std::optional(Moved{false, 0, *v}) : std::nullopt;
    }
    if (mov && src.kind == Operand::Scalar) {
        return Moved{true, 0, {}};
    }
    if (name == "s_mov_b32" && src.kind == Operand::Literal) {
        return Moved{true, src.literal, {}};
    }
    if (name == "s_mov_b32" && src.kind == Operand::Const) {
        auto x = inline_constant(src.a);
        return x ? std::optional(Moved{true, *x, {}}) : std::nullopt;
    }
    if (name == "s_brev_b32" && src.kind == Operand::Const) {
        auto x = inline_constant(src.a);
        if (!x) {
            return std::nullopt;
        }
        std::uint32_t r = 0;
        for (int i = 0; i < 32; ++i) {
            r |= ((*x >> i) & 1u) << (31 - i);
        }
        return Moved{true, r, {}};
    }
    if (name == "s_not_b32" && src.kind == Operand::Const) {
        auto x = inline_constant(src.a);
        return x ? std::optional(Moved{true, ~*x, {}}) : std::nullopt;
    }
    if (name == "s_mov_b64" && src.kind == Operand::Const && src.a >= 128 && src.a <= 208) {
        auto low = inline_constant(src.a);
        if (!low) {
            return std::nullopt;
        }
        return Moved{true, k == 0 ? *low : static_cast<std::uint32_t>(static_cast<std::int32_t>(*low) >> 31), {}};
    }
    return std::nullopt;
}

class Tracer {
public:
    Tracer(const ir::Program& p, const std::vector<RootParam>& layout, HwStage stage, const structure::Structured* lanes);

    const ir::Program& p;
    const std::vector<RootParam>& layout;
    HwStage stage;
    std::vector<std::optional<Site>> defs;
    std::vector<std::optional<std::pair<std::size_t, std::size_t>>> phis;
    std::vector<std::optional<Reg>> inputs;
    std::vector<Term> origin_;
    mutable std::vector<Site> lane_copied;
    const structure::Structured* lanes;
    mutable bool read_first_lane = false;
    mutable structure::Meets meets;
    std::vector<std::size_t> rpo;
    mutable std::optional<std::size_t> at;

    std::vector<Term> propagate_copies(std::size_t n) const;
    bool meet(const structure::Structured& l, const Known& conditions) const { return l.bools.meet_seen(conditions, meets); }
    const ir::Inst& inst(Site s) const { return p.blocks[s.first].insts[s.second]; }
    const ir::Inst* def(Value v) const { return defs[v.id] ? &inst(*defs[v.id]) : nullptr; }
    Term origin(Value v) const { return origin_[v.id]; }
    std::optional<Term> term(const ir::Inst& inst, const Operand& op) const;
    std::optional<std::pair<std::size_t, std::uint32_t>> param_of(Reg reg) const;
    std::optional<std::pair<const ir::Inst*, std::array<Term, 2>>> sop2(Value v, std::initializer_list<std::string_view> names) const;
    std::optional<RootIndexedView> root_indexed_view(const Descriptor& d, const std::vector<std::pair<Site, Address>>& scalar) const;
    Sum sum(Term t) const;
    std::optional<Value> code_join(Value v) const;
    bool code_high_in(Term t, std::uint32_t depth) const;
    std::optional<Value> code_base(Value v, std::uint32_t depth) const;
    bool is_high(Term t, Value high) const;
    std::optional<Value> first_lane(Value v) const;
    const ir::Inst* getpc(Value lo) const;
    std::optional<std::array<Term, 2>> vadd(Value v) const;
    std::optional<Term> vterm(const ir::Inst& inst, const Operand& op) const;
    Term vorigin(Value v) const;
    Res<Address> by_address(const char* why) const;
    Res<Address> flat_address(const ir::Inst& inst, const Opt& addr, const Opt& saddr, std::int16_t offset) const;
    Res<Address> address(Term lo, Term hi, Sum extra) const;
    bool high_half(Term hi, std::optional<Value> high) const;
    bool carried(Term hi, Value high) const;
    const char* why_no_base(std::optional<Value> dynamic) const;
    void leaves(Value v, std::vector<Value>& out) const;
    Res<Address> smem_address(const ir::Inst& inst, const Operand& base, std::int32_t offset, const Operand& soffset) const;
    Res<Source> descriptor(const ir::Inst& inst, const Operand& op, std::size_t at_block) const;
    Res<Source> classify(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const;
    std::optional<std::size_t> only_edge(std::size_t block, Known& known) const;
    bool anywhere(const Source& source) const;
    Term live(Term t) const;
    Res<std::optional<Source>> phi(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const;
    Res<std::optional<Source>> select(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const;
    std::optional<std::tuple<Value, Term, Term>> picked(Value v) const;
    std::optional<std::pair<Term, std::uint32_t>> set_bits(Value v) const;
    std::optional<Value> first_lane_source(Value v) const;
    std::optional<Value> first_lane_offset(Site load) const;
    bool kept_per_lane(const ir::Inst& inst, const Operand& op) const;
    std::optional<std::vector<Value>> first_lane_words(const ir::Inst& inst, const Operand& op) const;
    Term lane_term(Term t) const;
    bool varies(Term t) const;
    Res<Term> lane_value(Term t, const Known& known) const;
    Term vstep(Value v) const;
    std::optional<Term> raw(const ir::Inst& inst, const Operand& op) const;
    Status runs(Site site, const Known& known) const;
    Res<Source> per_lane(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const;
    Res<std::optional<Source>> fetched(const std::vector<Term>& words, const Known& known) const;
    Res<std::optional<Source>> lane_select(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const;
    std::optional<std::tuple<Value, Term, Term>> lane_picked(Value v) const;
    Res<std::optional<Source>> lane_phi(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const;
    bool reaches(std::size_t from, std::size_t to) const;
    Res<Waterfall> waterfall(std::size_t b, const std::vector<std::size_t>& accesses, const std::vector<Value>& reads) const;
    std::optional<Source> loaded(const std::vector<Term>& words) const;
    std::optional<Source> lane_copies(const std::vector<Term>& words) const;
    bool scalar_copies(Value v, std::vector<Term>& out) const;
    bool base_from_entry(Site site) const;
    std::optional<Moved> restored(const ir::Inst& inst) const;

    std::optional<Moved> spilled(Value v, std::uint32_t lane, std::vector<Value>& seen, bool& ok) const;
};

Tracer::Tracer(const ir::Program& prog, const std::vector<RootParam>& lay, HwStage st, const structure::Structured* l)
    : p(prog), layout(lay), stage(st), lanes(l) {
    std::uint32_t n = 0;
    for (const auto& i : p.inputs) {
        n = std::max(n, i.second.id + 1);
    }
    for (const auto& b : p.blocks) {
        for (const auto& phi : b.phis) {
            n = std::max(n, phi.value.id + 1);
        }
        for (const auto& inst : b.insts) {
            for (const auto& w : inst.writes) {
                n = std::max(n, w.second.id + 1);
            }
        }
    }
    defs.assign(n, std::nullopt);
    phis.assign(n, std::nullopt);
    inputs.assign(n, std::nullopt);
    for (const auto& [reg, v] : p.inputs) {
        inputs[v.id] = reg;
    }
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const auto& block = p.blocks[b];
        for (std::size_t i = 0; i < block.phis.size(); ++i) {
            phis[block.phis[i].value.id] = std::pair{b, i};
        }
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            for (const auto& w : block.insts[i].writes) {
                defs[w.second.id] = Site{b, i};
            }
        }
    }
    rpo.assign(p.blocks.size(), SIZE_MAX);
    auto order = ir::reverse_postorder(p.blocks);
    for (std::size_t k = 0; k < order.size(); ++k) {
        rpo[order[k]] = k;
    }
    origin_ = propagate_copies(n);
}


std::vector<Term> Tracer::propagate_copies(std::size_t n) const {
    std::vector<std::optional<Term>> o(n);
    for (const auto& [r, v] : p.inputs) {
        o[v.id] = Term::value(v);
    }
    auto order = ir::reverse_postorder(p.blocks);
    for (std::size_t round = 0; round < MAX_ROUNDS; ++round) {
        bool changed = false;
        for (std::size_t b : order) {
            const auto& block = p.blocks[b];
            for (const auto& phi : block.phis) {
                Term own = Term::value(phi.value);
                std::optional<Term> first;
                bool all = true;
                for (Value a : phi.args) {
                    if (!o[a.id] || *o[a.id] == own) {
                        continue;
                    }
                    if (!first) {
                        first = o[a.id];
                    } else if (!(*o[a.id] == *first)) {
                        all = false;
                    }
                }
                std::optional<Term> now;
                if (first) {
                    now = all ? *first : own;
                }
                if (now && !(o[phi.value.id] && *o[phi.value.id] == *now)) {
                    o[phi.value.id] = now;
                    changed = true;
                }
            }
            for (const auto& inst : block.insts) {
                for (std::size_t k = 0; k < inst.writes.size(); ++k) {
                    Value v = inst.writes[k].second;
                    std::optional<Moved> m = copy_source(inst, k);
                    if (!m) {
                        m = restored(inst);
                    }
                    std::optional<Term> now;
                    if (m && m->is_const) {
                        now = Term::constant(m->c);
                    } else if (m) {
                        now = o[m->v.id];
                    } else {
                        now = Term::value(v);
                    }
                    if (now && !(o[v.id] && *o[v.id] == *now)) {
                        o[v.id] = now;
                        changed = true;
                    }
                }
            }
        }
        if (!changed) {
            std::vector<Term> out(n);
            for (std::size_t v = 0; v < n; ++v) {
                out[v] = o[v] ? *o[v] : Term::value(Value{static_cast<std::uint32_t>(v)});
            }
            return out;
        }
    }
    unsupported(0, "copies that do not settle");
}

std::optional<Term> Tracer::term(const ir::Inst& in, const Operand& op) const {
    switch (op.kind) {
        case Operand::Scalar: {
            if (op.a == NULL_CODE) {
                return Term::constant(0);
            }
            auto v = in.read(Reg::s(op.a));
            return v ? std::optional(origin(*v)) : std::nullopt;
        }
        case Operand::Const: {
            auto c = inline_constant(op.a);
            return c ? std::optional(Term::constant(*c)) : std::nullopt;
        }
        case Operand::Literal: return Term::constant(op.literal);
        default: return std::nullopt;
    }
}

std::optional<std::pair<std::size_t, std::uint32_t>> Tracer::param_of(Reg reg) const {
    if (reg.kind != Reg::S) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < layout.size(); ++i) {
        std::uint8_t first = user_sgpr(stage, layout[i].user_data);
        if (reg.n < first) {
            continue;
        }
        std::uint32_t dword = std::uint32_t{reg.n} - first;
        if (dword < layout[i].sgprs) {
            return std::pair{i, dword};
        }
    }
    return std::nullopt;
}

std::optional<std::pair<const ir::Inst*, std::array<Term, 2>>> Tracer::sop2(Value v, std::initializer_list<std::string_view> names) const {
    const ir::Inst* in = def(v);
    if (!in) {
        return std::nullopt;
    }
    auto name = in->name();
    if (std::find(names.begin(), names.end(), name) == names.end() || in->writes.empty() || in->writes[0].second != v) {
        return std::nullopt;
    }
    auto* f = std::get_if<SopFields>(&in->fields);
    if (!f || f->src.size() < 2) {
        return std::nullopt;
    }
    auto a = term(*in, f->src[0]);
    if (!a) {
        return std::nullopt;
    }
    auto b = term(*in, f->src[1]);
    if (!b) {
        return std::nullopt;
    }
    return std::pair{in, std::array<Term, 2>{*a, *b}};
}

std::optional<RootIndexedView> Tracer::root_indexed_view(const Descriptor& d, const std::vector<std::pair<Site, Address>>& scalar) const {
    if (d.role != Role::Resource || d.source.kind != Source::Loaded) {
        return std::nullopt;
    }
    Site load = d.source.load;
    auto it = std::lower_bound(scalar.begin(), scalar.end(), load, [](const auto& e, const Site& s) { return e.first < s; });
    if (it == scalar.end() || it->first != load) {
        return std::nullopt;
    }
    Address address = it->second;
    if (address.param >= layout.size()) {
        return std::nullopt;
    }
    const RootParam& base = layout[address.param];
    if (base.root != 3) {
        return std::nullopt;
    }
    if (!address.dynamic) {
        return std::nullopt;
    }
    Value dynamic = *address.dynamic;
    Value input = dynamic;
    std::uint8_t shift = 0;
    if (auto s = sop2(dynamic, {"s_lshl_b32"}); s && !s->second[0].is_const && s->second[1].is_const) {
        input = s->second[0].v;
        shift = static_cast<std::uint8_t>(s->second[1].c & 31);
    }
    if (!inputs[input.id]) {
        return std::nullopt;
    }
    auto pc = param_of(*inputs[input.id]);
    if (!pc || pc->first >= layout.size()) {
        return std::nullopt;
    }
    const RootParam& constant = layout[pc->first];
    if (constant.root != 0) {
        return std::nullopt;
    }
    return RootIndexedView{base.slot, constant.slot, pc->second, address.offset + std::uint32_t{d.source.first} * 4, shift};
}

Sum Tracer::sum(Term t) const {
    if (t.is_const) {
        return Sum{std::nullopt, t.c, std::nullopt};
    }
    Value v = t.v;
    Sum dynamic{std::nullopt, 0, v};
    if (inputs[v.id]) {
        auto pr = param_of(*inputs[v.id]);
        if (pr && pr->second == 0) {
            return Sum{v, 0, std::nullopt};
        }
        return dynamic;
    }
    if (getpc(v)) {
        return Sum{v, 0, std::nullopt};
    }
    if (auto pc = code_join(v)) {
        return Sum{*pc, 0, v};
    }
    if (auto lane = first_lane(v)) {
        Sum per_lane = sum(vorigin(*lane));
        return (per_lane.base || !read_first_lane) ? per_lane : dynamic;
    }
    std::optional<std::array<Term, 2>> ts;
    if (auto s = sop2(v, {"s_add_u32", "s_add_i32"})) {
        ts = s->second;
    } else {
        ts = vadd(v);
    }
    if (!ts) {
        return dynamic;
    }
    Sum x = sum((*ts)[0]), y = sum((*ts)[1]);
    if ((x.base && y.base) || (x.dynamic && y.dynamic)) {
        return dynamic;
    }
    return Sum{x.base ? x.base : y.base, x.offset + y.offset, x.dynamic ? x.dynamic : y.dynamic};
}

std::optional<Value> Tracer::code_join(Value v) const {
    if (!phis[v.id]) {
        return std::nullopt;
    }
    auto [b, i] = *phis[v.id];
    std::optional<Value> base;
    for (Value a : p.blocks[b].phis[i].args) {
        Term t = vorigin(a);
        if (t.is_const) {
            return std::nullopt;
        }
        if (t.v == v) {
            continue;
        }
        auto pc = code_base(t.v, 4);
        if (!pc) {
            return std::nullopt;
        }
        if (!base) {
            base = pc;
        }
    }
    return base;
}

bool Tracer::code_high_in(Term t, std::uint32_t depth) const {
    if (t.is_const) {
        return false;
    }
    Value x = t.v;
    if (const ir::Inst* in = def(x); in && in->name() == "s_getpc_b64" && in->writes.size() > 1 && in->writes[1].second == x) {
        return true;
    }
    if (depth == 0) {
        return false;
    }
    auto s = sop2(x, {"s_addc_u32"});
    return s && (code_high_in(s->second[0], depth - 1) || code_high_in(s->second[1], depth - 1));
}

std::optional<Value> Tracer::code_base(Value v, std::uint32_t depth) const {
    if (getpc(v)) {
        return v;
    }
    if (auto lane = first_lane(v)) {
        Term t = vorigin(*lane);
        return t.is_const ? std::nullopt : code_base(t.v, depth);
    }
    if (depth == 0 || phis[v.id]) {
        return std::nullopt;
    }
    std::optional<std::array<Term, 2>> ts;
    if (auto s = sop2(v, {"s_add_u32", "s_add_i32"})) {
        ts = s->second;
    } else {
        ts = vadd(v);
    }
    if (!ts) {
        return std::nullopt;
    }
    for (const Term& t : *ts) {
        if (!t.is_const) {
            if (auto r = code_base(t.v, depth - 1)) {
                return r;
            }
        }
    }
    return std::nullopt;
}

bool Tracer::is_high(Term t, Value high) const {
    if (t.is_const) {
        return false;
    }
    if (t.v == high) {
        return true;
    }
    auto s = sop2(t.v, {"s_addc_u32"});
    return s && (s->second[0] == Term::value(high) || s->second[1] == Term::value(high));
}

std::optional<Value> Tracer::first_lane(Value v) const {
    const ir::Inst* in = def(v);
    if (!in || in->name() != "v_readfirstlane_b32" || in->writes.empty() || in->writes[0].second != v) {
        return std::nullopt;
    }
    auto* f = std::get_if<Valu>(&in->fields);
    if (!f || f->abs != 0 || f->neg != 0 || f->sdwa || f->dpp || f->src.empty()) {
        return std::nullopt;
    }
    if (f->src[0].kind == Operand::Vector) {
        return in->read(Reg::v(f->src[0].a));
    }
    return std::nullopt;
}

const ir::Inst* Tracer::getpc(Value lo) const {
    const ir::Inst* in = def(lo);
    if (in && in->name() == "s_getpc_b64" && !in->writes.empty() && in->writes[0].second == lo) {
        return in;
    }
    return nullptr;
}

std::optional<std::array<Term, 2>> Tracer::vadd(Value v) const {
    const ir::Inst* in = def(v);
    if (!in) {
        return std::nullopt;
    }
    auto* f = std::get_if<Valu>(&in->fields);
    if (!f) {
        return std::nullopt;
    }
    auto name = in->name();
    if (!(name == "v_add_co_u32" || name == "v_add_nc_u32") || !f->dst || f->dst->kind != Operand::Vector) {
        return std::nullopt;
    }
    if (in->write(Reg::v(f->dst->a)) != std::optional(v) || f->abs != 0 || f->neg != 0 || f->sdwa || f->dpp) {
        return std::nullopt;
    }
    if (f->src.size() < 2) {
        return std::nullopt;
    }
    auto a = vterm(*in, f->src[0]);
    if (!a) {
        return std::nullopt;
    }
    auto b = vterm(*in, f->src[1]);
    if (!b) {
        return std::nullopt;
    }
    return std::array<Term, 2>{*a, *b};
}

std::optional<Term> Tracer::vterm(const ir::Inst& in, const Operand& op) const {
    if (op.kind == Operand::Vector) {
        auto v = in.read(Reg::v(op.a));
        return v ? std::optional(vorigin(*v)) : std::nullopt;
    }
    return term(in, op);
}

Term Tracer::vorigin(Value v) const {
    if (const ir::Inst* in = def(v); in && in->name() == "v_mov_b32") {
        if (auto* f = std::get_if<Valu>(&in->fields); f && f->abs == 0 && f->neg == 0 && !f->sdwa && !f->dpp && !f->src.empty()) {
            if (auto t = vterm(*in, f->src[0])) {
                return *t;
            }
        }
    }
    return origin(v);
}

Res<Address> Tracer::by_address(const char* why) const {
    if (stage == HwStage::Cs && std::find(std::begin(RAW), std::end(RAW), std::string_view(why)) != std::end(RAW)) {
        return Address{MEMORY, 0, std::nullopt};
    }
    return Fail{why};
}

Res<Address> Tracer::flat_address(const ir::Inst& in, const Opt& addr, const Opt& saddr, std::int16_t offset) const {
    if (!addr || addr->kind != Operand::Vector) {
        return Fail{"a global address not in VGPRs"};
    }
    std::uint8_t reg = addr->a;
    std::uint32_t off = static_cast<std::uint32_t>(static_cast<std::int32_t>(offset));
    if (saddr && saddr->kind == Operand::Scalar && saddr->a != NULL_CODE) {
        std::uint8_t code = saddr->a;
        auto r0 = in.read(Reg::s(code));
        auto r1 = in.read(Reg::s(static_cast<std::uint8_t>(code + 1)));
        if (!r0 || !r1) {
            return Fail{"an address not in SGPRs"};
        }
        Term lo = origin(*r0), hi = origin(*r1);
        auto v = in.read(Reg::v(reg));
        if (!v) {
            return Fail{"a global address"};
        }
        Sum extra = sum(vorigin(*v));
        if (extra.base) {
            return Fail{"an offset from a root parameter"};
        }
        extra.offset += off;
        return address(lo, hi, extra);
    }
    auto w0 = in.read(Reg::v(reg));
    auto w1 = in.read(Reg::v(static_cast<std::uint8_t>(reg + 1)));
    if (!w0 || !w1) {
        return Fail{"a global address"};
    }
    return address(vorigin(*w0), vorigin(*w1), Sum{std::nullopt, off, std::nullopt});
}

Res<Address> Tracer::address(Term lo, Term hi_in, Sum extra) const {
    Term hi = hi_in;
    Term l = live(hi_in);
    if (l.is_const) {
        hi = l;
    } else if (inputs[l.v.id]) {
        hi = l;
    }
    Sum s = sum(lo);
    if (s.dynamic && extra.dynamic) {
        return Fail{"an address with two computed terms"};
    }
    if (!s.base) {
        return Fail{why_no_base(s.dynamic)};
    }
    Value base = *s.base;
    if (const ir::Inst* pc = getpc(base)) {
        std::optional<Value> high = pc->writes.size() > 1 ? std::optional(pc->writes[1].second) : std::nullopt;
        if (!high_half(hi, high) && !(high && carried(hi, *high))) {
            return Fail{"an address with an untracked high half"};
        }
        bool joined = s.dynamic && code_join(*s.dynamic).has_value();
        std::uint32_t first = joined ? 0 : static_cast<std::uint32_t>(pc->machine.offset) + 4;
        return Address{CODE, first + s.offset + extra.offset, s.dynamic ? s.dynamic : extra.dynamic};
    }
    if (!inputs[base.id]) {
        return Fail{"an address not from a root parameter"};
    }
    auto pr = param_of(*inputs[base.id]);
    if (!pr) {
        return Fail{"an address not from a root parameter"};
    }
    std::size_t param = pr->first;
    std::optional<Value> high;
    switch (layout[param].root) {
        case 3:
        case 5:
        case 6: break;
        case 1: {
            Reg hreg = Reg::s(static_cast<std::uint8_t>(user_sgpr(stage, layout[param].user_data) + 1));
            auto it = std::find_if(p.inputs.begin(), p.inputs.end(), [&](const auto& i) { return i.first == hreg; });
            if (it == p.inputs.end()) {
                return Fail{"a root descriptor without its high half"};
            }
            high = it->second;
            break;
        }
        default: return Fail{"an address from a root parameter that is no address"};
    }
    std::optional<Value> dynamic = s.dynamic ? s.dynamic : extra.dynamic;
    if (!high_half(hi, high) && !(dynamic && high && carried(hi, *high))) {
        return Fail{"an address with an untracked high half"};
    }
    return Address{param, s.offset + extra.offset, s.dynamic ? s.dynamic : extra.dynamic};
}

bool Tracer::high_half(Term hi, std::optional<Value> high) const {
    Term want = high ? Term::value(*high) : Term::constant(0);
    if (hi == want) {
        return true;
    }
    if (hi.is_const) {
        return false;
    }
    auto s = sop2(hi.v, {"s_addc_u32"});
    if (!s) {
        return false;
    }
    const Term& a = s->second[0];
    const Term& b = s->second[1];
    return (a == want && b == Term::constant(0)) || (b == want && a == Term::constant(0));
}

bool Tracer::carried(Term hi, Value high) const {
    if (hi.is_const) {
        return false;
    }
    Value v = hi.v;
    if (v == high) {
        return true;
    }
    if (phis[v.id]) {
        auto [b, i] = *phis[v.id];
        auto high_of = [&](Term t) { return is_high(t, high) || code_high_in(t, 4); };
        for (Value a : p.blocks[b].phis[i].args) {
            Term t = vorigin(a);
            if (t == Term::value(v) || high_of(t)) {
                continue;
            }
            if (t.is_const) {
                return false;
            }
            Value x = t.v;
            if (auto lane = first_lane(x)) {
                Term y = vorigin(*lane);
                if (y.is_const) {
                    return false;
                }
                x = y.v;
            }
            const ir::Inst* in = def(x);
            if (!in) {
                return false;
            }
            auto* f = std::get_if<Valu>(&in->fields);
            if (!f || in->name() != "v_add_co_ci_u32") {
                return false;
            }
            bool any = false;
            for (std::size_t k = 0; k < std::min<std::size_t>(2, f->src.size()) && !any; ++k) {
                auto vt = vterm(*in, f->src[k]);
                any = vt && high_of(*vt);
            }
            if (!any) {
                return false;
            }
        }
        return true;
    }
    if (auto lane = first_lane(v)) {
        return carried(vorigin(*lane), high);
    }
    if (auto s = sop2(v, {"s_addc_u32"})) {
        return carried(s->second[0], high) || carried(s->second[1], high);
    }
    const ir::Inst* in = def(v);
    if (!in) {
        return false;
    }
    auto* f = std::get_if<Valu>(&in->fields);
    if (!f || in->name() != "v_add_co_ci_u32") {
        return false;
    }
    for (std::size_t k = 0; k < std::min<std::size_t>(2, f->src.size()); ++k) {
        auto vt = vterm(*in, f->src[k]);
        if (vt && carried(*vt, high)) {
            return true;
        }
    }
    return false;
}

const char* Tracer::why_no_base(std::optional<Value> dynamic) const {
    if (!dynamic) {
        return "a constant address";
    }
    Value v = *dynamic;
    if (inputs[v.id]) {
        return "an address in an SGPR that is no root parameter";
    }
    std::vector<Value> ls;
    leaves(v, ls);
    for (Value l : ls) {
        if (const ir::Inst* in = def(l); in && in->name() == "s_getpc_b64") {
            return "an address relative to the program counter";
        }
    }
    const ir::Inst* in = def(v);
    if (!in) {
        return "an address from a phi";
    }
    if (in->name().starts_with("s_load_")) {
        return "an address loaded from memory";
    }
    return "an address not from a root parameter";
}

void Tracer::leaves(Value v, std::vector<Value>& out) const {
    if (auto s = sop2(v, {"s_add_u32", "s_add_i32"})) {
        for (const Term& t : s->second) {
            if (!t.is_const) {
                leaves(t.v, out);
            }
        }
        return;
    }
    out.push_back(v);
}

Res<Address> Tracer::smem_address(const ir::Inst& in, const Operand& base, std::int32_t offset, const Operand& soffset) const {
    if (base.kind != Operand::Scalar) {
        return Fail{"an address not in SGPRs"};
    }
    auto r0 = in.read(Reg::s(base.a));
    auto r1 = in.read(Reg::s(static_cast<std::uint8_t>(base.a + 1)));
    if (!r0 || !r1) {
        return Fail{"an address not in SGPRs"};
    }
    Term lo = origin(*r0), hi = origin(*r1);
    auto t = term(in, soffset);
    if (!t) {
        return Fail{"an unknown offset operand"};
    }
    Sum extra = sum(*t);
    if (extra.base) {
        return Fail{"an offset from a root parameter"};
    }
    extra.offset += static_cast<std::uint32_t>(offset);
    return address(lo, hi, extra);
}

Res<Source> Tracer::descriptor(const ir::Inst& in, const Operand& op, std::size_t at_block) const {
    if (op.kind != Operand::Scalar) {
        return Fail{"a descriptor not in SGPRs"};
    }
    std::vector<Term> words;
    for (std::uint8_t k = 0; k < op.b; ++k) {
        auto v = in.read(Reg::s(static_cast<std::uint8_t>(op.a + k)));
        if (!v) {
            return Fail{"a descriptor not in SGPRs"};
        }
        words.push_back(origin(*v));
    }

    Known known;
    if (lanes) {
        known.emplace_back(lanes->reach[at_block], true);
    }
    std::vector<std::size_t> joins;
    return classify(words, joins, known);
}

Res<Source> Tracer::classify(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const {
    {
        TRY(ph, phi(words, joins, known));
        if (ph) {
            return *ph;
        }
    }
    {
        TRY(sel, select(words, joins, known));
        if (sel) {
            return *sel;
        }
    }
    auto read = [&](const Term& w) -> std::optional<Value> { return w.is_const ? std::nullopt : first_lane_source(w.v); };
    std::vector<Value> sources;
    bool all = true;
    for (const Term& w : words) {
        auto r = read(w);
        if (!r) {
            all = false;
            break;
        }
        sources.push_back(*r);
    }
    if (all) {
        if (auto s = lane_copies(words)) {
            return *s;
        }
        std::vector<Term> ls;
        for (Value v : sources) {
            ls.push_back(Term::value(v));
        }
        return per_lane(ls, joins, known);
    }
    for (const Term& w : words) {
        if (read(w)) {
            return Fail{"a descriptor partly read from the first lane"};
        }
    }
    {
        std::vector<std::uint32_t> consts;
        bool every = true;
        for (const Term& w : words) {
            if (!w.is_const) {
                every = false;
                break;
            }
            consts.push_back(w.c);
        }
        if (every) {
            Source s;
            s.kind = Source::Constant;
            s.consts = std::move(consts);
            return s;
        }
    }
    if (auto s = loaded(words)) {
        return *s;
    }
    if (words.size() == 4 && words[2].is_const && words[3].is_const) {
        Term hi = words[1];
        std::uint32_t bits = 0;
        while (!hi.is_const) {
            auto sb = set_bits(hi.v);
            if (!sb) {
                break;
            }
            hi = sb->first;
            bits |= sb->second;
        }
        TRY(a, address(words[0], hi, Sum{}));
        Source s;
        s.kind = Source::Built;
        s.address = a;
        s.bits = bits;
        s.words[0] = words[2].c;
        s.words[1] = words[3].c;
        return s;
    }
    for (const Term& w : words) {
        if (!w.is_const) {
            if (const ir::Inst* in = def(w.v); in && in->name().starts_with("s_load_")) {
                return Fail{"a descriptor changed after loading"};
            }
        }
    }
    return Fail{"a descriptor neither loaded nor built"};
}

std::optional<std::size_t> Tracer::only_edge(std::size_t block, Known& known) const {
    if (!lanes) {
        return std::nullopt;
    }
    const auto& preds = p.blocks[block].preds;
    if (preds.size() < 2) {
        return std::nullopt;
    }
    for (std::size_t q : preds) {
        if (rpo[q] >= rpo[block]) {
            return std::nullopt;
        }
    }
    std::optional<std::size_t> only;
    for (std::size_t j = 0; j < preds.size(); ++j) {
        std::size_t q = preds[j];
        std::size_t n = known.size();
        known.emplace_back(lanes->reach[q], true);
        const LaneExit& e = lanes->exits[q];
        if (e.kind == LaneExit::Branch && e.taken != e.not_taken) {
            known.emplace_back(e.when, e.taken == block);
        }
        bool possible = meet(*lanes, known);
        known.resize(n);
        if (possible) {
            if (only) {
                return std::nullopt;
            }
            only = j;
        }
    }
    return only;
}

bool Tracer::anywhere(const Source& source) const {
    switch (source.kind) {
        case Source::Constant: return true;
        case Source::Built: return !source.address.dynamic;
        case Source::Loaded: {
            const ir::Inst& in = inst(source.load);
            if (auto* f = std::get_if<SmemFields>(&in.fields)) {
                auto a = smem_address(in, f->base, f->offset, f->soffset);
                return a.ok() && !a.value.dynamic;
            }
            return false;
        }
        default: return false;
    }
}

Term Tracer::live(Term t) const {
    if (!at || !lanes) {
        return t;
    }
    Known known{{lanes->reach[*at], true}};
    for (std::size_t round = 0; round < MAX_JOINS; ++round) {
        if (t.is_const) {
            break;
        }
        if (!phis[t.v.id]) {
            break;
        }
        auto [block, i] = *phis[t.v.id];
        auto j = only_edge(block, known);
        if (!j) {
            break;
        }
        t = origin(p.blocks[block].phis[i].args[*j]);
    }
    return t;
}

Res<std::optional<Source>> Tracer::phi(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const {
    auto phi_of = [&](const Term& w) -> std::optional<std::pair<std::size_t, std::size_t>> { return w.is_const ? std::nullopt : phis[w.v.id]; };
    std::optional<std::size_t> found;
    for (const Term& w : words) {
        if (auto x = phi_of(w)) {
            found = x->first;
            break;
        }
    }
    if (!found) {
        return std::optional<Source>{};
    }
    std::size_t block = *found;
    for (const Term& w : words) {
        if (auto x = phi_of(w); x && x->first != block) {
            return Fail{"a descriptor joined in several blocks"};
        }
    }
    if (std::find(joins.begin(), joins.end(), block) != joins.end()) {
        return Fail{"a descriptor that changes in a loop"};
    }
    if (joins.size() >= MAX_JOINS) {
        return Fail{"a descriptor joined too often"};
    }
    joins.push_back(block);
    const auto& ph = p.blocks[block].phis;
    auto edge = [&](std::size_t j) {
        std::vector<Term> out;
        out.reserve(words.size());
        for (const Term& w : words) {
            auto x = phi_of(w);
            out.push_back(x ? origin(ph[x->second].args[j]) : w);
        }
        return out;
    };
    std::vector<Source> args;
    const char* err = nullptr;
    for (std::size_t j = 0; j < p.blocks[block].preds.size(); ++j) {
        auto r = classify(edge(j), joins, known);
        if (!r.ok()) {
            err = r.why;
            break;
        }
        args.push_back(std::move(r.value));
    }
    Res<std::optional<Source>> out;
    if (!err) {
        Source s;
        s.kind = Source::Phi;
        s.block = block;
        s.args = std::move(args);
        out = std::optional<Source>(std::move(s));
    } else if (auto j = only_edge(block, known)) {
        auto r = classify(edge(*j), joins, known);
        if (r.ok() && anywhere(r.value)) {
            out = std::optional<Source>(std::move(r.value));
        } else {
            out = Fail{err};
        }
    } else {
        out = Fail{err};
    }
    joins.pop_back();
    return out;
}

Res<std::optional<Source>> Tracer::select(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const {
    std::vector<std::optional<std::tuple<Value, Term, Term>>> picks;
    picks.reserve(words.size());
    for (const Term& w : words) {
        picks.push_back(w.is_const ? std::nullopt : picked(w.v));
    }
    std::optional<Value> scc;
    for (const auto& pk : picks) {
        if (pk) {
            scc = std::get<0>(*pk);
            break;
        }
    }
    if (!scc) {
        return std::optional<Source>{};
    }
    for (const auto& pk : picks) {
        if (pk && std::get<0>(*pk) != *scc) {
            return Fail{"a descriptor chosen by several conditions"};
        }
    }
    if (joins.size() >= MAX_JOINS) {
        return Fail{"a descriptor joined too often"};
    }
    joins.push_back(SIZE_MAX);
    auto side = [&](bool set) {
        std::vector<Term> out;
        for (std::size_t k = 0; k < words.size(); ++k) {
            out.push_back(picks[k] ? (set ? std::get<1>(*picks[k]) : std::get<2>(*picks[k])) : words[k]);
        }
        return out;
    };
    std::vector<Source> args;
    const char* err = nullptr;
    for (bool set : {true, false}) {
        auto r = classify(side(set), joins, known);
        if (!r.ok()) {
            err = r.why;
            break;
        }
        args.push_back(std::move(r.value));
    }
    joins.pop_back();
    if (err) {
        return Fail{err};
    }
    Source s;
    s.kind = Source::Select;
    s.value = *scc;
    s.args = std::move(args);
    return std::optional<Source>(std::move(s));
}

std::optional<std::tuple<Value, Term, Term>> Tracer::picked(Value v) const {
    const ir::Inst* in = def(v);
    if (!in) {
        return std::nullopt;
    }
    auto* f = std::get_if<SopFields>(&in->fields);
    if (!f) {
        return std::nullopt;
    }
    std::optional<std::size_t> kpos;
    for (std::size_t i = 0; i < in->writes.size(); ++i) {
        if (in->writes[i].second == v) {
            kpos = i;
            break;
        }
    }
    if (!kpos) {
        return std::nullopt;
    }
    std::uint8_t k = static_cast<std::uint8_t>(*kpos);
    auto name = in->name();
    bool wide;
    if (name == "s_cselect_b32" || name == "s_cmov_b32") {
        wide = false;
    } else if (name == "s_cselect_b64" || name == "s_cmov_b64") {
        wide = true;
    } else {
        return std::nullopt;
    }
    auto dword = [&](const Operand& op) -> std::optional<Term> {
        if (op.kind == Operand::Scalar && op.a == NULL_CODE) {
            return Term::constant(0);
        }
        if (op.kind == Operand::Scalar) {
            auto r = in->read(Reg::s(static_cast<std::uint8_t>(op.a + k)));
            return r ? std::optional(origin(*r)) : std::nullopt;
        }
        if (op.kind == Operand::Const && !wide) {
            auto c = inline_constant(op.a);
            return c ? std::optional(Term::constant(*c)) : std::nullopt;
        }
        if (op.kind == Operand::Const && op.a >= 128 && op.a <= 208) {
            auto low = inline_constant(op.a);
            if (!low) {
                return std::nullopt;
            }
            return Term::constant(k == 0 ? *low : static_cast<std::uint32_t>(static_cast<std::int32_t>(*low) >> 31));
        }
        if (op.kind == Operand::Literal && !wide) {
            return Term::constant(op.literal);
        }
        return std::nullopt;
    };
    if (f->src.empty()) {
        return std::nullopt;
    }
    auto set = dword(f->src[0]);
    if (!set) {
        return std::nullopt;
    }
    std::optional<Term> clear;
    if (name.starts_with("s_cmov")) {
        if (!f->dst) {
            return std::nullopt;
        }
        clear = dword(*f->dst);
    } else {
        if (f->src.size() < 2) {
            return std::nullopt;
        }
        clear = dword(f->src[1]);
    }
    if (!clear) {
        return std::nullopt;
    }
    auto scc = in->read(Reg::scc());
    if (!scc) {
        return std::nullopt;
    }
    return std::tuple{*scc, *set, *clear};
}

std::optional<std::pair<Term, std::uint32_t>> Tracer::set_bits(Value v) const {
    if (auto s = sop2(v, {"s_or_b32"})) {
        const Term& a = s->second[0];
        const Term& b = s->second[1];
        if (b.is_const) {
            return std::pair{a, b.c};
        }
        if (a.is_const) {
            return std::pair{b, a.c};
        }
    }
    const ir::Inst* in = def(v);
    if (!in) {
        return std::nullopt;
    }
    auto* f = std::get_if<SopFields>(&in->fields);
    if (!f || !f->dst || f->dst->kind != Operand::Scalar || f->src.empty()) {
        return std::nullopt;
    }
    auto t = term(*in, f->src[0]);
    if (!t) {
        return std::nullopt;
    }
    if (in->name() == "s_bitset1_b32" && t->is_const) {
        auto r = in->read(Reg::s(f->dst->a));
        if (!r) {
            return std::nullopt;
        }
        return std::pair{origin(*r), 1u << (t->c & 31)};
    }
    return std::nullopt;
}

std::optional<Value> Tracer::first_lane_source(Value v) const {
    const ir::Inst* in = def(v);
    if (!in) {
        return std::nullopt;
    }
    auto* f = std::get_if<Valu>(&in->fields);
    if (!f || in->name() != "v_readfirstlane_b32" || f->sdwa || f->dpp) {
        return std::nullopt;
    }
    if (f->src.empty() || f->src[0].kind != Operand::Vector) {
        return std::nullopt;
    }
    return in->read(Reg::v(f->src[0].a));
}

std::optional<Value> Tracer::first_lane_offset(Site load) const {
    const ir::Inst& in = inst(load);
    auto* f = std::get_if<SmemFields>(&in.fields);
    if (!f) {
        return std::nullopt;
    }
    read_first_lane = true;
    auto a = smem_address(in, f->base, f->offset, f->soffset);
    read_first_lane = false;
    if (!a.ok() || !a.value.dynamic) {
        return std::nullopt;
    }
    Term t = origin(*a.value.dynamic);
    if (!t.is_const && first_lane_source(t.v)) {
        return t.v;
    }
    return std::nullopt;
}

bool Tracer::kept_per_lane(const ir::Inst& in, const Operand& op) const {
    if (op.kind != Operand::Scalar) {
        return false;
    }
    std::vector<Term> words;
    for (std::uint8_t k = 0; k < op.b; ++k) {
        auto r = in.read(Reg::s(static_cast<std::uint8_t>(op.a + k)));
        if (!r) {
            return false;
        }
        words.push_back(origin(*r));
    }
    return lane_copies(words).has_value();
}

std::optional<std::vector<Value>> Tracer::first_lane_words(const ir::Inst& in, const Operand& op) const {
    if (op.kind != Operand::Scalar) {
        return std::nullopt;
    }
    std::vector<Value> out;
    for (std::uint8_t k = 0; k < op.b; ++k) {
        auto r = in.read(Reg::s(static_cast<std::uint8_t>(op.a + k)));
        if (!r) {
            return std::nullopt;
        }
        Term t = origin(*r);
        if (t.is_const || !first_lane_source(t.v)) {
            return std::nullopt;
        }
        out.push_back(t.v);
    }
    return out;
}

Term Tracer::lane_term(Term t) const {
    for (std::size_t i = 0; i < MAX_JOINS * 2; ++i) {
        if (t.is_const) {
            break;
        }
        Term next = vorigin(t.v);
        if (next == t) {
            break;
        }
        t = next;
    }
    return t;
}

bool Tracer::varies(Term t) const {
    if (t.is_const) {
        return false;
    }
    Value v = t.v;
    std::optional<Reg> reg;
    if (inputs[v.id]) {
        reg = inputs[v.id];
    } else if (phis[v.id]) {
        reg = p.blocks[phis[v.id]->first].phis[phis[v.id]->second].reg;
    } else if (const ir::Inst* in = def(v)) {
        for (const auto& w : in->writes) {
            if (w.second == v) {
                reg = w.first;
                break;
            }
        }
    }
    return reg && reg->kind == Reg::V;
}

Res<Term> Tracer::lane_value(Term t, const Known& known) const {
    for (std::size_t i = 0; i < MAX_JOINS * 2; ++i) {
        if (t.is_const) {
            break;
        }
        Value v = t.v;
        Term next = vstep(v);
        if (next == t) {
            break;
        }
        if (varies(t) && defs[v.id]) {
            if (Status s = runs(*defs[v.id], known)) {
                return Fail{s};
            }
        }
        t = next;
    }
    return t;
}

Term Tracer::vstep(Value v) const {
    if (const ir::Inst* in = def(v); in && in->name() == "v_mov_b32") {
        if (auto* f = std::get_if<Valu>(&in->fields); f && f->abs == 0 && f->neg == 0 && !f->sdwa && !f->dpp && !f->src.empty()) {
            if (auto t = raw(*in, f->src[0])) {
                return *t;
            }
        }
    }
    return origin(v);
}

std::optional<Term> Tracer::raw(const ir::Inst& in, const Operand& op) const {
    if (op.kind == Operand::Vector) {
        auto v = in.read(Reg::v(op.a));
        return v ? std::optional(Term::value(*v)) : std::nullopt;
    }
    return term(in, op);
}

Status Tracer::runs(Site site, const Known& known) const {
    if (!lanes) {
        return nullptr;
    }
    if (site.first >= lanes->guards.size() || site.second >= lanes->guards[site.first].size()) {
        return nullptr;
    }
    const auto& g = lanes->guards[site.first][site.second];
    if (!g) {
        return nullptr;
    }
    Known skips = known;
    skips.emplace_back(lanes->reach[site.first], true);
    skips.emplace_back(*g, false);
    if (meet(*lanes, skips)) {
        return "a per-lane descriptor written where some lanes may keep the old one";
    }
    return nullptr;
}

Res<Source> Tracer::per_lane(const std::vector<Term>& in_words, std::vector<std::size_t>& joins, Known& known) const {
    std::vector<Term> words;
    words.reserve(in_words.size());
    for (const Term& w : in_words) {
        TRY(x, lane_value(w, known));
        words.push_back(x);
    }
    std::optional<Term> lane;
    for (const Term& w : words) {
        if (varies(w)) {
            lane = w;
            break;
        }
    }
    if (!lane) {
        return classify(words, joins, known);
    }
    {
        TRY(f, fetched(words, known));
        if (f) {
            return *f;
        }
    }
    {
        TRY(s, lane_select(words, joins, known));
        if (s) {
            return *s;
        }
    }
    {
        TRY(ph, lane_phi(words, joins, known));
        if (ph) {
            return *ph;
        }
    }
    Value v = lane->v;
    const ir::Inst* in = def(v);
    if (inputs[v.id]) {
        return Fail{"a descriptor read from the first lane of an input VGPR"};
    }
    if (!in) {
        return Fail{"a descriptor read from the first lane of a phi"};
    }
    auto n = in->name();
    if (n.starts_with("ds_read")) {
        return Fail{"a descriptor read from the first lane of an LDS read"};
    }
    if (n.starts_with("buffer_load")) {
        return Fail{"a descriptor read from the first lane of a buffer load"};
    }
    if (n.starts_with("global_load") || n.starts_with("flat_load")) {
        return Fail{"a descriptor read from the first lane of a partial or mixed load"};
    }
    return Fail{"a descriptor read from the first lane of computed VGPRs"};
}

Res<std::optional<Source>> Tracer::fetched(const std::vector<Term>& words, const Known& known) const {
    std::vector<std::pair<Site, std::size_t>> sites;
    for (const Term& w : words) {
        if (w.is_const || !defs[w.v.id]) {
            return std::optional<Source>{};
        }
        Site site = *defs[w.v.id];
        const ir::Inst& in = inst(site);
        if (!in.name().starts_with("global_load_dword")) {
            return std::optional<Source>{};
        }
        std::optional<std::size_t> k;
        std::size_t idx = 0;
        for (const auto& wr : in.writes) {
            if (wr.first.kind != Reg::V) {
                continue;
            }
            if (wr.second == w.v) {
                k = idx;
                break;
            }
            ++idx;
        }
        if (!k) {
            return std::optional<Source>{};
        }
        sites.emplace_back(site, *k);
    }
    auto addr_of = [&](Site site) -> Res<Address> {
        const ir::Inst& in = inst(site);
        auto* f = std::get_if<FlatFields>(&in.fields);
        if (!f) {
            return Fail{"a global load"};
        }
        return flat_address(in, f->addr, f->saddr, f->offset);
    };
    auto [load, first] = sites[0];
    TRY(a0, addr_of(load));
    for (std::size_t k = 0; k < sites.size(); ++k) {
        auto [site, dword] = sites[k];
        if (Status s = runs(site, known)) {
            return Fail{s};
        }
        TRY(a, addr_of(site));
        std::uint32_t want = a0.offset + static_cast<std::uint32_t>(4 * (first + k));
        if (a.param != a0.param || a.dynamic != a0.dynamic || a.offset + static_cast<std::uint32_t>(4 * dword) != want) {
            return Fail{"a descriptor read from the first lane of loads that do not line up"};
        }
    }
    Source s;
    s.kind = Source::Fetched;
    s.load = load;
    s.first = static_cast<std::uint8_t>(first);
    return std::optional<Source>(std::move(s));
}

Res<std::optional<Source>> Tracer::lane_select(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const {
    std::vector<std::optional<std::tuple<Value, Term, Term>>> picks;
    for (const Term& w : words) {
        picks.push_back(w.is_const ? std::nullopt : lane_picked(w.v));
    }
    std::optional<Value> mask;
    for (const auto& pk : picks) {
        if (pk) {
            mask = std::get<0>(*pk);
            break;
        }
    }
    if (!mask) {
        return std::optional<Source>{};
    }
    for (const auto& pk : picks) {
        if (pk && std::get<0>(*pk) != *mask) {
            return Fail{"a descriptor chosen per lane by several masks"};
        }
    }
    for (std::size_t k = 0; k < words.size(); ++k) {
        if (!words[k].is_const && picks[k] && defs[words[k].v.id]) {
            if (Status s = runs(*defs[words[k].v.id], known)) {
                return Fail{s};
            }
        }
    }
    if (joins.size() >= MAX_JOINS) {
        return Fail{"a descriptor joined too often"};
    }
    joins.push_back(SIZE_MAX);
    std::optional<Bool> bit;
    if (lanes) {
        if (auto it = lanes->masks.find(*mask); it != lanes->masks.end()) {
            bit = it->second;
        }
    }
    auto side = [&](bool set) {
        std::vector<Term> out;
        for (std::size_t k = 0; k < words.size(); ++k) {
            out.push_back(picks[k] ? (set ? std::get<1>(*picks[k]) : std::get<2>(*picks[k])) : words[k]);
        }
        return out;
    };
    std::vector<Source> args;
    const char* err = nullptr;
    for (bool set : {true, false}) {
        std::size_t n = known.size();
        if (bit) {
            known.emplace_back(*bit, set);
        }
        auto r = per_lane(side(set), joins, known);
        known.resize(n);
        if (!r.ok()) {
            err = r.why;
            break;
        }
        args.push_back(std::move(r.value));
    }
    joins.pop_back();
    if (err) {
        return Fail{err};
    }
    Source s;
    s.kind = Source::Lanes;
    s.value = *mask;
    s.args = std::move(args);
    return std::optional<Source>(std::move(s));
}

std::optional<std::tuple<Value, Term, Term>> Tracer::lane_picked(Value v) const {
    const ir::Inst* in = def(v);
    if (!in) {
        return std::nullopt;
    }
    auto* f = std::get_if<Valu>(&in->fields);
    if (!f || !f->dst || f->dst->kind != Operand::Vector) {
        return std::nullopt;
    }
    if (in->name() != "v_cndmask_b32" || in->write(Reg::v(f->dst->a)) != std::optional(v) || f->abs != 0 || f->neg != 0 || f->clamp ||
        f->omod != 0 || f->sdwa || f->dpp) {
        return std::nullopt;
    }
    if (f->src.size() != 3 || f->src[2].kind != Operand::Scalar) {
        return std::nullopt;
    }
    auto m = in->read(Reg::s(f->src[2].a));
    if (!m) {
        return std::nullopt;
    }
    auto set = raw(*in, f->src[1]);
    if (!set) {
        return std::nullopt;
    }
    auto clear = raw(*in, f->src[0]);
    if (!clear) {
        return std::nullopt;
    }
    return std::tuple{*m, *set, *clear};
}

Res<std::optional<Source>> Tracer::lane_phi(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const {
    auto phi_of = [&](const Term& w) -> std::optional<std::pair<std::size_t, std::size_t>> { return w.is_const ? std::nullopt : phis[w.v.id]; };
    std::optional<std::size_t> found;
    for (const Term& w : words) {
        if (auto x = phi_of(w)) {
            found = x->first;
            break;
        }
    }
    if (!found) {
        return std::optional<Source>{};
    }
    std::size_t block = *found;
    for (const Term& w : words) {
        if (auto x = phi_of(w); x && x->first != block) {
            return Fail{"a descriptor joined in several blocks"};
        }
    }
    if (std::find(joins.begin(), joins.end(), block) != joins.end()) {
        return Fail{"a descriptor that changes in a loop"};
    }
    const auto& preds = p.blocks[block].preds;
    for (std::size_t q : preds) {
        if (rpo[q] >= rpo[block]) {
            return Fail{"a descriptor read from the first lane of a phi at a loop header"};
        }
    }
    if (!lanes) {
        return Fail{"a descriptor read from the first lane of a phi, without the lanes' paths"};
    }
    for (const Term& w : words) {
        if (!w.is_const && !phi_of(w) && defs[w.v.id]) {
            std::size_t d = defs[w.v.id]->first;
            if (d == block || reaches(block, d)) {
                return Fail{"a descriptor partly joined by control flow"};
            }
        }
    }
    if (joins.size() >= MAX_JOINS) {
        return Fail{"a descriptor joined too often"};
    }
    joins.push_back(block);
    const auto& ph = p.blocks[block].phis;
    Source s;
    s.kind = Source::LanePhi;
    s.block = block;
    for (std::size_t j = 0; j < preds.size(); ++j) {
        std::size_t q = preds[j];
        std::size_t n = known.size();
        known.emplace_back(lanes->reach[q], true);
        const LaneExit& e = lanes->exits[q];
        if (e.kind == LaneExit::Branch && e.taken != e.not_taken) {
            known.emplace_back(e.when, e.taken == block);
        }
        Res<Source> arg;
        bool present = false;
        if (meet(*lanes, known)) {
            std::vector<Term> edge;
            for (const Term& w : words) {
                auto x = phi_of(w);
                edge.push_back(x ? Term::value(ph[x->second].args[j]) : w);
            }
            arg = per_lane(edge, joins, known);
            present = true;
        }
        known.resize(n);
        if (!arg.ok()) {
            joins.pop_back();
            return Fail{arg.why};
        }
        s.args.push_back(present ? std::move(arg.value) : Source{});
        s.present.push_back(present);
    }
    joins.pop_back();
    return std::optional<Source>(std::move(s));
}

bool Tracer::reaches(std::size_t from, std::size_t to) const {
    std::vector<bool> seen(p.blocks.size(), false);
    std::vector<std::size_t> stack = p.blocks[from].exit.successor_list();
    while (!stack.empty()) {
        std::size_t x = stack.back();
        stack.pop_back();
        if (x == to) {
            return true;
        }
        if (seen[x]) {
            continue;
        }
        seen[x] = true;
        for (std::size_t s : p.blocks[x].exit.successor_list()) {
            stack.push_back(s);
        }
    }
    return false;
}

Res<Waterfall> Tracer::waterfall(std::size_t b, const std::vector<std::size_t>& accesses, const std::vector<Value>& reads) const {
    static constexpr const char* LOOP = "a descriptor read from the first lane outside a waterfall loop";
    static constexpr const char* SPLIT = "a waterfall loop nested in another or split over blocks";
    static constexpr const char* SELECT = "a waterfall loop whose compares do not select the access's lanes";
    static constexpr const char* RETRY = "a waterfall loop whose retry does not keep the lanes still waiting";
    std::size_t access = accesses[0];
    const ir::Block& block = p.blocks[b];
    if (!lanes) {
        return Fail{LOOP};
    }
    const structure::Structured& l = *lanes;
    if (block.insts.empty()) {
        return Fail{LOOP};
    }
    const ir::Inst& exit = block.insts.back();
    if (!(block.exit.kind == ir::Exit::Branch && block.exit.a == b && block.exit.b != b) || exit.name() != "s_cbranch_execnz") {
        return Fail{LOOP};
    }
    auto mask = [&](std::optional<Value> low, std::optional<Value> high) -> std::optional<Bool> {
        std::optional<Value> h;
        if (p.wave64) {
            if (!high) {
                return std::nullopt;
            }
            h = high;
        }
        if (!low) {
            return std::nullopt;
        }
        return l.mask_bits(*low, h);
    };
    auto exec = [&](const ir::Inst& in) { return mask(in.read(Reg::s(ir::EXEC)), in.read(Reg::s(ir::EXEC + 1))); };
    for (Value v : reads) {
        if (!defs[v.id] || defs[v.id]->first != b) {
            return Fail{SPLIT};
        }
    }
    auto selected_o = exec(block.insts[access]);
    if (!selected_o) {
        return Fail{SELECT};
    }
    Bool selected = *selected_o;
    for (std::size_t i : accesses) {
        if (exec(block.insts[i]) != std::optional(selected)) {
            return Fail{SELECT};
        }
    }
    auto retry_o = exec(exit);
    if (!retry_o) {
        return Fail{RETRY};
    }
    Bool retry = *retry_o;
    if (!defs[reads[0].id]) {
        return Fail{LOOP};
    }
    Site first = *defs[reads[0].id];
    auto remaining_o = exec(inst(first));
    if (!remaining_o) {
        return Fail{SELECT};
    }
    Bool remaining = *remaining_o;
    for (Value v : reads) {
        if (!defs[v.id] || defs[v.id]->second >= access || exec(inst(*defs[v.id])) != std::optional(remaining)) {
            return Fail{SELECT};
        }
    }
    std::pair<Bool, bool> reach{l.reach[b], true};
    auto meet_ = [&](const Known& c) { return meet(l, c); };
    Waterfall out;
    Known required{{remaining, true}};
    for (std::size_t i = 0; i < access; ++i) {
        const ir::Inst& in = block.insts[i];
        auto* f = std::get_if<Valu>(&in.fields);
        if (!f) {
            continue;
        }
        auto name = in.name();
        std::string_view ty;
        if (name.starts_with("v_cmp_eq_")) {
            ty = name.substr(9);
        } else if (name.starts_with("v_cmpx_eq_")) {
            ty = name.substr(10);
        } else {
            continue;
        }
        std::uint8_t dwords;
        if (ty == "u32" || ty == "i32") {
            dwords = 1;
        } else if (ty == "u64" || ty == "i64") {
            dwords = 2;
        } else {
            continue;
        }
        if (f->abs != 0 || f->neg != 0 || f->sdwa || f->dpp) {
            continue;
        }
        auto pair = [&](const Operand& fst, const Operand& own) -> std::optional<std::vector<Value>> {
            if (fst.kind != Operand::Scalar || own.kind != Operand::Vector) {
                return std::nullopt;
            }
            std::vector<Value> xs;
            for (std::uint8_t k = 0; k < dwords; ++k) {
                auto r = in.read(Reg::s(static_cast<std::uint8_t>(fst.a + k)));
                if (!r) {
                    return std::nullopt;
                }
                Term t = origin(*r);
                if (t.is_const) {
                    return std::nullopt;
                }
                Value x = t.v;
                auto lane = in.read(Reg::v(static_cast<std::uint8_t>(own.a + k)));
                if (!lane || !defs[x.id]) {
                    return std::nullopt;
                }
                std::size_t read = defs[x.id]->second;
                auto fls = first_lane_source(x);
                if (!fls) {
                    return std::nullopt;
                }
                bool same = std::find(reads.begin(), reads.end(), x) != reads.end() && read < i &&
                            lane_term(Term::value(*fls)) == lane_term(Term::value(*lane));
                if (!same) {
                    return std::nullopt;
                }
                xs.push_back(x);
            }
            return xs;
        };
        if (f->src.size() < 2) {
            continue;
        }
        auto xs = pair(f->src[0], f->src[1]);
        if (!xs) {
            xs = pair(f->src[1], f->src[0]);
        }
        if (!xs) {
            continue;
        }
        std::uint8_t code;
        if (name.starts_with("v_cmpx_")) {
            code = ir::EXEC;
        } else if (f->dst && f->dst->kind == Operand::Scalar) {
            code = f->dst->a;
        } else {
            continue;
        }
        auto matched = mask(in.write(Reg::s(code)), in.write(Reg::s(static_cast<std::uint8_t>(code + 1))));
        if (!matched) {
            continue;
        }
        if (meet_({reach, {selected, true}, {*matched, false}})) {
            continue;
        }
        required.emplace_back(*matched, true);
        out.compares.emplace_back(b, i);
        for (Value x : *xs) {
            if (std::find(out.reads.begin(), out.reads.end(), x) == out.reads.end()) {
                out.reads.push_back(x);
            }
        }
    }
    for (Value v : reads) {
        if (std::find(out.reads.begin(), out.reads.end(), v) == out.reads.end()) {
            return Fail{SELECT};
        }
    }
    if (meet_({reach, {selected, true}, {remaining, false}})) {
        return Fail{SELECT};
    }
    required.push_back(reach);
    required.emplace_back(selected, false);
    if (meet_(required)) {
        return Fail{SELECT};
    }
    if (meet_({reach, {retry, true}, {remaining, false}}) || meet_({reach, {retry, true}, {selected, true}}) ||
        meet_({reach, {remaining, true}, {selected, false}, {retry, false}})) {
        return Fail{RETRY};
    }
    return out;
}

std::optional<Source> Tracer::loaded(const std::vector<Term>& words) const {
    if (words.empty() || words[0].is_const) {
        return std::nullopt;
    }
    Value v0 = words[0].v;
    if (!defs[v0.id]) {
        return std::nullopt;
    }
    Site load = *defs[v0.id];
    const ir::Inst& in = inst(load);
    if (!in.name().starts_with("s_load_")) {
        return std::nullopt;
    }
    std::optional<std::size_t> first;
    for (std::size_t i = 0; i < in.writes.size(); ++i) {
        if (in.writes[i].second == v0) {
            first = i;
            break;
        }
    }
    if (!first) {
        return std::nullopt;
    }
    for (std::size_t k = 0; k < words.size(); ++k) {
        if (*first + k >= in.writes.size() || !(words[k] == Term::value(in.writes[*first + k].second))) {
            return std::nullopt;
        }
    }
    Source s;
    s.kind = Source::Loaded;
    s.load = load;
    s.first = static_cast<std::uint8_t>(*first);
    return s;
}

std::optional<Source> Tracer::lane_copies(const std::vector<Term>& words) const {
    std::vector<Site> loads;
    std::optional<std::size_t> first;
    for (std::size_t k = 0; k < words.size(); ++k) {
        const Term& w = words[k];
        if (w.is_const) {
            return std::nullopt;
        }
        auto fl = first_lane(w.v);
        if (!fl) {
            return std::nullopt;
        }
        std::vector<Term> copies;
        if (!scalar_copies(*fl, copies)) {
            return std::nullopt;
        }
        bool found = false;
        for (const Term& c : copies) {
            if (c.is_const || !defs[c.v.id]) {
                continue;
            }
            Site site = *defs[c.v.id];
            const ir::Inst& load = inst(site);
            if (!load.name().starts_with("s_load_")) {
                continue;
            }
            std::optional<std::size_t> dword;
            for (std::size_t x = 0; x < load.writes.size(); ++x) {
                if (load.writes[x].second == c.v) {
                    dword = x;
                    break;
                }
            }
            if (!dword || *dword < k) {
                return std::nullopt;
            }
            std::size_t f = *dword - k;
            if (!first) {
                first = f;
            }
            if (*first != f) {
                return std::nullopt;
            }
            if (std::find(loads.begin(), loads.end(), site) == loads.end()) {
                loads.push_back(site);
            }
            found = true;
        }
        if (!found) {
            return std::nullopt;
        }
    }
    auto addr = [&](Site site) -> std::optional<Address> {
        const ir::Inst& in = inst(site);
        if (auto* f = std::get_if<SmemFields>(&in.fields)) {
            auto a = smem_address(in, f->base, f->offset, f->soffset);
            if (a.ok()) {
                return a.value;
            }
        }
        return std::nullopt;
    };
    if (loads.empty()) {
        return std::nullopt;
    }
    auto a = addr(loads[0]);
    if (!a || a->dynamic) {
        return std::nullopt;
    }
    for (const Site& l : loads) {
        if (addr(l) != a) {
            return std::nullopt;
        }
    }
    std::sort(loads.begin(), loads.end());
    std::optional<Site> load;
    for (const Site& l : loads) {
        if (base_from_entry(l)) {
            load = l;
            break;
        }
    }
    if (!load) {
        return std::nullopt;
    }
    lane_copied.insert(lane_copied.end(), loads.begin(), loads.end());
    if (!first) {
        return std::nullopt;
    }
    Source s;
    s.kind = Source::Loaded;
    s.load = *load;
    s.first = static_cast<std::uint8_t>(*first);
    return s;
}

bool Tracer::scalar_copies(Value start, std::vector<Term>& out) const {
    std::vector<Value> seen, work{start};
    while (!work.empty()) {
        Value v = work.back();
        work.pop_back();
        if (std::find(seen.begin(), seen.end(), v) != seen.end()) {
            continue;
        }
        if (seen.size() == MAX_COPIES) {
            return false;
        }
        seen.push_back(v);
        if (phis[v.id]) {
            const auto& args = p.blocks[phis[v.id]->first].phis[phis[v.id]->second].args;
            work.insert(work.end(), args.begin(), args.end());
            continue;
        }
        const ir::Inst* in = def(v);
        if (!in) {
            continue;
        }
        if (in->name().starts_with("global_load_")) {
            return false;
        }
        auto* f = std::get_if<Valu>(&in->fields);
        if (!f) {
            continue;
        }
        std::size_t sources;
        if (in->name() == "v_mov_b32") {
            sources = 1;
        } else if (in->name() == "v_cndmask_b32") {
            sources = 2;
        } else {
            continue;
        }
        if (f->abs != 0 || f->neg != 0 || f->sdwa || f->dpp) {
            continue;
        }
        for (std::size_t k = 0; k < std::min(sources, f->src.size()); ++k) {
            const Operand& op = f->src[k];
            if (op.kind == Operand::Vector) {
                if (auto r = in->read(Reg::v(op.a))) {
                    work.push_back(*r);
                }
            } else if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
                if (auto r = in->read(Reg::s(op.a))) {
                    out.push_back(origin(*r));
                }
            }
        }
        if (f->dst && f->dst->kind == Operand::Vector) {
            if (auto r = in->read(Reg::v(f->dst->a))) {
                work.push_back(*r);
            }
        }
    }
    return true;
}

bool Tracer::base_from_entry(Site site) const {
    const ir::Inst& in = inst(site);
    auto* f = std::get_if<SmemFields>(&in.fields);
    if (!f || f->base.kind != Operand::Scalar || f->soffset.kind != Operand::Scalar || f->soffset.a != NULL_CODE) {
        return false;
    }
    auto v0 = in.read(Reg::s(f->base.a));
    if (!v0) {
        return false;
    }
    Value v = *v0;
    for (;;) {
        if (inputs[v.id]) {
            return true;
        }
        const ir::Inst* d = def(v);
        if (!d) {
            return false;
        }
        std::optional<std::size_t> k;
        for (std::size_t i = 0; i < d->writes.size(); ++i) {
            if (d->writes[i].second == v) {
                k = i;
                break;
            }
        }
        if (!k) {
            return false;
        }
        auto m = copy_source(*d, *k);
        if (!m || m->is_const) {
            return false;
        }
        v = m->v;
    }
}

std::optional<Moved> Tracer::restored(const ir::Inst& in) const {
    auto* v = std::get_if<Valu>(&in.fields);
    if (!v || in.name() != "v_readlane_b32") {
        return std::nullopt;
    }
    if (v->src.size() != 2 || v->src[0].kind != Operand::Vector) {
        return std::nullopt;
    }
    auto r = in.read(Reg::v(v->src[0].a));
    auto lane = lane_of(v->src[1]);
    if (!r || !lane) {
        return std::nullopt;
    }
    std::vector<Value> seen;
    bool ok = true;
    auto m = spilled(*r, *lane, seen, ok);
    if (!ok) {
        return std::nullopt;
    }
    return m;
}

std::optional<Moved> Tracer::spilled(Value v, std::uint32_t lane, std::vector<Value>& seen, bool& ok) const {
    if (phis[v.id]) {
        if (std::find(seen.begin(), seen.end(), v) != seen.end()) {
            return std::nullopt;
        }
        seen.push_back(v);
        std::optional<Moved> found;
        for (Value a : p.blocks[phis[v.id]->first].phis[phis[v.id]->second].args) {
            auto m = spilled(a, lane, seen, ok);
            if (!ok) {
                return std::nullopt;
            }
            if (m) {
                if (found && !(*found == *m)) {
                    ok = false;
                    return std::nullopt;
                }
                found = m;
            }
        }
        return found;
    }
    const ir::Inst* in = def(v);
    if (!in) {
        ok = false;
        return std::nullopt;
    }
    auto* w = std::get_if<Valu>(&in->fields);
    if (!w || !w->dst || w->dst->kind != Operand::Vector || w->src.size() != 2) {
        ok = false;
        return std::nullopt;
    }
    if (in->name() != "v_writelane_b32") {
        ok = false;
        return std::nullopt;
    }
    auto at_lane = lane_of(w->src[1]);
    if (!at_lane) {
        ok = false;
        return std::nullopt;
    }
    if (*at_lane != lane) {
        auto r = in->read(Reg::v(w->dst->a));
        if (!r) {
            ok = false;
            return std::nullopt;
        }
        return spilled(*r, lane, seen, ok);
    }
    const Operand& value = w->src[0];
    std::optional<Moved> m;
    if (value.kind == Operand::Scalar && value.a != NULL_CODE) {
        if (auto r = in->read(Reg::s(value.a))) {
            m = Moved{false, 0, *r};
        }
    } else if (value.kind == Operand::Literal) {
        m = Moved{true, value.literal, {}};
    } else if (value.kind == Operand::Const) {
        if (auto c = inline_constant(value.a)) {
            m = Moved{true, *c, {}};
        }
    }
    if (!m) {
        ok = false;
        return std::nullopt;
    }
    return m;
}

}   

std::optional<std::uint32_t> inline_constant(std::uint8_t code) {
    auto fl = [](float f) { return std::bit_cast<std::uint32_t>(f); };
    if (code >= 128 && code <= 192) {
        return std::uint32_t{code} - 128;
    }
    if (code >= 193 && code <= 208) {
        return static_cast<std::uint32_t>(192 - std::int32_t{code});
    }
    switch (code) {
        case 240: return fl(0.5f);
        case 241: return fl(-0.5f);
        case 242: return fl(1.0f);
        case 243: return fl(-1.0f);
        case 244: return fl(2.0f);
        case 245: return fl(-2.0f);
        case 246: return fl(4.0f);
        case 247: return fl(-4.0f);
        case 248: return 0x3E22F983u;
        default: return std::nullopt;
    }
}

std::vector<const Source*> Source::leaves() const {
    if (kind == Phi || kind == Select || kind == Lanes) {
        std::vector<const Source*> out;
        for (const Source& a : args) {
            auto l = a.leaves();
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    }
    if (kind == LanePhi) {
        std::vector<const Source*> out;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (present[i]) {
                auto l = args[i].leaves();
                out.insert(out.end(), l.begin(), l.end());
            }
        }
        return out;
    }
    return {this};
}

bool Source::operator==(const Source& o) const {
    if (kind != o.kind) {
        return false;
    }
    switch (kind) {
        case Loaded:
        case Fetched: return load == o.load && first == o.first;
        case Built: return address == o.address && bits == o.bits && words[0] == o.words[0] && words[1] == o.words[1];
        case Constant: return consts == o.consts;
        case Phi: return block == o.block && args == o.args;
        case Select:
        case Lanes: return value == o.value && args == o.args;
        case LanePhi:
            if (block != o.block || present != o.present || args.size() != o.args.size()) {
                return false;
            }
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (present[i] && !(args[i] == o.args[i])) {
                    return false;
                }
            }
            return true;
    }
    return false;
}

Resources track(const ir::Program& p, const std::vector<RootParam>& layout, HwStage stage, const structure::Structured* lanes) {
    Tracer t(p, layout, stage, lanes);
    Resources out;
    for (const auto& [reg, v] : p.inputs) {
        if (auto pr = t.param_of(reg)) {
            out.params.emplace_back(v, pr->first, pr->second);
        }
    }
    std::map<std::size_t, std::pair<std::vector<std::size_t>, std::vector<Value>>> first_lane_at;
    std::map<std::size_t, std::tuple<std::vector<std::size_t>, std::vector<Value>, std::vector<Site>>> offset_at;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        t.at = b;
        const auto& block = p.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const ir::Inst& inst = block.insts[i];
            Site site{b, i};
            auto fail = [&](const char* what) { unsupported(inst.machine.offset, what); };
            std::vector<Value> first_lane;
            std::vector<std::pair<Value, Site>> offset;
            auto descriptor = [&](const Operand& op, Role role) {
                auto source = t.descriptor(inst, op, b);
                if (!source.ok()) {
                    fail(source.why);
                }
                if (!t.kept_per_lane(inst, op)) {
                    if (auto w = t.first_lane_words(inst, op)) {
                        first_lane.insert(first_lane.end(), w->begin(), w->end());
                    }
                }
                if (source.value.kind == Source::Loaded) {
                    Site load = source.value.load;
                    if (std::any_of(out.scalar.begin(), out.scalar.end(), [&](const auto& s) { return s.first == load; })) {
                        if (auto v = t.first_lane_offset(load)) {
                            offset.emplace_back(*v, load);
                        }
                    }
                }
                out.descriptors.push_back(Descriptor{site, role, std::move(source.value)});
            };
            auto name = inst.name();
            if (auto* f = std::get_if<ImageFields>(&inst.fields)) {
                if (name.starts_with("image_bvh")) {
                    auto word = [&](std::uint8_t k) -> std::optional<Term> {
                        if (f->rsrc.kind == Operand::Scalar && k < f->rsrc.b) {
                            auto r = inst.read(Reg::s(static_cast<std::uint8_t>(f->rsrc.a + k)));
                            return r ? std::optional(t.origin(*r)) : std::nullopt;
                        }
                        return std::nullopt;
                    };
                    auto w1 = word(1), w3 = word(3);
                    if (w1 && w1->is_const && w3 && w3->is_const && w3->c >> 28 == 8) {
                        out.bvh.emplace_back(site, BvhNodes{BvhNodes::Memory, w1->c, w3->c});
                    } else {
                        descriptor(f->rsrc, Role::Resource);
                        out.bvh.emplace_back(site, BvhNodes{BvhNodes::Resource, 0, 0});
                    }
                } else {
                    descriptor(f->rsrc, Role::Resource);
                    if (f->sampler) {
                        descriptor(*f->sampler, Role::Sampler);
                    }
                }
            } else if (auto* fb = std::get_if<BufferFields>(&inst.fields)) {
                if (!name.ends_with("_inv")) {
                    descriptor(fb->rsrc, Role::Resource);
                }
            } else if (auto* fs = std::get_if<SmemFields>(&inst.fields)) {
                if (name.starts_with("s_buffer_")) {
                    descriptor(fs->base, Role::Resource);
                } else if (name.find("_load_") != std::string_view::npos || name.find("_store_") != std::string_view::npos) {
                    auto a = t.smem_address(inst, fs->base, fs->offset, fs->soffset);
                    if (!a.ok()) {
                        a = t.by_address(a.why);
                    }
                    if (!a.ok()) {
                        fail(a.why);
                    }
                    out.scalar.emplace_back(site, a.value);
                }
            } else if (auto* ff = std::get_if<FlatFields>(&inst.fields)) {
                if (name.starts_with("global_")) {
                    auto a = t.flat_address(inst, ff->addr, ff->saddr, ff->offset);
                    if (!a.ok()) {
                        a = t.by_address(a.why);
                    }
                    if (!a.ok()) {
                        fail(a.why);
                    }
                    out.scalar.emplace_back(site, a.value);
                }
            }
            if (!first_lane.empty()) {
                auto& [accesses, reads] = first_lane_at[b];
                accesses.push_back(i);
                reads.insert(reads.end(), first_lane.begin(), first_lane.end());
            }
            if (!offset.empty()) {
                auto& [accesses, reads, loads] = offset_at[b];
                accesses.push_back(i);
                for (const auto& [v, load] : offset) {
                    reads.push_back(v);
                    loads.push_back(load);
                }
            }
        }
    }
    t.at.reset();
    if (!t.lanes) {
        return out;
    }
    for (auto& [block, entry] : first_lane_at) {
        auto& [accesses, reads] = entry;
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        auto proof = t.waterfall(block, accesses, reads);
        if (!proof.ok()) {
            unsupported(t.inst({block, accesses[0]}).machine.offset, proof.why);
        }
        out.waterfall.compares.insert(out.waterfall.compares.end(), proof.value.compares.begin(), proof.value.compares.end());
        out.waterfall.reads.insert(out.waterfall.reads.end(), proof.value.reads.begin(), proof.value.reads.end());
    }
    for (auto& [block, entry] : offset_at) {
        auto& [accesses, reads, loads] = entry;
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        auto proof = t.waterfall(block, accesses, reads);
        if (proof.ok()) {
            out.waterfall.compares.insert(out.waterfall.compares.end(), proof.value.compares.begin(), proof.value.compares.end());
            out.waterfall.reads.insert(out.waterfall.reads.end(), proof.value.reads.begin(), proof.value.reads.end());
            out.waterfall.loads.insert(out.waterfall.loads.end(), loads.begin(), loads.end());
        }
    }
    out.lane_copied = std::move(t.lane_copied);
    std::sort(out.lane_copied.begin(), out.lane_copied.end());
    out.lane_copied.erase(std::unique(out.lane_copied.begin(), out.lane_copied.end()), out.lane_copied.end());
    for (const Descriptor& d : out.descriptors) {
        if (auto v = t.root_indexed_view(d, out.scalar)) {
            out.root_indexed_views.push_back(*v);
        }
    }
    return out;
}

}   
