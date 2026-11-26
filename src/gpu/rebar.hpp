// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/rebar.rs (DanziLoader / victory-perf, see
// NOTICE.md): `VICTORY_REBAR` (opt-in, off by default), host copies in the
// card's memory that the CPU maps (Resizable BAR, D3D12's GPU upload heap).
// The placement rule, the probe at the host's start, and the bytes placed.
//
// Where the port differs (M3.9): the reference places level 2's small title
// buffers, which shaders may write, in a GPU upload heap and then reads them
// on the CPU for snapshots and copy-backs. On the RTX 5080 that read runs at
// 5.7 MB/s (11.8 s for 64 MiB). The port gives such a copy the card path
// instead (`CardHost::make_copy`): the card side in VRAM, its CPU side in
// the PC's memory, never read over the bus.
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
