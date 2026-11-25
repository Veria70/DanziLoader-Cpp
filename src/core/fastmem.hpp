// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "win.hpp"

namespace danzi::fastmem {


bool try_copy(void* dst, const void* src, usize n) noexcept;


bool equal(const std::uint8_t* a, const std::uint8_t* b, usize n) noexcept;


std::optional<usize> first_difference(const std::uint8_t* a, const std::uint8_t* b, usize n) noexcept;
std::optional<usize> last_difference(const std::uint8_t* a, const std::uint8_t* b, usize n) noexcept;


bool merge_gpu_into_title(std::uint8_t* title, std::uint8_t* host, const std::uint8_t* old, usize n) noexcept;


bool merge_piece(const std::uint8_t* now, std::uint8_t* held, std::uint8_t* title, usize n) noexcept;


bool merge_card(std::uint8_t* title, const std::uint8_t* gpu, std::uint8_t* old, usize n) noexcept;


std::vector<std::pair<usize, usize>> changed_runs(const std::uint8_t* now, const std::uint8_t* was, usize n);

}   
