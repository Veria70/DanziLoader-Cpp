// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the D3D12 side of crates/victory-gpu/src/inplace.rs (DanziLoader
// / victory-perf, see NOTICE.md): title memory as the card's own memory. A
// host heap is opened on a committed part of the title's private memory
// (`OpenExistingHeapFromAddress1`, by address: review #13, NVIDIA's driver
// 591.59 leaked 512 MB of commit per section opened from a file mapping)
// and a buffer is placed over it, so that shaders read and write the
// title's bytes and no copy stands between.
#pragma once

#include <cstdint>
#include <optional>

#include "device.hpp"

namespace danzi::gpu::inplace {


struct Placed {
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12Heap> heap;
    std::uint64_t gpu = 0;    
    std::uint64_t bias = 0;   
};


std::optional<Placed> place_part(Device& device, std::uint64_t start, std::uint64_t len, HRESULT* hr = nullptr);


std::optional<Placed> place_for(Device& device, std::uint64_t start, std::uint64_t len, bool window);

}   
