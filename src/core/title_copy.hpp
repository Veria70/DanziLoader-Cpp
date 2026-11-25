// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the title-memory reads and writes of crates/victory-gpu/src/host.rs


#pragma once

#include "win.hpp"

namespace danzi::host {


void copy_into_host(std::uint8_t* dst, const std::uint8_t* src, usize len);


bool read_into(const std::uint8_t* src, std::uint8_t* dst, usize len);


void write_mapped(std::uint8_t* dst, const std::uint8_t* src, usize len);


using WriteObserver = void (*)(void* context, usize at, usize len);
void set_write_observer(WriteObserver observer, void* context);


bool fast_access();

}   
