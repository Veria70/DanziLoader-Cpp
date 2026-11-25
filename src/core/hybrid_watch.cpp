// SPDX-License-Identifier: GPL-3.0-or-later
#include "hybrid_watch.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "env.hpp"
#include "page_table.hpp"
#include "watch.hpp"

namespace danzi::hybrid {

namespace {

using watch::PAGE;   
constexpr usize KERNEL_PAGE = 4096;


constexpr usize MERGE_GAP = 32;


constexpr std::uint64_t LIMIT_MOST = 1024;

std::atomic<int> forced{-1};

struct State {


    std::map<usize, usize> spans;   

    PageSet known;

    PageSet cold;


    PageSet faulted;

    PageMap idle;


    PageMap limit;


    PageMap last_write;


    PageMap cold_at;
    PageMap period;
    std::uint64_t clock = 0;
};

SrwLock LOCK;
State STATE;

std::atomic<std::uint64_t> FAULTS{0}, MADE_COLD{0}, KERNEL_ASKINGS{0}, KERNEL_PAGES{0};

usize idle_limit() {
    static const usize n = [] {
        if (auto v = env::get(L"VICTORY_HYBRID_IDLE")) {
            try {
                return std::clamp<usize>(std::stoul(*v), 1, 1 << 20);
            } catch (...) {
            }
        }
        return usize{16};
    }();
    return n;
}


bool backoff_on() {
    static const bool on = env::not_set_or_not(L"VICTORY_HYBRID_BACKOFF", L"0");
    return on;
}


bool ask_kernel(usize first, usize last, std::vector<usize>& out) {
    thread_local std::vector<void*> found;
    ULONG_PTR count = (last - first) * PAGE / KERNEL_PAGE;
    if (found.size() < count) {
        found.resize(count);
    }
    ULONG granularity = 0;
    KERNEL_ASKINGS.fetch_add(1, std::memory_order_relaxed);
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(first * PAGE), (last - first) * PAGE, found.data(), &count, &granularity) != 0) {
        return false;
    }
    KERNEL_PAGES.fetch_add(last - first, std::memory_order_relaxed);
    for (ULONG_PTR k = 0; k < count; ++k) {
        out.push_back(reinterpret_cast<usize>(found[k]) / PAGE);
    }
    return true;
}

bool protect(usize first, usize last, DWORD how) {
    DWORD old = 0;
    return VirtualProtect(reinterpret_cast<void*>(first * PAGE), (last - first) * PAGE, how, &old) != 0;
}


void make_writable(usize b) {
    if (protect(b, b + 1, PAGE_READWRITE)) {
        return;
    }
    for (usize k = 0; k < PAGE / KERNEL_PAGE; ++k) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(b * PAGE + k * KERNEL_PAGE), KERNEL_PAGE, PAGE_READWRITE, &old);
    }
}


template <class F>
void runs_of(const std::vector<usize>& v, usize gap, F&& f) {
    usize i = 0;
    while (i < v.size()) {
        usize j = i + 1;
        while (j < v.size() && v[j] <= v[j - 1] + 1 + gap) {
            ++j;
        }
        f(v[i], v[j - 1] + 1);
        i = j;
    }
}


bool proven(const State& s, usize first, usize last) {
    auto it = s.spans.upper_bound(first);
    if (it == s.spans.begin()) {
        return false;
    }
    --it;
    return it->first <= first && last <= it->second;
}


void prove(State& s, usize first, usize last) {
    auto it = s.spans.upper_bound(first);
    if (it != s.spans.begin() && std::prev(it)->second > first) {
        --it;
    }
    while (it != s.spans.end() && it->first < last) {
        first = std::min(first, it->first);
        last = std::max(last, it->second);
        it = s.spans.erase(it);
    }
    s.spans.emplace(first, last);
}


void open_cold(State& s, usize first, usize last, bool backoff) {
    for (usize b : s.cold.range(first, last)) {
        make_writable(b);
        s.cold.erase(b);
        s.faulted.insert(b);
        s.idle.insert_or_assign(b, 1);
        if (backoff && backoff_on()) {


            std::uint64_t l = s.limit.get(b);
            l = l == 0 ? idle_limit() : l;
            std::uint64_t per = std::max<std::uint64_t>(s.period.get(b), 2) - 1;
            std::uint64_t since = (s.clock + 1 - std::max<std::uint64_t>(s.cold_at.get(b), 1)) / per;
            if (since < l) {
                s.limit.insert_or_assign(b, std::min<std::uint64_t>(l * 2, LIMIT_MOST));
            } else {
                s.limit.erase(b);
            }
        }
        s.last_write.insert_or_assign(b, s.clock + 1);
        s.cold_at.erase(b);
        s.period.erase(b);
    }
}

}   

bool enabled() {
    int f = forced.load(std::memory_order_relaxed);
    if (f >= 0) {
        return f != 0;
    }


    static const bool on = env::not_set_or_not(L"VICTORY_HYBRID_WATCH", L"0");
    return on;
}

void detail::set_enabled_for_tests(int on) { forced.store(on, std::memory_order_relaxed); }

bool written(usize first, usize last, std::vector<usize>& out) {
    if (first >= last) {
        return true;
    }
    std::lock_guard lock(LOCK);
    State& s = STATE;
    ++s.clock;
    usize start = out.size();
    std::vector<usize> asked;   
    std::vector<usize> hot;     
    if (!proven(s, first, last)) {


        if (!ask_kernel(first, last, asked)) {
            return false;
        }
        prove(s, first, last);
        for (usize b = first; b < last; ++b) {
            if (!s.known.contains(b)) {
                s.known.insert(b);
                s.idle.insert_or_assign(b, 1);
            }
            if (!s.cold.contains(b)) {
                hot.push_back(b);
            }
        }
    } else {
        s.known.each(first, last, [&](usize b) {
            if (!s.cold.contains(b)) {
                hot.push_back(b);
            }
        });
        bool refused = false;
        runs_of(hot, MERGE_GAP, [&](usize a, usize b) {
            if (!refused && !ask_kernel(a, b, asked)) {
                refused = true;
            }
        });
        if (refused) {


            for (usize p : asked) {
                s.faulted.insert(p);
            }
            for (auto it = s.spans.begin(); it != s.spans.end();) {
                it = it->first < last && first < it->second ? s.spans.erase(it) : std::next(it);
            }
            return false;
        }
    }

    std::vector<usize> faulted = s.faulted.range(first, last);
    for (usize b : faulted) {
        s.faulted.erase(b);
    }
    out.insert(out.end(), asked.begin(), asked.end());
    out.insert(out.end(), faulted.begin(), faulted.end());

    std::sort(asked.begin(), asked.end());
    std::vector<usize> to_cold;
    usize w = 0;
    for (usize b : hot) {
        while (w < asked.size() && asked[w] < b) {
            ++w;
        }
        if (w < asked.size() && asked[w] == b) {
            s.idle.insert_or_assign(b, 1);
            s.last_write.insert_or_assign(b, s.clock + 1);
            continue;
        }
        std::uint64_t n = s.idle.get(b) + 1;
        std::uint64_t l = s.limit.get(b);
        if (n > (l == 0 ? idle_limit() : l)) {
            to_cold.push_back(b);
        } else {
            s.idle.insert_or_assign(b, n);
        }
    }
    if (!to_cold.empty()) {
        watch::detail::install_handler();
    }
    runs_of(to_cold, 0, [&](usize a, usize b) {
        if (!protect(a, b, PAGE_READONLY)) {
            return;   
        }
        s.cold.insert_range(a, b);
        for (usize p = a; p < b; ++p) {


            std::uint64_t lw = s.last_write.get(p);
            std::uint64_t n = std::max<std::uint64_t>(s.idle.get(p), 1);
            std::uint64_t ticks = lw == 0 ? n : s.clock + 1 - lw;
            s.period.insert_or_assign(p, std::max<std::uint64_t>(ticks / n, 1) + 1);
            s.cold_at.insert_or_assign(p, s.clock + 1);
            s.idle.erase(p);
        }
        MADE_COLD.fetch_add(b - a, std::memory_order_relaxed);


        ask_kernel(a, b, out);
    });

    std::sort(out.begin() + static_cast<std::ptrdiff_t>(start), out.end());
    out.erase(std::unique(out.begin() + static_cast<std::ptrdiff_t>(start), out.end()), out.end());
    return true;
}

bool on_write_fault(usize at) {
    std::lock_guard lock(LOCK);
    usize b = at / PAGE;
    if (!STATE.cold.contains(b)) {
        return false;
    }
    FAULTS.fetch_add(1, std::memory_order_relaxed);
    open_cold(STATE, b, b + 1, true);
    return true;
}

void opening(usize at, usize len) {
    if (!enabled() || len == 0) {
        return;
    }
    std::lock_guard lock(LOCK);
    open_cold(STATE, at / PAGE, div_ceil(saturating_add(at, len), PAGE), false);
}

void forget(usize at, usize len) {
    if (!enabled() || len == 0) {
        return;
    }
    std::lock_guard lock(LOCK);
    State& s = STATE;
    usize first = at / PAGE, last = div_ceil(saturating_add(at, len), PAGE);
    for (usize b : s.cold.range(first, last)) {
        make_writable(b);   
        s.cold.erase(b);
    }
    for (usize b : s.known.range(first, last)) {
        s.known.erase(b);
        s.idle.erase(b);
        s.limit.erase(b);
        s.last_write.erase(b);
        s.cold_at.erase(b);
        s.period.erase(b);
    }

    for (auto it = s.spans.begin(); it != s.spans.end();) {
        it = it->first < last && first < it->second ? s.spans.erase(it) : std::next(it);
    }

}

Stats stats() {
    return {FAULTS.load(), MADE_COLD.load(), KERNEL_ASKINGS.load(), KERNEL_PAGES.load()};
}

}   
