// SPDX-License-Identifier: GPL-3.0-or-later


#include <windows.h>

#include <set>
#include <vector>

#include "check.hpp"
#include "core/memcopy.hpp"
#include "core/readback.hpp"
#include "gpu/rebar.hpp"

namespace rb = danzi::gpu::rebar;
using danzi::memcopy::lane_hash;
using danzi::memcopy::RangeSet;

namespace {

constexpr std::uint64_t MIB = std::uint64_t{1} << 20;
constexpr std::uint64_t GIB = std::uint64_t{1} << 30;

rb::Ask ask(std::uint8_t level, rb::Kind kind) {
    rb::Ask a;
    a.level = level;
    a.supported = true;
    a.kind = kind;
    a.size = 4 * MIB;
    a.cpu_read = false;
    a.placed = 0;
    a.most = GIB;
    a.card = std::make_pair(11 * GIB, 2 * GIB);
    return a;
}

}   

TEST(rebar_the_switch_reads_only_one_and_two) {
    CHECK_EQ(rb::level_of(std::nullopt), 0);
    CHECK_EQ(rb::level_of("0"), 0);
    CHECK_EQ(rb::level_of(" 1 "), 1);
    CHECK_EQ(rb::level_of("2"), 2);
    CHECK_EQ(rb::level_of("yes"), 0);
    CHECK_EQ(rb::level_of("3"), 0);
}

TEST(rebar_off_or_unsupported_places_nothing) {
    CHECK(rb::decide(ask(0, rb::Kind::Window)) == rb::Decision::Off);
    auto a = ask(2, rb::Kind::Window);
    a.supported = false;
    CHECK(rb::decide(a) == rb::Decision::Unsupported);
}

TEST(rebar_level_one_places_windows_only_level_two_buffers_too) {
    CHECK(rb::decide(ask(1, rb::Kind::Window)) == rb::Decision::Rebar);
    CHECK(rb::decide(ask(1, rb::Kind::Buffer)) == rb::Decision::NotThisKind);
    CHECK(rb::decide(ask(2, rb::Kind::Buffer)) == rb::Decision::Rebar);
}

TEST(rebar_cpu_read_buffers_never_go_there) {
    for (std::uint8_t level : {1, 2}) {
        for (auto kind : {rb::Kind::Window, rb::Kind::Buffer}) {
            auto a = ask(level, kind);
            a.cpu_read = true;
            CHECK(rb::decide(a) == rb::Decision::CpuRead);
        }
    }
}

TEST(rebar_the_budget_holds_to_the_byte) {
    auto a = ask(1, rb::Kind::Window);
    a.placed = GIB - 4 * MIB;
    CHECK(rb::decide(a) == rb::Decision::Rebar);
    a.placed += 1;
    CHECK(rb::decide(a) == rb::Decision::OverBudget);
    a.placed = ~std::uint64_t{0};
    CHECK(rb::decide(a) == rb::Decision::OverBudget);
}

TEST(rebar_a_fifth_of_the_card_stays_free) {
    auto a = ask(1, rb::Kind::Window);
    a.card = std::make_pair(10 * GIB, 8 * GIB - 4 * MIB);
    CHECK(rb::decide(a) == rb::Decision::Rebar);
    a.card = std::make_pair(10 * GIB, 8 * GIB);
    CHECK(rb::decide(a) == rb::Decision::CardFull);
    a.card.reset();
    CHECK(rb::decide(a) == rb::Decision::Rebar);
}

TEST(rebar_default_budget_is_a_quarter_of_the_card_at_most_4_gib) {
    CHECK_EQ(rb::default_most(std::nullopt), 256 * MIB);
    CHECK_EQ(rb::default_most(11 * GIB), 11 * GIB / 4);
    CHECK_EQ(rb::default_most(30 * GIB), 4 * GIB);
}

TEST(memcopy_the_lane_hash_sees_every_one_byte_change) {
    std::vector<std::uint8_t> data(4099);
    for (std::uint32_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 13);
    }
    auto base = lane_hash(data.data(), data.size());
    CHECK(base == lane_hash(data.data(), data.size()));
    for (std::size_t at : {0, 1, 7, 8, 63, 64, 65, 511, 2048, 4095, 4096, 4098}) {
        std::uint8_t was = data[at];
        for (std::uint8_t flip : {std::uint8_t{1}, std::uint8_t{0x80}, std::uint8_t{0xff}}) {
            data[at] = was ^ flip;
            CHECK(lane_hash(data.data(), data.size()) != base);
        }
        data[at] = was;
    }
    CHECK(lane_hash(data.data(), data.size()) == base);
}

TEST(memcopy_the_lane_hash_tells_lengths_and_zeros_apart) {
    std::vector<std::uint8_t> zeros(256, 0);
    std::set<std::array<std::uint8_t, 32>> seen;
    for (std::size_t n = 0; n <= 256; ++n) {
        CHECK(seen.insert(lane_hash(zeros.data(), n)).second);
    }
}

TEST(memcopy_the_lane_hash_sees_swapped_words) {
    std::vector<std::uint8_t> a(1024, 0);
    a[0] = 1;
    a[8] = 2;
    auto b = a;
    std::swap(b[0], b[8]);
    CHECK(lane_hash(a.data(), a.size()) != lane_hash(b.data(), b.size()));
    auto c = a;
    c[64] = 1;
    c[0] = 0;
    CHECK(lane_hash(a.data(), a.size()) != lane_hash(c.data(), c.size()));
}

TEST(memcopy_a_range_set_finds_overlaps_only) {
    RangeSet s;
    s.insert(0x1000, 0x2000);
    s.insert(0x100000, 0x500000);
    CHECK(s.overlaps(0x1fff, 0x3000));
    CHECK(!s.overlaps(0x2000, 0x3000));
    CHECK(!s.overlaps(0x0, 0x1000));
    CHECK(s.overlaps(0x4fffff, 0x600000));
    CHECK(s.overlaps(0x200000, 0x200001));
    CHECK(!s.overlaps(0x500000, 0x600000));
    CHECK(!s.overlaps(5, 5));
}

TEST(memcopy_a_range_set_counts_and_lets_go) {
    RangeSet s;
    s.insert(100, 200);
    s.insert(100, 200);
    s.insert(100, 150);
    CHECK_EQ(s.size(), 2u);
    CHECK(s.remove_at(100));
    CHECK(s.remove_at(100));
    CHECK(s.overlaps(120, 130));
    CHECK(s.remove_at(100));
    CHECK(!s.overlaps(0, 1000));
    CHECK(!s.remove_at(100));
    CHECK(!s.remove(1, 2));
    s.insert(7, 7);
    CHECK_EQ(s.size(), 0u);
}

TEST(memcopy_check_turns_come_once_in_every_n_frames) {
    std::size_t start = 0x12340000;
    int turns = 0;
    for (std::uint64_t f = 0; f < 64; ++f) {
        turns += danzi::memcopy::check_turn(f, start, 8) ? 1 : 0;
        CHECK(!danzi::memcopy::check_turn(f, start, 0));
    }
    CHECK_EQ(turns, 8);
    for (std::uint64_t f = 0; f < 5; ++f) {
        CHECK(danzi::memcopy::check_turn(f, start, 1));
    }
    CHECK_EQ(danzi::memcopy::hash_every_of(std::nullopt), 8u);
    CHECK_EQ(danzi::memcopy::hash_every_of(" 32 "), 32u);
    CHECK_EQ(danzi::memcopy::hash_every_of("x"), 8u);
}


TEST(readback_this_test_binary_has_its_own_functions_in_its_table) {
    auto base = reinterpret_cast<danzi::usize>(GetModuleHandleW(nullptr));


    auto f = reinterpret_cast<danzi::usize>(&danzi::readback::function_at);
    auto r = danzi::readback::function_at(base, static_cast<std::uint32_t>(f - base));
    CHECK(r.has_value());
    CHECK_EQ(r->first, f);
    CHECK(r->second > r->first);
}
