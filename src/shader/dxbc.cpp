// SPDX-License-Identifier: GPL-3.0-or-later
// The DXBC container (`dxbc.rs`; DanziLoader / victory-perf, see NOTICE.md).
#include "dxbc.hpp"

#include <cstring>

#include "error.hpp"

namespace danzi::shader {

namespace {

constexpr std::size_t HEADER_SIZE = 0x20, MAX_PARTS = 256;

std::uint32_t u32(std::span<const std::uint8_t> d, std::size_t at) {
    if (at > d.size() || d.size() - at < 4) {
        malformed("truncated");
    }
    std::uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

std::uint16_t u16(std::span<const std::uint8_t> d, std::size_t at) {
    if (at > d.size() || d.size() - at < 2) {
        malformed("truncated");
    }
    std::uint16_t v;
    std::memcpy(&v, d.data() + at, 2);
    return v;
}

}   

Container Container::parse(std::span<const std::uint8_t> data) {
    if (data.size() < HEADER_SIZE || std::memcmp(data.data(), "DXBC", 4) != 0) {
        throw ShaderError(ShaderError::NotDxbc, "not a DXBC container");
    }
    Container c;
    std::memcpy(c.digest, data.data() + 4, 16);
    c.version[0] = u16(data, 0x14);
    c.version[1] = u16(data, 0x16);
    std::size_t total = u32(data, 0x18);
    if (total > data.size() || total < HEADER_SIZE) {
        malformed("container size disagrees with data");
    }
    data = data.first(total);
    std::size_t count = u32(data, 0x1C);
    if (count > MAX_PARTS) {
        malformed("implausible part count");
    }
    c.parts.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t offset = u32(data, HEADER_SIZE + 4 * i);
        if (offset > data.size() || data.size() - offset < 4) {
            malformed("part header out of bounds");
        }
        std::uint32_t fourcc;
        std::memcpy(&fourcc, data.data() + offset, 4);
        std::size_t size = u32(data, offset + 4);
        if (offset + 8 > data.size() || size > data.size() - (offset + 8)) {
            malformed("part data out of bounds");
        }
        c.parts.push_back({fourcc, offset, data.subspan(offset + 8, size)});
    }
    return c;
}

std::string fourcc_text(std::uint32_t fourcc) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        auto b = static_cast<unsigned char>(fourcc >> (8 * i));
        s.push_back(b > 0x20 && b < 0x7F ? static_cast<char>(b) : '.');
    }
    return s;
}

}   
