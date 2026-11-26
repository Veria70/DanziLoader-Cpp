// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/watch.rs (DanziLoader / victory-perf, see
// NOTICE.md).
//
// Which title memory the title wrote since a host copy took it. The runtime
// reserves title GPU memory with write watching, so the kernel keeps a dirty
// bit per 4 KiB page; asking for a range's written pages also clears them.
// Host copies of the same memory can overlap (nested buffers, textures in
// buffers), so what one asks is kept here for the others: a journal of each
// page's last write, numbered by the asking.
//
// Physical-page views (xmem's sections) cannot be write-watched. There a
// copy leaves each 64 KiB block it took read-only, and a vectored handler
// takes the first write to it: it notes the block as written and gives the
// write access back. The runtime tells us when it maps or unmaps views
// (`title_memory_remapped`), which resets their protection. Writes into
// physical pages through another view are seen too (`writing`, and every
// view of a guarded page is guarded with it, up to `MANY_VIEWS`).
#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <span>
#include <utility>
#include <vector>

#include "win.hpp"

namespace danzi::watch {


inline constexpr usize PAGE = usize{64} << 10;

inline constexpr usize MANY_VIEWS = 64;

using Range = std::pair<usize, usize>;            
using PageWrite = std::pair<usize, std::uint64_t>;   

struct Writes {
    std::uint64_t now = 0;
    std::vector<PageWrite> written;
};

struct WritesKind {
    std::uint64_t now = 0;
    std::vector<PageWrite> written;
    bool is_view = false;
};

void ask_ahead(std::span<const Range> ranges);
void ask_ahead_done();
bool once();
void ask_once(std::span<const Range> ranges);
void ask_once_end();
void wrote(usize at, usize len);
std::optional<Writes> asked(usize start, usize end);
std::optional<Writes> writes_guarded(usize start, usize end);
std::optional<WritesKind> writes_kind(usize start, usize end, bool guarded);
std::optional<Writes> writes(usize start, usize end, bool guarded);


void writing(usize at, usize len);


void views_mapped(usize section, std::uint64_t offset, usize len);

void title_memory_remapped(usize at, usize len);


class Remapping {
public:
    Remapping();
    Remapping(const Remapping&) = delete;
    Remapping& operator=(const Remapping&) = delete;

private:
    std::unique_lock<SrwLock> lock_;
};

class Reading {
public:
    Reading();
    explicit Reading(std::shared_lock<SrwLock>&& lock) : lock_(std::move(lock)) {}
    Reading(Reading&&) = default;

private:
    std::shared_lock<SrwLock> lock_;
};

Remapping title_memory_remapping();
Reading title_memory_reading();
std::optional<Reading> title_memory_reading_now();


namespace detail {

void install_handler();
std::optional<std::vector<usize>> guard(usize start, usize end);
bool is_view(usize start, usize end);
bool writable(usize at);
}   

}   
