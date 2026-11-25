// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the D3D12 side of crates/victory-gpu/src/inplace.rs (DanziLoader


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
