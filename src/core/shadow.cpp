// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/shadow.rs (host copies of title memory) and
// of `hand_back`, `relay_hand_back`, `join_ranges` from host.rs.
#include "shadow.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "env.hpp"
#include "inplace.hpp"
#include "log.hpp"
#include "fastmem.hpp"
#include "title_copy.hpp"
#include "watch.hpp"
#include "wide.hpp"

namespace danzi::shadow {

namespace {


std::atomic<std::uint64_t> BACK_CLASS[7]{};


void compared(bool window, usize len, usize from, usize to) {
    usize bytes = window ? 2 : 3, ranges = window ? 5 : 6;
    BACK_CLASS[bytes].fetch_add(to - from, std::memory_order_relaxed);
    BACK_CLASS[ranges].fetch_add(1, std::memory_order_relaxed);
    if (!window && len >= usize{1} << 20) {
        BACK_CLASS[4].fetch_add(to - from, std::memory_order_relaxed);
    }
}

using watch::PAGE;


std::atomic<std::uint64_t> PREPARED{0};


std::atomic<std::uint64_t> KEPT_BYTES{0};


std::optional<std::uint64_t> env_u64(const wchar_t* name) {
    auto v = env::get(name);
    if (!v) {
        return std::nullopt;
    }
    std::wstring s = *v;
    auto b = s.find_first_not_of(L" \t\r\n");
    auto e = s.find_last_not_of(L" \t\r\n");
    s = b == std::wstring::npos ? L"" : s.substr(b, e - b + 1);
    if (!s.empty() && s[0] == L'+') {
        s = s.substr(1);
    }
    bool digits = !s.empty() && std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (!digits) {
        return std::nullopt;
    }
    std::uint64_t out = 0;
    for (wchar_t c : s) {
        std::uint64_t d = static_cast<std::uint64_t>(c - L'0');
        if (out > (~std::uint64_t{0} - d) / 10) {
            return std::nullopt;   
        }
        out = out * 10 + d;
    }
    return out;
}

std::uint64_t kept_most() {
    static const std::uint64_t most = env_u64(L"VICTORY_KEEP_SNAPSHOTS_MB").value_or(256) << 20;
    return most;
}


std::pair<usize, usize> pieces_of(usize start, usize from, usize to) {
    usize first = start / PIECE;
    return {(start + from) / PIECE - first, (start + to - 1) / PIECE - first + 1};
}


Range piece_at(usize start, usize len, usize piece) {
    usize first = start / PIECE;
    usize lo = saturating_sub((first + piece) * PIECE, start);
    usize hi = std::min((first + piece + 1) * PIECE - start, len);
    return {lo, hi};
}


SrwLock POOL_LOCK;
std::vector<Snapshot> POOL;

Snapshot pooled(usize n) {
    std::lock_guard lock(POOL_LOCK);
    usize most = std::max<usize>(n > ~usize{0} / 4 ? ~usize{0} : n * 4, usize{1} << 20);
    usize best = POOL.size();
    for (usize i = 0; i < POOL.size(); ++i) {
        usize c = POOL[i].capacity;
        if (c >= n && c <= most && (best == POOL.size() || c < POOL[best].capacity)) {
            best = i;
        }
    }
    if (best < POOL.size()) {
        Snapshot s = std::move(POOL[best]);

        POOL[best] = std::move(POOL.back());
        POOL.pop_back();
        return s;
    }
    Snapshot s;
    s.bytes = std::make_unique_for_overwrite<std::uint8_t[]>(std::max<usize>(n, 1));
    s.capacity = n;
    return s;
}


void give_back(Snapshot v) {
    constexpr usize KEPT = usize{512} << 20;
    std::lock_guard lock(POOL_LOCK);
    POOL.push_back(std::move(v));
    std::stable_sort(POOL.begin(), POOL.end(), [](const Snapshot& a, const Snapshot& b) { return a.capacity > b.capacity; });
    if (POOL.size() > 8) {
        POOL.resize(8);
    }
    usize seen = 0, total = 0;
    std::vector<Snapshot> keep;
    for (auto& s : POOL) {
        ++seen;
        total = saturating_add(total, s.capacity);
        if (seen == 1 || total <= KEPT) {
            keep.push_back(std::move(s));
        }
    }
    POOL = std::move(keep);
}


std::uint8_t* piece_scratch() {
    thread_local std::unique_ptr<std::uint8_t[]> room = std::make_unique_for_overwrite<std::uint8_t[]>(PIECE);
    return room.get();
}


constexpr usize WIDE_JOB = usize{1} << 20;


void snapshot_copy(void* dst, const void* src, usize n) { std::memcpy(dst, src, n); }


usize snapshot_job(usize total) { return std::clamp<usize>(total / (wide::threads() + 1), usize{256} << 10, WIDE_JOB); }

struct CopyPart {
    usize src, dst, len;
};


SrwLock SNAPS_LOCK;
std::vector<CopyPart> SNAPS;


std::vector<wide::Job> copy_jobs(const std::vector<CopyPart>& copies, usize job_bytes) {
    std::vector<wide::Job> jobs;
    std::vector<CopyPart> group;
    usize bytes = 0;
    auto flush = [&] {
        if (!group.empty()) {
            jobs.emplace_back([g = std::move(group)] {
                for (const auto& c : g) {
                    snapshot_copy(reinterpret_cast<void*>(c.dst), reinterpret_cast<const void*>(c.src), c.len);
                }
            });
            group.clear();
        }
    };
    for (const auto& c : copies) {
        usize at = 0;
        while (at < c.len) {
            usize n = std::min(job_bytes - bytes, c.len - at);
            group.push_back({c.src + at, c.dst + at, n});
            at += n;
            bytes += n;
            if (bytes >= job_bytes) {
                flush();
                bytes = 0;
            }
        }
    }
    flush();
    return jobs;
}


void copy_wide(const std::uint8_t* src, std::uint8_t* dst, usize len) {
    constexpr usize PER_THREAD = usize{16} << 20;
    usize threads = std::clamp<usize>(len / PER_THREAD, 1, 6);
    if (threads == 1) {
        snapshot_copy(dst, src, len);
        return;
    }
    usize each = div_ceil(len, threads);
    std::vector<wide::Job> jobs;
    for (usize i = 0; i < threads; ++i) {
        usize at = std::min(i * each, len);
        usize n = std::min(each, len - at);
        jobs.emplace_back([=] { snapshot_copy(dst + at, src + at, n); });
    }
    wide::run(jobs);
}

struct Scan {
    usize data = 0, before = 0, start = 0, from = 0, to = 0;
};


std::vector<std::vector<Piece>> find_pieces(const std::vector<Scan>& scans) {
    struct Part {
        usize which, place, at, next;
    };
    std::vector<std::vector<std::vector<Piece>>> found(scans.size());
    std::vector<wide::Job> jobs;
    std::vector<Part> group;
    usize bytes = 0;
    auto flush = [&] {
        if (group.empty()) {
            return;
        }
        jobs.emplace_back([&scans, &found, g = std::move(group)] {
            for (const Part& p : g) {
                const Scan& s = scans[p.which];
                auto& pieces = found[p.which][p.place];
                usize at = p.at;
                while (at < p.next) {
                    usize n = std::min(PIECE - (s.start + at) % PIECE, p.next - at);
                    if (!fastmem::equal(reinterpret_cast<const std::uint8_t*>(s.data + at), reinterpret_cast<const std::uint8_t*>(s.before + at), n)) {
                        pieces.emplace_back(at, n);
                    }
                    at += n;
                }
            }
        });
        group.clear();
    };
    for (usize which = 0; which < scans.size(); ++which) {
        const Scan& s = scans[which];
        usize at = s.from, place = 0;
        while (at < s.to) {

            usize room = std::max(WIDE_JOB - bytes, PIECE);
            usize next = std::min(at + room, s.to);
            if (next < s.to) {
                next -= (s.start + next) % PIECE;
                if (next <= at) {
                    next = std::min(at + PIECE - (s.start + at) % PIECE, s.to);
                }
            }
            group.push_back({which, place, at, next});
            bytes += next - at;
            ++place;
            at = next;
            if (bytes >= WIDE_JOB) {
                flush();
                bytes = 0;
            }
        }
        found[which].resize(place);
    }
    flush();
    wide::run(jobs);
    std::vector<std::vector<Piece>> out(scans.size());
    for (usize which = 0; which < scans.size(); ++which) {
        for (auto& part : found[which]) {
            out[which].insert(out[which].end(), part.begin(), part.end());
        }
    }
    return out;
}


void join_ranges(std::vector<Range>& ranges, std::span<const Range> more) {
    for (const auto& r : more) {
        if (r.first < r.second) {
            ranges.push_back(r);
        }
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<Range> out;
    out.reserve(ranges.size());
    for (const auto& [from, to] : ranges) {
        if (!out.empty() && from <= out.back().second) {
            out.back().second = std::max(out.back().second, to);
        } else {
            out.emplace_back(from, to);
        }
    }
    ranges = std::move(out);
}

}   


std::atomic<std::uint64_t> CARD_BYTES[2]{};
std::atomic<std::uint64_t> WINDOW_CARD_BYTES{0};
std::atomic<std::uint64_t> CARD_STATS[7]{};

namespace {


SrwLock CARDED_LOCK;
std::vector<Shadow*> CARDED;


std::atomic<std::uint64_t> RELAY_FETCHED{0};


std::vector<Range> joined(std::vector<Range>& ranges) {
    std::sort(ranges.begin(), ranges.end());
    std::vector<Range> out;
    out.reserve(ranges.size());
    for (const auto& [from, to] : ranges) {
        if (!out.empty() && from <= out.back().second) {
            out.back().second = std::max(out.back().second, to);
        } else {
            out.emplace_back(from, to);
        }
    }
    return out;
}


std::vector<Range> outside(usize from, usize to, std::vector<Range> skip) {
    std::sort(skip.begin(), skip.end());
    std::vector<Range> out;
    usize at = from;
    for (const auto& [a, b] : skip) {
        if (b <= at) {
            continue;
        }
        if (a >= to) {
            break;
        }
        if (a > at) {
            out.emplace_back(at, a);
        }
        at = std::max(at, b);
    }
    if (at < to) {
        out.emplace_back(at, to);
    }
    return out;
}

CardSpan card_span_of(usize start, usize from, usize to) {
    auto [a, b] = pieces_of(start, from, to);
    return {static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to), static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b - a),
            static_cast<std::uint32_t>(start % PIECE)};
}

Range card_piece_of(usize start, usize len, const CardSpan& span, std::uint32_t i) {
    auto [lo, hi] = piece_at(start, len, static_cast<usize>(span.first) + i);
    return {std::max<usize>(lo, span.from), std::min<usize>(hi, span.to)};
}

}   

bool cards() {
    static const bool on = env::not_set_or_not(L"VICTORY_CARD_BUFFERS", L"0");
    return on;
}

std::pair<std::uint64_t, std::uint64_t> card_limits() {
    static const std::pair<std::uint64_t, std::uint64_t> limits = [] {
        std::uint64_t least = std::max<std::uint64_t>(env_u64(L"VICTORY_CARD_BUFFERS_MIN_MB").value_or(1) << 20, std::uint64_t{1} << 20);
        std::uint64_t most = env_u64(L"VICTORY_CARD_BUFFERS_MB").value_or(1024) << 20;
        return std::make_pair(least, most);
    }();
    return limits;
}

std::uint64_t card_vram_percent() {
    static const std::uint64_t percent = [] {
        auto v = env_u64(L"VICTORY_CARD_VRAM_PERCENT");
        return v ? std::clamp<std::uint64_t>(*v, 50, 95) : std::uint64_t{92};
    }();
    return percent;
}

std::array<std::uint64_t, 10> card_stats() {
    std::array<std::uint64_t, 10> out{};
    for (usize i = 0; i < 7; ++i) {
        out[i] = CARD_STATS[i].exchange(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard lock(CARDED_LOCK);
        out[7] = CARDED.size();
    }
    out[8] = CARD_BYTES[0].load(std::memory_order_relaxed) + WINDOW_CARD_BYTES.load(std::memory_order_relaxed);
    out[9] = CARD_BYTES[1].load(std::memory_order_relaxed);
    return out;
}

std::vector<CardSpan> joined_piece_runs(std::vector<CardSpan> runs) {
    std::sort(runs.begin(), runs.end(), [](const CardSpan& a, const CardSpan& b) { return a.first < b.first; });
    std::vector<CardSpan> out;
    out.reserve(runs.size());
    for (const auto& run : runs) {
        if (!out.empty() && run.first <= out.back().first + out.back().pieces) {
            auto& last = out.back();
            last.pieces = std::max(last.pieces, run.first + run.pieces - last.first);
            last.to = std::max(last.to, run.to);
        } else {
            out.push_back(run);
        }
    }
    return out;
}

std::vector<std::pair<Shadow*, std::vector<Range>>> card_dirty(std::uint64_t sent_as) {
    std::vector<std::pair<Shadow*, std::vector<Range>>> out;
    std::lock_guard carded(CARDED_LOCK);
    std::uint64_t fetched = RELAY_FETCHED.load(std::memory_order_acquire);
    bool relayed = relay();


    thread_local std::vector<Range> ranges;
    for (Shadow* shadow : CARDED) {
        Card& c = *shadow->card_;
        {
            std::lock_guard dirty(c.dirty_lock);
            if (c.dirty.empty()) {
                if (relayed) {
                    std::erase_if(c.sent, [&](const auto& s) { return std::get<2>(s) <= fetched; });
                }
                continue;
            }
            ranges.assign(c.dirty.begin(), c.dirty.end());
            c.dirty.clear();
            if (relayed) {
                std::erase_if(c.sent, [&](const auto& s) { return std::get<2>(s) <= fetched; });
                for (const auto& [a, b] : ranges) {
                    c.sent.emplace_back(a, b, sent_as);
                }
            }
        }
        out.emplace_back(shadow, joined(ranges));
    }
    return out;
}

void warm_thread() {
    std::uint8_t* room = piece_scratch();
    for (usize i = 0; i < PIECE; i += 4096) {
        reinterpret_cast<volatile std::uint8_t*>(room)[i] = 0;
    }
}

void relay_fetched(std::uint64_t number) {
    std::uint64_t now = RELAY_FETCHED.load(std::memory_order_relaxed);
    while (now < number && !RELAY_FETCHED.compare_exchange_weak(now, number, std::memory_order_acq_rel)) {
    }
}

bool keeps() {
    static const bool on = env::is(L"VICTORY_KEEP_SNAPSHOTS", L"1");
    return on;
}

bool relay() {


    static const bool on = [] {
        auto v = env::get(L"VICTORY_RELAY");
        return (v ? (*v == L"1" || *v == L"2") : true) && !keeps();
    }();
    return on;
}

bool wide_back() {
    static const bool on = env::not_set_or_not(L"VICTORY_WIDE_BACK", L"0");
    return on;
}

void preparing(std::uint64_t number) { PREPARED.store(number, std::memory_order_relaxed); }

void CopiedPages::set(usize index, std::uint64_t now) {
    if (index >= at.size()) {
        return;
    }
    std::uint64_t& slot = at[index];
    if (slot == 0 && now != 0) {
        uncopied -= 1;
    } else if (slot != 0 && now == 0) {
        uncopied += 1;
    }
    slot = now;
}

void Flights::declare(Range range, std::uint64_t number) {
    for (auto& [r, count, last] : f) {
        if (r == range) {
            if (last != number) {
                count += 1;
                last = number;
            }
            return;
        }
    }
    f.emplace_back(range, 1, number);
}

std::vector<Range> Flights::done(std::span<const Range> ranges) {
    std::vector<Range> free;
    for (const Range& range : ranges) {
        auto it = std::find_if(f.begin(), f.end(), [&](const auto& e) { return std::get<0>(e) == range; });
        if (it == f.end()) {

            free.push_back(range);
            continue;
        }
        auto& count = std::get<1>(*it);
        count = count > 0 ? count - 1 : 0;
        if (count == 0) {
            *it = f.back();   
            f.pop_back();
            free.push_back(range);
        }
    }
    return free;
}

std::atomic<std::uint64_t> SHADOW_IDS{0};

Shadow::Shadow(std::uint8_t* data, std::uint64_t start, std::uint64_t len)
    : data_(data), start_(start), id_(SHADOW_IDS.fetch_add(1, std::memory_order_relaxed) + 1), len_(len), copied_(static_cast<usize>(div_ceil(start + len, PAGE) - start / PAGE)) {}

Shadow::~Shadow() {
    if (placed_) {

        inplace::gone(window_, len_);
        return;
    }
    if (on_release_) {
        on_release_();
    }
    if (kept_counted_) {
        KEPT_BYTES.fetch_sub(len_, std::memory_order_relaxed);
    }
    if (card_) {
        {
            std::lock_guard lock(CARDED_LOCK);
            std::erase(CARDED, this);
        }
        (window_ ? WINDOW_CARD_BYTES : CARD_BYTES[0]).fetch_sub(card_->size, std::memory_order_relaxed);
        if (card_->before) {
            CARD_BYTES[1].fetch_sub(card_hash_bytes(card_->size), std::memory_order_relaxed);
        }
    }
}

void Shadow::make_placed(Microsoft::WRL::ComPtr<ID3D12Resource> buffer, Microsoft::WRL::ComPtr<ID3D12Heap> heap, std::uint64_t bias,
                         bool window) {
    buffer_ = std::move(buffer);
    place_heap_ = std::move(heap);
    place_bias_ = bias;
    placed_ = true;
    window_ = window;

    watched_.store(NOT_WATCHED, std::memory_order_relaxed);
}

void Shadow::make_carded(Microsoft::WRL::ComPtr<ID3D12Resource> buffer, std::unique_ptr<Card> card) {
    buffer_ = std::move(buffer);
    card_ = std::move(card);
    std::lock_guard lock(CARDED_LOCK);
    CARDED.push_back(this);
}

void Shadow::card_changed(usize from, usize to) {
    if (!card_ || from >= to) {
        return;
    }
    std::lock_guard lock(card_->dirty_lock);
    auto& dirty = card_->dirty;
    if (!dirty.empty() && from <= dirty.back().second && to >= dirty.back().first) {
        dirty.back() = {std::min(dirty.back().first, from), std::max(dirty.back().second, to)};
    } else {
        dirty.emplace_back(from, to);
    }
}

usize Shadow::card_merge_relay(usize at, const std::uint8_t* gpu, usize n, std::uint64_t fetched_after) {
    usize start = start_;
    if (!card_ || at + n > len_) {
        return 0;
    }


    std::lock_guard side(before_lock_);
    std::uint8_t* held = data_ + at;
    auto first = fastmem::first_difference(gpu, held, n);
    if (!first) {
        return 0;
    }
    usize last = fastmem::last_difference(gpu, held, n).value_or(*first);
    usize at2 = at + *first;
    usize n2 = last + 1 - *first;
    std::vector<Range> skip;
    {
        std::lock_guard dirty(card_->dirty_lock);
        for (const auto& [a, b] : card_->dirty) {
            if (a < at2 + n2 && b > at2) {
                skip.emplace_back(a, b);
            }
        }
        for (const auto& [a, b, sent] : card_->sent) {
            if (sent > fetched_after && a < at2 + n2 && b > at2) {
                skip.emplace_back(a, b);
            }
        }
    }
    std::vector<std::uint8_t> longer;
    std::uint8_t* title = n2 <= PIECE ? piece_scratch() : (longer.resize(n2), longer.data());
    bool mapped = host::read_into(reinterpret_cast<const std::uint8_t*>(start + at2), title, n2);
    bool changed = false;
    const std::uint8_t* now = gpu + *first;
    std::uint8_t* kept = held + *first;
    for (const auto& [a, b] : outside(at2, at2 + n2, std::move(skip))) {
        usize o = a - at2;


        changed |= fastmem::merge_piece(now + o, kept + o, title + o, b - a);
    }
    if (!mapped || !changed) {
        return 0;
    }
    watch::writing(start + at2, n2);
    host::write_mapped(reinterpret_cast<std::uint8_t*>(start + at2), title, n2);
    return n2;
}

std::optional<Shadow::CardSides> Shadow::card_sides() const {
    if (!card_) {
        return std::nullopt;
    }
    std::lock_guard lock(card_->before_lock);
    return CardSides{card_->staging.Get(), card_->before, card_->size};
}

std::optional<CardSpan> Shadow::card_span(usize address, usize end) const {
    auto [from, to] = inside(address, saturating_sub(end, address));
    if (from >= to) {
        return std::nullopt;
    }
    return card_span_of(start_, from, to);
}

CardSpan Shadow::card_run(usize from, usize to) const {
    to = std::min<usize>(to, len_);
    return card_span_of(start_, std::min(from, saturating_sub(to, 1)), to);
}

Range Shadow::card_piece(const CardSpan& span, std::uint32_t i) const { return card_piece_of(start_, len_, span, i); }

usize Shadow::card_merge(usize at, const std::uint8_t* gpu, usize n, std::vector<Range>* changed) {
    usize start = start_;
    if (!card_ || at + n > len_) {
        return 0;
    }
    std::uint8_t* old = data_ + at;
    auto first = fastmem::first_difference(gpu, old, n);
    if (!first) {
        return 0;
    }
    usize last = fastmem::last_difference(gpu, old, n).value_or(*first);
    usize at2 = at + *first;
    usize n2 = last + 1 - *first;
    usize written = 0;

    std::vector<std::uint8_t> longer;
    std::uint8_t* title = n2 <= PIECE ? piece_scratch() : (longer.resize(n2), longer.data());
    if (host::read_into(reinterpret_cast<const std::uint8_t*>(start + at2), title, n2)) {


        if (fastmem::merge_card(title, gpu + *first, old + *first, n2)) {
            watch::writing(start + at2, n2);
            host::write_mapped(reinterpret_cast<std::uint8_t*>(start + at2), title, n2);
            written = n2;
        }
    } else {

        std::memcpy(old + *first, gpu + *first, n2);
    }

    if (changed) {
        changed->emplace_back(at2, at2 + n2);
    } else {
        card_changed(at2, at2 + n2);
    }
    return written;
}

bool Shadow::unwatched() const { return watched_.load(std::memory_order_relaxed) == NOT_WATCHED; }

bool Shadow::is_view_guarded() const { return watched_.load(std::memory_order_relaxed) == VIEW_GUARDED; }

Range Shadow::inside(usize address, usize len) const {
    usize start = start_;
    usize end = start_ + len_;
    usize from = std::clamp(address, start, end);
    return {from - start, std::clamp(address + len, from, end) - start};
}

std::pair<usize, usize> Shadow::pieces(usize from, usize to) const { return pieces_of(start_, from, to); }

Range Shadow::piece(usize p) const { return piece_at(start_, len_, p); }

void Shadow::changed(usize from, usize to) {
    if (!keeps() || from >= to) {
        return;
    }
    std::lock_guard kept(kept_lock_);
    if (kept_pieces_.empty()) {
        return;
    }
    auto [a, b] = pieces(from, to);
    for (usize p = a; p < b; ++p) {
        kept_pieces_[p] = false;
    }
}

void Shadow::writing(usize address, usize len) {
    if (placed_) {
        return;
    }
    Range range{address, address + len};
    std::lock_guard pending(pending_lock_);


    if (relay()) {
        std::lock_guard flights(flights_lock_);
        flights_.declare(range, PREPARED.load(std::memory_order_relaxed));
    }

    if (std::find(pending_.begin(), pending_.end(), range) != pending_.end()) {
        return;
    }


    if (card_) {
        pending_.push_back(range);
        return;
    }

    std::vector<Range> parts{inside(address, len)};
    for (const auto& [a0, e0] : pending_) {
        auto [a, e] = inside(a0, e0 - a0);
        std::vector<Range> rest;
        rest.reserve(parts.size() + 1);
        for (const auto& [from, to] : parts) {
            if (e <= from || a >= to) {
                rest.emplace_back(from, to);
                continue;
            }
            if (from < a) {
                rest.emplace_back(from, a);
            }
            if (e < to) {
                rest.emplace_back(e, to);
            }
        }
        parts = std::move(rest);
    }
    pending_.push_back(range);
    usize taken = 0;
    for (const auto& [from, to] : parts) {
        taken += saturating_sub(to, from);
    }
    BACK_CLASS[window_ ? 0 : 1].fetch_add(taken, std::memory_order_relaxed);
    usize total = len_;
    std::lock_guard before(before_lock_);
    if (!before_) {
        before_ = pooled(total);
    }
    std::uint8_t* snapshot = before_->bytes.get();
    if (keeps()) {

        std::lock_guard kept(kept_lock_);
        if (kept_pieces_.empty()) {
            kept_pieces_.assign(pieces(0, std::max<usize>(total, 1)).second, false);
        }
        for (const auto& [from, to] : parts) {
            if (from >= to) {
                continue;
            }
            auto [pa, pb] = pieces(from, to);
            for (usize p = pa; p < pb; ++p) {
                if (kept_pieces_[p]) {
                    continue;
                }
                auto [lo, hi] = piece(p);
                bool whole = from <= lo && hi <= to;
                lo = std::max(lo, from);
                hi = std::min(hi, to);
                std::memcpy(snapshot + lo, data_ + lo, hi - lo);
                kept_pieces_[p] = whole;
            }
        }
        return;
    }
    if (wide_back()) {


        std::lock_guard snaps(SNAPS_LOCK);
        for (const auto& [from, to] : parts) {
            if (from < to) {
                SNAPS.push_back({reinterpret_cast<usize>(data_) + from, reinterpret_cast<usize>(snapshot) + from, to - from});
            }
        }
        return;
    }
    for (const auto& [from, to] : parts) {
        if (from < to) {
            copy_wide(data_ + from, snapshot + from, to - from);
        }
    }
}

void Shadow::copy_back_done(std::span<const Range> ranges) {
    if (placed_) {
        return;
    }
    std::lock_guard pending(pending_lock_);
    auto keep_if_not_in = [&](const std::vector<Range>& gone) {
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&](const Range& r) { return std::find(gone.begin(), gone.end(), r) != gone.end(); }),
                       pending_.end());
    };
    if (relay()) {
        std::vector<Range> free;
        {
            std::lock_guard flights(flights_lock_);
            free = flights_.done(ranges);
        }
        keep_if_not_in(free);
    } else {
        keep_if_not_in(std::vector<Range>(ranges.begin(), ranges.end()));
    }
    if (!pending_.empty()) {
        return;
    }
    if (keeps()) {


        std::lock_guard kept(kept_lock_);
        if (!kept_counted_ && len_ >= (std::uint64_t{1} << 20)) {
            if (KEPT_BYTES.fetch_add(len_, std::memory_order_relaxed) + len_ <= kept_most()) {
                kept_counted_ = true;
            } else {
                KEPT_BYTES.fetch_sub(len_, std::memory_order_relaxed);
            }
        }
        if (kept_counted_) {
            return;
        }

        kept_pieces_.clear();
    }
    std::optional<Snapshot> gone;
    {
        std::lock_guard before(before_lock_);
        gone.swap(before_);
    }
    if (gone) {
        give_back(std::move(*gone));
    }
}

bool Shadow::writes_pending() const {
    std::lock_guard pending(pending_lock_);
    return !pending_.empty();
}

bool Shadow::stale_under_writes(usize address, usize len) const {
    usize start = start_;
    usize end = start_ + len_;
    usize from = std::max(address, start);
    usize to = std::min(address + len, end);
    if (from >= to) {
        return false;
    }
    std::vector<Range> busy;
    {
        std::lock_guard pending(pending_lock_);
        for (const auto& [a, e] : pending_) {
            if (a < to && e > from) {
                busy.emplace_back(std::max(a, from), std::min(e, to));
            }
        }
    }
    if (busy.empty()) {
        return false;
    }
    if (watched_.load(std::memory_order_relaxed) == NOT_WATCHED) {
        return true;
    }
    usize base = start / PAGE;
    std::lock_guard copied(copied_lock_);
    for (const auto& [a, e] : busy) {
        auto asked = watch::asked(a, e);
        if (!asked) {
            return true;
        }
        for (const auto& [page, write] : asked->written) {
            if (page < base) {
                continue;
            }
            usize i = page - base;
            if (i >= copied_.at.size() || copied_.at[i] == 0 || write > copied_.at[i]) {
                return true;
            }
        }
    }
    return false;
}

bool Shadow::merge_under_writes(usize address, usize len, usize& runs_left) {
    usize start = start_;
    usize end = start_ + len_;
    usize from = std::max(address, start);
    usize to = std::min(address + len, end);
    if (from >= to) {
        return false;
    }
    std::vector<Range> busy;
    {
        std::lock_guard pending(pending_lock_);
        for (const auto& [a, e] : pending_) {
            if (a < to && e > from) {
                busy.emplace_back(std::max(a, from), std::min(e, to));
            }
        }
    }
    if (busy.empty()) {
        return false;
    }
    if (watched_.load(std::memory_order_relaxed) == NOT_WATCHED) {
        return true;
    }
    usize base = start / PAGE;
    std::uint8_t* title = piece_scratch();   
    for (const auto& [a, e] : busy) {


        auto w = watch::writes(a, e, true);
        if (!w) {
            return true;
        }
        std::vector<usize> stale;
        {
            std::lock_guard copied(copied_lock_);
            for (const auto& [page, write] : w->written) {
                if (page < base) {
                    continue;
                }
                usize i = page - base;
                if (i >= copied_.at.size() || copied_.at[i] == 0 || write > copied_.at[i]) {
                    stale.push_back(page);
                }
            }
        }
        for (usize page : stale) {
            usize lo = std::max(page * PAGE, a);
            usize hi = std::min((page + 1) * PAGE, e);
            if (lo >= hi) {
                continue;
            }
            usize n = hi - lo;
            {


                std::lock_guard before(before_lock_);


                if (!host::read_into(reinterpret_cast<const std::uint8_t*>(lo), title, n)) {
                    continue;
                }
                usize at = lo - start;
                if (card_) {


                    std::uint8_t* held = data_ + at;
                    auto runs = fastmem::changed_runs(title, held, n);
                    if (runs.size() > runs_left) {
                        return true;
                    }
                    runs_left -= runs.size();
                    for (const auto& [r, m] : runs) {
                        std::memcpy(held + r, title + r, m);
                        card_changed(at + r, at + r + m);
                    }
                } else {
                    if (!before_) {
                        return true;
                    }
                    std::uint8_t* held = before_->bytes.get() + at;
                    for (const auto& [r, m] : fastmem::changed_runs(title, held, n)) {


                        std::memcpy(data_ + at + r, title + r, m);
                        std::memcpy(held + r, title + r, m);
                    }
                }
            }

            if (std::max(page * PAGE, start) >= a && std::min((page + 1) * PAGE, end) <= e) {
                std::lock_guard copied(copied_lock_);
                copied_.set(page - base, w->now);
            }
        }
    }
    return false;
}

usize Shadow::update(usize address, usize len) {

    if (placed_) {
        return 0;
    }
    usize start = start_;
    usize end = start_ + len_;
    usize from = std::max(address, start);
    usize to = std::min(address + len, end);
    if (from >= to) {
        return 0;
    }
    std::vector<Range> busy;
    {
        std::lock_guard pending(pending_lock_);
        for (const auto& r : pending_) {
            if (r.first < to && r.second > from) {
                busy.push_back(r);
            }
        }
    }
    if (!busy.empty()) {

        std::sort(busy.begin(), busy.end());
        usize at = from;
        usize bytes = 0;
        for (const auto& [a, e] : busy) {
            if (a > at) {
                bytes += update_free(at, a);
            }
            at = std::max(at, e);
        }
        if (at < to) {
            bytes += update_free(at, to);
        }
        return bytes;
    }
    return update_free(from, to);
}

usize Shadow::update_free(usize from, usize to) {
    std::uint8_t mode = watched_.load(std::memory_order_relaxed);
    if (mode != NOT_WATCHED) {
        std::optional<watch::WritesKind> result;
        if (mode == VIEW_GUARDED) {
            if (auto w = watch::writes_guarded(from, to)) {
                result = watch::WritesKind{w->now, std::move(w->written), true};
            }
        } else {
            result = watch::writes_kind(from, to, true);
        }
        if (result) {
            std::uint8_t next = result->is_view ? VIEW_GUARDED : WATCHED;
            if (mode != next) {
                watched_.store(next, std::memory_order_relaxed);
            }
            return copy_stale(from, to, result->now, result->written);
        }
        watched_.store(NOT_WATCHED, std::memory_order_relaxed);
    }
    copy(from, to);
    return to - from;
}

usize Shadow::copy_stale(usize from, usize to, std::uint64_t now, std::span<const std::pair<usize, std::uint64_t>> written) {
    usize base = static_cast<usize>(start_) / PAGE;
    usize start = start_;
    usize end = start_ + len_;
    usize first_page = from / PAGE;
    usize last = (to - 1) / PAGE;
    std::lock_guard lock(copied_lock_);
    CopiedPages& copied = copied_;
    auto whole_in_range = [&](usize page) { return std::max(page * PAGE, start) >= from && std::min((page + 1) * PAGE, end) <= to; };


    if (copied.all_copied()) {
        if (written.empty()) {
            return 0;
        }
        usize bytes = 0;
        std::optional<Range> run;
        for (const auto& [page, write] : written) {
            if (page < first_page || page > last) {
                continue;
            }
            std::uint64_t at = copied.at[page - base];
            if (write <= at) {
                continue;
            }
            if (whole_in_range(page)) {
                copied.set(page - base, now);
            }
            if (run && page < run->second) {
            } else if (run && page == run->second) {
                run->second = page + 1;
            } else if (run) {
                bytes += copy(std::max(run->first * PAGE, from), std::min(run->second * PAGE, to));
                run = Range{page, page + 1};
            } else {
                run = Range{page, page + 1};
            }
        }
        if (run) {
            bytes += copy(std::max(run->first * PAGE, from), std::min(run->second * PAGE, to));
        }
        return bytes;
    }

    usize w = 0;
    usize bytes = 0;
    std::optional<usize> run;
    for (usize page = first_page; page <= last; ++page) {
        while (w < written.size() && written[w].first < page) {
            ++w;
        }
        std::uint64_t write = 0;
        if (w < written.size() && written[w].first == page) {
            write = written[w].second;
            ++w;
        }
        std::uint64_t at = copied.at[page - base];
        if (at == 0 || write > at) {

            if (whole_in_range(page)) {
                copied.set(page - base, now);
            }
            if (!run) {
                run = page;
            }
        } else if (run) {
            usize first = *run;
            run.reset();
            bytes += copy(std::max(first * PAGE, from), std::min(page * PAGE, to));
        }
    }
    if (run) {
        bytes += copy(std::max(*run * PAGE, from), std::min((last + 1) * PAGE, to));
    }
    return bytes;
}

usize Shadow::copy(usize from, usize to) {
    usize start = start_;
    usize end = start_ + len_;
    from = std::max(from, start);
    to = std::min(to, end);
    if (observer_) {
        observer_(observer_context_, from, to - from);
    }
    host::copy_into_host(data_ + (from - start), reinterpret_cast<const std::uint8_t*>(from), to - from);

    changed(from - start, to - start);
    card_changed(from - start, to - start);
    return to - from;
}

std::vector<Piece> Shadow::differing(usize from, usize to, const std::uint8_t* before) const {
    usize start = start_;
    const std::uint8_t* data = data_;
    auto scan = [&](usize a, usize b, std::vector<Piece>& found) {
        usize at = a;
        while (at < b) {
            usize n = std::min(PIECE - (start + at) % PIECE, b - at);
            if (!fastmem::equal(data + at, before + (at - from), n)) {
                found.emplace_back(at, n);
            }
            at += n;
        }
    };
    constexpr usize PER_THREAD = usize{16} << 20;
    usize threads = std::clamp<usize>((to - from) / PER_THREAD, 1, 8);
    std::vector<Piece> out;
    if (threads == 1) {
        scan(from, to, out);
        return out;
    }


    auto cut = [&](usize i) {
        if (i == threads) {
            return to;
        }
        usize at = from + (to - from) / threads * i;
        return std::max(at - (start + at) % PIECE, from);
    };
    std::vector<std::vector<Piece>> parts(threads);
    std::vector<wide::Job> jobs;
    for (usize i = 0; i < threads; ++i) {
        jobs.emplace_back([&, i] { scan(cut(i), cut(i + 1), parts[i]); });
    }
    wide::run(jobs);
    for (auto& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

usize Shadow::copy_back_with(usize address, usize len, const std::vector<Piece>* found) {
    if (placed_) {
        return 0;
    }
    std::lock_guard snapshot(before_lock_);
    usize written = 0;
    auto [from, to] = inside(address, len);
    usize start = start_;
    compared(window_, len_, from, to);
    if (!before_ || from >= to) {
        return written;
    }
    const std::uint8_t* snap = before_->bytes.get();
    std::vector<Piece> computed;
    if (!found) {
        computed = differing(from, to, snap + from);
        found = &computed;
    }

    for (const auto& [at, n] : *found) {
        changed(at, at + n);
    }
    std::uint8_t* title = piece_scratch();
    for (const auto& [at0, n0] : *found) {
        std::uint8_t* host = data_ + at0;
        const std::uint8_t* old = snap + at0;


        auto first = fastmem::first_difference(host, old, n0);
        if (!first) {
            continue;
        }
        usize last = fastmem::last_difference(host, old, n0).value_or(*first);
        usize at = at0 + *first;
        usize n = last + 1 - *first;
        if (!host::read_into(reinterpret_cast<const std::uint8_t*>(start + at), title, n)) {
            continue;
        }


        if (fastmem::merge_gpu_into_title(title, host + *first, old + *first, n)) {
            watch::writing(start + at, n);
            host::write_mapped(reinterpret_cast<std::uint8_t*>(start + at), title, n);
            written += n;
        }
    }
    return written;
}

usize Shadow::relay_merge(usize at, usize n, std::vector<std::uint8_t>& now, std::vector<std::uint8_t>& title) {
    usize start = start_;
    if (card_ || n == 0 || at + n > len_) {
        return 0;
    }
    std::lock_guard snapshot(before_lock_);
    if (!before_) {
        return 0;
    }
    now.resize(n);


    std::memcpy(now.data(), data_ + at, n);
    std::uint8_t* held = before_->bytes.get() + at;
    auto first = fastmem::first_difference(now.data(), held, n);
    if (!first) {
        return 0;
    }
    usize last = fastmem::last_difference(now.data(), held, n).value_or(*first);
    usize at2 = at + *first;
    usize n2 = last + 1 - *first;
    title.resize(n2);


    if (!host::read_into(reinterpret_cast<const std::uint8_t*>(start + at2), title.data(), n2)) {
        return 0;
    }
    if (!fastmem::merge_piece(now.data() + *first, held + *first, title.data(), n2)) {
        return 0;
    }
    watch::writing(start + at2, n2);
    host::write_mapped(reinterpret_cast<std::uint8_t*>(start + at2), title.data(), n2);
    return n2;
}

Shadow::State Shadow::state() const {
    State s;
    s.mode = watched_.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(copied_lock_);
        s.at = copied_.at;
        s.uncopied = copied_.uncopied;
    }
    {
        std::lock_guard lock(pending_lock_);
        s.pending = pending_;
    }
    return s;
}

std::optional<std::uint64_t> Shadow::snapshot_hash(std::span<const Range> ranges) const {
    std::lock_guard lock(before_lock_);
    if (!before_) {
        return std::nullopt;
    }
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& [a, b] : ranges) {
        for (usize k = a; k < b; ++k) {
            h ^= before_->bytes[k];
            h *= 0x00000100000001b3ull;
        }
    }
    return h;
}

void take_snapshots() {
    std::vector<CopyPart> snaps;
    {
        std::lock_guard lock(SNAPS_LOCK);
        snaps.swap(SNAPS);
    }
    if (snaps.empty()) {
        return;
    }
    usize total = 0;
    for (const auto& c : snaps) {
        total += c.len;
    }
    auto jobs = copy_jobs(snaps, snapshot_job(total));
    wide::run(jobs);
}

std::vector<std::vector<Piece>> find_all(std::span<const std::tuple<Shadow*, usize, usize>> work) {
    std::vector<Scan> scans;
    scans.reserve(work.size());
    for (const auto& [shadow, address, end] : work) {
        auto [from, to] = shadow->inside(address, saturating_sub(end, address));

        std::lock_guard lock(shadow->before_lock_);
        if (shadow->before_) {
            scans.push_back({reinterpret_cast<usize>(shadow->data_), reinterpret_cast<usize>(shadow->before_->bytes.get()),
                             static_cast<usize>(shadow->start_), from, to});
        } else {
            scans.push_back({});
        }
    }
    return find_pieces(scans);
}

void hand_back(std::span<const Declared> back) {
    auto spans_of = [](const std::vector<Range>& declared) {
        std::vector<Range> sorted = declared;
        std::sort(sorted.begin(), sorted.end());
        std::vector<Range> spans;
        for (const auto& [from, to] : sorted) {
            if (!spans.empty() && from <= spans.back().second) {
                spans.back().second = std::max(spans.back().second, to);
            } else {
                spans.emplace_back(from, to);
            }
        }
        return spans;
    };
    if (wide_back()) {


        std::vector<std::tuple<Shadow*, usize, usize>> work;
        for (const auto& [shadow, declared] : back) {
            if (shadow->carded()) {
                continue;
            }
            for (const auto& [from, to] : spans_of(declared)) {
                work.emplace_back(shadow, from, to);
            }
        }
        auto found = find_all(work);
        for (usize i = 0; i < work.size(); ++i) {
            auto [shadow, from, to] = work[i];
            shadow->copy_back_with(from, to - from, &found[i]);
        }
        for (const auto& [shadow, declared] : back) {
            shadow->copy_back_done(declared);
        }
        return;
    }
    for (const auto& [shadow, declared] : back) {
        if (shadow->carded()) {
            shadow->copy_back_done(declared);
            continue;
        }
        for (const auto& [from, to] : spans_of(declared)) {
            shadow->copy_back(from, to - from);
        }
        shadow->copy_back_done(declared);
    }
}

void relay_hand_back(std::span<const Declared> back) {
    std::vector<std::tuple<Shadow*, usize, usize>> work;
    for (const auto& [shadow, declared] : back) {
        if (shadow->carded()) {
            continue;
        }
        std::vector<Range> spans;
        join_ranges(spans, declared);
        for (const auto& [from, to] : spans) {
            auto [a, e] = shadow->inside(from, to - from);
            compared(shadow->window(), shadow->len(), a, e);
        }
        for (const auto& [from, to] : spans) {
            work.emplace_back(shadow, from, to);
        }
    }
    if (work.empty()) {
        return;
    }

    auto found = find_all(work);
    std::vector<std::uint8_t> now, title;
    for (usize i = 0; i < work.size(); ++i) {
        Shadow* shadow = std::get<0>(work[i]);
        for (const auto& [at, n] : found[i]) {
            shadow->relay_merge(at, n, now, title);
        }
    }
}

void back_line(double frames) {
    double c[7];
    bool any = false;
    for (usize i = 0; i < 7; ++i) {
        c[i] = static_cast<double>(BACK_CLASS[i].exchange(0, std::memory_order_relaxed)) / frames;
        any = any || c[i] != 0.0;
    }
    if (!any) {
        return;
    }
    auto f = [](double v, int places) {
        char b[64];
        std::snprintf(b, sizeof b, "%.*f", places, v);
        return std::string(b);
    };
    auto mb = [&](double v) { return f(v / static_cast<double>(1 << 20), 1); };
    log::info("victory_gpu::shadow", "shader writes looked for on the CPU per frame: " + mb(c[2]) + " MB compared in " + f(c[5], 0) +
                                         " ranges of windows outside buffers (" + mb(c[0]) + " MB snapshotted), " + mb(c[3]) + " MB in " + f(c[6], 0) +
                                         " ranges of title buffers' copies (" + mb(c[1]) + " MB snapshotted), of them " + mb(c[4]) +
                                         " MB in copies of 1 MiB or more (VICTORY_CARD_BUFFERS_MIN_MB=1 would have the card look there)");
}

}   
