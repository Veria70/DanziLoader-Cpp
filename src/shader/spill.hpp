// SPDX-License-Identifier: GPL-3.0-or-later
// The spilled user-data table of a compute program (`spill.rs`) and the
// geometry half of a merged ES-GS stage (`merged.rs`; DanziLoader /
// victory-perf, see NOTICE.md).
#pragma once

#include <cstddef>

#include "ir.hpp"
#include "native.hpp"

namespace danzi::shader {


ir::Program spill_build(const NativeProgram& native);


std::size_t spill_user_data(ir::Program& p, const NativeProgram& native);


std::size_t merged_prepare(ir::Program& p, const NativeProgram& native);

}   
