// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the selector port: the reference's own CopiedPages test, ported,
// and checks of the selector's contract on real write-watched memory.
#include "check.hpp"
#include "core/shadow.hpp"
#include "core/watch.hpp"
#include "core/win.hpp"

using danzi::usize;
using danzi::shadow::CopiedPages;
using danzi::shadow::Flights;
using danzi::shadow::Shadow;
using danzi::watch::PAGE;

TEST(copied_pages_tracks_uncopied_count_and_fast_path_condition) {
    CopiedPages pages(4);
    CHECK(!pages.all_copied());
    CHECK_EQ(pages.at.size() - pages.uncopied, usize{0});
    pages.set(0, 10);
    pages.set(0, 20);   
    CHECK_EQ(pages.at.size() - pages.uncopied, usize{1});
    CHECK(!pages.all_copied());
    pages.set(1, 10);
    pages.set(2, 10);
    pages.set(3, 10);
    CHECK(pages.all_copied());
    CHECK_EQ(pages.at.size() - pages.uncopied, usize{4});
    CHECK_EQ(pages.at[2], std::uint64_t{10});
    pages.set(1, 0);
    CHECK(!pages.all_copied());
    CHECK_EQ(pages.at.size() - pages.uncopied, usize{3});
}

TEST(flights_free_a_range_when_its_last_submission_is_done) {
    Flights f;
    f.declare({0x1000, 0x2000}, 1);
    f.declare({0x1000, 0x2000}, 1);   
    f.declare({0x1000, 0x2000}, 2);
    f.declare({0x3000, 0x4000}, 2);
    std::vector<danzi::shadow::Range> one{{0x1000, 0x2000}};
    CHECK(f.done(one).empty());   
    CHECK_EQ(f.done(one), one);
    std::vector<danzi::shadow::Range> unknown{{0x9000, 0xa000}};
    CHECK_EQ(f.done(unknown), unknown);   
}

namespace {
struct Region {
    usize p;
    usize bytes;
    explicit Region(usize n) : bytes(n) {
        p = reinterpret_cast<usize>(VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
        CHECK(p != 0);
    }
    ~Region() { VirtualFree(reinterpret_cast<void*>(p), 0, MEM_RELEASE); }
};
}   

TEST(a_refresh_copies_written_pages_and_leaves_shader_ranges_alone) {
    Region title(8 * PAGE);
    std::vector<std::uint8_t> host(6 * PAGE);
    usize start = title.p + 4096;   
    Shadow s(host.data(), start, host.size());
    CHECK_EQ(s.update(start, host.size()), host.size());   

    s.update(start, host.size());
    CHECK_EQ(s.update(start, host.size()), usize{0});
    reinterpret_cast<volatile std::uint8_t*>(title.p)[2 * PAGE + 5] = 7;
    CHECK_EQ(s.update(start, host.size()), PAGE);   
    CHECK_EQ(host[2 * PAGE + 5 - 4096], std::uint8_t{7});

    s.writing(title.p + 3 * PAGE + 100, 200);
    host[3 * PAGE + 100 - 4096] = 0xAB;
    reinterpret_cast<volatile std::uint8_t*>(title.p)[3 * PAGE + 100] = 1;
    reinterpret_cast<volatile std::uint8_t*>(title.p)[3 * PAGE + 400] = 2;
    CHECK_EQ(s.update(start, host.size()), PAGE - 200);
    CHECK_EQ(host[3 * PAGE + 100 - 4096], std::uint8_t{0xAB});
    CHECK_EQ(host[3 * PAGE + 400 - 4096], std::uint8_t{2});
    std::vector<danzi::shadow::Range> done{{title.p + 3 * PAGE + 100, title.p + 3 * PAGE + 300}};
    s.copy_back_done(done);
    CHECK(!s.writes_pending());
}
