// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/descriptor/host.rs (DanziLoader /
// victory-perf, see NOTICE.md): host descriptor heaps behind the title's
// shader-visible heaps. Translated shaders index the heaps directly at the
// title's descriptor positions, so each host heap keeps the title heap's
// order: slot `i` is made from what the title wrote at slot `i`, when it
// writes it (view creation and descriptor copies) and again when a draw
// reads it through a table and finds the slot changed. View heaps have
// dynamic slots past the title's for tables made afresh.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/win.hpp"
#include "descriptor.hpp"
#include "object.hpp"

namespace danzi::shadow {
class Shadow;
}

namespace danzi::d3d12x {

using Microsoft::WRL::ComPtr;


constexpr std::uint32_t DYNAMIC = 1u << 18;


void probe_view_limit();

std::uint32_t view_heap_limit();


class Spans {
public:
    std::optional<std::uint64_t> bytes(const std::array<std::uint32_t, 14>& desc, std::uint32_t first);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> overlapping(std::uint64_t start, std::uint64_t end) const;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> against(std::uint64_t start, std::uint64_t end, const std::array<std::uint32_t, 14>& desc) const;
    std::vector<std::uint32_t> note(std::uint32_t index, std::uint64_t start, std::uint64_t end, const std::array<std::uint32_t, 14>& desc, bool newer);
    void forget(std::uint32_t index);
    enum class Claim { Own, Free, SameStart, Other };
    std::pair<Claim, std::optional<std::pair<std::uint64_t, std::uint64_t>>> claim(std::uint32_t index, std::uint64_t start, std::uint64_t end,
                                                                                   const std::array<std::uint32_t, 14>& desc) const;
    std::vector<std::uint32_t> slots_in(std::uint64_t start, std::uint64_t end) const;

private:
    using Key = std::pair<std::uint64_t, std::uint64_t>;
    struct Span {
        std::array<std::uint32_t, 14> desc;
        std::set<std::uint32_t> slots;
    };
    std::map<Key, Span> spans_;
    std::unordered_map<std::uint32_t, Key> by_slot_;
    struct SizeKey {
        std::array<std::uint32_t, 14> desc;
        std::uint32_t first;
        bool operator==(const SizeKey&) const = default;
    };
    struct SizeHash {
        std::size_t operator()(const SizeKey& k) const;
    };
    std::unordered_map<SizeKey, std::optional<std::uint64_t>, SizeHash> sizes_;
};


struct Recheck {
    std::uint64_t due;
    std::pair<std::uint64_t, std::uint64_t> at;
    std::uint64_t print;
    std::uint8_t looks;
    std::uint64_t uploaded;
    bool operator==(const Recheck&) const = default;
};


class TextureQueue {
public:
    struct Waiting {
        std::uint64_t due, written, write;
        std::uint8_t looks;
        std::uint64_t mapped;
    };
    void written(std::uint32_t index, std::uint64_t frame, std::uint64_t due, bool seen);
    void remove(std::uint32_t index);
    void recheck(std::uint32_t index, std::uint64_t frame, std::pair<std::uint64_t, std::uint64_t> at, std::uint64_t print);
    std::vector<std::pair<std::uint32_t, Recheck>> rechecks_due(std::uint64_t frame, std::size_t max) const;
    void rechecked(std::uint32_t index, const Recheck& seen, bool changed, std::uint64_t frame);
    std::vector<std::pair<std::uint32_t, std::uint64_t>> due(std::uint64_t frame, std::size_t max) const;
    void done(std::uint32_t index, std::uint64_t write);
    void later(std::uint32_t index, std::uint64_t write, std::uint64_t frame, std::uint64_t most);
    void partly(std::uint32_t index, std::uint64_t write, std::uint64_t due, std::uint64_t mapped);
    void changed(std::uint32_t index, std::uint64_t frame, std::uint64_t due);
    std::uint64_t mapped(std::uint32_t index) const;
    std::array<std::uint64_t, 5> census(std::uint64_t frame) const;

    std::map<std::uint32_t, Waiting> slots;
    std::map<std::uint32_t, Recheck> rechecks;

private:
    Waiting* same(std::uint32_t index, std::uint64_t write);
    std::uint64_t writes_ = 0;
};


bool is_texture_view(const View& view);

std::optional<std::pair<std::uint64_t, std::uint64_t>> buffer_range(const View& view);

std::optional<std::pair<std::uint64_t, std::uint64_t>> buffer_extent(const View& view);

struct BufferCopy {
    std::shared_ptr<shadow::Shadow> shadow;
    std::uint64_t address, len;
};
std::optional<BufferCopy> buffer_copy(const View& view);


std::optional<View> placed_texture(const View& view);


void remapped(std::uintptr_t at, std::size_t len);

class HostHeap {
public:
    static std::unique_ptr<HostHeap> make(bool sampler, std::uint32_t count);
    ~HostHeap();

    ID3D12DescriptorHeap* heap() const { return heap_.Get(); }


    std::optional<std::uint32_t> dynamic(std::uint32_t n);
    void release(std::uint32_t first, std::uint32_t n);

    std::uint32_t count() const { return count_; }
    bool sampler() const { return sampler_; }
    std::optional<std::uint64_t> strides_gpu() const { return strides_ ? std::optional(strides_gpu_) : std::nullopt; }
    ID3D12Resource* strides_buffer() const { return strides_.Get(); }


    std::pair<std::vector<std::pair<std::uint32_t, std::uint64_t>>, std::array<std::uint64_t, 5>> texture_slots_due(std::uint64_t frame, std::size_t max);
    std::vector<std::pair<std::uint32_t, bool>> texture_slots_all_due(std::uint64_t frame);
    void texture_slot_done(std::uint32_t index, std::uint64_t write, std::optional<std::pair<std::uint64_t, std::uint64_t>> again);
    void texture_slot_recheck(std::uint32_t index, std::uint64_t frame, std::pair<std::uint64_t, std::uint64_t> at, std::uint64_t print);
    std::array<std::uint64_t, 3> texture_slots_rechecked(std::uint64_t frame);
    void texture_slot_later(std::uint32_t index, std::uint64_t write, std::uint64_t frame, std::uint64_t most);
    std::uint64_t texture_slot_mapped(std::uint32_t index);
    std::size_t texture_slots_remapped(std::uint64_t frame);
    bool texture_slot_made_from(std::uint32_t index, std::uint64_t handle);
    void texture_sweep(DescHeap& heap, std::uint64_t frame);
    bool texture_slot_gone(std::uint32_t index);
    void texture_slot_tabled(std::uint32_t index, bool read);

    std::vector<std::uint32_t> buffer_slots();
    std::vector<std::uint32_t> buffer_slots_bucket(std::uint32_t bucket);
    std::vector<std::uint32_t> buffer_changes();

    void make_at(std::uint32_t index, const View& view);
    void make_null(std::uint32_t index);
    std::uint64_t evict_kept(const std::unordered_set<std::uintptr_t>& victims);
    void raw_at(std::uint32_t index, ID3D12Resource* buffer, std::uint64_t offset, std::uint64_t len);
    void null_raw_at(std::uint32_t index);
    void raw_uav_at(std::uint32_t index, ID3D12Resource* buffer, std::uint64_t offset, std::uint64_t len);
    void null_raw_uav_at(std::uint32_t index);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(std::uint32_t index) const { return {gpu_ + std::uint64_t{index} * step_}; }
    void constant_at(std::uint32_t index, std::uint64_t gpu, std::uint32_t size);


    struct BufferMirror {
        enum Kind { Unchanged, Changed, Empty } kind;
        std::uint64_t key = 0;
        std::optional<View> view;
    };
    BufferMirror mirror_buffer(std::uint32_t index, std::uint64_t handle, std::uint64_t cached_key);


    std::optional<View> mirror(std::uint32_t index, std::uint64_t handle, bool for_draw);

private:
    HostHeap() = default;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(std::uint32_t index) const { return {static_cast<SIZE_T>(cpu_ + std::size_t{index} * step_)}; }
    void fill_null(D3D12_DESCRIPTOR_HEAP_TYPE kind, std::uint32_t total);
    void null_sampler(std::uint32_t index);
    std::vector<std::uint32_t> note_buffer(std::uint32_t index, const View* view, bool seen);
    void keep(std::uint32_t index, ComPtr<ID3D12Resource> resource);
    void unmake(std::vector<std::uint64_t>& made, std::uint32_t index);
    void tabled_in(std::uint32_t index, bool read, std::uint64_t frame);
    bool tabled_lately(std::uint32_t index, std::uint64_t frame) const;
    std::uint32_t tabled_stamp(std::uint32_t index) const;
    void queue_texture(std::uint32_t index, bool texture, bool seen, std::uint64_t frame);
    void sweep_part(std::uint32_t from, std::uint32_t to, DescHeap& heap, std::uint64_t frame, bool put_back, std::array<std::uint64_t, 10>& counts);

    ComPtr<ID3D12DescriptorHeap> heap_;
    std::uintptr_t cpu_ = 0;
    std::uint64_t gpu_ = 0;
    std::uint32_t step_ = 0;
    std::uint32_t count_ = 0;
    std::uint32_t dynamic_ = 0;
    bool sampler_ = false;
    danzi::FastLock made_lock_;
    std::vector<std::uint64_t> made_;
    danzi::FastLock kept_lock_;
    std::vector<ComPtr<ID3D12Resource>> kept_;
    danzi::FastLock next_lock_;
    std::uint32_t next_ = 0;
    std::vector<std::uint8_t> taken_;
    std::atomic<std::uint32_t> free_{DYNAMIC - 1};
    std::atomic<std::uint32_t> too_long_{UINT32_MAX};
    ComPtr<ID3D12Resource> strides_;
    std::uint64_t strides_gpu_ = 0;
    std::uint32_t* strides_data_ = nullptr;
    std::array<std::unordered_set<std::uint32_t>, 64> buffers_;
    std::unordered_set<std::uint32_t> changed_;


    danzi::FastLock notes_lock_;
    TextureQueue textures_;
    std::unique_ptr<std::atomic<std::uint32_t>[]> tabled_;
    std::uint32_t tabled_count_ = 0;
    Spans placed_;
};


std::array<std::uint64_t, 8> slot_counts();

}   
