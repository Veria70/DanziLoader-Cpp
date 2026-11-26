// SPDX-License-Identifier: GPL-3.0-or-later
// Wave ORs that only steer a loop over each lane's own bits
// (`emit/own_bits.rs`; DanziLoader / victory-perf, see NOTICE.md).
#include "own_bits.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

#include "lower.hpp"
#include "resource.hpp"
#include "wave.hpp"

namespace danzi::shader::own_bits {

using ir::Reg;
using ir::Value;
using structure::Atom;
using structure::Bool;

namespace {

using Why = const char*;
constexpr Why USED = "a wave OR used as data";
constexpr Why NO_LOOP = "a wave OR that steers no loop over its bits";
constexpr Why SHAPE = "a loop over a wave OR's bits of another shape";
constexpr Why TEST = "a loop over a wave OR's bits whose lanes do not test their own bit";
constexpr Why LEAVES = "a value of a round over a wave OR's bits that outlives the round";
constexpr Why EFFECT = "an effect in a loop over a wave OR's bits outside the lanes' own bits";
constexpr Why CROSS = "a cross-lane step in a loop over a wave OR's bits outside the lanes' own bits";
constexpr Why EXIT = "a loop over a wave OR's bits left another way";
constexpr Why STALE = "a lane's own-bit test used outside its round";
constexpr Why MASK = "a wave OR over a mask a running lane may be outside";

using Site = std::pair<std::size_t, std::size_t>;

struct User {
    bool phi;
    std::size_t b, i;
};

struct Arg {
    enum Kind : std::uint8_t { Val, K, Other } kind = Other;
    Value v{};
    std::uint32_t k = 0;
    bool operator==(const Arg& o) const {
        if (kind != o.kind) {
            return false;
        }
        return kind == Val ? v == o.v : kind == K ? k == o.k : true;
    }
    static Arg val(Value x) { return {Val, x, 0}; }
    static Arg konst(std::uint32_t x) { return {K, {}, x}; }
};

struct Fail {
    std::size_t offset;
    Why what;
};

std::vector<Value> used(const ir::Program& p, const structure::Structured& st, std::size_t b, std::size_t i) {
    const ir::Inst& inst = p.blocks[b].insts[i];
    auto ops = ir::operand_reads(p, inst);
    bool guarded = st.guards[b][i].has_value();
    std::vector<Value> out;
    for (const auto& [r, v] : inst.reads) {
        bool keep;
        if (r.kind == Reg::S && (r.n == ir::EXEC || r.n == ir::EXEC + 1)) {
            keep = false;
        } else if (r.kind == Reg::V) {
            keep = std::find(ops.begin(), ops.end(), r) != ops.end() || guarded;
        } else {
            keep = true;
        }
        if (keep) {
            out.push_back(v);
        }
    }
    return out;
}

std::optional<Value> value(const ir::Inst& inst, const Operand& op) {
    if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
        return inst.read(Reg::s(op.a));
    }
    if (op.kind == Operand::Vector) {
        return inst.read(Reg::v(op.a));
    }
    return std::nullopt;
}

std::vector<Arg> args(const ir::Inst& inst) {
    const Operand* src = nullptr;
    std::size_t n = 0;
    if (auto* f = std::get_if<SopFields>(&inst.fields)) {
        src = f->src.data();
        n = f->src.size();
    } else if (auto* v = std::get_if<Valu>(&inst.fields);
               v && !v->dpp && !v->sdwa && v->abs == 0 && v->neg == 0 && !v->clamp && v->omod == 0) {
        src = v->src.data();
        n = v->src.size();
    }
    std::vector<Arg> out;
    for (std::size_t k = 0; k < n; ++k) {
        const Operand& o = src[k];
        if (auto v = value(inst, o)) {
            out.push_back(Arg::val(*v));
        } else if (o.kind == Operand::Const) {
            auto c = resource::inline_constant(o.a);
            out.push_back(c ? Arg::konst(*c) : Arg{});
        } else if (o.kind == Operand::Literal) {
            out.push_back(Arg::konst(o.literal));
        } else {
            out.push_back(Arg{});
        }
    }
    return out;
}

std::optional<Arg> other(const ir::Inst& inst, Value a) {
    auto as = args(inst);
    if (as.size() != 2) {
        return std::nullopt;
    }
    if (as[0].kind == Arg::Val && as[0].v == a) {
        return as[1];
    }
    if (as[1].kind == Arg::Val && as[1].v == a) {
        return as[0];
    }
    return std::nullopt;
}

std::optional<Value> result(const ir::Inst& inst) {
    std::optional<Value> one;
    int count = 0;
    for (const auto& w : inst.writes) {
        if (w.first.kind == Reg::Scc) {
            continue;
        }
        if (count == 0) {
            one = w.second;
        }
        ++count;
        if (count > 1) {
            return std::nullopt;
        }
    }
    return count == 1 ? one : std::nullopt;
}

bool effect(const ir::Inst& inst) {
    auto name = inst.name();
    bool ds = std::holds_alternative<DsFields>(inst.fields);
    return std::holds_alternative<ExpFields>(inst.fields) ||
           (std::holds_alternative<FlatFields>(inst.fields) && !name.starts_with("global_load_")) || name.find("store") != std::string_view::npos ||
           name.find("atomic") != std::string_view::npos ||
           (ds && !(name.starts_with("ds_read") || name.starts_with("ds_swizzle") || name.find("permute") != std::string_view::npos)) ||
           name == "s_barrier" || name == "s_sendmsg" || name == "s_sendmsghalt";
}

bool is_cmp_zero_name(std::string_view n) { return n == "s_cmp_eq_u32" || n == "s_cmp_lg_u32" || n == "s_cmp_eq_i32" || n == "s_cmp_lg_i32"; }

struct Ctx {
    const ir::Program& p;
    const structure::Structured& st;
    std::unordered_map<Value, std::vector<User>, structure::ValueHash> users;
    std::unordered_map<Value, Site, structure::ValueHash> def;
    std::unordered_set<Value, structure::ValueHash> live;
    std::vector<std::size_t> idom;
    std::vector<std::unordered_map<std::size_t, wave::Section>> sections;

    Ctx(const ir::Program& prog, const structure::Structured& s) : p(prog), st(s) {
        for (std::size_t b = 0; b < p.blocks.size(); ++b) {
            const auto& block = p.blocks[b];
            for (std::size_t k = 0; k < block.phis.size(); ++k) {
                for (Value a : block.phis[k].args) {
                    users[a].push_back(User{true, b, k});
                }
            }
            for (std::size_t i = 0; i < block.insts.size(); ++i) {
                for (Value v : used(p, st, b, i)) {
                    users[v].push_back(User{false, b, i});
                    live.insert(v);
                }
                for (const auto& w : block.insts[i].writes) {
                    def[w.second] = Site{b, i};
                }
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& block : p.blocks) {
                for (const auto& phi : block.phis) {
                    if (live.contains(phi.value)) {
                        for (Value a : phi.args) {
                            changed |= live.insert(a).second;
                        }
                    }
                }
            }
        }
        auto order = ir::reverse_postorder(p.blocks);
        std::vector<std::size_t> index(p.blocks.size(), SIZE_MAX);
        for (std::size_t n = 0; n < order.size(); ++n) {
            index[order[n]] = n;
        }
        idom = ir::idoms(p.blocks, order, index);
        for (std::size_t b = 0; b < p.blocks.size(); ++b) {
            sections.push_back(wave::sections(p, b));
        }
    }

    const ir::Inst& inst(Site u) const { return p.blocks[u.first].insts[u.second]; }
    std::size_t at(Site u) const { return inst(u).machine.offset; }

    bool dominates(std::size_t a, std::size_t b) const {
        for (;;) {
            if (a == b) {
                return true;
            }
            std::size_t d = b < idom.size() ? idom[b] : SIZE_MAX;
            if (d == b || d == SIZE_MAX) {
                return false;
            }
            b = d;
        }
    }

    std::vector<Site> readers(Value v) const {
        std::vector<Site> out;
        if (auto it = users.find(v); it != users.end()) {
            for (const User& u : it->second) {
                if (!u.phi) {
                    out.emplace_back(u.b, u.i);
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    std::vector<Site> joins(Value v) const {
        std::vector<Site> out;
        if (auto it = users.find(v); it != users.end()) {
            for (const User& u : it->second) {
                if (u.phi && live.contains(p.blocks[u.b].phis[u.i].value)) {
                    out.emplace_back(u.b, u.i);
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    std::vector<bool> natural_loop(std::size_t h) const {
        std::vector<bool> inside(p.blocks.size(), false);
        std::vector<std::size_t> work;
        for (std::size_t q : p.blocks[h].preds) {
            if (dominates(h, q)) {
                work.push_back(q);
            }
        }
        if (work.empty()) {
            return inside;
        }
        inside[h] = true;
        while (!work.empty()) {
            std::size_t b = work.back();
            work.pop_back();
            if (inside[b]) {
                continue;
            }
            inside[b] = true;
            work.insert(work.end(), p.blocks[b].preds.begin(), p.blocks[b].preds.end());
        }
        return inside;
    }

    bool implied(Bool b, Atom a, std::unordered_map<std::uint32_t, bool>& memo) const {
        if (b == structure::FALSE_) {
            return true;
        }
        if (auto it = memo.find(b.id); it != memo.end()) {
            return it->second;
        }
        bool r;
        auto n = st.bools.node(b);
        if (!n) {
            r = false;
        } else if (n->atom == a) {
            r = n->lo == structure::FALSE_;
        } else {
            r = implied(n->lo, a, memo) && implied(n->hi, a, memo);
        }
        memo[b.id] = r;
        return r;
    }

    std::optional<std::tuple<Site, std::size_t, std::size_t>> branch(Value scc) const {
        auto rs = readers(scc);
        if (rs.size() != 1) {
            return std::nullopt;
        }
        Site u = rs[0];
        const auto& e = p.blocks[u.first].exit;
        if (e.kind != ir::Exit::Branch) {
            return std::nullopt;
        }
        if (u.second + 1 != p.blocks[u.first].insts.size()) {
            return std::nullopt;
        }
        auto n = inst(u).name();
        if (n == "s_cbranch_scc1") {
            return std::tuple{u, e.a, e.b};
        }
        if (n == "s_cbranch_scc0") {
            return std::tuple{u, e.b, e.a};
        }
        return std::nullopt;
    }


    bool site(std::size_t sb, Value t, Value x, Value mask, std::vector<Value>& alike, Fail& fail) const;
};

#define FAIL(off, why)          \
    do {                        \
        fail = Fail{(off), (why)}; \
        return false;           \
    } while (0)

bool Ctx::site(std::size_t sb, Value t, Value x, Value mask, std::vector<Value>& alike, Fail& fail) const {
    std::size_t n = p.blocks.size();
    std::size_t here = p.blocks[sb].offset;
    auto is_mask = [&](Value v) { return st.masks.contains(v) || st.highs.contains(v); };
    auto mit = st.masks.find(mask);
    if (mit == st.masks.end()) {
        FAIL(here, MASK);
    }
    Bool mb = mit->second;
    if (st.bools.meet({{st.reach[sb], true}, {mb, false}})) {
        FAIL(here, MASK);
    }
    std::unordered_set<Value, structure::ValueHash> rem{t};
    std::set<Site> pattern;
    alike.clear();
    auto copy = [&](Site u) -> std::optional<Value> {
        const ir::Inst& i = inst(u);
        auto nm = i.name();
        if ((nm == "v_mov_b32" || nm == "s_mov_b32") && args(i).size() == 1 && !st.guards[u.first][u.second]) {
            return result(i);
        }
        return std::nullopt;
    };
    std::vector<Value> init{t};
    std::optional<std::pair<Value, bool>> zero;
    for (Site u : readers(t)) {
        const ir::Inst& i = inst(u);
        auto name = i.name();
        auto o = other(i, t);
        std::optional<Value> scc;
        if (is_cmp_zero_name(name) && o && *o == Arg::konst(0) && !zero && (scc = i.write(Reg::scc()))) {
            zero = std::pair{*scc, name.find("_eq_") != std::string_view::npos};
            rem.insert(*scc);
        } else if (auto w = copy(u)) {
            init.push_back(*w);
            rem.insert(*w);
            if (i.writes[0].first.kind == Reg::V) {
                alike.push_back(*w);
            }
        } else {
            FAIL(at(u), USED);
        }
        pattern.insert(u);
    }
    std::optional<Site> join;
    for (Value v : init) {
        if (v != t) {
            auto rs = readers(v);
            if (!rs.empty()) {
                FAIL(at(rs[0]), USED);
            }
        }
        for (Site j : joins(v)) {
            if (join && *join != j) {
                FAIL(p.blocks[j.first].offset, USED);
            }
            join = j;
        }
    }
    if (!join) {
        FAIL(here, NO_LOOP);
    }
    auto [h, k] = *join;
    const ir::Phi& phi = p.blocks[h].phis[k];
    Value r = phi.value;
    rem.insert(r);
    std::size_t head = p.blocks[h].offset;
    auto lp = natural_loop(h);
    if (!lp[h] || lp[sb]) {
        FAIL(head, NO_LOOP);
    }
    Value cur;
    if (phi.reg.kind == Reg::V) {
        alike.push_back(r);
        auto rs = readers(r);
        if (rs.size() != 1) {
            FAIL(head, USED);
        }
        Site u = rs[0];
        const ir::Inst& i = inst(u);
        if (i.name() != "v_readfirstlane_b32" || args(i).size() != 1 || !lp[u.first]) {
            FAIL(at(u), USED);
        }
        pattern.insert(u);
        auto c = result(i);
        if (!c) {
            FAIL(at(u), SHAPE);
        }
        if (!joins(r).empty()) {
            FAIL(head, USED);
        }
        cur = *c;
    } else {
        cur = r;
    }
    rem.insert(cur);
    std::optional<Value> idx;
    std::optional<std::pair<Value, Value>> next;
    std::vector<std::pair<Value, bool>> more;
    std::vector<Value> bits;
    for (Site u : readers(cur)) {
        const ir::Inst& i = inst(u);
        auto name = i.name();
        if (!lp[u.first]) {
            FAIL(at(u), USED);
        }
        auto as = args(i);
        if (name == "s_ff1_i32_b32" && as.size() == 1 && as[0].kind == Arg::Val && !idx) {
            idx = result(i);
        } else if ((name == "s_xor_b32" || (name == "s_andn2_b32" && as.size() == 2 && as[0].kind == Arg::Val && as[1].kind == Arg::Val)) && !next) {
            Value bit;
            if (name == "s_andn2_b32" && as[0].v == cur && as[1].v != cur) {
                bit = as[1].v;
            } else if (name == "s_xor_b32") {
                auto o = other(i, cur);
                if (o && o->kind == Arg::Val && o->v != cur) {
                    bit = o->v;
                } else {
                    FAIL(at(u), USED);
                }
            } else {
                FAIL(at(u), USED);
            }
            auto w = result(i);
            if (!w) {
                FAIL(at(u), SHAPE);
            }
            next = std::pair{*w, bit};
            bits.push_back(bit);
            if (auto scc = i.write(Reg::scc())) {
                more.emplace_back(*scc, true);
                rem.insert(*scc);
            }
        } else if (is_cmp_zero_name(name)) {
            auto o = other(i, cur);
            auto scc = i.write(Reg::scc());
            if (!(o && o->kind == Arg::Val) || !scc) {
                FAIL(at(u), USED);
            }
            bits.push_back(o->v);
            more.emplace_back(*scc, name.find("_lg_") != std::string_view::npos);
            rem.insert(*scc);
        } else {
            FAIL(at(u), USED);
        }
        pattern.insert(u);
    }
    if (cur != r && !joins(cur).empty()) {
        FAIL(head, USED);
    }
    if (!idx || !next) {
        FAIL(head, SHAPE);
    }
    auto [nx, bit] = *next;
    for (Value b : bits) {
        if (b != bit) {
            FAIL(head, SHAPE);
        }
    }
    auto made = [&](Value v) -> std::optional<std::pair<Site, const ir::Inst*>> {
        auto it = def.find(v);
        if (it == def.end()) {
            return std::nullopt;
        }
        return std::pair{it->second, &inst(it->second)};
    };
    std::optional<Value> shifted;
    if (auto m = made(bit); m && m->second->name() == "s_lshl_b32" && lp[m->first.first]) {
        auto as = args(*m->second);
        if (as.size() == 2 && as[0] == Arg::konst(1) && as[1].kind == Arg::Val) {
            shifted = as[1].v;
        }
    }
    bool by_idx = false;
    if (shifted) {
        if (*shifted == *idx) {
            by_idx = true;
        } else if (auto m = made(*shifted); m && m->second->name() == "s_and_b32") {
            auto o = other(*m->second, *idx);
            by_idx = o && *o == Arg::konst(31);
        }
    }
    if (!by_idx) {
        FAIL(head, SHAPE);
    }
    rem.insert(nx);
    std::vector<Value> nexts{nx};
    for (Site u : readers(nx)) {
        const ir::Inst& i = inst(u);
        auto name = i.name();
        auto o = other(i, nx);
        std::optional<Value> scc;
        if (is_cmp_zero_name(name) && o && *o == Arg::konst(0) && (scc = i.write(Reg::scc()))) {
            more.emplace_back(*scc, name.find("_lg_") != std::string_view::npos);
            rem.insert(*scc);
        } else if (auto w = copy(u)) {
            nexts.push_back(*w);
            rem.insert(*w);
            if (i.writes[0].first.kind == Reg::V) {
                alike.push_back(*w);
            }
        } else {
            FAIL(at(u), USED);
        }
        pattern.insert(u);
    }
    for (Value v : nexts) {
        if (v != nx) {
            auto rs = readers(v);
            if (!rs.empty()) {
                FAIL(at(rs[0]), USED);
            }
        }
        for (Site j : joins(v)) {
            if (j != Site{h, k}) {
                FAIL(head, USED);
            }
        }
    }
    for (std::size_t q = 0; q < p.blocks[h].preds.size() && q < phi.args.size(); ++q) {
        std::size_t pb = p.blocks[h].preds[q];
        Value a = phi.args[q];
        bool ok = lp[pb] ? std::find(nexts.begin(), nexts.end(), a) != nexts.end() : std::find(init.begin(), init.end(), a) != init.end();
        if (!ok) {
            FAIL(head, SHAPE);
        }
    }
    std::unordered_set<std::size_t> leaves;
    for (const auto& [scc, set_while_more] : more) {
        if (!joins(scc).empty()) {
            FAIL(head, USED);
        }
        if (readers(scc).empty()) {
            continue;
        }
        auto br = branch(scc);
        if (!br) {
            FAIL(head, USED);
        }
        auto [u, set, clear] = *br;
        std::size_t on = set_while_more ? set : clear, out = set_while_more ? clear : set;
        if (!lp[u.first] || !lp[on] || lp[out]) {
            FAIL(at(u), SHAPE);
        }
        leaves.insert(u.first);
        pattern.insert(u);
    }
    if (leaves.empty()) {
        FAIL(head, SHAPE);
    }
    std::vector<bool> pre(n, false);
    if (zero) {
        auto [scc, set_when_empty] = *zero;
        if (!joins(scc).empty()) {
            FAIL(here, USED);
        }
        if (!readers(scc).empty()) {
            auto br = branch(scc);
            if (!br) {
                FAIL(here, USED);
            }
            auto [u, set, clear] = *br;
            std::size_t none = set_when_empty ? set : clear, some = set_when_empty ? clear : set;
            if (none == some || lp[u.first]) {
                FAIL(at(u), SHAPE);
            }
            if (some != h) {
                if (p.blocks[some].preds != std::vector<std::size_t>{u.first} || !dominates(some, h)) {
                    FAIL(at(u), SHAPE);
                }
                for (std::size_t b = 0; b < n; ++b) {
                    pre[b] = dominates(some, b) && !dominates(h, b);
                }
                for (std::size_t b = 0; b < n; ++b) {
                    if (!pre[b]) {
                        continue;
                    }
                    const auto& e = p.blocks[b].exit;
                    bool inside;
                    if (e.kind == ir::Exit::End || e.kind == ir::Exit::Indirect) {
                        inside = false;
                    } else {
                        inside = true;
                        for (std::size_t s : e.successor_list()) {
                            if (!(pre[s] || s == h)) {
                                inside = false;
                            }
                        }
                    }
                    if (!inside) {
                        FAIL(p.blocks[b].offset, SHAPE);
                    }
                }
            }
            pattern.insert(u);
        }
    }
    std::vector<std::tuple<Atom, std::size_t, std::size_t>> tests;
    for (Site u : readers(bit)) {
        const ir::Inst& i = inst(u);
        auto o = other(i, bit);
        if (i.name() != "v_and_b32" || !(o && *o == Arg::val(x)) || st.guards[u.first][u.second]) {
            continue;
        }
        auto y = result(i);
        if (!y) {
            continue;
        }
        for (Site c : readers(*y)) {
            const ir::Inst& ci = inst(c);
            auto cn = ci.name();
            bool cmp = cn == "v_cmp_ne_u32" || cn == "v_cmpx_ne_u32" || cn == "v_cmp_lg_u32" || cn == "v_cmpx_lg_u32" || cn == "v_cmp_ne_i32" ||
                       cn == "v_cmpx_ne_i32" || cn == "v_cmp_lg_i32" || cn == "v_cmpx_lg_i32";
            auto oc = other(ci, *y);
            if (cmp && oc && *oc == Arg::konst(0) && lp[c.first]) {
                for (const auto& w : ci.writes) {
                    tests.emplace_back(Atom{Atom::Lane, w.second.id}, c.first, c.second);
                }
            }
        }
    }
    if (tests.empty()) {
        FAIL(head, TEST);
    }
    std::vector<std::unordered_map<std::uint32_t, bool>> memo(tests.size());
    std::vector<bool> body_block(n, false);
    for (std::size_t b = 0; b < n; ++b) {
        if (!lp[b]) {
            continue;
        }
        for (std::size_t j = 0; j < tests.size(); ++j) {
            auto [a, tb, ti] = tests[j];
            if (b != tb && dominates(tb, b) && implied(st.reach[b], a, memo[j])) {
                body_block[b] = true;
                break;
            }
        }
    }
    auto body_inst = [&](std::size_t b, std::size_t i) {
        if (body_block[b]) {
            return true;
        }
        if (!lp[b]) {
            return false;
        }
        const auto& g = st.guards[b][i];
        if (!g) {
            return false;
        }
        for (std::size_t j = 0; j < tests.size(); ++j) {
            auto [a, tb, ti] = tests[j];
            if (((b == tb && i > ti) || (b != tb && dominates(tb, b))) && implied(*g, a, memo[j])) {
                return true;
            }
        }
        return false;
    };
    for (std::size_t b = 0; b < n; ++b) {
        if (!(lp[b] || pre[b])) {
            continue;
        }
        const auto& insts = p.blocks[b].insts;
        for (std::size_t i = 0; i < insts.size(); ++i) {
            const ir::Inst& in = insts[i];
            if (lp[b] && body_inst(b, i)) {
                continue;
            }
            Site u{b, i};
            for (const auto& [reg, w] : in.writes) {
                if (rem.contains(w) || is_mask(w) || (reg.kind == Reg::S && (reg.n == ir::EXEC || reg.n == ir::EXEC + 1))) {
                    continue;
                }
                if (auto it = users.find(w); it != users.end()) {
                    for (const User& us : it->second) {
                        bool stays = us.phi ? (body_block[us.b] || !live.contains(p.blocks[us.b].phis[us.i].value)) : (lp[us.b] || pre[us.b]);
                        if (!stays) {
                            FAIL(at(u), LEAVES);
                        }
                    }
                }
            }
            if (pattern.contains(u)) {
                continue;
            }
            if (effect(in)) {
                FAIL(at(u), EFFECT);
            }
            auto name = in.name();
            bool cross = name.starts_with("ds_swizzle") || name.find("permute") != std::string_view::npos || name.starts_with("v_mbcnt") ||
                         name.find("lane") != std::string_view::npos;
            if (!cross) {
                if (auto* v = std::get_if<Valu>(&in.fields); v && v->dpp) {
                    cross = true;
                }
            }
            if (!cross) {
                cross = lower::reads_mask_as_data(st, in);
            }
            if (!cross) {
                for (const auto& [s, sec] : sections[b]) {
                    if (i >= s && i <= sec.end) {
                        cross = true;
                        break;
                    }
                }
            }
            if (cross) {
                FAIL(at(u), CROSS);
            }
            for (Value v : used(p, st, b, i)) {
                if (rem.contains(v)) {
                    FAIL(at(u), USED);
                }
            }
        }
        if (lp[b] && !body_block[b]) {
            const auto& e = p.blocks[b].exit;
            bool ok;
            if (e.kind == ir::Exit::End || e.kind == ir::Exit::Indirect) {
                ok = false;
            } else {
                ok = leaves.contains(b);
                if (!ok) {
                    ok = true;
                    for (std::size_t s : e.successor_list()) {
                        if (!lp[s]) {
                            ok = false;
                        }
                    }
                }
            }
            if (!ok) {
                FAIL(p.blocks[b].offset, EXIT);
            }
        }
    }
    std::vector<Atom> dep;
    auto dep_has = [&](const Atom& a) { return std::find(dep.begin(), dep.end(), a) != dep.end(); };
    for (const auto& tst : tests) {
        if (!dep_has(std::get<0>(tst))) {
            dep.push_back(std::get<0>(tst));
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [id, edges] : st.phi_masks) {
            Atom pa{Atom::Phi, id.id};
            if (dep_has(pa)) {
                continue;
            }
            bool any = false;
            for (Bool e : edges) {
                for (const Atom& a : st.bools.support(e)) {
                    if (dep_has(a)) {
                        any = true;
                        break;
                    }
                }
                if (any) {
                    break;
                }
            }
            if (any) {
                dep.push_back(pa);
                changed = true;
            }
        }
    }
    std::unordered_map<std::uint32_t, bool> seen;
    auto tainted = [&](Bool b) {
        auto it = seen.find(b.id);
        if (it != seen.end()) {
            return it->second;
        }
        bool r = false;
        for (const Atom& a : st.bools.support(b)) {
            if (dep_has(a)) {
                r = true;
                break;
            }
        }
        seen[b.id] = r;
        return r;
    };
    for (std::size_t b = 0; b < n; ++b) {
        bool in_round = false;
        if (lp[b]) {
            for (const auto& tst : tests) {
                if (dominates(std::get<1>(tst), b)) {
                    in_round = true;
                    break;
                }
            }
        }
        if (in_round) {
            continue;
        }
        bool stale = false;
        for (const auto& g : st.guards[b]) {
            if (g && tainted(*g)) {
                stale = true;
                break;
            }
        }
        if (!stale && st.exits[b].kind == structure::LaneExit::Branch && tainted(st.exits[b].when)) {
            stale = true;
        }
        if (stale) {
            FAIL(p.blocks[b].offset, STALE);
        }
    }
    for (const auto& [v, m] : st.masks) {
        if (!tainted(m)) {
            continue;
        }
        if (auto it = users.find(v); it != users.end()) {
            for (const User& us : it->second) {
                bool outside = us.phi ? (!lp[us.b] && live.contains(p.blocks[us.b].phis[us.i].value)) : !lp[us.b];
                if (outside) {
                    FAIL(head, STALE);
                }
            }
        }
    }
    return true;
}

#undef FAIL

}   

OwnBits find(const ir::Program& p, const structure::Structured& st, bool why) {
    OwnBits out;
    std::optional<Ctx> ctx;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        auto secs = wave::sections(p, b);
        std::vector<std::size_t> starts;
        for (const auto& [s, _] : secs) {
            starts.push_back(s);
        }
        std::sort(starts.begin(), starts.end());
        for (std::size_t s : starts) {
            const wave::Section& sec = secs.at(s);
            std::vector<std::pair<Value, wave::Shape>> left(sec.shapes.begin(), sec.shapes.end());
            std::sort(left.begin(), left.end(), [](const auto& x, const auto& y) { return x.first.id < y.first.id; });
            for (const auto& [t, shape] : left) {
                if (!(shape.kind == wave::Shape::Active && shape.op == wave::Op::Or && shape.ident == 0)) {
                    continue;
                }
                if (!ctx) {
                    ctx.emplace(p, st);
                }
                auto it = ctx->users.find(t);
                bool only_inside = true;
                if (it != ctx->users.end()) {
                    for (const User& u : it->second) {
                        if (!(!u.phi && u.b == b && u.i > s && u.i < sec.end)) {
                            only_inside = false;
                            break;
                        }
                    }
                }
                if (only_inside) {
                    continue;
                }
                std::vector<Value> alike;
                Fail fail{};
                if (ctx->site(b, t, shape.a, shape.b, alike, fail)) {
                    out.totals.insert(t);
                    out.alike.insert(alike.begin(), alike.end());
                } else if (why) {
                    std::fprintf(stderr, "WAVE64-OWN-BITS the wave OR at %#zx: not a loop over the lanes' own bits at %#zx: %s\n",
                                 p.blocks[b].insts[s].machine.offset, fail.offset, fail.what);
                }
            }
        }
    }
    return out;
}

}   
