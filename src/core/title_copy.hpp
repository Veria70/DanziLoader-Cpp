// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the title-memory reads and writes of crates/victory-gpu/src/host.rs
// (`copy_mapped`, `read_sparse`, `read_pieces`, `read_into`, `read`,
// `write_mapped`, `for_pieces`; DanziLoader / victory-perf, see NOTICE.md).
//
// The reference goes through the kernel for every access
// (`NtReadVirtualMemory`, `NtWriteVirtualMemory`), which reports an
// unmapped page instead of faulting. Rust has no structured exception
// handling; C++ does. Here an access is first a plain copy that catches the
// fault; only when it faults does the reference's kernel path run, piece by
// piece, so what lands is the same. (Measured: write watching reports user
// and kernel accesses alike, `results/m2-copy-back`.)
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
