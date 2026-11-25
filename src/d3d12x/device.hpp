// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/device.rs, console.rs and dxgi.rs


#pragma once

#include "object.hpp"

namespace danzi::d3d12x {


inline constexpr Iid DEVICE_IID = guid(0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7});

inline constexpr std::uint32_t CHECKED_VERSION = 0x00020004;


bool is_device(const Iid* iid);
This device();


bool newer();


HRESULT __stdcall create_device(const void* adapter, const std::uint32_t* parameters, const Iid* iid, This* out);


struct VirtualMemory {
    std::uint32_t flags;
    std::uint32_t page_table_pages;
    std::uint32_t preserved_bytes;
    std::uint64_t lowest;
    std::uint64_t highest;
};
static_assert(sizeof(VirtualMemory) == 0x20);
HRESULT __stdcall configure_virtual_memory(VirtualMemory* config);


enum class Console { SeriesX, SeriesS };
void set_console(Console c);
Console current_console();

std::pair<std::size_t, std::size_t> title_memory(Console c);


This dxgi_device();
extern const std::array<Iid, 3> DXGI_DEVICE_IIDS;

}   
