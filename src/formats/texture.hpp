// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-formats/src/texture.rs (DanziLoader / victory-perf,
// see NOTICE.md): texture layouts of the console GPU (RDNA2, GFX10.3): a
// texture's size and alignment, where each mip and array slice starts, the
// address of each element, and copies between tiled memory and linear rows.
//
// Units: an element is a texel, or a 4x4 block of a block-compressed format.
// Mips of one array slice (or one depth slab of a thick 3D texture) are
// stored together, smallest first: the mip tail (the small mips sharing one
// block) at offset 0, mip 0 last.
//
// The algorithm follows AMD's address library (addrlib), as shipped in Mesa
// under `src/amd/addrlib`: block dimensions, mip placement and mip tails from
// `gfx10addrlib.cpp` and `addrlib2.cpp`, and the 256-byte micro tile patterns
// from `gfx10SwizzlePattern.h` (the RB+ tables, as on RDNA2). addrlib carries
// this notice:
//
// Copyright (C) 2007-2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions: the
// above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
// "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace danzi::formats {


template <class T, std::size_t N>
class Inline {
    static_assert(std::is_trivially_destructible_v<T>);

public:
    Inline() = default;
    Inline(const Inline& o) : count_(o.count_) {
        for (std::size_t i = 0; i < count_; ++i) {
            new (raw_ + i * sizeof(T)) T(o[i]);
        }
    }
    Inline& operator=(const Inline& o) {
        if (this != &o) {
            count_ = o.count_;
            for (std::size_t i = 0; i < count_; ++i) {
                new (raw_ + i * sizeof(T)) T(o[i]);
            }
        }
        return *this;
    }
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    T& operator[](std::size_t i) { return items()[i]; }
    const T& operator[](std::size_t i) const { return items()[i]; }
    T* begin() { return items(); }
    T* end() { return items() + count_; }
    const T* begin() const { return items(); }
    const T* end() const { return items() + count_; }
    T& back() { return items()[count_ - 1]; }
    const T& back() const { return items()[count_ - 1]; }
    void push_back(const T& v) { new (raw_ + count_++ * sizeof(T)) T(v); }
    void resize(std::size_t n) {
        for (std::size_t i = count_; i < n; ++i) {
            new (raw_ + i * sizeof(T)) T{};
        }
        count_ = n;
    }
    void clear() { count_ = 0; }

private:
    T* items() { return std::launder(reinterpret_cast<T*>(raw_)); }
    const T* items() const { return std::launder(reinterpret_cast<const T*>(raw_)); }
    alignas(T) unsigned char raw_[N * sizeof(T)];
    std::size_t count_ = 0;
};


struct Swizzle {
    std::uint32_t mode = 0;

    static constexpr std::uint32_t LINEAR = 0;
    static constexpr std::uint32_t SW_256B_S = 1;
    static constexpr std::uint32_t SW_256B_D = 2;
    static constexpr std::uint32_t SW_4KB_S = 5;
    static constexpr std::uint32_t SW_4KB_D = 6;
    static constexpr std::uint32_t SW_64KB_S = 9;
    static constexpr std::uint32_t SW_64KB_D = 10;
    static constexpr std::uint32_t SW_64KB_S_T = 17;
    static constexpr std::uint32_t SW_64KB_D_T = 18;
    static constexpr std::uint32_t SW_4KB_S_X = 21;
    static constexpr std::uint32_t SW_4KB_D_X = 22;
    static constexpr std::uint32_t SW_4KB_R_X = 23;
    static constexpr std::uint32_t SW_64KB_Z_X = 24;
    static constexpr std::uint32_t SW_64KB_S_X = 25;
    static constexpr std::uint32_t SW_64KB_D_X = 26;
    static constexpr std::uint32_t SW_64KB_R_X = 27;


    std::optional<std::uint32_t> block_log2() const;
    bool standard() const;
    bool display() const;
    bool z_order() const { return mode == 24; }

    bool xor_mode() const { return mode >= 16; }
    bool operator==(const Swizzle&) const = default;
};

enum class Dimension : std::uint32_t { Tex1d, Tex2d, Tex3d };

enum class Error : std::uint32_t {
    None,

    Invalid,

    Unsupported,

    TooSmall,
};


struct Desc {
    Dimension dimension;
    std::uint32_t width, height;

    std::uint32_t depth;
    std::uint32_t mips;

    std::uint32_t element_bytes;

    std::uint32_t block;
    std::uint32_t samples;
    Swizzle swizzle;
};

struct Tail {
    std::uint32_t offset, x, y;
};


struct Mip {
    std::uint32_t width, height, depth;
    std::uint32_t pitch, rows, depth_pitch;
    std::uint64_t offset;
    std::optional<Tail> tail;
};

class Layout {
public:
    Desc desc{};

    std::array<std::uint32_t, 3> block{};
    bool thick = false;
    std::uint32_t slices = 0;
    std::uint64_t slice_size = 0;
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;
    std::uint32_t first_tail_mip = 0;

    Inline<Mip, 16> mips;


    static Error make(const Desc& d, Layout& out, bool addressing = true);

    bool addressable() const;


    Error offset(std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t& out) const;
    std::uint32_t subresource_depth(std::uint32_t mip) const;


    std::uint64_t bytes_from(std::uint32_t first) const;

    Error detile(const std::uint8_t* tiled, std::size_t tiled_len, std::uint32_t mip, std::uint32_t slice, std::uint8_t* linear, std::size_t linear_len,
                 std::size_t row_pitch, std::size_t depth_pitch) const;
    Error tile(std::uint8_t* tiled, std::size_t tiled_len, std::uint32_t mip, std::uint32_t slice, const std::uint8_t* linear, std::size_t linear_len,
               std::size_t row_pitch, std::size_t depth_pitch) const;

private:


    std::array<const std::uint32_t*, 3> pattern_{};
    bool addressable_ = false;

    static Layout linear(const Desc& d);
    static Layout tiled(const Desc& d, std::uint32_t block_log2, bool addressing);
    std::pair<std::uint32_t, std::uint32_t> split(std::uint32_t z) const;
    std::uint64_t row(const Mip& m, std::uint32_t y, std::uint32_t z) const;
    std::uint64_t column(const Mip& m, std::uint32_t x) const;
    template <bool TILE>
    Error walk(std::uint8_t* tiled, std::size_t tiled_len, std::uint32_t mip, std::uint32_t slice, std::uint8_t* linear, std::size_t linear_len,
               std::size_t row_pitch, std::size_t depth_pitch) const;
};


struct Format {
    std::uint32_t bytes;
    std::uint32_t block;
    bool stencil;
};
std::optional<Format> format(std::uint32_t dxgi);


struct TextureDesc {
    Dimension dimension;
    std::uint32_t width, height;
    std::uint32_t depth_or_array;

    std::uint32_t mips;
    std::uint32_t format;
    std::uint32_t samples;
    Swizzle swizzle;
};

struct Plane {
    std::uint64_t offset;
    bool stencil;
    Layout layout;
};


struct Texture {

    Inline<Plane, 2> planes;
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;

    static Error make(const TextureDesc& desc, Texture& out, bool addressing = true);
};

}   
