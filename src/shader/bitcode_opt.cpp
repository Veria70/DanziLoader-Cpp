// SPDX-License-Identifier: GPL-3.0-or-later
// Clean-up of a finished module before it is written (`emit/bitcode/opt.rs`;
// DanziLoader / victory-perf, see NOTICE.md): dead code, constants and
// identities, one instruction for one value, what a branch taken says about
// its condition, one read for one place within a block. Every pass is exact.
// The order of every walk and of every constant made is the reference's,
// so the module written is the same.
#include <algorithm>
#include <cstring>
#include <bit>
#include <unordered_map>

#include "bitcode.hpp"

namespace danzi::shader::bitcode {

namespace {

std::uint64_t mask(std::uint32_t bits) { return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1; }


std::int64_t signed_of(std::uint32_t bits, std::uint64_t x) {
    if (bits < 64 && (x >> (bits - 1) & 1) != 0) {
        return static_cast<std::int64_t>(x | ~mask(bits));
    }
    return static_cast<std::int64_t>(x);
}


std::uint8_t opposite(std::uint8_t p) {
    if (p <= 15) {
        return static_cast<std::uint8_t>(15 - p);
    }
    switch (p) {
        case pred::EQ: return pred::NE;
        case pred::NE: return pred::EQ;
        case pred::IUGT: return pred::IULE;
        case pred::IULE: return pred::IUGT;
        case pred::IUGE: return pred::IULT;
        case pred::IULT: return pred::IUGE;
        case pred::SGT: return pred::SLE;
        case pred::SLE: return pred::SGT;
        case pred::SGE: return pred::SLT;
        default: return pred::SGE;
    }
}

V resolve(const std::vector<std::optional<V>>& repl, V v) {
    while (v.is(V::Inst)) {
        const auto& r = repl[v.index()];
        if (!r) {
            break;
        }
        v = *r;
    }
    return v;
}


struct CmpKey {
    std::uint32_t p;
    V a, b;
    bool operator==(const CmpKey&) const = default;
};
struct CmpHash {
    std::size_t operator()(const CmpKey& k) const {
        std::uint64_t h = (std::uint64_t{k.a.raw} << 32 | k.b.raw) * 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(h ^ (h >> 29) ^ k.p);
    }
};
CmpKey cmp_key(std::uint8_t p, V a, V b) { return CmpKey{p, a, b}; }

}   


static Effect effect_of(std::string_view name, std::optional<Attr> attr) {
    if (name.starts_with("dx.op.")) {
        name.remove_prefix(6);
    }
    if (attr == Attr::ReadNone) {
        return Effect::Pure;
    }
    if (attr == Attr::ReadOnly) {
        if (name.starts_with("cbufferLoadLegacy") || name.starts_with("getDimensions")) {
            return Effect::Pure;
        }
        if (name.starts_with("wave")) {
            return Effect::Lanes;
        }
        return Effect::Reads;
    }
    if (name.starts_with("wave") || name.starts_with("quad")) {
        return Effect::Lanes;
    }
    return Effect::Writes;
}

struct Module::Tree {
    std::vector<std::size_t> order;
    std::vector<std::vector<std::size_t>> children;
    std::vector<std::vector<std::size_t>> preds;
};

struct Module::Facts {
    std::unordered_map<std::uint32_t, bool> bits;
    std::unordered_map<CmpKey, bool, CmpHash> compares;
};

struct Module::Learned {
    std::vector<V> bits;
    std::vector<CmpKey> compares;
};


struct Module::Key {
    static constexpr std::uint32_t N = 10;
    std::uint32_t n = 0;
    std::uint32_t small[N];
    std::vector<std::uint32_t> big;

    void clear() {
        n = 0;
        big.clear();
    }
    void push(std::uint32_t w) {
        if (n < N) {
            small[n] = w;
        } else {
            if (n == N) {
                big.assign(small, small + N);
            }
            big.push_back(w);
        }
        ++n;
    }
    void set(std::initializer_list<std::uint32_t> ws) {
        clear();
        for (std::uint32_t w : ws) {
            push(w);
        }
    }
    const std::uint32_t* data() const { return n <= N ? small : big.data(); }
    bool operator==(const Key& o) const { return n == o.n && std::memcmp(data(), o.data(), n * sizeof(std::uint32_t)) == 0; }
};
struct Module::KeyHash {
    std::size_t operator()(const Key& k) const {
        std::uint64_t h = 0;
        const std::uint32_t* d = k.data();
        for (std::uint32_t i = 0; i < k.n; ++i) {
            h = (std::rotl(h, 5) ^ d[i]) * 0x517cc1b727220a95ull;
        }
        return static_cast<std::size_t>(h);
    }
};


class Module::KeyTable {
public:
    std::size_t size() const { return entries_.size(); }
    const V* find(const Key& k) const {
        if (slots_.empty()) {
            return nullptr;
        }
        std::uint64_t h = hash(k);
        std::size_t mask = slots_.size() - 1;
        for (std::size_t i = index(h);; i = (i + 1) & mask) {
            std::uint32_t e = slots_[i];
            if (e == 0) {
                return nullptr;
            }
            const Entry& x = entries_[e - 1];
            if (x.hash == h && x.key == k) {
                return &x.v;
            }
        }
    }

    void insert(const Key& k, V v) {
        if ((entries_.size() + 1) * 2 > slots_.size()) {
            grow();
        }
        std::uint64_t h = hash(k);
        entries_.push_back(Entry{k, v, h, 0});
        place(entries_.size() - 1);
    }

    void truncate(std::size_t n) {
        while (entries_.size() > n) {
            slots_[entries_.back().slot] = 0;
            entries_.pop_back();
        }
    }

private:
    struct Entry {
        Key key;
        V v;
        std::uint64_t hash;
        std::size_t slot;
    };
    std::vector<std::uint32_t> slots_;
    std::vector<Entry> entries_;
    unsigned shift_ = 64;

    static std::uint64_t hash(const Key& k) { return static_cast<std::uint64_t>(KeyHash{}(k)) * 0x9E3779B97F4A7C15ull; }
    std::size_t index(std::uint64_t h) const { return static_cast<std::size_t>(h >> shift_); }
    void place(std::size_t e) {
        std::size_t mask = slots_.size() - 1;
        std::size_t i = index(entries_[e].hash);
        while (slots_[i] != 0) {
            i = (i + 1) & mask;
        }
        slots_[i] = static_cast<std::uint32_t>(e + 1);
        entries_[e].slot = i;
    }
    void grow() {
        std::size_t n = slots_.empty() ? 64 : slots_.size() * 2;
        slots_.assign(n, 0);
        shift_ = static_cast<unsigned>(64 - std::countr_zero(n));
        for (std::size_t e = 0; e < entries_.size(); ++e) {
            place(e);
        }
    }
};

std::vector<Effect> Module::effects() const {
    std::vector<Effect> out;
    out.reserve(funcs_.size());
    for (const Func& f : funcs_) {
        out.push_back(effect_of(f.name, f.attr));
    }
    return out;
}

std::vector<bool> Module::reached() const {
    std::vector<bool> seen(blocks_.size(), false);
    if (blocks_.empty()) {
        return seen;
    }
    std::vector<std::size_t> stack{0};
    seen[0] = true;
    while (!stack.empty()) {
        std::size_t b = stack.back();
        stack.pop_back();
        for (std::size_t s : successors_of(blocks_[b].term)) {
            if (!seen[s]) {
                seen[s] = true;
                stack.push_back(s);
            }
        }
    }
    return seen;
}

void Module::optimise(std::uint32_t passes) {
    if (passes & (opt::FOLD | opt::REUSE | opt::FACTS | opt::LOADS)) {
        for (int round = 0; round < 4; ++round) {
            bool changed = merge(passes);
            if (passes & opt::FOLD) {
                changed |= prune();
            }
            if (!changed) {
                break;
            }
        }
    }
    if (passes & opt::DEAD) {
        drop_dead();
    }
}

Module::Tree Module::dominator_tree() const {
    std::size_t n = blocks_.size();
    std::vector<bool> seen(n, false);
    std::vector<std::size_t> post;
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, 0}};
    seen[0] = true;
    while (!stack.empty()) {
        auto& top = stack.back();
        auto [b, k] = top;
        ++top.second;
        Succs s = successors_of(blocks_[b].term);
        if (k < s.len) {
            if (!seen[s.to[k]]) {
                seen[s.to[k]] = true;
                stack.emplace_back(s.to[k], 0);
            }
        } else {
            post.push_back(b);
            stack.pop_back();
        }
    }
    std::reverse(post.begin(), post.end());
    Tree t;
    t.order = std::move(post);
    std::vector<std::size_t> number(n, SIZE_MAX);
    for (std::size_t k = 0; k < t.order.size(); ++k) {
        number[t.order[k]] = k;
    }
    t.preds.assign(n, {});
    for (std::size_t b : t.order) {
        for (std::size_t s : successors_of(blocks_[b].term)) {
            t.preds[s].push_back(b);
        }
    }
    std::vector<std::size_t> idom(n, SIZE_MAX);
    idom[0] = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t k = 1; k < t.order.size(); ++k) {
            std::size_t b = t.order[k];
            std::size_t now = SIZE_MAX;
            for (std::size_t p : t.preds[b]) {
                if (idom[p] == SIZE_MAX) {
                    continue;
                }
                if (now == SIZE_MAX) {
                    now = p;
                    continue;
                }
                std::size_t x = p, y = now;
                while (x != y) {
                    while (number[x] > number[y]) {
                        x = idom[x];
                    }
                    while (number[y] > number[x]) {
                        y = idom[y];
                    }
                }
                now = x;
            }
            if (idom[b] != now) {
                idom[b] = now;
                changed = true;
            }
        }
    }
    t.children.assign(n, {});
    for (std::size_t k = 1; k < t.order.size(); ++k) {
        t.children[idom[t.order[k]]].push_back(t.order[k]);
    }
    return t;
}

void Module::learn(V v, bool holds, Facts& facts, Learned& learned, std::uint32_t depth) const {
    if (!v.is(V::Inst)) {
        return;
    }
    if (facts.bits.contains(v.raw)) {
        return;
    }
    facts.bits.emplace(v.raw, holds);
    learned.bits.push_back(v);
    if (depth == 0) {
        return;
    }
    const Inst& x = insts_[v.index()];
    if (x.op == OpKind::Bin && x.code == static_cast<std::uint8_t>(Bin::And) && holds) {
        learn(x.a, true, facts, learned, depth - 1);
        learn(x.b, true, facts, learned, depth - 1);
    } else if (x.op == OpKind::Bin && x.code == static_cast<std::uint8_t>(Bin::Or) && !holds) {
        learn(x.a, false, facts, learned, depth - 1);
        learn(x.b, false, facts, learned, depth - 1);
    } else if (x.op == OpKind::Bin && x.code == static_cast<std::uint8_t>(Bin::Xor)) {
        auto ka = int_k(x.a), kb = int_k(x.b);
        V y;
        std::uint64_t k;
        if (!ka && kb && kb->first == 1) {
            y = x.a;
            k = kb->second;
        } else if (ka && ka->first == 1 && !kb) {
            y = x.b;
            k = ka->second;
        } else {
            return;
        }
        learn(y, holds != (k != 0), facts, learned, depth - 1);
    } else if (x.op == OpKind::Cmp) {
        std::uint8_t p = opposite(x.code);
        CmpKey key = cmp_key(p, x.a, x.b);
        if (!facts.compares.contains(key)) {
            facts.compares.emplace(key, !holds);
            learned.compares.push_back(key);
        }
    }
}

std::optional<std::uint32_t> Module::int_bits(Ty ty) const {
    const TypeDef& d = types_[ty.id];
    if (d.kind == TypeDef::Int) {
        return d.a;
    }
    return std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::uint64_t>> Module::int_k(V v) const {
    if (!v.is(V::Const)) {
        return std::nullopt;
    }
    const auto& [ty, c] = consts_[v.index()];
    if (c.kind != Const::Int) {
        return std::nullopt;
    }
    auto bits = int_bits(ty);
    if (!bits) {
        return std::nullopt;
    }
    return std::pair{*bits, static_cast<std::uint64_t>(c.value) & mask(*bits)};
}

V Module::k(Ty ty, std::uint32_t bits, std::uint64_t x) { return int_const(ty, signed_of(bits, x & mask(bits))); }

std::optional<V> Module::simplify(std::uint32_t i, bool& changed) {
    Ty ty = insts_[i].ty;
    const Inst& x = insts_[i];
    switch (x.op) {
        case OpKind::Bin: {
            auto bits_o = int_bits(ty);
            if (!bits_o) {
                return std::nullopt;
            }
            std::uint32_t bits = *bits_o;
            auto ka_o = int_k(x.a), kb_o = int_k(x.b);
            std::optional<std::uint64_t> ka, kb;
            if (ka_o) {
                ka = ka_o->second;
            }
            if (kb_o) {
                kb = kb_o->second;
            }
            std::uint64_t all = mask(bits);
            auto op = static_cast<Bin>(x.code);
            if (ka && kb) {
                std::uint64_t a = *ka, b = *kb;
                std::optional<std::uint32_t> amount;
                if (b < bits) {
                    amount = static_cast<std::uint32_t>(b);
                }
                std::uint64_t r;
                switch (op) {
                    case Bin::Add: r = a + b; break;
                    case Bin::Sub: r = a - b; break;
                    case Bin::Mul: r = a * b; break;
                    case Bin::And: r = a & b; break;
                    case Bin::Or: r = a | b; break;
                    case Bin::Xor: r = a ^ b; break;
                    case Bin::Shl:
                        if (!amount) {
                            return std::nullopt;
                        }
                        r = a << *amount;
                        break;
                    case Bin::LShr:
                        if (!amount) {
                            return std::nullopt;
                        }
                        r = a >> *amount;
                        break;
                    case Bin::AShr:
                        if (!amount) {
                            return std::nullopt;
                        }
                        r = static_cast<std::uint64_t>(signed_of(bits, a) >> *amount);
                        break;
                    case Bin::UDiv:
                        if (b == 0) {
                            return std::nullopt;
                        }
                        r = a / b;
                        break;
                    case Bin::URem:
                        if (b == 0) {
                            return std::nullopt;
                        }
                        r = a % b;
                        break;
                    default: return std::nullopt;
                }
                return k(ty, bits, r);
            }
            auto is = [](const std::optional<std::uint64_t>& o, std::uint64_t v) { return o && *o == v; };
            switch (op) {
                case Bin::Add:
                case Bin::Sub:
                case Bin::Or:
                case Bin::Xor:
                case Bin::Shl:
                case Bin::LShr:
                case Bin::AShr:
                    if (is(kb, 0)) {
                        return x.a;
                    }
                    break;
                default: break;
            }
            if ((op == Bin::Add || op == Bin::Or || op == Bin::Xor) && is(ka, 0)) {
                return x.b;
            }
            if ((op == Bin::Mul || op == Bin::UDiv) && is(kb, 1)) {
                return x.a;
            }
            if (op == Bin::Mul && is(ka, 1)) {
                return x.b;
            }
            if ((op == Bin::Mul || op == Bin::And) && is(kb, 0)) {
                return x.b;
            }
            if ((op == Bin::Mul || op == Bin::And) && is(ka, 0)) {
                return x.a;
            }
            if (op == Bin::And && is(kb, all)) {
                return x.a;
            }
            if (op == Bin::And && is(ka, all)) {
                return x.b;
            }
            if (op == Bin::Or && is(kb, all)) {
                return x.b;
            }
            if (op == Bin::Or && is(ka, all)) {
                return x.a;
            }
            if ((op == Bin::And || op == Bin::Or) && x.a == x.b) {
                return x.a;
            }
            return std::nullopt;
        }
        case OpKind::Cmp: {
            std::uint8_t p = x.code;
            if (p <= pred::TRUE_) {
                if (!x.a.is(V::Const) || !x.b.is(V::Const)) {
                    return std::nullopt;
                }
                const Const& cx = consts_[x.a.index()].second;
                const Const& cy = consts_[x.b.index()].second;
                if (cx.kind != Const::Float || cy.kind != Const::Float) {
                    return std::nullopt;
                }
                float fx = std::bit_cast<float>(static_cast<std::uint32_t>(cx.value));
                float fy = std::bit_cast<float>(static_cast<std::uint32_t>(cy.value));
                bool unordered = fx != fx || fy != fy;
                bool holds = unordered ? (p & 8) != 0 : (((p & 1) && fx == fy) || ((p & 2) && fx > fy) || ((p & 4) && fx < fy));
                return boolean(holds);
            }
            if (p < pred::EQ || p > pred::SLE) {
                return std::nullopt;
            }
            auto holds = [&](std::uint64_t a, std::uint64_t b, std::uint32_t bits) {
                std::int64_t sa = signed_of(bits, a), sb = signed_of(bits, b);
                switch (p) {
                    case pred::EQ: return a == b;
                    case pred::NE: return a != b;
                    case pred::IUGT: return a > b;
                    case pred::IUGE: return a >= b;
                    case pred::IULT: return a < b;
                    case pred::IULE: return a <= b;
                    case pred::SGT: return sa > sb;
                    case pred::SGE: return sa >= sb;
                    case pred::SLT: return sa < sb;
                    default: return sa <= sb;
                }
            };
            auto ka = int_k(x.a), kb = int_k(x.b);
            if (ka && kb) {
                return boolean(holds(ka->second, kb->second, ka->first));
            }
            if (x.a == x.b) {
                return boolean(holds(0, 0, 32));
            }
            if (p == pred::NE && kb && kb->second == 0 && x.a.is(V::Inst)) {
                const Inst& z = insts_[x.a.index()];
                if (z.op == OpKind::Cast && z.code == static_cast<std::uint8_t>(Cast::ZExt) && type_of(z.a) == ty) {
                    return z.a;
                }
            }
            return std::nullopt;
        }
        case OpKind::Select: {
            V c = x.a, t = x.b, f = x.c;
            if (auto kc = int_k(c)) {
                return kc->second != 0 ? t : f;
            }
            bool inner = false;
            while (t.is(V::Inst)) {
                const Inst& y = insts_[t.index()];
                if (y.op != OpKind::Select || y.a != c) {
                    break;
                }
                t = y.b;
                inner = true;
            }
            while (f.is(V::Inst)) {
                const Inst& y = insts_[f.index()];
                if (y.op != OpKind::Select || y.a != c) {
                    break;
                }
                f = y.c;
                inner = true;
            }
            if (t == f) {
                return t;
            }
            auto kt = int_k(t), kf = int_k(f);
            if (int_bits(ty) == std::optional<std::uint32_t>(1) && kt && kt->second == 1 && kf && kf->second == 0) {
                return c;
            }
            if (inner) {
                insts_[i].b = t;
                insts_[i].c = f;
                changed = true;
            }
            return std::nullopt;
        }
        case OpKind::Cast: {
            Ty from = type_of(x.a);
            std::optional<std::pair<std::uint8_t, V>> through;
            if (x.a.is(V::Inst) && insts_[x.a.index()].op == OpKind::Cast) {
                through = std::pair{insts_[x.a.index()].code, insts_[x.a.index()].a};
            }
            auto op = static_cast<Cast>(x.code);
            if (op == Cast::Bitcast) {
                if (from == ty) {
                    return x.a;
                }
                if (through && through->first == static_cast<std::uint8_t>(Cast::Bitcast) && type_of(through->second) == ty) {
                    return through->second;
                }
                if (!x.a.is(V::Const)) {
                    return std::nullopt;
                }
                bool to_float = types_[ty.id].kind == TypeDef::Float;
                const Const& c = consts_[x.a.index()].second;
                if (c.kind == Const::Int && to_float && int_bits(from) == std::optional<std::uint32_t>(32)) {
                    return f32(static_cast<std::uint32_t>(c.value));
                }
                if (c.kind == Const::Float && int_bits(ty) == std::optional<std::uint32_t>(32)) {
                    return int_const(ty, static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(c.value))));
                }
                return std::nullopt;
            }
            if (op == Cast::ZExt || op == Cast::SExt || op == Cast::Trunc) {
                auto to = int_bits(ty);
                if (!to) {
                    return std::nullopt;
                }
                if (auto kx = int_k(x.a)) {
                    std::uint64_t v = op == Cast::SExt ? static_cast<std::uint64_t>(signed_of(kx->first, kx->second)) : kx->second;
                    return k(ty, *to, v);
                }
                if (op == Cast::Trunc && through &&
                    (through->first == static_cast<std::uint8_t>(Cast::ZExt) || through->first == static_cast<std::uint8_t>(Cast::SExt)) &&
                    type_of(through->second) == ty) {
                    return through->second;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }
        default: return std::nullopt;
    }
}

Effect Module::klass(const std::vector<Effect>& effects, std::uint32_t i) const {
    const Inst& x = insts_[i];
    switch (x.op) {
        case OpKind::Bin:
        case OpKind::Cast:
        case OpKind::Cmp:
        case OpKind::Select:
        case OpKind::Extract:
        case OpKind::Gep: return Effect::Pure;
        case OpKind::Load: {
            V base = x.a;
            if (x.a.is(V::Inst) && insts_[x.a.index()].op == OpKind::Gep) {
                base = insts_[x.a.index()].a;
            }
            if (base.is(V::Global) && globals_[base.index()].init) {
                return Effect::Pure;
            }
            return Effect::Reads;
        }
        case OpKind::Store:
        case OpKind::AtomicRmw:
        case OpKind::CmpXchg: return Effect::Writes;
        case OpKind::Phi: return Effect::Lanes;
        case OpKind::Call: return effects[x.x];
    }
    return Effect::Writes;
}

bool Module::key(std::uint32_t i, Key& out) const {
    const Inst& x = insts_[i];
    Key& w = out;
    switch (x.op) {
        case OpKind::Bin: {
            auto op = static_cast<Bin>(x.code);
            bool commutes = (op == Bin::Add || op == Bin::Mul || op == Bin::And || op == Bin::Or || op == Bin::Xor) && int_bits(x.ty);
            V a = x.a, b = x.b;
            if (commutes && b < a) {
                std::swap(a, b);
            }
            w.set({0, x.code, a.raw, b.raw});
            return true;
        }
        case OpKind::Cast: w.set({1, x.code, x.ty.id, x.a.raw}); return true;
        case OpKind::Cmp: w.set({2, x.code, x.a.raw, x.b.raw}); return true;
        case OpKind::Select: w.set({3, x.a.raw, x.b.raw, x.c.raw}); return true;
        case OpKind::Extract: w.set({4, x.a.raw, x.x}); return true;
        case OpKind::Call:
            w.set({5, x.x});
            for (V v : x.list) {
                w.push(v.raw);
            }
            return true;
        case OpKind::Gep:
            w.set({6, x.x, x.a.raw});
            for (V v : x.list) {
                w.push(v.raw);
            }
            return true;
        case OpKind::Load: w.set({7, x.a.raw}); return true;
        default: return false;
    }
}

bool Module::merge(std::uint32_t passes) {
    if (blocks_.empty()) {
        return false;
    }
    std::vector<Effect> fx = effects();
    Tree tree = dominator_tree();
    std::vector<std::optional<V>> repl(insts_.size());
    bool changed = false;

    if (passes & opt::FOLD) {
        bool again = true;
        while (again) {
            again = false;
            for (std::size_t b : tree.order) {
                for (std::uint32_t p : blocks_[b].phis) {
                    if (repl[p]) {
                        continue;
                    }
                    const Inst& x = insts_[p];
                    if (x.op != OpKind::Phi) {
                        continue;
                    }
                    std::optional<V> one;
                    bool same = true;
                    for (V v0 : x.list) {
                        V v = resolve(repl, v0);
                        if (v == V::inst(p)) {
                            continue;
                        }
                        if (!one) {
                            one = v;
                        } else if (*one != v) {
                            same = false;
                        }
                    }
                    if (same && one) {
                        repl[p] = one;
                        again = true;
                        changed = true;
                    }
                }
            }
        }
    }
    KeyTable known;
    KeyTable read;
    Facts facts;
    V yes = boolean(true), no = boolean(false);
    struct Step {
        bool enter;
        std::size_t block;


        std::size_t known_before;
        Learned learned;
    };
    std::vector<Step> steps;
    steps.push_back(Step{true, 0, 0, {}});
    Key key_buf;
    while (!steps.empty()) {
        Step step = std::move(steps.back());
        steps.pop_back();
        if (!step.enter) {
            known.truncate(step.known_before);
            for (V v : step.learned.bits) {
                facts.bits.erase(v.raw);
            }
            for (const CmpKey& k : step.learned.compares) {
                facts.compares.erase(k);
            }
            continue;
        }
        std::size_t b = step.block;
        std::size_t known_before = known.size();
        Learned learned;
        if ((passes & opt::FACTS) && tree.preds[b].size() == 1) {
            std::size_t p = tree.preds[b][0];
            const auto& t = blocks_[p].term;
            if (t && t->kind == Term::CondBr && t->t != t->f) {
                learn(resolve(repl, t->cond), b == t->t, facts, learned, 8);
            }
        }
        auto known_bit = [&](V v) {
            if (auto it = facts.bits.find(v.raw); it != facts.bits.end()) {
                return it->second ? yes : no;
            }
            return v;
        };
        read.truncate(0);

        if ((passes & opt::REUSE) && blocks_[b].phis.size() > 1) {
            std::unordered_map<Key, V, KeyHash> joins;
            for (std::uint32_t p : blocks_[b].phis) {
                if (repl[p]) {
                    continue;
                }
                const Inst& x = insts_[p];
                if (x.op != OpKind::Phi) {
                    continue;
                }
                std::vector<std::pair<std::size_t, V>> entries;
                for (std::size_t k = 0; k < x.list.size(); ++k) {
                    entries.emplace_back(x.from[k], resolve(repl, x.list[k]));
                }
                std::sort(entries.begin(), entries.end());
                Key key;
                for (const auto& [from, v] : entries) {
                    key.push(static_cast<std::uint32_t>(from));
                    key.push(static_cast<std::uint32_t>(from >> 32));
                    key.push(v.raw);
                }
                key.push(x.ty.id);
                if (auto it = joins.find(key); it != joins.end()) {
                    repl[p] = it->second;
                    changed = true;
                } else {
                    joins.emplace(std::move(key), V::inst(p));
                }
            }
        }
        std::vector<std::uint32_t> body = std::move(blocks_[b].body);
        blocks_[b].body.clear();
        std::vector<std::uint32_t> kept;
        kept.reserve(body.size());
        for (std::uint32_t i : body) {
            map_operands(insts_[i], [&](V v) {
                V r = resolve(repl, v);
                V bit = known_bit(r);
                changed |= bit != r;
                return bit;
            });
            const Inst& xi = insts_[i];
            if (xi.op == OpKind::Cmp) {
                if (auto it = facts.compares.find(cmp_key(xi.code, xi.a, xi.b)); it != facts.compares.end()) {
                    repl[i] = it->second ? yes : no;
                    changed = true;
                    continue;
                }
            }
            if (passes & opt::FOLD) {
                if (auto v = simplify(i, changed)) {
                    repl[i] = *v;
                    changed = true;
                    continue;
                }
            }
            Effect cls = klass(fx, i);
            if ((passes & opt::REUSE) && cls == Effect::Pure && key(i, key_buf)) {
                if (const V* hit = known.find(key_buf)) {
                    repl[i] = *hit;
                    changed = true;
                    continue;
                }
                known.insert(key_buf, V::inst(i));
            }
            if ((passes & opt::LOADS) && (cls == Effect::Reads || cls == Effect::Lanes) && key(i, key_buf)) {
                if (const V* hit = read.find(key_buf)) {
                    repl[i] = *hit;
                    changed = true;
                    continue;
                }
                read.insert(key_buf, V::inst(i));
            }
            if (cls == Effect::Writes) {
                read.truncate(0);
            }
            kept.push_back(i);
        }
        blocks_[b].body = std::move(kept);
        if (auto& t = blocks_[b].term; t && t->kind == Term::CondBr) {
            t->cond = known_bit(resolve(repl, t->cond));
        }
        steps.push_back(Step{false, b, known_before, std::move(learned)});
        const auto& ch = tree.children[b];
        for (auto it = ch.rbegin(); it != ch.rend(); ++it) {
            steps.push_back(Step{true, *it, 0, {}});
        }
    }
    if (!changed) {
        return false;
    }
    for (Block& blk : blocks_) {
        std::erase_if(blk.phis, [&](std::uint32_t p) { return repl[p].has_value(); });
        if (blk.term && blk.term->kind == Term::CondBr) {
            blk.term->cond = resolve(repl, blk.term->cond);
        }
    }
    for (Block& blk : blocks_) {
        for (std::uint32_t i : blk.phis) {
            map_operands(insts_[i], [&](V v) { return resolve(repl, v); });
        }
        for (std::uint32_t i : blk.body) {
            map_operands(insts_[i], [&](V v) { return resolve(repl, v); });
        }
    }
    return true;
}

bool Module::prune() {
    bool changed = false;
    auto drop_from = [&](std::uint32_t p, auto keep) {
        Inst& x = insts_[p];
        if (x.op != OpKind::Phi) {
            return;
        }
        std::size_t n = 0;
        for (std::size_t k = 0; k < x.list.size(); ++k) {
            if (keep(x.from[k])) {
                x.list[n] = x.list[k];
                x.from[n] = x.from[k];
                ++n;
            }
        }
        x.list.resize(n);
        x.from.resize(n);
    };
    for (std::size_t b = 0; b < blocks_.size(); ++b) {
        auto& t = blocks_[b].term;
        if (!t || t->kind != Term::CondBr || t->t == t->f) {
            continue;
        }
        auto kc = int_k(t->cond);
        if (!kc) {
            continue;
        }
        std::size_t taken = kc->second != 0 ? t->t : t->f;
        std::size_t not_taken = kc->second != 0 ? t->f : t->t;
        t = Term::br(taken);
        changed = true;
        for (std::size_t n = 0; n < blocks_[not_taken].phis.size(); ++n) {
            drop_from(blocks_[not_taken].phis[n], [&](std::size_t from) { return from != b; });
        }
    }
    if (!changed) {
        return false;
    }
    std::vector<bool> r = reached();
    for (std::size_t b = 0; b < blocks_.size(); ++b) {
        if (!r[b]) {
            continue;
        }
        for (std::uint32_t p : blocks_[b].phis) {
            drop_from(p, [&](std::size_t from) { return static_cast<bool>(r[from]); });
        }
    }
    return true;
}

bool Module::has_effect(const std::vector<Effect>& effects, std::uint32_t i) const {
    const Inst& x = insts_[i];
    switch (x.op) {
        case OpKind::Store:
        case OpKind::AtomicRmw:
        case OpKind::CmpXchg: return true;
        case OpKind::Call: return effects[x.x] == Effect::Writes;
        default: return false;
    }
}

void Module::drop_dead() {
    std::vector<Effect> fx = effects();
    std::vector<bool> r = reached();
    std::vector<bool> lv(insts_.size(), false);
    std::vector<std::uint32_t> work;
    auto mark = [&](V v) {
        if (v.is(V::Inst) && !lv[v.index()]) {
            lv[v.index()] = true;
            work.push_back(v.index());
        }
    };
    for (std::size_t b = 0; b < blocks_.size(); ++b) {
        if (!r[b]) {
            continue;
        }
        for (std::uint32_t i : blocks_[b].body) {
            if (has_effect(fx, i)) {
                mark(V::inst(i));
            }
        }
        if (blocks_[b].term && blocks_[b].term->kind == Term::CondBr) {
            mark(blocks_[b].term->cond);
        }
    }
    while (!work.empty()) {
        std::uint32_t i = work.back();
        work.pop_back();
        for_each_operand(insts_[i], mark);
    }
    for (std::size_t b = 0; b < blocks_.size(); ++b) {
        if (r[b]) {
            std::erase_if(blocks_[b].body, [&](std::uint32_t i) { return !lv[i]; });
            std::erase_if(blocks_[b].phis, [&](std::uint32_t i) { return !lv[i]; });
        } else {
            blocks_[b].body.clear();
            blocks_[b].phis.clear();
        }
    }
    drop_unused_functions();
    drop_unused_tables();
}

Usage Module::usage() const {
    Usage u;
    std::vector<bool> r = reached();
    auto wide = [&](Ty ty) { return types_[ty.id].kind == TypeDef::Int && types_[ty.id].a == 64; };
    auto annotation = [&](V h) -> std::optional<std::pair<std::int64_t, std::int64_t>> {
        if (!h.is(V::Inst)) {
            return std::nullopt;
        }
        const Inst& a = insts_[h.index()];
        if (a.op != OpKind::Call || funcs_[a.x].name != "dx.op.annotateHandle") {
            return std::nullopt;
        }
        if (a.list.size() <= 2 || !a.list[2].is(V::Const)) {
            return std::nullopt;
        }
        const Const& props = consts_[a.list[2].index()].second;
        if (props.kind != Const::Aggregate) {
            return std::nullopt;
        }
        const Const& f0 = consts_[props.fields[0]].second;
        const Const& f1 = consts_[props.fields[1]].second;
        if (f0.kind == Const::Int && f1.kind == Const::Int) {
            return std::pair{f0.value, f1.value};
        }
        return std::nullopt;
    };
    auto visit = [&](std::uint32_t i) {
        const Inst& x = insts_[i];
        u.int64 |= wide(x.ty);
        for_each_operand(x, [&](V v) { u.int64 |= wide(type_of(v)); });
        if (x.op != OpKind::Call) {
            return;
        }
        std::string_view name = funcs_[x.x].name;
        if (name.starts_with("dx.op.")) {
            name.remove_prefix(6);
        }
        if (name.starts_with("wave") || name.starts_with("quad")) {
            u.wave_ops = true;
        } else if (name == "createHandleFromHeap") {
            std::optional<std::pair<std::uint32_t, std::uint64_t>> s;
            if (x.list.size() > 2) {
                s = int_k(x.list[2]);
            }
            if (s && s->second == 0) {
                u.heap_views = true;
            } else {
                u.heap_samplers = true;
            }
        } else if (name.starts_with("atomic") && name.ends_with(".i64")) {
            u.atomic64 = true;
        } else if (name.starts_with("textureLoad") || name.starts_with("bufferLoad")) {
            std::optional<std::pair<std::int64_t, std::int64_t>> an;
            if (x.list.size() > 1) {
                an = annotation(x.list[1]);
            }
            u.typed_uav_loads |= !an || ((an->first & 0x1000) != 0 && (an->second >> 8 & 0xff) > 1);
        }
    };
    for (std::size_t b = 0; b < blocks_.size(); ++b) {
        if (!r[b]) {
            continue;
        }
        for (std::uint32_t i : blocks_[b].phis) {
            visit(i);
        }
        for (std::uint32_t i : blocks_[b].body) {
            visit(i);
        }
    }
    return u;
}

void Module::drop_unused_tables() {
    std::vector<bool> used;
    for (const Global& g : globals_) {
        used.push_back(!g.init.has_value());
    }
    auto all = [&] { return std::all_of(used.begin(), used.end(), [](bool b) { return b; }); };
    if (all()) {
        return;
    }
    auto note = [&](V v) {
        if (v.is(V::Global)) {
            used[v.index()] = true;
        }
    };
    for (const Block& blk : blocks_) {
        for (std::uint32_t i : blk.phis) {
            for_each_operand(insts_[i], note);
        }
        for (std::uint32_t i : blk.body) {
            for_each_operand(insts_[i], note);
        }
    }
    for (const MdDef& m : mds_) {
        if (m.kind == MdDef::Value && m.v.is(V::Global)) {
            used[m.v.index()] = true;
        }
    }
    if (all()) {
        return;
    }
    std::vector<std::uint32_t> now(used.size(), UINT32_MAX);
    std::uint32_t next = 0;
    for (std::size_t old = 0; old < used.size(); ++old) {
        if (used[old]) {
            now[old] = next++;
        }
    }
    std::vector<Global> kept;
    for (std::size_t old = 0; old < globals_.size(); ++old) {
        if (used[old]) {
            kept.push_back(std::move(globals_[old]));
        }
    }
    globals_ = std::move(kept);
    auto renumber = [&](V v) {
        if (v.is(V::Global) && used[v.index()]) {
            return V::global(now[v.index()]);
        }
        return v;
    };
    for (Inst& x : insts_) {
        map_operands(x, renumber);
    }
    for (MdDef& m : mds_) {
        if (m.kind == MdDef::Value) {
            m.v = renumber(m.v);
        }
    }
}

void Module::drop_unused_functions() {
    std::vector<bool> used;
    for (const Func& f : funcs_) {
        used.push_back(f.body);
    }
    for (const Block& blk : blocks_) {
        for (std::uint32_t i : blk.phis) {
            if (insts_[i].op == OpKind::Call) {
                used[insts_[i].x] = true;
            }
        }
        for (std::uint32_t i : blk.body) {
            if (insts_[i].op == OpKind::Call) {
                used[insts_[i].x] = true;
            }
        }
    }
    for (const MdDef& m : mds_) {
        if (m.kind == MdDef::Value && m.v.is(V::Func)) {
            used[m.v.index()] = true;
        }
    }
    if (std::all_of(used.begin(), used.end(), [](bool b) { return b; })) {
        return;
    }
    std::vector<std::uint32_t> now(used.size(), UINT32_MAX);
    std::uint32_t next = 0;
    for (std::size_t old = 0; old < used.size(); ++old) {
        if (used[old]) {
            now[old] = next++;
        }
    }
    std::vector<Func> kept;
    for (std::size_t old = 0; old < funcs_.size(); ++old) {
        if (used[old]) {
            kept.push_back(std::move(funcs_[old]));
        }
    }
    funcs_ = std::move(kept);
    func_ids_.clear();
    for (std::size_t i = 0; i < funcs_.size(); ++i) {
        func_ids_.emplace(funcs_[i].name, static_cast<std::uint32_t>(i));
    }
    for (Inst& x : insts_) {
        if (x.op == OpKind::Call && used[x.x]) {
            x.x = now[x.x];
        }
    }
    for (MdDef& m : mds_) {
        if (m.kind == MdDef::Value && m.v.is(V::Func)) {
            m.v = V::func(now[m.v.index()]);
        }
    }
}

}   
