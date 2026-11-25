// SPDX-License-Identifier: GPL-3.0-or-later


#include <vector>

#include "check.hpp"
#include "core/shadow.hpp"
#include "core/windows.hpp"

using danzi::windows::Window;
namespace w = danzi::windows;

TEST(windows_an_ask_takes_the_longest_window_that_holds_it) {
    std::uint64_t cell = 0xd12000000ull;
    std::uint64_t ask = cell + 0x90000, end = cell + 0xb0000;
    CHECK(!w::holder(std::vector<Window>{}, ask, end));
    std::vector<Window> there = {
        {cell, 0x40000},             
        {cell + 0x80000, 0x20000},   
        {cell + 0x80000, 0x40000},   
        {cell + 0x40000, 0x100000},  
        {cell + 0xa0000, 0x400000},  
    };
    CHECK(w::holder(there, ask, end) == Window(cell + 0x40000, 0x100000));
    std::vector<Window> two = {{cell + 0x80000, 0x80000}, {cell + 0x40000, 0x80000}};
    CHECK(w::holder(two, ask, end) == Window(cell + 0x40000, 0x80000));
}

TEST(windows_overlapping_windows_cover_their_memory_once) {
    CHECK_EQ(w::cover(std::vector<Window>{}), 0u);
    std::vector<Window> apart = {{0x100000, 0x40000}, {0x200000, 0x40000}};
    CHECK_EQ(w::cover(apart), 0x80000u);
    std::vector<Window> over = {{0x100000, 0x100000}, {0x140000, 0x40000}, {0x1c0000, 0x80000}, {0x240000, 0x40000}};
    CHECK_EQ(w::cover(over), 0x180000u);
}

TEST(windows_tails_run_to_the_cells_end_and_the_longest_is_taken) {
    std::uint64_t cell = 0xd12000000ull;
    std::uint64_t ask = cell + 0x123456;
    CHECK(w::tail_of(ask, ask + 1) == Window(cell + 0x100000, w::WINDOW - 0x100000));
    CHECK(w::tail_of(cell + 0xffff, cell + 0x10000) == Window(cell, w::WINDOW));
    auto tail = [&](std::uint64_t from) { return Window(cell + from, w::WINDOW - from); };
    CHECK(!w::longest_tail(std::vector<Window>{}, ask, ask + 1));
    CHECK(!w::longest_tail(std::vector<Window>{tail(0x200000)}, ask, ask + 1));
    std::vector<Window> there = {{cell - w::WINDOW, w::WINDOW}, {cell, 0x40000}, tail(0x80000), tail(0x100000), {cell + 0x100000, 0x40000}, tail(0x200000)};
    CHECK(w::longest_tail(there, ask, ask + 1) == tail(0x80000));
    std::vector<Window> with_whole = {{cell, w::WINDOW}, tail(0x80000)};
    CHECK(w::longest_tail(with_whole, ask, ask + 1) == Window(cell, w::WINDOW));
}

TEST(windows_the_grid_is_a_power_of_two_or_the_default) {
    CHECK_EQ(w::grid(std::nullopt), w::WINDOW);
    CHECK_EQ(w::grid(4096), w::WINDOW);
    CHECK_EQ(w::grid(1024), std::uint64_t{1} << 20);
    CHECK_EQ(w::grid(256), std::uint64_t{256} << 10);
    CHECK_EQ(w::grid(64), std::uint64_t{64} << 10);
    for (std::uint64_t bad : {0, 32, 100, 1000, 8192}) {
        CHECK_EQ(w::grid(bad), w::WINDOW);
    }
}

TEST(windows_hold_their_ask_on_the_grid) {
    std::uint64_t address = 0xd12345678ull, end = address + 0x9000;
    CHECK(w::window_of(address, end, w::WINDOW) == Window(0xd12000000ull, std::uint64_t{4} << 20));
    CHECK(w::window_of(address, end, std::uint64_t{1} << 20) == Window(0xd12300000ull, std::uint64_t{1} << 20));
    CHECK(w::window_of(address, end, std::uint64_t{256} << 10) == Window(0xd12340000ull, std::uint64_t{256} << 10));
    end = address + 0x40000;
    CHECK(w::window_of(address, end, std::uint64_t{256} << 10) == Window(0xd12340000ull, std::uint64_t{512} << 10));
    for (std::uint64_t g : {std::uint64_t{64} << 10, std::uint64_t{256} << 10, std::uint64_t{1} << 20, w::WINDOW}) {
        auto [start, len] = w::window_of(address, end, g);
        CHECK(start <= address && start + len >= end);
        CHECK(start % g == 0 && len % g == 0);
    }
}

TEST(windows_copied_pages_tracks_uncopied_count_and_fast_path_condition) {
    danzi::shadow::CopiedPages pages(4);
    CHECK(!pages.all_copied());
    pages.set(0, 10);
    pages.set(0, 20);
    CHECK_EQ(pages.at.size() - pages.uncopied, 1u);
    CHECK(!pages.all_copied());
    pages.set(1, 10);
    pages.set(2, 10);
    pages.set(3, 10);
    CHECK(pages.all_copied());
    CHECK_EQ(pages.at[2], 10u);
    pages.set(1, 0);
    CHECK(!pages.all_copied());
    CHECK_EQ(pages.at.size() - pages.uncopied, 3u);
}
