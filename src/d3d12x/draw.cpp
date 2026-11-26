// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/draw.rs (DanziLoader / victory-perf, see
// NOTICE.md). See draw.hpp.
#include "draw.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "core/env.hpp"
#include "core/log.hpp"
#include "descriptor.hpp"
#include "host.hpp"
#include "host_heap.hpp"
#include "indirect.hpp"
#include "list.hpp"
#include "memory.hpp"
#include "pipeline.hpp"
#include "resource.hpp"
#include "shadows.hpp"
#include "texture.hpp"

namespace danzi::d3d12x {

namespace {


alignas(64) std::atomic<std::uint64_t> DRAWN{0};
alignas(64) std::atomic<std::uint64_t> DISPATCHED{0};

}   

std::uint64_t drawn() { return DRAWN.load(std::memory_order_relaxed); }
std::uint64_t dispatched() { return DISPATCHED.load(std::memory_order_relaxed); }

namespace {


constexpr std::uint32_t DEPTH_WRITE = 0x10;
constexpr std::uint32_t DEPTH_READ = 0x20;
constexpr std::uint32_t SHADER_RESOURCE = 0x40 | 0x80;
constexpr std::uint32_t UNORDERED_ACCESS = 0x8;


constexpr std::uint32_t USER_MEMORY_BASE = 0x100;
constexpr std::uint32_t USER_VIEW_ADDRESS = 0x101;
constexpr std::uint32_t USER_VIEW_BIAS = 0x103;
constexpr std::uint32_t USER_GROUP_BASE = 0x104;


constexpr std::uint8_t SRV = 0, CBV = 2, UAV = 3;
constexpr std::uint32_t NATIVE_TABLE = 3;

constexpr std::string_view TARGET = "victory_gpu::draw";


#define DRAW_ONCE(...)                                                                                            \
    do {                                                                                                          \
        static std::atomic<bool> said_{false};                                                                    \
        if (!said_.load(std::memory_order_relaxed) && !said_.exchange(true, std::memory_order_relaxed)) {        \
            log::warn(TARGET, __VA_ARGS__);                                                                       \
        }                                                                                                         \
    } while (false)


bool write_skip() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_WRITE_SKIP", L"0");
    return on;
}

std::uint64_t root_uav_limit() {
    static const std::uint64_t limit = [] {
        wchar_t v[32] = {};
        DWORD n = GetEnvironmentVariableW(L"VICTORY_ROOT_UAV_MB", v, 32);
        return n > 0 && n < 32 ? std::wcstoull(v, nullptr, 10) << 20 : UINT64_MAX;
    }();
    return limit;
}

std::uint64_t root_view_limit() {
    static const std::uint64_t limit = [] {
        wchar_t v[32] = {};
        DWORD n = GetEnvironmentVariableW(L"VICTORY_ROOT_VIEW_MB", v, 32);
        return n > 0 && n < 32 ? std::wcstoull(v, nullptr, 10) << 20 : UINT64_MAX;
    }();
    return limit;
}

std::optional<std::uint8_t> view_kind(const View& view) {
    switch (view.kind) {
        case View::Shader: return SRV;
        case View::Unordered: return UAV;
        case View::Placed: return view.flag ? UAV : SRV;
        default: return std::nullopt;
    }
}

}   


void ListState::readable(HostList& list, std::uint8_t kind, const View& given) {
    auto placed = placed_texture(given);
    const View& view = placed ? *placed : given;


    if (auto copy = buffer_copy(view)) {
        shadow_refresh(list, copy->shadow, copy->address, copy->len);
        if (kind == UAV && (stores_ || !write_skip())) {
            shadow_gpu_writes(list, copy->shadow, copy->address, copy->len);
        }
    }
    This resource = reinterpret_cast<This>(view.a);
    if (view.kind == View::Shader && kind == SRV) {
        bool aliased = false;
        if (This source = alias_of(resource)) {
            aliased = alias(list, resource, source);
        }
        if (!aliased) {
            if (ID3D12Resource* t = host_resource(resource); t && title_written(t)) {
                std::uint32_t first = placed && view.flag ? first_mip(view.words.data(), 10) : 0;
                upload_from(list, resource, t, first);
            }
        }
        list.twin_use(resource, false, false);
        std::uint32_t state = SHADER_RESOURCE;
        if (tested_.first && tested_.first == resource) {
            tested_.second = true;
            state = DEPTH_READ | SHADER_RESOURCE;
        }
        ensure_state(list, resource, state);
    } else if (view.kind == View::Unordered && kind == UAV) {
        if (!list.twin_use(resource, true, true)) {
            ensure_state(list, resource, UNORDERED_ACCESS);
        }
    }
}

namespace {


std::optional<std::uint64_t> root_view(HostList& list, std::uint64_t address, bool srv) {
    auto s = shadow_find(address, 1);
    if (!s) {
        return std::nullopt;
    }
    std::uint64_t len = std::min(shadow_left(*s, address), srv ? root_view_limit() : std::uint64_t{1} << 16);
    std::uint64_t gpu = shadow_gpu(*s, address);
    if (srv || gpu % 256 == 0) {
        shadow_refresh(list, s, address, len);
        return gpu;
    }
    auto room = list.upload(len, 256);
    if (!room) {
        return std::nullopt;
    }
    list.defer_fill(room->data, reinterpret_cast<const std::uint8_t*>(address), static_cast<usize>(len));
    return room->buffer->GetGPUVirtualAddress() + room->offset;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> group_base(const HostPipeline& p) {
    if (!p.compute || p.stages.empty() || !p.stages[0].user_data) {
        return std::nullopt;
    }
    for (const UserData& u : p.stages[0].translated().host.user_data) {
        if (u.root == RootKind::Other && u.other == USER_GROUP_BASE) {
            return std::pair{p.stages[0].user_data->first, u.index};
        }
    }
    return std::nullopt;
}

void keep(HostList& list, IUnknown* object) { list.keep(ComPtr<IUnknown>(object)); }


bool walls_trace() {
    static const bool on = danzi::env::is(L"VICTORY_WALLS_TRACE", L"1") || danzi::env::is(L"VICTORY_WALLS_TRACE", L"2");
    return on;
}


void walls_census(const HostPipeline& p, bool compute, const std::vector<This>& targets, const std::vector<D3D12_RECT>& scissors) {
    static constexpr std::pair<const char*, const char*> CULL[9] = {
        {"55679a0547649d0b", "tree"},   {"89d08e561897d4e5", "tree_imposter"}, {"9209777e8face9dd", "grass"},
        {"17af15c4015cb4b6", "prop"},   {"fdd56d97a806bcca", "prop_nocull"},   {"42f717f1b4d18e94", "simple"},
        {"1c4b3c0547e3e6fa", "slod"},   {"9146291b38ea9ce1", "meshlet_args"},  {"42683eebec081520", "depth_pyramid_cpu_occlusion"},
    };
    struct Census {
        std::uint64_t frame = UINT64_MAX;
        std::map<std::array<std::int32_t, 4>, std::uint32_t> rects;
        std::array<std::uint32_t, 9> counts{};
    };
    static std::mutex lock;
    static Census census;
    std::uint64_t frame = frame_current();
    std::lock_guard g(lock);
    if (census.frame != frame) {
        std::uint64_t draws = 0;
        for (const auto& [_, n] : census.rects) {
            draws += n;
        }
        if (draws > 0) {
            std::vector<std::pair<std::array<std::int32_t, 4>, std::uint32_t>> rects(census.rects.begin(), census.rects.end());
            std::stable_sort(rects.begin(), rects.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::vector<std::string> shown;
            for (usize i = 0; i < rects.size() && i < 5; ++i) {
                const auto& [r, n] = rects[i];
                shown.push_back(std::to_string(r[0]) + " " + std::to_string(r[1]) + " " + std::to_string(r[2]) + " " + std::to_string(r[3]) + " x" +
                                std::to_string(n));
            }
            std::vector<std::string> culling;
            for (usize i = 0; i < 9; ++i) {
                if (census.counts[i] > 0) {
                    culling.push_back(std::string(CULL[i].second) + "=" + std::to_string(census.counts[i]));
                }
            }
            auto list_debug = [](const std::vector<std::string>& v) {
                std::string s = "[";
                for (usize i = 0; i < v.size(); ++i) {
                    s += (i ? ", " : "") + log::debug(v[i]);
                }
                return s + "]";
            };
            log::info(TARGET, "walls trace: the frame's main pass by scissor rectangle, and its culling dispatches",
                      {{"frame", log::debug(census.frame)},
                       {"main_pass_draws", log::debug(draws)},
                       {"rectangles", log::debug(static_cast<std::uint64_t>(rects.size()))},
                       {"scissors", list_debug(shown)},
                       {"culling", list_debug(culling)}});
        }
        census.frame = frame;
        census.rects.clear();
        census.counts = {};
    }
    if (compute) {
        for (usize i = 0; i < 9; ++i) {
            if (std::any_of(p.stages.begin(), p.stages.end(), [&](const Stage& s) { return s.shader->hash == CULL[i].first; })) {
                ++census.counts[i];
            }
        }
    } else if (std::count_if(targets.begin(), targets.end(), [](This t) { return t != nullptr; }) >= 4) {
        std::array<std::int32_t, 4> r{};
        if (!scissors.empty()) {
            r = {scissors[0].left, scissors[0].top, scissors[0].right, scissors[0].bottom};
        }
        ++census.rects[r];
    }
}

}   

ListState& ListState::operator=(ListState&& o) noexcept {
    if (this != &o) {
        give_back();
        this->~ListState();
        new (this) ListState(std::move(o));
        o.dynamic_.clear();
    }
    return *this;
}

ListState::~ListState() { give_back(); }

void ListState::give_back() {
    for (const Dynamic& d : dynamic_) {
        release_dynamic(d.heap, d.first, d.n);
    }
    dynamic_.clear();
}

void ListState::reset(This pipeline) {
    *this = ListState{};
    pipeline_ = pipeline;
}

void ListState::set_heaps(const This* heaps, std::uint32_t count) {
    heaps_ = {};
    for (std::uint32_t i = 0; i < count && heaps; ++i) {
        if (heaps[i]) {
            heaps_[heap_samplers(heap_of_object(heaps[i])) ? 1 : 0] = heaps[i];
        }
    }
}

void ListState::set_table(bool compute, std::uint32_t slot, std::uint64_t handle) {
    Roots& r = roots(compute);
    if (r.size() <= slot) {
        r.resize(slot + 1);
    }
    r[slot] = Root{Root::Table, handle, {}};
}

void ListState::set_view(bool compute, std::uint32_t slot, std::uint64_t address) {
    Roots& r = roots(compute);
    if (r.size() <= slot) {
        r.resize(slot + 1);
    }
    r[slot] = Root{Root::View, address, {}};
}

void ListState::set_constants(bool compute, std::uint32_t slot, const std::uint32_t* values, std::uint32_t count, std::uint32_t offset) {
    Roots& r = roots(compute);
    if (r.size() <= slot) {
        r.resize(slot + 1);
    }
    if (r[slot].kind != Root::Constants) {
        r[slot] = Root{Root::Constants, 0, {}};
    }
    auto& c = r[slot].constants;
    std::size_t end = std::size_t{offset} + count;
    if (c.size() < end) {
        c.resize(end, 0);
    }
    if (count) {
        std::memcpy(c.data() + offset, values, std::size_t{count} * 4);
    }
}

void ListState::tables_done() {
    if (tables_gone_ && dynamic_.empty()) {
        return;
    }
    if (tables_gone_) {
        DRAW_ONCE("a list ran again after its descriptor tables were given back");
    }
    give_back();
    fresh_tables_.clear();
    root_srv_slots_.clear();
    root_uav_slots_.clear();
    tables_gone_ = true;
}

void ListState::replay(HostList& list) {
    host_pipeline_ = nullptr;
    host_roots_ = {};
    host_heaps_ = {};
    if (topology_set_) {
        set_topology(list, topology_);
    }
    if (targets_set_) {
        auto handles = std::move(target_handles_);
        set_targets(list, handles.data(), static_cast<std::uint32_t>(handles.size()), depth_handle_);
    }
    if (!viewports_.empty()) {
        list.list()->RSSetViewports(static_cast<UINT>(viewports_.size()), viewports_.data());
    }
    if (!scissors_.empty()) {
        auto rects = std::move(scissors_);
        set_scissors(list, rects.data(), static_cast<std::uint32_t>(rects.size()));
    }
    if (blend_) {
        list.list()->OMSetBlendFactor(blend_->data());
    }
    if (stencil_) {
        list.list()->OMSetStencilRef(*stencil_);
    }
    if (depth_bounds_) {
        set_depth_bounds(list, (*depth_bounds_)[0], (*depth_bounds_)[1]);
    }
}


namespace {

constexpr std::uint32_t TRIANGLE_FAN = 6, RECTANGLES = 17, LINE_LOOP = 18, QUADS = 19, QUAD_STRIP = 20;

bool scissor_test() {
    static const bool on = danzi::env::is(L"VICTORY_SCISSOR_TEST", L"full");
    return on;
}


std::uint32_t depth_state(ID3D12Resource* texture, const std::optional<std::array<std::uint32_t, 6>>& words) {
    if (!words) {
        return DEPTH_WRITE;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC desc;
    std::memcpy(&desc, words->data(), sizeof desc);
    auto format = desc.Format == DXGI_FORMAT_UNKNOWN ? texture->GetDesc().Format : desc.Format;
    UINT needed = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    if (format != 55 && format != 40) {
        needed |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    }
    return (desc.Flags & needed) == needed ? DEPTH_READ : DEPTH_WRITE;
}


enum class Kind { F32, F16, Unorm8, Unorm16 };

std::optional<std::pair<std::size_t, Kind>> components(std::uint32_t format) {
    switch (format) {
        case 2: return std::pair{std::size_t{4}, Kind::F32};
        case 6: return std::pair{std::size_t{3}, Kind::F32};
        case 16: return std::pair{std::size_t{2}, Kind::F32};
        case 41: return std::pair{std::size_t{1}, Kind::F32};
        case 10: return std::pair{std::size_t{4}, Kind::F16};
        case 34: return std::pair{std::size_t{2}, Kind::F16};
        case 54: return std::pair{std::size_t{1}, Kind::F16};
        case 11: return std::pair{std::size_t{4}, Kind::Unorm16};
        case 35: return std::pair{std::size_t{2}, Kind::Unorm16};
        case 28:
        case 29:
        case 87:
        case 91: return std::pair{std::size_t{4}, Kind::Unorm8};
        default: return std::nullopt;
    }
}

std::size_t kind_bytes(Kind k) { return k == Kind::F32 ? 4 : k == Kind::Unorm8 ? 1 : 2; }

float half_to_f32(std::uint16_t h) {
    std::uint32_t sign = std::uint32_t(h >> 15) << 31, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    std::uint32_t bits;
    if (exp == 0 && man == 0) {
        bits = sign;
    } else if (exp == 0) {
        float v = static_cast<float>(man) * std::ldexp(1.0f, -24);
        return sign ? -v : v;
    } else if (exp == 31) {
        bits = sign | 0x7f800000 | man << 13;
    } else {
        bits = sign | (exp + 112) << 23 | man << 13;
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::uint16_t f32_to_half(float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000);
    std::int32_t exp = static_cast<std::int32_t>((bits >> 23) & 0xff) - 127 + 15;
    std::uint32_t man = bits & 0x7fffff;
    if (((bits >> 23) & 0xff) == 0xff) {
        return static_cast<std::uint16_t>(sign | 0x7c00 | (man ? 0x200 : 0));
    }
    if (exp >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7c00);
    }
    if (exp <= 0) {
        if (exp < -10) {
            return sign;
        }
        man |= 0x800000;
        std::uint32_t shift = static_cast<std::uint32_t>(14 - exp);
        std::uint32_t half = man >> shift;
        std::uint32_t rest = man & ((1u << shift) - 1);
        std::uint32_t mid = 1u << (shift - 1);
        if (rest > mid || (rest == mid && (half & 1))) {
            ++half;
        }
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = static_cast<std::uint32_t>(exp) << 10 | man >> 13;
    std::uint32_t rest = man & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) {
        ++half;
    }
    return static_cast<std::uint16_t>(sign | half);
}

float kind_read(Kind k, const std::uint8_t* at, std::size_t c) {
    const std::uint8_t* b = at + c * kind_bytes(k);
    switch (k) {
        case Kind::F32: {
            float f;
            std::memcpy(&f, b, 4);
            return f;
        }
        case Kind::F16: return half_to_f32(static_cast<std::uint16_t>(b[0] | b[1] << 8));
        case Kind::Unorm16: return static_cast<float>(b[0] | b[1] << 8) / 65535.0f;
        case Kind::Unorm8: return static_cast<float>(b[0]) / 255.0f;
    }
    return 0;
}

void kind_write(Kind k, std::uint8_t* at, std::size_t c, float v) {
    std::uint8_t* b = at + c * kind_bytes(k);
    switch (k) {
        case Kind::F32: std::memcpy(b, &v, 4); break;
        case Kind::F16: {
            std::uint16_t h = f32_to_half(v);
            std::memcpy(b, &h, 2);
            break;
        }
        case Kind::Unorm16: {
            auto u = static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
            std::memcpy(b, &u, 2);
            break;
        }
        case Kind::Unorm8: b[0] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); break;
    }
}


void expand_rects(const std::uint8_t* src, std::uint8_t* dst, std::size_t n, std::size_t stride, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& elements) {
    for (std::size_t r = 0; r < n; ++r) {
        const std::uint8_t* rect = src + r * 3 * stride;
        std::uint8_t* out = dst + r * 6 * stride;
        auto v = [&](std::size_t i) { return rect + i * stride; };
        const std::size_t order[5] = {0, 1, 2, 0, 2};
        for (std::size_t slot = 0; slot < 5; ++slot) {
            std::memcpy(out + slot * stride, v(order[slot]), stride);
        }
        std::uint8_t* fourth = out + 5 * stride;
        std::memcpy(fourth, v(0), stride);
        for (const auto& [offset, format] : elements) {
            auto c = components(format);
            if (!c) {
                continue;
            }
            std::size_t at = offset;
            std::size_t inside = stride > at ? (stride - at) / kind_bytes(c->second) : 0;
            for (std::size_t k = 0; k < std::min(c->first, inside); ++k) {
                float value = kind_read(c->second, v(0) + at, k) + kind_read(c->second, v(2) + at, k) - kind_read(c->second, v(1) + at, k);
                kind_write(c->second, fourth + at, k, value);
            }
        }
    }
}

}   

void ListState::set_topology(HostList& list, std::uint32_t topology) {
    topology_ = topology;
    topology_set_ = true;
    std::uint32_t host_topology = topology;
    if (topology == RECTANGLES || topology == TRIANGLE_FAN || topology == LINE_LOOP || topology == QUADS || topology == QUAD_STRIP) {
        host_topology = 4;   
        if (topology != RECTANGLES) {
            DRAW_ONCE("console topology drawn as a triangle list", {{"topology", log::debug(topology)}});
        }
    }
    list.list()->IASetPrimitiveTopology(static_cast<D3D_PRIMITIVE_TOPOLOGY>(host_topology));
}

void ListState::set_vertex_buffers(std::uint32_t start, const VertexView* views, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count && start + i < 32; ++i) {
        vertex_[start + i] = views[i];
    }
}

void ListState::clear_vertex_buffers(std::uint32_t start, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count && start + i < 32; ++i) {
        vertex_[start + i] = {};
    }
}

void ListState::set_index_buffer(const IndexView* view) { index_ = view ? *view : IndexView{}; }

void ListState::set_targets(HostList& list, const std::uint64_t* handles, std::uint32_t count, std::uint64_t depth) {
    Host* h = host();
    if (!h) {
        return;
    }
    target_handles_.assign(handles, handles + count);
    depth_handle_ = depth;
    targets_set_ = true;
    targets_.clear();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
    UINT n = 0;
    for (std::uint32_t i = 0; i < count && n < 8; ++i) {
        auto record = rtv_record(handles[i]);
        if (!record) {
            DRAW_ONCE("render target without a recorded view", {{"handle", log::hex(handles[i])}});
            continue;
        }
        auto words = record->host_desc(false);
        auto copy = host_copy(record->resource);
        if (!copy) {

            if (words && (*words)[0] != 0 && (*words)[1] != 0) {
                rtvs[n++] = h->rtv(nullptr, reinterpret_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(words->data()));
            }
            continue;
        }
        targets_.push_back(record->resource);


        rtvs[n++] = h->view_of(*copy, false, words ? &*words : nullptr);
    }
    depth_ = nullptr;
    depth_state_ = DEPTH_WRITE;
    std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> dsv;
    if (auto record = dsv_record(depth)) {
        if (auto copy = host_copy(record->resource)) {
            auto words = record->host_desc(true);
            depth_ = record->resource;
            depth_state_ = depth_state(copy->texture.Get(), words);
            dsv = h->view_of(*copy, true, words ? &*words : nullptr);
        }
    }
    list.list()->OMSetRenderTargets(n, n ? rtvs : nullptr, FALSE, dsv ? &*dsv : nullptr);
}

void ListState::set_viewports(HostList& list, const D3D12_VIEWPORT* viewports, std::uint32_t count) {
    viewports_.assign(viewports, viewports + count);
    list.list()->RSSetViewports(count, viewports);
}

void ListState::set_scissors(HostList& list, const D3D12_RECT* rects, std::uint32_t count) {
    scissors_.assign(rects, rects + count);
    if (scissor_test()) {
        std::vector<D3D12_RECT> full(count, D3D12_RECT{0, 0, 16384, 16384});
        list.list()->RSSetScissorRects(count, full.data());
        return;
    }
    list.list()->RSSetScissorRects(count, rects);
}

void ListState::set_blend_factor(HostList& list, const float* factor) {
    if (factor) {
        blend_ = std::array<float, 4>{factor[0], factor[1], factor[2], factor[3]};
    } else {
        blend_.reset();
    }
    list.list()->OMSetBlendFactor(factor);
}

void ListState::set_stencil_ref(HostList& list, std::uint32_t reference) {
    stencil_ = reference;
    list.list()->OMSetStencilRef(reference);
}

void ListState::set_depth_bounds(HostList& list, float min, float max) {
    depth_bounds_ = std::array<float, 2>{min, max};
    if (host_depth_bounds()) {
        ComPtr<ID3D12GraphicsCommandList1> l1;
        if (SUCCEEDED(list.list()->QueryInterface(IID_PPV_ARGS(&l1)))) {
            l1->OMSetDepthBounds(min, max);
        }
    }
}

bool ListState::prepare(HostList& list, bool indexed, bool mesh) {
    const HostPipeline* p = host_pipeline(pipeline_);
    if (!p) {
        return false;
    }
    if (p->compute || p->mesh != mesh) {
        DRAW_ONCE("a draw or mesh dispatch with another kind of pipeline", {{"mesh", log::debug(mesh)}});
        return false;
    }
    if (p->rects && topology_ != RECTANGLES) {
        DRAW_ONCE("a rectangle pipeline drawn with another topology");
    }
    if (!bind(list, *p, false)) {
        return false;
    }
    ID3D12GraphicsCommandList* l = list.list();
    D3D12_VERTEX_BUFFER_VIEW views[32] = {};
    UINT count = 0;
    for (std::uint32_t slot = 0; slot < 32; ++slot) {
        if ((p->slots & 1u << slot) == 0) {
            continue;
        }
        count = slot + 1;
        const VertexView& v = vertex_[slot];
        if (v.address == 0 || v.size == 0) {
            continue;
        }
        auto s = shadow_find_any(v.address, v.size);
        if (!s) {
            continue;
        }
        shadow_refresh(list, s, v.address, v.size);
        views[slot] = {shadow_gpu(*s, v.address), v.size, v.stride};
    }
    if (count > 0) {
        l->IASetVertexBuffers(0, count, views);
    }
    if (indexed) {
        auto s = shadow_find_any(index_.address, index_.size);
        if (!s) {
            return false;
        }
        shadow_refresh(list, s, index_.address, index_.size);
        D3D12_INDEX_BUFFER_VIEW view{shadow_gpu(*s, index_.address), index_.size, static_cast<DXGI_FORMAT>(index_.format)};
        l->IASetIndexBuffer(&view);
    }
    constexpr std::uint32_t RENDER_TARGET = 0x4;
    for (This t : targets_) {
        ensure_state(list, t, RENDER_TARGET);
    }
    if (depth_) {
        list.twin_use(depth_, false, true);
        std::uint32_t state = DEPTH_WRITE;
        if (depth_state_ == DEPTH_READ) {
            state = tested_.second && tested_.first == depth_ ? DEPTH_READ | SHADER_RESOURCE : DEPTH_READ;
        }
        ensure_state(list, depth_, state);
    }
    return true;
}

bool ListState::rectangles(HostList& list, const HostPipeline& p, std::uint32_t count, std::uint32_t first) {
    std::uint64_t rects = count / 3;
    for (std::uint32_t slot = 0; slot < 32; ++slot) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> elements;
        for (const Input& e : p.inputs) {
            if (e.slot == slot && !e.per_instance) {
                elements.emplace_back(e.offset, e.format);
            }
        }
        if (elements.empty()) {
            continue;
        }
        const VertexView& v = vertex_[slot];
        std::uint64_t stride = v.stride;
        std::uint64_t from = v.address + std::uint64_t{first} * stride;
        if (v.address == 0 || stride == 0 || std::uint64_t{first + count} * stride > v.size || !shadow_find(from, rects * 3 * stride)) {
            DRAW_ONCE("rectangle vertices outside their buffer", {{"slot", log::debug(slot)}});
            return false;
        }
        auto room = list.upload(rects * 6 * stride, 16);
        if (!room) {
            return false;
        }
        std::uint8_t* to = room->data;
        std::size_t n = static_cast<std::size_t>(rects), st = static_cast<std::size_t>(stride);
        list.defer_host([from, to, n, st, elements = std::move(elements)] {
            expand_rects(reinterpret_cast<const std::uint8_t*>(from), to, n, st, elements);
        });
        D3D12_VERTEX_BUFFER_VIEW view{room->buffer->GetGPUVirtualAddress() + room->offset, static_cast<UINT>(rects * 6 * stride), v.stride};
        list.list()->IASetVertexBuffers(slot, 1, &view);
    }
    return true;
}

void ListState::draw(HostList& list, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first, std::uint32_t first_instance) {
    end_window(list);
    bool ready = prepare(list, false);
    const HostPipeline* p = ready ? host_pipeline(pipeline_) : nullptr;
    bool rects = topology_ == RECTANGLES && !(p && p->rects);
    bool ran = ready && (!rects || rectangles(list, *p, vertices, first));
    if (!ran) {
        return;
    }
    std::uint32_t count = rects ? vertices / 3 * 6 : vertices;
    std::uint32_t from = rects ? 0 : first;
    list.list()->DrawInstanced(count, instances, from, first_instance);
    if (std::any_of(p->stages.begin(), p->stages.end(), [](const Stage& s) { return s.translated().writes; })) {
        list.set_uav_pending(true);
    }
    DRAWN.fetch_add(1, std::memory_order_relaxed);
}

namespace {


std::uint8_t mesh_test() {
    static const std::uint8_t on = [] {
        auto v = danzi::env::get(L"VICTORY_MESH_TEST");
        return !v ? std::uint8_t{0} : *v == L"skip" ? std::uint8_t{1} : *v == L"scissor" ? std::uint8_t{2} : std::uint8_t{0};
    }();
    return on;
}


bool mesh_kept(This pipeline) {
    struct Keep {
        bool on = false;
        std::string last, before;
    };
    static const Keep keep = [] {
        Keep k;
        auto v = danzi::env::get(L"VICTORY_MESH_TEST");
        if (!v || v->rfind(L"keep:", 0) != 0) {
            return k;
        }
        std::string rest(v->begin() + 5, v->end());
        auto colon = rest.find(':');
        k.last = rest.substr(0, colon);
        if (colon != std::string::npos) {
            k.before = rest.substr(colon + 1);
            k.before = k.before.substr(0, k.before.find(':'));
        }
        k.on = true;
        return k;
    }();
    if (!keep.on) {
        return true;
    }
    const HostPipeline* p = host_pipeline(pipeline);
    if (!p) {
        return true;
    }
    auto at = p->name.find("mesh ");
    if (at == std::string::npos || p->name.size() < at + 5 + 16) {
        return true;
    }
    const char* hash = p->name.data() + at + 5;
    return keep.last.find(hash[15]) != std::string::npos && (keep.before.empty() || keep.before.find(hash[14]) != std::string::npos);
}

}   

void ListState::mesh_scissor(HostList& list, bool on) {
    if (mesh_test() != 2) {
        return;
    }
    if (on) {
        std::vector<D3D12_RECT> one(std::max<std::size_t>(scissors_.size(), 1), D3D12_RECT{0, 0, 1, 1});
        list.list()->RSSetScissorRects(static_cast<UINT>(one.size()), one.data());
    } else if (!scissors_.empty()) {
        list.list()->RSSetScissorRects(static_cast<UINT>(scissors_.size()), scissors_.data());
    }
}

void ListState::dispatch_mesh(HostList& list, std::uint32_t x) {
    end_window(list);
    if (mesh_test() == 1 || !mesh_kept(pipeline_) || !prepare(list, false, true)) {
        return;
    }
    ID3D12GraphicsCommandList6* l6 = list.list6();
    if (!l6) {
        return;
    }
    mesh_scissor(list, true);
    l6->DispatchMesh(x, 1, 1);
    const HostPipeline* p = host_pipeline(pipeline_);
    if (std::any_of(p->stages.begin(), p->stages.end(), [](const Stage& s) { return s.translated().writes; })) {
        list.set_uav_pending(true);
    }
    mesh_scissor(list, false);
    DRAWN.fetch_add(1, std::memory_order_relaxed);
}

void ListState::draw_indexed(HostList& list, std::uint32_t indices, std::uint32_t instances, std::uint32_t first, std::int32_t base,
                             std::uint32_t first_instance) {
    end_window(list);
    const HostPipeline* p = host_pipeline(pipeline_);
    bool lost = topology_ == RECTANGLES && !(p && p->rects);
    if (lost) {
        DRAW_ONCE("indexed rectangles not drawn");
    }
    if (lost || !prepare(list, true)) {
        return;
    }
    list.list()->DrawIndexedInstanced(indices, instances, first, base, first_instance);
    if (std::any_of(p->stages.begin(), p->stages.end(), [](const Stage& s) { return s.translated().writes; })) {
        list.set_uav_pending(true);
    }
    DRAWN.fetch_add(1, std::memory_order_relaxed);
}

DescHeap* ListState::heap_of(std::uint64_t handle) const {
    for (This h : heaps_) {
        if (h && heap_index(heap_of_object(h), handle)) {
            return &heap_of_object(h);
        }
    }
    return nullptr;
}

std::uint32_t ListState::user_value(bool compute, std::uint8_t slot, std::uint8_t root_kind, std::uint32_t dword) const {
    const Roots& r = roots(compute);
    const Root* root = slot < r.size() ? &r[slot] : nullptr;
    auto kind = static_cast<RootKind>(root_kind);
    if (root && root->kind == Root::Table && kind == RootKind::Table) {
        if (DescHeap* heap = heap_of(root->value)) {
            return static_cast<std::uint32_t>(root->value - heap_start(*heap));
        }
        DRAW_ONCE("descriptor table outside the bound heaps", {{"handle", log::hex(root->value)}});
        return 0;
    }
    if (root && root->kind == Root::Constants && kind == RootKind::Constants) {
        return dword < root->constants.size() ? root->constants[dword] : 0;
    }
    if (kind != RootKind::Other) {
        static constexpr const char* NAMES[] = {"Constants", "Descriptor", "Table", "ImmediateSampler", "ResourceHeap", "SamplerHeap"};
        DRAW_ONCE("a shader reads a root argument the title did not set", {{"slot", log::debug(int{slot})}, {"root", NAMES[root_kind]}});
    }
    return 0;
}

std::uint32_t ListState::host_word(bool compute, std::uint32_t kind, std::uint8_t slot, std::uint32_t dword, const MemoryReady* memory) const {

    if (kind == USER_GROUP_BASE) {
        return 0;
    }
    if (kind == USER_MEMORY_BASE) {
        return memory ? memory->base : 0;
    }
    const Roots& r = roots(compute);
    const Root* root = slot < r.size() ? &r[slot] : nullptr;
    if (kind == USER_VIEW_BIAS) {
        if (root && root->kind == Root::View) {


            const shadow::Shadow* s = nullptr;
            if (heaps_[0]) {
                SlotKey key{reinterpret_cast<std::uintptr_t>(&heap_of_object(heaps_[0])), root->value};
                if (auto it = root_uav_slots_.find(key); it != root_uav_slots_.end()) {
                    s = it->second.shadow.get();
                } else if (auto it2 = root_srv_slots_.find(SlotKey{key.heap, root->value | std::uint64_t{1} << 63}); it2 != root_srv_slots_.end()) {
                    s = it2->second.shadow.get();
                }
            }
            if (s) {
                return static_cast<std::uint32_t>(shadow_offset(*s, root->value) & 15);
            }
            if (auto found = shadow_find(root->value, 1)) {
                return static_cast<std::uint32_t>(shadow_offset(*found, root->value) & 15);
            }
        }
        return 0;
    }
    if (kind != USER_VIEW_ADDRESS && kind != USER_VIEW_ADDRESS + 1) {
        DRAW_ONCE("a shader reads a host value this build does not know", {{"kind", log::debug(kind)}});
        return 0;
    }
    std::uint64_t address = 0;
    if (root && root->kind == Root::Table) {
        if (auto v = view(root->value + std::uint64_t{dword} * 4)) {
            if (auto range = buffer_range(*v)) {
                address = range->first;
            }
        }
    }
    if (address == 0) {
        DRAW_ONCE("a shader takes the address of a buffer view that is not there: it reads 0",
                  {{"slot", log::debug(int{slot})}, {"dword", log::debug(dword)}});
    }
    return kind == USER_VIEW_ADDRESS ? static_cast<std::uint32_t>(address) : static_cast<std::uint32_t>(address >> 32);
}

void ListState::table_entry(HostList& list, std::uint8_t slot, std::uint32_t table_offset_dwords, std::uint8_t kind, bool compute) {
    const Roots& r = roots(compute);
    if (slot >= r.size() || r[slot].kind != Root::Table) {
        return;
    }
    std::uint64_t handle = r[slot].value + std::uint64_t{table_offset_dwords} * 4;
    DescHeap* heap = heap_of(handle);
    if (!heap) {
        return;
    }
    HostHeap* host = heap_host(*heap);
    auto index = heap_index(*heap, handle);
    if (!host || !index) {
        return;
    }
    if (auto v = host->mirror(*index, handle, true)) {
        readable(list, kind, *v);
        if (is_texture_view(*v)) {
            host->texture_slot_tabled(*index, true);
        }
    }
}

void ListState::tables(HostList& list, const void* translated, bool compute, Made& made) {
    const auto& t = *static_cast<const Translated*>(translated);
    SmallVec<const shader::UsedResource*, 32> used;
    for (const auto& u : t.used) {
        if (u.root == NATIVE_TABLE) {
            used.push_back(&u);
        }
    }
    if (used.empty()) {
        return;
    }
    const Roots& r = roots(compute);


    SmallVec<std::uint8_t, 32> fresh;
    for (const auto* u : used) {
        if (u->kind == CBV) {
            fresh.push_back(u->slot);
        }
    }
    for (const auto* u : used) {
        if (std::find(fresh.begin(), fresh.end(), u->slot) != fresh.end()) {
            continue;
        }
        if (u->slot >= r.size() || r[u->slot].kind != Root::Table) {
            continue;
        }
        std::uint64_t table = r[u->slot].value;
        std::uint64_t end = table + std::uint64_t{u->table_offset_dwords} * 4;
        DescHeap* heap = heap_of(table);
        if (!heap) {
            continue;
        }
        HostHeap* host = heap_host(*heap);
        auto index = heap_index(*heap, end);
        if (!host || !index) {
            continue;
        }
        if (*index >= host->count()) {
            fresh.push_back(u->slot);
        }
    }
    std::sort(fresh.begin(), fresh.end());
    fresh.erase_from(std::unique(fresh.begin(), fresh.end()));
    for (const auto* u : used) {
        if (std::find(fresh.begin(), fresh.end(), u->slot) == fresh.end()) {
            table_entry(list, u->slot, u->table_offset_dwords, u->kind, compute);
        }
    }
    for (std::uint8_t slot : fresh) {
        if (slot >= r.size() || r[slot].kind != Root::Table) {
            continue;
        }
        std::uint64_t table = r[slot].value;
        SmallVec<const shader::UsedResource*, 32> entries;
        for (const auto* u : used) {
            if (u->slot == slot) {
                entries.push_back(u);
            }
        }
        DescHeap* heap = heap_of(table);
        if (!heap || heap_samplers(*heap) || (table - heap_start(*heap)) % 32 != 0) {
            continue;
        }
        HostHeap* host = heap_host(*heap);
        if (!host) {
            continue;
        }
        SmallVec<std::uint32_t, 32> selected;
        for (const RootIndexedView& index : t.host.root_indexed_views) {
            if (index.table_slot == slot) {
                std::uint32_t constant = user_value(compute, index.constant_slot, static_cast<std::uint8_t>(RootKind::Constants), index.constant_dword);
                selected.push_back((index.byte_offset + (constant << index.shift)) / 32);
            }
        }
        std::sort(selected.begin(), selected.end());
        selected.erase_from(std::unique(selected.begin(), selected.end()));
        std::uint32_t n = 0;
        for (const auto* u : entries) {
            n = std::max(n, u->table_offset_dwords / 8);
        }
        if (!selected.empty()) {
            n = std::max(n, selected.back());
        }
        n += 1;
        std::uint64_t last = table + std::uint64_t{n - 1} * 32;
        if (n > DYNAMIC || last < table || !heap_index(*heap, last)) {
            DRAW_ONCE("fresh descriptor table exceeds the available span", {{"slot", log::debug(slot)}, {"n", log::debug(n)}});
            continue;
        }

        std::uint64_t hash = 1469598103934665603ull;
        for (std::uint32_t k = 0; k < n; ++k) {
            std::uint32_t w[3];
            std::memcpy(w, reinterpret_cast<const void*>(table + std::uint64_t{k} * 32), sizeof w);
            for (std::uint32_t x : w) {
                hash = (hash ^ x) * 1099511628211ull;
            }
        }
        TableKey key{table, n, hash};
        if (auto it = fresh_tables_.find(key); it != fresh_tables_.end()) {
            for (const auto* u : entries) {
                if (u->kind != CBV) {
                    if (auto v = view(table + std::uint64_t{u->table_offset_dwords} * 4)) {
                        readable(list, u->kind, *v);
                    }
                }
            }
            for (std::uint32_t k : selected) {
                bool named = std::any_of(entries.begin(), entries.end(), [&](const auto* u) { return u->table_offset_dwords / 8 == k; });
                if (named) {
                    continue;
                }
                if (auto v = view(table + std::uint64_t{k} * 32)) {
                    if (auto kind = view_kind(*v)) {
                        readable(list, *kind, *v);
                    }
                }
            }
            made.emplace_back(slot, it->second * 32);
            continue;
        }
        auto first = host->dynamic(n);
        if (!first) {
            made.emplace_back(slot, UINT32_MAX);
            continue;
        }
        fresh_tables_.emplace(key, *first);
        dynamic_.push_back({reinterpret_cast<std::uintptr_t>(heap), *first, n});
        auto title_first = heap_index(*heap, table);
        if (title_first && *title_first + n > host->count()) {
            title_first.reset();
        }
        for (std::uint32_t k = 0; k < n; ++k) {
            std::uint32_t index = *first + k;
            const shader::UsedResource* u = nullptr;
            for (const auto* e : entries) {
                if (e->table_offset_dwords / 8 == k) {
                    u = e;
                    break;
                }
            }
            auto v = view(table + std::uint64_t{k} * 32);
            if (v && v->kind == View::Constant) {
                auto room = list.upload(v->b, 256);
                if (!room) {
                    host->make_null(index);
                    continue;
                }
                list.defer_fill(room->data, reinterpret_cast<const std::uint8_t*>(v->a), static_cast<usize>(v->b));
                host->constant_at(index, room->buffer->GetGPUVirtualAddress() + room->offset, static_cast<std::uint32_t>(v->b));
            } else if (v) {
                host->make_at(index, *v);
                std::optional<std::uint8_t> kind;
                if (u) {
                    kind = u->kind;
                } else if (std::binary_search(selected.begin(), selected.end(), k)) {
                    kind = view_kind(*v);
                }
                if (kind) {
                    readable(list, *kind, *v);
                }
                if (title_first && is_texture_view(*v)) {
                    host->texture_slot_tabled(*title_first + k, kind.has_value());
                    if (kind) {
                        host->mirror(*title_first + k, table + std::uint64_t{k} * 32, true);
                    }
                }
            } else {
                host->make_null(index);
                if (u) {
                    DRAW_ONCE("a table entry without a view", {{"slot", log::debug(slot)}});
                }
            }
        }
        made.emplace_back(slot, *first * 32);
    }
}

bool ListState::bind(HostList& list, const HostPipeline& p, bool compute) {
    ID3D12GraphicsCommandList* l = list.list();

    early_textures(list, p, compute);
    if (walls_trace()) {
        walls_census(p, compute, targets_, scissors_);
    }


    if (host_heaps_ != heaps_) {
        ID3D12DescriptorHeap* heaps[2];
        UINT n = 0;
        for (This h : heaps_) {
            if (h) {
                if (HostHeap* host = heap_host(heap_of_object(h))) {
                    heaps[n++] = host->heap();
                    keep(list, host->heap());
                }
            }
        }
        l->SetDescriptorHeaps(n, heaps);
        host_heaps_ = heaps_;
        host_roots_ = {};
    }


    tested_ = {!compute && depth_state_ == DEPTH_READ ? depth_ : nullptr, false};
    const void* root = p.root.Get();
    if (host_roots_[compute ? 1 : 0] != root) {
        if (compute) {
            l->SetComputeRootSignature(p.root.Get());
        } else {
            l->SetGraphicsRootSignature(p.root.Get());
        }
        host_roots_[compute ? 1 : 0] = root;
    }
    if (host_pipeline_ != p.pso.Get()) {
        l->SetPipelineState(p.pso.Get());
        host_pipeline_ = p.pso.Get();


        if (kept_list_ != &list || kept_epoch_ != list.ranges_epoch() || kept_pso_ != p.pso.Get()) {
            keep(list, p.pso.Get());
            keep(list, p.root.Get());
            kept_list_ = &list;
            kept_epoch_ = list.ranges_epoch();
            kept_pso_ = p.pso.Get();
        }
    }
    for (const Stage& stage : p.stages) {
        const Translated& t = stage.translated();
        stores_ = t.stores;
        Made made;
        tables(list, &t, compute, made);
        stores_ = true;
        if (std::any_of(made.begin(), made.end(), [](const auto& m) { return m.second == UINT32_MAX; })) {
            DRAW_ONCE("a draw whose descriptor table found no free slots is dropped");
            return false;
        }


        bool writes = (t.stores || !write_skip()) && !builder_quarantined(pipeline_);

        std::optional<MemoryReady> memory;
        if (std::find(t.host.root_srvs.begin(), t.host.root_srvs.end(), SLOT_MEMORY) != t.host.root_srvs.end()) {
            memory = memory_ready_for(list, compute, writes);
            if (!memory) {
                DRAW_ONCE("a shader that reads title memory by address without a view heap to hold its views: the draw is dropped");
                return false;
            }
        }
        if (stage.user_data) {
            auto [param, dwords] = *stage.user_data;


            std::uint32_t few[64];
            std::vector<std::uint32_t> large;
            std::uint32_t* values = few;
            if (dwords > 64) {
                large.assign(dwords, 0);
                values = large.data();
            } else {
                std::fill_n(few, dwords, 0u);
            }
            for (const UserData& u : t.host.user_data) {
                if (u.index >= dwords) {
                    continue;
                }
                if (u.root == RootKind::Other) {
                    values[u.index] = host_word(compute, u.other, u.slot, u.dword, memory ? &*memory : nullptr);
                    continue;
                }
                std::optional<std::uint32_t> fresh;
                if (u.root == RootKind::Table) {
                    for (const auto& m : made) {
                        if (m.first == u.slot) {
                            fresh = m.second;
                            break;
                        }
                    }
                }
                values[u.index] = fresh ? *fresh : user_value(compute, u.slot, static_cast<std::uint8_t>(u.root), u.dword);
            }
            if (compute) {
                l->SetComputeRoot32BitConstants(param, dwords, values, 0);
            } else {
                l->SetGraphicsRoot32BitConstants(param, dwords, values, 0);
            }
        }
        const Roots& r = roots(compute);
        for (int which = 0; which < 2; ++which) {
            bool srv = which == 1;
            for (const auto& [slot, param] : srv ? stage.root_srvs : stage.root_cbvs) {
                if (srv && slot == SLOT_MEMORY) {

                    if (memory) {
                        if (compute) {
                            l->SetComputeRootShaderResourceView(param, memory->directory);
                        } else {
                            l->SetGraphicsRootShaderResourceView(param, memory->directory);
                        }
                    }
                    continue;
                }
                if (srv && slot == SLOT_STRIDES) {


                    if (auto gpu = heap_buffers(list)) {
                        if (compute) {
                            l->SetComputeRootShaderResourceView(param, *gpu);
                        } else {
                            l->SetGraphicsRootShaderResourceView(param, *gpu);
                        }
                    } else {
                        DRAW_ONCE("a shader picks buffers from a view heap that is not set");
                    }
                    continue;
                }
                bool tabled = srv && bounded_root_srvs();
                const Root* root_arg = slot < r.size() ? &r[slot] : nullptr;
                if (!root_arg || root_arg->kind != Root::View) {
                    DRAW_ONCE("a shader reads a root view the title did not set", {{"slot", log::debug(slot)}});
                    if (tabled && !root_srv_table(list, std::nullopt, param, compute, false)) {
                        return false;
                    }
                    continue;
                }
                std::uint64_t address = root_arg->value;
                bool biased = false;
                if (srv) {
                    for (const UserData& u : t.host.user_data) {
                        if (u.slot == slot && u.root == RootKind::Other && u.other == USER_VIEW_BIAS) {
                            biased = true;
                        }
                    }
                }
                if (tabled) {
                    if (!root_srv_table(list, address, param, compute, biased)) {
                        return false;
                    }
                    continue;
                }
                auto gpu = root_view(list, address, srv);
                if (!gpu) {
                    continue;
                }
                if (biased) {
                    *gpu -= host_word(compute, USER_VIEW_BIAS, slot, 0);
                }
                if (compute) {
                    srv ? l->SetComputeRootShaderResourceView(param, *gpu) : l->SetComputeRootConstantBufferView(param, *gpu);
                } else {
                    srv ? l->SetGraphicsRootShaderResourceView(param, *gpu) : l->SetGraphicsRootConstantBufferView(param, *gpu);
                }
            }
        }
        for (const auto& [slot, param] : stage.root_uavs) {
            if (slot == SLOT_FAULTS) {

                if (memory) {
                    if (compute) {
                        l->SetComputeRootUnorderedAccessView(param, memory->faults);
                    } else {
                        l->SetGraphicsRootUnorderedAccessView(param, memory->faults);
                    }
                }
                continue;
            }
            std::optional<std::uint64_t> address;
            if (slot < r.size() && r[slot].kind == Root::View) {
                address = r[slot].value;
            } else {
                DRAW_ONCE("a shader writes through a root view the title did not set", {{"slot", log::debug(slot)}});
            }
            if (!root_uav(list, address, param, compute, writes)) {
                return false;
            }
        }
    }


    list.twins_written();
    return true;
}

bool ListState::root_uav(HostList& list, std::optional<std::uint64_t> address, std::uint32_t param, bool compute, bool writes) {
    std::uint64_t limit = root_uav_limit();
    auto find = [](std::uint64_t a) -> std::shared_ptr<shadow::Shadow> {
        auto s = shadow_find(a, 1);
        if (!s) {
            return nullptr;
        }
        if ((s->buffer()->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
            DRAW_ONCE("a root UAV of a buffer whose host copy shaders cannot write");
            return nullptr;
        }
        return s;
    };
    auto ready = [&](std::uint64_t a, const std::shared_ptr<shadow::Shadow>& s) {
        std::uint64_t len = std::min(shadow_left(*s, a), limit);
        shadow_refresh(list, s, a, len);
        if (writes) {
            shadow_gpu_writes(list, s, a, len);
        }
    };
    ID3D12GraphicsCommandList* l = list.list();
    if (!bounded_root_srvs()) {
        std::shared_ptr<shadow::Shadow> s = address ? find(*address) : nullptr;
        if (!s) {
            DRAW_ONCE("a root UAV with nothing to bind: the draw is dropped");
            return false;
        }
        ready(*address, s);
        std::uint64_t base = s->buffer()->GetGPUVirtualAddress();
        std::uint64_t gpu = base + ((shadow_gpu(*s, *address) - base) & ~std::uint64_t{15});
        compute ? l->SetComputeRootUnorderedAccessView(param, gpu) : l->SetGraphicsRootUnorderedAccessView(param, gpu);
        return true;
    }
    HostHeap* host = heaps_[0] ? heap_host(heap_of_object(heaps_[0])) : nullptr;
    if (!host) {
        DRAW_ONCE("a root UAV without a bound view heap: the draw is dropped");
        return false;
    }
    DescHeap& heap = heap_of_object(heaps_[0]);
    SlotKey key{reinterpret_cast<std::uintptr_t>(&heap), address.value_or(0)};
    std::uint32_t index;
    RootSlot* slot_entry = nullptr;


    if (auto it = root_uav_slots_.find(key); it != root_uav_slots_.end()) {
        index = it->second.index;
        slot_entry = &it->second;
    } else {
        std::shared_ptr<shadow::Shadow> shadow = address ? find(*address) : nullptr;
        auto slot = host->dynamic(1);
        if (!slot) {
            return false;
        }
        index = *slot;
        dynamic_.push_back({key.heap, index, 1});
        if (shadow) {
            std::uint64_t base = shadow->buffer()->GetGPUVirtualAddress();
            std::uint64_t offset = shadow_gpu(*shadow, *address) - base;
            std::uint64_t bias = offset & 15;
            host->raw_uav_at(index, shadow->buffer(), offset - bias, std::min(shadow_left(*shadow, *address), limit) + bias);
        } else {
            host->null_raw_uav_at(index);
        }
        slot_entry = &root_uav_slots_.emplace(key, RootSlot{index, std::move(shadow)}).first->second;
    }
    const std::shared_ptr<shadow::Shadow>& shadow = slot_entry->shadow;
    if (address && shadow) {
        std::uint64_t epoch = list.ranges_epoch();
        if (slot_entry->ready_list != &list || slot_entry->ready_epoch != epoch || (writes && !slot_entry->ready_writes)) {
            ready(*address, shadow);


            slot_entry->ready_list = &list;
            slot_entry->ready_epoch = epoch;
            slot_entry->ready_writes = writes;
        }
    }
    auto handle = host->gpu_handle(index);
    compute ? l->SetComputeRootDescriptorTable(param, handle) : l->SetGraphicsRootDescriptorTable(param, handle);
    return true;
}

bool ListState::root_srv_table(HostList& list, std::optional<std::uint64_t> address, std::uint32_t param, bool compute, bool biased) {
    HostHeap* host = heaps_[0] ? heap_host(heap_of_object(heaps_[0])) : nullptr;
    if (!host) {
        DRAW_ONCE("a root SRV without a bound view heap: the draw is dropped");
        return false;
    }
    DescHeap& heap = heap_of_object(heaps_[0]);
    SlotKey key{reinterpret_cast<std::uintptr_t>(&heap), address.value_or(0) | (biased ? std::uint64_t{1} << 63 : 0)};
    std::uint32_t index;
    RootSlot* slot_entry = nullptr;
    if (auto it = root_srv_slots_.find(key); it != root_srv_slots_.end()) {
        index = it->second.index;
        slot_entry = &it->second;
    } else {
        std::shared_ptr<shadow::Shadow> shadow = address ? shadow_find(*address, 1) : nullptr;
        auto slot = host->dynamic(1);
        if (!slot) {
            return false;
        }
        index = *slot;
        dynamic_.push_back({key.heap, index, 1});
        slot_entry = &root_srv_slots_.emplace(key, RootSlot{index, shadow}).first->second;
        if (shadow) {
            std::uint64_t base = shadow->buffer()->GetGPUVirtualAddress();
            std::uint64_t offset = shadow_gpu(*shadow, *address) - base;
            if (biased) {
                std::uint64_t bias = offset & 15;
                host->raw_at(index, shadow->buffer(), offset - bias, shadow_left(*shadow, *address) + bias);
            } else if (offset % 16 == 0) {
                host->raw_at(index, shadow->buffer(), offset, shadow_left(*shadow, *address));
            } else {
                DRAW_ONCE("a root SRV not on 16 bytes reads zeros", {{"address", log::hex(*address)}});
                host->null_raw_at(index);
            }
        } else {
            host->null_raw_at(index);
        }
    }
    const std::shared_ptr<shadow::Shadow>& shadow = slot_entry->shadow;
    if (address && shadow) {
        std::uint64_t epoch = list.ranges_epoch();
        if (slot_entry->ready_list != &list || slot_entry->ready_epoch != epoch) {
            shadow_refresh(list, shadow, *address, shadow_left(*shadow, *address));
            slot_entry->ready_list = &list;
            slot_entry->ready_epoch = epoch;
        }
    }
    auto handle = host->gpu_handle(index);
    compute ? list.list()->SetComputeRootDescriptorTable(param, handle) : list.list()->SetGraphicsRootDescriptorTable(param, handle);
    return true;
}

std::optional<MemoryReady> ListState::memory_ready_for(HostList& list, bool compute, bool writes) {
    HostHeap* heap = heaps_[0] ? heap_host(heap_of_object(heaps_[0])) : nullptr;
    if (!heap) {
        return std::nullopt;
    }

    thread_local std::vector<std::uint64_t> addresses;
    thread_local std::vector<std::uint32_t> constants;
    addresses.clear();
    constants.clear();
    for (const Root& r : roots(compute)) {
        if (r.kind == Root::View) {
            addresses.push_back(r.value);
        } else if (r.kind == Root::Constants) {
            for (std::uint32_t c : r.constants) {
                constants.push_back(c);
            }
        }
    }
    return memory_ready(list, *heap, addresses, constants, writes);
}

void ListState::dispatch(HostList& list, std::uint32_t x, std::uint32_t y, std::uint32_t z) {

    end_window(list);

    if (is_builder(pipeline_) && !builder_runs()) {
        return;
    }
    const HostPipeline* p = host_pipeline(pipeline_);
    if (!p) {
        DRAW_ONCE("a dispatch without a host pipeline dropped", {{"pipeline", log::hex(reinterpret_cast<std::uintptr_t>(pipeline_))}});
        return;
    }
    if (!p->compute) {
        DRAW_ONCE("a dispatch with a graphics pipeline");
        return;
    }
    if (!bind(list, *p, true)) {


        if (is_builder(pipeline_)) {
            builder_off("one of its dispatches could not be bound");
        }
        return;
    }
    ID3D12GraphicsCommandList* l = list.list();
    auto base = group_base(*p);
    if (base && x > PART) {
        for (std::uint32_t at = 0; at < x; at += PART) {
            l->SetComputeRoot32BitConstant(base->first, at, base->second);
            l->Dispatch(std::min(x - at, PART), y, z);
        }
        l->SetComputeRoot32BitConstant(base->first, 0, base->second);
    } else {
        l->Dispatch(x, y, z);
    }

    bool writes = std::any_of(p->stages.begin(), p->stages.end(), [](const Stage& s) { return s.translated().writes; });
    if (writes) {
        list.set_uav_pending(true);
    }
    DISPATCHED.fetch_add(1, std::memory_order_relaxed);
}

}   


namespace danzi::d3d12x {

namespace {

bool windows_on() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_INDIRECT_WINDOW", L"0");
    return on;
}

bool merge_on() {
    static const bool on = [] {
        bool asked = danzi::env::not_set_or_not(L"VICTORY_INDIRECT_MERGE", L"0");
        bool joined = asked && windows_on();
        if (joined) {
            log::info(TARGET, "consecutive indirect calls on one window of records joined into one (VICTORY_INDIRECT_MERGE=1)");
        } else if (asked) {
            log::warn(TARGET, "VICTORY_INDIRECT_MERGE=1 needs VICTORY_INDIRECT_WINDOW=1: not joined");
        }
        return joined;
    }();
    return on;
}


constexpr usize BUCKETS = 9;
std::atomic<std::uint64_t> BATCH_OPS{0}, BATCH_CALLS{0};
std::array<std::atomic<std::uint64_t>, BUCKETS> BATCH_RUNS{};

void batch_recorded(std::uint32_t n) {
    BATCH_OPS.fetch_add(n, std::memory_order_relaxed);
    BATCH_CALLS.fetch_add(1, std::memory_order_relaxed);
    usize bucket = n <= 1 ? 0 : std::min<usize>(32 - std::countl_zero(n - 1), BUCKETS - 1);
    BATCH_RUNS[bucket].fetch_add(1, std::memory_order_relaxed);
}

bool light_restore() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_CLAMP_LIGHT_RESTORE", L"0");
    return on;
}

std::uint64_t window_most() {
    static const std::uint64_t most = [] {
        auto v = danzi::env::get(L"VICTORY_INDIRECT_WINDOW_MOST");
        std::uint64_t n = v ? std::wcstoull(v->c_str(), nullptr, 10) : 4096;
        return std::clamp<std::uint64_t>(v ? n : 4096, 16, 65536);
    }();
    return most;
}


ID3D12CommandSignature* host_signature(std::uint32_t kind, std::uint32_t stride) {
    static danzi::FastLock lock;
    static std::vector<std::pair<std::pair<std::uint32_t, std::uint32_t>, ComPtr<ID3D12CommandSignature>>> made;
    std::lock_guard g(lock);
    for (const auto& [k, s] : made) {
        if (k == std::pair{kind, stride}) {
            return s.Get();
        }
    }
    Host* h = host();
    if (!h) {
        return nullptr;
    }
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(kind);
    D3D12_COMMAND_SIGNATURE_DESC desc{stride, 1, &argument, 0};
    ComPtr<ID3D12CommandSignature> signature;
    if (HRESULT hr = h->device().device()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)); FAILED(hr)) {
        DRAW_ONCE("host command signature refused", {{"error", log::hresult(hr)}, {"kind", log::debug(kind)}, {"stride", log::debug(stride)}});
        return nullptr;
    }
    made.emplace_back(std::pair{kind, stride}, signature);
    return signature.Get();
}

bool legal_stride(std::uint32_t step, std::uint32_t stride) { return step >= stride && step % 4 == 0 && step > 0; }

}   

void batch_line(double frames) {
    if (!merge_on() || frames <= 0.0) {
        return;
    }
    std::uint64_t ops = BATCH_OPS.exchange(0, std::memory_order_relaxed);
    std::uint64_t calls = BATCH_CALLS.exchange(0, std::memory_order_relaxed);
    std::array<std::uint64_t, BUCKETS> runs;
    for (usize i = 0; i < BUCKETS; ++i) {
        runs[i] = BATCH_RUNS[i].exchange(0, std::memory_order_relaxed);
    }
    if (ops == 0) {
        return;
    }
    static constexpr const char* NAMES[BUCKETS] = {"1", "2", "3-4", "5-8", "9-16", "17-32", "33-64", "65-128", "129+"};
    std::string by_len;
    char b[64];
    for (usize i = 0; i < BUCKETS; ++i) {
        if (runs[i] > 0) {
            std::snprintf(b, sizeof b, "%s%s: %.0f", by_len.empty() ? "" : ", ", NAMES[i], static_cast<double>(runs[i]) / frames);
            by_len += b;
        }
    }
    std::snprintf(b, sizeof b, "%.0f ops -> %.0f host calls (", static_cast<double>(ops) / frames, static_cast<double>(calls) / frames);
    log::info(TARGET, std::string("indirect calls joined per frame (VICTORY_INDIRECT_MERGE=1): ") + b + by_len + ")");
}

std::optional<std::uint32_t> ListState::Call::slot() const {
    if (step == 0 || offset < base) {
        return std::nullopt;
    }
    std::uint64_t from = offset - base;
    if (from % step != 0) {
        return std::nullopt;
    }
    std::uint64_t s = from / step;
    return s < records ? std::optional(static_cast<std::uint32_t>(s)) : std::nullopt;
}

bool ListState::Call::same_state(const Call& o) const {
    return pipeline == o.pipeline && kind == o.kind && stride == o.stride && epoch == o.epoch && copy == o.copy && base == o.base && step == o.step &&
           records == o.records;
}

bool ListState::Run::push(const Call& next) {
    auto f = first.slot(), n = next.slot();
    if (!f || !n || !first.same_state(next) || std::uint64_t{*n} != std::uint64_t{*f} + count) {
        return false;
    }
    ++count;
    return true;
}

void ListState::touched(HostList& list) {
    ++epoch_;
    flush_held(list);
}

void ListState::finish_list(HostList& list) {
    flush_held(list);
    end_window(list);
}

void ListState::end_window(HostList& list) {
    if (window_) {
        auto w = std::move(*window_);
        window_.reset();
        indirect_done(list, w.scratch.Get());
    }
}

void ListState::flush_held(HostList& list) {
    if (!held_) {
        return;
    }
    Held h = std::move(*held_);
    held_.reset();
    constexpr std::uint32_t MESH = 10;
    std::uint32_t slot = h.run.first.slot().value_or(0);
    std::uint32_t step = h.run.first.step;
    if (ID3D12CommandSignature* signature = host_signature(h.run.first.kind, step)) {
        list.list()->ExecuteIndirect(signature, h.run.count, h.scratch.Get(), std::uint64_t{slot} * step, nullptr, 0);
        batch_recorded(h.run.count);
        const HostPipeline* p = host_pipeline(pipeline_);
        if (barrier_merge() && (!p || std::any_of(p->stages.begin(), p->stages.end(), [](const Stage& s) { return s.translated().writes; }))) {
            list.set_uav_pending(true);
        }
    }
    if (h.run.first.kind == MESH) {
        mesh_scissor(list, false);
    }
    if (!window_ || window_->scratch.Get() != h.scratch.Get()) {
        indirect_done(list, h.scratch.Get());
    }
}

bool ListState::execute_indirect(HostList& list, std::uint32_t kind, std::uint32_t stride, std::uint32_t max, std::uint64_t arguments,
                                 std::optional<std::uint64_t> count) {
    constexpr std::uint32_t DRAW = 0, INDEXED = 1, DISPATCH = 2, MESH = 10;

    if (is_builder(pipeline_) && !builder_runs()) {
        return false;
    }
    const HostPipeline* p = host_pipeline(pipeline_);
    if (!p) {
        return false;
    }
    bool has_vertex_inputs = std::any_of(p->inputs.begin(), p->inputs.end(), [](const Input& e) { return !e.per_instance; });


    bool made_rects = !p->rects && kind == DRAW && topology_ == RECTANGLES && !has_vertex_inputs;
    if (made_rects) {
        DRAW_ONCE("indirect rectangles without vertex inputs drawn as their first triangle");
    }
    bool matches = false;
    switch (kind) {
        case DRAW:
        case INDEXED: matches = (topology_ != RECTANGLES || made_rects || p->rects) && !p->compute && !p->mesh; break;
        case MESH: matches = !p->compute && p->mesh; break;
        case DISPATCH: matches = p->compute; break;
        default: break;
    }
    if (!matches) {
        return false;
    }
    std::uint64_t len = std::uint64_t{stride} * std::max(max, 1u);
    auto args = shadow_find_any(arguments, len);
    if (!args) {
        return false;
    }
    std::shared_ptr<shadow::Shadow> counter;
    if (count) {
        counter = shadow_find_any(*count, 4);
        if (!counter) {
            return false;
        }
    }
    ID3D12CommandSignature* signature = host_signature(kind, stride);
    if (!signature) {
        return false;
    }
    if (kind == MESH && (mesh_test() == 1 || !mesh_kept(pipeline_))) {
        return false;   
    }
    constexpr usize WINDOW_USE = SIZE_MAX - 0x77;
    std::uint64_t offset = shadow_offset(*args, arguments);


    std::optional<std::pair<std::pair<std::uint32_t, std::uint32_t>, ID3D12CommandSignature*>> parts;
    if (kind == DISPATCH && max == 1 && !count) {
        if (auto base = group_base(*p)) {
            if (auto split = host_signature(DISPATCH, static_cast<std::uint32_t>(PART_BYTES))) {
                parts = std::pair{*base, split};
            }
        }
    }
    bool windowed = windows_on() && kind != DISPATCH && max == 1 && !count &&
                    !std::any_of(p->stages.begin(), p->stages.end(), [](const Stage& s) { return s.translated().stores; });


    if (list.first_use(WINDOW_USE)) {
        window_.reset();
    } else if (!windowed) {
        flush_held(list);
        end_window(list);
    }
    bool joining = merge_on() && windowed && !parts && max == 1 && !count;
    auto this_call = [&](const Window& w) { return Call{pipeline_, kind, stride, epoch_, w.copy, w.base, w.step, w.records, offset}; };
    if (held_) {
        bool joined = false;
        if (joining && window_) {
            joined = held_->run.push(this_call(*window_));
        }
        if (joined) {
            DRAWN.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        flush_held(list);
    }
    auto same = [&](const Window& w) { return w.copy == args->id() && w.kind == kind && w.stride == stride; };
    ID3D12Resource* bounded = nullptr;
    std::uint64_t at = 0;
    if (window_ && same(*window_) && offset >= window_->base && (offset - window_->base) % window_->step == 0 &&
        (offset - window_->base) / window_->step < window_->records) {
        bounded = window_->scratch.Get();
        at = offset - window_->base;
    } else {


        std::uint32_t step = stride, records = max;
        if (window_ && windowed && same(*window_)) {
            const Window& w = *window_;
            std::uint64_t last = w.base + std::uint64_t{w.records - 1} * w.step;
            std::uint64_t d = offset > last ? offset - last : 0;
            bool walks = d >= stride && d <= 256 && d % 4 == 0 && (w.records == 1 || d == w.step);
            if (walks) {
                std::uint64_t fit = std::min(shadow_left(*args, arguments) / d, window_most());
                step = static_cast<std::uint32_t>(d);
                std::uint32_t want = w.records == 1 ? 16 : w.records * 2;
                records = std::max<std::uint32_t>(std::min<std::uint64_t>(want, fit), 1);
            }
        }
        end_window(list);
        shadow_refresh(list, args, arguments, std::max(len, std::uint64_t{step} * records));
        if (counter) {
            shadow_refresh(list, counter, *count, 4);
        }
        std::optional<std::pair<ID3D12Resource*, std::uint64_t>> count_arg;
        if (counter) {
            count_arg = std::pair{counter->buffer(), shadow_offset(*counter, *count)};
        }
        bool own = !args->placed() && (!counter || !counter->placed());
        if (parts) {
            bounded = bound_split(list, args->buffer(), offset, own);
        } else if (windowed && records > 1) {
            bounded = bound_ahead(list, args->buffer(), offset, Spec{kind, step, records}, own);
        } else {
            bounded = bound(list, args->buffer(), offset, count_arg, Spec{kind, step, records}, own);
        }
        if (!bounded) {
            return false;
        }


        const void* before = host_pipeline_;
        host_roots_[1] = nullptr;
        host_pipeline_ = nullptr;
        if (light_restore() && kind != DISPATCH && before == p->pso.Get()) {
            list.list()->SetPipelineState(p->pso.Get());
            host_pipeline_ = before;
        }
        if (windowed) {
            window_ = Window{args->id(), kind, stride, step, offset, records, bounded};
        } else {
            window_.reset();
        }
        at = 0;
    }
    bool ran = false;
    switch (kind) {
        case DRAW:
        case INDEXED: ran = prepare(list, kind == INDEXED); break;
        case MESH: ran = prepare(list, false, true); break;
        case DISPATCH: ran = bind(list, *p, true); break;
        default: break;
    }
    if (!ran) {
        if (!window_ || window_->scratch.Get() != bounded) {
            indirect_done(list, bounded);
        }
        return false;
    }
    if (kind == MESH) {
        mesh_scissor(list, true);
    }

    if (joining && window_ && window_->scratch.Get() == bounded) {
        Call c = this_call(*window_);
        auto slot = c.slot();
        if (slot && legal_stride(c.step, c.stride) && std::uint64_t{*slot} * c.step == at && host_signature(kind, c.step)) {
            held_ = Held{Run{c, 1}, bounded};
            DRAWN.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    if (parts) {
        auto [base, split] = *parts;
        for (std::uint32_t part = 0; part < SPLIT; ++part) {
            list.list()->SetComputeRoot32BitConstant(base.first, part * PART, base.second);
            list.list()->ExecuteIndirect(split, 1, bounded, part * PART_BYTES, nullptr, 0);
        }
        list.list()->SetComputeRoot32BitConstant(base.first, 0, base.second);
    } else {
        list.list()->ExecuteIndirect(signature, max, bounded, at, counter ? counter->buffer() : nullptr, counter ? shadow_offset(*counter, *count) : 0);
    }
    batch_recorded(1);
    if (barrier_merge() && std::any_of(p->stages.begin(), p->stages.end(), [](const Stage& s) { return s.translated().writes; })) {
        list.set_uav_pending(true);
    }
    if (!window_ || window_->scratch.Get() != bounded) {
        indirect_done(list, bounded);
    }
    if (kind == MESH) {
        mesh_scissor(list, false);
    }
    (kind == DISPATCH ? DISPATCHED : DRAWN).fetch_add(1, std::memory_order_relaxed);
    return true;
}

}   
