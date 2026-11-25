// SPDX-License-Identifier: GPL-3.0-or-later
// Port of parts of crates/victory-gpu/src/memcopy.rs (DanziLoader /


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

}   
