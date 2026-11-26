// SPDX-License-Identifier: GPL-3.0-or-later
// A DXIL module and its LLVM 3.7 bitcode (`emit/bitcode.rs`, `emit/bitcode/
// opt.rs`; DanziLoader / victory-perf, see NOTICE.md), written with
// unabbreviated records only. Values are symbolic while the module is built
// (`V`) and numbered when it is written: globals, functions, constants (all
// at module level), then the instructions of `main` in block order. Types,
// constants and functions are numbered in the order first asked for, as the
// reference numbers them, so the bytes written are the reference's.
//
// Port's choices: a value is one packed dword (kind in the top two bits,
// ordered as the reference's derived order); operand lists of calls, phis
// and element pointers are vectors, the others inline. The instruction
// census (`VICTORY_SHADER_CENSUS`) is a diagnostic: milestone 7.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "flat_map.hpp"

namespace danzi::shader::bitcode {

struct Ty {
    std::uint32_t id = 0;
    bool operator==(const Ty&) const = default;
    auto operator<=>(const Ty&) const = default;
};


struct V {
    enum Kind : std::uint32_t { Global = 0, Func = 1, Const = 2, Inst = 3 };
    std::uint32_t raw = 0;
    static V make(Kind k, std::uint32_t i) { return V{static_cast<std::uint32_t>(k) << 30 | i}; }
    static V global(std::uint32_t i) { return make(Global, i); }
    static V func(std::uint32_t i) { return make(Func, i); }
    static V konst(std::uint32_t i) { return make(Const, i); }
    static V inst(std::uint32_t i) { return make(Inst, i); }
    Kind kind() const { return static_cast<Kind>(raw >> 30); }
    std::uint32_t index() const { return raw & 0x3FFFFFFFu; }
    bool is(Kind k) const { return kind() == k; }
    bool operator==(const V&) const = default;
    auto operator<=>(const V&) const = default;
};


enum class Tag : std::uint8_t { Guard, Clamp, Counter };

enum class Bin : std::uint8_t { Add = 0, Sub = 1, Mul = 2, UDiv = 3, SDiv = 4, URem = 5, Shl = 7, LShr = 8, AShr = 9, And = 10, Or = 11, Xor = 12 };
enum class Rmw : std::uint8_t { Xchg = 0, Add = 1, Sub = 2, And = 3, Or = 5, Xor = 6, Max = 7, Min = 8, UMax = 9, UMin = 10 };
enum class Cast : std::uint8_t { Trunc = 0, ZExt = 1, SExt = 2, FpToUi = 3, FpToSi = 4, UiToFp = 5, SiToFp = 6, Bitcast = 11 };


namespace pred {
constexpr std::uint8_t FALSE_ = 0, OEQ = 1, OGT = 2, OGE = 3, OLT = 4, OLE = 5, ONE = 6, ORD = 7, UNO = 8, UEQ = 9, UGT = 10, UGE = 11,
                       ULT = 12, ULE = 13, UNE = 14, TRUE_ = 15, EQ = 32, NE = 33, IUGT = 34, IUGE = 35, IULT = 36, IULE = 37, SGT = 38,
                       SGE = 39, SLT = 40, SLE = 41;
}

struct Term {
    enum Kind : std::uint8_t { Br, CondBr, Ret } kind = Ret;
    V cond{};
    std::size_t t = 0, f = 0;
    static Term br(std::size_t to) { return {Br, {}, to, 0}; }
    static Term cond_br(V c, std::size_t t, std::size_t f) { return {CondBr, c, t, f}; }
    static Term ret() { return {Ret, {}, 0, 0}; }
};


enum class Attr : std::uint8_t { ReadNone = 1, NoUnwind = 2, ReadOnly = 3, NoDuplicate = 4 };

struct Md {
    std::uint32_t id = 0;
};


struct Usage {
    bool wave_ops = false, int64 = false, typed_uav_loads = false, heap_views = false, heap_samplers = false, atomic64 = false;
};


namespace opt {
constexpr std::uint32_t DEAD = 1, FOLD = 2, REUSE = 4, FACTS = 8, LOADS = 16, FIRST = 32;
constexpr std::uint32_t MODULE = DEAD | FOLD | REUSE | FACTS | LOADS;
constexpr std::uint32_t ALL = MODULE | FIRST;
}   


enum class Effect : std::uint8_t { Pure, Reads, Lanes, Writes };

class Module {
public:

    Ty void_();
    Ty int_(std::uint32_t bits);
    Ty float_();
    Ty double_();
    Ty ptr(Ty to) { return ptr_in(to, 0); }
    Ty ptr_in(Ty to, std::uint32_t space);
    Ty array(std::uint32_t n, Ty of);
    Ty named_struct(std::string_view name, std::vector<Ty> fields);
    Ty func_ty(Ty ret, std::vector<Ty> params);


    V int_const(Ty ty, std::int64_t v);
    V i32(std::uint32_t v) { return int_const(int_(32), static_cast<std::int64_t>(static_cast<std::int32_t>(v))); }
    V i8(std::uint8_t v) { return int_const(int_(8), static_cast<std::int64_t>(static_cast<std::int8_t>(v))); }
    V boolean(bool v) { return int_const(int_(1), v ? -1 : 0); }
    V f32(std::uint32_t bits);
    V undef(Ty ty);
    V aggregate(Ty ty, const std::vector<V>& fields);
    std::optional<std::int64_t> int_value(V v) const;
    void replace_const(V old, V now);

    V function(std::string_view name, Ty ty, std::optional<Attr> attr, bool body);
    V global(std::string_view name, Ty ty, std::uint32_t space);
    V constant_global(std::string_view name, V init);
    void tag(V, Tag) {}

    std::size_t new_block() {
        blocks_.emplace_back();
        return blocks_.size() - 1;
    }
    std::size_t block_count() const { return blocks_.size(); }
    Ty type_of(V v) const;

    V bin(std::size_t block, Bin op, V a, V b);
    V cast(std::size_t block, Cast op, V v, Ty to);
    V cmp(std::size_t block, std::uint8_t pred, V a, V b);
    V select(std::size_t block, V c, V t, V f);
    V extract(std::size_t block, V agg, std::uint32_t index);
    V call(std::size_t block, V f, std::vector<V> args);
    V phi(std::size_t block, Ty ty);
    V element(std::size_t block, V ptr, V index);
    V load(std::size_t block, V ptr);
    void store(std::size_t block, V ptr, V value);
    V atomic(std::size_t block, Rmw op, V ptr, V value);
    V cmpxchg(std::size_t block, V ptr, V expected, V now);
    void set_phi(V phi, std::vector<std::pair<V, std::size_t>> incoming);
    void terminate(std::size_t block, Term t) { blocks_[block].term = t; }
    bool terminated(std::size_t block) const { return blocks_[block].term.has_value(); }

    Md md_str(std::string_view s);
    Md md_value(V v);
    Md md_node(std::vector<std::optional<Md>> ops);
    void named_md(std::string_view name, std::vector<Md> nodes);


    void optimise(std::uint32_t passes);
    Usage usage() const;
    std::vector<std::uint8_t> write() const;


    std::vector<bool> reached() const;
    std::vector<Effect> effects() const;
    std::size_t inst_count() const { return insts_.size(); }

private:
    struct TypeDef {
        enum Kind : std::uint8_t { Void, Int, Float, Double, Ptr, Struct, Array, Func } kind = Void;
        std::uint32_t a = 0, b = 0;
        std::vector<std::uint32_t> list;
        bool named = false;
        std::string name;
        bool operator==(const TypeDef&) const = default;
    };
    struct TypeHash {
        std::size_t operator()(const TypeDef& t) const;
    };
    struct Const {
        enum Kind : std::uint8_t { Int, Float, Undef, Aggregate } kind = Undef;
        std::int64_t value = 0;
        std::vector<std::uint32_t> fields;
    };
    struct ConstKey {
        std::uint32_t ty, kind;
        std::uint64_t value;
    };
    struct Global {
        std::string name;
        Ty ty, ptr;
        std::uint32_t space;
        std::optional<std::uint32_t> init;
    };
    enum class OpKind : std::uint8_t { Bin, Cast, Cmp, Select, Extract, Call, Phi, Gep, Load, Store, AtomicRmw, CmpXchg };
    struct Inst {
        Ty ty;
        OpKind op;

        std::uint8_t code = 0;

        std::uint32_t x = 0;
        V a{}, b{}, c{};

        std::vector<V> list;

        std::vector<std::size_t> from;
    };
    struct Block {
        std::vector<std::uint32_t> phis, body;
        std::optional<Term> term;
    };
    struct Func {
        std::string name;
        Ty ty;
        std::optional<Attr> attr;
        bool body;
    };
    struct MdDef {
        enum Kind : std::uint8_t { Str, Value, Node } kind;
        std::string str;
        Ty ty;
        V v;
        std::vector<std::optional<Md>> ops;
    };

    std::vector<TypeDef> types_;
    std::unordered_map<TypeDef, std::uint32_t, TypeHash> type_ids_;

    std::uint32_t int_ids_[65] = {};
    std::uint32_t float_id_ = 0, void_id_ = 0;
    std::vector<std::pair<Ty, Const>> consts_;
    FlatMap<ConstKey, std::uint32_t> scalar_ids_;
    std::unordered_map<std::string, std::uint32_t> aggregate_ids_;
    std::vector<Global> globals_;
    std::vector<Func> funcs_;
    std::unordered_map<std::string, std::uint32_t> func_ids_;
    std::vector<Inst> insts_;
    std::vector<Block> blocks_;
    std::vector<MdDef> mds_;
    std::vector<std::pair<std::string, std::vector<Md>>> named_;


    struct Succs {
        std::size_t to[2];
        std::size_t len;
        const std::size_t* begin() const { return to; }
        const std::size_t* end() const { return to + len; }
    };
    static Succs successors_of(const std::optional<Term>& t);

    Ty ty(TypeDef def);
    V constant(Ty ty, Const c);
    bool is_void(Ty t) const { return types_[t.id].kind == TypeDef::Void; }
    V push(std::size_t block, Ty ty, Inst inst);
    std::vector<bool> live() const;
    template <class F>
    static void for_each_operand(const Inst& inst, F f);
    template <class F>
    static void map_operands(Inst& inst, F f);


    struct Tree;
    struct Facts;
    struct Learned;
    struct Key;
    struct KeyHash;
    class KeyTable;
    Tree dominator_tree() const;
    void learn(V v, bool holds, Facts& facts, Learned& learned, std::uint32_t depth) const;
    std::optional<std::uint32_t> int_bits(Ty ty) const;
    std::optional<std::pair<std::uint32_t, std::uint64_t>> int_k(V v) const;
    V k(Ty ty, std::uint32_t bits, std::uint64_t x);
    std::optional<V> simplify(std::uint32_t i, bool& changed);
    Effect klass(const std::vector<Effect>& effects, std::uint32_t i) const;
    bool key(std::uint32_t i, Key& out) const;
    bool merge(std::uint32_t passes);
    bool prune();
    bool has_effect(const std::vector<Effect>& effects, std::uint32_t i) const;
    void drop_dead();
    void drop_unused_tables();
    void drop_unused_functions();
};

template <class F>
inline void Module::for_each_operand(const Inst& x, F f) {
    switch (x.op) {
        case OpKind::Bin:
        case OpKind::Cmp:
        case OpKind::Store:
        case OpKind::AtomicRmw:
            f(x.a);
            f(x.b);
            break;
        case OpKind::Cast:
        case OpKind::Extract:
        case OpKind::Load: f(x.a); break;
        case OpKind::Select:
        case OpKind::CmpXchg:
            f(x.a);
            f(x.b);
            f(x.c);
            break;
        case OpKind::Call:
        case OpKind::Phi:
            for (V v : x.list) {
                f(v);
            }
            break;
        case OpKind::Gep:
            f(x.a);
            for (V v : x.list) {
                f(v);
            }
            break;
    }
}

template <class F>
inline void Module::map_operands(Inst& x, F f) {
    switch (x.op) {
        case OpKind::Bin:
        case OpKind::Cmp:
        case OpKind::Store:
        case OpKind::AtomicRmw:
            x.a = f(x.a);
            x.b = f(x.b);
            break;
        case OpKind::Cast:
        case OpKind::Extract:
        case OpKind::Load: x.a = f(x.a); break;
        case OpKind::Select:
        case OpKind::CmpXchg:
            x.a = f(x.a);
            x.b = f(x.b);
            x.c = f(x.c);
            break;
        case OpKind::Call:
        case OpKind::Phi:
            for (V& v : x.list) {
                v = f(v);
            }
            break;
        case OpKind::Gep:
            x.a = f(x.a);
            for (V& v : x.list) {
                v = f(v);
            }
            break;
    }
}

}   
