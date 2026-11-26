// SPDX-License-Identifier: GPL-3.0-or-later
// Instruction text in AMD's SP3 assembly syntax (`gfx10/sp3.rs`), and the
// `{:?}` forms of a program's root layout and used-resource entries
// (`native.rs`'s derived `Debug`; DanziLoader / victory-perf, see
// NOTICE.md): what `victory shader-info --disasm` lists.
#pragma once

#include <string>

#include "gfx10.hpp"
#include "native.hpp"

namespace danzi::shader {


std::string instruction_text(const Instruction& i, bool wave64);


std::string debug_text(const RootParam& p);
std::string debug_text(const UsedResource& u);

}   
