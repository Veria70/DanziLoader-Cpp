// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/descriptor.rs (DanziLoader / victory-perf,
// see NOTICE.md): descriptor heaps, zeroed memory with handles that are plain
// addresses (CPU and GPU alike, as on the console); views and samplers
// recorded in descriptor memory as a tag and two words naming the view (whole
// for constant buffers and placed memory, else an entry of the table of
// views, with a generation that goes stale when the resource goes); render
// target and depth views as records of their own; copies.
//
// Still to come (with the host, M4.7): the host heaps that mirror shader-
// visible heaps (`descriptor/host.rs`). Until then a write into a
// shader-visible heap is noted as pending, as the reference notes it until
// the host heap is made.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "object.hpp"

namespace danzi::d3d12x {

struct DescriptorHeapDesc {
    std::uint32_t kind, count, flags, nodes;
};


HRESULT __stdcall create_descriptor_heap(This, const DescriptorHeapDesc* desc, const Iid* iid, This* out);
std::uint32_t __stdcall descriptor_increment_size(This, std::uint32_t kind);
void __stdcall create_cbv(This, const std::uint32_t* desc, std::uint64_t handle);
void __stdcall create_srv(This, This resource, const std::uint32_t* desc, std::uint64_t handle);
void __stdcall create_uav(This, This resource, This counter, const std::uint32_t* desc, std::uint64_t handle);
void __stdcall create_rtv(This, This resource, const std::uint32_t* desc, std::uint64_t handle);
void __stdcall create_dsv(This, This resource, const std::uint32_t* desc, std::uint64_t handle);
void __stdcall create_placed_srv(This, const std::uint32_t* resource, const std::uint32_t* desc, std::uint64_t handle);
void __stdcall create_placed_uav(This, const std::uint32_t* resource, const std::uint32_t* desc, std::uint64_t handle);
void __stdcall create_sampler(This, const std::uint32_t* desc, std::uint64_t handle);
void __stdcall create_sampler_x(This, const std::uint32_t* desc, std::uint64_t handle);
void __stdcall copy_descriptors_simple(This, std::uint32_t count, std::uint64_t dst, std::uint64_t src, std::uint32_t kind);
void __stdcall copy_descriptors(This, std::uint32_t dst_ranges, const std::uint64_t* dst_starts, const std::uint32_t* dst_sizes, std::uint32_t src_ranges,
                                const std::uint64_t* src_starts, const std::uint32_t* src_sizes, std::uint32_t kind);


struct RtvRecord {
    std::uint32_t tag;
    std::uint32_t described;
    This resource;
    std::uint32_t desc[6];


    std::optional<std::array<std::uint32_t, 6>> host_desc(bool depth) const;
};
std::optional<RtvRecord> rtv_record(std::uint64_t handle);
std::optional<RtvRecord> dsv_record(std::uint64_t handle);


void forget_views(std::uintptr_t resource);


struct View {
    enum Kind : std::uint8_t { Constant, Shader, Unordered, Sampler, ConsoleSampler, Placed } kind;

    std::uint64_t a = 0, b = 0;


    bool flag = false;


    std::array<std::uint32_t, 30> words;

    bool operator==(const View& o) const;
};

std::string view_debug(const View& v, bool hex = false);

std::optional<View> view(std::uint64_t handle);


constexpr std::uint32_t VIEW_RECORD_TAG = 0x45495676;   
constexpr std::uint32_t VIEW_KIND_SHIFT = 30;
constexpr std::uint32_t VIEW_KIND_CONSTANT = 1;

std::optional<View> view_at_words(std::uint32_t i, std::uint32_t generation);


struct DescHeap;
class HostHeap;


bool heap_samplers(const DescHeap& heap);
std::uint64_t heap_start(const DescHeap& heap);
std::optional<std::uint32_t> heap_index(const DescHeap& heap, std::uint64_t handle);
std::optional<std::uint64_t> heap_handle(const DescHeap& heap, std::uint32_t index);
HostHeap* heap_host(DescHeap& heap);
inline DescHeap& heap_of_object(This object) { return *reinterpret_cast<DescHeap*>(object); }


void release_dynamic(std::uintptr_t heap, std::uint32_t first, std::uint32_t n);


std::uint64_t evict_kept_resources(const std::vector<std::uintptr_t>& victims);


void texture_made(std::uintptr_t resource);
void make_waiting();


void wait_for(std::uintptr_t resource, std::uint64_t handle);

}   
