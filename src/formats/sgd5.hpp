// SPDX-License-Identifier: GPL-3.0-or-later
// `SGD5`: the engine's shader file (`crates/victory-game/src/sgd5.rs`;
// DanziLoader / victory-perf, see NOTICE.md). Each record holds one shader
// container, deflate-packed. The header tables are not decoded yet, so
// records are found by their marker and cross-checked by their sizes:
//
//   u8 name length (with NUL), name, NUL, u8 kind,
//   u32 n, u32 0x000DDCE7, u32 n, u32 unpacked size, raw deflate (n - 12 bytes)
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace danzi::sgd5 {

struct Record {

    std::size_t offset = 0;


    std::optional<std::string_view> name;


    std::uint8_t kind = 0;
    std::span<const std::uint8_t> packed;
    std::size_t unpacked_size = 0;


    std::optional<std::vector<std::uint8_t>> unpack() const;
};


bool is_sgd5(std::span<const std::uint8_t> file);


std::vector<Record> records(std::span<const std::uint8_t> file);


template <class F>
void find_iter(std::span<const std::uint8_t> hay, std::span<const std::uint8_t> needle, F f) {
    const std::size_t n = needle.size();
    if (n == 0 || hay.size() < n) {
        return;
    }
    const std::uint8_t* base = hay.data();
    const std::uint8_t* end = base + hay.size() - n + 1;
    const std::uint8_t* at = base;
    while (at < end) {
        const void* hit = std::memchr(at, needle[0], static_cast<std::size_t>(end - at));
        if (!hit) {
            return;
        }
        at = static_cast<const std::uint8_t*>(hit);
        if (std::memcmp(at, needle.data(), n) == 0) {
            f(static_cast<std::size_t>(at - base));
            at += n;
        } else {
            ++at;
        }
    }
}

}   
