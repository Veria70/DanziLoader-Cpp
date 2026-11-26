// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-formats/src/texture.rs (see texture.hpp for the
// notices).
#include "texture.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

namespace danzi::formats {

namespace {

constexpr std::uint32_t div_ceil(std::uint32_t a, std::uint32_t b) { return a / b + (a % b != 0); }
constexpr std::uint32_t next_multiple(std::uint32_t a, std::uint32_t b) { return div_ceil(a, b) * b; }
constexpr std::uint64_t next_multiple64(std::uint64_t a, std::uint64_t b) { return (a / b + (a % b != 0)) * b; }


std::uint32_t dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) { return div_ceil(std::max(texels >> mip, 1u), block); }


std::uint32_t padded_dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) {
    return div_ceil(div_ceil(std::max(texels, 1u), block), 1u << mip);
}


constexpr const char* MICRO[3][5] = {
    {"x0x1x2x3y0y1y2y3", "--x0x1x2y0y1y2x3", "----x0x1y0y1y2x2", "------x0y0y1x1x2", "--------y0y1x0x1"},
    {"x0x1x2y1y0y2x3y3", "--x0x1x2y0y1y2x3", "----x0x1y0y1x2y2", "------x0y0x1x2y1", "--------x0y0x1y1"},
    {"x0x1z0y0z1y1x2z2", "--x0z0y0z1y1x1z2", "----x0y0z0y1x1z1", "------x0z0y0x1z1", "--------z0y0x0z1"},
};


std::array<std::vector<std::uint32_t>, 3> pattern(std::size_t kind, std::uint32_t element_log2, std::uint32_t block_log2) {
    std::array<std::vector<std::uint32_t>, 3> axes;
    const char* micro = MICRO[kind][element_log2];
    for (std::uint32_t bit = 0; bit < 8; ++bit) {
        char axis_char = micro[2 * bit];
        int axis = axis_char == 'x' ? 0 : axis_char == 'y' ? 1 : axis_char == 'z' ? 2 : -1;
        if (axis < 0) {
            continue;
        }
        std::size_t index = static_cast<std::size_t>(micro[2 * bit + 1] - '0');
        auto& to = axes[axis];
        if (to.size() <= index) {
            to.resize(index + 1, 0);
        }
        to[index] = bit;
    }
    static constexpr std::size_t THIN[] = {1, 0};
    static constexpr std::size_t THICK[] = {1, 0, 2};
    const std::size_t* order = kind == 2 ? THICK : THIN;
    std::size_t order_len = kind == 2 ? 3 : 2;
    std::size_t k = 0;
    for (std::uint32_t bit = 8; bit < block_log2; ++bit, ++k) {
        axes[order[k % order_len]].push_back(bit);
    }
    return axes;
}


std::vector<std::uint32_t> spread(const std::vector<std::uint32_t>& bits) {
    std::vector<std::uint32_t> out(std::size_t{1} << bits.size());
    for (std::uint32_t c = 0; c < out.size(); ++c) {
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i < bits.size(); ++i) {
            sum += ((c >> i) & 1u) << bits[i];
        }
        out[c] = sum;
    }
    return out;
}


std::array<const std::uint32_t*, 3> shared_spread(std::size_t kind, std::uint32_t element_log2, std::uint32_t block_log2) {
    struct Tables {
        std::vector<std::uint32_t> axes[3][5][3][3];
        Tables() {
            const std::uint32_t blocks[3] = {8, 12, 16};
            for (std::size_t k = 0; k < 3; ++k) {
                for (std::uint32_t e = 0; e < 5; ++e) {
                    for (std::size_t b = 0; b < 3; ++b) {
                        auto p = pattern(k, e, blocks[b]);
                        for (int a = 0; a < 3; ++a) {
                            axes[k][e][b][a] = spread(p[a]);
                        }
                    }
                }
            }
        }
    };
    static const Tables tables;
    std::size_t b = block_log2 == 8 ? 0 : block_log2 == 12 ? 1 : 2;
    const auto& t = tables.axes[kind][element_log2][b];
    return {t[0].data(), t[1].data(), t[2].data()};
}

constexpr std::uint32_t MAX_MIPS = 16;

constexpr std::uint32_t MAX_EXTENT = 1u << 14;

constexpr std::uint32_t BLOCK_256_2D[5][2] = {{16, 16}, {16, 8}, {8, 8}, {8, 4}, {4, 4}};
constexpr std::uint32_t BLOCK_256_3D[5][3] = {{8, 4, 8}, {4, 4, 8}, {4, 4, 4}, {4, 2, 4}, {2, 2, 4}};
constexpr std::uint32_t BLOCK_1K_3D[5][3] = {{16, 8, 8}, {8, 8, 8}, {8, 8, 4}, {8, 4, 4}, {4, 4, 4}};

bool mul_ok(std::size_t a, std::size_t b, std::size_t& out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    out = a * b;
    return true;
}

bool add_ok(std::size_t a, std::size_t b, std::size_t& out) {
    if (b > SIZE_MAX - a) {
        return false;
    }
    out = a + b;
    return true;
}

bool power_of_two(std::uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }


struct Run {
    std::size_t tiled, linear, len;
};

template <std::size_t N, bool TILE>
inline void copy_runs(const Run* runs, std::size_t count, std::uint8_t* tiled, std::uint8_t* linear) {
    std::size_t i = 0;
    if constexpr (N != 0) {


        for (; i + 4 <= count; i += 4) {
            std::uint8_t held[4][N];
            std::size_t to[4];
            for (std::size_t k = 0; k < 4; ++k) {
                const Run& r = runs[i + k];
                to[k] = TILE ? r.tiled : r.linear;
                std::memcpy(held[k], TILE ? linear + r.linear : tiled + r.tiled, N);
            }
            std::uint8_t* __restrict out = TILE ? tiled : linear;
            for (std::size_t k = 0; k < 4; ++k) {
                std::memcpy(out + to[k], held[k], N);
            }
        }
    }
    for (; i < count; ++i) {
        const Run& r = runs[i];
        std::size_t len = N == 0 ? r.len : N;
        if constexpr (TILE) {
            std::memcpy(tiled + r.tiled, linear + r.linear, N == 0 ? len : N);
        } else {
            std::memcpy(linear + r.linear, tiled + r.tiled, N == 0 ? len : N);
        }
    }
}


template <std::size_t N, bool TILE, std::uint32_t ROWS>
inline void copy_rows(const Run* runs, std::size_t count, std::uint8_t* const* tiled, std::uint8_t* const* linear) {
    std::uint8_t* t[ROWS];
    std::uint8_t* l[ROWS];
    for (std::uint32_t g = 0; g < ROWS; ++g) {
        t[g] = tiled[g];
        l[g] = linear[g];
    }
    for (std::size_t i = 0; i < count; ++i) {
        const Run& r = runs[i];
        std::size_t len = N == 0 ? r.len : N;
        for (std::uint32_t g = 0; g < ROWS; ++g) {
            if constexpr (TILE) {
                std::memcpy(t[g] + r.tiled, l[g] + r.linear, N == 0 ? len : N);
            } else {
                std::memcpy(l[g] + r.linear, t[g] + r.tiled, N == 0 ? len : N);
            }
        }
    }
}

template <bool TILE, std::uint32_t ROWS>
inline void group_copy_rows(std::size_t run_len, const Run* runs, std::size_t count, std::uint8_t* const* tiled, std::uint8_t* const* linear) {
    switch (run_len) {
        case 8: copy_rows<8, TILE, ROWS>(runs, count, tiled, linear); break;
        case 16: copy_rows<16, TILE, ROWS>(runs, count, tiled, linear); break;
        case 32: copy_rows<32, TILE, ROWS>(runs, count, tiled, linear); break;
        case 64: copy_rows<64, TILE, ROWS>(runs, count, tiled, linear); break;
        default: copy_rows<0, TILE, ROWS>(runs, count, tiled, linear); break;
    }
}


template <bool TILE>
inline void group_copy(std::size_t run_len, std::uint32_t rows, const Run* runs, std::size_t count, std::uint8_t* const* tiled,
                       std::uint8_t* const* linear) {
    switch (rows) {
        case 2: group_copy_rows<TILE, 2>(run_len, runs, count, tiled, linear); break;
        case 4: group_copy_rows<TILE, 4>(run_len, runs, count, tiled, linear); break;
        default: group_copy_rows<TILE, 8>(run_len, runs, count, tiled, linear); break;
    }
}

}   

std::optional<std::uint32_t> Swizzle::block_log2() const {
    switch (mode) {
        case 0: return 0;
        case 1: case 2: return 8;
        case 5: case 6: case 21: case 22: case 23: return 12;
        case 9: case 10: case 17: case 18: case 24: case 25: case 26: case 27: return 16;
        default: return std::nullopt;
    }
}

bool Swizzle::standard() const { return mode == 1 || mode == 5 || mode == 9 || mode == 17 || mode == 21 || mode == 25; }

bool Swizzle::display() const { return mode == 2 || mode == 6 || mode == 10 || mode == 18 || mode == 22 || mode == 26; }

Error Layout::make(const Desc& d, Layout& out, bool addressing) {
    auto block_log2 = d.swizzle.block_log2();
    if (!block_log2) {
        return Error::Invalid;
    }
    auto in = [](std::uint32_t v, std::uint32_t lo, std::uint32_t hi) { return v >= lo && v <= hi; };
    std::uint32_t m = d.swizzle.mode;
    bool valid = in(d.width, 1, MAX_EXTENT) && in(d.height, 1, MAX_EXTENT) && in(d.depth, 1, MAX_EXTENT) && in(d.mips, 1, MAX_MIPS) &&
                 power_of_two(d.element_bytes) && d.element_bytes <= 16 && (d.block == 1 || d.block == 4) && power_of_two(d.samples) && d.samples <= 8 &&
                 (d.samples == 1 || (d.dimension == Dimension::Tex2d && d.mips == 1)) && (d.dimension != Dimension::Tex1d || d.height == 1) &&
                 (d.dimension != Dimension::Tex3d || m == 0 || m == 5 || m == 9 || m == 17 || m == 21 || (m >= 24 && m <= 27));
    if (!valid) {
        return Error::Invalid;
    }
    out = *block_log2 == 0 ? linear(d) : tiled(d, *block_log2, addressing);
    return Error::None;
}


Layout Layout::linear(const Desc& d) {
    std::uint64_t bpe = d.element_bytes;
    std::uint32_t align = 256 / d.element_bytes;
    bool three_d = d.dimension == Dimension::Tex3d;
    Layout l;
    l.mips.resize(d.mips);
    std::uint64_t offset = 0;
    for (std::uint32_t i = d.mips; i-- > 0;) {
        std::uint32_t h = padded_dim(d.height, i, d.block);
        std::uint32_t pitch = next_multiple(padded_dim(d.width, i, d.block), align);
        l.mips[i] = Mip{dim(d.width, i, d.block), dim(d.height, i, d.block), three_d ? dim(d.depth, i, 1) : 1, pitch, h, three_d ? d.depth : 1, offset,
                        std::nullopt};
        offset += std::uint64_t{pitch} * h * bpe;
    }
    l.desc = d;
    l.block = {align, 1, 1};
    l.thick = false;
    l.slices = d.depth;
    l.slice_size = offset;
    l.size = offset * d.depth;
    l.alignment = 256;
    l.first_tail_mip = d.mips;
    l.addressable_ = true;
    return l;
}

Layout Layout::tiled(const Desc& d, std::uint32_t block_log2, bool addressing) {
    std::uint32_t element_log2 = static_cast<std::uint32_t>(std::countr_zero(d.element_bytes));
    std::uint64_t bpe = d.element_bytes;
    std::uint64_t block_bytes = std::uint64_t{1} << block_log2;
    bool thick = d.dimension == Dimension::Tex3d && (d.swizzle.standard() || d.swizzle.display());
    std::array<std::uint32_t, 3> block;
    if (thick) {

        std::uint32_t amp = (block_log2 - 10) / 3;
        std::uint32_t rest = (block_log2 - 10) % 3;
        const std::uint32_t* b = BLOCK_1K_3D[element_log2];
        block = {b[0] << amp, b[1] << (amp + rest / 2), b[2] << (amp + (rest != 0 ? 1 : 0))};
    } else {

        std::uint32_t samples_log2 = static_cast<std::uint32_t>(std::countr_zero(d.samples));
        std::uint32_t elements_log2 = block_log2 - element_log2 - samples_log2;
        bool wide = (samples_log2 & 1) == 0 || (block_log2 & 1) != 0;
        std::uint32_t w = (elements_log2 + (wide ? 1 : 0)) / 2;
        block = {1u << w, 1u << (elements_log2 - w), 1};
    }
    std::uint32_t slices = next_multiple(d.depth, block[2]);
    bool three_d = d.dimension == Dimension::Tex3d;
    auto mip_depth = [&](std::uint32_t i) { return thick ? padded_dim(d.depth, i, 1) : 1u; };
    auto depth_of = [&](std::uint32_t i) { return three_d ? dim(d.depth, i, 1) : 1u; };
    auto width = [&](std::uint32_t i) { return dim(d.width, i, d.block); };
    auto height = [&](std::uint32_t i) { return dim(d.height, i, d.block); };
    Layout l;
    auto& mips = l.mips;
    std::uint32_t first_tail_mip = d.mips;
    std::uint64_t slice_size = 0;
    if (d.mips == 1) {
        Mip m{width(0), height(0), depth_of(0), next_multiple(width(0), block[0]), next_multiple(height(0), block[1]), three_d ? slices : 1, 0,
              std::nullopt};
        mips.push_back(m);
        slice_size = std::uint64_t{m.pitch} * m.rows * bpe * d.samples;
    } else {

        auto tail_max = block;
        if (thick) {
            switch (block_log2 % 3) {
                case 0: tail_max[1] >>= 1; break;
                case 1: tail_max[0] >>= 1; break;
                default: tail_max[2] >>= 1; break;
            }
        } else {
            tail_max[0] >>= 1;
        }

        std::uint32_t effective = thick ? block_log2 - (block_log2 - 8) / 3 : block_log2;
        std::uint32_t max_in_tail = effective == 8 ? 0 : effective <= 11 ? 1 + (1u << (effective - 9)) : effective - 4;

        auto fixed = tail_max;
        if (d.swizzle.z_order() && element_log2 <= 1) {
            const std::uint32_t* b = BLOCK_256_2D[element_log2];
            fixed[0] /= b[0] / BLOCK_256_2D[2][0];
            fixed[1] /= b[1] / BLOCK_256_2D[2][1];
        }
        std::uint64_t sizes[MAX_MIPS];
        std::size_t sized = 0;
        for (std::uint32_t i = 0; i < d.mips; ++i) {
            std::uint32_t w = padded_dim(d.width, i, d.block);
            std::uint32_t h = padded_dim(d.height, i, d.block);
            if (w <= fixed[0] && h <= fixed[1] && d.mips - i <= max_in_tail) {
                first_tail_mip = i;
                slice_size += block_bytes / block[2];
                break;
            }
            std::uint32_t pitch = next_multiple(w, block[0]);
            std::uint32_t rows = next_multiple(h, block[1]);
            std::uint32_t depth = next_multiple(mip_depth(i), block[2]);
            std::uint64_t one = std::uint64_t{pitch} * rows * bpe;
            slice_size += one;
            sizes[sized++] = one * block[2];
            mips.push_back(Mip{width(i), height(i), depth_of(i), pitch, rows, three_d ? depth : 1, 0, std::nullopt});
        }

        std::uint64_t offset = first_tail_mip < d.mips ? block_bytes : 0;
        for (std::size_t i = first_tail_mip; i-- > 0;) {
            mips[i].offset = offset;
            offset += sizes[i];
        }
        std::uint32_t micro[2];
        if (thick) {
            micro[0] = BLOCK_256_3D[element_log2][0];
            micro[1] = BLOCK_256_3D[element_log2][1];
        } else {
            micro[0] = BLOCK_256_2D[element_log2][0];
            micro[1] = BLOCK_256_2D[element_log2][1];
        }
        std::uint32_t pitch = tail_max[0], rows = tail_max[1];
        for (std::uint32_t i = first_tail_mip; i < d.mips; ++i) {
            std::uint32_t m = max_in_tail - 1 - (i - first_tail_mip);
            std::uint32_t off = m > 6 ? 16u << m : m << 8;

            auto bits = [&](std::uint32_t from) {
                std::uint32_t v = 0;
                for (std::uint32_t b = 0; b < 6; ++b) {
                    v += ((off >> (from + 2 * b)) & 1u) << b;
                }
                return v;
            };
            std::uint32_t x = bits(9), y = bits(8);
            if ((block_log2 & 1) != 0) {
                std::swap(x, y);
                if ((element_log2 & 1) != 0) {
                    y = (y << 1) | (x & 1);
                    x >>= 1;
                }
            }
            std::uint32_t depth_pitch = thick ? next_multiple(mip_depth(i), BLOCK_256_3D[element_log2][2]) : 1;
            mips.push_back(Mip{width(i), height(i), depth_of(i), pitch, rows, depth_pitch, 0, Tail{off, x * micro[0], y * micro[1]}});
            pitch = std::max(pitch >> 1, micro[0]);
            rows = std::max(rows >> 1, micro[1]);
        }
    }

    std::uint64_t slab = thick ? block[2] : 1;
    slice_size *= slab;
    slices /= block[2];
    bool addressable = !d.swizzle.xor_mode() && d.samples == 1;
    l.addressable_ = addressable;
    if (addressable && addressing) {
        std::size_t kind = thick ? 2 : (d.swizzle.display() ? 1 : 0);
        l.pattern_ = shared_spread(kind, element_log2, block_log2);
    }
    l.desc = d;
    l.block = block;
    l.thick = thick;
    l.slices = slices;
    l.slice_size = slice_size;
    l.size = slice_size * slices;
    l.alignment = block_bytes;
    l.first_tail_mip = first_tail_mip;
    return l;
}

bool Layout::addressable() const { return addressable_; }

std::pair<std::uint32_t, std::uint32_t> Layout::split(std::uint32_t z) const {
    if (thick) {
        return {z / block[2], z % block[2]};
    }
    return {z, 0};
}

Error Layout::offset(std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t& out) const {
    if (mip >= mips.size()) {
        return Error::Invalid;
    }
    const Mip& m = mips[mip];
    std::uint32_t depth = desc.dimension == Dimension::Tex3d ? m.depth : slices;
    bool inside = x < m.width && y < m.height && z < depth;
    if (!inside || !addressable()) {
        return inside ? Error::Unsupported : Error::Invalid;
    }
    auto [slice, inner] = split(z);
    std::uint64_t base = std::uint64_t{slice} * slice_size + m.offset;
    if (desc.swizzle.mode == Swizzle::LINEAR) {
        std::uint64_t r = std::uint64_t{y} * m.pitch + x;
        out = base + r * desc.element_bytes;
        return Error::None;
    }
    out = base + row(m, y, inner) + column(m, x);
    return Error::None;
}

std::uint64_t Layout::row(const Mip& m, std::uint32_t y, std::uint32_t z) const {
    std::uint32_t bw = block[0], bh = block[1];
    int log2 = std::countr_zero(alignment);
    if (m.tail) {
        y += m.tail->y;
    }
    std::uint64_t blocks = std::uint64_t{y / bh} * (m.pitch / bw);
    return (blocks << log2) + (pattern_[1][y % bh] | pattern_[2][z]);
}

std::uint64_t Layout::column(const Mip& m, std::uint32_t x) const {
    std::uint32_t bw = block[0];
    if (m.tail) {
        x += m.tail->x;
    }
    return (std::uint64_t{x / bw} << std::countr_zero(alignment)) + pattern_[0][x % bw];
}

std::uint32_t Layout::subresource_depth(std::uint32_t mip) const { return mip < mips.size() ? mips[mip].depth : 0; }

std::uint64_t Layout::bytes_from(std::uint32_t first) const {
    bool whole = first == 0 || slices != 1 || thick || desc.dimension == Dimension::Tex3d || desc.samples != 1 || first >= desc.mips;
    if (whole) {
        return size;
    }
    std::uint64_t most = 0;
    bool any = false;
    for (std::size_t i = first; i < mips.size(); ++i) {
        const Mip& m = mips[i];
        std::uint64_t end = m.tail ? alignment : m.offset + std::uint64_t{m.pitch} * m.rows * desc.element_bytes;
        most = any ? std::max(most, end) : end;
        any = true;
    }
    return std::min(any ? most : size, size);
}

template <bool TILE>
Error Layout::walk(std::uint8_t* tiled, std::size_t tiled_len, std::uint32_t mip, std::uint32_t slice, std::uint8_t* linear, std::size_t linear_len,
                   std::size_t row_pitch, std::size_t depth_pitch) const {
    if (mip >= mips.size()) {
        return Error::Invalid;
    }
    const Mip& m = mips[mip];
    std::uint32_t depth = m.depth;
    std::uint32_t first;
    if (desc.dimension == Dimension::Tex3d) {
        if (slice != 0) {
            return Error::Invalid;
        }
        first = 0;
    } else {
        first = slice;
    }
    if (first >= slices) {
        return Error::Invalid;
    }
    if (!addressable()) {
        return Error::Unsupported;
    }
    std::size_t n = desc.element_bytes;
    std::size_t bytes = std::size_t{m.width} * n;

    std::size_t depth_offset, row_offset, need;
    if (!mul_ok(std::size_t{depth} - 1, depth_pitch, depth_offset) || !mul_ok(std::size_t{m.height} - 1, row_pitch, row_offset) ||
        !add_ok(depth_offset, row_offset, need) || !add_ok(need, bytes, need)) {
        return Error::TooSmall;
    }
    if (tiled_len < bytes_from(mip) || linear_len < need || row_pitch < bytes) {
        return Error::TooSmall;
    }
    bool is_linear = desc.swizzle.mode == Swizzle::LINEAR;


    thread_local std::vector<Run> runs;
    runs.clear();
    if (!is_linear) {
        std::size_t first_linear = 0;
        std::size_t first_tiled = static_cast<std::size_t>(column(m, 0));
        for (std::uint32_t x = 1; x < m.width; ++x) {
            std::size_t off = static_cast<std::size_t>(column(m, x));
            std::size_t at = std::size_t{x} * n;
            if (off != first_tiled + (at - first_linear)) {
                runs.push_back({first_tiled, first_linear, at - first_linear});
                first_tiled = off;
                first_linear = at;
            }
        }
        runs.push_back({first_tiled, first_linear, bytes - first_linear});
    }


    std::size_t body = runs.empty() ? 0 : runs.size() - 1;
    std::size_t run_len = 0;
    if (body > 0) {
        run_len = runs[0].len;
        for (std::size_t i = 1; i < body; ++i) {
            if (runs[i].len != run_len) {
                run_len = 0;
                break;
            }
        }
    }


    std::uint32_t group_bits = 0;
    if (!is_linear && !m.tail && run_len != 0) {
        while ((1u << group_bits) < block[1] && pattern_[1][1u << group_bits] < 64) {
            ++group_bits;
        }
    }
    const std::uint32_t group = 1u << group_bits;
    for (std::uint32_t z = 0; z < depth; ++z) {
        auto [s, inner] = split(first + z);
        std::uint64_t base = std::uint64_t{s} * slice_size + m.offset;
        std::uint32_t y = 0;
        if (group > 1) {
            for (; y + group <= m.height; y += group) {
                std::uint8_t* t[8];
                std::uint8_t* lin[8];
                std::uint32_t g_count = std::min(group, 8u);
                for (std::uint32_t done = 0; done < group; done += g_count) {
                    for (std::uint32_t g = 0; g < g_count; ++g) {
                        t[g] = tiled + static_cast<std::size_t>(base + row(m, y + done + g, inner));
                        lin[g] = linear + std::size_t{z} * depth_pitch + std::size_t{y + done + g} * row_pitch;
                    }
                    group_copy<TILE>(run_len, g_count, runs.data(), body, t, lin);
                    for (std::uint32_t g = 0; g < g_count; ++g) {
                        copy_runs<0, TILE>(runs.data() + body, runs.size() - body, t[g], lin[g]);
                    }
                }
            }
        }
        for (; y < m.height; ++y) {
            std::uint8_t* lin = linear + std::size_t{z} * depth_pitch + std::size_t{y} * row_pitch;
            if (is_linear) {
                std::uint8_t* t = tiled + static_cast<std::size_t>(base + std::uint64_t{y} * m.pitch * n);
                if constexpr (TILE) {
                    std::memcpy(t, lin, bytes);
                } else {
                    std::memcpy(lin, t, bytes);
                }
            } else {
                std::uint8_t* t = tiled + static_cast<std::size_t>(base + row(m, y, inner));
                switch (run_len) {
                    case 8: copy_runs<8, TILE>(runs.data(), body, t, lin); break;
                    case 16: copy_runs<16, TILE>(runs.data(), body, t, lin); break;
                    case 32: copy_runs<32, TILE>(runs.data(), body, t, lin); break;
                    case 64: copy_runs<64, TILE>(runs.data(), body, t, lin); break;
                    default: copy_runs<0, TILE>(runs.data(), body, t, lin); break;
                }
                copy_runs<0, TILE>(runs.data() + body, runs.size() - body, t, lin);
            }
        }
    }
    return Error::None;
}

Error Layout::detile(const std::uint8_t* tiled, std::size_t tiled_len, std::uint32_t mip, std::uint32_t slice, std::uint8_t* linear,
                     std::size_t linear_len, std::size_t row_pitch, std::size_t depth_pitch) const {
    return walk<false>(const_cast<std::uint8_t*>(tiled), tiled_len, mip, slice, linear, linear_len, row_pitch, depth_pitch);
}

Error Layout::tile(std::uint8_t* tiled, std::size_t tiled_len, std::uint32_t mip, std::uint32_t slice, const std::uint8_t* linear,
                   std::size_t linear_len, std::size_t row_pitch, std::size_t depth_pitch) const {
    return walk<true>(tiled, tiled_len, mip, slice, const_cast<std::uint8_t*>(linear), linear_len, row_pitch, depth_pitch);
}

std::optional<Format> format(std::uint32_t dxgi) {
    auto in = [&](std::uint32_t lo, std::uint32_t hi) { return dxgi >= lo && dxgi <= hi; };
    if (in(1, 4)) return Format{16, 1, false};
    if (in(9, 18)) return Format{8, 1, false};
    if (in(19, 22) || in(44, 47)) return Format{4, 1, true};
    if (in(23, 43) || dxgi == 67 || in(87, 93) || dxgi == 116 || dxgi == 117) return Format{4, 1, false};
    if (in(48, 59) || dxgi == 85 || dxgi == 86 || dxgi == 115) return Format{2, 1, false};
    if (in(118, 120)) return Format{2, 1, true};
    if (in(60, 65)) return Format{1, 1, false};
    if (in(70, 72) || in(79, 81)) return Format{8, 4, false};
    if (in(73, 78) || in(82, 84) || in(94, 99)) return Format{16, 4, false};
    return std::nullopt;
}

Error Texture::make(const TextureDesc& desc, Texture& out, bool addressing) {
    auto f = format(desc.format);
    if (!f) {
        return Error::Invalid;
    }
    std::uint32_t depth3d = desc.dimension == Dimension::Tex3d ? desc.depth_or_array : 1;
    std::uint32_t mips = desc.mips;
    if (mips == 0) {
        mips = 32 - static_cast<std::uint32_t>(std::countl_zero(std::max({desc.width, desc.height, depth3d, 1u})));
    }
    auto plane = [&](std::uint32_t bytes) {
        return Desc{desc.dimension, desc.width, desc.height, desc.depth_or_array, mips, bytes, f->block, std::max(desc.samples, 1u), desc.swizzle};
    };
    Texture t;
    Plane first{0, false, {}};
    if (Error e = Layout::make(plane(f->bytes), first.layout, addressing); e != Error::None) {
        return e;
    }
    t.planes.push_back(std::move(first));
    if (f->stencil) {
        Plane stencil{0, true, {}};
        if (Error e = Layout::make(plane(1), stencil.layout, addressing); e != Error::None) {
            return e;
        }
        stencil.offset = next_multiple64(t.planes[0].layout.size, stencil.layout.alignment);
        t.planes.push_back(std::move(stencil));
    }
    const Plane& last = t.planes.back();
    t.size = last.offset + last.layout.size;
    t.alignment = 1;
    bool any = false;
    for (const Plane& p : t.planes) {
        t.alignment = any ? std::max(t.alignment, p.layout.alignment) : p.layout.alignment;
        any = true;
    }
    out = std::move(t);
    return Error::None;
}

}   
