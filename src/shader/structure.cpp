// SPDX-License-Identifier: GPL-3.0-or-later
// Control-flow structurisation (`structure.rs`; DanziLoader / victory-perf,
// see NOTICE.md). See structure.hpp. Every BDD operation is made in the
// reference's order (its short-circuits and its evaluation order), so the
// nodes get the reference's numbers.
#include "structure.hpp"

#include <algorithm>
#include <bit>
#include <string>

#include "error.hpp"

namespace danzi::shader::structure {

using ir::Reg;
using ir::Value;

namespace {

constexpr std::uint32_t TERMINAL = UINT32_MAX;
constexpr std::size_t MAX_NODES = std::size_t{1} << 21;
constexpr std::size_t MAX_MEET = std::size_t{1} << 16;
constexpr std::size_t MAX_MEETS = std::size_t{1} << 18;
constexpr std::size_t MAX_ROUNDS = 256;
constexpr std::size_t MAX_MEMO = std::size_t{1} << 23;

[[noreturn]] void too_large() { isa(0, "lane conditions too large"); }

}   

Bools::Bools() {
    nodes_.push_back({TERMINAL, FALSE_, FALSE_});
    nodes_.push_back({TERMINAL, TRUE_, TRUE_});
}

void Bools::reset() {
    nodes_.resize(2);
    unique_.clear_keep();
    memo_.clear_keep();
    atoms_.clear();
    born_.clear();
    ids_.clear_keep();
    overflow = false;
}

std::optional<Bools::Node> Bools::node(Bool b) const {
    const Raw& r = nodes_[b.id];
    if (r.v == TERMINAL) {
        return std::nullopt;
    }
    return Node{atoms_[r.v], r.lo, r.hi};
}

std::vector<std::uint32_t> Bools::vars(Bool b) const {


    if (seen_.size() < nodes_.size()) {
        seen_.resize(nodes_.size(), 0);
    }
    if (var_seen_.size() < atoms_.size()) {
        var_seen_.resize(atoms_.size(), 0);
    }
    if (++stamp_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0);
        std::fill(var_seen_.begin(), var_seen_.end(), 0);
        stamp_ = 1;
    }
    std::vector<std::uint32_t> out;
    auto& stack = stack_;
    stack.clear();
    stack.push_back(b);
    while (!stack.empty()) {
        Bool x = stack.back();
        stack.pop_back();
        const Raw& r = nodes_[x.id];
        if (r.v == TERMINAL || seen_[x.id] == stamp_) {
            continue;
        }
        seen_[x.id] = stamp_;
        if (var_seen_[r.v] != stamp_) {
            var_seen_[r.v] = stamp_;
            out.push_back(r.v);
        }
        stack.push_back(r.lo);
        stack.push_back(r.hi);
    }
    return out;
}

std::vector<Atom> Bools::support(Bool b) const {
    auto v = vars(b);
    std::sort(v.begin(), v.end());
    std::vector<Atom> out;
    out.reserve(v.size());
    for (std::uint32_t x : v) {
        out.push_back(atoms_[x]);
    }
    return out;
}

Bool Bools::atom(Atom a, std::size_t born) {
    auto [id, fresh] = ids_.try_emplace(a, static_cast<std::uint32_t>(atoms_.size()));
    if (fresh) {
        atoms_.push_back(a);
        born_.push_back(born);
    }
    return mk(*id, FALSE_, TRUE_);
}

Bool Bools::mk(std::uint32_t v, Bool lo, Bool hi) {
    if (lo == hi) {
        return lo;
    }
    if (const std::uint32_t* b = unique_.find(v, lo.id, hi.id)) {
        return Bool{*b};
    }
    if (nodes_.size() >= MAX_NODES) {
        overflow = true;
        return FALSE_;
    }
    Bool b{static_cast<std::uint32_t>(nodes_.size())};
    nodes_.push_back({v, lo, hi});
    unique_.insert(v, lo.id, hi.id, b.id);
    return b;
}

std::pair<Bool, Bool> Bools::split(Bool b, std::uint32_t v) const {
    const Raw& r = nodes_[b.id];
    return r.v == v ? std::pair{r.lo, r.hi} : std::pair{b, b};
}

Bool Bools::ite(Bool f, Bool g, Bool h) {
    if (f == TRUE_ || g == h) {
        return g;
    }
    if (f == FALSE_) {
        return h;
    }
    if (g == TRUE_ && h == FALSE_) {
        return f;
    }
    if (const std::uint32_t* r = memo_.find(f.id, g.id, h.id)) {
        return Bool{*r};
    }
    std::uint32_t v = std::min({nodes_[f.id].v, nodes_[g.id].v, nodes_[h.id].v});
    auto [f0, f1] = split(f, v);
    auto [g0, g1] = split(g, v);
    auto [h0, h1] = split(h, v);
    Bool lo = ite(f0, g0, h0);
    Bool hi = ite(f1, g1, h1);
    Bool r = mk(v, lo, hi);


    if (nodes_.size() > memo_.capacity()) {
        memo_.reserve(nodes_.size(), MAX_MEMO);
    }
    memo_.insert(f.id, g.id, h.id, r.id);
    return r;
}

bool Bools::meet(const std::vector<std::pair<Bool, bool>>& all) const {
    Meets seen;
    return meet_seen(all, seen);
}

bool Bools::meet_seen(const std::vector<std::pair<Bool, bool>>& all, Meets& seen) const {
    std::vector<std::uint32_t> work;
    work.reserve(all.size());
    for (const auto& [b, holds] : all) {
        work.push_back(b.id << 1 | (holds ? 1u : 0u));
    }
    std::size_t budget = MAX_MEET;
    return meet_in(0, work, budget, seen);
}

bool Bools::meet_in(std::size_t at, std::vector<std::uint32_t>& work, std::size_t& budget, Meets& seen) const {
    std::size_t kept = at;
    for (std::size_t i = at; i < work.size(); ++i) {
        std::uint32_t b = work[i] >> 1;
        bool holds = (work[i] & 1) != 0;
        if (b > TRUE_.id) {
            work[kept++] = work[i];
        } else if ((b == TRUE_.id) != holds) {
            work.resize(at);
            return false;
        }
    }
    work.resize(kept);
    if (kept == at || budget == 0) {
        work.resize(at);
        return true;
    }
    std::sort(work.begin() + static_cast<std::ptrdiff_t>(at), work.end());
    std::size_t end = at + 1;
    for (std::size_t i = at + 1; i < kept; ++i) {
        if (work[i] != work[end - 1]) {
            work[end++] = work[i];
        }
    }
    work.resize(end);
    std::string key(reinterpret_cast<const char*>(work.data() + at), (end - at) * 4);
    if (auto it = seen.none.find(key); it != seen.none.end()) {
        std::size_t splits = it->second;
        bool within = splits <= budget;
        budget = budget > splits ? budget - splits : 0;
        work.resize(at);
        return !within;
    }
    std::size_t before = budget;
    budget -= 1;
    std::uint32_t v = UINT32_MAX;
    for (std::size_t i = at; i < end; ++i) {
        v = std::min(v, nodes_[work[i] >> 1].v);
    }
    bool found = false;
    for (bool value : {false, true}) {
        for (std::size_t i = at; i < end; ++i) {
            auto [lo, hi] = split(Bool{work[i] >> 1}, v);
            work.push_back((value ? hi : lo).id << 1 | (work[i] & 1));
        }
        if (meet_in(end, work, budget, seen)) {
            found = true;
            break;
        }
    }
    if (!found && seen.none.size() < MAX_MEETS) {
        seen.none.emplace(key, before - budget);
    }
    work.resize(at);
    return found;
}

Bool Bools::forget(Bool b, std::size_t from, FlatMap<std::uint32_t, Bool>& memo) {
    Raw r = nodes_[b.id];
    if (r.v == TERMINAL) {
        return b;
    }
    if (Bool* m = memo.find(b.id)) {
        return *m;
    }
    Bool lo = forget(r.lo, from, memo);
    Bool hi = forget(r.hi, from, memo);
    Bool out = born_[r.v] >= from ? or_(lo, hi) : mk(r.v, lo, hi);
    memo.insert(b.id, out);
    return out;
}

Bool Bools::restrict(Bool b, std::uint32_t v, bool value, FlatMap<std::uint32_t, Bool>& memo) {
    Raw r = nodes_[b.id];
    if (r.v == TERMINAL || r.v > v) {
        return b;
    }
    if (r.v == v) {
        return value ? r.hi : r.lo;
    }
    if (Bool* m = memo.find(b.id)) {
        return *m;
    }
    Bool l = restrict(r.lo, v, value, memo);
    Bool h = restrict(r.hi, v, value, memo);
    Bool out = mk(r.v, l, h);
    memo.insert(b.id, out);
    return out;
}

Bool Bools::given(Bool b, Bool known) {
    for (std::uint32_t v : vars(b)) {
        Bool x = mk(v, FALSE_, TRUE_);
        Bool nx = not_(x);
        if (implies(known, x)) {
            FlatMap<std::uint32_t, Bool> memo;
            b = restrict(b, v, true, memo);
        } else if (implies(known, nx)) {
            FlatMap<std::uint32_t, Bool> memo;
            b = restrict(b, v, false, memo);
        }
    }
    return b;
}

std::optional<Bool> Structured::mask_bits(Value low, std::optional<Value> high) const {
    if (high) {
        const Bool* b = paired_masks.find(Key{low.id, high->id});
        return b ? std::optional(*b) : std::nullopt;
    }
    auto it = masks.find(low);
    return it != masks.end() ? std::optional(it->second) : std::nullopt;
}

namespace {

struct SopMasks {
    std::optional<std::pair<Key, Bool>> dst, exec;
    bool derived = false;
    std::optional<Bool> scc;
};

struct Join {
    std::size_t block;
    Value id;
    bool value, derived;
    std::vector<Key> args;
};

struct PhiJoin {
    std::size_t block;
    Value id;
    bool derived;
    std::vector<Key> args;
};

using Site = std::pair<std::size_t, std::size_t>;

class Lanes {
public:
    explicit Lanes(const ir::Program& p);

    const ir::Program& p;
    std::vector<std::size_t> order, index;
    std::vector<std::vector<std::pair<std::size_t, bool>>> incoming;
    FlatMap<std::uint32_t, Site> defs, phis;
    std::unordered_set<std::uint32_t> unresolved, derived_phis;
    Bools b;
    FlatMap<Key, Bool> masks, other;
    std::vector<Key> seen;
    FlatMap<std::uint32_t, Bool> scc_lane;
    std::vector<Join> joins;
    std::vector<PhiJoin> phi_joins;
    std::unordered_set<std::uint32_t> phi_join_ids;
    std::vector<Bool> first;
    std::vector<std::optional<Bool>> when;
    std::unordered_set<std::uint32_t> active;
    std::unordered_set<std::size_t> wide;
    std::unordered_set<std::uint32_t> scc_every;
    std::vector<std::optional<bool>> wave_any;

    void reset();
    std::vector<std::vector<std::optional<Bool>>> masks_in_order();
    Bool edge(Bool c, std::size_t q, bool taken);
    std::pair<std::vector<Value>, std::vector<Value>> check_joins(const std::vector<Bool>& cond);
    Structured finish(const std::vector<std::vector<std::optional<Bool>>>& execs, const std::vector<Bool>& cond);
    template <class F>
    std::optional<Key> key(F value, std::uint8_t code) const;
    std::size_t born(Value v) const;
    Bool exec(const ir::Inst& inst);
    std::pair<Bool, bool> mask(Key k);
    std::optional<std::pair<Bool, bool>> join(Key k);
    std::optional<std::pair<Bool, bool>> operand(const ir::Inst& inst, const Operand& op);
    std::optional<SopMasks> sop(const ir::Inst& inst);
    bool clear_after_loop(Value scc) const;
    std::optional<Bool> combine(std::string_view f, Bool a, Bool c);
    void step(const ir::Inst& inst, std::optional<Bool> exec);
    std::optional<bool> wave_kind(const ir::Inst& inst) const;
    Bool branch(const ir::Inst& inst);
    std::vector<Bool> paths();
};

Lanes::Lanes(const ir::Program& prog) : p(prog) {
    const auto& blocks = p.blocks;
    order = ir::reverse_postorder(blocks);
    index.assign(blocks.size(), SIZE_MAX);
    for (std::size_t i = 0; i < order.size(); ++i) {
        index[order[i]] = i;
    }


    if (auto e = ir::irreducible_edge(blocks, order, index)) {
        unsupported(blocks[e->first].offset, "irreducible control flow");
    }
    incoming.assign(blocks.size(), {});
    for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
        const ir::Block& block = blocks[bi];
        std::size_t s[2];
        std::size_t n = block.exit.successors(s);
        for (std::size_t k = 0; k < n; ++k) {
            incoming[s[k]].emplace_back(bi, k == 0);
        }
        for (std::size_t i = 0; i < block.phis.size(); ++i) {
            phis.insert(block.phis[i].value.id, {bi, i});
        }
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            for (const auto& w : block.insts[i].writes) {
                defs.insert(w.second.id, {bi, i});
            }
        }
    }
    first.assign(blocks.size(), FALSE_);
    when.assign(blocks.size(), std::nullopt);
    wave_any.assign(blocks.size(), std::nullopt);
}

void Lanes::reset() {
    b.reset();
    masks.clear();
    other.clear();
    seen.clear();
    scc_lane.clear();
    scc_every.clear();
    std::fill(wave_any.begin(), wave_any.end(), std::nullopt);
    joins.clear();
    phi_joins.clear();
    phi_join_ids.clear();
    std::fill(first.begin(), first.end(), FALSE_);
    std::fill(when.begin(), when.end(), std::nullopt);
    active.clear();
}

template <class F>
std::optional<Key> Lanes::key(F value, std::uint8_t code) const {
    std::optional<Value> lo = value(Reg::s(code));
    if (!lo) {
        return std::nullopt;
    }
    Value hi = *lo;
    if (p.wave64) {
        auto h = value(Reg::s(static_cast<std::uint8_t>(code + 1)));
        if (!h) {
            return std::nullopt;
        }
        hi = *h;
    }
    return Key{lo->id, hi.id};
}

std::size_t Lanes::born(Value v) const {
    if (const Site* d = defs.find(v.id)) {
        return index[d->first];
    }
    if (const Site* d = phis.find(v.id)) {
        return index[d->first];
    }
    return 0;
}

Bool Lanes::exec(const ir::Inst& inst) {
    auto k = key([&](Reg r) { return inst.read(r); }, ir::EXEC);
    return k ? mask(*k).first : TRUE_;
}

std::vector<std::vector<std::optional<Bool>>> Lanes::masks_in_order() {
    auto input = [&](Reg reg) -> std::optional<Value> {
        for (const auto& i : p.inputs) {
            if (i.first == reg) {
                return i.second;
            }
        }
        return std::nullopt;
    };
    if (auto lo = input(Reg::s(ir::EXEC))) {
        Value hi = *lo;
        if (p.wave64) {
            hi = input(Reg::s(ir::EXEC + 1)).value_or(*lo);
        }
        masks.insert(Key{lo->id, hi.id}, TRUE_);
        active.insert(lo->id);
    }
    std::vector<std::vector<std::optional<Bool>>> execs;
    execs.reserve(p.blocks.size());
    for (const auto& block : p.blocks) {
        execs.emplace_back(block.insts.size(), std::nullopt);
    }
    for (std::size_t n = 0; n < order.size(); ++n) {
        std::size_t bi = order[n];
        Bool c = n == 0 ? TRUE_ : FALSE_;
        for (std::size_t k = 0; k < incoming[bi].size(); ++k) {
            auto [q, taken] = incoming[bi][k];
            if (index[q] < n) {
                Bool e = edge(first[q], q, taken);
                c = b.or_(c, e);
            }
        }
        first[bi] = c;
        for (const auto& phi : p.blocks[bi].phis) {
            if (phi.reg == Reg::s(ir::EXEC)) {
                active.insert(phi.value.id);
            }
        }
        std::optional<Bool> now;
        const auto& insts = p.blocks[bi].insts;
        for (std::size_t i = 0; i < insts.size(); ++i) {
            const ir::Inst& inst = insts[i];
            if (inst.read(Reg::s(ir::EXEC))) {
                Bool e = exec(inst);
                execs[bi][i] = e;
                now = e;
            }
            step(inst, now);
            if (auto k = key([&](Reg r) { return inst.write(r); }, ir::EXEC)) {
                if (const Bool* m = masks.find(*k)) {
                    now = *m;
                } else if (const Bool* o = other.find(*k)) {
                    now = *o;
                } else {
                    now = std::nullopt;
                }
            }
        }
        if (p.blocks[bi].exit.kind == ir::Exit::Branch && !insts.empty()) {
            when[bi] = branch(insts.back());
            wave_any[bi] = wave_kind(insts.back());
        }
    }
    return execs;
}

Bool Lanes::edge(Bool c, std::size_t q, bool taken) {
    if (wide.contains(q)) {
        return c;
    }
    if (when[q]) {
        Bool w = *when[q];
        if (taken) {
            return b.and_(c, w);
        }
        Bool nw = b.not_(w);
        return b.and_(c, nw);
    }
    return c;
}

std::pair<std::vector<Value>, std::vector<Value>> Lanes::check_joins(const std::vector<Bool>& cond) {
    std::vector<Value> wrong, new_derived;
    std::unordered_set<std::uint32_t> seen_derived;
    std::size_t j = 0, pj = 0;
    while (j < joins.size() || pj < phi_joins.size()) {
        while (j < joins.size()) {
            std::size_t block = joins[j].block;
            Value id = joins[j].id;
            bool value = joins[j].value, was_derived = joins[j].derived;
            std::vector<Key> args = joins[j].args;
            bool failed = false;
            for (std::size_t n = 0; n < args.size(); ++n) {
                auto [q, taken] = incoming[block][n];
                Bool e = edge(cond[q], q, taken);
                auto [m, d] = mask(args[n]);
                if (!was_derived && d && !derived_phis.contains(id.id) && seen_derived.insert(id.id).second) {
                    new_derived.push_back(id);
                }
                if (!value) {
                    m = b.not_(m);
                }
                if (!failed && !b.implies(e, m)) {
                    wrong.push_back(id);
                    failed = true;
                }
            }
            ++j;
        }
        while (pj < phi_joins.size()) {
            Value id = phi_joins[pj].id;
            bool was_derived = phi_joins[pj].derived;
            std::vector<Key> args = phi_joins[pj].args;
            for (const Key& a : args) {
                auto [m, d] = mask(a);
                if (!was_derived && d && !derived_phis.contains(id.id) && seen_derived.insert(id.id).second) {
                    new_derived.push_back(id);
                }
            }
            ++pj;
        }
    }
    return {std::move(wrong), std::move(new_derived)};
}

Structured Lanes::finish(const std::vector<std::vector<std::optional<Bool>>>& execs, const std::vector<Bool>& cond) {
    std::vector<PhiJoin> taken_joins = std::move(phi_joins);
    phi_joins.clear();
    Structured out;
    for (PhiJoin& pjoin : taken_joins) {
        std::vector<Bool> in;
        for (std::size_t n = 0; n < pjoin.args.size(); ++n) {
            auto [q, taken] = incoming[pjoin.block][n];
            Bool e = edge(cond[q], q, taken);
            Bool m = mask(pjoin.args[n]).first;
            in.push_back(b.given(m, e));
        }
        out.phi_masks[pjoin.id] = std::move(in);
    }
    const auto& blocks = p.blocks;
    out.guards.resize(blocks.size());
    for (std::size_t n = 0; n < blocks.size(); ++n) {
        const auto& insts = blocks[n].insts;
        out.guards[n].reserve(insts.size());
        for (std::size_t i = 0; i < insts.size(); ++i) {
            const auto& e = execs[n][i];
            const Fields& f = insts[i].fields;
            if (!e || std::holds_alternative<SopFields>(f) || std::holds_alternative<SoppFields>(f) || std::holds_alternative<SmemFields>(f)) {
                out.guards[n].push_back(std::nullopt);
            } else if (b.implies(cond[n], *e)) {
                out.guards[n].push_back(std::nullopt);
            } else {
                out.guards[n].push_back(b.given(*e, cond[n]));
            }
        }
    }
    out.exits.reserve(blocks.size());
    for (std::size_t n = 0; n < blocks.size(); ++n) {
        const ir::Exit& x = blocks[n].exit;
        LaneExit le;
        switch (x.kind) {
            case ir::Exit::Jump: le.kind = LaneExit::Jump; le.taken = x.a; break;
            case ir::Exit::End: le.kind = LaneExit::End; break;
            case ir::Exit::Indirect: le.kind = LaneExit::Indirect; break;
            case ir::Exit::Branch:
                le.kind = LaneExit::Branch;
                le.when = b.given(when[n].value_or(FALSE_), cond[n]);
                le.taken = x.a;
                le.not_taken = x.b;
                break;
        }
        out.exits.push_back(le);
    }
    if (b.overflow) {
        too_large();
    }

    FlatMap<std::uint32_t, std::uint32_t> high;
    std::unordered_set<std::uint32_t> split;
    auto note = [&](const Key& k) {
        auto [h, fresh] = high.try_emplace(k.lo, k.hi);
        if (*h != k.hi) {
            split.insert(k.lo);
        }
    };
    masks.for_each([&](const Key& k, const Bool&) { note(k); });
    other.for_each([&](const Key& k, const Bool&) { note(k); });
    for (const Key& k : seen) {
        note(k);
    }
    FlatMap<Key, Bool> kept;
    masks.for_each([&](const Key& k, const Bool& m) {
        if (!split.contains(k.lo)) {
            kept.insert(k, m);
        }
    });
    std::erase_if(active, [&](std::uint32_t lo) { return split.contains(lo); });
    kept.for_each([&](const Key& k, const Bool& m) {
        out.masks[Value{k.lo}] = m;
        if (k.lo != k.hi) {
            out.highs[Value{k.hi}] = Value{k.lo};
        }
    });
    if (p.wave64) {
        out.paired_masks = std::move(kept);
    }
    for (std::uint32_t a : active) {
        out.active.insert(Value{a});
    }
    out.wide = wide;
    out.wave_any = wave_any;
    scc_lane.for_each([&](const std::uint32_t& scc, const Bool& m) {
        if (!scc_every.contains(scc)) {
            out.scc_some[Value{scc}] = m;
        }
    });
    out.reach = cond;
    out.bools = std::move(b);
    return out;
}

std::pair<Bool, bool> Lanes::mask(Key k) {
    if (const Bool* m = masks.find(k)) {
        return {*m, true};
    }
    if (const Bool* m = other.find(k)) {
        return {*m, false};
    }
    std::pair<Bool, bool> result;
    if (auto j = join(k)) {
        result = *j;
    } else {
        const Site* lo = defs.find(k.lo);
        const Site* hi = defs.find(k.hi);
        auto def = [&](const Site* d) -> const ir::Inst* { return d ? &p.blocks[d->first].insts[d->second] : nullptr; };
        std::optional<Bool> m;
        if (lo && hi && *lo == *hi) {
            if (auto r = sop(*def(lo)); r && r->dst) {
                m = r->dst->second;
            }
        }
        auto exec_write = [&](const Site* d, std::uint32_t v) {
            const ir::Inst* i = def(d);
            if (!i) {
                return false;
            }
            auto e0 = i->write(Reg::s(ir::EXEC));
            auto e1 = i->write(Reg::s(ir::EXEC + 1));
            return (e0 && e0->id == v) || (e1 && e1->id == v);
        };
        bool derived = exec_write(lo, k.lo) || exec_write(hi, k.hi);
        if (!m) {
            std::size_t at = born(Value{k.lo});
            m = b.atom(Atom{Atom::Opaque, k.lo}, at);
        }
        result = {*m, derived};
    }
    (result.second ? masks : other).insert(k, result.first);
    return result;
}

std::optional<std::pair<Bool, bool>> Lanes::join(Key k) {
    const Site* lo = phis.find(k.lo);
    const Site* hi = phis.find(k.hi);
    std::size_t block;
    if (lo && hi && lo->first != hi->first) {
        return std::nullopt;
    }
    if (lo) {
        block = lo->first;
    } else if (hi) {
        block = hi->first;
    } else {
        return std::nullopt;
    }
    std::optional<Site> los = lo ? std::optional(*lo) : std::nullopt;
    std::optional<Site> his = hi ? std::optional(*hi) : std::nullopt;
    const ir::Block& target = p.blocks[block];
    auto arg = [&](const std::optional<Site>& phi, std::uint32_t own, std::size_t n) -> std::uint32_t {
        return phi ? target.phis[phi->second].args[n].id : own;
    };
    std::vector<Key> args;
    for (std::size_t n = 0; n < target.preds.size(); ++n) {
        args.push_back(Key{arg(los, k.lo, n), arg(his, k.hi, n)});
    }
    Value id{los ? k.lo : k.hi};
    bool derived = derived_phis.contains(id.id);
    std::vector<std::pair<Bool, Bool>> forward;
    for (std::size_t n = 0; n < args.size(); ++n) {
        auto [q, taken] = incoming[block][n];
        if (index[q] < index[block]) {
            auto [m, d] = mask(args[n]);
            derived |= d;
            Bool e = edge(first[q], q, taken);
            forward.emplace_back(e, m);
        } else {
            seen.push_back(args[n]);
        }
    }
    if (forward.size() == args.size() && !forward.empty()) {
        Bool m = forward[0].second;
        if (std::all_of(forward.begin(), forward.end(), [&](const auto& f) { return f.second == m; })) {
            return std::pair{m, derived};
        }
    }
    if (!unresolved.contains(id.id)) {
        for (bool value : {true, false}) {
            bool holds = true;
            for (const auto& [e, m0] : forward) {
                Bool m = value ? m0 : b.not_(m0);
                if (!b.implies(e, m)) {
                    holds = false;
                    break;
                }
            }
            if (holds) {
                joins.push_back(Join{block, id, value, derived, args});
                return std::pair{value ? TRUE_ : FALSE_, derived};
            }
        }
    }
    if (phi_join_ids.insert(id.id).second) {
        phi_joins.push_back(PhiJoin{block, id, derived, args});
    }
    std::size_t at = index[block];
    return std::pair{b.atom(Atom{Atom::Phi, id.id}, at), derived};
}

std::optional<std::pair<Bool, bool>> Lanes::operand(const ir::Inst& inst, const Operand& op) {
    bool wave64 = p.wave64;
    auto constant = [&](std::uint64_t v) {
        if (!wave64) {
            v &= 0xFFFFFFFFull;
        }
        if (v == 0) {
            return FALSE_;
        }
        if (v == (wave64 ? UINT64_MAX : (UINT64_MAX >> 32))) {
            return TRUE_;
        }
        return b.atom(Atom{Atom::Constant, v}, 0);
    };
    if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
        auto k = key([&](Reg r) { return inst.read(r); }, op.a);
        if (!k) {
            return std::nullopt;
        }
        return mask(*k);
    }
    if (op.kind == Operand::Const && op.a >= 128 && op.a <= 208) {
        std::int64_t v = op.a <= 192 ? std::int64_t{op.a} - 128 : 192 - std::int64_t{op.a};
        return std::pair{constant(static_cast<std::uint64_t>(v)), false};
    }
    if (op.kind == Operand::Literal) {
        return std::pair{constant(std::uint64_t{op.literal}), false};
    }
    return std::nullopt;
}

std::optional<SopMasks> Lanes::sop(const ir::Inst& inst) {
    auto* f = std::get_if<SopFields>(&inst.fields);
    if (!f) {
        return std::nullopt;
    }
    std::string_view width = p.wave64 ? "_b64" : "_b32";
    std::string_view name = inst.name();
    if (!name.starts_with("s_") || !name.ends_with(width) || name.size() < 2 + width.size()) {
        return std::nullopt;
    }
    std::string_view op = name.substr(2, name.size() - 2 - width.size());
    std::optional<Key> dst;
    if (f->dst && f->dst->kind == Operand::Scalar && f->dst->a != NULL_CODE) {
        dst = key([&](Reg r) { return inst.write(r); }, f->dst->a);
    }
    auto exec_out = key([&](Reg r) { return inst.write(r); }, ir::EXEC);
    std::optional<bool> save;
    std::string_view fname = op;
    if (op.ends_with("_saveexec")) {
        save = true;
        fname = op.substr(0, op.size() - 9);
    } else if (op.ends_with("_wrexec")) {
        save = false;
        fname = op.substr(0, op.size() - 7);
    }
    bool derived = exec_out.has_value() || save.has_value();
    auto arg = [&](std::size_t n) -> std::optional<Bool> {
        if (n >= f->src.size()) {
            return std::nullopt;
        }
        auto r = operand(inst, f->src[n]);
        if (!r) {
            return std::nullopt;
        }
        derived |= r->second;
        return r->first;
    };
    if (save) {
        auto s = arg(0);
        if (!s) {
            return std::nullopt;
        }
        Bool e = exec(inst);
        auto now = combine(fname, *s, e);
        if (!now) {
            return std::nullopt;
        }
        Bool saved = *save ? e : *now;
        SopMasks r;
        if (dst) {
            r.dst = std::pair{*dst, saved};
        }
        if (exec_out) {
            r.exec = std::pair{*exec_out, *now};
        }
        r.derived = derived;
        r.scc = *now;
        return r;
    }
    Bool result;
    std::optional<Bool> scc;
    if (op == "mov") {
        auto a = arg(0);
        if (!a) {
            return std::nullopt;
        }
        result = *a;
    } else if (op == "not") {
        auto a = arg(0);
        if (!a) {
            return std::nullopt;
        }
        result = b.not_(*a);
        scc = result;
    } else if (op == "wqm") {
        auto a = arg(0);
        if (!a) {
            return std::nullopt;
        }
        auto lo_key = dst ? dst : exec_out;
        if (!lo_key) {
            return std::nullopt;
        }
        Value lo{lo_key->lo};
        std::size_t at = born(lo);
        Bool q = b.atom(Atom{Atom::Quad, lo.id}, at);
        result = b.or_(*a, q);
        scc = result;
    } else if (op == "cselect") {
        auto a = arg(0);
        if (!a) {
            return std::nullopt;
        }
        auto c = arg(1);
        if (!c) {
            return std::nullopt;
        }
        auto sccv = inst.read(Reg::scc());
        if (!sccv) {
            return std::nullopt;
        }
        if (clear_after_loop(*sccv)) {
            result = *c;
        } else {
            std::size_t at = born(*sccv);
            Bool u = b.atom(Atom{Atom::Uniform, sccv->id}, at);
            result = b.ite(u, *a, *c);
        }
    } else {
        auto a = arg(0);
        if (!a) {
            return std::nullopt;
        }
        auto c = arg(1);
        if (!c) {
            return std::nullopt;
        }
        auto r = combine(op, *a, *c);
        if (!r) {
            return std::nullopt;
        }
        result = *r;
        scc = result;
    }
    SopMasks r;
    if (dst) {
        r.dst = std::pair{*dst, result};
    }
    if (exec_out) {
        r.exec = std::pair{*exec_out, result};
    }
    r.derived = derived;
    r.scc = scc;
    return r;
}

bool Lanes::clear_after_loop(Value scc) const {
    const Site* d = defs.find(scc.id);
    if (!d) {
        return false;
    }
    const ir::Block& block = p.blocks[d->first];
    std::size_t i = d->second;
    if (i >= block.insts.size() || block.insts.empty()) {
        return false;
    }
    const ir::Inst& writer = block.insts[i];
    const ir::Inst& last = block.insts.back();
    auto ex = writer.write(Reg::s(ir::EXEC));
    bool back = block.exit.kind == ir::Exit::Branch && index[block.exit.a] <= index[d->first];
    return back && i + 2 == block.insts.size() && ex && last.name() == "s_cbranch_execnz" && last.read(Reg::s(ir::EXEC)) == ex &&
           (!p.wave64 || last.read(Reg::s(ir::EXEC + 1)) == writer.write(Reg::s(ir::EXEC + 1)));
}

std::optional<Bool> Lanes::combine(std::string_view f, Bool a, Bool c) {
    if (f == "and") {
        return b.and_(a, c);
    }
    if (f == "or") {
        return b.or_(a, c);
    }
    if (f == "xor") {
        return b.xor_(a, c);
    }
    if (f == "andn2") {
        Bool nc = b.not_(c);
        return b.and_(a, nc);
    }
    if (f == "orn2") {
        Bool nc = b.not_(c);
        return b.or_(a, nc);
    }
    if (f == "andn1") {
        Bool na = b.not_(a);
        return b.and_(na, c);
    }
    if (f == "orn1") {
        Bool na = b.not_(a);
        return b.or_(na, c);
    }
    if (f == "nand") {
        Bool r = b.and_(a, c);
        return b.not_(r);
    }
    if (f == "nor") {
        Bool r = b.or_(a, c);
        return b.not_(r);
    }
    if (f == "xnor") {
        Bool r = b.xor_(a, c);
        return b.not_(r);
    }
    return std::nullopt;
}

void Lanes::step(const ir::Inst& inst, std::optional<Bool> exec_now) {
    std::string_view name = inst.name();
    auto read = [&](Reg r) { return inst.read(r); };
    auto write = [&](Reg r) { return inst.write(r); };
    if (auto* f = std::get_if<SopFields>(&inst.fields)) {
        bool takes_mask = false;
        for (const Operand& s : f->src) {
            if (s.kind == Operand::Scalar && s.a != NULL_CODE) {
                if (auto k = key(read, s.a)) {
                    bool phi = phis.contains(k->lo) || phis.contains(k->hi);
                    bool rhs = masks.contains(*k) || (phi && mask(*k).second);
                    takes_mask = takes_mask || rhs;
                }
            }
        }
        bool touches_exec = write(Reg::s(ir::EXEC)).has_value() || read(Reg::s(ir::EXEC)).has_value();
        if (takes_mask || touches_exec) {
            auto r = sop(inst);
            if (r && r->derived) {
                std::optional<std::pair<Key, Bool>> list[2] = {r->dst, r->exec};
                for (const auto& entry : list) {
                    if (!entry) {
                        continue;
                    }
                    auto [k, m] = *entry;
                    masks.insert(k, m);
                    bool is_exec = r->exec && r->exec->first == k;
                    if (is_exec || (exec_now && b.implies(m, *exec_now))) {
                        active.insert(k.lo);
                    }
                }
                auto scc = write(Reg::scc());
                if (scc && r->scc) {
                    scc_lane.insert(scc->id, *r->scc);
                }
            }
        }

        std::string_view width = p.wave64 ? "64" : "32";
        if (takes_mask && name.starts_with("s_cmp_")) {
            auto scc = write(Reg::scc());
            std::string_view rest = name.substr(6);
            if (scc && rest.ends_with(width)) {
                std::string_view r = rest.substr(0, rest.size() - width.size());
                if ((r == "eq_u" || r == "lg_u") && f->src.size() == 2) {
                    auto a = operand(inst, f->src[0]);
                    auto c = operand(inst, f->src[1]);
                    if (a && c) {
                        Bool ne = b.xor_(a->first, c->first);
                        Bool m = rest.starts_with("eq") ? b.not_(ne) : ne;
                        scc_lane.insert(scc->id, m);
                        if (rest.starts_with("eq")) {
                            scc_every.insert(scc->id);
                        }
                    }
                }
            }
        }

        if (takes_mask && name.starts_with("s_bcnt1_i32_b")) {
            auto scc = write(Reg::scc());
            if (scc && f->src.size() == 1) {
                auto o = operand(inst, f->src[0]);
                if (o && o->second) {
                    Bool m = o->first;
                    Bool e = exec_now.value_or(TRUE_);
                    Bool own = b.given(m, e);
                    if (b.implies(m, e)) {
                        auto sup = b.support(own);
                        if (std::all_of(sup.begin(), sup.end(), [](const Atom& a) { return a.kind == Atom::Uniform; })) {
                            scc_lane.insert(scc->id, own);
                        }
                    }
                }
            }
        }
        return;
    }
    if (auto* v = std::get_if<Valu>(&inst.fields)) {
        Bool e = exec(inst);
        auto lane = [&](Key k) {
            std::size_t at = born(Value{k.lo});
            Bool a = b.atom(Atom{Atom::Lane, k.lo}, at);
            Bool m = b.and_(e, a);
            masks.insert(k, m);
            active.insert(k.lo);
        };
        if ((name == "v_cndmask_b32" || name.find("_co_ci_") != std::string_view::npos) && v->src.size() > 2 &&
            v->src[2].kind == Operand::Scalar && v->src[2].a != NULL_CODE) {
            if (auto k = key(read, v->src[2].a)) {
                if (name == "v_cndmask_b32") {
                    mask(*k);
                } else {
                    seen.push_back(*k);
                }
            }
        }
        if (name == "v_div_fmas_f32" || name == "v_div_fmas_f64") {
            if (auto k = key(read, ir::VCC)) {
                mask(*k);
            }
        }
        if (name.starts_with("v_cmpx")) {
            if (auto k = key(write, ir::EXEC)) {
                lane(*k);
            }
        } else {
            Opt compare = name.starts_with("v_cmp") ? v->dst : std::nullopt;
            const Opt* ops[2] = {&compare, &v->carry};
            for (const Opt* op : ops) {
                if (*op && (*op)->kind == Operand::Scalar) {
                    if (auto k = key(write, (*op)->a)) {
                        lane(*k);
                    }
                }
            }
        }
    }
}

std::optional<bool> Lanes::wave_kind(const ir::Inst& inst) const {
    std::string_view n = inst.name();
    if (n == "s_cbranch_execnz" || n == "s_cbranch_vccnz") {
        return true;
    }
    if (n == "s_cbranch_execz" || n == "s_cbranch_vccz") {
        return false;
    }
    if (n == "s_cbranch_scc0" || n == "s_cbranch_scc1") {
        auto scc = inst.read(Reg::scc());
        if (!scc || !scc_lane.contains(scc->id)) {
            return std::nullopt;
        }
        bool every = scc_every.contains(scc->id);
        return (n == "s_cbranch_scc1") != every;
    }
    return std::nullopt;
}

Bool Lanes::branch(const ir::Inst& inst) {
    Bool e = exec(inst);
    std::string_view n = inst.name();
    if (n == "s_cbranch_execz") {
        return b.not_(e);
    }
    if (n == "s_cbranch_execnz") {
        return e;
    }
    if (n == "s_cbranch_vccz" || n == "s_cbranch_vccnz") {
        auto k = key([&](Reg r) { return inst.read(r); }, ir::VCC);
        Bool vcc = k ? mask(*k).first : FALSE_;
        Bool any = b.and_(vcc, e);
        return n.ends_with("nz") ? any : b.not_(any);
    }
    auto scc = inst.read(Reg::scc());
    if (!scc) {
        return FALSE_;
    }
    Bool set;
    if (const Bool* m = scc_lane.find(scc->id)) {
        set = *m;
    } else {
        std::size_t at = born(*scc);
        set = b.atom(Atom{Atom::Uniform, scc->id}, at);
    }
    return n == "s_cbranch_scc1" ? set : b.not_(set);
}

std::vector<Bool> Lanes::paths() {
    std::size_t n = p.blocks.size();
    std::vector<Bool> cond(n, FALSE_);
    if (!order.empty()) {
        cond[order[0]] = TRUE_;
    }
    for (std::size_t round = 0; round < MAX_ROUNDS; ++round) {
        bool changed = false;
        for (std::size_t i = 1; i < order.size(); ++i) {
            std::size_t s = order[i];
            Bool c = FALSE_;
            for (std::size_t k = 0; k < incoming[s].size(); ++k) {
                auto [q, taken] = incoming[s][k];
                Bool e = edge(cond[q], q, taken);
                if (i <= index[q]) {
                    FlatMap<std::uint32_t, Bool> memo;
                    e = b.forget(e, i, memo);
                }
                c = b.or_(c, e);
            }
            if (c != cond[s]) {
                cond[s] = c;
                changed = true;
            }
        }
        if (b.overflow) {
            too_large();
        }
        if (!changed) {
            return cond;
        }
    }
    isa(0, "lane conditions do not settle");
}

}   

Structured structure(const ir::Program& program) { return structure_wide(program, {}); }

Structured structure_wide(const ir::Program& program, const std::unordered_set<std::size_t>& wide) {
    Lanes l(program);
    l.wide = wide;
    for (;;) {
        auto execs = l.masks_in_order();
        auto cond = l.paths();
        auto [wrong, new_derived] = l.check_joins(cond);
        if (l.b.overflow) {
            too_large();
        }
        if (wrong.empty() && new_derived.empty()) {
            return l.finish(execs, cond);
        }
        for (Value v : wrong) {
            l.unresolved.insert(v.id);
        }
        for (Value v : new_derived) {
            l.derived_phis.insert(v.id);
        }
        l.reset();
    }
}

}   
