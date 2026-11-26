// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the host copies of title memory, crates/victory-gpu/src/shadow.rs
// (DanziLoader / victory-perf, see NOTICE.md), and of the host's landing of
// shader writes (`hand_back`, `relay_hand_back` in host.rs).
//
// A title buffer's host copy. The ranges each submission reads are brought
// up to date from title memory before it runs: the 64 KiB pages the title
// wrote since they were last copied (`watch`), all of it where neither
// write watching nor guards work. Ranges shaders may write (`writing`) are
// left alone by every refresh; their bytes before the submission are kept
// (the snapshot), and after it what differs goes back into title memory,
// only those bytes (`copy_back_with`, or `relay_merge` with the relay).
//
// Ported: milestones 1 and 2 (everything a copy in the PC's memory does),
// and milestone 3's copies in the card's memory, their CPU side (`Card`,
// `card_dirty`, `card_merge`; the device side is `gpu/card.*`).
// Not yet: in-place copies, windows outside buffers, the
// `VICTORY_REFRESH_CHECK=1` diagnostic and the frame-line counters.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "win.hpp"

namespace danzi::shadow {

using Range = std::pair<usize, usize>;   
using Piece = std::pair<usize, usize>;   


bool relay();

bool keeps();

bool wide_back();

void preparing(std::uint64_t number);

inline constexpr std::uint8_t WATCH_UNKNOWN = 0;
inline constexpr std::uint8_t WATCHED = 1;
inline constexpr std::uint8_t NOT_WATCHED = 2;
inline constexpr std::uint8_t VIEW_GUARDED = 3;


inline constexpr usize PIECE = usize{64} << 10;


struct CopiedPages {
    std::vector<std::uint64_t> at;
    usize uncopied;

    explicit CopiedPages(usize pages) : at(pages, 0), uncopied(pages) {}
    void set(usize index, std::uint64_t now);
    bool all_copied() const { return uncopied == 0; }
};


struct Flights {
    std::vector<std::tuple<Range, std::uint32_t, std::uint64_t>> f;
    void declare(Range range, std::uint64_t number);
    std::vector<Range> done(std::span<const Range> ranges);
};


struct Snapshot {
    std::unique_ptr<std::uint8_t[]> bytes;
    usize capacity = 0;
};


using CopyObserver = void (*)(void* context, usize at, usize len);


bool cards();


std::pair<std::uint64_t, std::uint64_t> card_limits();


std::uint64_t card_vram_percent();

extern std::atomic<std::uint64_t> CARD_BYTES[2];

extern std::atomic<std::uint64_t> WINDOW_CARD_BYTES;


extern std::atomic<std::uint64_t> CARD_STATS[7];

std::array<std::uint64_t, 10> card_stats();


void back_line(double frames);

inline std::uint64_t card_hash_bytes(std::uint64_t size) { return (size / PIECE + 2) * 16; }


struct CardSpan {
    std::uint32_t from = 0, to = 0, first = 0, pieces = 0, skew = 0;
};

std::vector<CardSpan> joined_piece_runs(std::vector<CardSpan> runs);


struct Card {
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;   
    SrwLock before_lock;
    Microsoft::WRL::ComPtr<ID3D12Resource> before;   
    SrwLock dirty_lock;
    std::vector<Range> dirty;                                        
    std::vector<std::tuple<usize, usize, std::uint64_t>> sent;   
    std::uint64_t size = 0;
};

class Shadow;


std::vector<std::pair<Shadow*, std::vector<Range>>> card_dirty(std::uint64_t sent_as);

void relay_fetched(std::uint64_t number);


void warm_thread();

class Shadow {
public:

    Shadow(std::uint8_t* data, std::uint64_t start, std::uint64_t len);
    ~Shadow();
    Shadow(const Shadow&) = delete;
    Shadow& operator=(const Shadow&) = delete;

    std::uint64_t start() const { return start_; }

    std::uint64_t id() const { return id_; }


    std::atomic<std::uint64_t> used{0};
    std::uint64_t len() const { return len_; }
    std::uint8_t* data() const { return data_; }

    bool unwatched() const;
    bool is_view_guarded() const;

    bool carded() const { return card_ != nullptr; }


    void make_carded(Microsoft::WRL::ComPtr<ID3D12Resource> buffer, std::unique_ptr<Card> card);
    ID3D12Resource* buffer() const { return buffer_.Get(); }


    void hold_buffer(Microsoft::WRL::ComPtr<ID3D12Resource> buffer) { buffer_ = std::move(buffer); }

    void on_release(std::function<void()> f) { on_release_ = std::move(f); }

    bool window() const { return window_; }
    void set_window(bool w) { window_ = w; }


    void make_placed(Microsoft::WRL::ComPtr<ID3D12Resource> buffer, Microsoft::WRL::ComPtr<ID3D12Heap> heap, std::uint64_t bias, bool window);
    bool placed() const { return placed_; }
    std::uint64_t place_bias() const { return place_bias_; }

    std::uint64_t rebar() const { return rebar_; }
    void set_rebar(std::uint64_t bytes) { rebar_ = bytes; }


    void card_changed(usize from, usize to);
    struct CardSides {
        ID3D12Resource* staging;
        Microsoft::WRL::ComPtr<ID3D12Resource> before;
        std::uint64_t size;
    };
    std::optional<CardSides> card_sides() const;

    template <class Make>
    bool card_make_before(Make&& make) {
        if (!card_) {
            return false;
        }
        std::lock_guard lock(card_->before_lock);
        if (card_->before) {
            return false;
        }
        card_->before = make(card_->size);
        if (card_->before) {
            CARD_BYTES[1].fetch_add(card_hash_bytes(card_->size), std::memory_order_relaxed);
        }
        return card_->before != nullptr;
    }
    std::optional<CardSpan> card_span(usize address, usize end) const;
    CardSpan card_run(usize from, usize to) const;
    Range card_piece(const CardSpan& span, std::uint32_t i) const;


    usize card_merge(usize at, const std::uint8_t* gpu, usize n, std::vector<Range>* changed = nullptr);


    usize card_merge_relay(usize at, const std::uint8_t* gpu, usize n, std::uint64_t fetched_after);


    void writing(usize address, usize len);

    void copy_back_done(std::span<const Range> ranges);
    bool writes_pending() const;


    bool stale_under_writes(usize address, usize len) const;


    bool merge_under_writes(usize address, usize len, usize& runs_left);


    usize update(usize address, usize len);


    usize copy_back(usize address, usize len) { return copy_back_with(address, len, nullptr); }
    usize copy_back_with(usize address, usize len, const std::vector<Piece>* found);


    usize relay_merge(usize at, usize n, std::vector<std::uint8_t>& now, std::vector<std::uint8_t>& title);


    Range inside(usize address, usize len) const;

    void set_copy_observer(CopyObserver observer, void* context) {
        observer_ = observer;
        observer_context_ = context;
    }

    struct State {
        std::uint8_t mode;
        std::vector<std::uint64_t> at;
        usize uncopied;
        std::vector<Range> pending;
    };
    State state() const;


    std::optional<std::uint64_t> snapshot_hash(std::span<const Range> ranges) const;

private:
    friend std::vector<std::vector<Piece>> find_all(std::span<const std::tuple<Shadow*, usize, usize>> work);

    usize update_free(usize from, usize to);
    usize copy_stale(usize from, usize to, std::uint64_t now, std::span<const std::pair<usize, std::uint64_t>> written);
    usize copy(usize from, usize to);
    std::vector<Piece> differing(usize from, usize to, const std::uint8_t* before) const;


    std::pair<usize, usize> pieces(usize from, usize to) const;
    Range piece(usize p) const;


    void changed(usize from, usize to);

    std::uint8_t* data_;
    std::uint64_t start_;
    std::uint64_t id_;
    std::uint64_t len_;


    bool placed_ = false;
    mutable SrwLock copied_lock_;
    CopiedPages copied_;
    std::atomic<std::uint8_t> watched_{WATCH_UNKNOWN};
    mutable SrwLock pending_lock_;
    std::vector<Range> pending_;

    SrwLock flights_lock_;
    Flights flights_;

    mutable SrwLock before_lock_;
    std::optional<Snapshot> before_;


    SrwLock kept_lock_;
    std::vector<bool> kept_pieces_;
    bool kept_counted_ = false;
    CopyObserver observer_ = nullptr;
    void* observer_context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    std::unique_ptr<Card> card_;
    std::function<void()> on_release_;
    bool window_ = false;
    std::uint64_t rebar_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Heap> place_heap_;
    std::uint64_t place_bias_ = 0;

    friend std::vector<std::pair<Shadow*, std::vector<Range>>> card_dirty(std::uint64_t sent_as);
};


void take_snapshots();


std::vector<std::vector<Piece>> find_all(std::span<const std::tuple<Shadow*, usize, usize>> work);


using Declared = std::pair<Shadow*, std::vector<Range>>;


void hand_back(std::span<const Declared> back);


void relay_hand_back(std::span<const Declared> back);

}   
