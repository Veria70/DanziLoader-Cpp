// SPDX-License-Identifier: GPL-3.0-or-later
// Port of parts of crates/victory-gpu/src/readback.rs (DanziLoader /


#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "win.hpp"

namespace danzi::readback {


bool enabled();


std::optional<std::pair<usize, usize>> function_at(usize base, std::uint32_t rva);

void tag(std::uint64_t start, std::uint64_t len);

bool tagged_in(std::uint64_t start, std::uint64_t len);


bool copy_back(std::uint64_t start, std::uint64_t len);

void untag(std::uint64_t start);
usize tagged();

}   
