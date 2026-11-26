// SPDX-License-Identifier: GPL-3.0-or-later
// `SGD5` records (`crates/victory-game/src/sgd5.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See sgd5.hpp.
#include "sgd5.hpp"

#include "shader/native.hpp"

namespace danzi::sgd5 {

namespace {

constexpr std::uint8_t MAGIC[4] = {'S', 'G', 'D', '5'};
constexpr std::uint8_t MARKER[4] = {0xE7, 0xDC, 0x0D, 0x00};

constexpr std::size_t MAX_UNPACKED = std::size_t{64} << 20;

std::optional<std::size_t> u32_at(std::span<const std::uint8_t> file, std::size_t o) {
    if (o > file.size() || file.size() - o < 4) {
        return std::nullopt;
    }
    return std::size_t{file[o]} | std::size_t{file[o + 1]} << 8 | std::size_t{file[o + 2]} << 16 | std::size_t{file[o + 3]} << 24;
}

bool graphic(std::uint8_t b) { return b >= 0x21 && b <= 0x7E; }


std::optional<std::string_view> name_before(std::span<const std::uint8_t> file, std::size_t nul) {
    std::size_t run = nul;
    while (run > 0 && graphic(file[run - 1])) {
        --run;
    }

    for (std::size_t l = run > 0 ? run - 1 : 0; l < nul; ++l) {
        if (std::size_t{file[l]} == nul - l) {

            return std::string_view(reinterpret_cast<const char*>(file.data() + l + 1), nul - l - 1);
        }
    }
    return std::nullopt;
}

std::optional<Record> record(std::span<const std::uint8_t> file, std::size_t at) {
    if (at < 4) {
        return std::nullopt;
    }
    auto n = u32_at(file, at - 4);
    if (!n) {
        return std::nullopt;
    }
    auto unpacked = u32_at(file, at + 8);
    if (!unpacked) {
        return std::nullopt;
    }
    auto again = u32_at(file, at + 4);
    if (!again || *again != *n || *n < 12 || *unpacked > MAX_UNPACKED) {
        return std::nullopt;
    }
    if (at < 6) {
        return std::nullopt;
    }
    std::size_t nul = at - 6;
    Record r;
    r.offset = at;
    r.name = file[nul] == 0 ? name_before(file, nul) : std::nullopt;
    r.kind = file[at - 5];
    if (at + *n > file.size()) {
        return std::nullopt;
    }
    r.packed = file.subspan(at + 12, *n - 12);
    r.unpacked_size = *unpacked;
    return r;
}

}   

std::optional<std::vector<std::uint8_t>> Record::unpack() const {


    return shader::inflate_raw_exact(packed, unpacked_size);
}

bool is_sgd5(std::span<const std::uint8_t> file) {
    bool found = false;

    if (file.size() >= 4) {
        std::string_view hay(reinterpret_cast<const char*>(file.data()), file.size());
        found = hay.find(std::string_view(reinterpret_cast<const char*>(MAGIC), 4)) != std::string_view::npos;
    }
    return found;
}

std::vector<Record> records(std::span<const std::uint8_t> file) {
    std::vector<Record> out;
    find_iter(file, MARKER, [&](std::size_t at) {
        if (auto r = record(file, at)) {
            out.push_back(*r);
        }
    });
    return out;
}

}   
