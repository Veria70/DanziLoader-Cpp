// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/resource.rs (DanziLoader / victory-perf,
// see NOTICE.md): heaps, committed and placed resources, resources at title
// addresses, page pools, and resource sizes (allocation info, copyable
// footprints) from the console's texture layouts.
//
// Heaps and resources are host memory and a resource's GPU address is its
// CPU address, as on the console. Buffers are registered for the host
// copies (milestones 1-3's registry, once the host exists).
//
// Still to come (M4.4c, with the lists and descriptors that use them): the
// textures' host copies (made on first use), depth textures' colour twins,
// textures placed in title memory without a resource (`texture_at`),
// aliases, the host copies' memory accounting and idle release.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "formats/texture.hpp"
#include "object.hpp"

namespace danzi::d3d12x {

struct HeapProperties {
    std::uint32_t kind, cpu_pages, pool, creation_nodes, visible_nodes;
};
struct HeapDesc {
    std::uint64_t size;
    HeapProperties properties;
    std::uint64_t alignment;
    std::uint32_t flags;
};
struct ResourceDesc {
    std::uint32_t dimension;
    std::uint64_t alignment;
    std::uint64_t width;
    std::uint32_t height;
    std::uint16_t depth_or_array_size;
    std::uint16_t mip_levels;
    std::uint32_t format;
    std::uint32_t sample_count;
    std::uint32_t sample_quality;
    std::uint32_t layout;
    std::uint32_t flags;
};

std::string desc_debug(const ResourceDesc& d);
struct AllocationInfo {
    std::uint64_t size, alignment;
};
static_assert(sizeof(HeapProperties) == 20);
static_assert(sizeof(HeapDesc) == 48);
static_assert(sizeof(ResourceDesc) == 56);

inline constexpr std::uint32_t BUFFER = 1;
inline constexpr std::uint32_t TEXTURE_3D = 4;


bool element(std::uint32_t format, std::uint64_t& bytes, std::uint64_t& block);


bool texture_layout(const ResourceDesc& desc, formats::Texture& out, bool addressing);


using AllocRoutine = void*(__stdcall*)(std::size_t, std::uint64_t);
using FreeRoutine = void(__stdcall*)(void*, std::uint64_t);
using PageMapper = int(__stdcall*)(std::uint64_t, std::uint64_t, std::uint64_t);
void use_runtime(AllocRoutine alloc, FreeRoutine free, PageMapper mapper);
PageMapper page_mapper();


HRESULT __stdcall create_heap(This, const HeapDesc* desc, const Iid* iid, This* out);
HRESULT __stdcall create_committed(This, const HeapProperties* heap, std::uint32_t heap_flags, const ResourceDesc* desc, std::uint32_t state,
                                   const void* clear, const Iid* iid, This* out);
HRESULT __stdcall create_committed3(This, const HeapProperties* heap, std::uint32_t heap_flags, const ResourceDesc* desc, std::uint32_t layout,
                                    const void* clear, This protected_session, std::uint32_t castable_count, const std::uint32_t* castable,
                                    const Iid* iid, This* out);
HRESULT __stdcall create_placed(This, This heap, std::uint64_t offset, const ResourceDesc* desc, std::uint32_t state, const void* clear, const Iid* iid,
                                This* out);
HRESULT __stdcall create_at_address(This, std::uint64_t address, const ResourceDesc* desc, std::uint32_t layout, const void* clear, const Iid* iid,
                                    This* out);
HRESULT __stdcall register_page_pool(This, std::uint64_t address, std::uint32_t pages, std::size_t* out);
HRESULT __stdcall unregister_page_pool(This, std::size_t handle);
struct PlacedFootprint {
    std::uint64_t offset;
    std::uint32_t format, width, height, depth, row_pitch;
};
static_assert(sizeof(PlacedFootprint) == 32);
void __stdcall copyable_footprints(This, const ResourceDesc* desc, std::uint32_t first, std::uint32_t count, std::uint64_t base, PlacedFootprint* layouts,
                                   std::uint32_t* rows, std::uint64_t* row_sizes, std::uint64_t* total);


AllocationInfo* __stdcall allocation_info(AllocationInfo* out, This, std::uint32_t visible, std::uint32_t count, const ResourceDesc* descs);
struct ResourceDesc1 {
    ResourceDesc desc;
    std::uint32_t mip_region[3];
};
static_assert(sizeof(ResourceDesc1) == 72);
struct AllocationInfo1 {
    std::uint64_t offset, alignment, size;
};
AllocationInfo* __stdcall allocation_info2(AllocationInfo* out, This, std::uint32_t visible, std::uint32_t count, const ResourceDesc1* descs,
                                           AllocationInfo1* each);
HeapProperties* __stdcall custom_heap_properties(HeapProperties* out, This, std::uint32_t nodes, std::uint32_t kind);


struct PageRange {
    std::uint32_t kind, start, count;
};
struct PageBatch {
    std::uint64_t destination;
    std::uint32_t count;
    const PageRange* ranges;
};
HRESULT __stdcall copy_page_mappings(This, std::uint32_t count, const PageBatch* batches, std::uint64_t pool, std::uint32_t pool_pages,
                                     std::uint32_t flags);


bool texture_memory(This resource, const std::uint8_t*& data, ResourceDesc& desc);
bool buffer_bytes(This resource, std::uint8_t*& data, std::uint64_t& size);


struct HostTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    std::atomic<std::uint32_t> state;

    std::uint64_t bytes = 0;

    bool filled = false;

    std::uint32_t flags = 0;


    struct View {
        std::array<std::uint32_t, 6> desc;
        bool none;
        std::uint64_t handle;
    };
    static constexpr std::uint8_t VIEWS = 2;
    View views[2][VIEWS] = {};
    std::atomic<std::uint8_t> view_count[2] = {0, 0};
    std::mutex views_lock;
    HostTexture(Microsoft::WRL::ComPtr<ID3D12Resource> texture, std::uint32_t state);
    ~HostTexture();
};


struct Twin {
    std::shared_ptr<HostTexture> colour;


    std::atomic<std::uint32_t> behind{0};
    Microsoft::WRL::ComPtr<ID3D12Resource> through;
    std::shared_ptr<HostTexture> plane;
    ~Twin();
};


std::uint32_t pc_format(std::uint32_t format);


std::uint32_t target_format(std::uint32_t format);

std::uint32_t family(std::uint32_t format);


inline constexpr std::pair<std::uint32_t, std::uint32_t> SRGB[3] = {{29, 28}, {91, 87}, {93, 88}};
inline constexpr std::pair<std::uint32_t, std::uint32_t> DEPTH_FORMATS[4] = {{20, 19}, {40, 39}, {45, 44}, {55, 53}};

std::uint32_t legal_for(std::uint32_t flags, std::uint32_t state);
std::uint32_t legal_state(ID3D12Resource* texture, std::uint32_t state);


ID3D12Resource* host_resource(This resource);
std::shared_ptr<HostTexture> host_copy(This resource);


HostTexture* host_copy_raw(This resource);


void resources_let_go_idle();


std::array<std::uint64_t, 2> textures_let_go();


std::optional<std::pair<ID3D12Resource*, std::uint32_t>> host_transition(This resource, std::uint32_t wanted);

std::shared_ptr<Twin> twin(This resource);
ID3D12Resource* twin_resource(This resource);
ID3D12Resource* twin_plane_resource(This resource);


bool host_waits(This resource);


bool title_fills(This resource);

bool stands_in(This resource);

std::optional<std::uint32_t> view_format(This resource);

bool depth_and_unordered(This resource);


bool room();


This texture_at(std::uint64_t address, const std::uint32_t* desc);


This alias_of(This resource);

std::optional<std::uint64_t> placed_bytes(const std::uint32_t* desc);
std::optional<std::uint64_t> placed_bytes_from(const std::uint32_t* desc, std::uint32_t first);

std::uint32_t first_mip(const std::uint32_t* view, std::size_t words);


std::uint64_t moment();


struct BufferSink {
    virtual void made(std::uintptr_t resource, std::uint64_t start, std::uint64_t len) = 0;
    virtual void released(std::uintptr_t resource, std::uint64_t start) = 0;
};
void adopt_buffers(BufferSink* sink);

}   
