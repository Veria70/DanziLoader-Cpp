// SPDX-License-Identifier: GPL-3.0-or-later
// Hybrid write tracking against the ground truth: every page written since
// the last asking is reported, whichever way it was written (a CPU write that
// faults on a cold block, one into a hot block, a kernel write told through
// `watch::writing`), through many hot/cold transitions.
#include <random>
#include <set>

#include "check.hpp"
#include "core/hybrid_watch.hpp"
#include "core/watch.hpp"
#include "core/win.hpp"

using danzi::usize;
using danzi::watch::PAGE;

namespace {
struct Forced {
    Forced() { danzi::hybrid::detail::set_enabled_for_tests(1); }
    ~Forced() { danzi::hybrid::detail::set_enabled_for_tests(-1); }
};
}   

TEST(hybrid_tracking_reports_every_written_page) {
    Forced on;
    constexpr usize BLOCKS = 48;
    auto p = reinterpret_cast<usize>(VirtualAlloc(nullptr, BLOCKS * PAGE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    CHECK(p != 0);
    usize first = p / PAGE;
    std::mt19937_64 rng(12345);
    auto before = danzi::hybrid::stats();

    auto ask = [&](usize a, usize b) {
        auto w = danzi::watch::writes(p + a * PAGE, p + b * PAGE, false);
        CHECK(w.has_value());
        std::set<usize> got;
        for (auto [page, at] : w->written) {
            if (at == w->now) {
                got.insert(page - first);
            }
        }
        return got;
    };
    std::set<usize> truth;   
    ask(0, BLOCKS);
    for (int round = 0; round < 3000; ++round) {

        int n = static_cast<int>(rng() % 4);
        for (int k = 0; k < n; ++k) {
            usize b = rng() % 4 == 0 ? rng() % 4 : rng() % BLOCKS;
            usize off = rng() % PAGE;
            if (rng() % 8 == 0) {

                danzi::watch::writing(p + b * PAGE + off, 1);
                SIZE_T done = 0;
                std::uint8_t v = static_cast<std::uint8_t>(round);
                CHECK(WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p + b * PAGE + off), &v, 1, &done));
            } else {
                *reinterpret_cast<volatile std::uint8_t*>(p + b * PAGE + off) = static_cast<std::uint8_t>(round);
            }
            truth.insert(b);
        }
        auto got = ask(0, BLOCKS);
        for (usize b : truth) {
            if (!got.contains(b)) {
                check::fail(__FILE__, __LINE__, "round " + std::to_string(round) + ": block " + std::to_string(b) + " written and not reported");
            }
        }
        truth.clear();
    }
    auto after = danzi::hybrid::stats();

    CHECK(after.faults > before.faults);
    CHECK(after.made_cold > before.made_cold);
    danzi::watch::title_memory_remapped(p, BLOCKS * PAGE);
    VirtualFree(reinterpret_cast<void*>(p), 0, MEM_RELEASE);
}
