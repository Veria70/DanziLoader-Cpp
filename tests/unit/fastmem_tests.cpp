// SPDX-License-Identifier: GPL-3.0-or-later


#include <optional>
#include <vector>

#include "check.hpp"
#include "core/fastmem.hpp"
#include "core/win.hpp"

using danzi::usize;
namespace fm = danzi::fastmem;

namespace {

std::optional<usize> first_ref(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, usize n) {
    for (usize i = 0; i < n; ++i) {
        if (a[i] != b[i]) return i;
    }
    return std::nullopt;
}

std::optional<usize> last_ref(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, usize n) {
    for (usize i = n; i > 0; --i) {
        if (a[i - 1] != b[i - 1]) return i - 1;
    }
    return std::nullopt;
}

std::vector<std::pair<usize, usize>> runs_ref(const std::vector<std::uint8_t>& now, const std::vector<std::uint8_t>& was) {
    std::vector<std::pair<usize, usize>> runs;
    usize i = 0, n = now.size();
    while (i < n) {
        while (i < n && now[i] == was[i]) ++i;
        if (i >= n) break;
        usize first = i;
        while (i < n && now[i] != was[i]) ++i;
        runs.emplace_back(first, i - first);
    }
    return runs;
}

}   


TEST(differences_are_found_as_byte_by_byte) {
    std::vector<std::uint8_t> base(200);
    for (usize i = 0; i < base.size(); ++i) base[i] = static_cast<std::uint8_t>(i * 7 + 3);
    CHECK(!fm::first_difference(base.data(), base.data(), base.size()));
    CHECK(fm::equal(base.data(), base.data(), base.size()));
    for (usize len : {0, 1, 7, 8, 9, 16, 31, 32, 33, 61, 64, 65, 127, 128, 129, 200}) {
        for (usize at = 0; at < len; at += (len > 70 ? 5 : 1)) {
            for (usize other = at; other < len; other += (len > 70 ? 7 : 1)) {
                auto b = base;
                b[at] ^= 0x80;
                b[other] ^= 0x01;
                CHECK_EQ(fm::first_difference(base.data(), b.data(), len), first_ref(base, b, len));
                CHECK_EQ(fm::last_difference(base.data(), b.data(), len), last_ref(base, b, len));
                CHECK_EQ(fm::equal(base.data(), b.data(), len), !first_ref(base, b, len).has_value());
            }
        }
    }
}


TEST(changed_runs_are_exactly_the_bytes_that_differ) {
    std::vector<std::uint8_t> was(300);
    for (usize i = 0; i < was.size(); ++i) was[i] = static_cast<std::uint8_t>(i * 13 + 5);
    std::vector<std::vector<usize>> cases = {{},
                                             {0},
                                             {299},
                                             {7, 8, 9},
                                             {7, 9},
                                             {15, 16, 17, 23, 24, 64, 65, 66, 67, 68, 69, 70, 71, 72, 298, 299},
                                             {31, 32},
                                             {30, 31, 32, 33, 63, 64, 95, 96, 97}};
    std::vector<usize> all;
    for (usize i = 0; i < 300; ++i) all.push_back(i);
    cases.push_back(all);
    std::vector<usize> stripes;
    for (usize i = 20; i < 280; ++i) {
        if ((i / 3) % 2) stripes.push_back(i);
    }
    cases.push_back(stripes);
    for (const auto& changes : cases) {
        auto now = was;
        for (usize at : changes) now[at] ^= 0x41;
        CHECK_EQ(fm::changed_runs(now.data(), was.data(), now.size()), runs_ref(now, was));
    }
}


TEST(the_merge_stores_only_what_the_card_changed) {
    for (usize n : {200, 31, 32, 64, 97}) {
        std::vector<std::uint8_t> held0(n);
        for (usize i = 0; i < n; ++i) held0[i] = static_cast<std::uint8_t>(i * 5 + 1);
        auto now = held0;
        for (usize at : {0, 3, 4, 5, 64, 199}) {
            if (at < n) now[at] ^= 0x33;
        }
        auto title = held0;
        for (usize at : {1, 5, 100}) {
            if (at < n) title[at] = 0xee;
        }
        auto title0 = title;
        auto held = held0;
        CHECK(fm::merge_piece(now.data(), held.data(), title.data(), n));
        for (usize at = 0; at < n; ++at) {
            if (now[at] != held0[at]) {
                CHECK_EQ(title[at], now[at]);
                CHECK_EQ(held[at], now[at]);
            } else {
                CHECK_EQ(title[at], title0[at]);
                CHECK_EQ(held[at], held0[at]);
            }
        }
        auto title1 = title, held1 = held;
        CHECK(!fm::merge_piece(now.data(), held.data(), title.data(), n));
        CHECK(title == title1);
        CHECK(held == held1);
    }
}


TEST(the_copy_back_merge_takes_the_gpus_bytes_only) {
    for (usize n : {1, 31, 32, 33, 100, 4096}) {
        std::vector<std::uint8_t> old(n), host(n), title(n);
        for (usize i = 0; i < n; ++i) {
            old[i] = static_cast<std::uint8_t>(i * 3);
            host[i] = old[i];
            title[i] = static_cast<std::uint8_t>(i * 3 + (i % 5 == 0 ? 1 : 0));
        }
        auto t0 = title;
        CHECK_EQ(fm::merge_gpu_into_title(title.data(), host.data(), old.data(), n), false);
        CHECK(title == t0);
        CHECK(host == t0);
        for (usize i = 0; i < n; i += 7) host[i] = static_cast<std::uint8_t>(~old[i]);
        auto h1 = host;
        title = t0;
        bool changed = fm::merge_gpu_into_title(title.data(), host.data(), old.data(), n);
        bool expect = false;
        for (usize i = 0; i < n; ++i) {
            std::uint8_t want = h1[i] != old[i] ? h1[i] : t0[i];
            CHECK_EQ(title[i], want);
            CHECK_EQ(host[i], want);
            expect |= want != t0[i];
        }
        CHECK_EQ(changed, expect);
    }
}

TEST(a_copy_from_an_unmapped_page_is_reported) {
    auto* p = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 3 * 4096, MEM_RESERVE, PAGE_READWRITE));
    CHECK(p != nullptr);
    VirtualAlloc(p, 4096, MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc(p + 2 * 4096, 4096, MEM_COMMIT, PAGE_READWRITE);
    std::vector<std::uint8_t> out(3 * 4096);
    CHECK(fm::try_copy(out.data(), p, 4096));
    CHECK(!fm::try_copy(out.data(), p, 3 * 4096));
    CHECK(!fm::try_copy(p + 4096, out.data(), 16));   
    DWORD old = 0;
    VirtualProtect(p, 4096, PAGE_READONLY, &old);
    CHECK(fm::try_copy(out.data(), p, 4096));
    CHECK(!fm::try_copy(p, out.data(), 16));   
    VirtualFree(p, 0, MEM_RELEASE);
}
