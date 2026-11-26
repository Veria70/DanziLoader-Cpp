// SPDX-License-Identifier: GPL-3.0-or-later
// Port of parts of crates/victory-gpu/src/memcopy.rs (see memcopy.hpp).
#include "memcopy.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <string>

#include "log.hpp"
#include "readback.hpp"

namespace danzi::memcopy {

namespace {

constexpr usize COUNTERS = 13;
std::atomic<std::uint64_t> COUNT[COUNTERS]{};


std::string fixed(double v, int places) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", places, v);
    return b;
}

}   

void add(M m, std::uint64_t n) { COUNT[static_cast<usize>(m)].fetch_add(n, std::memory_order_relaxed); }

void log(double frames, bool known_writes, bool texture_overlap, std::uint64_t hash_every) {
    double c[COUNTERS];
    for (usize i = 0; i < COUNTERS; ++i) {
        c[i] = static_cast<double>(COUNT[i].exchange(0, std::memory_order_relaxed)) / std::max(frames, 1.0);
    }
    constexpr bool fast_hash = true;
    bool back = readback::enabled();
    if (!(known_writes || texture_overlap || fast_hash || hash_every != 8 || back)) {
        return;
    }
    auto g = [&](M m) { return c[static_cast<usize>(m)]; };
    log::info("victory_gpu::memcopy",
              "memcopy per frame: title memory copies at submission " + fixed(g(M::RunsKept), 0) + " kept the asking, " + fixed(g(M::RunsEnded), 0) + " ended it, " +
                  fixed(g(M::RunsNotBlocking), 0) + " late submissions not held back by them (VICTORY_KNOWN_WRITES=" + std::to_string(known_writes ? 1 : 0) +
                  "); shader-written ranges " + fixed(g(M::BackKept), 0) + " kept on the card " + fixed(g(M::BackKeptBytes) / 1e6, 1) + " MB, " +
                  fixed(g(M::BackGoes), 0) + " copied back (VICTORY_COPY_BACK_ONLY_READBACK=" + std::to_string(back ? 1 : 0) + ", " +
                  std::to_string(readback::tagged()) + " read-back buffers); texture hashes " + fixed(g(M::Hashes), 0) + " " + fixed(g(M::HashBytes) / 1e6, 1) +
                  " MB " + fixed(g(M::HashUs) / 1e3, 2) + " ms (VICTORY_FAST_HASH=" + std::to_string(fast_hash ? 1 : 0) + "), " + fixed(g(M::ChecksSkipped), 0) +
                  " check hashes left out (VICTORY_TEXTURE_HASH_EVERY=" + std::to_string(hash_every) + "); texture refreshes beside the submitting thread " +
                  fixed(g(M::Overlapped), 0) + ", " + fixed(g(M::OverlapUs) / 1e3, 2) + " ms on their threads, " + fixed(g(M::OverlapWaitUs) / 1e3, 2) +
                  " ms waited for (VICTORY_TEXTURE_OVERLAP=" + std::to_string(texture_overlap ? 1 : 0) + ")");
}

void RangeSet::insert(std::uint64_t start, std::uint64_t end) {
    if (start >= end) {
        return;
    }
    ++ranges_[{start, end}];
    longest_ = std::max(longest_, end - start);
}

bool RangeSet::remove(std::uint64_t start, std::uint64_t end) {
    auto it = ranges_.find({start, end});
    if (it == ranges_.end()) {
        return false;
    }
    if (it->second > 1) {
        --it->second;
    } else {
        ranges_.erase(it);
    }
    return true;
}

bool RangeSet::remove_at(std::uint64_t start) {
    auto it = ranges_.lower_bound({start, 0});
    if (it == ranges_.end() || it->first.first != start) {
        return false;
    }
    return remove(it->first.first, it->first.second);
}

bool RangeSet::overlaps(std::uint64_t start, std::uint64_t end) const {
    if (start >= end) {
        return false;
    }
    std::uint64_t from = start > longest_ ? start - longest_ : 0;
    for (auto it = ranges_.lower_bound({from, 0}); it != ranges_.end() && it->first < std::pair<std::uint64_t, std::uint64_t>{end, 0}; ++it) {
        auto [a, b] = it->first;
        if (a < end && start < b) {
            return true;
        }
    }
    return false;
}

namespace {

constexpr usize LANES = 8;
constexpr std::uint64_t K[LANES] = {
    0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x85ebca77c2b2ae63ull,
    0x27d4eb2f165667c5ull, 0xff51afd7ed558ccdull, 0xc4ceb9fe1a85ec53ull, 0xd6e8feb86659fd93ull,
};

inline std::uint64_t load_le(const std::uint8_t* p) {
    std::uint64_t w;
    std::memcpy(&w, p, 8);   
    return w;
}

}   

std::array<std::uint8_t, 32> lane_hash(const std::uint8_t* bytes, usize len) {
    std::uint64_t lane[LANES];
    for (usize i = 0; i < LANES; ++i) {
        lane[i] = K[i];
    }
    usize blocks = len / (8 * LANES);
    const std::uint8_t* p = bytes;
    for (usize b = 0; b < blocks; ++b, p += 8 * LANES) {

        for (usize i = 0; i < LANES; ++i) {
            lane[i] = (std::rotl(lane[i], 23) ^ load_le(p + 8 * i)) * (K[i] | 1);
        }
    }
    usize rest = len - blocks * 8 * LANES;
    for (usize j = 0; j * 8 < rest; ++j) {
        std::uint8_t w[8] = {};
        std::memcpy(w, p + 8 * j, std::min<usize>(8, rest - 8 * j));
        lane[j] = (std::rotl(lane[j], 23) ^ load_le(w)) * (K[j] | 1);
    }
    lane[0] ^= static_cast<std::uint64_t>(len);
    std::array<std::uint8_t, 32> out{};
    for (usize o = 0; o < 4; ++o) {
        std::uint64_t x = static_cast<std::uint64_t>(o) * K[7] ^ static_cast<std::uint64_t>(len);
        for (usize i = 0; i < LANES; ++i) {
            x = (x ^ std::rotl(lane[i], static_cast<int>((7 * i + 13 * o) % 64))) * (K[(i + o) % LANES] | 1);
            x ^= x >> 29;
        }
        std::memcpy(out.data() + 8 * o, &x, 8);
    }
    return out;
}

bool check_turn(std::uint64_t frame, usize start, std::uint64_t every) {
    return every != 0 && (frame + static_cast<std::uint64_t>(start >> 16)) % every == 0;
}

std::uint64_t hash_every_of(std::optional<std::string_view> value) {
    if (!value) {
        return 8;
    }

    std::string_view v = *value;
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\n' || v.front() == '\r')) {
        v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\n' || v.back() == '\r')) {
        v.remove_suffix(1);
    }
    if (!v.empty() && v.front() == '+') {
        v.remove_prefix(1);
    }
    if (v.empty()) {
        return 8;
    }
    std::uint64_t n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') {
            return 8;
        }
        std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (n > (~std::uint64_t{0} - d) / 10) {
            return 8;
        }
        n = n * 10 + d;
    }
    return n;
}

}   
