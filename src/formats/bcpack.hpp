// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the BCPack decoder (`victory-formats/src/bcpack.rs`; DanziLoader /
// victory-perf, see NOTICE.md).
//
// BCPack: lossless packing of BC1-BC7 block streams, as the console's
// DirectStorage hardware unpacks them. Decode only; BC2 is left out (no title
// data uses it).
//
// A packed stream has no header. It is 12 or 16 interleaved rANS *lanes*; each
// lane is a run of fragments (header byte, length, payload) coded with one of
// 128 static models, and a fixed schedule decides which lane takes the next
// input byte. The lanes' symbols form one record per block: two back
// references (to one of the last 256 blocks, or to a per-format dictionary
// that stands in for blocks -1 to -255) and the residuals of the endpoints and
// indices against them. The models, the index rank tables, the BC7 control
// codes and the dictionaries are constants of the format, kept in
// `bcpack-tables.bin` (embedded here as `bcpack_tables.inc`, see `builtin`).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace danzi::formats::bcpack {

inline constexpr std::size_t kModels = 128;
inline constexpr std::size_t kBuckets = 9;
inline constexpr std::size_t kDictionary = 255;


inline constexpr std::size_t kTablesSize = kModels * kBuckets * 2 + 256 + 64 + 256 + 64 + 64 + 512 * 2 +
                                           kDictionary * (6 + 16 + 8 + 32 + 2 + 16 + 4 + 32 + 25 + 33 + 50 + 66);

namespace detail {


struct Plain {
    std::array<std::uint8_t, 8> ep{};
    std::array<std::uint8_t, 32> ix{};
};

struct Bc6Ep {

    std::array<std::array<std::uint16_t, 3>, 4> e{};
};

struct Bc6Ix {
    std::uint8_t mode = 0;
    std::array<std::int16_t, 16> ix{};
};

struct Bc7Ep {
    std::uint16_t ctrl = 0;

    std::array<std::array<std::uint16_t, 8>, 3> rows{};
};

struct Bc7Ix {
    std::array<std::uint8_t, 2> bits{};
    std::array<std::array<std::int16_t, 16>, 2> ix{};
};

}   


struct Tables {
    std::array<std::array<std::uint32_t, kBuckets>, kModels> freq{};


    std::array<std::array<std::uint32_t, kBuckets + 1>, kModels> start{};


    std::array<std::array<std::uint8_t, 256>, kModels> bucket{};

    std::array<std::array<std::uint64_t, kBuckets>, kModels> recip{};
    std::array<std::uint8_t, 256> rank1{};
    std::array<std::uint8_t, 64> rank3a{};
    std::array<std::uint8_t, 256> rank3c{};
    std::array<std::uint8_t, 64> rank4{};
    std::array<std::uint8_t, 64> rank5{};
    std::array<std::uint16_t, 512> ctrl7{};
    std::vector<detail::Plain> bc1;
    std::vector<detail::Plain> bc3;
    std::vector<detail::Plain> bc4;
    std::vector<detail::Plain> bc5;
    std::vector<std::pair<detail::Bc6Ep, detail::Bc6Ix>> bc6;
    std::vector<std::pair<detail::Bc7Ep, detail::Bc7Ix>> bc7;
};


const Tables* builtin(std::string* error);


std::unique_ptr<Tables> parse(std::span<const std::uint8_t> bytes, std::string* error);


std::optional<std::string> decode(std::uint8_t bc, std::span<const std::uint8_t> src, std::span<std::uint8_t> dst,
                                  const Tables& tables);

namespace detail {


bool lanes(const Tables& t, std::span<const std::uint8_t> widths, std::size_t per, std::size_t blocks,
           const std::function<int(std::size_t)>& fetch, std::span<std::uint8_t> records);


std::size_t bucket_of(const Tables& t, std::uint8_t model, std::uint32_t slot);


std::pair<int, std::uint32_t> payload_step(const Tables& t, std::uint8_t model, std::uint32_t x);

}   

}   
