// SPDX-License-Identifier: GPL-3.0-or-later
// Port of parts of crates/victory-gpu/src/readback.rs (DanziLoader /
// victory-perf, see NOTICE.md): the title's read-back buffers, tagged when
// made and never placed where the CPU reads slowly (`rebar`), and the copy
// back of shader writes limited to them (`VICTORY_COPY_BACK_ONLY_READBACK`,
// testing, off by default).
//
// Ported now: the tags and the lookup of a function's range in a mapped
// image. Waits for the runtime (milestone 6): finding the title's creator
// on the stack when a buffer is made (`creating_readback`).
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
