// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/draw.rs (DanziLoader / victory-perf, see
// NOTICE.md): a list keeps the state the title sets for its draws and
// dispatches (pipeline, descriptor heaps, root arguments, vertex and index
// buffers, targets); each draw or dispatch then brings the host list up to
// date: the host pipeline and our root signature, the user data translated
// shaders read, the host copies of the buffers and descriptors they read,
// and the states of the textures involved.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "descriptor.hpp"
#include "object.hpp"

namespace danzi::shadow {
class Shadow;
}

namespace danzi::d3d12x {

class HostList;
struct HostPipeline;
struct DescHeap;
class HostHeap;
struct MemoryReady;

struct VertexView {
    std::uint64_t address;
    std::uint32_t size, stride;
};
struct IndexView {
    std::uint64_t address;
    std::uint32_t size, format;
};


template <class T, std::size_t N>
class SmallVec {
public:
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    T* begin() { return data(); }
    T* end() { return data() + count_; }
    const T* begin() const { return data(); }
    const T* end() const { return data() + count_; }
    T& operator[](std::size_t i) { return data()[i]; }
    const T& operator[](std::size_t i) const { return data()[i]; }
    T& back() { return data()[count_ - 1]; }
    void clear() {
        count_ = 0;
        spill_.clear();
    }
    void push_back(const T& v) {
        if (count_ < N && spill_.empty()) {
            inline_[count_++] = v;
            return;
        }
        if (spill_.empty()) {
            spill_.assign(inline_.begin(), inline_.begin() + count_);
        }
        spill_.push_back(v);
        ++count_;
    }
    template <class... A>
    void emplace_back(A&&... a) {
        push_back(T{std::forward<A>(a)...});
    }
    void erase_from(T* at) {
        std::size_t n = static_cast<std::size_t>(at - data());
        if (!spill_.empty()) {
            spill_.resize(n);
        }
        count_ = n;
    }

private:
    T* data() { return spill_.empty() ? inline_.data() : spill_.data(); }
    const T* data() const { return spill_.empty() ? inline_.data() : spill_.data(); }
    std::array<T, N> inline_{};
    std::size_t count_ = 0;
    std::vector<T> spill_;
};

using ComPtrResource = Microsoft::WRL::ComPtr<ID3D12Resource>;

class ListState {
public:
    ListState() = default;
    ListState(ListState&&) noexcept = default;
    ListState& operator=(ListState&& o) noexcept;
    ~ListState();


    void set_topology(HostList& list, std::uint32_t topology);
    void set_vertex_buffers(std::uint32_t start, const VertexView* views, std::uint32_t count);
    void clear_vertex_buffers(std::uint32_t start, std::uint32_t count);
    void set_index_buffer(const IndexView* view);
    void set_targets(HostList& list, const std::uint64_t* handles, std::uint32_t count, std::uint64_t depth);
    void set_viewports(HostList& list, const D3D12_VIEWPORT* viewports, std::uint32_t count);
    void set_scissors(HostList& list, const D3D12_RECT* rects, std::uint32_t count);
    void set_blend_factor(HostList& list, const float* factor);
    void set_stencil_ref(HostList& list, std::uint32_t reference);
    void set_depth_bounds(HostList& list, float min, float max);

    void draw(HostList& list, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first, std::uint32_t first_instance);
    void draw_indexed(HostList& list, std::uint32_t indices, std::uint32_t instances, std::uint32_t first, std::int32_t base, std::uint32_t first_instance);

    void reset(This pipeline);
    void set_pipeline(This pipeline) { pipeline_ = pipeline; }
    void set_heaps(const This* heaps, std::uint32_t count);
    void set_table(bool compute, std::uint32_t slot, std::uint64_t handle);
    void set_view(bool compute, std::uint32_t slot, std::uint64_t address);
    void set_constants(bool compute, std::uint32_t slot, const std::uint32_t* values, std::uint32_t count, std::uint32_t offset);

    void dispatch(HostList& list, std::uint32_t x, std::uint32_t y, std::uint32_t z);


    void dispatch_mesh(HostList& list, std::uint32_t x);


    void tables_done();


    void replay(HostList& list);


    void touched(HostList& list);


    void finish_list(HostList& list);


    bool execute_indirect(HostList& list, std::uint32_t kind, std::uint32_t stride, std::uint32_t max, std::uint64_t arguments,
                          std::optional<std::uint64_t> count);

private:
    struct Root {
        enum Kind : std::uint8_t { None, Table, Constants, View } kind = None;
        std::uint64_t value = 0;
        std::vector<std::uint32_t> constants;
    };
    using Roots = std::vector<Root>;
    Roots& roots(bool compute) { return compute ? compute_ : graphics_; }
    const Roots& roots(bool compute) const { return compute ? compute_ : graphics_; }
    DescHeap* heap_of(std::uint64_t handle) const;
    std::uint32_t user_value(bool compute, std::uint8_t slot, std::uint8_t root_kind, std::uint32_t dword) const;
    std::uint32_t host_word(bool compute, std::uint32_t kind, std::uint8_t slot, std::uint32_t dword, const MemoryReady* memory = nullptr) const;


    std::optional<MemoryReady> memory_ready_for(HostList& list, bool compute, bool writes);
    bool bind(HostList& list, const HostPipeline& p, bool compute);
    using Made = SmallVec<std::pair<std::uint8_t, std::uint32_t>, 16>;
    void tables(HostList& list, const void* translated, bool compute, Made& made);
    void table_entry(HostList& list, std::uint8_t slot, std::uint32_t table_offset_dwords, std::uint8_t kind, bool compute);
    bool root_uav(HostList& list, std::optional<std::uint64_t> address, std::uint32_t param, bool compute, bool writes);
    bool root_srv_table(HostList& list, std::optional<std::uint64_t> address, std::uint32_t param, bool compute, bool biased);
    bool prepare(HostList& list, bool indexed, bool mesh = false);
    void mesh_scissor(HostList& list, bool on);
    bool rectangles(HostList& list, const HostPipeline& p, std::uint32_t count, std::uint32_t first);
    void give_back();
    void end_window(HostList& list);
    void flush_held(HostList& list);
    void readable(HostList& list, std::uint8_t kind, const View& view);


    DescHeap* view_heap() const;
    std::optional<std::uint64_t> heap_buffers(HostList& list);
    void heap_textures(HostList& list, DescHeap& heap, HostHeap& host, std::optional<std::pair<std::uint64_t, std::uint64_t>> pass_budget);
    void early_textures(HostList& list, const HostPipeline& p, bool compute);

    This pipeline_ = nullptr;

    std::array<This, 2> heaps_{};
    Roots graphics_, compute_;


    const void* host_pipeline_ = nullptr;
    std::array<const void*, 2> host_roots_{};
    std::array<This, 2> host_heaps_{};
    struct TableKey {
        std::uint64_t table;
        std::uint32_t n;
        std::uint64_t hash;
        bool operator==(const TableKey&) const = default;
    };
    struct TableKeyHash {
        std::size_t operator()(const TableKey& k) const { return static_cast<std::size_t>(k.table * 0x9e3779b97f4a7c15ull ^ k.hash ^ k.n); }
    };
    std::unordered_map<TableKey, std::uint32_t, TableKeyHash> fresh_tables_;
    struct Dynamic {
        std::uintptr_t heap;
        std::uint32_t first, n;
    };
    std::vector<Dynamic> dynamic_;
    struct SlotKey {
        std::uintptr_t heap;
        std::uint64_t address;
        bool operator==(const SlotKey&) const = default;
    };
    struct SlotKeyHash {
        std::size_t operator()(const SlotKey& k) const { return static_cast<std::size_t>(k.heap * 31 ^ k.address * 0x9e3779b97f4a7c15ull); }
    };
    struct RootSlot {
        std::uint32_t index;
        std::shared_ptr<shadow::Shadow> shadow;


        const HostList* ready_list = nullptr;
        std::uint64_t ready_epoch = 0;
        bool ready_writes = false;
    };
    std::unordered_map<SlotKey, RootSlot, SlotKeyHash> root_srv_slots_, root_uav_slots_;
    bool tables_gone_ = false;

    std::array<VertexView, 32> vertex_{};
    IndexView index_{};
    std::vector<This> targets_;
    This depth_ = nullptr;
    std::uint32_t depth_state_ = 0x10;
    std::uint32_t topology_ = 0;
    bool topology_set_ = false;
    std::vector<std::uint64_t> target_handles_;
    std::uint64_t depth_handle_ = 0;
    bool targets_set_ = false;
    std::vector<D3D12_VIEWPORT> viewports_;
    std::vector<D3D12_RECT> scissors_;
    std::optional<std::array<float, 4>> blend_;
    std::optional<std::uint32_t> stencil_;
    std::optional<std::array<float, 2>> depth_bounds_;


    struct Window {
        std::uint64_t copy;
        std::uint32_t kind, stride, step;
        std::uint64_t base;
        std::uint32_t records;
        ComPtrResource scratch;
    };
    std::optional<Window> window_;
    struct Call {
        This pipeline;
        std::uint32_t kind, stride;
        std::uint64_t epoch, copy, base;
        std::uint32_t step, records;
        std::uint64_t offset;
        std::optional<std::uint32_t> slot() const;
        bool same_state(const Call& o) const;
    };
    struct Run {
        Call first;
        std::uint32_t count;
        bool push(const Call& next);
    };
    struct Held {
        Run run;
        ComPtrResource scratch;
    };
    std::optional<Held> held_;
    std::uint64_t epoch_ = 0;

    const HostList* kept_list_ = nullptr;
    std::uint64_t kept_epoch_ = 0;
    const void* kept_pso_ = nullptr;


    bool stores_ = true;
    std::pair<This, bool> tested_{nullptr, false};
};


void batch_line(double frames);


std::uint64_t drawn();
std::uint64_t dispatched();

}   
