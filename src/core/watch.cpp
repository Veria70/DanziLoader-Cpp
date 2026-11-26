// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/watch.rs. Function by function; the
// comments say what the reference's comments say where it matters for the
// behavior. Diagnostics counters (`feature = "diagnostics"`) are left out,
// as in the reference's plain release build.
#include "watch.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "env.hpp"
#include "hybrid_watch.hpp"
#include "inplace.hpp"
#include "page_table.hpp"

namespace danzi::watch {

namespace {


constexpr usize KERNEL_PAGE = 4096;


std::atomic<std::uint64_t> ASKED{0};

std::uint64_t next_asking() { return ASKED.fetch_add(1, std::memory_order_acq_rel) + 1; }

struct Journal {

    PageMap written;

    std::vector<usize> found;


    std::vector<Range> ahead;
    std::uint64_t ahead_asked = 0;

    bool kept = false;
};

SrwLock JOURNAL_LOCK;
Journal JOURNAL;


constexpr usize AHEAD_GAP = (usize{8} << 20) / PAGE;

constexpr usize AHEAD_MAX = (usize{1} << 30) / PAGE;


std::vector<PageWrite> written_in(const Journal& j, usize first, usize last) { return j.written.range(first, last); }

bool ahead_holds(const Journal& j, usize first, usize last) {
    return std::any_of(j.ahead.begin(), j.ahead.end(), [&](const Range& r) { return r.first <= first && last <= r.second; });
}


bool ask_kernel(Journal& j, usize first, usize last) {
    if (hybrid::enabled()) {

        std::vector<usize> pages;
        if (!hybrid::written(first, last, pages)) {
            return false;
        }
        for (usize p : pages) {
            j.written.insert_or_assign(p, j.ahead_asked);
        }
        return true;
    }
    ULONG_PTR count = (last - first) * PAGE / KERNEL_PAGE;
    j.found.resize(std::max<usize>(count, j.found.size()), 0);
    ULONG granularity = 0;
    bool failed = GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(first * PAGE), (last - first) * PAGE,
                                reinterpret_cast<void**>(j.found.data()), &count, &granularity) != 0;
    if (failed) {
        return false;
    }
    std::uint64_t now = j.ahead_asked;
    for (ULONG_PTR k = 0; k < count; ++k) {
        j.written.insert_or_assign(j.found[k] / PAGE, now);
    }
    return true;
}


void ask_run(Journal& j, std::span<const Range> pieces) {
    if (pieces.size() < 2) {
        return;
    }
    usize first = pieces[0].first;
    usize last = first;
    bool any = false;
    for (const auto& p : pieces) {
        last = any ? std::max(last, p.second) : p.second;
        any = true;
    }
    if (ask_kernel(j, first, last)) {
        j.ahead.emplace_back(first, last);
        return;
    }
    usize half = pieces.size() / 2;
    ask_run(j, pieces.subspan(0, half));
    ask_run(j, pieces.subspan(half));
}


Writes finish_writes(usize first, usize last, const std::vector<usize>& new_pages) {
    if (new_pages.empty()) {
        std::shared_lock j_lock(JOURNAL_LOCK);
        std::uint64_t now = next_asking();
        return Writes{now, written_in(JOURNAL, first, last)};
    }
    std::unique_lock j_lock(JOURNAL_LOCK);
    std::uint64_t now = next_asking();
    for (usize page : new_pages) {
        JOURNAL.written.insert_or_assign(page, now);
    }
    return Writes{now, written_in(JOURNAL, first, last)};
}


constexpr usize BLOCK = PAGE;

struct Guards {

    PageSet protected_;

    PageSet clean;

    PageSet absent;

    PageSet aliased;
};

SrwLock GUARDS_LOCK;
Guards GUARDS;

std::vector<usize> set_range(const PageSet& s, usize first, usize last) { return s.range(first, last); }

usize set_count(const PageSet& s, usize first, usize last) { return s.count(first, last); }

bool view_guard() {
    static const bool on = env::not_set_or_not(L"VICTORY_VIEW_GUARD", L"0");
    return on;
}


usize uncommitted_end(usize block) {
    MEMORY_BASIC_INFORMATION info{};
    SIZE_T n = VirtualQuery(reinterpret_cast<void*>(block * BLOCK), &info, sizeof(info));
    if (n != 0 && (info.State == MEM_RESERVE || info.State == MEM_FREE)) {
        return saturating_add(reinterpret_cast<usize>(info.BaseAddress), info.RegionSize) / BLOCK;
    }
    return block;
}


void protect_here(Guards& g, usize from, usize to) {
    auto set = [](usize f, usize t) {
        DWORD old = 0;
        return VirtualProtect(reinterpret_cast<void*>(f * BLOCK), (t - f) * BLOCK, PAGE_READONLY, &old) != 0;
    };
    auto note = [](Guards& gg, usize f, usize t) {
        gg.protected_.insert_range(f, t);
        gg.clean.insert_range(f, t);
    };
    if (set(from, to)) {
        note(g, from, to);
        return;
    }
    for (usize block = from; block < to; ++block) {
        if (set(block, block + 1)) {
            note(g, block, block + 1);
        } else if (block < uncommitted_end(block)) {
            g.absent.insert(block);
        }
    }
}


void protect_alias(Guards& g, usize block) {
    if (g.protected_.contains(block)) {
        return;
    }
    DWORD old = 0;
    if (VirtualProtect(reinterpret_cast<void*>(block * BLOCK), BLOCK, PAGE_READONLY, &old) != 0) {
        g.protected_.insert(block);
    }
}


void protect_alias_run(Guards& g, usize from, usize to) {
    if (to <= from) {
        return;
    }
    DWORD old = 0;
    if (VirtualProtect(reinterpret_cast<void*>(from * BLOCK), (to - from) * BLOCK, PAGE_READONLY, &old) != 0) {
        g.protected_.insert_range(from, to);
        return;
    }
    for (usize block = from; block < to; ++block) {
        protect_alias(g, block);
    }
}


void protect(Guards& g, usize from, usize to) {
    protect_here(g, from, to);
    if (!view_guard()) {
        return;
    }
    auto aliases = inplace::alias_blocks_in(from, to, MANY_VIEWS);
    if (aliases.blocks_counted == 0) {
        return;
    }
    for (usize b : aliases.all_aliased) {
        g.aliased.insert(b);
    }
    std::optional<Range> run;
    for (usize alias : aliases.others) {
        if (g.protected_.contains(alias)) {
            continue;
        }
        if (run && alias == run->second) {
            run->second = alias + 1;
        } else if (run) {
            protect_alias_run(g, run->first, run->second);
            run = Range{alias, alias + 1};
        } else {
            run = Range{alias, alias + 1};
        }
    }
    if (run) {
        protect_alias_run(g, run->first, run->second);
    }
}


bool open_here(Guards& g, usize start, usize end) {
    auto blocks = set_range(g.protected_, start / BLOCK, div_ceil(end, BLOCK));
    for (usize block : blocks) {
        g.protected_.erase(block);
        g.clean.erase(block);
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(block * BLOCK), BLOCK, PAGE_READWRITE, &old);
    }
    return !blocks.empty();
}


bool open(Guards& g, usize start, usize end) {
    bool any = open_here(g, start, end);
    if (start >= end || g.aliased.empty()) {
        return any;
    }
    auto aliased = set_range(g.aliased, start / BLOCK, div_ceil(end, BLOCK));
    for (usize block : aliased) {
        g.aliased.erase(block);
        for (usize alias : inplace::pages_of(block * BLOCK)) {
            if (alias / BLOCK != block) {
                open_here(g, alias, saturating_add(alias, BLOCK));
            }
            g.aliased.erase(alias / BLOCK);
        }
    }
    return any;
}

LONG CALLBACK on_write(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    constexpr ULONG_PTR WRITE = 1;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != WRITE) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    usize at = r->ExceptionInformation[1];

    if (hybrid::enabled() && hybrid::on_write_fault(at)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }


    std::lock_guard lock(GUARDS_LOCK);
    if (open(GUARDS, at, at + 1)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return detail::writable(at) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}


void install() {
    static std::once_flag once_flag;
    std::call_once(once_flag, [] { AddVectoredExceptionHandler(1, on_write); });
}

SrwLock MAPPING;

}   


void ask_ahead(std::span<const Range> ranges) {
    std::vector<Range> pieces;
    pieces.reserve(ranges.size());
    for (const auto& r : ranges) {
        if (r.first < r.second) {
            pieces.emplace_back(r.first / PAGE, div_ceil(r.second, PAGE));
        }
    }
    if (pieces.size() < 2) {
        return;
    }
    std::sort(pieces.begin(), pieces.end());
    std::unique_lock j_lock(JOURNAL_LOCK);
    Journal& j = JOURNAL;


    if (j.kept) {
        return;
    }
    j.ahead_asked = next_asking();
    j.ahead.clear();
    usize i = 0;
    while (i < pieces.size()) {
        usize first = pieces[i].first;
        usize last = pieces[i].second;
        usize k = i + 1;
        while (k < pieces.size() && pieces[k].first <= last + AHEAD_GAP && std::max(last, pieces[k].second) - first <= AHEAD_MAX) {
            last = std::max(last, pieces[k].second);
            ++k;
        }
        ask_run(j, std::span<const Range>(pieces).subspan(i, k - i));
        i = k;
    }
}

void ask_ahead_done() {
    std::unique_lock j_lock(JOURNAL_LOCK);
    if (!JOURNAL.kept) {
        JOURNAL.ahead.clear();
    }
}

bool once() {

    static const bool on = env::not_set_or_not(L"VICTORY_ASK_ONCE", L"0");
    return on;
}

void ask_once(std::span<const Range> ranges) {
    if (!once()) {
        return;
    }
    ask_once_end();
    ask_ahead(ranges);
    std::unique_lock j_lock(JOURNAL_LOCK);
    Journal& j = JOURNAL;
    if (j.ahead.empty()) {

        j.ahead_asked = next_asking();
    }

    for (const auto& [start, end] : ranges) {
        if (start >= end) {
            continue;
        }
        usize first = start / PAGE;
        usize last = div_ceil(end, PAGE);
        if (!ahead_holds(j, first, last) && ask_kernel(j, first, last)) {
            j.ahead.emplace_back(first, last);
        }
    }
    j.kept = !j.ahead.empty();
}

void ask_once_end() {
    std::unique_lock j_lock(JOURNAL_LOCK);
    if (JOURNAL.kept) {
        JOURNAL.kept = false;
        JOURNAL.ahead.clear();
    }
}

void wrote(usize at, usize len) {
    std::unique_lock j_lock(JOURNAL_LOCK);
    if (!JOURNAL.kept || len == 0) {
        return;
    }
    std::uint64_t now = next_asking();
    for (usize page = at / PAGE; page < div_ceil(saturating_add(at, len), PAGE); ++page) {
        JOURNAL.written.insert_or_assign(page, now);
    }
}

std::optional<Writes> asked(usize start, usize end) {
    usize first = start / PAGE;
    usize last = div_ceil(end, PAGE);
    std::shared_lock j_lock(JOURNAL_LOCK);
    if (!ahead_holds(JOURNAL, first, last)) {
        return std::nullopt;
    }
    return Writes{JOURNAL.ahead_asked, written_in(JOURNAL, first, last)};
}

std::optional<Writes> writes_guarded(usize start, usize end) {
    usize first = start / PAGE;
    usize last = div_ceil(end, PAGE);
    auto written = detail::guard(first * PAGE, last * PAGE);
    if (!written) {
        return std::nullopt;
    }
    return finish_writes(first, last, *written);
}

std::optional<WritesKind> writes_kind(usize start, usize end, bool guarded) {
    usize first = start / PAGE;
    usize last = div_ceil(end, PAGE);

    {
        std::shared_lock j_lock(JOURNAL_LOCK);
        if (ahead_holds(JOURNAL, first, last)) {
            return WritesKind{JOURNAL.ahead_asked, written_in(JOURNAL, first, last), false};
        }
    }


    thread_local usize* found = nullptr;
    thread_local usize found_len = 0;
    usize pages_needed = (last - first) * PAGE / KERNEL_PAGE;
    std::vector<usize> found_pages;
    if (hybrid::enabled()) {

        if (hybrid::written(first, last, found_pages)) {
            Writes w = finish_writes(first, last, found_pages);
            return WritesKind{w.now, std::move(w.written), false};
        }
        if (!guarded) {
            return std::nullopt;
        }
        auto written = detail::guard(first * PAGE, last * PAGE);
        if (!written) {
            return std::nullopt;
        }
        Writes w = finish_writes(first, last, *written);
        return WritesKind{w.now, std::move(w.written), true};
    }
    if (found_len < pages_needed) {
        delete[] found;
        found = new usize[pages_needed];
        found_len = pages_needed;
    }
    ULONG_PTR count = pages_needed;
    ULONG granularity = 0;
    bool failed = GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(first * PAGE), (last - first) * PAGE,
                                reinterpret_cast<void**>(found), &count, &granularity) != 0;
    if (!failed && count > 0) {
        found_pages.reserve(count);
        for (ULONG_PTR k = 0; k < count; ++k) {
            found_pages.push_back(found[k] / PAGE);
        }
    }
    bool is_view = false;
    if (failed) {
        if (!guarded) {
            return std::nullopt;
        }
        auto written = detail::guard(first * PAGE, last * PAGE);
        if (!written) {
            return std::nullopt;
        }
        found_pages = std::move(*written);
        is_view = true;
    }
    Writes w = finish_writes(first, last, found_pages);
    return WritesKind{w.now, std::move(w.written), is_view};
}

std::optional<Writes> writes(usize start, usize end, bool guarded) {
    auto k = writes_kind(start, end, guarded);
    if (!k) {
        return std::nullopt;
    }
    return Writes{k->now, std::move(k->written)};
}


namespace detail {

void install_handler() { install(); }


std::optional<std::vector<usize>> guard(usize start, usize end) {


    static const bool ask = env::is(L"VICTORY_GUARD_ASK", L"1");
    bool known = false;
    if (!ask) {
        std::lock_guard lock(GUARDS_LOCK);
        usize first = start / BLOCK;
        known = GUARDS.protected_.contains(first) || GUARDS.absent.contains(first);
    }
    if (!known && !is_view(start, end)) {
        return std::nullopt;
    }
    install();
    std::lock_guard lock(GUARDS_LOCK);
    Guards& g = GUARDS;
    std::vector<usize> written;
    std::optional<usize> run;
    usize first = start / BLOCK;
    usize last = div_ceil(end, BLOCK);

    usize clean_count = set_count(g.clean, first, last);
    if (clean_count == last - first) {
        return written;
    }
    written.reserve((last - first - clean_count) * (BLOCK / PAGE));
    auto clean = set_range(g.clean, first, last);
    usize next_clean = 0;
    usize uncommitted_until = first;
    for (usize block = first; block <= last; ++block) {
        while (next_clean < clean.size() && clean[next_clean] < block) {
            ++next_clean;
        }
        bool is_clean = next_clean < clean.size() && clean[next_clean] == block;
        bool open_it = block < last && !is_clean;
        if (open_it && g.absent.contains(block)) {


            if (block >= uncommitted_until) {
                uncommitted_until = uncommitted_end(block);
            }
            open_it = block >= uncommitted_until;
            if (open_it) {
                g.absent.erase(block);
            }
        }
        if (open_it) {
            for (usize p = block * BLOCK / PAGE; p < (block + 1) * BLOCK / PAGE; ++p) {
                written.push_back(p);
            }
            if (g.protected_.contains(block)) {

                g.clean.insert(block);
            } else {
                if (!run) {
                    run = block;
                }
                continue;
            }
        }
        if (run) {
            usize from = *run;
            run.reset();
            protect(g, from, block);
        }
    }
    return written;
}


bool is_view(usize start, usize end) {
    static const bool holes = env::not_set_or_not(L"VICTORY_GUARD_HOLES", L"0");
    usize at = start;
    for (;;) {
        MEMORY_BASIC_INFORMATION info{};
        SIZE_T n = VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info));
        if (n == 0) {
            return false;
        }
        if (info.Type == MEM_MAPPED) {
            return true;
        }
        if (info.State == MEM_COMMIT || !holes) {
            return false;
        }
        usize next = saturating_add(reinterpret_cast<usize>(info.BaseAddress), info.RegionSize);
        if (next <= at || next >= end) {
            return true;
        }
        at = next;
    }
}

bool writable(usize at) {
    MEMORY_BASIC_INFORMATION info{};
    SIZE_T n = VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info));
    return n != 0 && info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE;
}

}   

void writing(usize at, usize len) {
    {
        std::lock_guard lock(GUARDS_LOCK);
        open(GUARDS, at, saturating_add(at, len));
    }
    hybrid::opening(at, len);


}

void views_mapped(usize section, std::uint64_t offset, usize len) {
    if (!view_guard()) {
        return;
    }
    auto ranges = inplace::views_of_section(section, offset, len);
    if (ranges.empty()) {
        return;
    }
    std::lock_guard lock(GUARDS_LOCK);
    for (const auto& [at, n] : ranges) {
        open_here(GUARDS, at, saturating_add(at, n));
    }
}

void title_memory_remapped(usize at, usize len) {
    hybrid::forget(at, len);


    inplace::views_gone(at, len);

    inplace::title_memory_freeing(at, len);
    std::lock_guard lock(GUARDS_LOCK);
    Guards& g = GUARDS;
    usize first = at / BLOCK;
    usize last = div_ceil(saturating_add(at, len), BLOCK);
    for (usize block : set_range(g.absent, first, last)) {
        g.absent.erase(block);
    }
    for (usize block : set_range(g.protected_, first, last)) {
        g.protected_.erase(block);
        g.clean.erase(block);
    }
    for (usize block : set_range(g.aliased, first, last)) {
        g.aliased.erase(block);
    }
}

Remapping::Remapping() : lock_(MAPPING) {}
Reading::Reading() : lock_(MAPPING) {}

Remapping title_memory_remapping() { return Remapping{}; }
Reading title_memory_reading() { return Reading{}; }

std::optional<Reading> title_memory_reading_now() {
    std::shared_lock<SrwLock> lock(MAPPING, std::try_to_lock);
    if (!lock.owns_lock()) {
        return std::nullopt;
    }
    return Reading(std::move(lock));
}

}   
