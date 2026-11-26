// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/list.rs (see list.hpp).
#include "list.hpp"
#include "root.hpp"
#include "draw.hpp"
#include "pipeline.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "core/env.hpp"
#include "core/log.hpp"
#include "core/title_copy.hpp"
#include "core/win.hpp"
#include "device.hpp"
#include "resource.hpp"
#include "descriptor.hpp"
#include "resource_iids.hpp"
#include "texture.hpp"
#include "sync.hpp"

namespace danzi::d3d12x {

namespace {

const std::array<Iid, 4> ALLOCATOR_IIDS = {
    guid(0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};

const std::array<Iid, 10> LIST_IIDS = {
    guid(0x7116d91c, 0xe7e4, 0x47ce, {0xb8, 0xc6, 0xec, 0x81, 0x68, 0xf4, 0x37, 0xe5}),
    guid(0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}),
    guid(0x553103fb, 0x1fe7, 0x4557, {0xbb, 0x38, 0x94, 0x6d, 0x7d, 0x0e, 0x7c, 0xa7}),
    guid(0x38c3e585, 0xff17, 0x412c, {0x91, 0x50, 0x4f, 0xc6, 0xf9, 0xd7, 0x2a, 0x28}),
    guid(0x6fda83a7, 0xb84c, 0x4e38, {0x9a, 0xc8, 0xc7, 0xbd, 0x22, 0x01, 0x6b, 0x3d}),
    guid(0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}),
    guid(0x55050859, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}),
    guid(0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}),
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};

struct Allocator {
    Object object;
    std::uint32_t kind;
};


struct ListPart {
    std::vector<ValueWait> waits;
    std::unique_ptr<HostList> host;
};


struct List {
    Object object;
    std::uint32_t kind;


    std::uint64_t put[5];
    std::uint32_t reserved[64];

    std::uint32_t graphics_root_headers[128];
    std::uint32_t compute_root_headers[128];
    std::uint32_t pipeline_shadow[8];


    std::uint32_t newer[256];


    danzi::FastLock uploads_lock;
    std::vector<std::unique_ptr<std::uint8_t[]>> uploads;
    std::unique_ptr<HostList> host;
    danzi::FastLock state_lock;
    ListState state;
    std::vector<ListPart> earlier;
    std::vector<ValueWait> gate;
};
static_assert(offsetof(List, put) == 0x18);
static_assert(offsetof(List, pipeline_shadow) + 32 == 0x560);

List& as_list(This self) { return *reinterpret_cast<List*>(self); }


std::mutex IDLE_LOCK;
std::vector<std::unique_ptr<HostList>> IDLE;
constexpr std::size_t IDLE_MOST = 128;


bool pools() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_LIST_POOL", L"0");
    return on;
}


std::unique_ptr<HostList> fresh_list() {
    {
        std::lock_guard lock(IDLE_LOCK);
        if (!IDLE.empty()) {
            auto list = std::move(IDLE.back());
            IDLE.pop_back();
            return list;
        }
    }
    Host* h = host();
    return h ? h->new_list() : nullptr;
}


std::mutex VALUE_WAITS_LOCK;
std::vector<std::pair<This, ValueWait>> VALUE_WAITS;


HostList* host_list_rec(This self) {
    HostList* h = host_list(self);


    if (h) {
        List& l = as_list(self);
        if (l.state_lock.try_lock()) {
            l.state.touched(*h);
            l.state_lock.unlock();
        }
    }
    return h;
}


void cut(This self, const ValueWait& wait) {
    if (!self) {
        return;
    }
    List& list = as_list(self);
    if (!list.host) {
        return;
    }
    if (list.host->untouched()) {
        list.gate.push_back(wait);
        return;
    }
    auto fresh = fresh_list();
    if (!fresh) {


        static std::once_flag once;
        std::call_once(once, [] { log::warn("victory_gpu::list", "no host list for the part after a wait: the wait moves to the part's start"); });
        list.gate.push_back(wait);
        return;
    }
    {
        std::lock_guard lock(list.state_lock);
        list.state.finish_list(*list.host);
    }
    list.host->close();
    static std::atomic<std::uint64_t> cut_count{0};
    if (cut_count.fetch_add(1, std::memory_order_relaxed) < 4) {
        log::info("victory_gpu::list", "a list is cut at a wait for a memory value", {{"parts", log::debug(static_cast<std::uint64_t>(list.earlier.size() + 2))}});
    }
    std::unique_ptr<HostList> done = std::exchange(list.host, std::move(fresh));
    std::vector<ValueWait> waits = std::exchange(list.gate, std::vector<ValueWait>{wait});
    list.earlier.push_back({std::move(waits), std::move(done)});

    {
        std::lock_guard lock(list.state_lock);
        list.state.replay(*list.host);
    }
}

void wait_for(This self, std::uint64_t address, std::uint32_t comparison, std::uint64_t reference, bool wide) {
    ValueWait wait{address, comparison, reference, wide};
    static std::atomic<std::uint64_t> seen{0};
    if (seen.fetch_add(1, std::memory_order_relaxed) < 12) {
        log::info("victory_gpu::list", "a list waits for a memory value",
                  {{"address", log::hex(address)}, {"comparison", log::debug(comparison)}, {"reference", log::debug(reference)}, {"wide", log::debug(wide)},
                   {"met", log::debug(wait.met())}});
    }
    if (cuts()) {
        cut(self, wait);
        return;
    }
    std::lock_guard lock(VALUE_WAITS_LOCK);
    VALUE_WAITS.emplace_back(self, wait);
}

void defer_write(This self, std::uint64_t address, Written::Kind kind, std::uint64_t value, std::size_t bytes) {
    if (address == 0) {
        return;
    }
    if (HostList* list = host_list_rec(self)) {
        list->defer_write(reinterpret_cast<std::uint8_t*>(address), kind, value, bytes);
        return;
    }

    Written{kind, value}.write(reinterpret_cast<std::uint8_t*>(address), bytes);
}


void value_written(This self, std::uint64_t address) {

    static const bool on = danzi::env::is(L"VICTORY_CUT_WRITES", L"1") && cuts();
    if (!on || address == 0 || !self) {
        return;
    }
    List& list = as_list(self);
    if (!list.host) {
        return;
    }
    auto fresh = fresh_list();
    if (!fresh) {
        return;
    }
    list.host->close();
    std::unique_ptr<HostList> done = std::exchange(list.host, std::move(fresh));
    list.earlier.push_back({std::exchange(list.gate, {}), std::move(done)});
}


__declspec(noinline) void outside_buffer(std::uint64_t offset, std::uint64_t len, std::uint64_t size) {
    log::warn("victory_gpu::list", "copy outside its buffer", {{"offset", log::debug(offset)}, {"len", log::debug(len)}, {"size", log::debug(size)}});
}


std::uint8_t* buffer_range(This buffer, std::uint64_t offset, std::uint64_t len) {
    std::uint8_t* data;
    std::uint64_t size;
    if (!buffer_bytes(buffer, data, size) || offset > UINT64_MAX - len) {
        return nullptr;
    }
    if (offset + len > size) {
        outside_buffer(offset, len, size);
        return nullptr;
    }
    return data + offset;
}


HRESULT __stdcall allocator_query(This self, const Iid* iid, This* out) { return answer(ALLOCATOR, iid, is(iid, ALLOCATOR_IIDS) ? self : nullptr, out); }
std::uint32_t __stdcall allocator_release(This self) { return release_boxed<Allocator>(self); }

HRESULT __stdcall allocator_reset(This) { return S_OK_; }

const void* const* allocator_functions() {
    static const Table<24> table = [] {
        auto f = d3d12_object(missing_table<ALLOCATOR, 24>());
        f[0] = fn(&allocator_query);
        f[2] = fn(&allocator_release);
        f[7] = fn(&parent_device);
        f[8] = fn(&allocator_reset);
        f[9] = fn(&ignore);   
        return f;
    }();
    return table.data();
}


HRESULT __stdcall list_query(This self, const Iid* iid, This* out) { return answer(LIST, iid, is(iid, LIST_IIDS) ? self : nullptr, out); }
std::uint32_t __stdcall list_release(This self) { return release_boxed<List>(self); }
std::uint32_t __stdcall list_type(This self) { return as_list(self).kind; }

HRESULT __stdcall close(This self) {
    if (HostList* h = host_list(self)) {
        {
            std::lock_guard lock(as_list(self).state_lock);
            as_list(self).state.finish_list(*h);
        }
        h->close();
    }
    return S_OK_;
}

HRESULT __stdcall reset(This self, This allocator, This pipeline) {
    if (!allocator) {
        return E_INVALIDARG_;
    }
    {

        std::lock_guard lock(VALUE_WAITS_LOCK);
        std::erase_if(VALUE_WAITS, [&](const auto& w) { return w.first == self; });
    }
    List& list = as_list(self);
    {
        std::lock_guard lock(list.uploads_lock);
        list.uploads.clear();
    }
    list.earlier.clear();
    list.gate.clear();
    if (list.host) {
        std::lock_guard lock(list.state_lock);
        list.host->reset();
        list.state.reset(pipeline);
    }
    return S_OK_;
}


HRESULT __stdcall reset_x(This self, This allocator, This pipeline, std::uint32_t) { return reset(self, allocator, pipeline); }

void __stdcall set_pipeline(This self, This pipeline) {
    pipeline_bound(pipeline);
    std::lock_guard lock(as_list(self).state_lock);
    as_list(self).state.set_pipeline(pipeline);
}


template <class F>
void with_state(This self, F&& f) {
    List& list = as_list(self);
    if (!list.host) {
        return;
    }
    std::lock_guard lock(list.state_lock);
    list.state.touched(*list.host);
    f(*list.host, list.state);
}

void __stdcall state_heaps(This self, std::uint32_t count, const This* heaps) {
    with_state(self, [&](HostList&, ListState& s) { s.set_heaps(heaps, count); });
}
void __stdcall compute_table(This self, std::uint32_t slot, std::uint64_t handle) {
    with_state(self, [&](HostList&, ListState& s) { s.set_table(true, slot, handle); });
}
void __stdcall graphics_table(This self, std::uint32_t slot, std::uint64_t handle) {
    with_state(self, [&](HostList&, ListState& s) { s.set_table(false, slot, handle); });
}
void __stdcall compute_constant(This self, std::uint32_t slot, std::uint32_t value, std::uint32_t offset) {
    with_state(self, [&](HostList&, ListState& s) { s.set_constants(true, slot, &value, 1, offset); });
}
void __stdcall graphics_constant(This self, std::uint32_t slot, std::uint32_t value, std::uint32_t offset) {
    with_state(self, [&](HostList&, ListState& s) { s.set_constants(false, slot, &value, 1, offset); });
}
void __stdcall compute_constants(This self, std::uint32_t slot, std::uint32_t count, const std::uint32_t* values, std::uint32_t offset) {
    with_state(self, [&](HostList&, ListState& s) { s.set_constants(true, slot, values, values ? count : 0, offset); });
}
void __stdcall graphics_constants(This self, std::uint32_t slot, std::uint32_t count, const std::uint32_t* values, std::uint32_t offset) {
    with_state(self, [&](HostList&, ListState& s) { s.set_constants(false, slot, values, values ? count : 0, offset); });
}
void __stdcall compute_view(This self, std::uint32_t slot, std::uint64_t address) {
    with_state(self, [&](HostList&, ListState& s) { s.set_view(true, slot, address); });
}
void __stdcall graphics_view(This self, std::uint32_t slot, std::uint64_t address) {
    with_state(self, [&](HostList&, ListState& s) { s.set_view(false, slot, address); });
}
void __stdcall state_topology(This self, std::uint32_t topology) {
    with_state(self, [&](HostList& l, ListState& s) { s.set_topology(l, topology); });
}
void __stdcall state_viewports(This self, std::uint32_t count, const D3D12_VIEWPORT* viewports) {
    with_state(self, [&](HostList& l, ListState& s) { s.set_viewports(l, viewports, viewports ? count : 0); });
}
void __stdcall state_scissors(This self, std::uint32_t count, const D3D12_RECT* rects) {
    with_state(self, [&](HostList& l, ListState& s) { s.set_scissors(l, rects, rects ? count : 0); });
}
void __stdcall state_blend_factor(This self, const float* factor) {
    with_state(self, [&](HostList& l, ListState& s) { s.set_blend_factor(l, factor); });
}
void __stdcall state_stencil_ref(This self, std::uint32_t reference) {
    with_state(self, [&](HostList& l, ListState& s) { s.set_stencil_ref(l, reference); });
}
void __stdcall state_depth_bounds(This self, float min, float max) {
    with_state(self, [&](HostList& l, ListState& s) { s.set_depth_bounds(l, min, max); });
}
void __stdcall state_index_buffer(This self, const IndexView* view) {
    with_state(self, [&](HostList&, ListState& s) { s.set_index_buffer(view); });
}
void __stdcall state_vertex_buffers(This self, std::uint32_t start, std::uint32_t count, const VertexView* views) {
    with_state(self, [&](HostList&, ListState& s) {
        if (views) {
            s.set_vertex_buffers(start, views, count);
        } else {
            s.clear_vertex_buffers(start, count);
        }
    });
}


void __stdcall state_targets(This self, std::uint32_t count, const std::uint64_t* handles, std::int32_t one_range, const std::uint64_t* depth) {
    std::uint64_t buffer[8];
    std::vector<std::uint64_t> more;
    std::uint64_t* list_of = buffer;
    std::uint32_t n = handles ? count : 0;
    if (n > 8) {
        more.resize(n);
        list_of = more.data();
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        list_of[i] = one_range ? handles[0] + 128 * std::uint64_t{i} : handles[i];
    }
    std::uint64_t d = depth ? *depth : 0;
    with_state(self, [&](HostList& l, ListState& s) { s.set_targets(l, list_of, n, d); });
}


alignas(64) std::atomic<std::uint64_t> DRAWS{0};
alignas(64) std::atomic<std::uint64_t> DISPATCHES{0};
std::atomic<std::uint64_t> INDIRECT{0};
void count_draw() { DRAWS.fetch_add(1, std::memory_order_relaxed); }

void __stdcall draw(This self, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first, std::uint32_t first_instance) {
    count_draw();
    with_state(self, [&](HostList& l, ListState& s) { s.draw(l, vertices, instances, first, first_instance); });
}
void __stdcall draw_indexed(This self, std::uint32_t indices, std::uint32_t instances, std::uint32_t first, std::int32_t base, std::uint32_t first_instance) {
    count_draw();
    with_state(self, [&](HostList& l, ListState& s) { s.draw_indexed(l, indices, instances, first, base, first_instance); });
}


void __stdcall execute_indirect(This self, This signature, std::uint32_t max, This arguments, std::uint64_t offset, This count, std::uint64_t count_offset) {
    count_draw();
    std::uint32_t stride = 0, n = 0;
    const std::array<std::uint32_t, 4>* args = nullptr;
    if (!signature_layout(signature, stride, n, args)) {
        return;
    }
    std::uint8_t* data = nullptr;
    std::uint64_t size = 0;
    std::optional<std::uint64_t> address;
    if (buffer_bytes(arguments, data, size)) {
        address = reinterpret_cast<std::uint64_t>(data) + offset;
    }
    std::optional<std::uint64_t> counter;
    if (count) {
        std::uint8_t* c = nullptr;
        std::uint64_t cs = 0;
        if (buffer_bytes(count, c, cs)) {
            counter = reinterpret_cast<std::uint64_t>(c) + count_offset;
        }
    }
    bool ran = false;
    if (n == 1 && args && address && (!count || counter)) {
        constexpr std::uint32_t DISPATCH = 2, DISPATCH_SINGLE = 105;
        std::uint32_t kind = args[0][0] == DISPATCH_SINGLE ? DISPATCH : args[0][0];

        List& list = as_list(self);
        if (list.host) {
            std::lock_guard lock(list.state_lock);
            ran = list.state.execute_indirect(*list.host, kind, stride, max, *address, counter);
        }
    }
    if (ran) {
        return;
    }
    INDIRECT.fetch_add(1, std::memory_order_relaxed);
    static std::mutex seen_lock;
    static std::vector<This> seen;
    std::lock_guard lock(seen_lock);
    if (std::find(seen.begin(), seen.end(), signature) == seen.end()) {
        seen.push_back(signature);
        std::string listed = "[";
        for (std::uint32_t i = 0; i < n && args; ++i) {
            listed += (i ? ", [" : "[") + std::to_string(args[i][0]) + ", " + std::to_string(args[i][1]) + ", " + std::to_string(args[i][2]) + ", " +
                      std::to_string(args[i][3]) + "]";
        }
        listed += "]";
        log::info("victory_gpu::list", "indirect execution dropped (first time for this signature)",
                  {{"stride", log::debug(stride)},
                   {"args", listed},
                   {"max", log::debug(max)},
                   {"address", address ? "Some(" + log::debug(log::hex(*address)) + ")" : std::string("None")},
                   {"counted", log::debug(count != nullptr)}});
    }
}

void __stdcall dispatch(This self, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    DISPATCHES.fetch_add(1, std::memory_order_relaxed);
    with_state(self, [&](HostList& l, ListState& s) { s.dispatch(l, x, y, z); });
}
void __stdcall dispatch_x(This self, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t) { dispatch(self, x, y, z); }

void __stdcall dispatch_mesh(This self, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    count_draw();
    if (y * z != 1) {
        static std::once_flag once;
        std::call_once(once, [&] {
            log::warn("victory_gpu::list", "a mesh dispatch with y or z groups", {{"x", log::debug(x)}, {"y", log::debug(y)}, {"z", log::debug(z)}});
        });
    }
    with_state(self, [&](HostList& l, ListState& s) { s.dispatch_mesh(l, x); });
}


void __stdcall dispatch_mesh_x(This self, std::uint32_t x, std::uint64_t, std::uint64_t indirect, std::uint64_t) {
    count_draw();
    if (indirect != 0) {
        static std::once_flag once;
        std::call_once(once, [] { log::warn("victory_gpu::list", "indirect mesh dispatches dropped"); });
        return;
    }
    with_state(self, [&](HostList& l, ListState& s) { s.dispatch_mesh(l, x); });
}


struct Barrier {
    std::uint32_t kind, flags;
    This first;
    std::uint64_t second, last;
};
static_assert(sizeof(Barrier) == sizeof(D3D12_RESOURCE_BARRIER));


std::optional<D3D12_RESOURCE_BARRIER> host_barrier(HostList& list, const Barrier& b) {
    D3D12_RESOURCE_BARRIER out{};
    out.Flags = static_cast<D3D12_RESOURCE_BARRIER_FLAGS>(b.flags);
    switch (b.kind) {
        case 0: {

            if (b.flags & 1) {
                return std::nullopt;
            }
            if (!b.first) {
                return std::nullopt;
            }


            std::uint32_t wanted = texture_state(static_cast<std::uint32_t>(b.last));
            auto t = list.transition(b.first, wanted);
            if (!t) {
                return std::nullopt;
            }
            return transition_barrier(t->first, t->second, legal_state(t->first, wanted));
        }
        case 1: {
            out.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            if (b.first) {
                out.Aliasing.pResourceBefore = host_resource(b.first);
                if (!out.Aliasing.pResourceBefore) {
                    return std::nullopt;
                }
            }
            if (b.second != 0) {
                out.Aliasing.pResourceAfter = host_resource(reinterpret_cast<This>(b.second));
                if (!out.Aliasing.pResourceAfter) {
                    return std::nullopt;
                }
            }
            return out;
        }
        case 2: {
            out.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            if (b.first) {

                ID3D12Resource* r = twin_resource(b.first);
                out.UAV.pResource = r ? r : host_resource(b.first);
                if (!out.UAV.pResource) {
                    return std::nullopt;
                }
            }
            return out;
        }
        default: return std::nullopt;
    }
}


std::uint32_t __stdcall barrier(This self, std::uint32_t count, const Barrier* barriers) {
    HostList* list = host_list_rec(self);
    if (!list || !barriers) {
        return 0;
    }
    std::vector<D3D12_RESOURCE_BARRIER> host;
    bool on_buffer = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const Barrier& b = barriers[i];
        std::uint8_t* data;
        std::uint64_t size;
        bool buffer = b.first && buffer_bytes(b.first, data, size);
        if (auto h = host_barrier(*list, b)) {
            host.push_back(*h);
        }
        if (b.kind != 1 && buffer) {
            on_buffer = true;
        }
    }
    if (on_buffer) {
        D3D12_RESOURCE_BARRIER u{};
        u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        host.push_back(u);
    }
    if (!host.empty()) {
        list->list()->ResourceBarrier(static_cast<UINT>(host.size()), host.data());
    }
    return 0;
}


void __stdcall copy_buffer_region(This self, This dst, std::uint64_t dst_offset, This src, std::uint64_t src_offset, std::uint64_t len) {
    HostList* list = host_list_rec(self);
    if (!list) {
        return;
    }
    std::uint8_t* to = buffer_range(dst, dst_offset, len);
    std::uint8_t* from = to ? buffer_range(src, src_offset, len) : nullptr;
    if (to && from) {
        list->defer_copy(to, from, static_cast<usize>(len));
    }
}


bool unlike(This dst, This src) {
    bool unlike = stands_in(dst) != stands_in(src);
    if (unlike) {
        static std::once_flag once;
        std::call_once(once, [&] {
            log::warn("victory_gpu::list", "copy between a stand-in target and title-format texels skipped",
                      {{"dst", log::hex(reinterpret_cast<std::uintptr_t>(dst))}, {"src", log::hex(reinterpret_cast<std::uintptr_t>(src))}});
        });
    }
    return unlike;
}

constexpr std::uint32_t COPY_DEST = 0x400;
constexpr std::uint32_t COPY_SOURCE = 0x800;

void __stdcall copy_resource(This self, This dst, This src) {
    HostList* list = host_list_rec(self);
    if (!list) {
        return;
    }
    ID3D12Resource* to = host_resource(dst);
    ID3D12Resource* from = to ? host_resource(src) : nullptr;
    if (to && from) {
        if (unlike(dst, src)) {
            return;
        }

        if (title_written(from)) {
            upload(*list, src, from);
        }

        list->twin_use(dst, false, true);
        list->twin_use(src, false, false);
        ensure_state(*list, dst, COPY_DEST);
        ensure_state(*list, src, COPY_SOURCE);
        host_written(dst);
        list->list()->CopyResource(to, from);
        return;
    }
    std::uint8_t *a_at, *b_at;
    std::uint64_t a, b;
    if (buffer_bytes(dst, a_at, a) && buffer_bytes(src, b_at, b)) {
        list->defer_copy(a_at, b_at, static_cast<usize>(std::min(a, b)));
    }
}


struct CopyLocation {
    This resource;
    std::uint32_t kind;
    PlacedFootprint footprint;
};
static_assert(sizeof(CopyLocation) == sizeof(D3D12_TEXTURE_COPY_LOCATION));
constexpr std::uint32_t FOOTPRINT = 1;


std::optional<std::uint64_t> footprint_bytes(const PlacedFootprint& f) {
    std::uint64_t each, block;
    if (!element(f.format, each, block)) {
        return std::nullopt;
    }
    std::uint64_t lines = (std::uint64_t{f.height} + block - 1) / block * f.depth;
    std::uint64_t last = (std::uint64_t{f.width} + block - 1) / block * each;
    return std::uint64_t{f.row_pitch} * (lines > 0 ? lines - 1 : 0) + last;
}


std::optional<std::pair<PlacedFootprint, std::array<std::uint64_t, 4>>> host_rows(const PlacedFootprint& f) {
    std::uint64_t each, block;
    if (!element(f.format, each, block)) {
        return std::nullopt;
    }
    std::uint64_t lines = std::max<std::uint64_t>((std::uint64_t{f.height} + block - 1) / block * f.depth, 1);
    std::uint64_t row = (std::uint64_t{f.width} + block - 1) / block * each;
    std::uint64_t pitch = (std::max<std::uint64_t>(row, 1) + 255) / 256 * 256;
    if (pitch > UINT32_MAX) {
        return std::nullopt;
    }
    PlacedFootprint host = f;
    host.offset = 0;
    host.row_pitch = static_cast<std::uint32_t>(pitch);
    return std::pair{host, std::array<std::uint64_t, 4>{pitch * lines, row, lines, f.row_pitch}};
}


const char* read_back(HostList& list, CopyLocation dst, std::uint32_t x, std::uint32_t y, std::uint32_t z, CopyLocation src, const D3D12_BOX* box) {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_READ_BACK", L"0");
    if (!on) {
        return "switched off";
    }
    if (dst.kind != FOOTPRINT || src.kind == FOOTPRINT) {
        return "not a texture subresource into a placed footprint";
    }
    if (x != 0 || y != 0 || z != 0) {
        return "a destination corner other than the footprint's";
    }
    Host* h = host();
    if (!h) {
        return "no host";
    }
    auto rows = host_rows(dst.footprint);
    if (!rows) {
        return "an unknown format";
    }
    auto [footprint, sizes] = *rows;
    auto [size, row, count, pitch] = sizes;

    std::uint64_t bytes = pitch * (count - 1) + row;
    std::uint8_t* data = buffer_range(dst.resource, dst.footprint.offset, bytes);
    if (!data) {
        return "a destination outside its buffer";
    }
    ID3D12Resource* texture = host_resource(src.resource);
    if (!texture) {
        return "no host texture";
    }
    auto back = list.read_back_room(size);
    if (!back) {
        return "no read-back buffer";
    }


    list.twin_use(src.resource, false, false);
    if (title_written(texture)) {
        upload(list, src.resource, texture);
    }
    ensure_state(list, src.resource, COPY_SOURCE);
    CopyLocation from = src, to = dst;
    from.resource = reinterpret_cast<This>(texture);
    to.resource = reinterpret_cast<This>(back->buffer);
    to.footprint = footprint;
    to.footprint.offset = back->offset;
    list.list()->CopyTextureRegion(reinterpret_cast<const D3D12_TEXTURE_COPY_LOCATION*>(&to), 0, 0, 0,
                                   reinterpret_cast<const D3D12_TEXTURE_COPY_LOCATION*>(&from), box);
    list.defer_read_back(back->data, data, static_cast<usize>(row), static_cast<usize>(count), static_cast<usize>(pitch));
    return nullptr;
}


void __stdcall copy_texture_region(This self, const CopyLocation* dst, std::uint32_t x, std::uint32_t y, std::uint32_t z, const CopyLocation* src,
                                   const D3D12_BOX* box) {
    HostList* list = host_list_rec(self);
    if (!list || !dst || !src) {
        return;
    }
    ID3D12Resource* to = host_resource(dst->resource);
    if (!to) {


        static std::atomic<std::uint64_t> count{0}, refused{0};
        std::uint64_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
        const char* made = read_back(*list, *dst, x, y, z, *src, box);
        if (n <= 16 || n % 4096 == 0 || made) {
            if (made && refused.fetch_add(1, std::memory_order_relaxed) >= 16) {
                return;
            }
            std::string source = "None";
            if (ID3D12Resource* r = host_resource(src->resource)) {
                D3D12_RESOURCE_DESC d = r->GetDesc();
                source = "Some(" +
                         log::debug(std::to_string(d.Width) + "x" + std::to_string(d.Height) + " f" + std::to_string(static_cast<int>(d.Format)) + " mips " +
                                    std::to_string(d.MipLevels)) +
                         ")";
            }
            auto bytes = footprint_bytes(dst->footprint);
            log::info("victory_gpu::list", "a texture to buffer copy (a read back)",
                      {{"copies", log::debug(n)},
                       {"made", made ? "Err(" + log::debug(made) + ")" : std::string("Ok(())")},
                       {"source", source},
                       {"source_kind", log::debug(src->kind)},
                       {"destination_kind", log::debug(dst->kind)},
                       {"destination_offset", log::debug(dst->footprint.offset)},
                       {"destination_bytes", bytes ? "Some(" + log::debug(*bytes) + ")" : std::string("None")}});
        }
        return;
    }
    if (unlike(dst->resource, src->resource)) {
        return;
    }
    CopyLocation from = *src;
    if (src->kind == FOOTPRINT) {
        auto bytes = footprint_bytes(src->footprint);
        if (!bytes) {
            return;
        }
        std::uint8_t* data = buffer_range(src->resource, src->footprint.offset, *bytes);
        if (!data) {
            return;
        }
        auto staging = list->upload(*bytes);
        if (!staging) {
            return;
        }
        list->defer_fill(staging->data, data, static_cast<usize>(*bytes));
        from.resource = reinterpret_cast<This>(staging->buffer);
        from.footprint.offset = staging->offset;
    } else {
        ID3D12Resource* texture = host_resource(src->resource);
        if (!texture) {
            return;
        }
        list->twin_use(src->resource, false, false);
        if (title_written(texture)) {
            upload(*list, src->resource, texture);
        }
        ensure_state(*list, src->resource, COPY_SOURCE);
        from.resource = reinterpret_cast<This>(texture);
    }

    list->twin_use(dst->resource, false, true);
    ensure_state(*list, dst->resource, COPY_DEST);
    host_written(dst->resource);
    CopyLocation to_location = *dst;
    to_location.resource = reinterpret_cast<This>(to);
    list->list()->CopyTextureRegion(reinterpret_cast<const D3D12_TEXTURE_COPY_LOCATION*>(&to_location), x, y, z,
                                    reinterpret_cast<const D3D12_TEXTURE_COPY_LOCATION*>(&from), box);
}

void __stdcall copy_texture_region_x(This self, const CopyLocation* dst, std::uint32_t x, std::uint32_t y, std::uint32_t z, const CopyLocation* src,
                                     const D3D12_BOX* box, std::uint32_t) {
    copy_texture_region(self, dst, x, y, z, src, box);
}

void __stdcall resolve(This self, This dst, std::uint32_t dst_sub, This src, std::uint32_t src_sub, std::uint32_t format) {
    constexpr std::uint32_t RESOLVE_DEST = 0x1000, RESOLVE_SOURCE = 0x2000;
    HostList* list = host_list_rec(self);
    if (!list) {
        return;
    }
    ID3D12Resource* to = host_resource(dst);
    ID3D12Resource* from = to ? host_resource(src) : nullptr;
    if (!to || !from || unlike(dst, src)) {
        return;
    }
    list->twin_use(dst, false, true);
    list->twin_use(src, false, false);
    ensure_state(*list, dst, RESOLVE_DEST);
    ensure_state(*list, src, RESOLVE_SOURCE);
    host_written(dst);
    D3D12_RESOURCE_DESC a = to->GetDesc(), b = from->GetDesc();
    auto f = stands_in(dst) ? a.Format : static_cast<DXGI_FORMAT>(format);
    if (a.Width == b.Width && a.Height == b.Height) {
        list->list()->ResolveSubresource(to, dst_sub, from, src_sub, f);
    } else {

        ComPtr<ID3D12GraphicsCommandList1> list1;
        if (SUCCEEDED(list->list()->QueryInterface(IID_PPV_ARGS(&list1)))) {
            D3D12_RECT rect{0, 0, static_cast<LONG>(std::min(a.Width, b.Width)), static_cast<LONG>(std::min(a.Height, b.Height))};
            list1->ResolveSubresourceRegion(to, dst_sub, 0, 0, from, src_sub, &rect, f, D3D12_RESOLVE_MODE_AVERAGE);
        }
    }
}

void __stdcall clear_rtv(This self, std::uint64_t handle, const float* color, std::uint32_t count, const D3D12_RECT* rects) {
    constexpr std::uint32_t RENDER_TARGET = 0x4;
    HostList* list = host_list_rec(self);
    auto record = rtv_record(handle);
    if (!list || !record) {
        return;
    }
    auto copy = host_copy(record->resource);
    Host* h = host();
    if (!copy || !h || !color) {
        return;
    }
    auto words = record->host_desc(false);
    auto rtv = h->view_of(*copy, false, words ? &*words : nullptr);
    if (auto t = list->transition(copy, RENDER_TARGET)) {
        auto barrier = transition_barrier(t->first, t->second, RENDER_TARGET);
        list->list()->ResourceBarrier(1, &barrier);
    }
    list->list()->ClearRenderTargetView(rtv, color, count > 0 && rects ? count : 0, count > 0 ? rects : nullptr);
}


void __stdcall clear_dsv(This self, std::uint64_t handle, std::uint32_t flags, float depth, std::uint8_t stencil, std::uint32_t count, const D3D12_RECT* rects) {
    constexpr std::uint32_t DEPTH_WRITE = 0x10;
    HostList* list = host_list_rec(self);
    auto record = dsv_record(handle);
    if (!list || !record) {
        return;
    }
    auto copy = host_copy(record->resource);
    Host* h = host();
    if (!copy || !h) {
        return;
    }

    list->twin_use(record->resource, false, true);
    if (auto t = list->transition(copy, DEPTH_WRITE)) {
        auto barrier = transition_barrier(t->first, t->second, legal_for(copy->flags, DEPTH_WRITE));
        list->list()->ResourceBarrier(1, &barrier);
    }
    auto words = record->host_desc(true);
    auto dsv = h->view_of(*copy, true, words ? &*words : nullptr);
    list->list()->ClearDepthStencilView(dsv, static_cast<D3D12_CLEAR_FLAGS>(flags & 3), depth, stencil, count > 0 && rects ? count : 0,
                                        count > 0 ? rects : nullptr);
}

void __stdcall copy_resource_x(This self, This dst, This src, std::uint32_t) { copy_resource(self, dst, src); }


void __stdcall copy_memory(This self, std::uint64_t dst, std::uint64_t src, std::uint64_t len, std::uint32_t) {
    HostList* list = host_list_rec(self);
    if (!list || dst == 0 || src == 0) {
        return;
    }
    auto n = static_cast<usize>(len);
    list->defer_known(static_cast<usize>(dst), static_cast<usize>(src), n, [dst, src, n] {
        if (committed(reinterpret_cast<const void*>(src), n) && committed(reinterpret_cast<const void*>(dst), n)) {
            std::memmove(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), n);
        }
    });
}


std::uint64_t __stdcall driver_hint(This, std::uint64_t, std::uint64_t) { return 0; }


void __stdcall query_mark(This, This, std::uint32_t, std::uint32_t) { DANZI_EMPTY_BODY; }


void __stdcall resolve_queries(This self, This, std::uint32_t kind, std::uint32_t, std::uint32_t count, This buffer, std::uint64_t offset) {
    std::uint8_t* data;
    std::uint64_t size;
    if (!buffer_bytes(buffer, data, size)) {
        return;
    }
    Written what{Written::Value, 0};
    std::uint64_t bytes = 16;
    switch (kind) {
        case 0: what = {Written::Value, 1u << 20}; bytes = 8; break;
        case 1: what = {Written::Value, 1}; bytes = 8; break;
        case 2: what = {Written::GpuTimestamp, 0}; bytes = 8; break;
        case 3: what = {Written::Value, 0}; bytes = 88; break;
        default: break;
    }
    if (offset + std::uint64_t{count} * bytes > size) {
        return;
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        for (std::uint64_t piece = 0; piece < bytes / 8; ++piece) {
            std::uint8_t* at = data + offset + i * bytes + piece * 8;
            defer_write(self, reinterpret_cast<std::uint64_t>(at), piece == 0 ? what.kind : Written::Value, piece == 0 ? what.value : 0, 8);
        }
    }
}


void __stdcall sync(This, std::uint32_t, std::uint32_t, std::uint64_t, std::uint64_t) { DANZI_EMPTY_BODY; }


std::uint8_t* __stdcall allocate_upload(This self, std::uint64_t size, std::uint64_t alignment) {
    std::uint64_t align = std::max<std::uint64_t>(alignment, 16);
    if (!self || (align & (align - 1)) != 0 || size > SIZE_MAX - align) {
        return nullptr;
    }
    auto len = static_cast<std::size_t>(size + align);
    std::unique_ptr<std::uint8_t[]> block(new (std::nothrow) std::uint8_t[len]());
    if (!block) {
        return nullptr;
    }
    auto start = reinterpret_cast<std::uintptr_t>(block.get());
    auto* at = reinterpret_cast<std::uint8_t*>((start + align - 1) & ~(align - 1));
    List& list = as_list(self);
    std::lock_guard lock(list.uploads_lock);
    list.uploads.push_back(std::move(block));
    return at;
}


void __stdcall ignored_state(This) { DANZI_EMPTY_BODY; }

void __stdcall wait_value(This self, std::uint64_t address, std::uint32_t comparison, std::uint64_t reference) {
    wait_for(self, address, comparison, reference, false);
}
void __stdcall wait_value64(This self, std::uint64_t address, std::uint32_t comparison, std::uint64_t reference) {
    wait_for(self, address, comparison, reference, true);
}


void __stdcall barrier_groups(This self, std::uint32_t count, const std::uint8_t*) {
    if (count == 0) {
        return;
    }
    if (HostList* list = host_list_rec(self)) {


        if (barrier_merge()) {
            if (!list->uav_pending()) {
                return;
            }
            list->set_uav_pending(false);
        }
        D3D12_RESOURCE_BARRIER u{};
        u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->list()->ResourceBarrier(1, &u);
    }
}


void __stdcall write32(This self, std::uint64_t address, std::uint32_t value, std::uint32_t, std::uint32_t) {
    defer_write(self, address, Written::Value, value, 4);
    value_written(self, address);
}
void __stdcall write64(This self, std::uint64_t address, std::uint64_t value, std::uint32_t, std::uint32_t flags) {
    Written what = flags == 1 ? Written{Written::GpuTimestamp, 0} : flags == 2 ? Written{Written::SystemTimestamp, 0} : Written{Written::Value, value};
    defer_write(self, address, what.kind, what.value, 8);
    if (what.kind == Written::Value) {
        value_written(self, address);
    }
}


struct Method {
    std::size_t slot;
    const void* method;
};
const Method METHODS[] = {
    {16, fn(&copy_texture_region)}, {19, fn(&resolve)},          {51, fn(&clear_dsv)},          {52, fn(&clear_rtv)},
    {83, fn(&copy_texture_region_x)},
    {0, fn(&list_query)},          {2, fn(&list_release)},       {7, fn(&parent_device)},       {8, fn(&list_type)},
    {9, fn(&close)},               {10, fn(&reset)},             {15, fn(&copy_buffer_region)}, {17, fn(&copy_resource)},
    {12, fn(&draw)},               {13, fn(&draw_indexed)},      {20, fn(&state_topology)},     {21, fn(&state_viewports)},
    {22, fn(&state_scissors)},     {23, fn(&state_blend_factor)}, {24, fn(&state_stencil_ref)},  {47, fn(&state_index_buffer)},
    {48, fn(&state_vertex_buffers)}, {50, fn(&state_targets)},   {68, fn(&state_depth_bounds)},
    {59, fn(&execute_indirect)},
    {14, fn(&dispatch)},           {32, fn(&state_heaps)},       {35, fn(&compute_table)},      {36, fn(&graphics_table)},
    {37, fn(&compute_constant)},   {38, fn(&graphics_constant)}, {39, fn(&compute_constants)},  {40, fn(&graphics_constants)},
    {41, fn(&compute_view)},       {42, fn(&graphics_view)},     {43, fn(&compute_view)},       {44, fn(&graphics_view)},
    {45, fn(&compute_view)},       {46, fn(&graphics_view)},     {78, fn(&dispatch_x)},
    {127, fn(&dispatch_mesh)},     {98, fn(&dispatch_mesh_x)},
    {25, fn(&set_pipeline)},       {26, fn(&barrier)},           {28, fn(&query_mark)},         {29, fn(&query_mark)},
    {30, fn(&resolve_queries)},    {33, fn(&ignored_state)},     {34, fn(&ignored_state)},      {60, fn(&copy_memory)},
    {69, fn(&write32)},            {70, fn(&write64)},           {71, fn(&wait_value)},         {72, fn(&wait_value64)},
    {73, fn(&ignored_state)},      {84, fn(&copy_resource_x)},   {99, fn(&driver_hint)},        {124, fn(&ignored_state)},
    {129, fn(&sync)},              {130, fn(&sync)},             {137, fn(&reset_x)},
};


const Method NEWER_METHODS[] = {{142, fn(&ignored_state)}, {146, fn(&barrier_groups)}, {148, fn(&allocate_upload)}};


constexpr std::size_t newer_slot(std::size_t slot) { return slot >= 59 ? slot + 2 : slot; }

Table<160> list_table(bool newer) {
    auto f = d3d12_object(missing_table<LIST, 160>());
    for (const Method& m : METHODS) {
        f[newer ? newer_slot(m.slot) : m.slot] = m.method;
    }
    if (newer) {
        for (const Method& m : NEWER_METHODS) {
            f[m.slot] = m.method;
        }
    }
    return f;
}

const void* const* list_functions(bool newer) {
    static const Table<160> older = list_table(false);
    static const Table<160> newer_ = list_table(true);
    return newer ? newer_.data() : older.data();
}

const bool MADE = (allocator_functions(), list_functions(false), list_functions(true), true);


HRESULT new_list(std::uint32_t kind, This allocator, This pipeline, const Iid* iid, This* out) {
    if (!allocator) {
        return E_INVALIDARG_;
    }
    auto* list = new List{Object{list_functions(newer())}, kind};
    if (Host* h = host()) {
        HRESULT hr = S_OK;
        list->host = h->new_list(&hr);
        if (!list->host) {
            log::warn("victory_gpu::list", "no host command list", {{"error", log::hresult(hr)}});
        }
    }
    list->state.set_pipeline(pipeline);
    return hand_out(LIST, list, iid, LIST_IIDS, out);
}

}   

HRESULT __stdcall create_allocator(This, std::uint32_t kind, const Iid* iid, This* out) {
    auto* a = new Allocator{Object{allocator_functions()}, kind};
    return hand_out(ALLOCATOR, a, iid, ALLOCATOR_IIDS, out);
}

HRESULT __stdcall create_list(This, std::uint32_t, std::uint32_t kind, This allocator, This pipeline, const Iid* iid, This* out) {
    return new_list(kind, allocator, pipeline, iid, out);
}

HRESULT __stdcall create_list_x(This, const ListDesc* desc, This allocator, This pipeline, const Iid* iid, This* out) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    return new_list(desc->kind, allocator, pipeline, iid, out);
}

void ensure_state(HostList& list, This resource, std::uint32_t state) {
    if (auto t = list.transition_fast(resource, state)) {
        auto barrier = transition_barrier(t->first, t->second, legal_state(t->first, state));
        list.list()->ResourceBarrier(1, &barrier);
    }
}

bool ValueWait::holds_for(std::uint64_t value) const {
    std::uint64_t v = wide ? value : value & 0xffffffffu;
    std::uint64_t r = wide ? reference : reference & 0xffffffffu;
    switch (comparison) {
        case 1: return false;
        case 2: return v < r;
        case 3: return v == r;
        case 4: return v <= r;
        case 5: return v > r;
        case 6: return v != r;
        case 7: return v >= r;
        default: return true;
    }
}


bool ValueWait::met() const {
    std::size_t bytes = wide ? 8 : 4;
    if (address == 0) {
        return true;
    }
    static const bool ask = danzi::env::is(L"VICTORY_WAIT_ASK", L"1");
    if (ask && !committed(reinterpret_cast<const void*>(address), bytes)) {
        return true;
    }
    std::uint8_t raw[8] = {};
    if (!danzi::host::read_into(reinterpret_cast<const std::uint8_t*>(address), raw, bytes)) {
        return true;
    }
    std::uint64_t v;
    std::memcpy(&v, raw, 8);
    return holds_for(v);
}

HostList* host_list(This self) { return self ? as_list(self).host.get() : nullptr; }

void tables_done(std::span<const This>) {

}

bool cut_list(This self) {
    if (!self) {
        return false;
    }
    const List& list = as_list(self);
    return !list.earlier.empty() || !list.gate.empty();
}

Taken detach(std::span<const This> lists) {
    Taken out;
    std::vector<This> taken;
    for (This self : lists) {
        if (!self) {
            continue;
        }
        if (auto it = std::find(taken.begin(), taken.end(), self); it != taken.end()) {
            out.order.push_back(static_cast<std::size_t>(it - taken.begin()));
            continue;
        }
        auto fresh = fresh_list();
        if (!fresh) {
            continue;
        }


        fresh->close();
        List& list = as_list(self);
        if (!list.host) {
            continue;
        }
        Detached d;
        d.host = std::exchange(list.host, std::move(fresh));
        {
            std::lock_guard lock(list.state_lock);
            d.state = std::move(list.state);
            list.state = ListState{};
        }
        {
            std::lock_guard lock(list.uploads_lock);
            d.uploads = std::move(list.uploads);
            list.uploads.clear();
        }
        out.order.push_back(taken.size());
        taken.push_back(self);
        out.lists.push_back(std::move(d));
    }
    return out;
}

std::vector<Part> detach_parts(std::span<const This> lists) {
    std::vector<Part> parts(1);


    struct Seen {
        This list;
        bool single;
        std::size_t entry, place;
    };
    std::vector<Seen> seen;
    for (This self : lists) {
        if (!self) {
            continue;
        }
        if (auto it = std::find_if(seen.begin(), seen.end(), [&](const Seen& s) { return s.list == self; }); it != seen.end()) {


            if (it->single && it->entry + 1 == parts.size()) {
                parts[it->entry].taken.order.push_back(it->place);
            } else {
                static std::once_flag once;
                std::call_once(once, [] { log::warn("victory_gpu::list", "a list named twice by a submission that waits for memory values runs once"); });
            }
            continue;
        }
        auto fresh = fresh_list();
        if (!fresh) {
            continue;
        }
        fresh->close();
        List& list = as_list(self);
        if (!list.host) {
            continue;
        }
        std::unique_ptr<HostList> host = std::exchange(list.host, std::move(fresh));
        std::vector<ListPart> earlier = std::move(list.earlier);
        list.earlier.clear();
        std::vector<ValueWait> gate = std::move(list.gate);
        list.gate.clear();
        std::vector<std::unique_ptr<std::uint8_t[]>> uploads;
        {
            std::lock_guard lock(list.uploads_lock);
            uploads = std::move(list.uploads);
            list.uploads.clear();
        }
        bool single = earlier.empty() && gate.empty();
        std::vector<std::pair<std::vector<ValueWait>, Detached>> pieces;
        for (ListPart& p : earlier) {
            pieces.emplace_back(std::move(p.waits), Detached{std::move(p.host), {}, {}});
        }
        ListState state;
        {
            std::lock_guard lock(list.state_lock);
            state = std::move(list.state);
            list.state = ListState{};
        }
        pieces.emplace_back(std::move(gate), Detached{std::move(host), std::move(uploads), std::move(state)});
        std::size_t entry = 0, place = 0;
        for (auto& [waits, detached] : pieces) {
            if (!waits.empty()) {
                parts.push_back(Part{std::move(waits), {}});
            }
            entry = parts.size() - 1;
            Taken& t = parts[entry].taken;
            place = t.lists.size();
            t.order.push_back(place);
            t.lists.push_back(std::move(detached));
        }
        seen.push_back({self, single, entry, place});
    }

    std::erase_if(parts, [](const Part& p) { return p.taken.lists.empty(); });
    return parts;
}

void recycle(std::vector<Detached> lists, std::span<const std::size_t> ran) {
    if (!pools()) {
        return;
    }
    for (std::size_t i = 0; i < lists.size(); ++i) {
        Detached& d = lists[i];
        d.uploads.clear();

        d.state = ListState{};
        if (std::find(ran.begin(), ran.end(), i) == ran.end()) {
            continue;
        }
        {
            std::lock_guard lock(IDLE_LOCK);
            if (IDLE.size() >= IDLE_MOST) {
                continue;
            }
        }


        d.host->reset();
        std::lock_guard lock(IDLE_LOCK);
        if (IDLE.size() < IDLE_MOST) {
            IDLE.push_back(std::move(d.host));
        }
    }
}

std::vector<ValueWait> unmet_waits(std::span<const This> lists) {
    std::lock_guard lock(VALUE_WAITS_LOCK);
    std::vector<ValueWait> unmet;
    std::erase_if(VALUE_WAITS, [&](const std::pair<This, ValueWait>& w) {
        if (std::find(lists.begin(), lists.end(), w.first) == lists.end()) {
            return false;
        }
        if (!w.second.met()) {
            unmet.push_back(w.second);
        }
        return true;
    });
    return unmet;
}

std::vector<std::vector<ValueWait>> waits_by_list(std::span<const This> lists) {
    std::lock_guard lock(VALUE_WAITS_LOCK);
    std::vector<std::vector<ValueWait>> by(lists.size());
    std::erase_if(VALUE_WAITS, [&](const std::pair<This, ValueWait>& w) {
        auto it = std::find(lists.begin(), lists.end(), w.first);
        if (it == lists.end()) {
            return false;
        }
        by[static_cast<std::size_t>(it - lists.begin())].push_back(w.second);
        return true;
    });
    return by;
}

std::pair<std::uint64_t, std::uint64_t> take_counts() {
    return {DRAWS.exchange(0, std::memory_order_relaxed), DISPATCHES.exchange(0, std::memory_order_relaxed)};
}

std::uint64_t take_indirect() { return INDIRECT.exchange(0, std::memory_order_relaxed); }

}   
