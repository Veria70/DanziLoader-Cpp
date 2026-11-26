// SPDX-License-Identifier: GPL-3.0-or-later
// Hybrid write tracking for write-watched title memory (on by default;
// `VICTORY_HYBRID_WATCH=0` turns it off). Not in the Rust reference: the
// idea is leonida's (`runtime/shims/d3d12_vulkan_device.cpp`, GPL-3), which
// measured 0.14 ms per refresh against 2.4 ms for asking `GetWriteWatch`
// about all of a 242 MiB heap at every submission.
//
// The reference asks the kernel about every page of a range at every
// refresh, written or not; the kernel walks all of it. Here a 64 KiB block
// the title has not written for a while is made read-only ("cold"): its next
// write faults, the vectored handler of `watch` gives the write back and notes
// the block written. Only the blocks written lately ("hot") are asked about.
//
// What a refresh is told is never less than the reference is told: every
// page written since the last asking is reported, by a fault or by the
// kernel. It may be told less often that a page was written when nothing
// changed in it (the kernel's write watch also reports pages that a read
// brought in for the first time). The contents-equivalence mode of
// `dirty-diff` checks the result against the reference.
//
// A block that faults soon after going cold (within its own idle limit,
// counted in its own askings) waits twice as long the next time, up to 1024
// askings (`VICTORY_HYBRID_BACKOFF=0`: never); `VICTORY_HYBRID_IDLE` sets the
// default limit (16).
//
// Assumptions, as for the guards of view memory: the title's memory is
// read-write, the runtime tells `watch::writing` before a kernel write into
// it and `watch::title_memory_remapped` when a mapping changes (commit,
// decommit, protection).
#pragma once

#include <cstdint>
#include <vector>

#include "win.hpp"

namespace danzi::hybrid {

bool enabled();


bool written(usize first, usize last, std::vector<usize>& out);


bool on_write_fault(usize at);


void opening(usize at, usize len);


void forget(usize at, usize len);

struct Stats {
    std::uint64_t faults, made_cold, kernel_askings, kernel_pages;
};
Stats stats();

namespace detail {

void set_enabled_for_tests(int on);
}

}   
