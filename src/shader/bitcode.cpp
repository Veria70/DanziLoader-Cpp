// SPDX-License-Identifier: GPL-3.0-or-later
// A DXIL module and its bitcode (`emit/bitcode.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See bitcode.hpp.
#include "bitcode.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace danzi::shader::bitcode {

namespace {

constexpr std::uint64_t MODULE_BLOCK = 8, PARAMATTR_BLOCK = 9, PARAMATTR_GROUP_BLOCK = 10, CONSTANTS_BLOCK = 11, FUNCTION_BLOCK = 12,
                        VALUE_SYMTAB_BLOCK = 14, METADATA_BLOCK = 15, TYPE_BLOCK = 17;


std::uint64_t signed_vbr(std::int64_t v) {
    if (v >= 0) {
        return static_cast<std::uint64_t>(v) << 1;
    }
    return (static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(v)) << 1 | 1;
}


class Writer {
public:
    void bytes(const char* b, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            emit(static_cast<std::uint8_t>(b[i]), 8);
        }
    }
    void enter(std::uint64_t id, std::uint32_t width) {
        emit(1, this->width());   
        vbr(id, 8);
        vbr(width, 4);
        align();
        words_.push_back(0);
        stack_.emplace_back(width, words_.size());
    }
    void exit() {
        emit(0, width());   
        align();
        auto [w, start] = stack_.back();
        stack_.pop_back();
        words_[start - 1] = static_cast<std::uint32_t>(words_.size() - start);
    }

    void record(std::uint64_t code, const std::uint64_t* ops, std::size_t n) {
        emit(3, width());
        vbr(code, 6);
        vbr(n, 6);
        for (std::size_t i = 0; i < n; ++i) {
            vbr(ops[i], 6);
        }
    }
    void record(std::uint64_t code, const std::vector<std::uint64_t>& ops) { record(code, ops.data(), ops.size()); }
    void record(std::uint64_t code, std::initializer_list<std::uint64_t> ops) { record(code, ops.begin(), ops.size()); }
    void chars(std::uint64_t code, std::string_view s, std::optional<std::uint64_t> first = std::nullopt) {
        emit(3, width());
        vbr(code, 6);
        vbr(s.size() + (first ? 1 : 0), 6);
        if (first) {
            vbr(*first, 6);
        }
        for (char c : s) {
            vbr(static_cast<std::uint8_t>(c), 6);
        }
    }
    std::vector<std::uint8_t> finish() {
        align();
        std::vector<std::uint8_t> out(words_.size() * 4);
        std::memcpy(out.data(), words_.data(), out.size());
        return out;
    }

private:
    std::vector<std::uint32_t> words_;
    std::uint64_t cur_ = 0;
    std::uint32_t bits_ = 0;
    std::vector<std::pair<std::uint32_t, std::size_t>> stack_;

    std::uint32_t width() const { return stack_.empty() ? 2 : stack_.back().first; }
    void emit(std::uint64_t v, std::uint32_t width) {
        cur_ |= v << bits_;
        bits_ += width;
        while (bits_ >= 32) {
            words_.push_back(static_cast<std::uint32_t>(cur_));
            cur_ >>= 32;
            bits_ -= 32;
        }
    }
    void vbr(std::uint64_t v, std::uint32_t width) {
        std::uint64_t hi = std::uint64_t{1} << (width - 1);
        while (v >= hi) {
            emit((v & (hi - 1)) | hi, width);
            v >>= width - 1;
        }
        emit(v, width);
    }
    void align() {
        if (bits_ > 0) {
            words_.push_back(static_cast<std::uint32_t>(cur_));
            cur_ = 0;
            bits_ = 0;
        }
    }
};

}   

Module::Succs Module::successors_of(const std::optional<Term>& t) {
    if (!t) {
        return {{0, 0}, 0};
    }
    switch (t->kind) {
        case Term::Br: return {{t->t, 0}, 1};
        case Term::CondBr: return {{t->t, t->f}, 2};
        default: return {{0, 0}, 0};
    }
}

std::size_t Module::TypeHash::operator()(const TypeDef& t) const {
    std::uint64_t h = (std::uint64_t{t.kind} << 56) ^ (std::uint64_t{t.a} << 24) ^ t.b;
    for (std::uint32_t x : t.list) {
        h = (std::rotl(h, 5) ^ x) * 0x517cc1b727220a95ull;
    }
    if (t.named) {
        h ^= std::hash<std::string>{}(t.name);
    }
    return static_cast<std::size_t>(h);
}

Ty Module::ty(TypeDef def) {
    if (auto it = type_ids_.find(def); it != type_ids_.end()) {
        return Ty{it->second};
    }
    auto t = static_cast<std::uint32_t>(types_.size());
    types_.push_back(def);
    type_ids_.emplace(std::move(def), t);
    return Ty{t};
}

Ty Module::void_() {
    if (!void_id_) {
        void_id_ = ty(TypeDef{TypeDef::Void}).id + 1;
    }
    return Ty{void_id_ - 1};
}
Ty Module::int_(std::uint32_t bits) {
    if (bits <= 64) {
        if (!int_ids_[bits]) {
            int_ids_[bits] = ty(TypeDef{TypeDef::Int, bits}).id + 1;
        }
        return Ty{int_ids_[bits] - 1};
    }
    return ty(TypeDef{TypeDef::Int, bits});
}
Ty Module::float_() {
    if (!float_id_) {
        float_id_ = ty(TypeDef{TypeDef::Float}).id + 1;
    }
    return Ty{float_id_ - 1};
}
Ty Module::double_() { return ty(TypeDef{TypeDef::Double}); }
Ty Module::ptr_in(Ty to, std::uint32_t space) { return ty(TypeDef{TypeDef::Ptr, to.id, space}); }
Ty Module::array(std::uint32_t n, Ty of) { return ty(TypeDef{TypeDef::Array, n, of.id}); }
Ty Module::named_struct(std::string_view name, std::vector<Ty> fields) {
    TypeDef d{TypeDef::Struct};
    for (Ty f : fields) {
        d.list.push_back(f.id);
    }
    d.named = true;
    d.name = name;
    return ty(std::move(d));
}
Ty Module::func_ty(Ty ret, std::vector<Ty> params) {
    TypeDef d{TypeDef::Func, ret.id};
    for (Ty p : params) {
        d.list.push_back(p.id);
    }
    return ty(std::move(d));
}

V Module::constant(Ty ty, Const c) {
    if (c.kind == Const::Aggregate) {
        std::string key(4 + 4 * c.fields.size(), '\0');
        std::memcpy(key.data(), &ty.id, 4);
        std::memcpy(key.data() + 4, c.fields.data(), 4 * c.fields.size());
        if (auto it = aggregate_ids_.find(key); it != aggregate_ids_.end()) {
            return V::konst(it->second);
        }
        auto i = static_cast<std::uint32_t>(consts_.size());
        consts_.emplace_back(ty, std::move(c));
        aggregate_ids_.emplace(std::move(key), i);
        return V::konst(i);
    }
    ConstKey key{ty.id, c.kind, static_cast<std::uint64_t>(c.value)};
    auto i = static_cast<std::uint32_t>(consts_.size());
    auto [slot, inserted] = scalar_ids_.try_emplace(key, i);
    if (!inserted) {
        return V::konst(*slot);
    }
    consts_.emplace_back(ty, std::move(c));
    return V::konst(i);
}

V Module::int_const(Ty ty, std::int64_t v) { return constant(ty, Const{Const::Int, v, {}}); }
V Module::f32(std::uint32_t bits) { return constant(float_(), Const{Const::Float, static_cast<std::int64_t>(bits), {}}); }
V Module::undef(Ty ty) { return constant(ty, Const{Const::Undef, 0, {}}); }
V Module::aggregate(Ty ty, const std::vector<V>& fields) {
    Const c{Const::Aggregate, 0, {}};
    for (V f : fields) {
        if (!f.is(V::Const)) {
            throw std::logic_error("aggregate of a non-constant");
        }
        c.fields.push_back(f.index());
    }
    return constant(ty, std::move(c));
}

std::optional<std::int64_t> Module::int_value(V v) const {
    if (!v.is(V::Const)) {
        return std::nullopt;
    }
    const Const& c = consts_[v.index()].second;
    if (c.kind != Const::Int) {
        return std::nullopt;
    }
    return c.value;
}

void Module::replace_const(V old, V now) {
    if (old.is(V::Const) && now.is(V::Const)) {
        consts_[old.index()] = consts_[now.index()];
    }
}

V Module::function(std::string_view name, Ty ty, std::optional<Attr> attr, bool body) {
    std::string n(name);
    if (auto it = func_ids_.find(n); it != func_ids_.end()) {
        return V::func(it->second);
    }
    auto i = static_cast<std::uint32_t>(funcs_.size());
    funcs_.push_back(Func{n, ty, attr, body});
    func_ids_.emplace(std::move(n), i);
    return V::func(i);
}

V Module::global(std::string_view name, Ty ty, std::uint32_t space) {
    Ty p = ptr_in(ty, space);
    globals_.push_back(Global{std::string(name), ty, p, space, std::nullopt});
    return V::global(static_cast<std::uint32_t>(globals_.size() - 1));
}

V Module::constant_global(std::string_view name, V init) {
    if (!init.is(V::Const)) {
        throw std::logic_error("a global initialised with a non-constant");
    }
    Ty t = consts_[init.index()].first;
    V g = global(name, t, 0);
    globals_[g.index()].init = init.index();
    return g;
}

Ty Module::type_of(V v) const {
    switch (v.kind()) {
        case V::Global: return globals_[v.index()].ptr;
        case V::Const: return consts_[v.index()].first;
        case V::Inst: return insts_[v.index()].ty;
        case V::Func: {
            Ty t = funcs_[v.index()].ty;
            auto it = type_ids_.find(TypeDef{TypeDef::Ptr, t.id, 0});
            if (it == type_ids_.end()) {
                throw std::logic_error("function pointer type");
            }
            return Ty{it->second};
        }
    }
    return {};
}

V Module::push(std::size_t block, Ty ty, Inst inst) {
    auto i = static_cast<std::uint32_t>(insts_.size());
    inst.ty = ty;
    insts_.push_back(std::move(inst));
    blocks_[block].body.push_back(i);
    return V::inst(i);
}

V Module::bin(std::size_t block, Bin op, V a, V b) {
    Inst x{};
    x.op = OpKind::Bin;
    x.code = static_cast<std::uint8_t>(op);
    x.a = a;
    x.b = b;
    return push(block, type_of(a), std::move(x));
}
V Module::cast(std::size_t block, Cast op, V v, Ty to) {
    Inst x{};
    x.op = OpKind::Cast;
    x.code = static_cast<std::uint8_t>(op);
    x.a = v;
    return push(block, to, std::move(x));
}
V Module::cmp(std::size_t block, std::uint8_t pred, V a, V b) {
    Ty t = int_(1);
    Inst x{};
    x.op = OpKind::Cmp;
    x.code = pred;
    x.a = a;
    x.b = b;
    return push(block, t, std::move(x));
}
V Module::select(std::size_t block, V c, V t, V f) {
    Inst x{};
    x.op = OpKind::Select;
    x.a = c;
    x.b = t;
    x.c = f;
    return push(block, type_of(t), std::move(x));
}
V Module::extract(std::size_t block, V agg, std::uint32_t index) {
    const TypeDef& d = types_[type_of(agg).id];
    Ty t;
    if (d.kind == TypeDef::Struct) {
        t = Ty{d.list[index]};
    } else if (d.kind == TypeDef::Array) {
        t = Ty{d.b};
    } else {
        throw std::logic_error("extractvalue from a non-aggregate");
    }
    Inst x{};
    x.op = OpKind::Extract;
    x.a = agg;
    x.x = index;
    return push(block, t, std::move(x));
}
V Module::call(std::size_t block, V f, std::vector<V> args) {
    if (!f.is(V::Func)) {
        throw std::logic_error("call of a non-function");
    }
    const TypeDef& d = types_[funcs_[f.index()].ty.id];
    Inst x{};
    x.op = OpKind::Call;
    x.x = f.index();
    x.list = std::move(args);
    return push(block, Ty{d.a}, std::move(x));
}
V Module::phi(std::size_t block, Ty ty) {
    auto i = static_cast<std::uint32_t>(insts_.size());
    Inst x{};
    x.ty = ty;
    x.op = OpKind::Phi;
    insts_.push_back(std::move(x));
    blocks_[block].phis.push_back(i);
    return V::inst(i);
}
V Module::element(std::size_t block, V p, V index) {
    const TypeDef& pd = types_[type_of(p).id];
    if (pd.kind != TypeDef::Ptr) {
        throw std::logic_error("element of a non-pointer");
    }
    std::uint32_t arr = pd.a, space = pd.b;
    const TypeDef& ad = types_[arr];
    if (ad.kind != TypeDef::Array) {
        throw std::logic_error("element of a non-array");
    }
    Ty elem{ad.b};
    Ty t = ptr_in(elem, space);
    V zero = i32(0);
    Inst x{};
    x.op = OpKind::Gep;
    x.x = arr;
    x.a = p;
    x.list = {zero, index};
    return push(block, t, std::move(x));
}
V Module::load(std::size_t block, V p) {
    const TypeDef& pd = types_[type_of(p).id];
    if (pd.kind != TypeDef::Ptr) {
        throw std::logic_error("load from a non-pointer");
    }
    Inst x{};
    x.op = OpKind::Load;
    x.a = p;
    return push(block, Ty{pd.a}, std::move(x));
}
void Module::store(std::size_t block, V p, V value) {
    Ty v = void_();
    Inst x{};
    x.op = OpKind::Store;
    x.a = p;
    x.b = value;
    push(block, v, std::move(x));
}
V Module::atomic(std::size_t block, Rmw op, V p, V value) {
    Inst x{};
    x.op = OpKind::AtomicRmw;
    x.code = static_cast<std::uint8_t>(op);
    x.a = p;
    x.b = value;
    return push(block, type_of(value), std::move(x));
}
V Module::cmpxchg(std::size_t block, V p, V expected, V now) {
    Ty i32t = type_of(now);
    Ty i1 = int_(1);
    TypeDef d{TypeDef::Struct};
    d.list = {i32t.id, i1.id};
    Ty pair = ty(std::move(d));
    Inst x{};
    x.op = OpKind::CmpXchg;
    x.a = p;
    x.b = expected;
    x.c = now;
    V r = push(block, pair, std::move(x));
    return extract(block, r, 0);
}

void Module::set_phi(V p, std::vector<std::pair<V, std::size_t>> incoming) {
    Inst& x = insts_[p.index()];
    x.list.clear();
    x.from.clear();
    for (const auto& [v, b] : incoming) {
        x.list.push_back(v);
        x.from.push_back(b);
    }
}

Md Module::md_str(std::string_view s) {
    MdDef d{MdDef::Str};
    d.str = s;
    mds_.push_back(std::move(d));
    return Md{static_cast<std::uint32_t>(mds_.size() - 1)};
}
Md Module::md_value(V v) {
    MdDef d{MdDef::Value};
    d.ty = type_of(v);
    d.v = v;
    mds_.push_back(std::move(d));
    return Md{static_cast<std::uint32_t>(mds_.size() - 1)};
}
Md Module::md_node(std::vector<std::optional<Md>> ops) {
    MdDef d{MdDef::Node};
    d.ops = std::move(ops);
    mds_.push_back(std::move(d));
    return Md{static_cast<std::uint32_t>(mds_.size() - 1)};
}
void Module::named_md(std::string_view name, std::vector<Md> nodes) { named_.emplace_back(std::string(name), std::move(nodes)); }


std::vector<bool> Module::live() const {
    std::vector<bool> live(insts_.size(), false);
    std::vector<std::uint32_t> work;
    auto mark = [&](V v) {
        if (v.is(V::Inst) && !live[v.index()]) {
            live[v.index()] = true;
            work.push_back(v.index());
        }
    };
    for (const Block& b : blocks_) {
        for (std::uint32_t i : b.body) {
            mark(V::inst(i));
        }
        if (b.term && b.term->kind == Term::CondBr) {
            mark(b.term->cond);
        }
    }
    while (!work.empty()) {
        std::uint32_t i = work.back();
        work.pop_back();
        for_each_operand(insts_[i], mark);
    }
    return live;
}

std::vector<std::uint8_t> Module::write() const {
    Writer w;
    w.bytes("BC\xC0\xDE", 4);
    w.enter(MODULE_BLOCK, 3);
    w.record(1, {1});   


    w.enter(PARAMATTR_GROUP_BLOCK, 3);
    w.record(3, {1, 0xFFFFFFFF, 0, 18, 0, 20});
    w.record(3, {2, 0xFFFFFFFF, 0, 18});
    w.record(3, {3, 0xFFFFFFFF, 0, 18, 0, 21});
    w.record(3, {4, 0xFFFFFFFF, 0, 12, 0, 18});
    w.exit();
    w.enter(PARAMATTR_BLOCK, 3);
    for (std::uint64_t id = 1; id <= 4; ++id) {
        w.record(2, {id});
    }
    w.exit();

    std::vector<std::uint64_t> r;
    w.enter(TYPE_BLOCK, 4);
    w.record(1, {types_.size()});
    for (const TypeDef& t : types_) {
        switch (t.kind) {
            case TypeDef::Void: w.record(2, {}); break;
            case TypeDef::Float: w.record(3, {}); break;
            case TypeDef::Double: w.record(4, {}); break;
            case TypeDef::Int: w.record(7, {t.a}); break;
            case TypeDef::Ptr: w.record(8, {t.a, t.b}); break;
            case TypeDef::Array: w.record(11, {t.a, t.b}); break;
            case TypeDef::Struct:
                r.assign(1, 0);
                for (std::uint32_t f : t.list) {
                    r.push_back(f);
                }
                if (t.named) {
                    w.chars(19, t.name);
                    w.record(20, r);
                } else {
                    w.record(18, r);
                }
                break;
            case TypeDef::Func:
                r.assign({0, t.a});
                for (std::uint32_t p : t.list) {
                    r.push_back(p);
                }
                w.record(21, r);
                break;
        }
    }
    w.exit();

    w.chars(2, "dxil-ms-dx");
    w.chars(3, "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64");
    std::uint64_t first_const = globals_.size() + funcs_.size();
    for (const Global& g : globals_) {
        std::uint64_t constant = 0, init = 0, linkage = 0;
        if (g.init) {
            constant = 1;
            init = first_const + *g.init + 1;
            linkage = 3;
        }
        w.record(7, {g.ty.id, std::uint64_t{g.space} << 2 | 2 | constant, init, linkage, 3, 0});
    }
    for (const Func& f : funcs_) {
        std::uint64_t attr = f.attr ? static_cast<std::uint64_t>(*f.attr) : 0;
        w.record(8, {f.ty.id, 0, f.body ? 0u : 1u, 0, attr, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    }


    auto globals = static_cast<std::uint32_t>(globals_.size());
    auto funcs = static_cast<std::uint32_t>(globals + funcs_.size());
    w.enter(CONSTANTS_BLOCK, 4);
    std::optional<std::uint32_t> current;
    for (const auto& [ty, c] : consts_) {
        if (current != ty.id) {
            w.record(1, {ty.id});
            current = ty.id;
        }
        switch (c.kind) {
            case Const::Undef: w.record(3, {}); break;
            case Const::Int: w.record(4, {signed_vbr(c.value)}); break;
            case Const::Float: w.record(6, {static_cast<std::uint64_t>(c.value)}); break;
            case Const::Aggregate:
                r.clear();
                for (std::uint32_t i : c.fields) {
                    r.push_back(std::uint64_t{funcs} + i);
                }
                w.record(7, r);
                break;
        }
    }
    w.exit();


    std::vector<bool> lv = live();


    std::size_t nb = blocks_.size();
    std::vector<bool> seen(nb, false);
    std::vector<std::size_t> post;
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, 0}};
    seen[0] = true;
    while (!stack.empty()) {
        auto& top = stack.back();
        auto [b, k] = top;
        ++top.second;
        Succs s = successors_of(blocks_[b].term);
        if (k < s.len) {
            std::size_t x = s.to[k];
            if (!seen[x]) {
                seen[x] = true;
                stack.emplace_back(x, 0);
            }
        } else {
            post.push_back(b);
            stack.pop_back();
        }
    }
    std::reverse(post.begin(), post.end());
    std::vector<std::uint64_t> pos(nb, UINT64_MAX);
    for (std::size_t n = 0; n < post.size(); ++n) {
        pos[post[n]] = n;
    }

    std::vector<std::vector<std::size_t>> preds(nb);
    for (std::size_t b : post) {
        for (std::size_t s : successors_of(blocks_[b].term)) {
            preds[s].push_back(b);
        }
    }
    std::vector<std::optional<std::pair<std::size_t, std::size_t>>> fold(nb);
    for (std::size_t b : post) {
        const Block& blk = blocks_[b];
        if (blk.term && blk.term->kind == Term::Br && blk.phis.empty() && blk.body.empty() && preds[b].size() == 1) {
            std::size_t h = blk.term->t;
            std::size_t p = preds[b][0];
            Succs ps = successors_of(blocks_[p].term);
            bool to_h = std::find(ps.begin(), ps.end(), h) != ps.end();
            if (pos[h] < pos[b] && !to_h) {
                fold[b] = std::pair{p, h};
            }
        }
    }
    std::vector<std::size_t> layout;
    for (std::size_t b : post) {
        if (!fold[b]) {
            layout.push_back(b);
        }
    }
    pos.assign(nb, UINT64_MAX);
    for (std::size_t n = 0; n < layout.size(); ++n) {
        pos[layout[n]] = n;
    }
    auto target = [&](std::size_t t) { return fold[t] ? pos[fold[t]->second] : pos[t]; };
    auto from = [&](std::size_t b) { return fold[b] ? pos[fold[b]->first] : pos[b]; };


    std::uint32_t base = funcs + static_cast<std::uint32_t>(consts_.size());
    std::vector<std::uint32_t> ids(insts_.size(), UINT32_MAX);
    std::uint32_t next = base;
    for (std::size_t b : layout) {
        const Block& blk = blocks_[b];
        for (std::uint32_t i : blk.phis) {
            if (lv[i] && !is_void(insts_[i].ty)) {
                ids[i] = next++;
            }
        }
        for (std::uint32_t i : blk.body) {
            if (!is_void(insts_[i].ty)) {
                ids[i] = next++;
            }
        }
    }
    auto abs = [&](V v) -> std::uint32_t {
        switch (v.kind()) {
            case V::Global: return v.index();
            case V::Func: return globals + v.index();
            case V::Const: return funcs + v.index();
            case V::Inst: {
                std::uint32_t id = ids[v.index()];
                if (id == UINT32_MAX) {
                    throw std::logic_error("use of a void value");
                }
                return id;
            }
        }
        return 0;
    };

    w.enter(METADATA_BLOCK, 3);
    for (const MdDef& m : mds_) {
        switch (m.kind) {
            case MdDef::Str: w.chars(1, m.str); break;
            case MdDef::Value: w.record(2, {m.ty.id, abs(m.v)}); break;
            case MdDef::Node:
                r.clear();
                for (const auto& o : m.ops) {
                    r.push_back(o ? std::uint64_t{o->id} + 1 : 0);
                }
                w.record(3, r);
                break;
        }
    }
    for (const auto& [name, nodes] : named_) {
        w.chars(4, name);
        r.clear();
        for (Md m : nodes) {
            r.push_back(m.id);
        }
        w.record(10, r);
    }
    w.exit();

    w.enter(METADATA_BLOCK, 3);
    static const char* const KINDS[] = {"dbg",
                                        "tbaa",
                                        "prof",
                                        "fpmath",
                                        "range",
                                        "tbaa.struct",
                                        "invariant.load",
                                        "alias.scope",
                                        "noalias",
                                        "nontemporal",
                                        "llvm.mem.parallel_loop_access",
                                        "nonnull",
                                        "dereferenceable",
                                        "dereferenceable_or_null"};
    for (std::uint64_t id = 0; id < std::size(KINDS); ++id) {
        w.chars(6, KINDS[id], id);
    }
    w.exit();

    w.enter(VALUE_SYMTAB_BLOCK, 4);
    std::uint64_t sym = 0;
    for (const Global& g : globals_) {
        w.chars(1, g.name, sym++);
    }
    for (const Func& f : funcs_) {
        w.chars(1, f.name, sym++);
    }
    w.exit();

    w.enter(FUNCTION_BLOCK, 4);
    w.record(1, {layout.size()});
    std::uint32_t inst_id = base;
    auto write_inst = [&](std::uint32_t i) {
        const Inst& x = insts_[i];
        auto rel = [&](V v) -> std::uint64_t {
            std::uint32_t a = abs(v);
            if (a >= inst_id) {
                throw std::logic_error("forward reference outside a phi");
            }
            return inst_id - a;
        };
        switch (x.op) {
            case OpKind::Bin: w.record(2, {rel(x.a), rel(x.b), x.code}); break;
            case OpKind::Cast: w.record(3, {rel(x.a), x.ty.id, x.code}); break;
            case OpKind::Cmp: w.record(28, {rel(x.a), rel(x.b), x.code}); break;
            case OpKind::Select: w.record(29, {rel(x.b), rel(x.c), rel(x.a)}); break;
            case OpKind::Extract: w.record(26, {rel(x.a), x.x}); break;
            case OpKind::Call:
                r.assign({0, 1 << 15, funcs_[x.x].ty.id});
                r.push_back(rel(V::func(x.x)));
                for (V a : x.list) {
                    r.push_back(rel(a));
                }
                w.record(34, r);
                break;
            case OpKind::Phi:
                r.assign(1, x.ty.id);
                for (std::size_t k = 0; k < x.list.size(); ++k) {
                    r.push_back(signed_vbr(static_cast<std::int64_t>(inst_id) - static_cast<std::int64_t>(abs(x.list[k]))));
                    r.push_back(from(x.from[k]));
                }
                w.record(16, r);
                break;
            case OpKind::Gep:
                r.assign({0, x.x, rel(x.a)});
                for (V v : x.list) {
                    r.push_back(rel(v));
                }
                w.record(43, r);
                break;
            case OpKind::Load: w.record(20, {rel(x.a), x.ty.id, 3, 0}); break;
            case OpKind::Store: w.record(44, {rel(x.a), rel(x.b), 3, 0}); break;
            case OpKind::AtomicRmw: w.record(38, {rel(x.a), rel(x.b), x.code, 0, 6, 1}); break;
            case OpKind::CmpXchg: w.record(46, {rel(x.a), rel(x.b), rel(x.c), 0, 6, 1, 6, 0}); break;
        }
        if (!is_void(x.ty)) {
            ++inst_id;
        }
    };
    for (std::size_t b : layout) {
        const Block& blk = blocks_[b];
        for (std::uint32_t i : blk.phis) {
            if (lv[i]) {
                write_inst(i);
            }
        }
        for (std::uint32_t i : blk.body) {
            write_inst(i);
        }
        if (!blk.term) {
            throw std::logic_error("unterminated block");
        }
        switch (blk.term->kind) {
            case Term::Ret: w.record(10, {}); break;
            case Term::Br: w.record(11, {target(blk.term->t)}); break;
            case Term::CondBr: {
                std::uint32_t a = abs(blk.term->cond);
                if (a >= inst_id) {
                    throw std::logic_error("forward branch condition");
                }
                w.record(11, {target(blk.term->t), target(blk.term->f), std::uint64_t{inst_id - a}});
                break;
            }
        }
    }
    w.exit();
    w.exit();
    return w.finish();
}

}   
