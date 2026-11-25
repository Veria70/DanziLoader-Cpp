// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/rebar.rs (DanziLoader / victory-perf, see


#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "device.hpp"

namespace danzi::gpu::rebar {

enum class Kind { Window, Buffer };

enum class Decision { Rebar, Off, Unsupported, NotThisKind, CpuRead, OverBudget, CardFull };


const char* name(Decision d);

struct Ask {
    std::uint8_t level = 0;
    bool supported = false;
    Kind kind = Kind::Window;
    std::uint64_t size = 0;
    bool cpu_read = false;
    std::uint64_t placed = 0, most = 0;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> card;   
};


Decision decide(const Ask& ask);

std::uint8_t level_of(std::optional<std::string_view> value);

std::uint64_t default_most(std::optional<std::uint64_t> card_budget);

std::uint8_t level();
bool supported();

void probe(Device& device);

bool reserve(Device& device, Kind kind, std::uint64_t size, bool cpu_read);

void released(std::uint64_t size);

std::pair<std::uint64_t, std::uint64_t> placed();

}   
