// SPDX-License-Identifier: GPL-3.0-or-later
// Memory primitives the reference does byte- or slice-wise, done wide, and
// title-memory access without a system call when nothing faults.
//
// Every routine here returns what the reference's code returns for the same
// bytes (each is checked against a plain byte loop by the unit tests and,
// through the selector and the copy-back, by the differential tests). The
// reference is built for baseline x86-64; the product already requires AVX2
// (reference README), and this file uses it.
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
