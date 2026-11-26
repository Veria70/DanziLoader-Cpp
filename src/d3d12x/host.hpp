// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/host.rs, the submission core (DanziLoader /
// victory-perf, see NOTICE.md): the host GPU (milestone 3's device, card
// path and registry of host copies), host command lists with the work they
// do on the CPU at submission (copies of title memory, refreshes of host
// copies, ranges shaders write, bottom-of-pipe writes, read-backs), and the
// submission that is waited for: steps, snapshots, the card's plan, the
// lists, the wait, the card's fetch and the copy-back, then what tells the
// title the GPU is done.
//
// Still to come: submissions sent ahead without a wait (`VICTORY_LATE_WAIT`,
// the relay's landing thread: M4.7a part 2), the sweep that lets idle copies
// go (M4.11), preludes for texture states and twins (M4.7c), presentation
// (M4.10).
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "draw.hpp"

#include "core/shadow.hpp"
#include "core/win.hpp"
#include "gpu/card.hpp"
#include "gpu/device.hpp"
#include "gpu/lookup.hpp"
#include "sync.hpp"

namespace danzi::d3d12x {

using gpu::ComPtr;


inline std::uint32_t pc_state(std::uint32_t state) { return state & 0x01ffffffu; }


inline std::uint32_t texture_state(std::uint32_t state) { return pc_state(state) & ~std::uint32_t{0x1 | 0x2 | 0x100 | 0x200}; }


struct Step {
    enum Kind : std::uint8_t {

        Copy,

        Fill,

        Refresh,

        Run,

        Host,

        GpuWrite,


        Texture,
    } kind;
    bool known = false;


    std::uint32_t work = 0;
    usize dst = 0, src = 0, len = 0;
    std::shared_ptr<shadow::Shadow> shadow;
    std::shared_ptr<struct Staging> staging;
};


template <class T, class U>
void push_doubling(std::vector<T>& v, U&& item) {
    if (v.size() == v.capacity()) {
        v.reserve(std::max<std::size_t>(16, v.capacity() * 2));
    }
    v.push_back(std::forward<U>(item));
}

struct Deferred {
    std::vector<Step> steps;
    std::vector<std::function<void()>> works;


    std::vector<gpu::Pooled> uploads;
    std::uint64_t upload_bytes = 0;
    struct Chunk {
        ID3D12Resource* buffer = nullptr;
        std::uint8_t* data = nullptr;
        std::uint64_t used = 0, size = 0;
    };
    Chunk up_chunk, back_chunk;

    std::vector<ComPtr<IUnknown>> kept;


    struct Key {
        usize copy, address, len;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const { return k.copy * 0x9e3779b97f4a7c15ull ^ k.address * 31 ^ k.len; }
    };
    std::unordered_set<Key, KeyHash> refreshed, gpu_written;

    std::unordered_set<usize> textures;

    struct Write {
        usize at;
        Written what;
        usize bytes;
    };
    std::vector<Write> writes;


    struct Back {
        const std::uint8_t* from;
        usize at, row, rows, pitch;
    };
    std::vector<Back> backs;

    bool budget_zeroed = false;
};

struct HostTexture;
struct Twin;


D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource, std::uint32_t before, std::uint32_t after);


class HostList {
public:
    HostList(ComPtr<ID3D12CommandAllocator> allocator, ComPtr<ID3D12GraphicsCommandList> list);
    ~HostList();
    HostList(const HostList&) = delete;
    HostList& operator=(const HostList&) = delete;

    ID3D12GraphicsCommandList* list() const { return list_.Get(); }

    ID3D12GraphicsCommandList6* list6() {
        if (!list6_ && list_) {
            list_.As(&list6_);
        }
        return list6_.Get();
    }

    void close();

    void reset();

    bool untouched();


    std::optional<std::uint64_t> budget();


    ID3D12Resource* indirect_scratch(std::uint64_t size);


    std::uint64_t ranges_epoch() const { return epoch_.load(std::memory_order_relaxed); }
    bool uav_pending() const { return uav_pending_.load(std::memory_order_relaxed); }
    void set_uav_pending(bool pending) { uav_pending_.store(pending, std::memory_order_relaxed); }

    void defer_copy(std::uint8_t* dst, const std::uint8_t* src, usize len);
    void defer_fill(std::uint8_t* dst, const std::uint8_t* src, usize len);
    void defer_refresh(const std::shared_ptr<shadow::Shadow>& shadow, usize address, usize len);
    void defer_gpu_write(const std::shared_ptr<shadow::Shadow>& shadow, usize address, usize len);
    void defer_read_back(const std::uint8_t* from, std::uint8_t* dst, usize row, usize rows, usize pitch);


    void defer_write(std::uint8_t* dst, Written::Kind kind, std::uint64_t value, usize bytes);
    void defer(std::function<void()> work);
    void defer_known(usize dst, usize src, usize len, std::function<void()> work);
    void defer_host(std::function<void()> work);
    void keep(ComPtr<IUnknown> object);


    bool first_use(usize key);


    void defer_texture(std::shared_ptr<Staging> staging);


    struct Slice {
        ID3D12Resource* buffer;
        std::uint8_t* data;
        std::uint64_t offset;
    };


    std::optional<Slice> upload(std::uint64_t size, std::uint64_t align = 512) { return room(size, false, align); }
    std::optional<Slice> read_back_room(std::uint64_t size) { return room(size, true, 512); }
    std::optional<Slice> room(std::uint64_t size, bool back, std::uint64_t align);


    std::optional<std::pair<ID3D12Resource*, std::uint32_t>> transition(This resource, std::uint32_t wanted);
    std::optional<std::pair<ID3D12Resource*, std::uint32_t>> transition(const std::shared_ptr<HostTexture>& copy, std::uint32_t wanted);


    std::optional<std::pair<ID3D12Resource*, std::uint32_t>> transition_fast(This resource, std::uint32_t wanted);


    bool twin_use(This resource, bool colour, bool write);


    void twins_written();


    ID3D12CommandList* prelude();


    template <class F>
    auto with_deferred(F&& f) {
        std::lock_guard lock(lock_);
        return f(deferred_);
    }

private:
    struct TwinUse;
    void settle(const std::shared_ptr<HostTexture>& copy, std::uint32_t wanted);
    void twin_copy(const std::shared_ptr<HostTexture>& depth, const Twin& twin, bool colour);
    void pass_plane(const std::shared_ptr<HostTexture>& depth, const Twin& twin);
    void twins_to_depth();

    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12GraphicsCommandList6> list6_;
    danzi::FastLock lock_;
    Deferred deferred_;
    std::atomic<bool> uav_pending_{true};
    void bump_epoch();
    std::atomic<std::uint64_t> epoch_{0};


    ComPtr<ID3D12Resource> budget_;
    std::uint8_t* budget_data_ = nullptr;
    danzi::FastLock indirect_lock_;
    ComPtr<ID3D12Resource> indirect_args_;
    std::uint64_t indirect_cap_ = 0;


    struct State {
        std::uint32_t first, last;
        std::shared_ptr<HostTexture> copy;
    };
    std::mutex states_lock_;
    std::unordered_map<const HostTexture*, State> states_;
    std::mutex twins_lock_;
    std::unordered_map<const Twin*, std::unique_ptr<TwinUse>> twins_;


    std::optional<gpu::Prelude> prelude_;
    bool prelude_made_ = false;
};


struct Detached {
    std::unique_ptr<HostList> host;
    std::vector<std::unique_ptr<std::uint8_t[]>> uploads;

    ListState state;
};

struct Taken {
    std::vector<Detached> lists;
    std::vector<std::size_t> order;
};


struct ValueWait {
    std::uint64_t address;
    std::uint32_t comparison;
    std::uint64_t reference;
    bool wide;

    bool met() const;
    bool holds_for(std::uint64_t value) const;
};

struct Sent;

class Host {
public:
    explicit Host(std::unique_ptr<gpu::Device> device);
    ~Host();

    gpu::Device& device() { return *device_; }
    gpu::CardHost& card() { return *card_; }
    gpu::Registry& registry() { return *registry_; }


    std::unique_ptr<HostList> new_list(HRESULT* hr = nullptr);


    void execute(std::span<HostList* const> runs);


    void execute_late(Taken lists);


    std::uint64_t execute_own(HostList& list);

    static std::uint64_t finished();


    void settle(int why);

    void relay_land(std::vector<struct LateItem> items);


    ID3D12Fence* fence();


    D3D12_CPU_DESCRIPTOR_HANDLE rtv(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc);


    D3D12_CPU_DESCRIPTOR_HANDLE view_of(HostTexture& t, bool depth, const std::array<std::uint32_t, 6>* desc);
    void free_kept_view(bool depth, std::uint64_t handle);

    std::mutex& submission_lock() { return submitting_; }

private:
    std::optional<Sent> submit(std::span<HostList* const> runs, bool late);
    struct Sink;
    std::unique_ptr<gpu::Device> device_;
    std::unique_ptr<gpu::CardHost> card_;
    std::unique_ptr<gpu::Registry> registry_;
    std::unique_ptr<Sink> sink_;
    ComPtr<ID3D12DescriptorHeap> rtvs_, dsvs_;
    UINT rtv_size_ = 0, dsv_size_ = 0;
    std::atomic<std::uint32_t> next_rtv_{0}, next_dsv_{0};

    ComPtr<ID3D12DescriptorHeap> kept_[2];
    std::mutex kept_lock_;
    std::vector<std::uint64_t> kept_free_[2];

    std::mutex submitting_;
};


Host* host();


bool late();
bool late_free_present();


bool late_signal(This fence, std::uint64_t value);


bool late_met(const ValueWait& wait);


void settle_for_present();
void settle_for_fence();


void copy_mapped(std::uint8_t* dst, const std::uint8_t* src, usize len, bool title_dst);

bool committed(const void* at, usize len);


std::uint64_t mapped_bytes(const void* at, usize len);


std::uint64_t now_ms();
bool long_ago(std::uint64_t moment, std::uint64_t ms, std::uint64_t frames);
std::uint64_t ram_idle_ms();


void retire(std::shared_ptr<void> object);


void dispose(std::vector<std::shared_ptr<void>> batch);


void settle_disposals();


bool barrier_merge();


std::array<std::uint64_t, 2> windows_let_go();


void name_pipeline(ID3D12PipelineState* pso, const std::string& name);


void memcopy_line(double frames);


std::array<std::uint64_t, 2> take_mesh_groups();


bool trail_on();
void trail_note(std::string what);

template <class What>
void trail(What&& what) {
    if (trail_on()) {
        trail_note(what());
    }
}

std::uint64_t now_us();
std::uint32_t thread_id();


std::string removal_context();
void log_removal_context();


[[noreturn]] void fail(const char* what);


using FailureReport = void (*)(const char* text);
void on_failure(FailureReport report);

}   
