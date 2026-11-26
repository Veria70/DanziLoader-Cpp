// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/frame.rs (see sync.hpp): frame pacing. The
// title sets a frame interval and waits for each frame to begin. Ours is a
// clock started by the first interval set, ticking once per interval; the
// token is the frame number. Each event kind (origin, vblank, flip) happens
// once per tick, so two waits for the same kind never end on the same tick.
//
// shortcut (the reference's): event offsets are ignored (every event is at
// the tick).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>

#include "core/env.hpp"
#include "core/log.hpp"
#include "core/win.hpp"
#include "sync.hpp"

namespace danzi::d3d12x {

namespace {


constexpr HRESULT TIMEOUT = S_FALSE_;

constexpr std::uint32_t ORIGIN = 0;


constexpr std::uint32_t TOKEN_ON_TIMEOUT = 2;
constexpr std::uint64_t SIXTIETH_NS = 16'667'000;

std::uint64_t now_ns() {
    static const std::uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    auto t = static_cast<std::uint64_t>(c.QuadPart);
    return t / freq * 1'000'000'000 + t % freq * 1'000'000'000 / freq;
}

struct Clock {
    std::uint64_t start_ns;
    std::uint64_t interval_ns;

    std::uint64_t first;
};

danzi::FastLock CLOCK_LOCK;
std::optional<Clock> CLOCK;
std::atomic<std::uint64_t> FRAME{0};


std::atomic<std::uint64_t> LAST[3]{};

std::atomic<std::uint64_t>& last(std::uint32_t kind) { return LAST[std::min<std::size_t>(kind, 2)]; }

struct Tick {
    std::uint64_t tick;
    std::uint64_t wait_ns;

    std::uint64_t at_ns;
};


Tick next_tick(std::uint32_t kind) {
    std::lock_guard lock(CLOCK_LOCK);
    if (!CLOCK) {
        CLOCK = Clock{now_ns(), SIXTIETH_NS, 0};
    }
    const Clock& c = *CLOCK;
    std::uint64_t elapsed = now_ns() - c.start_ns;
    std::uint64_t interval = std::max<std::uint64_t>(c.interval_ns, 1);
    std::uint64_t tick = std::max(c.first + elapsed / interval + 1, last(kind).load(std::memory_order_relaxed) + 1);
    std::uint64_t at = (tick - c.first) * interval;
    return {tick, at > elapsed ? at - elapsed : 0, c.start_ns + at};
}


void wait_until(std::uint64_t deadline) {
    static const std::uint64_t spin = [] {
        auto v = danzi::env::get(L"VICTORY_PACE_SPIN_US");
        try {
            return v ? std::stoull(*v) * 1000 : 1'000'000ull;
        } catch (...) {
            return 1'000'000ull;
        }
    }();
    std::uint64_t now = now_ns();
    if (deadline <= now) {
        return;
    }
    if (deadline - now > spin) {
        sleep_ns(deadline - now - spin);
    }
    while (now_ns() < deadline) {
        YieldProcessor();
    }
}

void fetch_max(std::atomic<std::uint64_t>& a, std::uint64_t v) {
    std::uint64_t n = a.load(std::memory_order_relaxed);
    while (n < v && !a.compare_exchange_weak(n, v, std::memory_order_relaxed)) {
    }
}


std::string duration_debug(std::uint64_t ns) {
    const char* unit = ns >= 1'000'000'000 ? "s" : ns >= 1'000'000 ? "ms" : ns >= 1'000 ? "µs" : "ns";
    std::uint64_t div = ns >= 1'000'000'000 ? 1'000'000'000 : ns >= 1'000'000 ? 1'000'000 : ns >= 1'000 ? 1'000 : 1;
    std::string out = std::to_string(ns / div);
    std::uint64_t frac = ns % div;
    if (frac != 0) {
        out += '.';
        for (std::uint64_t d = div / 10; frac != 0; d /= 10) {
            out += static_cast<char>('0' + frac / d);
            frac %= d;
        }
    }
    return out + unit;
}

}   

void sleep_ns(std::uint64_t ns) {
    if (ns == 0) {
        return;
    }


    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(std::min<std::uint64_t>(ns / 100, INT64_MAX));
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) {
            return;
        }
    }
    Sleep(static_cast<DWORD>(std::min<std::uint64_t>((ns + 999'999) / 1'000'000, INFINITE - 1)));
}


HRESULT __stdcall frame_set_interval(This, This, std::uint32_t length, std::uint32_t, std::uint32_t) {
    std::uint64_t interval = length == 0 ? SIXTIETH_NS : std::uint64_t{length} * 1000;
    log::info("victory_gpu::frame", "frame interval", {{"interval", duration_debug(interval)}});

    std::uint64_t first = 0;
    for (auto& t : LAST) {
        first = std::max(first, t.load(std::memory_order_relaxed));
    }
    std::lock_guard lock(CLOCK_LOCK);
    CLOCK = Clock{now_ns(), interval, first};
    return S_OK_;
}


HRESULT __stdcall frame_schedule(This, std::uint32_t, std::uint32_t, const std::uint8_t*, std::uint32_t) { return S_OK_; }

std::uint64_t frame_current() { return FRAME.load(std::memory_order_relaxed); }


HRESULT __stdcall frame_wait(This, std::uint32_t kind, std::uint32_t timeout_ms, const std::uint8_t*, std::uint32_t flags, std::uint64_t* token) {
    bool origin = kind == ORIGIN;
    auto [tick, wait, deadline] = next_tick(kind);
    std::uint64_t limit = std::uint64_t{timeout_ms} * 1'000'000;


    static const bool sleep_on = danzi::env::is(L"VICTORY_LATE_WAITS", L"1");
    if (!sleep_on && limit >= wait && tick > last(kind).load(std::memory_order_relaxed) + 1) {
        tick -= 1;
        wait = 0;
    }
    HRESULT hr;
    if (wait > limit) {
        sleep_ns(limit);
        hr = TIMEOUT;
    } else {
        if (wait > 0) {
            wait_until(deadline);
        }
        fetch_max(last(kind), tick);
        hr = S_OK_;
    }

    if (origin && (hr == S_OK_ || (flags & TOKEN_ON_TIMEOUT) != 0)) {

        std::uint64_t frame = FRAME.fetch_add(1, std::memory_order_relaxed) + 1;
        if (token) {
            *token = frame;
        }
    }
    return hr;
}


HRESULT __stdcall frame_statistics(This, std::uint64_t, std::uint32_t, std::uint32_t* count, std::uint8_t*) {
    if (count) {
        *count = 0;
    }
    return S_OK_;
}

}   
