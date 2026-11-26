// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/indirect.rs (DanziLoader / victory-perf,
// see NOTICE.md): indirect execution's arguments, bounded before the GPU
// reads them. Active records are copied and clamped on the GPU into a
// device-local scratch buffer just before execution, by a compute shader of
// ours, leaving the title's argument and count buffers untouched; each list
// has a budget of work past which records run empty.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <optional>
#include <utility>

namespace danzi::d3d12x {

class HostList;


constexpr std::uint32_t SPLIT = 2;
constexpr std::uint32_t PART = 65'535;
constexpr std::uint64_t PART_BYTES = 12;


struct Spec {
    std::uint32_t kind, stride, records;
};
ID3D12Resource* bound(HostList& list, ID3D12Resource* src, std::uint64_t offset, std::optional<std::pair<ID3D12Resource*, std::uint64_t>> count,
                      Spec spec, bool own);

ID3D12Resource* bound_ahead(HostList& list, ID3D12Resource* src, std::uint64_t offset, Spec spec, bool own);

ID3D12Resource* bound_split(HostList& list, ID3D12Resource* src, std::uint64_t offset, bool own);

void indirect_done(HostList& list, ID3D12Resource* scratch);

}   
