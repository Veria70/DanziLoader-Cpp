// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the window choice of crates/victory-gpu/src/shadow.rs
// (DanziLoader / victory-perf, see NOTICE.md): title memory outside any
// title buffer gets copies of aligned windows of it. Pure: which window an
// ask of [address, end) takes (`window_of`, tails, `holder`).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "win.hpp"

namespace danzi::windows {

using Window = std::pair<std::uint64_t, std::uint64_t>;   


inline constexpr std::uint64_t WINDOW = std::uint64_t{4} << 20;

inline constexpr std::uint64_t TAIL_STEP = std::uint64_t{256} << 10;


std::uint64_t window();
std::uint64_t grid(std::optional<std::uint64_t> kb);

Window window_of(std::uint64_t address, std::uint64_t end, std::uint64_t grid);

bool tails();
Window tail_of(std::uint64_t address, std::uint64_t end);


std::optional<Window> holder(std::span<const Window> windows, std::uint64_t address, std::uint64_t end);

bool fine();

std::uint64_t cover(std::span<const Window> windows);

std::optional<Window> longest_tail(std::span<const Window> windows, std::uint64_t address, std::uint64_t end);


std::uint64_t window_card_most();

}   
