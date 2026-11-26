// SPDX-License-Identifier: GPL-3.0-or-later
// One-to-one SSA form of a native program (`ir.rs`; DanziLoader /
// victory-perf, see NOTICE.md). Every machine instruction becomes one
// `Inst`, except scheduling-only ones, which are dropped. Each register
// dword is a variable: a read names the `Value` it sees, a write defines a
// new one, and `Phi`s merge values where control flow joins. Implicit
// operands (SCC, M0, EXEC) are explicit; a write to a VGPR also reads its
// old value (lanes disabled in EXEC keep it).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "gfx10.hpp"

namespace danzi::shader::ir {


struct Reg {
    enum Kind : std::uint8_t { S, Scc, V } kind = S;
    std::uint8_t n = 0;
    static constexpr Reg s(std::uint8_t c) { return {S, c}; }
    static constexpr Reg scc() { return {Scc, 0}; }
    static constexpr Reg v(std::uint8_t r) { return {V, r}; }
    auto operator<=>(const Reg&) const = default;
    bool operator==(const Reg&) const = default;
};

constexpr std::uint8_t VCC = 106, M0 = 124, EXEC = 126, PARKED = 128;


struct Value {
    std::uint32_t id = 0;
    auto operator<=>(const Value&) const = default;
    bool operator==(const Value&) const = default;
};

struct Exit {
    enum Kind : std::uint8_t { Jump, Branch, End, Indirect } kind = End;

    std::size_t a = 0, b = 0;
    static Exit jump(std::size_t t) { return {Jump, t, 0}; }
    static Exit branch(std::size_t taken, std::size_t not_taken) { return {Branch, taken, not_taken}; }
    bool operator==(const Exit&) const = default;

    std::size_t successors(std::size_t (&out)[2]) const {
        switch (kind) {
            case Jump: out[0] = a; return 1;
            case Branch: out[0] = a; out[1] = b; return 2;
            default: return 0;
        }
    }
    std::vector<std::size_t> successor_list() const {
        std::size_t s[2];
        std::size_t n = successors(s);
        return std::vector<std::size_t>(s, s + n);
    }
};

struct Phi {
    Reg reg;
    Value value;

    std::vector<Value> args;
};

using RegValues = std::vector<std::pair<Reg, Value>>;

struct Inst {
    Instruction machine;
    Fields fields;

    RegValues reads, writes;
    std::string_view name() const { return machine.name(); }
    std::optional<Value> read(Reg reg) const {
        for (const auto& r : reads) {
            if (r.first == reg) {
                return r.second;
            }
        }
        return std::nullopt;
    }
    std::optional<Value> write(Reg reg) const {
        for (const auto& w : writes) {
            if (w.first == reg) {
                return w.second;
            }
        }
        return std::nullopt;
    }
};

struct Block {


    std::size_t offset = 0;
    std::vector<std::size_t> preds;
    std::vector<Phi> phis;
    std::vector<Inst> insts;
    Exit exit;
};

struct Program {
    bool wave64 = false;

    std::uint16_t vgprs = 0;


    std::vector<Block> blocks;

    RegValues inputs;
};


Program build(std::span<const std::uint8_t> code, bool wave64, std::uint16_t vgprs);


Program thread_edges(const Program& p, std::span<const std::array<std::size_t, 3>> edges);


std::optional<Program> without_early_outs(const Program& p);


std::optional<Program> without_skips(const Program& p, bool deep);

std::vector<std::size_t> reverse_postorder(const std::vector<Block>& blocks);


std::optional<std::pair<std::size_t, std::size_t>> irreducible_edge(const std::vector<Block>& blocks, const std::vector<std::size_t>& order,
                                                                    const std::vector<std::size_t>& index);

std::vector<std::size_t> idoms(const std::vector<Block>& blocks, const std::vector<std::size_t>& order, const std::vector<std::size_t>& index);

std::vector<Reg> operand_reads(const Program& p, const Inst& inst);

}   
