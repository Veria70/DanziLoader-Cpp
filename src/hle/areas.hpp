// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime API areas `QueryApiImpl` serves (`xgameruntime.rs` `AREAS`),
// in the reference's order.
#pragma once

#include <vector>

#include "xgameruntime.hpp"

namespace danzi::hle {
std::vector<Interface> all_area_interfaces();
}
