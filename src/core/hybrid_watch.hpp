// SPDX-License-Identifier: GPL-3.0-or-later


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
