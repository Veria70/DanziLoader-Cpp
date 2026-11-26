// SPDX-License-Identifier: GPL-3.0-or-later
// Whole-wave sections (`emit/wave.rs`; DanziLoader / victory-perf, see
// NOTICE.md): code between `s_or_saveexec -1` (or `s_nand_saveexec 0`) and
// the restore of the saved mask runs in every lane of the console's wave.
// Each section is simulated on the console's lanes with symbolic values;
// a value it leaves is lowered by what it turned out to be: the reduction
// or prefix of one operation over the lanes' (masked) inputs.
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "ir.hpp"
#include "structure.hpp"

namespace danzi::shader::wave {


enum class Op : std::uint8_t { AddU, AddF, AddH, MinU, MaxU, MinI, MaxI, MinF, MaxF, Or, And, Xor };


struct Shape {
    enum Kind : std::uint8_t {
        K,
        Value,

        Masked,

        Active,


        ActiveF64,

        Prefix,
    } kind = K;
    std::uint32_t k = 0;
    ir::Value a{}, b{}, c{};
    Op op = Op::AddU;
    std::uint32_t ident = 0;

    bool flag = false;
};


struct Section {
    std::size_t end = 0;
    std::unordered_map<ir::Value, Shape, structure::ValueHash> shapes;
};


std::unordered_map<std::size_t, Section> sections(const ir::Program& p, std::size_t block);

}   
