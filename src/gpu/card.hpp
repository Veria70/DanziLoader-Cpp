// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <set>
#include <tuple>
#include <vector>

#include "core/shadow.hpp"
#include "device.hpp"

namespace danzi::gpu {


inline constexpr usize CARD_INLINE_HEADER = 65536;
inline constexpr usize CARD_INLINE_STRIDE = 65536 + 64;
inline constexpr std::uint32_t CARD_INLINE_SLOTS = 2048;


bool card_inline_fetch();


bool rehash_runs();


bool wide_merge();


std::uint64_t card_inline_budget();


bool card_vram_ring();


bool card_pipeline();


bool card_copy_queue();


bool card_relay_copy_queue();


bool card_results_in_vram();


std::uint64_t card_budget_percent();

std::uint8_t late_stage();

bool relay();


struct GpuWrite {
    shadow::Shadow* shadow;
    usize address;
    usize len;
};


struct CardRange {
    shadow::Shadow* shadow;
    shadow::Range declared;
    shadow::CardSpan span;
    std::uint32_t at;   
    bool compared;      
};


struct CardPlan {
    std::vector<CardRange> ranges;
    ComPtr<ID3D12GraphicsCommandList> check;      
    ComPtr<ID3D12GraphicsCommandList> download;   
    std::uint64_t sent_as = 0;
    std::vector<shadow::Shadow*> synced;
    std::vector<std::pair<int, Prelude>> lists;   
};


struct Flight {
    std::optional<CardPlan> card;
    std::vector<shadow::Declared> back;
    std::uint64_t fence = 0;
    Prelude title;
    ComPtr<ID3D12Resource> title_source;
    bool names(const shadow::Shadow* s) const {
        return std::any_of(back.begin(), back.end(), [&](const shadow::Declared& d) { return d.first == s; });
    }
};

class CardHost {
public:
    explicit CardHost(Device& device);
    ~CardHost();
    CardHost(const CardHost&) = delete;
    CardHost& operator=(const CardHost&) = delete;

    Device& device() { return device_; }


    bool card_ready();

    std::uint64_t own_vram();


    ComPtr<ID3D12Resource> card_for(std::uint64_t size, bool small_ok = false);


    ComPtr<ID3D12Resource> window_card_for(std::uint64_t size);


    std::unique_ptr<shadow::Shadow> make_carded(std::uint64_t start, std::uint64_t len);


    std::optional<CardPlan> card_before(std::span<const GpuWrite> writes, bool late);


    using Seen = std::set<std::tuple<usize, usize, usize>>;
    void card_back(const CardPlan& plan, Seen* seen = nullptr);
    void card_done(CardPlan plan);
    bool card_results_low();


    void relay_card_back(std::span<const CardPlan* const> plans);


    std::pair<Flight, std::array<std::uint64_t, 2>> send(std::span<const GpuWrite> writes, Prelude title, ComPtr<ID3D12Resource> source,
                                                          std::vector<shadow::Declared> back);


    std::array<std::uint64_t, 2> land(std::vector<Flight> flights);


    std::array<std::uint64_t, 3> submit(std::span<const GpuWrite> writes, ID3D12CommandList* title, std::span<const shadow::Declared> back);

private:
    struct Work;
    struct RelayFetch;
    bool make_work();
    std::optional<Prelude> prelude();
    std::unique_ptr<RelayFetch> make_relay_fetch();
    std::optional<Prelude> record_download(Work& work, bool from_zero);

    Device& device_;
    std::mutex work_lock_;
    std::unique_ptr<Work> work_;
    std::once_flag ready_once_;
    bool ready_ = false;
    std::unique_ptr<RelayFetch> relay_fetch_;   
};


std::uint64_t queue_order();

}   
