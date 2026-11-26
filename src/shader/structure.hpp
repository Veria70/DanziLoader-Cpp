// SPDX-License-Identifier: GPL-3.0-or-later
// Control-flow structurisation (`structure.rs`; DanziLoader / victory-perf,
// see NOTICE.md): the program as one lane runs it, without EXEC. Lane masks
// become per-lane booleans (`Bools`, reduced ordered BDDs); a branch on a
// wave-wide test of a mask becomes a branch on the lane's own bit; a
// path-sensitive pass proves, for each vector instruction, that EXEC holds
// in every lane that reaches it, and what it cannot prove stays as the
// instruction's guard.
//
// BDD nodes are numbered in creation order, as the reference's: the port
// makes them in the same order (the dumps compare the numbers).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "flat_map.hpp"
#include "ir.hpp"

namespace danzi::shader::structure {


struct Bool {
    std::uint32_t id = 0;
    bool operator==(const Bool&) const = default;
    auto operator<=>(const Bool&) const = default;
};
constexpr Bool FALSE_{0}, TRUE_{1};


struct Atom {
    enum Kind : std::uint64_t { Lane, Uniform, Phi, Quad, Opaque, Constant } kind;

    std::uint64_t payload;
    bool operator==(const Atom&) const = default;
};


struct Meets {
    std::unordered_map<std::string, std::size_t> none;
};

class Bools {
public:
    Bools();


    void reset();
    struct Node {
        Atom atom;
        Bool lo, hi;
    };

    std::optional<Node> node(Bool b) const;

    std::vector<Atom> support(Bool b) const;

    bool meet(const std::vector<std::pair<Bool, bool>>& all) const;
    bool meet_seen(const std::vector<std::pair<Bool, bool>>& all, Meets& seen) const;

    Bool atom(Atom a, std::size_t born);
    Bool mk(std::uint32_t v, Bool lo, Bool hi);
    Bool ite(Bool f, Bool g, Bool h);
    Bool and_(Bool a, Bool b) { return ite(a, b, FALSE_); }
    Bool or_(Bool a, Bool b) { return ite(a, TRUE_, b); }
    Bool not_(Bool a) { return ite(a, FALSE_, TRUE_); }
    Bool xor_(Bool a, Bool b) {
        Bool nb = not_(b);
        return ite(a, nb, b);
    }
    bool implies(Bool a, Bool b) {
        Bool nb = not_(b);
        return and_(a, nb) == FALSE_;
    }
    Bool forget(Bool b, std::size_t from, FlatMap<std::uint32_t, Bool>& memo);
    Bool restrict(Bool b, std::uint32_t v, bool value, FlatMap<std::uint32_t, Bool>& memo);
    Bool given(Bool b, Bool known);
    std::vector<std::uint32_t> vars(Bool b) const;

    bool overflow = false;
    std::size_t node_count() const { return nodes_.size(); }

private:
    struct Raw {
        std::uint32_t v;
        Bool lo, hi;
    };
    struct Key3 {
        std::uint32_t a, b, c;
    };
    std::pair<Bool, Bool> split(Bool b, std::uint32_t v) const;
    bool meet_in(std::size_t at, std::vector<std::uint32_t>& work, std::size_t& budget, Meets& seen) const;

    std::vector<Raw> nodes_;

    mutable std::vector<std::uint32_t> seen_, var_seen_;
    mutable std::uint32_t stamp_ = 0;
    mutable std::vector<Bool> stack_;
    Triples unique_;
    TripleCache memo_;
    std::vector<Atom> atoms_;
    std::vector<std::size_t> born_;
    FlatMap<Atom, std::uint32_t> ids_;
};


struct LaneExit {
    enum Kind : std::uint8_t { Jump, Branch, End, Indirect } kind = End;
    Bool when;
    std::size_t taken = 0, not_taken = 0;
};


struct Key {
    std::uint32_t lo, hi;
    bool operator==(const Key&) const = default;
};

struct ValueHash {
    std::size_t operator()(ir::Value v) const { return std::hash<std::uint32_t>{}(v.id); }
};


struct Structured {
    Bools bools;
    std::vector<LaneExit> exits;


    std::vector<std::vector<std::optional<Bool>>> guards;

    std::vector<Bool> reach;

    std::unordered_map<ir::Value, Bool, ValueHash> masks;

    std::unordered_map<ir::Value, ir::Value, ValueHash> highs;
    std::unordered_set<ir::Value, ValueHash> active;
    std::unordered_map<ir::Value, std::vector<Bool>, ValueHash> phi_masks;
    std::unordered_set<std::size_t> wide;
    std::vector<std::optional<bool>> wave_any;
    std::unordered_map<ir::Value, Bool, ValueHash> scc_some;

    std::optional<Bool> mask_bits(ir::Value low, std::optional<ir::Value> high) const;


    FlatMap<Key, Bool> paired_masks;
};


Structured structure(const ir::Program& program);


Structured structure_wide(const ir::Program& program, const std::unordered_set<std::size_t>& wide);

}   
