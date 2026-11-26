// SPDX-License-Identifier: GPL-3.0-or-later
// Title memory by address (`memory.rs`; DanziLoader / victory-perf, see
// NOTICE.md).
//
// The copies are the ones every other access uses (a title buffer's copy,
// or the 4 MiB window of memory outside buffers), so a root view and a raw
// address reach the same host bytes. Kept here:
//
// - the directory: per 64 KiB page of title address space the number of
//   the copy that holds the page (0: none) and the page's byte offset in
//   it; one buffer for all lists, entries only ever go from empty to filled;
// - per view heap a run of slots with a raw unordered access view of each
//   numbered copy (slot 0 of the run: a null view);
// - the faults: the page numbers shaders found empty, read back here once a
//   frame, so the copies exist from the next frame on.
//
// shortcut (the reference's): a page not there when a shader touches it is
// a lost access (the read gives 0, the store is dropped). Before a dispatch
// what it names is made ready: the copy around each root view, each pair of
// root constants that is a title address, each root constant that is one in
// 256-byte units, and the 4 MiB after each. What a stage reaches only
// through pointers it reads out of memory is not.
// shortcut (the reference's): a numbered copy is kept for the rest of the
// run, at most `COPIES` of them; every dispatch of such a shader brings all
// numbered copies up to date (once per list) and, if it can store, declares
// all of them written.

#include "memory.hpp"

#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.hpp"
#include "host.hpp"
#include "host_heap.hpp"
#include "shader_cache.hpp"
#include "shadows.hpp"
#include "sync.hpp"

namespace danzi::d3d12x {

namespace {

constexpr std::uint32_t MEMORY_PAGES = 1u << 21, MEMORY_FAULTS = 4096;

constexpr std::uint32_t COPIES = 1023;

constexpr std::uint64_t CELL = 4u << 20, PAGE = 1u << 16;

struct Buffer {
    ComPtr<ID3D12Resource> resource;
    std::uint32_t* data = nullptr;
    std::uint64_t gpu = 0;
};

struct Memory {
    Buffer directory, faults;

    std::vector<std::shared_ptr<shadow::Shadow>> copies;


    std::unordered_map<const HostHeap*, std::pair<std::uint32_t, std::uint32_t>> heaps;

    std::uint64_t frame = UINT64_MAX;

    std::unordered_set<std::uint64_t> refused;

    static std::unique_ptr<Memory> make();
    void map(std::uint64_t address);
    void take_faults();

    void name(std::uint64_t address) {
        map(address);
        map((address & ~(CELL - 1)) + CELL);
    }
};

std::mutex MEMORY_LOCK;
std::unique_ptr<Memory> MEMORY;

std::unique_ptr<Memory> Memory::make() {
    Host* h = host();
    if (!h) {
        return nullptr;
    }
    auto buffer = [&](std::uint64_t dwords) -> std::optional<Buffer> {
        auto b = h->device().shared_buffer(dwords * 4);
        if (!b) {
            return std::nullopt;
        }
        std::memset(b->second, 0, static_cast<std::size_t>(dwords * 4));
        return Buffer{b->first, reinterpret_cast<std::uint32_t*>(b->second), b->first->GetGPUVirtualAddress()};
    };
    log::info("victory_gpu::memory", "title memory by address: a shader reads or writes through raw addresses");
    auto directory = buffer(std::uint64_t{MEMORY_PAGES} * 2);
    auto faults = buffer(MEMORY_FAULTS);
    if (!directory || !faults) {
        return nullptr;
    }
    auto m = std::make_unique<Memory>();
    m->directory = std::move(*directory);
    m->faults = std::move(*faults);
    return m;
}


void Memory::map(std::uint64_t address) {
    std::uint64_t page = address / PAGE;

    if (page >= MEMORY_PAGES - 1) {
        return;
    }
    volatile std::uint32_t* entry = directory.data + page * 2;
    if (entry[0] != 0) {
        return;
    }
    std::uint64_t start = page * PAGE;


    if (refused.contains(page)) {
        return;
    }
    if (!committed(reinterpret_cast<const void*>(start), PAGE)) {
        refused.insert(page);
        return;
    }


    auto whole = [&](const std::shared_ptr<shadow::Shadow>& s) {
        return s && start >= s->start() && start - s->start() < s->len() && shadow_left(*s, start) >= PAGE;
    };
    std::shared_ptr<shadow::Shadow> shadow = shadow_find(start, 1);
    if (!whole(shadow)) {
        shadow = shadow_find(start, PAGE);
        if (!whole(shadow)) {
            return;
        }
    }
    if ((shadow->buffer()->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        return;
    }
    auto at = std::find(copies.begin(), copies.end(), shadow);
    std::size_t n;
    if (at != copies.end()) {
        n = static_cast<std::size_t>(at - copies.begin());
    } else if (copies.size() < COPIES) {
        copies.push_back(shadow);
        n = copies.size() - 1;


        std::uint64_t first = shadow->start(), len = shadow->len();
        for (std::uint64_t p = (first + PAGE - 1) / PAGE * PAGE; p + PAGE <= first + len && p / PAGE < MEMORY_PAGES - 1; p += PAGE) {
            volatile std::uint32_t* other = directory.data + (p / PAGE) * 2;
            if (other[0] == 0) {

                other[1] = static_cast<std::uint32_t>(p - first);
                other[0] = static_cast<std::uint32_t>(n + 1);
            }
        }
    } else {
        static std::once_flag once;
        std::call_once(once, [] {
            log::warn("victory_gpu::memory", "title memory by address: no number left for another copy; its pages read zeros", {{"COPIES", log::debug(COPIES)}});
        });
        return;
    }
    entry[1] = static_cast<std::uint32_t>(start - shadow->start());
    entry[0] = static_cast<std::uint32_t>(n + 1);
}


void Memory::take_faults() {
    std::vector<std::uint32_t> pages;
    volatile std::uint32_t* f = faults.data;
    for (std::uint32_t i = 1; i < MEMORY_FAULTS; ++i) {

        std::uint32_t page = f[i];
        if (page != 0) {
            pages.push_back(page);
            f[i] = 0;
        }
    }
    if (!pages.empty()) {

        static std::atomic<std::uint32_t> said{0};
        if (said.fetch_add(1, std::memory_order_relaxed) < 50) {
            log::warn("victory_gpu::memory",
                      "title memory by address: shaders touched pages that had no copy (those accesses read 0 or were dropped); the copies are made now",
                      {{"pages", log::debug(static_cast<std::uint64_t>(pages.size()))}, {"first", log::hex(std::uint64_t{pages[0]} * PAGE)}});
        }
    }
    for (std::uint32_t page : pages) {
        map(std::uint64_t{page} * PAGE);
    }
}

}   

std::optional<MemoryReady> memory_ready(HostList& list, HostHeap& heap, std::span<const std::uint64_t> addresses,
                                        std::span<const std::uint32_t> constants, bool writes) {
    std::lock_guard lock(MEMORY_LOCK);
    if (!MEMORY) {
        MEMORY = Memory::make();
    }
    Memory* m = MEMORY.get();
    if (!m) {
        return std::nullopt;
    }
    std::uint64_t frame = frame_current();
    if (m->frame != frame) {
        m->frame = frame;
        m->refused.clear();
        m->take_faults();
    }
    std::size_t numbered = m->copies.size();
    for (std::uint64_t a : addresses) {
        m->name(a);
    }


    for (std::size_t i = 0; i < constants.size(); ++i) {
        std::uint32_t c = constants[i];
        if (i + 1 < constants.size() && constants[i + 1] >= 1 && constants[i + 1] <= 0xFF && c >= 0x10000) {
            m->name(std::uint64_t{c} | std::uint64_t{constants[i + 1]} << 32);
        }
        if (c >= 0x01000000) {
            m->name(std::uint64_t{c} << 8);
        }
    }
    std::uint32_t base, made;
    if (auto it = m->heaps.find(&heap); it != m->heaps.end()) {
        std::tie(base, made) = it->second;
    } else {

        auto first = heap.dynamic(COPIES + 1);
        if (!first) {
            return std::nullopt;
        }
        base = *first;
        made = 0;
        heap.null_raw_uav_at(base);
    }
    for (std::size_t n = made; n < m->copies.size(); ++n) {
        const auto& s = m->copies[n];
        heap.raw_uav_at(base + 1 + static_cast<std::uint32_t>(n), s->buffer(), 0, s->len());
    }
    m->heaps[&heap] = {base, static_cast<std::uint32_t>(m->copies.size())};


    constexpr usize READ = SIZE_MAX - 0x6d, WRITE = SIZE_MAX - 0x6e;
    bool first_read = list.first_use(READ);
    bool first_write = writes && list.first_use(WRITE);
    for (std::size_t n = 0; n < m->copies.size(); ++n) {
        const auto& s = m->copies[n];
        if (first_read || n >= numbered) {
            shadow_refresh(list, s, s->start(), s->len());
        }
        if (writes && (first_write || n >= numbered)) {
            shadow_gpu_writes(list, s, s->start(), s->len());
        }
    }
    if (first_read) {
        list.keep(ComPtr<IUnknown>(m->directory.resource.Get()));
        list.keep(ComPtr<IUnknown>(m->faults.resource.Get()));
    }
    return MemoryReady{m->directory.gpu, m->faults.gpu, base};
}

std::uint64_t memory_copies() {
    std::lock_guard lock(MEMORY_LOCK);
    return MEMORY ? MEMORY->copies.size() : 0;
}

}   
