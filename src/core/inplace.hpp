// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the view registry of crates/victory-gpu/src/inplace.rs
// (DanziLoader / victory-perf, see NOTICE.md): the title's views of its
// physical pages, as the runtime maps them. A write through one view of a
// page is a write of the bytes every other view shows (`watch` guards them
// all).
//
// Not ported yet: placing D3D12 heaps on title memory ("in place"). Until it
// is, nothing is ever placed, and `title_memory_freeing` has nothing to let
// go, exactly as the reference with nothing placed.
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "win.hpp"

namespace danzi::inplace {


void title_memory_section(usize section, std::uint64_t bytes, bool committed);


void title_memory_view(usize at, usize len, usize section, std::uint64_t offset);

void views_gone(usize at, usize len);


std::vector<usize> pages_of(usize page);

struct AliasBlocks {
    std::vector<usize> all_aliased;
    std::vector<usize> others;   
    std::uint64_t blocks_counted = 0;
};


AliasBlocks alias_blocks_in(usize from_block, usize to_block, usize max_views);


std::vector<std::pair<usize, usize>> views_of_section(usize section, std::uint64_t offset, usize len);


std::uint8_t level();

std::uint64_t most();


extern std::atomic<std::uint64_t> STATS[8];

void gone(bool window, std::uint64_t bytes);


enum class CoverKind { None, Holes, Scattered, Run };
struct Cover {
    CoverKind kind = CoverKind::None;
    usize section = 0;
    std::uint64_t offset = 0;
    bool operator==(const Cover&) const = default;
};
Cover cover(usize at, usize len);

bool private_whole(usize at, usize len);


void title_memory_freeing(usize at, usize len);
using FreeingHook = std::uint64_t (*)(void* context, std::uint64_t at, std::uint64_t len);
void set_freeing_hook(FreeingHook hook, void* context);


void tick(std::uint64_t now_ms);

}   
