// SPDX-License-Identifier: GPL-3.0-or-later


#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "core/inplace.hpp"
#include "core/watch.hpp"
#include "core/win.hpp"

using danzi::usize;
using namespace danzi::watch;
using danzi::watch::detail::guard;
using danzi::watch::detail::is_view;
using danzi::watch::detail::writable;

namespace {

constexpr usize BLOCK = PAGE;
constexpr usize KERNEL_PAGE = 4096;

std::vector<usize> page_run(usize from, usize to) {
    std::vector<usize> v;
    for (usize p = from; p < to; ++p) v.push_back(p);
    return v;
}

using Pages = std::optional<std::vector<usize>>;
Pages some(std::vector<usize> v) { return v; }


struct TwoViews {
    usize section, one, two;
};
TwoViews two_views(usize bytes) {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(bytes), nullptr);
    CHECK(section != nullptr);
    auto one = reinterpret_cast<usize>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, bytes));
    auto two = reinterpret_cast<usize>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, bytes));
    CHECK(one != 0);
    CHECK(two != 0);
    danzi::inplace::title_memory_view(one, bytes, reinterpret_cast<usize>(section), 0);
    danzi::inplace::title_memory_view(two, bytes, reinterpret_cast<usize>(section), 0);
    return {reinterpret_cast<usize>(section), one, two};
}

void drop_views(usize section, std::initializer_list<usize> views, usize bytes) {
    for (usize view : views) {
        title_memory_remapped(view, bytes);
        UnmapViewOfFile(reinterpret_cast<void*>(view));
    }
    CloseHandle(reinterpret_cast<HANDLE>(section));
}

}   

TEST(written_pages_are_reported_to_every_asker) {
    auto p = reinterpret_cast<usize>(VirtualAlloc(nullptr, 4 * PAGE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    CHECK(p != 0);
    usize start = p, end = p + 4 * PAGE;
    auto before = writes(start, end, false);
    CHECK(before.has_value());
    CHECK(before->written.empty());
    *reinterpret_cast<volatile std::uint8_t*>(p + PAGE + 8) = 1;
    auto after = writes(start, end, false);
    CHECK(after.has_value());
    CHECK_EQ(after->written, (std::vector<PageWrite>{{p / PAGE + 1, after->now}}));

    auto again = writes(p + PAGE, p + 2 * PAGE, false);
    CHECK(again.has_value());
    CHECK(again->now > after->now);
    CHECK_EQ(again->written, (std::vector<PageWrite>{{p / PAGE + 1, after->now}}));
    std::vector<std::uint8_t> plain(PAGE);
    CHECK(!writes(reinterpret_cast<usize>(plain.data()), reinterpret_cast<usize>(plain.data()) + PAGE, true).has_value());
    VirtualFree(reinterpret_cast<void*>(p), 0, MEM_RELEASE);
}


TEST(kernel_reads_into_guarded_blocks_after_opening) {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(2 * BLOCK), nullptr);
    CHECK(section != nullptr);
    auto view = reinterpret_cast<usize>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 2 * BLOCK));
    CHECK(view != 0);
    CHECK(guard(view, view + 2 * BLOCK).has_value());   
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"dzl", 0, path);
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    DWORD n = 0;
    WriteFile(file, "from the disk", 13, &n, nullptr);

    auto read = [&] {
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        DWORD got = 0;
        return ReadFile(file, reinterpret_cast<void*>(view + 100), 13, &got, nullptr) && got == 13;
    };
    CHECK(!read());   
    writing(view + 100, 13);
    CHECK(read());
    CHECK(std::memcmp(reinterpret_cast<void*>(view + 100), "from the disk", 13) == 0);
    CHECK_EQ(guard(view, view + 2 * BLOCK), some(page_run(view / PAGE, view / PAGE + 1)));
    CloseHandle(file);
    UnmapViewOfFile(reinterpret_cast<void*>(view));
    title_memory_remapped(view, 2 * BLOCK);
    CloseHandle(section);
}

TEST(writes_in_partly_committed_view_blocks_are_reported) {
    constexpr usize SIZE = 3 * BLOCK;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, static_cast<DWORD>(SIZE), nullptr);
    CHECK(section != nullptr);
    void* v = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, SIZE);
    CloseHandle(section);
    CHECK(v != nullptr);
    usize start = reinterpret_cast<usize>(v);
    auto commit = [](usize at, usize len) {
        CHECK_EQ(reinterpret_cast<usize>(VirtualAlloc(reinterpret_cast<void*>(at), len, MEM_COMMIT, PAGE_READWRITE)), at);
    };
    usize early = start + BLOCK + 3 * KERNEL_PAGE;
    usize late = start + 2 * BLOCK + 3 * KERNEL_PAGE;
    commit(start, BLOCK);
    commit(early, KERNEL_PAGE);
    auto refresh = [&] { return *guard(start, start + SIZE); };
    CHECK_EQ(refresh(), (std::vector<usize>{start / PAGE, early / PAGE, late / PAGE}));

    auto again = refresh();
    CHECK(std::find(again.begin(), again.end(), late / PAGE) == again.end());
    commit(late, KERNEL_PAGE);   
    for (usize at : {early, late}) {
        CHECK(writable(at));
        *reinterpret_cast<volatile std::uint8_t*>(at) = 1;
    }
    CHECK_EQ(refresh(), (std::vector<usize>{early / PAGE, late / PAGE}));
    title_memory_remapped(start, SIZE);
    UnmapViewOfFile(v);
}

namespace {
struct Placeholder {
    usize p;
    HANDLE section;
};
Placeholder placeholder_and_section(usize blocks, usize section_blocks) {
    auto p = reinterpret_cast<usize>(VirtualAlloc2(GetCurrentProcess(), nullptr, blocks * BLOCK, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    CHECK(p != 0);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(section_blocks * BLOCK), nullptr);
    CHECK(section != nullptr);
    return {p, section};
}
void* map_block(const Placeholder& ph, usize block) {
    VirtualFree(reinterpret_cast<void*>(ph.p + block * BLOCK), BLOCK, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    void* view = MapViewOfFile3(ph.section, GetCurrentProcess(), reinterpret_cast<void*>(ph.p + block * BLOCK), block * BLOCK, BLOCK,
                                MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    CHECK_EQ(reinterpret_cast<usize>(view), ph.p + block * BLOCK);
    return view;
}
}   


TEST(a_range_that_begins_in_an_unmapped_block_is_guarded) {
    auto ph = placeholder_and_section(3, 3);
    usize p = ph.p;
    auto pages = [&](usize block) { return page_run((p + block * BLOCK) / PAGE, (p + (block + 1) * BLOCK) / PAGE); };
    std::vector<usize> all;
    for (usize b = 0; b < 3; ++b) {
        auto x = pages(b);
        all.insert(all.end(), x.begin(), x.end());
    }

    CHECK(is_view(p, p + 3 * BLOCK));
    void* middle = map_block(ph, 1);
    title_memory_remapped(p + BLOCK, BLOCK);

    CHECK(is_view(p, p + 3 * BLOCK));
    CHECK_EQ(guard(p, p + 3 * BLOCK), some(all));
    CHECK_EQ(guard(p, p + 3 * BLOCK), some({}));   
    *reinterpret_cast<volatile std::uint8_t*>(p + BLOCK + 8) = 1;
    CHECK_EQ(guard(p, p + 3 * BLOCK), some(pages(1)));
    void* first = map_block(ph, 0);
    title_memory_remapped(p, BLOCK);
    CHECK_EQ(guard(p, p + 3 * BLOCK), some(pages(0)));
    CHECK_EQ(guard(p, p + 3 * BLOCK), some({}));

    std::uint8_t local[16]{};
    CHECK(!is_view(reinterpret_cast<usize>(local), reinterpret_cast<usize>(local) + sizeof local));
    UnmapViewOfFile2(GetCurrentProcess(), first, MEM_PRESERVE_PLACEHOLDER);
    UnmapViewOfFile2(GetCurrentProcess(), middle, MEM_PRESERVE_PLACEHOLDER);
    for (usize b = 0; b < 3; ++b) VirtualFree(reinterpret_cast<void*>(p + b * BLOCK), 0, MEM_RELEASE);
    CloseHandle(ph.section);
    title_memory_remapped(p, 3 * BLOCK);
}


TEST(unmapped_blocks_are_reported_once_until_mapped) {
    auto ph = placeholder_and_section(3, 2);
    usize p = ph.p;
    void* first = map_block(ph, 0);
    auto pages = [&](usize block) { return page_run((p + block * BLOCK) / PAGE, (p + (block + 1) * BLOCK) / PAGE); };
    std::vector<usize> all;
    for (usize b = 0; b < 3; ++b) {
        auto x = pages(b);
        all.insert(all.end(), x.begin(), x.end());
    }
    CHECK_EQ(guard(p, p + 3 * BLOCK), some(all));
    CHECK_EQ(guard(p, p + 3 * BLOCK), some({}));   
    void* second = map_block(ph, 1);
    title_memory_remapped(p + BLOCK, BLOCK);
    CHECK_EQ(guard(p, p + 3 * BLOCK), some(pages(1)));
    CHECK_EQ(guard(p, p + 3 * BLOCK), some({}));
    UnmapViewOfFile2(GetCurrentProcess(), first, MEM_PRESERVE_PLACEHOLDER);
    UnmapViewOfFile2(GetCurrentProcess(), second, MEM_PRESERVE_PLACEHOLDER);
    for (usize b = 0; b < 3; ++b) VirtualFree(reinterpret_cast<void*>(p + b * BLOCK), 0, MEM_RELEASE);
    CloseHandle(ph.section);
    title_memory_remapped(p, 3 * BLOCK);
}

TEST(a_write_through_another_view_is_reported_at_the_guarded_one) {
    constexpr usize SIZE = 2 * BLOCK;
    auto [section, guarded, other] = two_views(SIZE);
    auto pages = [&](usize block) { return (guarded + block * BLOCK) / PAGE; };
    CHECK_EQ(guard(guarded, guarded + SIZE), some({pages(0), pages(1)}));
    CHECK_EQ(guard(guarded, guarded + SIZE), some({}));   
    *reinterpret_cast<volatile std::uint8_t*>(other + BLOCK + 8) = 1;
    CHECK_EQ(guard(guarded, guarded + SIZE), some({pages(1)}));
    CHECK_EQ(guard(guarded, guarded + SIZE), some({}));
    drop_views(section, {guarded, other}, SIZE);
}

TEST(a_view_mapped_beside_a_guarded_one_makes_it_check_again) {
    constexpr usize SIZE = 2 * BLOCK;
    auto [section, guarded, other] = two_views(SIZE);
    auto pages = [&](usize block) { return (guarded + block * BLOCK) / PAGE; };
    CHECK_EQ(guard(guarded, guarded + SIZE), some({pages(0), pages(1)}));
    CHECK_EQ(guard(guarded, guarded + SIZE), some({}));
    title_memory_remapped(other, SIZE);
    danzi::inplace::title_memory_view(other, SIZE, section, 0);
    CHECK_EQ(guard(guarded, guarded + SIZE), some({pages(0), pages(1)}));
    CHECK_EQ(guard(guarded, guarded + SIZE), some({}));
    drop_views(section, {guarded, other}, SIZE);
}

TEST(a_write_told_at_one_view_is_reported_at_the_others) {
    constexpr usize SIZE = 2 * BLOCK;
    auto [section, guarded, other] = two_views(SIZE);
    auto pages = [&](usize block) { return (guarded + block * BLOCK) / PAGE; };
    CHECK_EQ(guard(guarded, guarded + SIZE), some({pages(0), pages(1)}));
    CHECK_EQ(guard(guarded, guarded + SIZE), some({}));
    writing(other + BLOCK, BLOCK);
    CHECK_EQ(guard(guarded, guarded + SIZE), some({pages(1)}));
    CHECK_EQ(guard(guarded, guarded + SIZE), some({}));

    writing(other + BLOCK / 2, BLOCK);
    CHECK_EQ(guard(guarded, guarded + SIZE), some({pages(0), pages(1)}));
    drop_views(section, {guarded, other}, SIZE);
}

TEST(a_write_is_reported_at_every_checked_view) {
    constexpr usize SIZE = 2 * BLOCK;
    auto [section, one, two] = two_views(SIZE);
    auto pages = [&](usize view, usize block) { return (view + block * BLOCK) / PAGE; };
    CHECK_EQ(guard(one, one + SIZE), some({pages(one, 0), pages(one, 1)}));
    CHECK_EQ(guard(two, two + SIZE), some({pages(two, 0), pages(two, 1)}));
    CHECK_EQ(guard(one, one + SIZE), some({}));
    CHECK_EQ(guard(two, two + SIZE), some({}));
    *reinterpret_cast<volatile std::uint8_t*>(two + BLOCK + 8) = 1;
    CHECK_EQ(guard(one, one + SIZE), some({pages(one, 1)}));
    CHECK_EQ(guard(two, two + SIZE), some({pages(two, 1)}));   
    *reinterpret_cast<volatile std::uint8_t*>(one + 8) = 1;
    CHECK_EQ(guard(two, two + SIZE), some({pages(two, 0)}));
    CHECK_EQ(guard(one, one + SIZE), some({pages(one, 0)}));   
    CHECK_EQ(guard(one, one + SIZE), some({}));
    CHECK_EQ(guard(two, two + SIZE), some({}));
    drop_views(section, {one, two}, SIZE);
}

TEST(a_page_of_many_views_is_guarded_one_view_at_a_time) {
    auto [section, one, two] = two_views(BLOCK);
    usize filler = one + (usize{1} << 45);
    for (usize n = 0; n < MANY_VIEWS; ++n) {
        danzi::inplace::title_memory_view(filler + n * BLOCK, BLOCK, section, 0);
    }
    CHECK_EQ(guard(one, one + BLOCK), some({one / PAGE}));
    CHECK(writable(two));   
    *reinterpret_cast<volatile std::uint8_t*>(one + 8) = 1;
    CHECK_EQ(guard(one, one + BLOCK), some({one / PAGE}));
    title_memory_remapped(filler, MANY_VIEWS * BLOCK);
    drop_views(section, {one, two}, BLOCK);
}

TEST(ranges_past_the_address_space_end_do_not_overflow) {
    usize top = ~usize{0} - 16;
    writing(top, PAGE);
    title_memory_remapped(top, ~usize{0});
}

TEST(reads_in_place_and_remappings_wait_for_each_other) {
    using namespace std::chrono_literals;
    std::atomic<bool> changed{false};
    {
        auto reading = std::make_unique<Reading>(title_memory_reading());
        std::atomic<bool> began{false};
        std::thread change([&] {
            began = true;
            auto changing = title_memory_remapping();
            changed = true;
        });
        while (!began) std::this_thread::yield();
        std::this_thread::sleep_for(100ms);
        CHECK(!changed.load());   
        reading.reset();
        change.join();
        CHECK(changed.load());
    }
    std::atomic<bool> read{false};
    {
        auto changing = std::make_unique<Remapping>();
        std::atomic<bool> began{false};
        std::thread reader([&] {
            began = true;
            auto r = title_memory_reading();
            read = true;
        });
        while (!began) std::this_thread::yield();
        std::this_thread::sleep_for(100ms);
        CHECK(!read.load());   
        changing.reset();
        reader.join();
        CHECK(read.load());
    }
}
