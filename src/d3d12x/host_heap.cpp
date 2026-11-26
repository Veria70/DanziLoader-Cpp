// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/descriptor/host.rs (DanziLoader /
// victory-perf, see NOTICE.md). See host_heap.hpp.
#include "host_heap.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>

#include "core/env.hpp"
#include "core/log.hpp"
#include "shader/dxil.hpp"
#include "host.hpp"
#include "resource.hpp"
#include "sampler.hpp"
#include "shadows.hpp"
#include "sync.hpp"
#include "texture.hpp"

namespace danzi::d3d12x {

namespace {

constexpr std::string_view TARGET = "victory_gpu::descriptor::host";


#define HEAP_ONCE(...)                                                                                            \
    do {                                                                                                          \
        static std::atomic<bool> said_{false};                                                                    \
        if (!said_.load(std::memory_order_relaxed) && !said_.exchange(true, std::memory_order_relaxed)) {        \
            log::warn(TARGET, __VA_ARGS__);                                                                       \
        }                                                                                                         \
    } while (false)


bool stale_out() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_STALE_TEXTURES", L"0");
    return on;
}

bool tabled_out() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_TABLED_TEXTURES", L"1");
    return on;
}

bool keep_views() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_KEEP_VIEWS", L"0");
    return on;
}

bool view_extent() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_VIEW_EXTENT", L"0");
    return on;
}


constexpr std::uint64_t TABLED_FRAMES = 240;
constexpr std::uint64_t TABLED_GRACE = 8;
constexpr std::uint32_t TABLED_READ = 1u << 31;
constexpr std::uint64_t RECHECK_FRAMES[5] = {4, 12, 40, 120, 400};

std::atomic<std::uint64_t> TABLED_WRITES[2]{};
std::atomic<std::uint64_t> STALE[5]{};
std::atomic<std::uint64_t> SWEPT[11]{};
std::atomic<std::uint64_t> SPANNED[2]{UINT64_MAX, 0};
std::atomic<std::uint64_t> REFUSED_VIEWS{0};
std::atomic<std::uint64_t> KEPT_VIEWS{0};
std::atomic<std::uint64_t> PLACED_TEXTURE_SLOTS{0};
std::atomic<std::uint64_t> OBJECT_TEXTURE_SLOTS{0};

std::mutex REMAPPED_LOCK;
std::vector<std::pair<std::uint64_t, std::uint64_t>> REMAPPED;

struct Sweep {
    std::uint32_t next = 0;
    std::array<std::uint64_t, 10> counts{};
    std::uint64_t at = 0;
};
std::mutex SWEEP_LOCK;
Sweep SWEEP;

std::atomic<std::uint32_t> VIEW_LIMIT{0};

ID3D12Device* device() {
    Host* h = host();
    return h ? h->device().device() : nullptr;
}


D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc(const std::uint32_t* words) {
    std::uint32_t w[10];
    std::memcpy(w, words, sizeof w);
    w[0] = pc_format(w[0]);
    D3D12_SHADER_RESOURCE_VIEW_DESC d;
    static_assert(sizeof d == 40);
    std::memcpy(&d, w, sizeof d);
    return d;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc(const std::uint32_t* words) {
    std::uint32_t w[10];
    std::memcpy(w, words, sizeof w);
    w[0] = pc_format(w[0]);
    D3D12_UNORDERED_ACCESS_VIEW_DESC d;
    static_assert(sizeof d == 40);
    std::memcpy(&d, w, sizeof d);
    return d;
}

std::uint64_t element_bytes(std::uint32_t format) {
    std::uint64_t bytes = 0, block = 0;
    return element(format, bytes, block) ? bytes : 0;
}


std::uint64_t element_size(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    const auto& b = desc.Buffer;
    if (b.StructureByteStride > 0) {
        return b.StructureByteStride;
    }
    if (b.Flags & D3D12_BUFFER_SRV_FLAG_RAW) {
        return 4;
    }
    return element_bytes(static_cast<std::uint32_t>(desc.Format));
}


const std::uint32_t* placed_resource(const View& v) { return v.words.data(); }
const std::uint32_t* placed_desc(const View& v) { return v.words.data() + 14; }
std::uint64_t placed_address(const View& v) { return std::uint64_t{placed_desc(v)[10]} | std::uint64_t{placed_desc(v)[11]} << 32; }
std::array<std::uint32_t, 14> placed_resource_words(const View& v) {
    std::array<std::uint32_t, 14> r;
    std::copy_n(v.words.begin(), 14, r.begin());
    return r;
}

constexpr std::uint32_t BUFFER_DIMENSION = 1;

std::optional<std::pair<std::uint8_t*, std::uint64_t>> buffer_of(std::uint64_t resource) {
    std::uint8_t* data = nullptr;
    std::uint64_t len = 0;
    if (!buffer_bytes(reinterpret_cast<This>(resource), data, len)) {
        return std::nullopt;
    }
    return std::pair{data, len};
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> unordered_range(std::uint64_t base, const std::uint32_t* words) {
    auto desc = uav_desc(words);
    const auto& b = desc.Buffer;
    std::uint64_t element;
    if (b.StructureByteStride > 0) {
        element = b.StructureByteStride;
    } else if (b.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
        element = 4;
    } else {
        std::uint64_t bytes = 0, block = 0;
        if (!d3d12x::element(static_cast<std::uint32_t>(desc.Format), bytes, block)) {
            return std::nullopt;
        }
        element = bytes;
    }
    return std::pair{base + b.FirstElement * element, std::uint64_t{b.NumElements} * element};
}


std::optional<std::uint32_t> view_stride(const View& view) {
    const std::uint32_t* words = nullptr;
    bool unordered = false;
    switch (view.kind) {
        case View::Shader:
            if (view.flag && view.words[1] == BUFFER_DIMENSION) {
                words = view.words.data();
            }
            break;
        case View::Unordered:
            if (view.flag && view.words[1] == BUFFER_DIMENSION) {
                words = view.words.data();
                unordered = true;
            }
            break;
        case View::Placed:
            if (placed_resource(view)[0] == BUFFER_DIMENSION) {
                words = placed_desc(view);
                unordered = view.flag;
            }
            break;
        default: break;
    }
    if (!words) {
        return std::nullopt;
    }
    std::uint32_t stride;
    bool raw;
    DXGI_FORMAT format;
    if (unordered) {
        auto d = uav_desc(words);
        stride = d.Buffer.StructureByteStride;
        raw = (d.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
        format = d.Format;
    } else {
        auto d = srv_desc(words);
        stride = d.Buffer.StructureByteStride;
        raw = (d.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
        format = d.Format;
    }
    if (stride > 0) {
        return stride;
    }
    if (raw) {
        return 1u;
    }
    return static_cast<std::uint32_t>(element_bytes(static_cast<std::uint32_t>(format)));
}


std::optional<std::pair<std::uint64_t, std::uint32_t>> raw_window(std::uint64_t offset, std::uint64_t first, std::uint32_t count, std::uint32_t stride) {
    if (stride == 0) {
        return std::nullopt;
    }

    if (first != 0 && stride > UINT64_MAX / first) {
        return std::nullopt;
    }
    std::uint64_t start = first * stride;
    if (start > UINT64_MAX - offset) {
        return std::nullopt;
    }
    start += offset;
    std::uint64_t bytes = std::uint64_t{count} * stride;
    if (start % 16 != 0 || bytes % 4 != 0 || bytes / 4 > UINT32_MAX) {
        return std::nullopt;
    }
    return std::pair{start / 4, static_cast<std::uint32_t>(bytes / 4)};
}


bool buffer_view_ok(DXGI_FORMAT format, std::uint64_t first, std::uint32_t count, std::uint32_t stride, bool raw, std::uint64_t width) {
    auto f = static_cast<std::uint32_t>(format);
    std::uint64_t element;
    if (raw) {
        if (format != DXGI_FORMAT_R32_TYPELESS || stride != 0 || first % 4 != 0) {
            return false;
        }
        element = 4;
    } else if (stride > 0) {
        if (format != DXGI_FORMAT_UNKNOWN || stride > 2048 || stride % 4 != 0) {
            return false;
        }
        element = stride;
    } else {
        bool typeless = family(f) == f && family(f + 1) == f;
        std::uint64_t bytes = 0, block = 0;
        if (!d3d12x::element(f, bytes, block) || block != 1 || f == 0 || typeless || bytes == 0) {
            return false;
        }
        element = bytes;
    }
    if (count == 0) {
        return false;
    }
    std::uint64_t end = first + count;
    if (end < first || (element != 0 && end > UINT64_MAX / element)) {
        return false;
    }
    return end * element <= width;
}

std::uint32_t constant_size(std::uint32_t size) {
    std::uint32_t s = std::min<std::uint32_t>(size, 64u << 10);
    return (s + 255) / 256 * 256;
}

std::uint32_t elements_inside(std::uint64_t first, std::uint32_t count, std::uint64_t element, std::uint64_t width) {
    std::uint64_t per = width / std::max<std::uint64_t>(element, 1);
    std::uint64_t room = per > first ? per - first : 0;
    if (room == 0 || room > UINT32_MAX) {
        return count;
    }
    return std::min(count, static_cast<std::uint32_t>(room));
}

using Kept = ComPtr<ID3D12Resource>;

std::optional<Kept> buffer_srv(std::uint64_t address, std::uint64_t len, D3D12_SHADER_RESOURCE_VIEW_DESC desc, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    auto shadow = shadow_find(address, len);
    if (!shadow) {
        return std::nullopt;
    }
    std::uint64_t offset = shadow_offset(*shadow, address);
    std::optional<std::pair<std::uint64_t, std::uint32_t>> window;
    if (desc.Format == DXGI_FORMAT_UNKNOWN) {
        window = raw_window(offset, desc.Buffer.FirstElement, desc.Buffer.NumElements, desc.Buffer.StructureByteStride);
    }
    if (window) {
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.Buffer = {window->first, window->second, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    }
    std::uint64_t element = element_size(desc);
    auto& buffer = desc.Buffer;
    if (!window) {
        if (element == 0 || offset % element != 0) {
            return std::nullopt;
        }
        buffer.FirstElement += offset / element;
    }
    std::uint64_t width = shadow->buffer()->GetDesc().Width;
    std::uint64_t unit = window ? 4 : element;
    buffer.NumElements = elements_inside(buffer.FirstElement, buffer.NumElements, unit, width);
    bool raw = (buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
    if (!buffer_view_ok(desc.Format, buffer.FirstElement, buffer.NumElements, buffer.StructureByteStride, raw, width)) {
        REFUSED_VIEWS.fetch_add(1, std::memory_order_relaxed);
        HEAP_ONCE("buffer view the host would refuse left null",
                  {{"format", log::debug(static_cast<int>(desc.Format))},
                   {"first", log::debug(static_cast<std::uint64_t>(buffer.FirstElement))},
                   {"count", log::debug(static_cast<std::uint32_t>(buffer.NumElements))},
                   {"stride", log::debug(static_cast<std::uint32_t>(buffer.StructureByteStride))},
                   {"raw", log::debug(raw)},
                   {"width", log::debug(width)}});
        return std::nullopt;
    }
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ID3D12Device* d = device();
    if (!d) {
        return std::nullopt;
    }
    d->CreateShaderResourceView(shadow->buffer(), &desc, at);
    return Kept(shadow->buffer());
}

std::optional<Kept> buffer_uav(std::uint64_t address, std::uint64_t len, D3D12_UNORDERED_ACCESS_VIEW_DESC desc, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    auto shadow = shadow_find(address, len);
    if (!shadow) {
        return std::nullopt;
    }
    if ((shadow->buffer()->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        HEAP_ONCE("unordered access to a buffer whose host copy shaders cannot write");
        return std::nullopt;
    }
    auto& buffer = desc.Buffer;
    std::uint64_t element;
    if (buffer.StructureByteStride > 0) {
        element = buffer.StructureByteStride;
    } else if (buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
        element = 4;
    } else {
        element = element_bytes(static_cast<std::uint32_t>(desc.Format));
    }
    std::uint64_t offset = shadow_offset(*shadow, address);
    std::optional<std::pair<std::uint64_t, std::uint32_t>> window;
    if (desc.Format == DXGI_FORMAT_UNKNOWN) {
        window = raw_window(offset, buffer.FirstElement, buffer.NumElements, buffer.StructureByteStride);
    }
    if (window) {
        buffer.FirstElement = window->first;
        buffer.NumElements = window->second;
        buffer.StructureByteStride = 0;
        buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    } else {
        if (element == 0 || offset % element != 0) {
            return std::nullopt;
        }
        buffer.FirstElement += offset / element;
    }
    buffer.CounterOffsetInBytes = 0;
    std::uint64_t width = shadow->buffer()->GetDesc().Width;
    std::uint64_t unit = window ? 4 : element;
    buffer.NumElements = elements_inside(buffer.FirstElement, buffer.NumElements, unit, width);
    bool raw = (buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
    if (window) {
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
    }
    if (!buffer_view_ok(desc.Format, buffer.FirstElement, buffer.NumElements, buffer.StructureByteStride, raw, width)) {
        REFUSED_VIEWS.fetch_add(1, std::memory_order_relaxed);
        HEAP_ONCE("unordered buffer view the host would refuse left null",
                  {{"format", log::debug(static_cast<int>(desc.Format))},
                   {"first", log::debug(static_cast<std::uint64_t>(buffer.FirstElement))},
                   {"count", log::debug(static_cast<std::uint32_t>(buffer.NumElements))},
                   {"stride", log::debug(static_cast<std::uint32_t>(buffer.StructureByteStride))},
                   {"raw", log::debug(raw)},
                   {"width", log::debug(width)}});
        return std::nullopt;
    }
    ID3D12Device* d = device();
    if (!d) {
        return std::nullopt;
    }
    d->CreateUnorderedAccessView(shadow->buffer(), nullptr, &desc, at);
    return Kept(shadow->buffer());
}


bool fits(ID3D12Resource* texture, DXGI_FORMAT* format, bool unordered) {
    D3D12_RESOURCE_DESC d = texture->GetDesc();
    bool allowed = unordered ? (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 : (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
    auto own = static_cast<std::uint32_t>(d.Format);
    bool depth = std::any_of(std::begin(DEPTH_FORMATS), std::end(DEPTH_FORMATS), [&](const auto& p) { return p.first == own; });
    if (!allowed || depth) {
        HEAP_ONCE("view the host does not allow",
                  {{"unordered", log::debug(unordered)}, {"format", log::debug(own)}, {"flags", log::debug(static_cast<int>(d.Flags))}});
        return false;
    }
    bool typeless = family(own) == own && family(own + 1) == own;
    if (format && unordered) {
        for (const auto& [s, linear] : SRGB) {
            if (s == static_cast<std::uint32_t>(*format)) {
                *format = static_cast<DXGI_FORMAT>(linear);
                break;
            }
        }
    }
    std::uint32_t view = format ? static_cast<std::uint32_t>(*format) : 0;
    if (view == 0) {
        return !typeless;
    }
    if (view != own && target_format(view) == own) {
        *format = d.Format;
        return true;
    }
    if (family(view) != family(own)) {
        HEAP_ONCE("view format of another family", {{"view", log::debug(view)}, {"own", log::debug(own)}});
        return false;
    }
    if (!typeless) {
        *format = d.Format;
    }
    return true;
}


std::optional<D3D12_UNORDERED_ACCESS_VIEW_DESC> twin_uav(ID3D12Resource* twin, const D3D12_UNORDERED_ACCESS_VIEW_DESC* given) {
    D3D12_RESOURCE_DESC d = twin->GetDesc();
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    if (given) {
        desc = *given;
    } else {
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        if (d.DepthOrArraySize > 1) {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {};
            desc.Texture2DArray.ArraySize = d.DepthOrArraySize;
        }
    }
    if (d.Format == 60) {
        std::uint32_t plane = 0;
        if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) {
            plane = std::exchange(desc.Texture2D.PlaneSlice, 0u);
        } else if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY) {
            plane = std::exchange(desc.Texture2DArray.PlaneSlice, 0u);
        }
        if (plane != 1 && desc.Format != 22 && desc.Format != 47) {
            return std::nullopt;
        }
        desc.Format = static_cast<DXGI_FORMAT>(62);
        return desc;
    }
    auto f = static_cast<std::uint32_t>(desc.Format);
    auto t = static_cast<std::uint32_t>(d.Format);
    bool plane_format = (f >= 19 && f <= 21) || (f >= 44 && f <= 46);
    if (t == 53 && (f == 0 || f == 53 || f == 55 || plane_format)) {
        desc.Format = static_cast<DXGI_FORMAT>(56);
    } else if (t == 39 && (f == 0 || f == 39 || f == 40 || plane_format)) {
        desc.Format = static_cast<DXGI_FORMAT>(41);
    }
    return desc;
}

void null_view(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(nullptr, &desc, at);
}

void null_buffer_view(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    d->CreateShaderResourceView(nullptr, &desc, at);
}

void null_like(const std::uint32_t* words, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    if (words[1] < 2 || words[1] > 10) {
        null_view(at);
        return;
    }
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    std::uint32_t w[10] = {};
    w[1] = words[1];
    switch (words[1]) {
        case 6: break;
        case 7: w[5] = 1; break;
        case 3:
        case 5:
        case 10:
            w[5] = 1;
            w[7] = 1;
            break;
        default: w[5] = 1; break;
    }
    auto desc = srv_desc(w);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d->CreateShaderResourceView(nullptr, &desc, at);
}

D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav_desc(std::optional<std::uint32_t> dimension) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    if (!dimension) {
        return desc;
    }
    switch (static_cast<D3D12_UAV_DIMENSION>(*dimension)) {
        case D3D12_UAV_DIMENSION_TEXTURE1D: desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; break;
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {};
            desc.Texture1DArray.ArraySize = 1;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {};
            desc.Texture2DArray.ArraySize = 1;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE3D:
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            desc.Texture3D = {};
            desc.Texture3D.WSize = UINT32_MAX;
            break;
        default: break;
    }
    return desc;
}

bool is_buffer_object(std::uint64_t r) { return buffer_of(r).has_value(); }


void null_for(const View& view, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    bool buffer = false;
    std::optional<std::uint32_t> dimension;
    switch (view.kind) {
        case View::Unordered:
            buffer = is_buffer_object(view.a) || (view.flag && view.words[1] == 1);
            if (view.flag) {
                dimension = view.words[1];
            }
            break;
        case View::Placed:
            if (view.flag) {
                buffer = placed_resource(view)[0] == 1;
                dimension = placed_desc(view)[1];
                break;
            }
            if (placed_resource(view)[0] == 1) {
                null_buffer_view(at);
                return;
            }
            null_like(placed_desc(view), at);
            return;
        case View::Shader:
            if (is_buffer_object(view.a) || (view.flag && view.words[1] == 1)) {
                null_buffer_view(at);
                return;
            }
            if (view.flag) {
                null_like(view.words.data(), at);
                return;
            }
            null_view(at);
            return;
        case View::Constant: {
            D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
            d->CreateConstantBufferView(&desc, at);
            return;
        }
        default: null_view(at); return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
    if (buffer) {
        desc = {};
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    } else {
        desc = null_uav_desc(dimension);
    }
    d->CreateUnorderedAccessView(nullptr, nullptr, &desc, at);
}

void null_sampler_at(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    d->CreateSampler(&desc, at);
}


std::optional<Kept> make_view(const View& view, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    ID3D12Device* d = device();
    if (!d) {
        return std::nullopt;
    }
    switch (view.kind) {
        case View::Constant: {
            auto shadow = shadow_find(view.a, view.b);
            if (!shadow) {
                return std::nullopt;
            }
            std::uint64_t gpu = shadow_gpu(*shadow, view.a);
            if (gpu % 256 != 0) {
                HEAP_ONCE("constant buffer not 256-byte aligned on the host", {{"address", log::hex(view.a)}});
                return std::nullopt;
            }
            D3D12_CONSTANT_BUFFER_VIEW_DESC desc{gpu, constant_size(static_cast<std::uint32_t>(view.b))};
            d->CreateConstantBufferView(&desc, at);
            return Kept(shadow->buffer());
        }
        case View::Shader: {
            This resource = reinterpret_cast<This>(view.a);
            if (ID3D12Resource* texture = host_resource(resource)) {
                std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> desc;
                if (view.flag) {
                    desc = srv_desc(view.words.data());
                }
                if (!fits(texture, desc ? &desc->Format : nullptr, false)) {
                    return std::nullopt;
                }
                d->CreateShaderResourceView(texture, desc ? &*desc : nullptr, at);
                return Kept(texture);
            }
            auto b = buffer_of(view.a);
            if (b && view.flag) {
                return buffer_srv(reinterpret_cast<std::uint64_t>(b->first), b->second, srv_desc(view.words.data()), at);
            }
            return std::nullopt;
        }
        case View::Unordered: {
            This resource = reinterpret_cast<This>(view.a);
            ID3D12Resource* texture = host_resource(resource);
            if (!texture) {
                auto b = buffer_of(view.a);
                if (b && view.flag) {
                    return buffer_uav(reinterpret_cast<std::uint64_t>(b->first), b->second, uav_desc(view.words.data()), at);
                }
                return std::nullopt;
            }


            if (depth_and_unordered(resource)) {
                static std::atomic<std::uint32_t> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 512) {
                    std::string described = "None";
                    if (view.flag) {
                        described = "Some([";
                        for (int k = 0; k < 10; ++k) {
                            described += (k ? ", " : "") + std::to_string(view.words[k]);
                        }
                        described += "])";
                    }
                    log::info(TARGET, "unordered access view of a depth texture",
                              {{"resource", log::hex(reinterpret_cast<std::uintptr_t>(resource))}, {"view", described}});
                }
            }
            std::optional<D3D12_UNORDERED_ACCESS_VIEW_DESC> desc;
            if (view.flag) {
                desc = uav_desc(view.words.data());
            }

            ID3D12Resource* chosen = texture;
            std::optional<D3D12_UNORDERED_ACCESS_VIEW_DESC> twin_view;
            if (ID3D12Resource* t = twin_resource(resource)) {
                if ((twin_view = twin_uav(t, desc ? &*desc : nullptr))) {
                    chosen = t;
                }
            }
            if (chosen == texture) {
                if (ID3D12Resource* t = twin_plane_resource(resource)) {
                    if ((twin_view = twin_uav(t, desc ? &*desc : nullptr))) {
                        chosen = t;
                    }
                }
            }
            if (chosen != texture) {
                desc = twin_view;
            }
            if (!fits(chosen, desc ? &desc->Format : nullptr, true)) {
                return std::nullopt;
            }
            d->CreateUnorderedAccessView(chosen, nullptr, desc ? &*desc : nullptr, at);
            return Kept(chosen);
        }
        case View::Sampler: {
            D3D12_SAMPLER_DESC desc;
            static_assert(sizeof desc == 52);
            std::memcpy(&desc, view.words.data(), sizeof desc);
            d->CreateSampler(&desc, at);
            return Kept();
        }
        case View::ConsoleSampler: {
            auto words = console_sampler(view.words.data());
            D3D12_SAMPLER_DESC desc;
            std::memcpy(&desc, words.data(), sizeof desc);
            d->CreateSampler(&desc, at);
            return Kept();
        }
        case View::Placed: {
            if (auto texture = placed_texture(view)) {
                return make_view(*texture, at);
            }
            if (placed_resource(view)[0] != BUFFER_DIMENSION) {


                HEAP_ONCE("placed unordered texture view not mirrored", {{"dimension", log::debug(placed_resource(view)[0])}});
                return std::nullopt;
            }
            std::uint64_t len = std::uint64_t{placed_resource(view)[4]} | std::uint64_t{placed_resource(view)[5]} << 32;
            std::uint64_t address = placed_address(view);
            if (view.flag) {
                return buffer_uav(address, len, uav_desc(placed_desc(view)), at);
            }
            return buffer_srv(address, len, srv_desc(placed_desc(view)), at);
        }
    }
    return std::nullopt;
}

bool same_placed_texture(const View& a, const View& b) {
    if (a.kind != View::Placed || b.kind != View::Placed || a.flag || b.flag) {
        return false;
    }
    return placed_resource(a)[0] != 1 && std::equal(a.words.begin(), a.words.begin() + 14, b.words.begin()) &&
           placed_desc(a)[10] == placed_desc(b)[10] && placed_desc(a)[11] == placed_desc(b)[11];
}

void retire_resource(ComPtr<ID3D12Resource> r) {
    if (!r) {
        return;
    }
    retire(std::shared_ptr<void>(r.Detach(), [](void* p) { static_cast<IUnknown*>(p)->Release(); }));
}


std::array<std::uint32_t, 3> record_at(std::uint64_t handle) {
    std::array<std::uint32_t, 3> r;
    std::memcpy(r.data(), reinterpret_cast<const void*>(handle), sizeof r);
    return r;
}

}   


void probe_view_limit() {
    static std::once_flag once;
    std::call_once(once, [] {
        ID3D12Device* d = device();
        if (!d) {
            return;
        }
        auto fits_count = [&](std::uint32_t count) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            ComPtr<ID3D12DescriptorHeap> heap;
            return SUCCEEDED(d->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
        };
        constexpr std::uint32_t PORTABLE_VIEWS = 1'000'000, UNCAPPED_VIEWS = 1u << 21;
        std::uint32_t views = fits_count(UNCAPPED_VIEWS) ? UNCAPPED_VIEWS : PORTABLE_VIEWS;
        log::info(TARGET,
                  "host view heaps hold " + std::to_string(views) + " views; view indices are clamped to the null slot at the end from the first shader on",
                  {{"views", log::debug(views)}});
        VIEW_LIMIT.store(views - 1, std::memory_order_relaxed);

        shader::dxil::set_view_heap_limit(views - 1);
    });
}

std::uint32_t view_heap_limit() { return VIEW_LIMIT.load(std::memory_order_relaxed); }


std::size_t Spans::SizeHash::operator()(const SizeKey& k) const {
    std::uint64_t h = k.first;
    for (std::uint32_t w : k.desc) {
        h = (h ^ w) * 0x100000001b3ull;
    }
    return static_cast<std::size_t>(h);
}

std::optional<std::uint64_t> Spans::bytes(const std::array<std::uint32_t, 14>& desc, std::uint32_t first) {
    SizeKey key{desc, first};
    auto it = sizes_.find(key);
    if (it != sizes_.end()) {
        return it->second;
    }
    auto b = placed_bytes_from(desc.data(), first);
    sizes_.emplace(key, b);
    return b;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> Spans::overlapping(std::uint64_t start, std::uint64_t end) const {
    std::vector<Key> out;

    auto lower = spans_.lower_bound(Key{start, 0});
    if (lower != spans_.begin()) {
        std::uint64_t at = std::prev(lower)->first.first;
        for (auto it = spans_.lower_bound(Key{at, 0}); it != spans_.end() && it->first.first == at; ++it) {
            if (it->first.second > start) {
                out.push_back(it->first);
            }
        }
    }
    for (auto it = lower; it != spans_.end() && it->first < Key{end, 0}; ++it) {
        out.push_back(it->first);
    }
    return out;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> Spans::against(std::uint64_t start, std::uint64_t end, const std::array<std::uint32_t, 14>& desc) const {
    auto spans = overlapping(start, end);
    std::erase_if(spans, [&](const Key& span) {
        if (span.first != start) {
            return false;
        }
        if (span.second == end) {
            return true;
        }
        auto it = spans_.find(span);
        return it != spans_.end() && it->second.desc == desc;
    });
    return spans;
}

std::vector<std::uint32_t> Spans::note(std::uint32_t index, std::uint64_t start, std::uint64_t end, const std::array<std::uint32_t, 14>& desc, bool newer) {
    if (auto it = by_slot_.find(index); it != by_slot_.end() && it->second == Key{start, end}) {
        return {};
    }
    forget(index);
    if (end <= start) {
        return {};
    }
    std::vector<std::uint32_t> stale;
    for (const Key& span : against(start, end, desc)) {
        if (!newer) {
            return stale;
        }
        if (auto it = spans_.find(span); it != spans_.end()) {
            for (std::uint32_t slot : it->second.slots) {
                by_slot_.erase(slot);
                stale.push_back(slot);
            }
            spans_.erase(it);
        }
    }
    auto [it, fresh] = spans_.try_emplace(Key{start, end}, Span{desc, {}});
    it->second.slots.insert(index);
    by_slot_[index] = Key{start, end};
    return stale;
}

void Spans::forget(std::uint32_t index) {
    auto it = by_slot_.find(index);
    if (it == by_slot_.end()) {
        return;
    }
    Key at = it->second;
    by_slot_.erase(it);
    if (auto s = spans_.find(at); s != spans_.end()) {
        s->second.slots.erase(index);
        if (s->second.slots.empty()) {
            spans_.erase(s);
        }
    }
}

std::pair<Spans::Claim, std::optional<std::pair<std::uint64_t, std::uint64_t>>> Spans::claim(std::uint32_t index, std::uint64_t start, std::uint64_t end,
                                                                                           const std::array<std::uint32_t, 14>& desc) const {
    if (auto it = by_slot_.find(index); it != by_slot_.end() && it->second == Key{start, end}) {
        return {Claim::Own, std::nullopt};
    }
    auto spans = against(start, end, desc);
    if (spans.empty()) {
        return {Claim::Free, std::nullopt};
    }
    if (std::all_of(spans.begin(), spans.end(), [&](const Key& s) { return s.first == start; })) {
        return {Claim::SameStart, spans.front()};
    }
    return {Claim::Other, spans.front()};
}

std::vector<std::uint32_t> Spans::slots_in(std::uint64_t start, std::uint64_t end) const {
    std::vector<std::uint32_t> out;
    for (const Key& at : overlapping(start, end)) {
        if (auto it = spans_.find(at); it != spans_.end()) {
            out.insert(out.end(), it->second.slots.begin(), it->second.slots.end());
        }
    }
    return out;
}


void TextureQueue::written(std::uint32_t index, std::uint64_t frame, std::uint64_t due, bool seen) {
    if (seen && slots.contains(index)) {
        return;
    }
    ++writes_;
    rechecks.erase(index);
    slots[index] = Waiting{due, frame, writes_, 0, 0};
}

void TextureQueue::remove(std::uint32_t index) {
    slots.erase(index);
    rechecks.erase(index);
}

void TextureQueue::recheck(std::uint32_t index, std::uint64_t frame, std::pair<std::uint64_t, std::uint64_t> at, std::uint64_t print) {
    if (!slots.contains(index)) {
        rechecks[index] = Recheck{frame + RECHECK_FRAMES[0], at, print, 0, frame};
    }
}

std::vector<std::pair<std::uint32_t, Recheck>> TextureQueue::rechecks_due(std::uint64_t frame, std::size_t max) const {
    std::vector<std::pair<std::uint32_t, Recheck>> due;
    for (const auto& [index, r] : rechecks) {
        if (r.due <= frame) {
            due.emplace_back(index, r);
        }
    }
    std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return std::pair{a.second.due, a.first} < std::pair{b.second.due, b.first}; });
    if (due.size() > max) {
        due.resize(max);
    }
    return due;
}

void TextureQueue::rechecked(std::uint32_t index, const Recheck& seen, bool changed_now, std::uint64_t frame) {
    auto it = rechecks.find(index);
    if (it == rechecks.end() || !(it->second == seen)) {
        return;
    }
    if (changed_now) {
        changed(index, frame, frame);
        return;
    }
    std::size_t next = std::size_t{seen.looks} + 1;
    if (next < std::size(RECHECK_FRAMES)) {
        Recheck r = seen;
        r.due = frame + RECHECK_FRAMES[next];
        r.looks = static_cast<std::uint8_t>(seen.looks + 1);
        it->second = r;
    } else {
        rechecks.erase(it);
    }
}

std::vector<std::pair<std::uint32_t, std::uint64_t>> TextureQueue::due(std::uint64_t frame, std::size_t max) const {
    struct E {
        std::uint64_t written;
        std::uint32_t index;
        std::uint64_t write;
    };
    std::vector<E> again, fresh;
    for (const auto& [index, w] : slots) {
        if (w.due > frame) {
            continue;
        }
        (w.looks > 0 || w.mapped > 0 ? again : fresh).push_back({w.written, index, w.write});
    }
    auto newest_first = [](const E& a, const E& b) { return b.write < a.write; };
    std::sort(again.begin(), again.end(), newest_first);
    std::sort(fresh.begin(), fresh.end(), newest_first);
    std::vector<std::pair<std::uint32_t, std::uint64_t>> order;
    for (const E& e : again) {
        order.emplace_back(e.index, e.write);
    }
    std::size_t nw = 0, old = fresh.size();
    while (nw < old && order.size() < max) {
        order.emplace_back(fresh[nw].index, fresh[nw].write);
        ++nw;
        if (nw < old && order.size() < max) {
            --old;
            order.emplace_back(fresh[old].index, fresh[old].write);
        }
    }
    if (order.size() > max) {
        order.resize(max);
    }
    return order;
}

TextureQueue::Waiting* TextureQueue::same(std::uint32_t index, std::uint64_t write) {
    auto it = slots.find(index);
    return it != slots.end() && it->second.write == write ? &it->second : nullptr;
}

void TextureQueue::done(std::uint32_t index, std::uint64_t write) {
    if (same(index, write)) {
        slots.erase(index);
    }
}

void TextureQueue::later(std::uint32_t index, std::uint64_t write, std::uint64_t frame, std::uint64_t most) {
    if (Waiting* w = same(index, write)) {
        w->due = frame + std::min<std::uint64_t>(std::uint64_t{1} << std::min<std::uint8_t>(w->looks, 16), std::max<std::uint64_t>(most, 1));
        w->looks = static_cast<std::uint8_t>(std::min(255, w->looks + 1));
    }
}

void TextureQueue::partly(std::uint32_t index, std::uint64_t write, std::uint64_t due_at, std::uint64_t mapped_bytes) {
    if (Waiting* w = same(index, write)) {
        w->due = due_at;
        w->looks = 0;
        w->mapped = std::max<std::uint64_t>(mapped_bytes, 1);
    }
}

void TextureQueue::changed(std::uint32_t index, std::uint64_t frame, std::uint64_t due_at) {
    ++writes_;
    auto it = slots.find(index);
    std::uint64_t d = it == slots.end() ? due_at : std::min(it->second.due, due_at);
    rechecks.erase(index);
    slots[index] = Waiting{d, frame, writes_, 0, 0};
}

std::uint64_t TextureQueue::mapped(std::uint32_t index) const {
    auto it = slots.find(index);
    return it == slots.end() ? 0 : it->second.mapped;
}

std::array<std::uint64_t, 5> TextureQueue::census(std::uint64_t frame) const {
    std::array<std::uint64_t, 5> c{slots.size(), 0, 0, 0, 0};
    for (const auto& [index, w] : slots) {
        if (w.mapped > 0) {
            ++c[3];
        } else if (w.looks > 0) {
            ++c[2];
        }
        if (w.due <= frame) {
            ++c[1];
            c[4] = std::max(c[4], frame > w.written ? frame - w.written : 0);
        }
    }
    return c;
}


bool is_texture_view(const View& view) {
    switch (view.kind) {
        case View::Placed: return !view.flag && placed_resource(view)[0] != 1;
        case View::Shader: return !is_buffer_object(view.a);
        default: return false;
    }
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> buffer_range(const View& view) {
    std::uint64_t base;
    const std::uint32_t* words;
    switch (view.kind) {
        case View::Shader: {
            if (!view.flag) {
                return std::nullopt;
            }
            auto b = buffer_of(view.a);
            if (!b) {
                return std::nullopt;
            }
            base = reinterpret_cast<std::uint64_t>(b->first);
            words = view.words.data();
            break;
        }
        case View::Unordered: {
            if (!view.flag) {
                return std::nullopt;
            }
            auto b = buffer_of(view.a);
            if (!b) {
                return std::nullopt;
            }
            return unordered_range(reinterpret_cast<std::uint64_t>(b->first), view.words.data());
        }
        case View::Placed:
            if (placed_resource(view)[0] != 1) {
                return std::nullopt;
            }
            if (view.flag) {
                return unordered_range(placed_address(view), placed_desc(view));
            }
            base = placed_address(view);
            words = placed_desc(view);
            break;
        default: return std::nullopt;
    }
    auto desc = srv_desc(words);
    std::uint64_t element = element_size(desc);
    return std::pair{base + desc.Buffer.FirstElement * element, std::uint64_t{desc.Buffer.NumElements} * element};
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> buffer_extent(const View& view) {
    switch (view.kind) {
        case View::Shader:
        case View::Unordered: {
            if (!view.flag) {
                return std::nullopt;
            }
            auto b = buffer_of(view.a);
            if (!b) {
                return std::nullopt;
            }
            return std::pair{reinterpret_cast<std::uint64_t>(b->first), b->second};
        }
        case View::Placed:
            if (placed_resource(view)[0] != 1) {
                return std::nullopt;
            }
            return std::pair{placed_address(view), std::uint64_t{placed_resource(view)[4]} | std::uint64_t{placed_resource(view)[5]} << 32};
        default: return std::nullopt;
    }
}

std::optional<BufferCopy> buffer_copy(const View& view) {
    auto range = buffer_range(view);
    if (!range || range->second == 0) {
        return std::nullopt;
    }
    auto [address, len] = *range;
    if (view_extent()) {
        if (auto extent = buffer_extent(view)) {
            auto [base, width] = *extent;
            if (address >= base && address - base < std::max<std::uint64_t>(width, 1)) {
                if (auto s = shadow_find(base, width)) {
                    return BufferCopy{std::move(s), address, len};
                }
            }
        }
    }
    auto s = shadow_find(address, len);
    if (!s) {
        return std::nullopt;
    }
    return BufferCopy{std::move(s), address, len};
}

std::optional<View> placed_texture(const View& view) {
    if (view.kind != View::Placed || view.flag || placed_resource(view)[0] == BUFFER_DIMENSION) {
        return std::nullopt;
    }
    This texture = texture_at(placed_address(view), placed_resource(view));
    if (!texture) {
        return std::nullopt;
    }
    View v{View::Shader};
    v.a = reinterpret_cast<std::uint64_t>(texture);
    v.flag = true;
    v.words = {};
    std::copy_n(placed_desc(view), 10, v.words.begin());
    return v;
}

void remapped(std::uintptr_t at, std::size_t len) {
    constexpr std::size_t MOST = 1u << 18;
    std::uint64_t start = at, end = at + len < at ? UINT64_MAX : at + len;
    if (end <= SPANNED[0].load(std::memory_order_relaxed) || start >= SPANNED[1].load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock(REMAPPED_LOCK);
    STALE[1].fetch_add(1, std::memory_order_relaxed);
    if (!REMAPPED.empty() && REMAPPED.back().second == start) {
        REMAPPED.back().second = end;
        return;
    }
    if (REMAPPED.size() >= MOST) {
        STALE[2].fetch_add(REMAPPED.size(), std::memory_order_relaxed);
        REMAPPED.clear();
    }
    REMAPPED.emplace_back(start, end);
}


std::unique_ptr<HostHeap> HostHeap::make(bool sampler, std::uint32_t count) {
    ID3D12Device* d = device();
    if (!d) {
        return nullptr;
    }
    D3D12_DESCRIPTOR_HEAP_TYPE kind = sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HRESULT made = S_OK;
    auto make_heap = [&](std::uint32_t n) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{kind, n, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ComPtr<ID3D12DescriptorHeap> heap;
        made = d->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
        return SUCCEEDED(made) ? heap : nullptr;
    };
    std::uint32_t extra = sampler ? 0 : DYNAMIC;
    if (!sampler) {
        probe_view_limit();
    }
    std::uint32_t null_slot = sampler ? 0 : view_heap_limit();
    ComPtr<ID3D12DescriptorHeap> heap;
    std::uint32_t fitted, size;
    if (null_slot == 0) {
        heap = make_heap(count + extra);
        fitted = count;
        size = count + extra;
        if (!heap) {
            log::warn(TARGET, "no host descriptor heap", {{"error", log::hresult(made)}, {"count", log::debug(count)}, {"sampler", log::debug(sampler)}});
        }
    } else {
        size = null_slot + 1;
        fitted = count;
        if (count + extra > null_slot) {

            log::warn(TARGET, "host view heap capped at " + std::to_string(size) + "; views past it read a null slot", {{"count", log::debug(count)}});
            fitted = null_slot - extra;
        }
        heap = make_heap(size);
        if (!heap) {
            log::warn(TARGET, "no host view heap", {{"error", log::hresult(made)}, {"count", log::debug(count)}, {"size", log::debug(size)}});
        }
    }
    if (!heap) {
        return nullptr;
    }
    std::unique_ptr<HostHeap> h(new HostHeap);
    h->heap_ = heap;
    h->cpu_ = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    h->gpu_ = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    h->step_ = d->GetDescriptorHandleIncrementSize(kind);
    h->count_ = fitted;
    h->dynamic_ = extra;
    h->sampler_ = sampler;
    h->made_.assign(fitted, 0);
    h->kept_.resize(std::size_t{fitted} + extra);
    h->taken_.assign(DYNAMIC, 0);
    if (!sampler) {


        constexpr std::uint64_t STRIDES_COUNT = 1u << 21;
        if (auto b = host()->device().upload_buffer(STRIDES_COUNT * 4)) {

            h->strides_ = b->first;
            h->strides_gpu_ = b->first->GetGPUVirtualAddress();
            h->strides_data_ = reinterpret_cast<std::uint32_t*>(b->second);
        }
        h->tabled_count_ = fitted;
        h->tabled_.reset(new std::atomic<std::uint32_t>[fitted]);
        for (std::uint32_t i = 0; i < fitted; ++i) {
            h->tabled_[i].store(0, std::memory_order_relaxed);
        }
    }
    h->fill_null(kind, size);
    return h;
}

HostHeap::~HostHeap() = default;

void HostHeap::fill_null(D3D12_DESCRIPTOR_HEAP_TYPE kind, std::uint32_t total) {
    constexpr std::uint32_t BLOCK = 1u << 16;
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC desc{kind, BLOCK, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> block;
    if (HRESULT hr = d->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&block)); FAILED(hr)) {
        log::warn(TARGET, "host descriptor heap not made null", {{"error", log::hresult(hr)}});
        return;
    }
    auto start = block->GetCPUDescriptorHandleForHeapStart();
    auto at = [&](std::uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{start.ptr + std::size_t{i} * step_}; };
    if (sampler_) {
        null_sampler_at(start);
    } else {
        null_view(start);
    }
    std::uint32_t filled = 1;
    while (filled < BLOCK) {
        std::uint32_t n = std::min(filled, BLOCK - filled);
        d->CopyDescriptorsSimple(n, at(filled), start, kind);
        filled += n;
    }
    std::uint32_t done = 0;
    while (done < total) {
        std::uint32_t n = std::min(BLOCK, total - done);
        d->CopyDescriptorsSimple(n, cpu(done), start, kind);
        done += n;
    }
}

void HostHeap::null_sampler(std::uint32_t index) { null_sampler_at(cpu(index)); }

std::optional<std::uint32_t> HostHeap::dynamic(std::uint32_t n) {
    if (sampler_ || n == 0 || n > dynamic_) {
        return std::nullopt;
    }
    std::lock_guard lock(next_lock_);
    bool enough = free_.load(std::memory_order_relaxed) >= n && n < too_long_.load(std::memory_order_relaxed);
    std::optional<std::size_t> found;
    if (enough) {

        std::size_t run = 0, last = SIZE_MAX, looked = 0;
        auto visit = [&](std::size_t at) {
            if (at != last + 1) {
                run = 0;
            }
            last = at;
            run = taken_[at] ? 0 : run + 1;
            return run == n;
        };
        for (std::size_t at = next_; at < dynamic_ && looked < DYNAMIC && !found; ++at, ++looked) {
            if (visit(at)) {
                found = at + 1 - n;
            }
        }
        for (std::size_t at = 0; at < next_ && looked < DYNAMIC && !found; ++at, ++looked) {
            if (visit(at)) {
                found = at + 1 - n;
            }
        }
    }
    if (!found) {
        if (enough) {
            std::uint32_t cur = too_long_.load(std::memory_order_relaxed);
            while (n < cur && !too_long_.compare_exchange_weak(cur, n, std::memory_order_relaxed)) {
            }
        }
        HEAP_ONCE("no free dynamic descriptor slots: a table is not made", {{"n", log::debug(n)}});
        return std::nullopt;
    }
    std::fill(taken_.begin() + static_cast<std::ptrdiff_t>(*found), taken_.begin() + static_cast<std::ptrdiff_t>(*found + n), std::uint8_t{1});
    free_.fetch_sub(n, std::memory_order_relaxed);
    next_ = static_cast<std::uint32_t>((*found + n) % DYNAMIC);
    return count_ + static_cast<std::uint32_t>(*found);
}

void HostHeap::release(std::uint32_t first, std::uint32_t n) {
    if (first < count_) {
        return;
    }
    std::uint32_t at = first - count_;
    std::lock_guard lock(next_lock_);
    std::uint32_t end = std::min(at + n, dynamic_);
    if (at > end) {
        return;
    }
    std::uint32_t given = 0;
    for (std::uint32_t i = at; i < end; ++i) {
        given += taken_[i];
        taken_[i] = 0;
    }
    free_.fetch_add(given, std::memory_order_relaxed);
    too_long_.store(UINT32_MAX, std::memory_order_relaxed);
}

std::pair<std::vector<std::pair<std::uint32_t, std::uint64_t>>, std::array<std::uint64_t, 5>> HostHeap::texture_slots_due(std::uint64_t frame,
                                                                                                                         std::size_t max) {
    std::lock_guard lock(notes_lock_);
    return {max == 0 ? std::vector<std::pair<std::uint32_t, std::uint64_t>>{} : textures_.due(frame, max), textures_.census(frame)};
}

std::vector<std::pair<std::uint32_t, bool>> HostHeap::texture_slots_all_due(std::uint64_t frame) {
    std::lock_guard lock(notes_lock_);
    std::vector<std::pair<std::uint32_t, bool>> out;
    for (const auto& [index, w] : textures_.slots) {
        if (w.due <= frame) {
            out.emplace_back(index, w.mapped > 0 || textures_.rechecks.contains(index));
        }
    }
    return out;
}

void HostHeap::texture_slot_done(std::uint32_t index, std::uint64_t write, std::optional<std::pair<std::uint64_t, std::uint64_t>> again) {
    std::lock_guard lock(notes_lock_);
    if (again) {
        textures_.partly(index, write, again->first, again->second);
    } else {
        textures_.done(index, write);
    }
}

void HostHeap::texture_slot_recheck(std::uint32_t index, std::uint64_t frame, std::pair<std::uint64_t, std::uint64_t> at, std::uint64_t print) {
    std::lock_guard lock(notes_lock_);
    textures_.recheck(index, frame, at, print);
}

std::array<std::uint64_t, 3> HostHeap::texture_slots_rechecked(std::uint64_t frame) {
    constexpr std::size_t MOST = 64;
    constexpr std::uint64_t SPAN_NS = 2'000'000;
    std::vector<std::pair<std::uint32_t, Recheck>> due;
    std::uint64_t waiting;
    {
        std::lock_guard lock(notes_lock_);
        due = textures_.rechecks_due(frame, MOST);
        waiting = textures_.rechecks.size();
    }
    auto began = std::chrono::steady_clock::now();
    std::array<std::uint64_t, 3> found{0, 0, waiting};
    for (const auto& [index, seen] : due) {
        if (static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count()) > SPAN_NS) {
            break;
        }
        bool changed = fingerprint(seen.at.first, seen.at.second) != seen.print;
        ++found[changed ? 1 : 0];
        if (changed) {
            static std::atomic<std::uint64_t> said{0};
            std::uint64_t n = said.fetch_add(1, std::memory_order_relaxed);
            if (n < 40 || n % 1000 == 0) {
                log::info(TARGET, "a texture picked by index changed after its upload with nothing to tell of it: uploaded again",
                          {{"slot", log::debug(index)},
                           {"address", log::hex(seen.at.first)},
                           {"bytes", log::debug(static_cast<std::uint64_t>(seen.at.second))},
                           {"frames_after_upload", log::debug(frame > seen.uploaded ? frame - seen.uploaded : 0)},
                           {"looks", log::debug(int{seen.looks})},
                           {"changed", log::debug(n + 1)}});
            }
        }
        std::lock_guard lock(notes_lock_);
        textures_.rechecked(index, seen, changed, frame);
    }
    return found;
}

void HostHeap::texture_slot_later(std::uint32_t index, std::uint64_t write, std::uint64_t frame, std::uint64_t most) {
    std::lock_guard lock(notes_lock_);
    textures_.later(index, write, frame, most);
}

std::uint64_t HostHeap::texture_slot_mapped(std::uint32_t index) {
    std::lock_guard lock(notes_lock_);
    return textures_.mapped(index);
}

std::size_t HostHeap::texture_slots_remapped(std::uint64_t frame) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    {
        std::lock_guard lock(REMAPPED_LOCK);
        ranges.swap(REMAPPED);
    }
    if (ranges.empty()) {
        return 0;
    }
    std::vector<std::uint32_t> slots;
    {
        std::lock_guard lock(notes_lock_);
        for (const auto& [start, end] : ranges) {
            auto in = placed_.slots_in(start, end);
            slots.insert(slots.end(), in.begin(), in.end());
        }
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    std::vector<bool> lately;
    lately.reserve(slots.size());
    for (std::uint32_t slot : slots) {
        lately.push_back(tabled_lately(slot, frame));
    }
    std::lock_guard lock(notes_lock_);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        textures_.changed(slots[i], frame, frame + (lately[i] ? TABLED_GRACE : 1));
    }
    STALE[3].fetch_add(slots.size(), std::memory_order_relaxed);
    return slots.size();
}

bool HostHeap::texture_slot_made_from(std::uint32_t index, std::uint64_t handle) {
    if (index >= count_) {
        return false;
    }
    auto [tag, i, generation] = record_at(handle);
    std::uint64_t key = std::uint64_t{generation} << 32 | i;
    if (tag != VIEW_RECORD_TAG) {
        return false;
    }
    std::lock_guard made(made_lock_);
    if (index >= made_.size() || made_[index] == 0 || made_[index] != key) {
        return false;
    }
    std::lock_guard kept(kept_lock_);
    return index < kept_.size() && kept_[index] != nullptr;
}

void HostHeap::texture_sweep(DescHeap& heap, std::uint64_t frame) {
    constexpr std::uint32_t SLOTS = 1u << 13;
    constexpr std::uint64_t EVERY_MS = 250;
    static const int mode = [] {
        wchar_t v[16] = {};
        DWORD n = GetEnvironmentVariableW(L"VICTORY_TEXTURE_SWEEP", v, 16);
        if (n > 0 && n < 16) {
            if (std::wstring(v) == L"0") {
                return 0;
            }
            if (std::wstring(v) == L"count") {
                return 1;
            }
        }
        return 2;
    }();
    if (mode == 0 || sampler_) {
        return;
    }
    std::uint64_t now = now_ms();
    std::unique_lock lock(SWEEP_LOCK, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    if (now - std::min(now, SWEEP.at) < EVERY_MS) {
        return;
    }
    SWEEP.at = now;
    std::uint32_t from = std::min(SWEEP.next, count_);
    std::uint32_t to = std::min<std::uint64_t>(std::uint64_t{from} + SLOTS, count_);
    auto counts = SWEEP.counts;
    sweep_part(from, to, heap, frame, mode == 2, counts);
    SWEEP.next = to;
    SWEEP.counts = counts;
    if (to >= count_) {
        for (std::size_t k = 0; k < counts.size(); ++k) {
            SWEPT[k].store(counts[k], std::memory_order_relaxed);
        }
        SWEEP.next = 0;
        SWEEP.counts = {};
    }
}

void HostHeap::sweep_part(std::uint32_t from, std::uint32_t to, DescHeap& heap, std::uint64_t frame, bool put_back, std::array<std::uint64_t, 10>& counts) {
    constexpr std::uint32_t MOST = 128;
    constexpr std::size_t SHORT = 4000;
    std::uint32_t back = 0;
    struct Unmade {
        std::uint32_t index, i, generation;
        bool other;
    };
    std::vector<Unmade> unmade;
    {
        std::lock_guard lock(made_lock_);
        std::pair<std::uint64_t, bool> page{UINT64_MAX, false};
        for (std::uint32_t index = from; index < to; ++index) {
            auto handle = heap_handle(heap, index);
            if (!handle) {
                break;
            }
            if (page.first != *handle >> 12) {
                page = {*handle >> 12, committed(reinterpret_cast<const void*>(*handle), 12)};
            }
            if (!page.second) {
                continue;
            }
            auto [tag, i, generation] = record_at(*handle);
            if (tag != VIEW_RECORD_TAG || generation >> VIEW_KIND_SHIFT == VIEW_KIND_CONSTANT) {
                continue;
            }
            std::uint64_t key = std::uint64_t{generation} << 32 | i;
            std::uint64_t shown = index < made_.size() ? made_[index] : 0;
            if (shown == key) {
                ++counts[0];
                ++counts[1];
            } else {
                unmade.push_back({index, i, generation, shown != 0});
            }
        }
    }
    for (const Unmade& u : unmade) {
        auto view = view_at_words(u.i, u.generation);
        if (!view || view->kind != View::Placed || view->flag) {
            continue;
        }
        if (placed_resource(*view)[0] == 1) {
            continue;
        }
        ++counts[0];
        bool queued;
        {
            std::lock_guard lock(notes_lock_);
            queued = textures_.slots.contains(u.index);
        }
        if (queued) {
            ++counts[2];
            continue;
        }
        if (tabled_lately(u.index, frame)) {
            ++counts[3];
            continue;
        }
        std::uint64_t start = placed_address(*view);
        if (!committed(reinterpret_cast<const void*>(start), 1)) {
            ++counts[4];
            continue;
        }
        Spans::Claim claim;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> against;
        {
            std::lock_guard lock(notes_lock_);
            auto resource = placed_resource_words(*view);
            auto bytes = placed_.bytes(resource, first_mip(placed_desc(*view), 16));
            if (!bytes) {
                continue;
            }
            std::tie(claim, against) = placed_.claim(u.index, start, start + *bytes < start ? UINT64_MAX : start + *bytes, resource);
        }
        ++counts[5 + static_cast<std::size_t>(claim)];
        counts[9] += u.other ? 1 : 0;
        static std::atomic<std::uint32_t> said[4]{};
        if (said[static_cast<std::size_t>(claim)].fetch_add(1, std::memory_order_relaxed) < 12) {
            static constexpr const char* CLAIMS[4] = {"Own", "Free", "SameStart", "Other"};
            const std::uint32_t* resource = placed_resource(*view);
            const std::uint32_t* desc = placed_desc(*view);
            char b[160];
            std::string against_text = "None";
            if (against) {
                std::snprintf(b, sizeof b, "Some((%llx, %llx))", static_cast<unsigned long long>(against->first),
                              static_cast<unsigned long long>(against->second));
                against_text = b;
            }
            std::string desc_text = "[";
            for (int k = 0; k < 10; ++k) {
                std::snprintf(b, sizeof b, "%s%x", k ? ", " : "", desc[k]);
                desc_text += b;
            }
            desc_text += "]";
            std::snprintf(b, sizeof b, "%ux%u f%u mips %u at 0x%llx, shows %s", resource[4], resource[6], resource[8], resource[7] >> 16,
                          static_cast<unsigned long long>(start), u.other ? "another view" : "nothing");
            log::info(TARGET, "a texture view nothing brings up to date: slot " + std::to_string(u.index) + ", " + CLAIMS[static_cast<std::size_t>(claim)] +
                                  ", " + b + "; against " + against_text + "; its view " + desc_text);
        }
        if (put_back && back < MOST && (claim == Spans::Claim::Own || claim == Spans::Claim::Free)) {
            std::lock_guard lock(notes_lock_);
            if (textures_.slots.size() < SHORT) {
                textures_.written(u.index, frame, frame + 1, false);
                ++back;
                SWEPT[10].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

bool HostHeap::texture_slot_gone(std::uint32_t index) {
    std::lock_guard lock(made_lock_);
    if (index >= made_.size() || made_[index] == 0) {
        return false;
    }
    unmake(made_, index);
    {
        std::lock_guard t(notes_lock_);
        textures_.rechecks.erase(index);
    }
    STALE[4].fetch_add(1, std::memory_order_relaxed);
    return true;
}

void HostHeap::unmake(std::vector<std::uint64_t>& made, std::uint32_t index) {
    if (index >= made.size()) {
        return;
    }
    std::uint64_t key = std::exchange(made[index], 0);
    if (key == 0) {
        return;
    }
    if (auto view = view_at_words(static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(key >> 32))) {
        null_for(*view, cpu(index));
    } else {
        null_view(cpu(index));
    }
    keep(index, nullptr);
}

void HostHeap::texture_slot_tabled(std::uint32_t index, bool read) { tabled_in(index, read, frame_current()); }

void HostHeap::tabled_in(std::uint32_t index, bool read, std::uint64_t frame) {
    if (index >= tabled_count_) {
        return;
    }
    auto& stamp = tabled_[index];
    std::uint32_t now = (static_cast<std::uint32_t>(frame) + 1) & ~TABLED_READ;
    std::uint32_t old = stamp.load(std::memory_order_relaxed);
    if (!read) {
        if ((old & ~TABLED_READ) != now) {
            stamp.store(now, std::memory_order_relaxed);
        }
        return;
    }
    if (old != (now | TABLED_READ)) {
        stamp.store(now | TABLED_READ, std::memory_order_relaxed);
        if (tabled_out()) {
            std::lock_guard lock(notes_lock_);
            if (textures_.slots.erase(index) > 0) {
                TABLED_WRITES[1].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

std::uint32_t HostHeap::tabled_stamp(std::uint32_t index) const { return index < tabled_count_ ? tabled_[index].load(std::memory_order_relaxed) : 0; }

bool HostHeap::tabled_lately(std::uint32_t index, std::uint64_t frame) const {
    std::uint32_t stamp = tabled_stamp(index) & ~TABLED_READ;
    std::uint32_t now = (static_cast<std::uint32_t>(frame) + 1) & ~TABLED_READ;
    return tabled_out() && stamp != 0 && std::uint64_t{now - stamp} <= TABLED_FRAMES;
}

void HostHeap::queue_texture(std::uint32_t index, bool texture, bool seen, std::uint64_t frame) {
    std::uint32_t stamp = tabled_out() ? tabled_stamp(index) : 0;
    std::uint32_t now = (static_cast<std::uint32_t>(frame) + 1) & ~TABLED_READ;
    bool lately = stamp != 0 && std::uint64_t{now - (stamp & ~TABLED_READ)} <= TABLED_FRAMES;
    bool read_now = seen && stamp == (now | TABLED_READ);
    std::lock_guard lock(notes_lock_);
    if (texture && !read_now) {
        std::uint64_t due = 0;
        if (lately && !seen) {
            TABLED_WRITES[0].fetch_add(1, std::memory_order_relaxed);
            due = frame + TABLED_GRACE;
        }
        textures_.written(index, frame, due, seen);
    } else {
        textures_.remove(index);
    }
}

std::vector<std::uint32_t> HostHeap::buffer_slots() {
    std::lock_guard lock(notes_lock_);
    std::vector<std::uint32_t> out;
    for (const auto& bucket : buffers_) {
        out.insert(out.end(), bucket.begin(), bucket.end());
    }
    return out;
}

std::vector<std::uint32_t> HostHeap::buffer_slots_bucket(std::uint32_t bucket) {
    std::lock_guard lock(notes_lock_);
    const auto& b = buffers_[bucket & 63];
    return {b.begin(), b.end()};
}

std::vector<std::uint32_t> HostHeap::buffer_changes() {
    std::lock_guard lock(notes_lock_);
    std::vector<std::uint32_t> out(changed_.begin(), changed_.end());
    changed_.clear();
    return out;
}

std::vector<std::uint32_t> HostHeap::note_buffer(std::uint32_t index, const View* view, bool seen) {
    if (sampler_) {
        return {};
    }
    bool texture = false;
    if (view) {
        if (view->kind == View::Placed && !view->flag && placed_resource(*view)[0] != 1) {
            PLACED_TEXTURE_SLOTS.fetch_add(1, std::memory_order_relaxed);
            texture = true;
        } else if (view->kind == View::Shader && !is_buffer_object(view->a)) {
            OBJECT_TEXTURE_SLOTS.fetch_add(1, std::memory_order_relaxed);
            texture = true;
        }
    }
    std::uint64_t frame = frame_current();
    queue_texture(index, texture, seen, frame);

    struct PlacedView {
        std::array<std::uint32_t, 14> resource;
        std::uint64_t start;
        std::uint32_t first;
    };
    std::optional<PlacedView> placed_view;
    if (view && view->kind == View::Placed && !view->flag && placed_resource(*view)[0] != 1 && !tabled_lately(index, frame)) {
        placed_view = PlacedView{placed_resource_words(*view), placed_address(*view), first_mip(placed_desc(*view), 16)};
    }
    std::vector<std::uint32_t> stale;
    {
        std::lock_guard lock(notes_lock_);
        std::optional<std::uint64_t> end;
        if (placed_view) {
            if (auto bytes = placed_.bytes(placed_view->resource, placed_view->first)) {
                end = placed_view->start + *bytes < placed_view->start ? UINT64_MAX : placed_view->start + *bytes;
            }
        }
        if (placed_view && end) {
            std::uint64_t cur = SPANNED[0].load(std::memory_order_relaxed);
            while (placed_view->start < cur && !SPANNED[0].compare_exchange_weak(cur, placed_view->start, std::memory_order_relaxed)) {
            }
            cur = SPANNED[1].load(std::memory_order_relaxed);
            while (*end > cur && !SPANNED[1].compare_exchange_weak(cur, *end, std::memory_order_relaxed)) {
            }
            stale = placed_.note(index, placed_view->start, *end, placed_view->resource, !seen && stale_out());
        } else {
            placed_.forget(index);
        }
    }
    if (!stale.empty()) {
        STALE[0].fetch_add(stale.size(), std::memory_order_relaxed);
        std::lock_guard lock(notes_lock_);
        for (std::uint32_t slot : stale) {
            textures_.remove(slot);
        }
    }
    std::optional<std::uint32_t> stride = view ? view_stride(*view) : std::nullopt;
    constexpr std::uint32_t STRIDES_COUNT = 1u << 21;
    if (strides_data_ && index < STRIDES_COUNT) {
        reinterpret_cast<volatile std::uint32_t*>(strides_data_)[index] = stride.value_or(0);
    }
    bool was;
    {
        std::lock_guard lock(notes_lock_);
        auto& bucket = buffers_[index & 63];
        if (stride) {
            bucket.insert(index);
            was = true;
        } else {
            was = bucket.erase(index) > 0;
        }
    }
    if (was) {
        std::lock_guard lock(notes_lock_);
        changed_.insert(index);
    }
    return stale;
}

void HostHeap::make_at(std::uint32_t index, const View& view) {
    auto kept = make_view(view, cpu(index));
    if (!kept) {
        null_for(view, cpu(index));
    }
    keep(index, kept ? *kept : nullptr);
}

void HostHeap::make_null(std::uint32_t index) {
    if (sampler_) {
        null_sampler(index);
    } else {
        null_view(cpu(index));
    }
    keep(index, nullptr);
}

std::uint64_t HostHeap::evict_kept(const std::unordered_set<std::uintptr_t>& victims) {
    if (sampler_ || victims.empty()) {
        return 0;
    }
    std::vector<std::uint32_t> evicted;
    std::vector<ComPtr<ID3D12Resource>> removed;
    {
        std::lock_guard lock(kept_lock_);
        std::size_t limit = std::min<std::size_t>(kept_.size(), count_);
        for (std::size_t i = 0; i < limit; ++i) {
            if (kept_[i] && victims.contains(reinterpret_cast<std::uintptr_t>(kept_[i].Get()))) {
                null_view(cpu(static_cast<std::uint32_t>(i)));
                removed.push_back(std::move(kept_[i]));
                evicted.push_back(static_cast<std::uint32_t>(i));
            }
        }
    }
    if (evicted.empty()) {
        return 0;
    }
    for (auto& r : removed) {
        retire_resource(std::move(r));
    }
    {
        std::lock_guard lock(made_lock_);
        for (std::uint32_t slot : evicted) {
            if (slot < made_.size()) {
                made_[slot] = 0;
            }
        }
    }
    {
        std::lock_guard lock(notes_lock_);
        for (std::uint32_t slot : evicted) {
            placed_.forget(slot);
        }
    }
    {
        std::lock_guard lock(notes_lock_);
        for (std::uint32_t slot : evicted) {
            textures_.remove(slot);
        }
    }
    return evicted.size();
}

void HostHeap::keep(std::uint32_t index, ComPtr<ID3D12Resource> resource) {
    ComPtr<ID3D12Resource> old;
    {
        std::lock_guard lock(kept_lock_);
        if (index >= kept_.size()) {
            return;
        }
        old = std::exchange(kept_[index], std::move(resource));
    }

    retire_resource(std::move(old));
}

void HostHeap::raw_at(std::uint32_t index, ID3D12Resource* buffer, std::uint64_t offset, std::uint64_t len) {
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = offset / 4;
    desc.Buffer.NumElements = static_cast<UINT>(std::clamp<std::uint64_t>(len / 4, 1, UINT32_MAX));
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    d->CreateShaderResourceView(buffer, &desc, cpu(index));
    keep(index, buffer);
}

void HostHeap::null_raw_at(std::uint32_t index) {
    null_buffer_view(cpu(index));
    keep(index, nullptr);
}

void HostHeap::raw_uav_at(std::uint32_t index, ID3D12Resource* buffer, std::uint64_t offset, std::uint64_t len) {
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = offset / 4;
    desc.Buffer.NumElements = static_cast<UINT>(std::clamp<std::uint64_t>(len / 4, 1, UINT32_MAX));
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    d->CreateUnorderedAccessView(buffer, nullptr, &desc, cpu(index));
    keep(index, buffer);
}

void HostHeap::null_raw_uav_at(std::uint32_t index) {
    keep(index, nullptr);
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    d->CreateUnorderedAccessView(nullptr, nullptr, &desc, cpu(index));
}

void HostHeap::constant_at(std::uint32_t index, std::uint64_t gpu, std::uint32_t size) {
    keep(index, nullptr);
    ID3D12Device* d = device();
    if (!d) {
        return;
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{gpu, constant_size(size)};
    d->CreateConstantBufferView(&desc, cpu(index));
}

HostHeap::BufferMirror HostHeap::mirror_buffer(std::uint32_t index, std::uint64_t handle, std::uint64_t cached_key) {
    if (cached_key != 0 && index < count_) {
        auto [tag, i, generation] = record_at(handle);
        std::uint64_t key = std::uint64_t{generation} << 32 | i;
        if (tag == VIEW_RECORD_TAG && key == cached_key && view_at_words(i, generation)) {
            std::lock_guard lock(made_lock_);
            if (made_[index] == key) {
                return {BufferMirror::Unchanged};
            }
        }
    }
    if (auto view = mirror(index, handle, true)) {
        auto [tag, i, generation] = record_at(handle);
        return {BufferMirror::Changed, std::uint64_t{generation} << 32 | i, std::move(view)};
    }
    return {BufferMirror::Empty};
}

std::optional<View> HostHeap::mirror(std::uint32_t index, std::uint64_t handle, bool for_draw) {
    if (index >= count_) {

        static std::atomic<std::uint32_t> highest{0};
        std::uint32_t seen = highest.load(std::memory_order_relaxed);
        while (seen < index && !highest.compare_exchange_weak(seen, index, std::memory_order_relaxed)) {
        }
        if (seen < index) {
            log::warn(TARGET, "title slot past the host heap", {{"index", log::debug(index)}, {"count", log::debug(count_)}, {"for_draw", log::debug(for_draw)}});
        }
        return std::nullopt;
    }
    auto [tag, i, generation] = record_at(handle);
    std::uint64_t key = std::uint64_t{generation} << 32 | i;
    std::lock_guard lock(made_lock_);
    std::optional<View> view;
    if (tag == VIEW_RECORD_TAG) {
        view = view_at_words(i, generation);
    }
    if (!view) {

        if (made_[index] != 0) {
            made_[index] = 0;
            make_null(index);
            note_buffer(index, nullptr, false);
        } else if (for_draw) {
            note_buffer(index, nullptr, false);
        }
        return std::nullopt;
    }
    if (made_[index] != key) {
        for (std::uint32_t stale : note_buffer(index, &*view, for_draw)) {
            if (stale != index) {
                unmake(made_, stale);
            }
        }

        std::optional<std::uintptr_t> waiting;
        if (!for_draw && (view->kind == View::Shader || view->kind == View::Unordered) && host_waits(reinterpret_cast<This>(view->a))) {
            waiting = static_cast<std::uintptr_t>(view->a);
        }
        if (waiting) {
            wait_for(*waiting, handle);
        }
        bool is_buffer = false;
        switch (view->kind) {
            case View::Constant: is_buffer = true; break;
            case View::Shader:
            case View::Unordered: is_buffer = is_buffer_object(view->a); break;


            case View::Placed: is_buffer = true; break;
            default: break;
        }
        bool later = !for_draw && (is_buffer || waiting.has_value());


        if (later && keep_views()) {
            std::uint64_t old = made_[index];
            if (auto old_view = view_at_words(static_cast<std::uint32_t>(old), static_cast<std::uint32_t>(old >> 32))) {
                if (same_placed_texture(*old_view, *view)) {
                    KEPT_VIEWS.fetch_add(1, std::memory_order_relaxed);
                    return view;
                }
            }
        }
        made_[index] = later ? 0 : key;
        std::optional<Kept> kept;
        if (!later) {
            kept = make_view(*view, cpu(index));
        }
        if (!kept && !sampler_) {
            null_for(*view, cpu(index));
        }
        keep(index, kept ? *kept : nullptr);
    }
    return view;
}

std::array<std::uint64_t, 8> slot_counts() {
    return {KEPT_VIEWS.load(std::memory_order_relaxed),    TABLED_WRITES[0].load(std::memory_order_relaxed),
            TABLED_WRITES[1].load(std::memory_order_relaxed), STALE[0].load(std::memory_order_relaxed),
            STALE[1].load(std::memory_order_relaxed),         STALE[2].load(std::memory_order_relaxed),
            STALE[3].load(std::memory_order_relaxed),         STALE[4].load(std::memory_order_relaxed)};
}

}   
