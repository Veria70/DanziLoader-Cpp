// SPDX-License-Identifier: GPL-3.0-or-later
// Wave ORs that only steer a loop over each lane's own bits
// (`emit/own_bits.rs`; DanziLoader / victory-perf, see NOTICE.md): a console
// wave64 program in two host waves of 32 may take each host wave's own OR
// where the loop visits the set bits and acts only at the lane's own bits.
// `find` proves that for each wave OR, with no sequence taken on trust.
#pragma once

#include <unordered_set>

#include "ir.hpp"
#include "structure.hpp"

namespace danzi::shader::own_bits {

struct OwnBits {


    std::unordered_set<ir::Value, structure::ValueHash> totals;

    std::unordered_set<ir::Value, structure::ValueHash> alike;
};


OwnBits find(const ir::Program& p, const structure::Structured& st, bool why);

}   
