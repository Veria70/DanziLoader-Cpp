// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/resource.rs (see resource.hpp).
#include "resource.hpp"

#include <windows.h>

#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <array>
#include <map>
#include <tuple>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include "core/readback.hpp"
#include "core/env.hpp"
#include "core/log.hpp"
#include "core/win.hpp"
#include "host.hpp"
#include "texture.hpp"
#include "descriptor.hpp"
#include "resource_iids.hpp"

namespace danzi::d3d12x {

namespace {

const std::array<Iid, 4> HEAP_IIDS = {
    guid(0x6b3b2502, 0x6e51, 0x45b3, {0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3}),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};
const std::array<Iid, 4> RESOURCE_IIDS = {
    guid(0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};

constexpr std::uint64_t KIB_64 = 64 << 10;
constexpr std::uint64_t MIB_4 = 4 << 20;

bool power_of_two(std::uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

constexpr std::string_view LOG_TARGET = "victory_gpu::resource";


bool next_multiple(std::uint64_t v, std::uint64_t of, std::uint64_t& out) {
    std::uint64_t r = v % of;
    if (r == 0) {
        out = v;
        return true;
    }
    std::uint64_t add = of - r;
    if (v > UINT64_MAX - add) {
        return false;
    }
    out = v + add;
    return true;
}

bool mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }
    out = a * b;
    return true;
}

std::uint64_t div_ceil(std::uint64_t a, std::uint64_t b) { return a / b + (a % b != 0); }


std::uint32_t mip_count(const ResourceDesc& d) {
    std::uint64_t depth = d.dimension == TEXTURE_3D ? d.depth_or_array_size : 1;
    if (d.mip_levels != 0) {
        return d.mip_levels;
    }
    std::uint64_t biggest = std::max({d.width, std::uint64_t{d.height}, depth, std::uint64_t{1}});
    return 64 - static_cast<std::uint32_t>(std::countl_zero(biggest));
}


bool footprint(const ResourceDesc& desc, AllocationInfo& out) {
    if (desc.dimension != BUFFER) {
        formats::Texture t;
        if (texture_layout(desc, t, false)) {


            std::uint64_t alignment = std::max(desc.alignment, t.alignment);
            if (!power_of_two(alignment)) {
                return false;
            }
            std::uint64_t size;
            if (!next_multiple(t.size, alignment, size)) {
                return false;
            }
            out = {size, alignment};
            return true;
        }
    }
    std::uint64_t bytes;
    if (desc.dimension == BUFFER) {
        bytes = desc.width;
    } else {
        std::uint64_t element_bytes, block;
        if (!element(desc.format, element_bytes, block)) {
            return false;
        }
        std::uint64_t depth = desc.dimension == TEXTURE_3D ? desc.depth_or_array_size : 1;
        std::uint64_t slices = desc.dimension == TEXTURE_3D ? 1 : desc.depth_or_array_size;
        std::uint64_t height = desc.height;
        std::uint32_t mips = std::min(mip_count(desc), 64u);
        std::uint64_t chain = 0;
        for (std::uint32_t mip = 0; mip < mips; ++mip) {
            std::uint64_t w = div_ceil(std::max(desc.width >> mip, std::uint64_t{1}), block);
            std::uint64_t h = div_ceil(std::max(height >> mip, std::uint64_t{1}), block);
            std::uint64_t d = std::max(depth >> mip, std::uint64_t{1});
            std::uint64_t one;
            if (!mul(w, h, one) || !mul(one, d, one) || !mul(one, element_bytes, one) || chain > UINT64_MAX - one) {
                return false;
            }
            chain += one;
        }
        if (!mul(chain, slices, bytes) || !mul(bytes, std::max(desc.sample_count, 1u), bytes)) {
            return false;
        }
    }
    if (bytes == 0) {
        return false;
    }
    std::uint64_t minimum = desc.sample_count > 1 ? MIB_4 : KIB_64;
    std::uint64_t alignment = std::max(desc.alignment, minimum);
    if (!power_of_two(alignment)) {
        return false;
    }
    std::uint64_t size;
    if (!next_multiple(bytes, KIB_64, size)) {
        return false;
    }
    out = {size, alignment};
    return true;
}


AllocRoutine ALLOC = nullptr;
FreeRoutine FREE = nullptr;
PageMapper MAPPER = nullptr;


struct Memory {
    enum class Own : std::uint8_t { None, Title, Pages, Heap };
    std::uint8_t* ptr = nullptr;
    std::uint64_t size = 0;
    std::uint64_t attributes = 0;
    Own own = Own::None;

    Memory() = default;
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&& o) noexcept : ptr(o.ptr), size(o.size), attributes(o.attributes), own(o.own) { o.own = Own::None; }
    ~Memory() { release(); }

    void release() {
        switch (own) {
            case Own::Title:
                if (FREE) {
                    FREE(ptr, attributes);
                }
                break;
            case Own::Pages: VirtualFree(ptr, 0, MEM_RELEASE); break;
            case Own::Heap: _aligned_free(ptr); break;
            default: break;
        }
        own = Own::None;
    }


    static HRESULT ours(std::uint64_t size, std::uint64_t alignment, Memory& out) {
        if (size == 0 || !power_of_two(alignment) || size > static_cast<std::uint64_t>(PTRDIFF_MAX) - alignment) {
            return E_INVALIDARG_;
        }
        if (size >= KIB_64 && alignment <= KIB_64) {
            void* p = VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!p) {
                return E_OUTOFMEMORY_;
            }
            out.ptr = static_cast<std::uint8_t*>(p);
            out.own = Own::Pages;
        } else {
            void* p = _aligned_malloc(static_cast<std::size_t>(size), static_cast<std::size_t>(alignment));
            if (!p) {
                return E_OUTOFMEMORY_;
            }
            std::memset(p, 0, static_cast<std::size_t>(size));
            out.ptr = static_cast<std::uint8_t*>(p);
            out.own = Own::Heap;
        }
        out.size = size;


        if (size >= std::uint64_t{32} << 20) {
            log::info(LOG_TARGET, "memory for the title from our allocator (a heap or a descriptor heap)",
                      {{"mb", log::debug(size >> 20)}, {"at", log::hex(reinterpret_cast<std::uintptr_t>(out.ptr))}});
        }
        return S_OK_;
    }


    static std::uint64_t resource_attributes(AllocationInfo info, std::uint32_t heap_kind) {
        constexpr std::uint64_t GRAPHICS_ALLOCATOR = std::uint64_t{128} << 16;
        constexpr std::uint32_t DEFAULT_HEAP = 1;
        std::uint64_t pages = info.size >= (2u << 20) ? 2 : 1;
        std::uint64_t memory = heap_kind == DEFAULT_HEAP ? 1 : 3;
        std::uint64_t alignment = static_cast<std::uint64_t>(std::countr_zero(std::max(info.alignment, std::uint64_t{1})));
        return GRAPHICS_ALLOCATOR | pages << 14 | alignment << 24 | memory << 29;
    }


    static HRESULT for_resource(AllocationInfo info, std::uint32_t heap_kind, Memory& out) {
        if (!ALLOC) {
            return ours(info.size, info.alignment, out);
        }
        if (!power_of_two(info.alignment) || info.size > static_cast<std::uint64_t>(PTRDIFF_MAX) - (info.alignment - 1)) {
            return E_INVALIDARG_;
        }
        std::uint64_t attributes = resource_attributes(info, heap_kind);
        void* p = ALLOC(static_cast<std::size_t>(info.size), attributes);
        if (!p) {
            return E_OUTOFMEMORY_;
        }
        std::memset(p, 0, static_cast<std::size_t>(info.size));
        out.ptr = static_cast<std::uint8_t*>(p);
        out.size = info.size;
        out.attributes = attributes;
        out.own = Own::Title;
        return S_OK_;
    }
};


struct Heap {
    Object object;
    HeapDesc desc;
    Memory memory;
};

HRESULT __stdcall heap_query(This self, const Iid* iid, This* out) { return answer(HEAP, iid, is(iid, HEAP_IIDS) ? self : nullptr, out); }
std::uint32_t __stdcall heap_release(This self) { return release_boxed<Heap>(self); }
void __stdcall heap_desc(This self, HeapDesc* out) {
    if (out) {
        *out = reinterpret_cast<Heap*>(self)->desc;
    }
}

const void* const* heap_functions() {

    static const Table<24> table = [] {
        auto f = d3d12_object(missing_table<HEAP, 24>());
        f[0] = fn(&heap_query);
        f[2] = fn(&heap_release);
        f[7] = fn(&parent_device);
        f[8] = fn(&heap_desc);
        return f;
    }();
    return table.data();
}


danzi::FastLock BUFFERS_LOCK;
BufferSink* SINK = nullptr;

std::map<std::pair<std::uint64_t, std::uintptr_t>, std::uint64_t> PENDING;

void register_buffer(std::uintptr_t resource, std::uint64_t start, std::uint64_t len) {
    std::lock_guard lock(BUFFERS_LOCK);
    if (SINK) {
        SINK->made(resource, start, len);
    } else {
        PENDING[{start, resource}] = len;
    }
}

void unregister_buffer(std::uintptr_t resource, std::uint64_t start) {
    std::lock_guard lock(BUFFERS_LOCK);
    if (SINK) {
        SINK->released(resource, start);
    } else {
        PENDING.erase({start, resource});
    }
}


danzi::FastLock TEXTURES_LOCK;
std::set<std::uintptr_t> TEXTURES;
std::map<std::uint64_t, std::uintptr_t> TARGETS_AT;


bool fills(const ResourceDesc& desc) {
    constexpr std::uint32_t TARGET_DEPTH_UNORDERED = 0x7;
    return desc.dimension != BUFFER && (desc.flags & TARGET_DEPTH_UNORDERED) == 0;
}


struct Resource;
struct Made {
    std::shared_ptr<HostTexture> host;
    std::shared_ptr<Twin> twin;
};
Made hosted(This self, bool make);
bool lazy();

struct Resource {
    Object object;


    std::uint8_t* at = nullptr;
    ResourceDesc desc;
    HeapProperties heap;
    std::uint32_t heap_flags;


    enum class Backing : std::uint8_t { Committed, Placed, Address } backing;
    Memory memory;
    This placed_heap = nullptr;
    std::uint64_t offset = 0;
    std::uint8_t* address = nullptr;


    std::uint32_t state = 0;
    bool has_clear = false;
    std::uint32_t clear[5]{};


    danzi::FastLock made_lock;
    std::shared_ptr<HostTexture> made;
    std::shared_ptr<Twin> made_twin;
    std::atomic<std::uint64_t> used{0};
    std::atomic<bool> refused{false}, gone{false}, placed{false};

    std::uint8_t* data() const { return at; }

    std::uint8_t* where() const {
        switch (backing) {
            case Backing::Committed: return memory.ptr;
            case Backing::Placed: return reinterpret_cast<Heap*>(placed_heap)->memory.ptr + offset;
            default: return address;
        }
    }

    ~Resource() {
        auto self = reinterpret_cast<std::uintptr_t>(this);

        if (desc.dimension != BUFFER) {
            std::lock_guard lock(TEXTURES_LOCK);
            TEXTURES.erase(self);
            if (!fills(desc)) {
                auto memory_at = reinterpret_cast<std::uint64_t>(data());
                auto it = TARGETS_AT.find(memory_at);
                if (it != TARGETS_AT.end() && it->second == self) {
                    TARGETS_AT.erase(it);
                }
            }
        } else {
            danzi::readback::untag(reinterpret_cast<std::uint64_t>(data()));
        }


        if (made) {
            retire(std::move(made));
        }
        if (made_twin) {
            retire(std::move(made_twin));
        }
        texture_forget(self);

        forget_views(self);
        if (desc.dimension == BUFFER) {
            unregister_buffer(self, reinterpret_cast<std::uint64_t>(data()));
        }
        if (backing == Backing::Placed) {
            heap_release(placed_heap);
        }
    }
};

Resource& resource(This self) { return *reinterpret_cast<Resource*>(self); }

HRESULT __stdcall resource_query(This self, const Iid* iid, This* out) { return answer(RESOURCE, iid, is(iid, RESOURCE_IIDS) ? self : nullptr, out); }
std::uint32_t __stdcall resource_release(This self) { return release_boxed<Resource>(self); }


#pragma code_seg(push, ".text$d3d12x_resource")

HRESULT __stdcall map(This self, std::uint32_t, const void*, std::uint8_t** out) {
    if (out) {
        *out = resource(self).data();
    }
    return S_OK_;
}
void __stdcall unmap(This, std::uint32_t, const void*) { DANZI_EMPTY_BODY; }
void __stdcall resource_desc(This self, ResourceDesc* out) {
    if (out) {


        const auto* from = reinterpret_cast<const std::uint8_t*>(&resource(self).desc);
        auto* to = reinterpret_cast<std::uint8_t*>(out);


        __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(from));
        __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + 16));
        __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + 32));
        std::uint64_t last;
        std::memcpy(&last, from + 48, 8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + 16), b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + 32), c);
        std::memcpy(to + 48, &last, 8);
    }
}
std::uint64_t __stdcall gpu_address(This self) { return reinterpret_cast<std::uint64_t>(resource(self).data()); }
#pragma code_seg(pop)
HRESULT __stdcall heap_properties(This self, HeapProperties* properties, std::uint32_t* flags) {
    const Resource& r = resource(self);
    if (properties) {
        *properties = r.heap;
    }
    if (flags) {
        *flags = r.heap_flags;
    }
    return S_OK_;
}

const void* const* resource_functions() {


    static const Table<32> table = [] {
        auto f = d3d12_object(missing_table<RESOURCE, 32>());
        f[0] = fn(&resource_query);
        f[2] = fn(&resource_release);
        f[7] = fn(&parent_device);
        f[8] = fn(&map);
        f[9] = fn(&unmap);
        f[10] = fn(&resource_desc);
        f[11] = fn(&gpu_address);
        f[14] = fn(&heap_properties);
        return f;
    }();
    return table.data();
}
const bool MADE = (heap_functions(), resource_functions(), true);

HRESULT hand_out_resource(const ResourceDesc& desc, const HeapProperties& heap, std::uint32_t heap_flags, Resource::Backing backing, Memory&& memory,
                          This placed_heap, std::uint64_t offset, std::uint8_t* address, std::uint32_t state, const void* clear, const Iid* iid,
                          This* out) {
    auto* r = new Resource{Object{resource_functions()}, nullptr, desc, heap, heap_flags, backing, std::move(memory), placed_heap, offset, address, state};
    r->at = r->where();
    if (clear) {
        r->has_clear = true;
        std::memcpy(r->clear, clear, sizeof r->clear);
    }
    auto self = reinterpret_cast<std::uintptr_t>(r);

    if (desc.dimension != BUFFER) {
        {
            std::lock_guard lock(TEXTURES_LOCK);
            TEXTURES.insert(self);
            if (!fills(desc)) {
                TARGETS_AT.insert_or_assign(reinterpret_cast<std::uint64_t>(r->data()), self);
            }
        }
        if (!lazy()) {
            hosted(reinterpret_cast<This>(r), true);
        }
    } else {
        register_buffer(self, reinterpret_cast<std::uint64_t>(r->data()), desc.width);


    }
    return hand_out(RESOURCE, r, iid, RESOURCE_IIDS, out);
}


danzi::FastLock POOLS_LOCK;
std::vector<std::optional<std::pair<std::uint64_t, std::uint32_t>>> PAGE_POOLS;

}   

bool element(std::uint32_t format, std::uint64_t& bytes, std::uint64_t& block) {
    auto in = [&](std::uint32_t lo, std::uint32_t hi) { return format >= lo && format <= hi; };
    block = 1;
    if (in(1, 4)) {
        bytes = 16;
    } else if (in(5, 8)) {
        bytes = 12;
    } else if (in(9, 22)) {
        bytes = 8;
    } else if (in(23, 47) || format == 67 || in(87, 93) || format == 116 || format == 117 || format == 189 || format == 191) {
        bytes = 4;
    } else if (in(118, 120)) {
        bytes = 3;
    } else if (in(48, 59) || format == 68 || format == 69 || format == 85 || format == 86 || format == 115) {
        bytes = 2;
    } else if (in(60, 65) || format == 190) {
        bytes = 1;
    } else if (in(70, 72) || in(79, 81)) {
        bytes = 8;
        block = 4;
    } else if (in(73, 78) || in(82, 84) || in(94, 99)) {
        bytes = 16;
        block = 4;
    } else {
        return false;
    }
    return true;
}

bool texture_layout(const ResourceDesc& desc, formats::Texture& out, bool addressing) {
    constexpr std::uint32_t TEXTURE_1D = 2, TEXTURE_2D = 3;
    constexpr std::uint32_t RENDER_TARGET = 1, DEPTH = 2;
    formats::Dimension dimension;
    switch (desc.dimension) {
        case TEXTURE_1D: dimension = formats::Dimension::Tex1d; break;
        case TEXTURE_2D: dimension = formats::Dimension::Tex2d; break;
        case TEXTURE_3D: dimension = formats::Dimension::Tex3d; break;
        default: return false;
    }
    std::uint32_t swizzle;
    if (desc.layout == 0) {
        swizzle = dimension == formats::Dimension::Tex1d ? formats::Swizzle::LINEAR
                  : (desc.flags & DEPTH) != 0            ? formats::Swizzle::SW_64KB_Z_X
                  : (desc.flags & RENDER_TARGET) != 0    ? formats::Swizzle::SW_64KB_R_X
                                                         : formats::Swizzle::SW_4KB_S;
    } else if (desc.layout == 1) {
        swizzle = formats::Swizzle::LINEAR;
    } else if ((desc.layout & ~0xffu) == 0x100) {
        swizzle = desc.layout & 0xff;
    } else {
        return false;
    }
    if (desc.width > UINT32_MAX) {
        return false;
    }
    formats::TextureDesc t{dimension,
                           static_cast<std::uint32_t>(desc.width),
                           desc.height,
                           desc.depth_or_array_size,
                           desc.mip_levels,
                           desc.format,
                           std::max(desc.sample_count, 1u),
                           formats::Swizzle{swizzle}};
    return formats::Texture::make(t, out, addressing) == formats::Error::None;
}

void use_runtime(AllocRoutine alloc, FreeRoutine free, PageMapper mapper) {

    if (!ALLOC) {
        ALLOC = alloc;
        FREE = free;
    }
    if (!MAPPER) {
        MAPPER = mapper;
    }
}

PageMapper page_mapper() { return MAPPER; }

std::string desc_debug(const ResourceDesc& d) {
    char b[320];
    std::snprintf(b, sizeof b,
                  "ResourceDesc { dimension: %u, alignment: %llu, width: %llu, height: %u, depth_or_array_size: %u, mip_levels: %u, format: %u, "
                  "sample_count: %u, sample_quality: %u, layout: %u, flags: %u }",
                  d.dimension, static_cast<unsigned long long>(d.alignment), static_cast<unsigned long long>(d.width), d.height,
                  unsigned{d.depth_or_array_size}, unsigned{d.mip_levels}, d.format, d.sample_count, d.sample_quality, d.layout, d.flags);
    return b;
}

HRESULT __stdcall create_heap(This, const HeapDesc* desc, const Iid* iid, This* out) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    if (desc->alignment != 0 && !power_of_two(desc->alignment)) {
        return E_INVALIDARG_;
    }
    Memory memory;
    if (HRESULT hr = Memory::ours(desc->size, std::max(desc->alignment, KIB_64), memory); hr != S_OK_) {
        return hr;
    }
    auto* h = new Heap{Object{heap_functions()}, *desc, std::move(memory)};
    return hand_out(HEAP, h, iid, HEAP_IIDS, out);
}

HRESULT __stdcall create_committed(This, const HeapProperties* heap, std::uint32_t heap_flags, const ResourceDesc* desc, std::uint32_t state,
                                   const void* clear, const Iid* iid, This* out) {
    if (!heap || !desc) {
        return E_INVALIDARG_;
    }
    AllocationInfo info;
    if (!footprint(*desc, info)) {
        log::warn(LOG_TARGET, "no size for this resource", {{"desc", desc_debug(*desc)}});
        return E_INVALIDARG_;
    }
    Memory memory;
    if (HRESULT hr = Memory::for_resource(info, heap->kind, memory); hr != S_OK_) {
        return hr;
    }
    return hand_out_resource(*desc, *heap, heap_flags, Resource::Backing::Committed, std::move(memory), nullptr, 0, nullptr, state, clear, iid, out);
}


HRESULT __stdcall create_committed3(This self, const HeapProperties* heap, std::uint32_t heap_flags, const ResourceDesc* desc, std::uint32_t,
                                    const void* clear, This, std::uint32_t, const std::uint32_t*, const Iid* iid, This* out) {
    constexpr std::uint32_t COMMON = 0;
    return create_committed(self, heap, heap_flags, desc, COMMON, clear, iid, out);
}

HRESULT __stdcall register_page_pool(This, std::uint64_t address, std::uint32_t pages, std::size_t* out) {
    if (!out || pages == 0 || address % KIB_64 != 0) {
        return E_INVALIDARG_;
    }
    std::lock_guard lock(POOLS_LOCK);
    PAGE_POOLS.emplace_back(std::pair{address, pages});
    *out = PAGE_POOLS.size();
    return S_OK_;
}

HRESULT __stdcall unregister_page_pool(This, std::size_t handle) {
    std::lock_guard lock(POOLS_LOCK);
    std::size_t i = handle - 1;   
    if (i < PAGE_POOLS.size() && PAGE_POOLS[i]) {
        PAGE_POOLS[i].reset();
        return S_OK_;
    }
    return E_INVALIDARG_;
}


HRESULT __stdcall create_at_address(This, std::uint64_t address, const ResourceDesc* desc, std::uint32_t, const void* clear, const Iid* iid,
                                    This* out) {
    if (!desc || address == 0) {
        return E_INVALIDARG_;
    }
    constexpr std::uint32_t COMMON = 0;
    constexpr HeapProperties DEFAULT{1, 0, 0, 1, 1};
    return hand_out_resource(*desc, DEFAULT, 0, Resource::Backing::Address, Memory{}, nullptr, 0, reinterpret_cast<std::uint8_t*>(address), COMMON,
                             clear, iid, out);
}

HRESULT __stdcall create_placed(This, This heap, std::uint64_t offset, const ResourceDesc* desc, std::uint32_t state, const void* clear, const Iid* iid,
                                This* out) {
    if (!desc || !heap) {
        return E_INVALIDARG_;
    }
    const Heap& h = *reinterpret_cast<Heap*>(heap);
    AllocationInfo info;
    bool fits = footprint(*desc, info) && offset % info.alignment == 0 && offset <= UINT64_MAX - info.size && offset + info.size <= h.desc.size;
    if (!fits) {
        log::warn(LOG_TARGET, "placed resource does not fit",
                  {{"desc", desc_debug(*desc)}, {"offset", log::debug(offset)}, {"heap", log::debug(h.desc.size)}});
        return E_INVALIDARG_;
    }
    add_ref(heap);
    return hand_out_resource(*desc, h.desc.properties, h.desc.flags, Resource::Backing::Placed, Memory{}, heap, offset, nullptr, state, clear, iid, out);
}

namespace {

constexpr PlacedFootprint NO_FOOTPRINT{UINT64_MAX, 0, 0, 0, 0, 0};


struct Subresources {
    const ResourceDesc& desc;
    bool buffer;
    std::uint32_t mips = 1, slices = 1;
    bool known = false;
    std::uint64_t element_bytes = 0;
    int shift = 0;
    std::uint64_t round = 0;

    explicit Subresources(const ResourceDesc& d) : desc(d), buffer(d.dimension == BUFFER) {
        if (buffer) {
            return;
        }
        mips = mip_count(d);
        slices = d.dimension == TEXTURE_3D ? 1 : std::max<std::uint32_t>(d.depth_or_array_size, 1);
        std::uint64_t block;
        known = element(d.format, element_bytes, block);

        shift = block == 4 ? 2 : 0;
        round = block - 1;
    }


    bool get(std::uint32_t index, std::uint32_t mip, std::uint32_t slice, PlacedFootprint& footprint, std::uint32_t& rows, std::uint64_t& row_size) const {
        constexpr std::uint64_t PITCH = 256;
        if (buffer) {
            if (desc.width > UINT32_MAX - (PITCH - 1) || index != 0) {
                return false;
            }
            std::uint64_t pitch = (desc.width + PITCH - 1) & ~(PITCH - 1);
            footprint = {0, 0, static_cast<std::uint32_t>(desc.width), 1, 1, static_cast<std::uint32_t>(pitch)};
            rows = 1;
            row_size = desc.width;
            return true;
        }
        if (slice >= slices || !known) {
            return false;
        }
        std::uint64_t w = std::max(desc.width >> mip, std::uint64_t{1});
        std::uint64_t h = std::max(std::uint64_t{desc.height} >> mip, std::uint64_t{1});

        std::uint64_t width = (w + round) >> shift << shift;
        std::uint64_t height = (h + round) >> shift << shift;
        std::uint32_t depth = desc.dimension == TEXTURE_3D ? std::max<std::uint32_t>(std::uint32_t{desc.depth_or_array_size} >> mip, 1u) : 1;
        std::uint64_t size = (width >> shift) * element_bytes;
        if (width > UINT32_MAX || height > UINT32_MAX || size > UINT32_MAX - (PITCH - 1)) {
            return false;
        }
        std::uint64_t pitch = (size + PITCH - 1) & ~(PITCH - 1);
        footprint = {0, desc.format, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), depth, static_cast<std::uint32_t>(pitch)};
        rows = static_cast<std::uint32_t>(height >> shift);
        row_size = size;
        return true;
    }
};


template <class Get, class Each>
AllocationInfo place(std::uint32_t count, Get&& get, Each&& each) {
    AllocationInfo total{0, KIB_64};
    for (std::uint32_t i = 0; i < count; ++i) {
        AllocationInfo one;
        if (!footprint(get(i), one)) {
            log::warn(LOG_TARGET, "no size for this resource", {{"desc", desc_debug(get(i))}});
            each(i, AllocationInfo1{UINT64_MAX, 0, UINT64_MAX});
            return {UINT64_MAX, 0};
        }
        std::uint64_t offset = div_ceil(total.size, one.alignment) * one.alignment;
        each(i, AllocationInfo1{offset, one.alignment, one.size});
        total.size = offset + one.size;
        total.alignment = std::max(total.alignment, one.alignment);
    }
    return total;
}

}   


void __stdcall copyable_footprints(This, const ResourceDesc* desc, std::uint32_t first, std::uint32_t count, std::uint64_t base, PlacedFootprint* layouts,
                                   std::uint32_t* rows, std::uint64_t* row_sizes, std::uint64_t* total) {
    constexpr std::uint64_t PLACEMENT = 512;
    std::uint64_t end = base;
    bool known = desc != nullptr;

    if (desc && desc->dimension == BUFFER && count == 1 && first == 0) {
        constexpr std::uint64_t PITCH = 256;
        if (desc->width <= UINT32_MAX - (PITCH - 1)) {
            PlacedFootprint layout{(base + PLACEMENT - 1) & ~(PLACEMENT - 1), 0, static_cast<std::uint32_t>(desc->width), 1, 1,
                                   static_cast<std::uint32_t>((desc->width + PITCH - 1) & ~(PITCH - 1))};
            if (layouts) {
                *layouts = layout;
            }
            if (rows) {
                *rows = 1;
            }
            if (row_sizes) {
                *row_sizes = desc->width;
            }
            if (total) {
                *total = layout.offset + desc->width - base;
            }
            return;
        }
    }
    std::optional<Subresources> all;
    std::uint32_t mip = 0, slice = 0;
    if (desc) {
        all.emplace(*desc);

        if (all->mips > 1) {
            mip = first % all->mips;
            slice = first / all->mips;
        } else {
            slice = first;
        }
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        PlacedFootprint layout{};
        std::uint32_t row_count = 0;
        std::uint64_t row_size = 0;
        bool got = all && all->get(first + i, mip, slice, layout, row_count, row_size);
        if (all && ++mip == all->mips) {
            mip = 0;
            ++slice;
        }
        if (got) {
            layout.offset = (end + PLACEMENT - 1) & ~(PLACEMENT - 1);
            std::uint64_t pitch = layout.row_pitch;
            std::uint64_t lines = std::uint64_t{row_count} * layout.depth;
            end = layout.offset + pitch * (lines == 0 ? 0 : lines - 1) + row_size;
        } else {
            known = false;
            layout = NO_FOOTPRINT;
            row_count = UINT32_MAX;
            row_size = UINT64_MAX;
        }
        if (layouts) {
            layouts[i] = layout;
        }
        if (rows) {
            rows[i] = row_count;
        }
        if (row_sizes) {
            row_sizes[i] = row_size;
        }
    }
    if (!known) {
        log::warn(LOG_TARGET, "no footprint for this resource", {{"desc", desc ? "Some(" + desc_debug(*desc) + ")" : std::string("None")}});
    }
    if (total) {
        *total = known ? end - base : UINT64_MAX;
    }
}

AllocationInfo* __stdcall allocation_info(AllocationInfo* out, This, std::uint32_t, std::uint32_t count, const ResourceDesc* descs) {
    if (!descs) {
        count = 0;
    }
    *out = place(count, [&](std::uint32_t i) -> const ResourceDesc& { return descs[i]; }, [](std::uint32_t, const AllocationInfo1&) {});
    return out;
}

AllocationInfo* __stdcall allocation_info2(AllocationInfo* out, This, std::uint32_t, std::uint32_t count, const ResourceDesc1* descs, AllocationInfo1* each) {
    if (!descs) {
        count = 0;
    }
    *out = place(count, [&](std::uint32_t i) -> const ResourceDesc& { return descs[i].desc; },
                 [&](std::uint32_t i, const AllocationInfo1& info) {
                     if (each) {
                         each[i] = info;
                     }
                 });
    return out;
}


HeapProperties* __stdcall custom_heap_properties(HeapProperties* out, This, std::uint32_t nodes, std::uint32_t kind) {
    constexpr std::uint32_t CUSTOM = 4, NOT_AVAILABLE = 1, WRITE_BACK = 3, L0 = 1;
    nodes = std::max(nodes, 1u);
    std::uint32_t cpu_pages = kind == 1 ? NOT_AVAILABLE : (kind == 2 || kind == 3) ? WRITE_BACK : 0;
    if (out) {
        *out = HeapProperties{CUSTOM, cpu_pages, cpu_pages == 0 ? 0u : L0, nodes, nodes};
    }
    return out;
}


HRESULT __stdcall copy_page_mappings(This, std::uint32_t count, const PageBatch* batches, std::uint64_t pool, std::uint32_t pool_pages, std::uint32_t) {
    constexpr std::uint64_t PAGE = 64 << 10;
    PageMapper mapper = MAPPER;
    if (!mapper) {
        return E_INVALIDARG_;
    }
    if (!batches && count > 0) {
        return E_INVALIDARG_;
    }
    auto map = [&](std::uint64_t to, std::uint64_t from, std::uint64_t len) { return mapper(to, from, len) != 0; };
    bool ok = true;
    for (std::uint32_t b = 0; b < count; ++b) {
        const PageBatch& batch = batches[b];
        if (!batch.ranges) {
            continue;
        }
        std::uint64_t at = batch.destination;
        for (std::uint32_t i = 0; i < batch.count; ++i) {
            const PageRange& r = batch.ranges[i];
            std::uint64_t len = std::uint64_t{r.count} * PAGE;
            auto inside = [&](std::uint32_t pages) { return r.start <= UINT32_MAX - pages && r.start + pages <= pool_pages; };
            std::uint64_t source = pool + std::uint64_t{r.start} * PAGE;
            bool done;
            switch (r.kind) {
                case 0: done = inside(r.count) && map(at, source, len); break;
                case 1:
                    done = inside(1);
                    for (std::uint64_t k = 0; done && k < r.count; ++k) {
                        done = map(at + k * PAGE, source, PAGE);
                    }
                    break;
                case 2: done = map(at, 0, len); break;
                case 3: done = true; break;
                default: done = false; break;
            }
            ok = ok && done;
            at += len;
        }
    }
    if (!ok) {
        static std::once_flag once;
        std::call_once(once, [&] { log::warn("victory_gpu::queue", "a page mapping failed", {{"pool", log::hex(pool)}}); });
    }
    return S_OK_;
}

bool texture_memory(This self, const std::uint8_t*& data, ResourceDesc& desc) {
    if (!self || resource(self).desc.dimension == BUFFER) {
        return false;
    }
    data = resource(self).data();
    desc = resource(self).desc;
    return true;
}

bool buffer_bytes(This self, std::uint8_t*& data, std::uint64_t& size) {
    if (!self || resource(self).desc.dimension != BUFFER) {
        return false;
    }
    data = resource(self).data();
    size = resource(self).desc.width;
    return true;
}

void adopt_buffers(BufferSink* sink) {
    std::lock_guard lock(BUFFERS_LOCK);
    SINK = sink;
    if (sink) {
        for (const auto& [key, len] : PENDING) {
            sink->made(key.second, key.first, len);
        }
        PENDING.clear();
    }
}


namespace {


std::atomic<std::uint64_t> TEXTURE_KINDS[7]{};

std::atomic<std::uint64_t> HOST_TEXTURES[2]{};

std::atomic<std::uint64_t> TWINS[2]{};

std::atomic<std::uint64_t> DEPTH_UNORDERED{0};


bool lazy_on() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_LAZY_TEXTURES", L"0");
    return on;
}

std::uint64_t host_bytes(ID3D12Resource* texture) {
    Host* h = host();
    if (!h) {
        return 0;
    }
    D3D12_RESOURCE_DESC d = texture->GetDesc();
    return h->device().device()->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
}


ComPtr<ID3D12Resource> committed_texture(const ResourceDesc& d, std::uint32_t state, const D3D12_CLEAR_VALUE* clear, std::string_view refused) {
    Host* h = host();
    if (!h) {
        return nullptr;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    static_assert(sizeof(ResourceDesc) == sizeof(D3D12_RESOURCE_DESC));
    HRESULT hr = h->device().device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, reinterpret_cast<const D3D12_RESOURCE_DESC*>(&d),
                                                               static_cast<D3D12_RESOURCE_STATES>(state), clear, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        log::warn(LOG_TARGET, refused, {{"error", log::hresult(hr)}, {"desc", desc_debug(d)}});
    }
    return texture;
}


ComPtr<ID3D12Resource> host_texture(const ResourceDesc& desc, std::uint32_t state, const std::uint32_t* clear) {
    constexpr std::uint32_t PC_FLAGS = 0xf;   
    constexpr std::uint32_t TARGET = 1, DEPTH = 2, UNORDERED = 4, DENY_SHADER_RESOURCE = 8;
    if (desc.dimension == BUFFER || !host()) {
        return nullptr;
    }
    ResourceDesc d = desc;
    d.layout = 0;
    d.alignment = 0;
    d.flags &= PC_FLAGS;


    if (d.flags & DEPTH) {
        if (d.flags & UNORDERED) {
            DEPTH_UNORDERED.fetch_add(1, std::memory_order_relaxed);
        }
        d.flags &= ~UNORDERED;
    }
    d.format = pc_format(d.format);
    if (d.flags & (TARGET | UNORDERED)) {
        d.format = target_format(d.format);
    }

    if (!(d.flags & DENY_SHADER_RESOURCE)) {
        for (const auto& [f, typeless] : DEPTH_FORMATS) {
            if (f == d.format) {
                d.format = typeless;
                break;
            }
        }
    }

    if (d.flags & UNORDERED) {
        for (const auto& [srgb, _] : SRGB) {
            if (srgb == d.format) {
                d.format = family(d.format);
                break;
            }
        }
    }


    D3D12_CLEAR_VALUE value{};
    if (clear) {
        std::memcpy(&value, clear, 20);
        value.Format = static_cast<DXGI_FORMAT>(pc_format(static_cast<std::uint32_t>(value.Format)));
        if (d.flags & (TARGET | UNORDERED)) {
            value.Format = static_cast<DXGI_FORMAT>(target_format(static_cast<std::uint32_t>(value.Format)));
        }
    }
    return committed_texture(d, legal_for(d.flags, texture_state(state)), clear ? &value : nullptr, "no host texture");
}


std::shared_ptr<Twin> twin_texture(const ResourceDesc& desc, ID3D12Resource* depth) {
    constexpr std::uint32_t DEPTH_AND_UNORDERED = 6, UNORDERED = 4, UNORDERED_ACCESS = 0x8;
    constexpr std::uint32_t ONE_PLANE[2] = {39, 53};
    constexpr std::uint32_t WITH_STENCIL[2] = {19, 44};
    constexpr std::uint32_t R8_TYPELESS = 60;
    if (desc.dimension == BUFFER || (desc.flags & DEPTH_AND_UNORDERED) != DEPTH_AND_UNORDERED) {
        return nullptr;
    }
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_DEPTH_TWINS", L"0");
    if (!on) {
        return nullptr;
    }
    std::uint32_t typeless = family(pc_format(desc.format));
    bool stencil = typeless == WITH_STENCIL[0] || typeless == WITH_STENCIL[1];
    static const bool stencil_only = danzi::env::is(L"VICTORY_DEPTH_TWINS", L"stencil");
    if (!stencil && stencil_only) {
        return nullptr;
    }
    if (!(stencil || typeless == ONE_PLANE[0] || typeless == ONE_PLANE[1]) || desc.sample_count > 1) {
        TWINS[1].fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    Host* h = host();
    if (!h) {
        return nullptr;
    }
    ResourceDesc d = desc;
    d.layout = 0;
    d.alignment = 0;
    d.flags = UNORDERED;
    d.format = stencil ? R8_TYPELESS : typeless;
    auto texture = committed_texture(d, UNORDERED_ACCESS, nullptr, "no colour twin for a depth texture");
    if (!texture) {
        TWINS[1].fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    ComPtr<ID3D12Resource> through;
    if (stencil) {

        D3D12_RESOURCE_DESC of = depth->GetDesc();
        UINT planes = UINT{of.MipLevels} * of.DepthOrArraySize;
        UINT64 bytes = 0;
        h->device().device()->GetCopyableFootprints(&of, planes, planes, 0, nullptr, nullptr, nullptr, &bytes);
        through = h->device().uav_buffer(bytes);
        if (!through) {
            TWINS[1].fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }
    std::shared_ptr<HostTexture> plane;
    static const bool no_plane = danzi::env::is(L"VICTORY_DEPTH_TWINS", L"nodepth");
    if (stencil && !no_plane) {
        d.format = desc.format >= 118 && desc.format <= 120 ? 53 : 39;
        if (auto p = committed_texture(d, UNORDERED_ACCESS, nullptr, "no twin for a depth plane")) {
            plane = std::make_shared<HostTexture>(std::move(p), UNORDERED_ACCESS);
        }
    }
    TWINS[0].fetch_add(1, std::memory_order_relaxed);
    auto t = std::make_shared<Twin>();
    t->colour = std::make_shared<HostTexture>(std::move(texture), UNORDERED_ACCESS);
    t->through = std::move(through);
    t->plane = std::move(plane);
    return t;
}


struct PlacedKey {
    std::uint64_t address;
    std::array<std::uint32_t, 14> desc;
    bool operator<(const PlacedKey& o) const { return std::tie(address, desc) < std::tie(o.address, o.desc); }
};
std::mutex PLACED_LOCK;
std::map<PlacedKey, This> PLACED_TEXTURES;

}   

HostTexture::HostTexture(ComPtr<ID3D12Resource> t, std::uint32_t s) : texture(std::move(t)), state(s) {
    bytes = host_bytes(texture.Get());
    flags = static_cast<std::uint32_t>(texture->GetDesc().Flags);
    HOST_TEXTURES[0].fetch_add(1, std::memory_order_relaxed);
    HOST_TEXTURES[1].fetch_add(bytes, std::memory_order_relaxed);
}

HostTexture::~HostTexture() {
    if (Host* h = view_count[0].load(std::memory_order_relaxed) + view_count[1].load(std::memory_order_relaxed) ? host() : nullptr) {
        for (int depth = 0; depth < 2; ++depth) {
            for (std::uint8_t i = 0; i < view_count[depth].load(std::memory_order_relaxed); ++i) {
                h->free_kept_view(depth != 0, views[depth][i].handle);
            }
        }
    }
    HOST_TEXTURES[0].fetch_sub(1, std::memory_order_relaxed);
    HOST_TEXTURES[1].fetch_sub(bytes, std::memory_order_relaxed);
    if (filled) {
        TEXTURE_KINDS[0].fetch_sub(1, std::memory_order_relaxed);
        TEXTURE_KINDS[1].fetch_sub(bytes, std::memory_order_relaxed);
    }
}

Twin::~Twin() { TWINS[0].fetch_sub(1, std::memory_order_relaxed); }

namespace {

bool lazy() { return lazy_on(); }


Made hosted(This self, bool make) {
    Resource& r = resource(self);
    if (r.desc.dimension == BUFFER) {
        return {};
    }
    std::lock_guard lock(r.made_lock);
    if (!r.made) {
        if (!make || r.refused.load(std::memory_order_relaxed)) {
            return {};
        }
        auto texture = host_texture(r.desc, r.state, r.has_clear ? r.clear : nullptr);
        if (!texture) {
            r.refused.store(true, std::memory_order_relaxed);
            return {};
        }
        auto twin = twin_texture(r.desc, texture.Get());

        if ((r.desc.flags & 6) == 6) {
            log::info(LOG_TARGET, "depth texture with unordered access",
                      {{"resource", log::hex(reinterpret_cast<std::uintptr_t>(self))}, {"desc", desc_debug(r.desc)}, {"twin", log::debug(twin != nullptr)}});
        }
        std::uint32_t state = legal_state(texture.Get(), texture_state(r.state));
        auto copy = std::make_shared<HostTexture>(std::move(texture), state);
        copy->filled = fills(r.desc);
        if (copy->filled) {
            TEXTURE_KINDS[0].fetch_add(1, std::memory_order_relaxed);
            TEXTURE_KINDS[1].fetch_add(copy->bytes, std::memory_order_relaxed);
        }
        if (r.gone.load(std::memory_order_relaxed)) {
            TEXTURE_KINDS[4].fetch_add(1, std::memory_order_relaxed);
            TEXTURE_KINDS[5].fetch_add(copy->bytes, std::memory_order_relaxed);
        }
        TEXTURE_KINDS[6].fetch_add(1, std::memory_order_relaxed);
        r.made = std::move(copy);
        r.made_twin = std::move(twin);


        texture_made(reinterpret_cast<std::uintptr_t>(self));
    }
    if (std::uint64_t m = moment(); r.used.load(std::memory_order_relaxed) != m) {
        r.used.store(m, std::memory_order_relaxed);
    }
    return {r.made, r.made_twin};
}

}   

std::uint32_t pc_format(std::uint32_t format) {
    switch (format) {
        case 118: return 45;
        case 119: return 46;
        case 120: return 47;
        default: return format;
    }
}

std::uint32_t target_format(std::uint32_t format) {
    static constexpr std::pair<std::uint32_t, std::uint32_t> STAND_INS[2] = {{86, 28}, {115, 28}};
    int i = format == 86 ? 0 : format == 115 ? 1 : -1;
    if (i < 0) {
        return format;
    }

    static const std::array<bool, 2> refused = [] {
        std::array<bool, 2> out{false, false};
        Host* h = host();
        if (!h) {
            return out;
        }
        for (int k = 0; k < 2; ++k) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{static_cast<DXGI_FORMAT>(STAND_INS[k].first)};
            HRESULT hr = h->device().device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support);
            auto both = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
            out[k] = FAILED(hr) || (support.Support1 & both) != both;
            if (out[k]) {
                log::info(LOG_TARGET, "the host does not render to this format: 8-bit RGBA stands in", {{"format", log::debug(STAND_INS[k].first)}});
            }
        }
        return out;
    }();
    return refused[i] ? STAND_INS[i].second : format;
}

std::uint32_t family(std::uint32_t format) {
    constexpr std::pair<std::uint32_t, std::uint32_t> FAMILIES[22] = {
        {1, 4},   {5, 8},   {9, 14},  {15, 18}, {19, 22}, {23, 25}, {27, 32}, {33, 38}, {39, 43}, {44, 47}, {48, 52},
        {53, 59}, {60, 65}, {70, 72}, {73, 75}, {76, 78}, {79, 81}, {82, 84}, {90, 91}, {92, 93}, {94, 96}, {97, 99},
    };
    if (format == 87) {
        return 90;
    }
    if (format == 88) {
        return 92;
    }
    for (const auto& [first, last] : FAMILIES) {
        if (format >= first && format <= last) {
            return first;
        }
    }
    return format;
}

std::uint32_t legal_for(std::uint32_t flags, std::uint32_t state) {


    struct Rule {
        std::uint32_t flag;
        bool deny;
        std::uint32_t bits;
    };
    constexpr Rule FORBIDDEN[4] = {{0x1, false, 0x4}, {0x4, false, 0x8}, {0x2, false, 0x10 | 0x20}, {0x8, true, 0x40 | 0x80}};
    for (const Rule& r : FORBIDDEN) {
        if (((flags & r.flag) != 0) == r.deny) {
            state &= ~r.bits;
        }
    }
    return state;
}

std::uint32_t legal_state(ID3D12Resource* texture, std::uint32_t state) { return legal_for(static_cast<std::uint32_t>(texture->GetDesc().Flags), state); }

ID3D12Resource* host_resource(This self) {
    if (!self) {
        return nullptr;
    }


    Made m = hosted(self, true);
    return m.host ? m.host->texture.Get() : nullptr;
}

std::shared_ptr<HostTexture> host_copy(This self) { return self ? hosted(self, true).host : nullptr; }

namespace {


std::uint32_t references(IUnknown* object) {
    object->AddRef();
    return object->Release();
}

std::uint64_t vram_idle_ms() {
    Host* h = host();
    auto vm = h ? h->device().video_memory() : std::nullopt;
    if (!vm || vm->budget == 0) {
        return 5'000;
    }
    std::uint64_t pct = vm->usage * 100 / vm->budget;
    return pct >= 85 ? 1'000 : pct >= 75 ? 2'500 : 5'000;
}

bool evict_enabled() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_TEXTURE_EVICT", L"0");
    return on;
}

std::uint64_t room_percent() {
    static const std::uint64_t p = [] {
        auto v = danzi::env::get(L"VICTORY_TEXTURE_ROOM_PERCENT");
        return v ? std::wcstoull(v->c_str(), nullptr, 10) : 90;
    }();
    return p;
}

std::atomic<std::uint64_t> LET_GO_TEXTURES[2]{};


template <class Ready>
std::shared_ptr<HostTexture> take_if(Resource& r, std::uintptr_t at, Ready ready) {
    std::lock_guard lock(r.made_lock);
    if (!r.made || !ready(r)) {
        return nullptr;
    }


    unstage(reinterpret_cast<This>(at));
    return std::move(r.made);
}

void evict_under_vram_pressure(std::uint64_t idle_ms) {
    constexpr std::uint64_t EVICT_IDLE_FRAMES = 120, MIN_FREE = 128u << 20, MAX_FREE = 512u << 20;
    if (!evict_enabled()) {
        return;
    }
    Host* h = host();
    auto vm = h ? h->device().video_memory() : std::nullopt;
    if (!vm || vm->budget == 0) {
        return;
    }
    std::uint64_t cap = vm->budget / 100 * std::max<std::uint64_t>(room_percent(), 90);
    if (vm->usage < cap) {
        return;
    }


    settle_disposals();
    vm = h->device().video_memory();
    if (!vm || vm->budget == 0 || vm->usage < cap) {
        return;
    }
    std::uint64_t goal = vm->budget / 100 * (std::max<std::uint64_t>(room_percent(), 90) - 6);
    std::uint64_t target = std::clamp<std::uint64_t>(vm->usage > goal ? vm->usage - goal : 0, MIN_FREE, MAX_FREE);
    struct Candidate {
        std::uint64_t used;
        std::uintptr_t at;
        std::uintptr_t raw;
        std::uint64_t bytes;
    };
    std::vector<Candidate> candidates;
    {
        std::lock_guard lock(TEXTURES_LOCK);
        for (std::uintptr_t at : TEXTURES) {
            Resource& r = resource(reinterpret_cast<This>(at));
            if (!fills(r.desc) || host_wrote(at)) {
                continue;
            }
            std::uint64_t last = r.used.load(std::memory_order_relaxed);
            if (!long_ago(last, idle_ms, EVICT_IDLE_FRAMES)) {
                continue;
            }
            std::lock_guard made(r.made_lock);
            if (r.made && r.made.use_count() == 1 && references(r.made->texture.Get()) > 1) {
                candidates.push_back({last, at, reinterpret_cast<std::uintptr_t>(r.made->texture.Get()), r.made->bytes});
            }
        }
    }
    if (candidates.empty()) {
        return;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.used < b.used; });
    std::vector<std::uintptr_t> raws, victims;
    std::uint64_t selected = 0;
    for (const Candidate& c : candidates) {
        raws.push_back(c.raw);
        victims.push_back(c.at);
        selected += c.bytes;
        if (selected >= target) {
            break;
        }
    }
    std::uint64_t slots_cleared = evict_kept_resources(raws);
    if (slots_cleared == 0) {
        return;
    }
    std::uint64_t freed = 0, freed_bytes = 0;
    {
        std::lock_guard lock(TEXTURES_LOCK);
        for (std::uintptr_t at : victims) {
            if (!TEXTURES.contains(at)) {
                continue;
            }
            Resource& r = resource(reinterpret_cast<This>(at));
            auto gone = take_if(r, at, [](Resource& x) { return x.made.use_count() == 1 && references(x.made->texture.Get()) == 1; });
            if (gone) {
                r.gone.store(true, std::memory_order_relaxed);
                LET_GO_TEXTURES[0].fetch_add(1, std::memory_order_relaxed);
                LET_GO_TEXTURES[1].fetch_add(gone->bytes, std::memory_order_relaxed);
                ++freed;
                freed_bytes += gone->bytes;
                retire(std::move(gone));
            }
        }
    }
    if (freed > 0) {
        log::info(LOG_TARGET, "VRAM pressure: evicted LRU idle textures from descriptor heaps",
                  {{"textures", log::debug(freed)},
                   {"mb", log::debug(freed_bytes >> 20)},
                   {"slots_cleared", log::debug(slots_cleared)},
                   {"vram_used_mb", log::debug(static_cast<std::uint64_t>(vm->usage >> 20))},
                   {"vram_budget_mb", log::debug(static_cast<std::uint64_t>(vm->budget >> 20))}});
    }
}

}   

void resources_let_go_idle() {
    constexpr std::uint64_t IDLE_FRAMES = 30;
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_TEXTURE_SWEEP", L"0");
    if (!on) {
        return;
    }
    std::uint64_t idle_ms = vram_idle_ms();
    std::vector<std::shared_ptr<HostTexture>> gone;
    std::uint64_t bytes = 0;
    std::optional<std::uintptr_t> first;
    {
        std::lock_guard lock(TEXTURES_LOCK);
        for (std::uintptr_t at : TEXTURES) {
            Resource& r = resource(reinterpret_cast<This>(at));
            if (!fills(r.desc) || host_wrote(at)) {
                continue;
            }
            auto g = take_if(r, at, [&](Resource& x) {
                return long_ago(x.used.load(std::memory_order_relaxed), idle_ms, IDLE_FRAMES) && x.made.use_count() == 1 &&
                       references(x.made->texture.Get()) == 1;
            });
            if (g) {
                r.gone.store(true, std::memory_order_relaxed);
                TEXTURE_KINDS[2].fetch_add(1, std::memory_order_relaxed);
                TEXTURE_KINDS[3].fetch_add(g->bytes, std::memory_order_relaxed);
                LET_GO_TEXTURES[0].fetch_add(1, std::memory_order_relaxed);
                LET_GO_TEXTURES[1].fetch_add(g->bytes, std::memory_order_relaxed);
                bytes += g->bytes;
                if (!first) {
                    first = at;
                }
                gone.push_back(std::move(g));
            }
        }
    }
    if (!gone.empty()) {
        trail([&] {
            return "sweep: " + std::to_string(gone.size()) + " host copies of textures the title fills let go (" + std::to_string(bytes >> 10) +
                   " KiB), each held by its title texture alone (no list, no slot); the first: title texture " + log::hex(first.value_or(0));
        });
    }
    for (auto& g : gone) {
        retire(std::move(g));
    }
    evict_under_vram_pressure(idle_ms);
}

std::array<std::uint64_t, 2> textures_let_go() { return {LET_GO_TEXTURES[0].load(), LET_GO_TEXTURES[1].load()}; }

HostTexture* host_copy_raw(This self) {
    if (!self) {
        return nullptr;
    }
    Resource& r = resource(self);
    if (r.desc.dimension == BUFFER) {
        return nullptr;
    }
    {
        std::lock_guard lock(r.made_lock);
        if (r.made) {
            if (std::uint64_t m = moment(); r.used.load(std::memory_order_relaxed) != m) {
                r.used.store(m, std::memory_order_relaxed);
            }
            return r.made.get();
        }
    }

    return hosted(self, true).host.get();
}

std::optional<std::pair<ID3D12Resource*, std::uint32_t>> host_transition(This self, std::uint32_t wanted) {
    Made m = hosted(self, true);
    if (!m.host) {
        return std::nullopt;
    }
    std::uint32_t before = m.host->state.exchange(wanted, std::memory_order_relaxed);
    if (before == wanted) {
        return std::nullopt;
    }
    return std::pair{m.host->texture.Get(), before};
}

std::shared_ptr<Twin> twin(This self) { return self ? hosted(self, true).twin : nullptr; }

ID3D12Resource* twin_resource(This self) {
    auto t = twin(self);
    return t ? t->colour->texture.Get() : nullptr;
}

ID3D12Resource* twin_plane_resource(This self) {
    auto t = twin(self);
    return t && t->plane ? t->plane->texture.Get() : nullptr;
}

bool host_waits(This self) {
    if (!self || !lazy()) {
        return false;
    }
    Resource& r = resource(self);
    if (r.desc.dimension == BUFFER || r.refused.load(std::memory_order_relaxed)) {
        return false;
    }
    std::lock_guard lock(r.made_lock);
    return !r.made;
}

bool title_fills(This self) { return self && fills(resource(self).desc); }

bool stands_in(This self) {
    if (!self) {
        return false;
    }
    std::uint32_t own = pc_format(resource(self).desc.format);
    if (target_format(own) == own) {
        return false;
    }
    ID3D12Resource* h = host_resource(self);
    return h && static_cast<std::uint32_t>(h->GetDesc().Format) == target_format(own);
}

std::optional<std::uint32_t> view_format(This self) {
    ID3D12Resource* h = host_resource(self);
    if (!h) {
        return std::nullopt;
    }
    std::uint32_t own = pc_format(resource(self).desc.format);
    auto format = static_cast<std::uint32_t>(h->GetDesc().Format);
    if (format != own && format != target_format(own)) {
        return own;
    }
    return std::nullopt;
}

bool depth_and_unordered(This self) { return self && (resource(self).desc.flags & 6) == 6; }

bool room() {
    constexpr std::uint64_t FLOOR = std::uint64_t{2} << 30, WITHOUT_BUDGET = std::uint64_t{4} << 30;
    static const std::optional<std::uint64_t> limit = []() -> std::optional<std::uint64_t> {
        auto v = danzi::env::get(L"VICTORY_TEXTURE_BUDGET_MB");
        if (!v) {
            return std::nullopt;
        }
        try {
            return std::stoull(*v) << 20;
        } catch (...) {
            return std::nullopt;
        }
    }();
    static const std::uint64_t percent = [] {
        auto v = danzi::env::get(L"VICTORY_TEXTURE_ROOM_PERCENT");
        try {
            return v ? std::clamp<std::uint64_t>(std::stoull(*v), 50, 97) : 82;
        } catch (...) {
            return std::uint64_t{82};
        }
    }();
    std::uint64_t filled = TEXTURE_KINDS[1].load(std::memory_order_relaxed);
    if (limit) {
        return filled < *limit;
    }
    if (filled < FLOOR) {
        return true;
    }
    Host* h = host();
    if (auto vm = h ? h->device().video_memory() : std::nullopt) {
        return vm->usage < vm->budget / 100 * percent;
    }
    return filled < WITHOUT_BUDGET;
}

This texture_at(std::uint64_t address, const std::uint32_t* words) {
    static const bool kept = danzi::env::is(L"VICTORY_PLACED_FILLER", L"1");
    PlacedKey key{address, {}};
    std::memcpy(key.desc.data(), words, 56);
    if (!kept) {
        key.desc[1] = 0;
        key.desc[13] = 0;
    }
    std::lock_guard lock(PLACED_LOCK);
    if (auto it = PLACED_TEXTURES.find(key); it != PLACED_TEXTURES.end()) {
        return it->second;
    }
    ResourceDesc d;
    std::memcpy(&d, key.desc.data(), 56);
    if (d.dimension == BUFFER) {
        return nullptr;
    }
    This out = nullptr;
    if (create_at_address(nullptr, address, &d, 0, nullptr, &RESOURCE_IIDS[0], &out) != S_OK_) {
        return nullptr;
    }
    PLACED_TEXTURES.emplace(key, out);
    placed_in_host_written(address, key.desc.data());
    resource(out).placed.store(true, std::memory_order_relaxed);
    return out;
}

This alias_of(This self) {
    auto v = danzi::env::get(L"VICTORY_ALIAS_COPY");
    static const bool on = !v || *v != L"0";
    static const bool whole = !v || *v != L"1";
    if (!self || !on) {
        return nullptr;
    }
    Resource& r = resource(self);
    auto one = [](const ResourceDesc& d) {
        return d.dimension != BUFFER && d.mip_levels >= 1 && d.depth_or_array_size >= 1 && (whole || (d.mip_levels == 1 && d.depth_or_array_size == 1)) &&
               d.sample_count <= 1;
    };
    if (!r.placed.load(std::memory_order_relaxed) || !fills(r.desc) || !one(r.desc)) {
        return nullptr;
    }
    std::lock_guard lock(TEXTURES_LOCK);
    auto it = TARGETS_AT.find(reinterpret_cast<std::uint64_t>(r.data()));
    if (it == TARGETS_AT.end()) {
        return nullptr;
    }
    auto other = reinterpret_cast<This>(it->second);
    const ResourceDesc& o = resource(other).desc;
    std::uint64_t eb_o, bl_o, eb_r, bl_r;
    bool same = one(o) && o.dimension == r.desc.dimension && o.width == r.desc.width && o.height == r.desc.height && o.mip_levels == r.desc.mip_levels &&
                o.depth_or_array_size == r.desc.depth_or_array_size && (o.flags & 0x2) == 0 && target_format(o.format) == o.format &&
                element(o.format, eb_o, bl_o) && bl_o == 1 && element(r.desc.format, eb_r, bl_r) && eb_o == eb_r && bl_o == bl_r;
    return same ? other : nullptr;
}

std::optional<std::uint64_t> placed_bytes(const std::uint32_t* words) {
    ResourceDesc d;
    std::memcpy(&d, words, 56);
    AllocationInfo info;
    if (d.dimension == BUFFER || !footprint(d, info)) {
        return std::nullopt;
    }
    return info.size;
}

std::optional<std::uint64_t> placed_bytes_from(const std::uint32_t* words, std::uint32_t first) {
    ResourceDesc d;
    std::memcpy(&d, words, 56);
    if (first == 0 || d.dimension == BUFFER) {
        return placed_bytes(words);
    }
    auto whole = placed_bytes(words);
    if (!whole) {
        return std::nullopt;
    }
    formats::Texture t;
    if (!texture_layout(d, t, false) || t.planes.size() != 1) {
        return whole;
    }
    std::uint64_t bytes = t.planes[0].layout.bytes_from(first);
    std::uint64_t a = std::max<std::uint64_t>(t.alignment, 1);
    return std::min((bytes + a - 1) / a * a, *whole);
}

std::uint32_t first_mip(const std::uint32_t* view, std::size_t words) {
    constexpr std::uint32_t TEXTURE_2D_VIEW = 4;
    return words >= 5 && view[1] == TEXTURE_2D_VIEW ? std::min<std::uint32_t>(view[4], 15) : 0;
}

}   
