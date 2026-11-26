// SPDX-License-Identifier: GPL-3.0-or-later
// Title memory by address (`memory.rs`; DanziLoader / victory-perf, see
// NOTICE.md): for translated shaders that read and write through raw
// 40-bit addresses (`SLOT_MEMORY`: the console's ray-tracing structure
// builder and the programs that walk its structures).
#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace danzi::d3d12x {

class HostList;
class HostHeap;


struct MemoryReady {
    std::uint64_t directory, faults;
    std::uint32_t base;
};


std::optional<MemoryReady> memory_ready(HostList& list, HostHeap& heap, std::span<const std::uint64_t> addresses,
                                        std::span<const std::uint32_t> constants, bool writes);


std::uint64_t memory_copies();

}   
