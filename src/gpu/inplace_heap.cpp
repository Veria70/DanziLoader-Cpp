// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the D3D12 side of crates/victory-gpu/src/inplace.rs (see
// inplace_heap.hpp).
#include "inplace_heap.hpp"

#include "core/inplace.hpp"
#include "core/log.hpp"

namespace danzi::gpu::inplace {

namespace {

constexpr std::uint64_t K64 = std::uint64_t{64} << 10;

}   

std::optional<Placed> place_part(Device& device, std::uint64_t start, std::uint64_t len, HRESULT* hr) {
    if (hr) {
        *hr = S_OK;
    }
    std::uint64_t at = start & ~(K64 - 1);
    std::uint64_t bytes = (start + std::max<std::uint64_t>(len, 1) + K64 - 1) / K64 * K64 - at;
    if (!danzi::inplace::private_whole(static_cast<usize>(at), static_cast<usize>(bytes))) {
        return std::nullopt;
    }
    ComPtr<ID3D12Device13> d13;
    HRESULT r = device.device()->QueryInterface(IID_PPV_ARGS(&d13));
    Placed p;
    if (SUCCEEDED(r)) {
        r = d13->OpenExistingHeapFromAddress1(reinterpret_cast<const void*>(at), static_cast<SIZE_T>(bytes), IID_PPV_ARGS(&p.heap));
    }
    if (SUCCEEDED(r)) {

        auto desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        r = device.device()->CreatePlacedResource(p.heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.buffer));
    }
    if (FAILED(r)) {
        if (hr) {
            *hr = r;
        }
        return std::nullopt;
    }
    p.gpu = p.buffer->GetGPUVirtualAddress() + (start - at);
    p.bias = start - at;
    return p;
}

std::optional<Placed> place_for(Device& device, std::uint64_t start, std::uint64_t len, bool window) {
    using danzi::inplace::STATS;
    if (danzi::inplace::level() < (window ? 1 : 2)) {
        return std::nullopt;
    }

    if (danzi::inplace::cover(static_cast<usize>(start), static_cast<usize>(len)).kind != danzi::inplace::CoverKind::None) {
        STATS[4].fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    if (STATS[1].load(std::memory_order_relaxed) + STATS[3].load(std::memory_order_relaxed) + len > danzi::inplace::most()) {
        STATS[6].fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    HRESULT hr = S_OK;
    auto placed = place_part(device, start, len, &hr);
    if (placed) {
        usize at = window ? 0 : 2;
        STATS[at].fetch_add(1, std::memory_order_relaxed);
        STATS[at + 1].fetch_add(len, std::memory_order_relaxed);
        return placed;
    }
    if (hr == S_OK) {
        STATS[4].fetch_add(1, std::memory_order_relaxed);
    } else if (STATS[5].fetch_add(1, std::memory_order_relaxed) < 8) {
        log::warn("victory_gpu::inplace", "title memory not opened in place: a copy instead",
                  {{"start", log::hex(start)}, {"len", log::debug(len)}, {"error", log::hresult(hr)}});
    }
    return std::nullopt;
}

}   
