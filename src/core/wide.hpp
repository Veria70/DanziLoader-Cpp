// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the kept threads of crates/victory-gpu/src/shadow.rs (`wide`):
// runs jobs on a few kept threads and on the calling one, and returns when
// all are done. The jobs copy and compare memory, each a part of its own.
#pragma once

#include <functional>
#include <vector>

#include "win.hpp"

namespace danzi::wide {

using Job = std::function<void()>;


void run(std::vector<Job>& jobs);


usize threads();

}   
