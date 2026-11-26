// SPDX-License-Identifier: GPL-3.0-or-later
// Port of parts of crates/victory-gpu/src/memcopy.rs (DanziLoader /
// victory-perf, see NOTICE.md): the range set the read-back tags live in,
// the change hash of texture memory (`lane_hash`), and the check-hash
// schedule (`check_turn`, `hash_every_of`). The switches and counters of
// the copy path's cheaper modes come with the code that reads them
// (textures, milestone 4).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "win.hpp"

namespace danzi::memcopy {


class RangeSet {
public:
    void insert(std::uint64_t start, std::uint64_t end);

    bool remove(std::uint64_t start, std::uint64_t end);

    bool remove_at(std::uint64_t start);

    bool overlaps(std::uint64_t start, std::uint64_t end) const;
    usize size() const { return ranges_.size(); }

private:
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t> ranges_;

    std::uint64_t longest_ = 0;
};


std::array<std::uint8_t, 32> lane_hash(const std::uint8_t* bytes, usize len);


bool check_turn(std::uint64_t frame, usize start, std::uint64_t every);

std::uint64_t hash_every_of(std::optional<std::string_view> value);


enum class M { RunsKept, RunsEnded, RunsNotBlocking, BackKept, BackKeptBytes, BackGoes, Hashes, HashBytes, HashUs, ChecksSkipped, Overlapped, OverlapUs, OverlapWaitUs };
void add(M m, std::uint64_t n);


void log(double frames, bool known_writes, bool texture_overlap, std::uint64_t hash_every);

}   
