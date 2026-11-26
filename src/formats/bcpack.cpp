// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the BCPack decoder (`victory-formats/src/bcpack.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See bcpack.hpp for the format's outline.
//
// Integer arithmetic follows the reference's release build: what wraps there
// wraps here (unsigned types, explicit truncation to u8/u16), and a shift by
// the operand's width or more uses the amount modulo the width.
#include "bcpack.hpp"

#include <algorithm>
#include <cstring>

namespace danzi::formats::bcpack {

namespace {

using detail::Bc6Ep;
using detail::Bc6Ix;
using detail::Bc7Ep;
using detail::Bc7Ix;
using detail::Plain;

constexpr std::size_t kHistory = 256;

constexpr std::uint32_t kLow = 1u << 24;

constexpr std::size_t kRoundBytes = 8;

constexpr std::uint8_t kBuiltinTables[] = {
#include "bcpack_tables.inc"
};
static_assert(sizeof(kBuiltinTables) == kTablesSize);

std::string message(const char* what) { return std::string("BCPack: ") + what; }

struct Reader {
    const std::uint8_t* p;

    std::uint8_t u8() { return *p++; }
    std::uint16_t u16() {
        const std::uint16_t v = static_cast<std::uint16_t>(p[0] | p[1] << 8);
        p += 2;
        return v;
    }
    template <std::size_t N>
    std::array<std::uint8_t, N> bytes() {
        std::array<std::uint8_t, N> a;
        std::memcpy(a.data(), p, N);
        p += N;
        return a;
    }
};


struct Lane {

    std::uint8_t width = 0;

    std::uint8_t phase = 0;
    bool long_ = false;
    bool reset = false;
    std::uint8_t model = 0;
    bool need = false;
    bool has_latch = false;
    std::uint8_t latch = 0;
    bool has_out = false;
    std::uint8_t out = 0;
    bool stalled = false;
    std::uint32_t x = 0;
    std::uint32_t left = 0;
};


inline std::size_t bucket_of(const Tables& t, std::uint8_t model, std::uint32_t slot) {
    const auto& start = t.start[model];
    std::size_t b = t.bucket[model][slot >> 8];
    while (start[b + 1] <= slot) {
        ++b;
    }
    return b;
}


inline void step(Lane& l, const Tables& t) {
    if (l.phase != 3) {
        if (!l.has_latch) {
            return;
        }
        const std::uint8_t b = l.latch;
        l.has_latch = false;
        l.need = true;
        switch (l.phase) {
        case 0: {

            const auto group = static_cast<std::uint8_t>(l.width - (b >> 6));
            l.model = static_cast<std::uint8_t>(group * 16 + (b & 15));
            l.reset = (b & 0x10) != 0;
            l.long_ = (b & 0x20) != 0;
            l.left = 0;
            l.phase = 1;
            break;
        }
        case 1:
            if (l.long_) {
                l.left = b;
                l.phase = 2;
            } else {
                l.left = std::uint32_t{b} + 1;
                l.phase = 3;
            }
            break;
        default:
            l.left = (std::uint32_t{b} << 8 | l.left) + 257;
            l.phase = 3;
            break;
        }
        return;
    }
    std::uint32_t x = l.x;
    if (x < kLow && l.has_latch) {
        l.has_latch = false;
        l.left -= 1;
        x = x << 8 | l.latch;
    }
    l.need = l.left != 0 && !l.has_latch;
    if (x > kLow) {
        const auto& freq = t.freq[l.model];
        const auto& start = t.start[l.model];
        const std::uint32_t slot = x & 0xffff;

        const std::size_t b = bucket_of(t, l.model, slot);


        std::uint32_t sym, f, rem;
        if (b == 0) {
            sym = 0;
            f = freq[0] - 1;
            rem = slot - 1;
        } else {
            const std::uint32_t offset = slot - start[b];
            const auto q = static_cast<std::uint32_t>((std::uint64_t{offset} * t.recip[l.model][b]) >> 32);
            sym = (1u << (b - 1)) + q;
            f = freq[b];
            rem = offset - q * freq[b];
        }
        l.x = (x >> 16) * f + rem;
        l.has_out = true;
        l.out = static_cast<std::uint8_t>(sym);
    } else if (l.left != 0) {


        l.x = x;
    } else {

        l.phase = 0;
        l.need = !l.has_latch;
        if (l.reset) {
            l.x = 0;
        }
    }
}


template <class Fetch>
bool run_lanes(const Tables& t, std::span<const std::uint8_t> widths, std::size_t per, std::size_t blocks,
               Fetch&& fetch, std::uint8_t* records) {
    std::array<Lane, 16> all{};
    const std::size_t n = widths.size();
    for (std::size_t j = 0; j < n; ++j) {
        all[j].width = widths[j];
        all[j].need = true;
    }
    const std::size_t stride = n * per;
    const std::uint32_t full = (1u << n) - 1;
    std::size_t pos = 0;
    std::uint32_t done = 0;
    int idle = 0;

    std::size_t at = 0;
    while (pos < blocks * per) {
        std::size_t taken = 0;
        bool moved = false;
        for (std::size_t j = 0; j < n; ++j) {
            Lane& lane = all[j];
            const std::uint32_t bit = 1u << j;
            const std::size_t before = taken;
            if (!lane.stalled) {
                step(lane, t);
            }
            const bool waiting = lane.has_out && (done & bit) != 0;
            if (lane.need && !waiting && taken < kRoundBytes) {

                const int b = fetch(j);
                if (b >= 0) {
                    lane.latch = static_cast<std::uint8_t>(b);
                    lane.has_latch = true;
                    ++taken;
                }
            }
            lane.stalled = (lane.need && before >= kRoundBytes) || waiting;
            if (!lane.stalled && lane.has_out) {
                lane.has_out = false;
                records[at + j * per] = lane.out;
                done |= bit;
                moved = true;
            }
        }
        if (done == full) {
            ++pos;
            at += pos % per == 0 ? stride - per + 1 : 1;
            done = 0;
            moved = true;
        }

        idle = taken > 0 || moved ? 0 : idle + 1;
        if (idle > 3) {
            return false;
        }
    }
    return true;
}


inline std::uint8_t interleave(std::uint8_t a, std::uint8_t b) {
    const auto spread = [](std::uint8_t v) {
        unsigned acc = 0;
        for (unsigned k = 0; k < 4; ++k) {
            acc |= (v >> k & 1u) << (2 * k);
        }
        return static_cast<std::uint8_t>(acc);
    };
    return static_cast<std::uint8_t>(spread(a) | spread(b) << 1);
}


inline std::pair<std::size_t, std::size_t> references(const std::uint8_t* s, bool paired) {
    std::uint8_t d, ix;
    if (paired) {
        d = interleave(s[0], s[1]);
        ix = interleave(s[2], s[3]);
    } else {
        d = s[0];
        ix = s[1];
    }
    const auto delta = static_cast<std::uint8_t>((d >> 1) ^ static_cast<std::uint8_t>(0u - (d & 1u)));
    return {static_cast<std::uint8_t>(ix + delta), ix};
}


template <class T>
inline const T& pick(std::size_t i, std::size_t r, const std::vector<T>& history, const std::vector<T>& dictionary,
                     const T& zero) {
    if (r == 0) {
        return zero;
    }
    if (r <= i) {
        return history[(i - r) % kHistory];
    }
    return dictionary[r - i - 1];
}


inline std::uint8_t zz(std::uint8_t v, unsigned bits) {
    const auto mask = static_cast<std::uint8_t>((1u << bits) - 1);
    return static_cast<std::uint8_t>(((v >> 1) ^ ((v & 1) != 0 ? mask : 0)) & mask);
}


struct Widths {
    std::array<std::uint8_t, 8> ep;
    std::array<std::uint8_t, 32> ix;
    bool paired;
};

constexpr Widths widths(std::array<std::uint8_t, 8> ep, std::size_t ix3, std::size_t ix2, bool paired) {
    Widths w{ep, {}, paired};
    for (std::size_t k = 0; k < ix3 + ix2; ++k) {
        w.ix[k] = k < ix3 ? 3 : 2;
    }
    return w;
}

constexpr Widths kBc1 = widths({5, 6, 5, 5, 6, 5, 0, 0}, 0, 16, false);
constexpr Widths kBc3 = widths({5, 6, 5, 5, 6, 5, 8, 8}, 16, 16, true);
constexpr Widths kBc4 = widths({8, 8, 0, 0, 0, 0, 0, 0}, 16, 0, false);
constexpr Widths kBc5 = widths({8, 8, 8, 8, 0, 0, 0, 0}, 32, 0, true);

template <class Residual, class Pack>
void plain(const std::uint8_t* records, std::size_t stride, std::span<std::uint8_t> out, std::size_t block,
           const std::vector<Plain>& dictionary, const Widths& w, Residual residual, Pack pack) {
    std::vector<Plain> history(kHistory);
    const Plain zero{};
    const std::size_t blocks = out.size() / block;
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint8_t* s = records + i * stride;
        const auto [ep_i, ix_i] = references(s, w.paired);
        const auto& ep_ref = pick(i, ep_i, history, dictionary, zero).ep;
        const auto& ix_ref = pick(i, ix_i, history, dictionary, zero).ix;
        Plain v = residual(s);
        for (std::size_t k = 0; k < 8; ++k) {
            v.ep[k] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(v.ep[k] + ep_ref[k]) &
                                                static_cast<std::uint8_t>((1u << w.ep[k]) - 1));
        }
        for (std::size_t k = 0; k < 32; ++k) {
            v.ix[k] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(v.ix[k] + ix_ref[k]) &
                                                static_cast<std::uint8_t>((1u << w.ix[k]) - 1));
        }
        history[i % kHistory] = v;
        pack(v, out.data() + i * block);
    }
}


inline void quads(const std::array<std::uint8_t, 256>& rank, const std::uint8_t* s, std::uint8_t* ix) {
    constexpr std::size_t kPlaces[4] = {0, 1, 4, 5};
    for (std::size_t q = 0; q < 4; ++q) {
        const std::uint8_t v = rank[s[q]];
        const std::size_t base = (q >> 1) * 8 + (q & 1) * 2;
        for (std::size_t k = 0; k < 4; ++k) {
            ix[base + kPlaces[k]] = static_cast<std::uint8_t>(v >> (2 * k) & 3);
        }
    }
}


inline void pairs(const std::array<std::uint8_t, 64>& rank, const std::uint8_t* s, std::size_t count,
                  std::uint8_t* ix) {
    for (std::size_t k = 0; k < count; ++k) {
        const std::uint8_t v = rank[s[k] & 63];
        ix[2 * k] = v & 7;
        ix[2 * k + 1] = v >> 4 & 7;
    }
}

Plain bc1_residual(const Tables& t, const std::uint8_t* s) {
    Plain v{};
    constexpr unsigned kBits[6] = {5, 6, 5, 5, 6, 5};
    for (std::size_t k = 0; k < 6; ++k) {
        v.ep[k] = zz(s[2 + k], kBits[k]);
    }
    quads(t.rank1, s + 8, v.ix.data());
    return v;
}

Plain bc3_residual(const Tables& t, const std::uint8_t* s) {

    Plain v{};
    v.ep = {zz(s[14], 5), zz(s[16], 6), zz(s[18], 5), zz(s[15], 5),
            zz(s[17], 6), zz(s[19], 5), zz(s[4], 8),  zz(s[5], 8)};
    pairs(t.rank3a, s + 6, 8, v.ix.data());
    quads(t.rank3c, s + 20, v.ix.data() + 16);
    return v;
}

Plain bc4_residual(const Tables& t, const std::uint8_t* s) {
    Plain v{};
    v.ep[0] = zz(s[2], 8);
    v.ep[1] = zz(s[3], 8);
    pairs(t.rank4, s + 4, 8, v.ix.data());
    return v;
}

Plain bc5_residual(const Tables& t, const std::uint8_t* s) {
    Plain v{};
    for (std::size_t k = 0; k < 4; ++k) {
        v.ep[k] = zz(s[4 + k], 8);
    }
    pairs(t.rank5, s + 8, 16, v.ix.data());
    return v;
}


constexpr std::uint32_t kBc1Code[4] = {0, 2, 3, 1};
constexpr std::uint64_t kBc4Code[8] = {0, 2, 3, 4, 5, 6, 7, 1};

inline void put_le(std::uint8_t* out, std::uint64_t v, std::size_t n) {
    for (std::size_t k = 0; k < n; ++k) {
        out[k] = static_cast<std::uint8_t>(v >> (8 * k));
    }
}

void bc1_color(const std::uint8_t* ep, const std::uint8_t* ix, std::uint8_t* out) {
    const auto c0 = static_cast<std::uint16_t>(ep[0] << 11 | ep[1] << 5 | ep[2]);
    const auto c1 = static_cast<std::uint16_t>(ep[3] << 11 | ep[4] << 5 | ep[5]);
    std::uint32_t bits = 0;
    for (std::size_t p = 0; p < 16; ++p) {
        bits |= kBc1Code[ix[p] & 3] << (2 * p);
    }
    put_le(out, c0, 2);
    put_le(out + 2, c1, 2);
    put_le(out + 4, bits, 4);
}


void bc4_alpha(std::uint8_t e0, std::uint8_t e1, const std::uint8_t* ix, std::uint8_t* out) {
    std::uint64_t bits = 0;
    for (std::size_t p = 0; p < 16; ++p) {
        bits |= kBc4Code[ix[p] & 7] << (16 + 3 * p);
    }
    const std::uint64_t v = std::uint64_t{static_cast<std::uint8_t>(e0 ^ 0x80)} |
                            std::uint64_t{static_cast<std::uint8_t>(e1 ^ 0x80)} << 8 | bits;
    put_le(out, v, 8);
}

void bc1_pack(const Plain& v, std::uint8_t* out) { bc1_color(v.ep.data(), v.ix.data(), out); }

void bc3_pack(const Plain& v, std::uint8_t* out) {
    bc4_alpha(v.ep[6], v.ep[7], v.ix.data(), out);
    bc1_color(v.ep.data(), v.ix.data() + 16, out + 8);
}

void bc4_pack(const Plain& v, std::uint8_t* out) { bc4_alpha(v.ep[0], v.ep[1], v.ix.data(), out); }

void bc5_pack(const Plain& v, std::uint8_t* out) {
    bc4_alpha(v.ep[0], v.ep[1], v.ix.data(), out);
    bc4_alpha(v.ep[2], v.ep[3], v.ix.data() + 16, out + 8);
}


inline std::array<std::int16_t, 2> nibbles(std::uint8_t b, bool odd_first) {
    const auto half = [](unsigned v) {
        unsigned acc = 0;
        for (unsigned k = 0; k < 4; ++k) {
            acc |= (v >> (2 * k) & 1u) << k;
        }
        return acc;
    };
    const auto z = [](unsigned u) { return static_cast<std::int16_t>((u >> 1) ^ (0u - (u & 1))); };
    const std::int16_t even = z(half(b));
    const std::int16_t odd = z(half(static_cast<unsigned>(b >> 1)));
    if (odd_first) {
        return {odd, even};
    }
    return {even, odd};
}


inline std::int16_t add_index(std::int16_t cur, std::uint8_t bits, std::int16_t refv, std::uint8_t ref_bits) {
    const auto up = [](std::int16_t v, std::uint8_t b) -> std::uint16_t {
        if (b == 0 || b > 16) {
            return 0;
        }
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(v) << (16 - b));
    };
    const auto sum = static_cast<std::int16_t>(static_cast<std::uint16_t>(up(cur, bits) + up(refv, ref_bits)));
    if (bits == 0) {
        return 0;
    }
    return static_cast<std::int16_t>(sum >> (static_cast<std::uint8_t>(16 - bits) & 15));
}


struct Bits {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::uint32_t at = 0;

    void put(std::uint64_t value, std::uint32_t n) {
        if (n > 0 && at < 128) {

            const std::uint32_t m = n & 127;
            const std::uint64_t v = m >= 64 ? value : value & ((std::uint64_t{1} << m) - 1);
            if (at == 0) {
                lo |= v;
            } else if (at < 64) {
                lo |= v << at;
                hi |= v >> (64 - at);
            } else {
                hi |= v << (at - 64);
            }
        }
        at += n;
    }
    void write(std::uint8_t* out) const {
        put_le(out, lo, 8);
        put_le(out + 8, hi, 8);
    }
};


constexpr std::uint8_t kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,  8,  15, 2,  8,
    2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,  6,  2,  6,  8,
    15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15,
};
constexpr std::uint8_t kAnchor3a[64] = {
    3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,  3,  3,  8,  15, 3,  3,  6,  10, 5,  8,
    8,  6,  8,  5,  15, 15, 8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15, 3,  15, 5,  5,
    5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3,
};
constexpr std::uint8_t kAnchor3b[64] = {
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,  15, 8,  3,  15,
    6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,  15, 3,  15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};


constexpr std::uint8_t kBc7Bits[8][4] = {
    {4, 4, 4, 0}, {6, 6, 6, 0}, {5, 5, 5, 0}, {7, 7, 7, 0}, {5, 5, 5, 6}, {7, 7, 7, 8}, {7, 7, 7, 7}, {5, 5, 5, 5},
};

constexpr std::uint8_t kBc7Index[8][2] = {
    {3, 0}, {3, 0}, {2, 0}, {2, 0}, {3, 2}, {2, 2}, {4, 0}, {2, 0},
};

inline bool pbits(unsigned mode) { return mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7; }


inline std::array<std::uint8_t, 4> bc7_precision(std::uint16_t ctrl) {
    const unsigned mode = ctrl & 7;
    const unsigned rot = ctrl >> 3 & 3;
    std::array<std::uint8_t, 4> p = {kBc7Bits[mode][0], kBc7Bits[mode][1], kBc7Bits[mode][2], kBc7Bits[mode][3]};
    if (rot != 0) {
        std::swap(p[3], p[rot - 1]);
    } else if (pbits(mode)) {
        p[0] = static_cast<std::uint8_t>(p[0] + 1);
    }
    return p;
}

void bc7_pack(const Bc7Ep& ep, const Bc7Ix& ix, const std::array<std::uint8_t, 4>& prec, std::uint8_t* out) {
    const std::uint16_t ctrl = ep.ctrl;
    const unsigned mode = ctrl & 7;
    const std::size_t subsets = (ctrl & 0xff) >> 6;
    const std::size_t part = ctrl >> 8 & 63;
    const std::size_t rot = ctrl >> 3 & 3;
    Bits w;
    w.put(std::uint64_t{1} << mode, mode + 1);
    switch (mode) {
    case 0:
        w.put(part, 4);
        break;
    case 1:
    case 2:
    case 3:
    case 7:
        w.put(part, 6);
        break;
    case 4:
        w.put(rot, 2);
        w.put(ctrl >> 5 & 1, 1);
        break;
    case 5:
        w.put(rot, 2);
        break;
    default:
        break;
    }
    const std::size_t ends = 2 * subsets;


    const auto field = [&](std::size_t row, std::size_t col, std::uint8_t p) -> std::uint64_t {
        return static_cast<std::uint16_t>(ep.rows[row][col] >> (static_cast<std::uint8_t>(16 - p) & 15));
    };
    std::array<std::size_t, 4> order = {0, 1, 2, 3};
    if (rot != 0) {
        std::swap(order[3], order[rot - 1]);
    }
    for (const std::size_t ch : order) {
        const std::uint8_t p = prec[ch];
        if (p == 0) {
            continue;
        }
        const std::uint32_t drop = ch == 0 && pbits(mode) ? 1 : 0;
        for (std::size_t e = 0; e < ends; ++e) {
            w.put(field(e / 2, 2 * ch + e % 2, p) >> drop, std::uint32_t{p} - drop);
        }
    }
    if (mode == 1) {
        for (std::size_t row = 0; row < subsets; ++row) {
            w.put(field(row, 0, prec[0]) & 1, 1);
        }
    } else if (pbits(mode)) {
        for (std::size_t e = 0; e < ends; ++e) {
            w.put(field(e / 2, e % 2, prec[0]) & 1, 1);
        }
    }
    std::uint8_t anchors[3] = {0, 0, 0};
    std::size_t anchor_count = 1;
    if (subsets == 2) {
        anchors[1] = kAnchor2[part];
        anchor_count = 2;
    } else if (subsets == 3) {
        anchors[1] = kAnchor3a[part];
        anchors[2] = kAnchor3b[part];
        anchor_count = 3;
    }
    const auto indices = [&](std::size_t set) {
        const std::uint8_t bits = ix.bits[set];
        for (std::size_t p = 0; p < 16; ++p) {
            const bool anchor = std::find(anchors, anchors + anchor_count, static_cast<std::uint8_t>(p)) !=
                                anchors + anchor_count;
            const std::uint32_t n = std::uint32_t{bits} - (anchor ? 1u : 0u);
            w.put(static_cast<std::uint16_t>(ix.ix[set][p]), n);
        }
    };
    switch (mode) {
    case 4:
        indices(1);
        indices(0);
        break;
    case 5:
        indices(0);
        indices(1);
        break;
    default:
        indices(0);
        break;
    }
    w.write(out);
}

bool bc7(const std::uint8_t* records, std::size_t stride, std::span<std::uint8_t> out, const Tables& t) {
    std::vector<std::pair<Bc7Ep, Bc7Ix>> history(kHistory);
    const std::pair<Bc7Ep, Bc7Ix> zero{};
    const std::size_t blocks = out.size() / 16;
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint8_t* s = records + i * stride;
        const auto [ep_i, ix_i] = references(s, true);
        const std::uint16_t ctrl = t.ctrl7[static_cast<std::size_t>(s[31] & 15) << 5 | (s[30] & 31)];
        if (ctrl == 0) {
            return false;
        }
        const unsigned mode = ctrl & 7;
        const std::size_t subsets = (ctrl & 0xff) >> 6;
        const auto prec = bc7_precision(ctrl);
        const bool three = subsets == 3;
        const auto or0 = [&](std::uint8_t v) -> std::uint8_t { return three ? 0 : v; };
        const auto if3 = [&](std::uint8_t v) -> std::uint8_t { return three ? v : 0; };
        const std::uint8_t stored[3][8] = {
            {s[4], s[5], s[6], s[7], s[8], s[9], or0(s[10]), or0(s[11])},
            {s[12], s[13], s[14], s[15], s[16], s[17], or0(s[18]), or0(s[19])},
            {s[20], s[21], if3(s[10]), if3(s[11]), if3(s[18]), if3(s[19]), 0, 0},
        };
        const Bc7Ep& rf = pick(i, ep_i, history, t.bc7, zero).first;
        Bc7Ep ep{};
        ep.ctrl = ctrl;

        const std::size_t common = std::min<std::size_t>(subsets, (rf.ctrl & 0xff) >> 6);
        const std::size_t columns = prec[3] > 0 ? 8 : 6;
        for (std::size_t c = 0; c < columns; ++c) {
            const std::uint8_t p = prec[c / 2];
            if (p == 0) {
                continue;
            }
            const auto mask = static_cast<std::uint16_t>(0xffffu << (16 - p));
            for (std::size_t row = 0; row < std::min<std::size_t>(subsets, 3); ++row) {
                const std::uint8_t v = stored[row][c];
                const std::uint16_t neg = (v & 1) != 0 ? 0xffff : 0;
                const auto residual = static_cast<std::uint16_t>((static_cast<std::uint16_t>(v >> 1) ^ neg) << (16 - p));
                const auto base = static_cast<std::uint16_t>(rf.rows[row < common ? row : 0][c] & mask);
                ep.rows[row][c] = static_cast<std::uint16_t>(base + residual);
            }
        }
        const auto& bits = kBc7Index[mode];
        const Bc7Ix& rx = pick(i, ix_i, history, t.bc7, zero).second;
        Bc7Ix ix{};
        ix.bits = {bits[0], bits[1]};
        constexpr std::size_t kOffsets[2][4] = {{22, 24, 26, 28}, {12, 14, 16, 20}};
        for (std::size_t set = 0; set < 2; ++set) {
            if (bits[set] == 0) {
                continue;
            }
            for (std::size_t k = 0; k < 4; ++k) {
                const std::size_t o = kOffsets[set][k];
                for (std::size_t b = 0; b < 2; ++b) {
                    const auto pair = nibbles(s[o + b], k < 3);
                    for (std::size_t h = 0; h < 2; ++h) {
                        const std::size_t p = 4 * k + 2 * b + h;
                        ix.ix[set][p] = add_index(pair[h], bits[set], rx.ix[set][p], rx.bits[set]);
                    }
                }
            }
        }
        history[i % kHistory] = {ep, ix};
        bc7_pack(ep, ix, prec, out.data() + i * 16);
    }
    return true;
}


constexpr std::uint8_t kBc6Bits[15][4] = {
    {0, 0, 0, 0},     {10, 5, 5, 5}, {7, 6, 6, 6}, {11, 5, 4, 4}, {11, 4, 5, 4}, {11, 4, 4, 5},
    {9, 5, 5, 5},     {8, 6, 5, 5},  {8, 5, 6, 5}, {8, 5, 5, 6},  {6, 6, 6, 6},  {10, 10, 10, 10},
    {11, 9, 9, 9},    {12, 8, 8, 8}, {16, 4, 4, 4},
};

constexpr std::pair<std::uint64_t, std::uint32_t> kBc6Code[15] = {
    {0, 0}, {0, 2}, {1, 2}, {2, 5}, {6, 5}, {10, 5}, {14, 5}, {18, 5},
    {22, 5}, {26, 5}, {30, 5}, {3, 5}, {7, 5}, {11, 5}, {15, 5},
};


struct Run {
    std::uint8_t ch, ep, first, len;
};

constexpr std::uint8_t R = 0;
constexpr std::uint8_t G = 1;
constexpr std::uint8_t B = 2;
constexpr Run D = {3, 0, 0, 5};

constexpr Run r(std::uint8_t ch, std::uint8_t ep, std::uint8_t first, std::uint8_t len) { return {ch, ep, first, len}; }


constexpr Run kLayout1[] = {
    r(G, 2, 4, 1), r(B, 2, 4, 1), r(B, 3, 4, 1), r(R, 0, 0, 10), r(G, 0, 0, 10), r(B, 0, 0, 10),
    r(R, 1, 0, 5), r(G, 3, 4, 1), r(G, 2, 0, 4), r(G, 1, 0, 5), r(B, 3, 0, 1), r(G, 3, 0, 4),
    r(B, 1, 0, 5), r(B, 3, 1, 1), r(B, 2, 0, 4), r(R, 2, 0, 5), r(B, 3, 2, 1), r(R, 3, 0, 5),
    r(B, 3, 3, 1), D,
};
constexpr Run kLayout2[] = {
    r(G, 2, 5, 1), r(G, 3, 4, 1), r(G, 3, 5, 1), r(R, 0, 0, 7), r(B, 3, 0, 1), r(B, 3, 1, 1),
    r(B, 2, 4, 1), r(G, 0, 0, 7), r(B, 2, 5, 1), r(B, 3, 2, 1), r(G, 2, 4, 1), r(B, 0, 0, 7),
    r(B, 3, 3, 1), r(B, 3, 5, 1), r(B, 3, 4, 1), r(R, 1, 0, 6), r(G, 2, 0, 4), r(G, 1, 0, 6),
    r(G, 3, 0, 4), r(B, 1, 0, 6), r(B, 2, 0, 4), r(R, 2, 0, 6), r(R, 3, 0, 6), D,
};
constexpr Run kLayout3[] = {
    r(R, 0, 0, 10), r(G, 0, 0, 10), r(B, 0, 0, 10), r(R, 1, 0, 5), r(R, 0, 10, 1), r(G, 2, 0, 4),
    r(G, 1, 0, 4), r(G, 0, 10, 1), r(B, 3, 0, 1), r(G, 3, 0, 4), r(B, 1, 0, 4), r(B, 0, 10, 1),
    r(B, 3, 1, 1), r(B, 2, 0, 4), r(R, 2, 0, 5), r(B, 3, 2, 1), r(R, 3, 0, 5), r(B, 3, 3, 1), D,
};
constexpr Run kLayout4[] = {
    r(R, 0, 0, 10), r(G, 0, 0, 10), r(B, 0, 0, 10), r(R, 1, 0, 4), r(R, 0, 10, 1), r(G, 3, 4, 1),
    r(G, 2, 0, 4), r(G, 1, 0, 5), r(G, 0, 10, 1), r(G, 3, 0, 4), r(B, 1, 0, 4), r(B, 0, 10, 1),
    r(B, 3, 1, 1), r(B, 2, 0, 4), r(R, 2, 0, 4), r(B, 3, 0, 1), r(B, 3, 2, 1), r(R, 3, 0, 4),
    r(G, 2, 4, 1), r(B, 3, 3, 1), D,
};
constexpr Run kLayout5[] = {
    r(R, 0, 0, 10), r(G, 0, 0, 10), r(B, 0, 0, 10), r(R, 1, 0, 4), r(R, 0, 10, 1), r(B, 2, 4, 1),
    r(G, 2, 0, 4), r(G, 1, 0, 4), r(G, 0, 10, 1), r(B, 3, 0, 1), r(G, 3, 0, 4), r(B, 1, 0, 5),
    r(B, 0, 10, 1), r(B, 2, 0, 4), r(R, 2, 0, 4), r(B, 3, 1, 1), r(B, 3, 2, 1), r(R, 3, 0, 4),
    r(B, 3, 4, 1), r(B, 3, 3, 1), D,
};
constexpr Run kLayout6[] = {
    r(R, 0, 0, 9), r(B, 2, 4, 1), r(G, 0, 0, 9), r(G, 2, 4, 1), r(B, 0, 0, 9), r(B, 3, 4, 1),
    r(R, 1, 0, 5), r(G, 3, 4, 1), r(G, 2, 0, 4), r(G, 1, 0, 5), r(B, 3, 0, 1), r(G, 3, 0, 4),
    r(B, 1, 0, 5), r(B, 3, 1, 1), r(B, 2, 0, 4), r(R, 2, 0, 5), r(B, 3, 2, 1), r(R, 3, 0, 5),
    r(B, 3, 3, 1), D,
};
constexpr Run kLayout7[] = {
    r(R, 0, 0, 8), r(G, 3, 4, 1), r(B, 2, 4, 1), r(G, 0, 0, 8), r(B, 3, 2, 1), r(G, 2, 4, 1),
    r(B, 0, 0, 8), r(B, 3, 3, 1), r(B, 3, 4, 1), r(R, 1, 0, 6), r(G, 2, 0, 4), r(G, 1, 0, 5),
    r(B, 3, 0, 1), r(G, 3, 0, 4), r(B, 1, 0, 5), r(B, 3, 1, 1), r(B, 2, 0, 4), r(R, 2, 0, 6),
    r(R, 3, 0, 6), D,
};
constexpr Run kLayout8[] = {
    r(R, 0, 0, 8), r(B, 3, 0, 1), r(B, 2, 4, 1), r(G, 0, 0, 8), r(G, 2, 5, 1), r(G, 2, 4, 1),
    r(B, 0, 0, 8), r(G, 3, 5, 1), r(B, 3, 4, 1), r(R, 1, 0, 5), r(G, 3, 4, 1), r(G, 2, 0, 4),
    r(G, 1, 0, 6), r(G, 3, 0, 4), r(B, 1, 0, 5), r(B, 3, 1, 1), r(B, 2, 0, 4), r(R, 2, 0, 5),
    r(B, 3, 2, 1), r(R, 3, 0, 5), r(B, 3, 3, 1), D,
};
constexpr Run kLayout9[] = {
    r(R, 0, 0, 8), r(B, 3, 1, 1), r(B, 2, 4, 1), r(G, 0, 0, 8), r(B, 2, 5, 1), r(G, 2, 4, 1),
    r(B, 0, 0, 8), r(B, 3, 5, 1), r(B, 3, 4, 1), r(R, 1, 0, 5), r(G, 3, 4, 1), r(G, 2, 0, 4),
    r(G, 1, 0, 5), r(B, 3, 0, 1), r(G, 3, 0, 4), r(B, 1, 0, 6), r(B, 2, 0, 4), r(R, 2, 0, 5),
    r(B, 3, 2, 1), r(R, 3, 0, 5), r(B, 3, 3, 1), D,
};
constexpr Run kLayout10[] = {
    r(R, 0, 0, 6), r(G, 3, 4, 1), r(B, 3, 0, 1), r(B, 3, 1, 1), r(B, 2, 4, 1), r(G, 0, 0, 6),
    r(G, 2, 5, 1), r(B, 2, 5, 1), r(B, 3, 2, 1), r(G, 2, 4, 1), r(B, 0, 0, 6), r(G, 3, 5, 1),
    r(B, 3, 3, 1), r(B, 3, 5, 1), r(B, 3, 4, 1), r(R, 1, 0, 6), r(G, 2, 0, 4), r(G, 1, 0, 6),
    r(G, 3, 0, 4), r(B, 1, 0, 6), r(B, 2, 0, 4), r(R, 2, 0, 6), r(R, 3, 0, 6), D,
};
constexpr Run kLayout11[] = {
    r(R, 0, 0, 10), r(G, 0, 0, 10), r(B, 0, 0, 10), r(R, 1, 0, 10), r(G, 1, 0, 10), r(B, 1, 0, 10),
};
constexpr Run kLayout12[] = {
    r(R, 0, 0, 10), r(G, 0, 0, 10), r(B, 0, 0, 10), r(R, 1, 0, 9), r(R, 0, 10, 1), r(G, 1, 0, 9),
    r(G, 0, 10, 1), r(B, 1, 0, 9), r(B, 0, 10, 1),
};
constexpr Run kLayout13[] = {
    r(R, 0, 0, 10), r(G, 0, 0, 10), r(B, 0, 0, 10), r(R, 1, 0, 8), r(R, 0, 10, 2), r(G, 1, 0, 8),
    r(G, 0, 10, 2), r(B, 1, 0, 8), r(B, 0, 10, 2),
};
constexpr Run kLayout14[] = {
    r(R, 0, 0, 10), r(G, 0, 0, 10), r(B, 0, 0, 10), r(R, 1, 0, 4), r(R, 0, 10, 6), r(G, 1, 0, 4),
    r(G, 0, 10, 6), r(B, 1, 0, 4), r(B, 0, 10, 6),
};

constexpr std::span<const Run> kBc6Layout[15] = {
    {},        kLayout1,  kLayout2,  kLayout3,  kLayout4,  kLayout5,  kLayout6,  kLayout7,
    kLayout8,  kLayout9,  kLayout10, kLayout11, kLayout12, kLayout13, kLayout14,
};

inline std::uint8_t bc6_index_bits(std::uint8_t mode) {
    if (mode >= 1 && mode <= 10) {
        return 3;
    }
    if (mode >= 11 && mode <= 14) {
        return 4;
    }
    return 0;
}

bool bc6(const std::uint8_t* records, std::size_t stride, std::span<std::uint8_t> out,
         const std::vector<std::pair<Bc6Ep, Bc6Ix>>& dictionary) {
    std::vector<std::pair<Bc6Ep, Bc6Ix>> history(kHistory);
    std::pair<Bc6Ep, Bc6Ix> zero{};
    zero.second.mode = 11;
    const auto z = [](std::uint16_t v) { return static_cast<std::uint16_t>((v >> 1) ^ (0u - (v & 1u))); };
    const auto wide = [](std::uint8_t a, std::uint8_t b) {
        unsigned acc = 0;
        for (unsigned k = 0; k < 8; ++k) {
            acc |= (a >> k & 1u) << (2 * k) | (b >> k & 1u) << (2 * k + 1);
        }
        return static_cast<std::uint16_t>(acc);
    };
    const std::size_t blocks = out.size() / 16;
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint8_t* s = records + i * stride;
        const auto [ep_i, ix_i] = references(s, true);
        const auto mode = static_cast<std::uint8_t>(s[30] + 1);
        if (mode < 1 || mode > 14) {
            return false;
        }
        const bool two = mode < 11;
        std::array<unsigned, 4> shift;
        for (std::size_t k = 0; k < 4; ++k) {
            shift[k] = 16 - unsigned{kBc6Bits[mode][k]};
        }
        const std::array<std::uint16_t, 3> e0 = {z(wide(s[4], s[5])), z(wide(s[6], s[7])), z(wide(s[8], s[9]))};
        const std::array<std::uint16_t, 3> e1 = {z(wide(s[10], s[11])), z(wide(s[12], s[13])),
                                                 z(wide(s[14], s[15]))};
        std::array<std::uint16_t, 3> e2 = e0;
        std::array<std::uint16_t, 3> e3 = e1;
        if (two) {
            e2 = {z(s[16]), z(s[18]), z(s[20])};
            e3 = {z(s[17]), z(s[19]), z(s[21])};
        }
        const Bc6Ep& rf = pick(i, ep_i, history, dictionary, zero).first;
        Bc6Ep ep{};


        const auto mask = [](unsigned sh) { return static_cast<std::uint16_t>(0xffffu << sh); };
        for (std::size_t c = 0; c < 3; ++c) {
            ep.e[0][c] = static_cast<std::uint16_t>(static_cast<std::uint16_t>(rf.e[0][c] + (e0[c] << shift[0])) &
                                                    mask(shift[0]));
        }
        const std::array<std::uint16_t, 3>* res[3] = {&e1, &e2, &e3};
        for (std::size_t k = 0; k < (two ? 3u : 1u); ++k) {
            for (std::size_t c = 0; c < 3; ++c) {
                ep.e[k + 1][c] = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(rf.e[k + 1][c] + ((*res[k])[c] << shift[1 + c])) & mask(shift[1 + c]));
            }
        }
        if (!two) {
            for (std::size_t c = 0; c < 3; ++c) {
                ep.e[2][c] = static_cast<std::uint16_t>((ep.e[0][c] >> shift[0]) << shift[1]);
            }
            ep.e[3] = ep.e[1];
        }
        const Bc6Ix& rx = pick(i, ix_i, history, dictionary, zero).second;
        const std::uint8_t bits = bc6_index_bits(mode);
        Bc6Ix ix{};
        ix.mode = mode;
        for (std::size_t k = 0; k < 8; ++k) {
            const auto pair = nibbles(s[22 + k], k < 6);
            for (std::size_t h = 0; h < 2; ++h) {
                const std::size_t p = 2 * k + h;
                ix.ix[p] = add_index(pair[h], bits, rx.ix[p], bc6_index_bits(rx.mode));
            }
        }
        history[i % kHistory] = {ep, ix};

        const auto field = [&](std::size_t k, std::size_t c) -> std::uint64_t {
            const unsigned sh = k == 0 ? shift[0] : shift[1 + c];
            return static_cast<std::uint16_t>(ep.e[k][c] ^ 0x8000) >> sh;
        };
        Bits w;
        w.put(kBc6Code[mode].first, kBc6Code[mode].second);
        const std::uint8_t part = s[31] & 31;
        for (const Run& run : kBc6Layout[mode]) {
            if (run.ch == 3) {
                w.put(part, 5);
            } else {
                w.put(field(run.ep, run.ch) >> run.first, run.len);
            }
        }
        const std::uint8_t anchor = two ? kAnchor2[part] : 0;
        for (std::size_t p = 0; p < 16; ++p) {
            const bool short_ = p == 0 || p == anchor;
            const auto v = static_cast<std::uint16_t>(static_cast<std::uint16_t>(ix.ix[p]) + (1u << (bits - 1)));
            w.put(v, std::uint32_t{bits} - (short_ ? 1u : 0u));
        }
        w.write(out.data() + i * 16);
    }
    return true;
}

}   

std::unique_ptr<Tables> parse(std::span<const std::uint8_t> bytes, std::string* error) {
    const auto fail = [&](const char* what) {
        if (error != nullptr) {
            *error = message(what);
        }
        return std::unique_ptr<Tables>();
    };
    if (bytes.size() != kTablesSize) {
        return fail("table file has the wrong size");
    }
    auto t = std::make_unique<Tables>();
    Reader rd{bytes.data()};
    for (std::size_t m = 0; m < kModels; ++m) {
        for (auto& f : t->freq[m]) {
            f = rd.u16();
        }

        const std::size_t used = m / 16 + 2;
        std::uint32_t at = 0;
        for (std::size_t b = 0; b < used; ++b) {
            if (t->freq[m][b] == 0) {
                return fail("a model has an empty bucket");
            }
            t->start[m][b] = at;
            const std::uint64_t f = t->freq[m][b];
            t->recip[m][b] = ((std::uint64_t{1} << 32) + f - 1) / f;
            at += t->freq[m][b] << (b == 0 ? 0 : b - 1);
        }
        if (at != 1u << 16) {
            return fail("a model does not tile 65536");
        }
        for (std::size_t b = used; b < kBuckets + 1; ++b) {
            t->start[m][b] = 1u << 16;
        }
        for (std::size_t window = 0; window < 256; ++window) {
            const auto first = static_cast<std::uint32_t>(window) << 8;
            std::size_t count = 0;
            for (std::size_t b = 1; b < kBuckets + 1; ++b) {
                count += t->start[m][b] <= first ? 1 : 0;
            }
            t->bucket[m][window] = static_cast<std::uint8_t>(count);
        }
    }
    t->rank1 = rd.bytes<256>();
    t->rank3a = rd.bytes<64>();
    t->rank3c = rd.bytes<256>();
    t->rank4 = rd.bytes<64>();
    t->rank5 = rd.bytes<64>();
    for (auto& c : t->ctrl7) {
        c = rd.u16();
    }
    const auto plain_entries = [&](std::size_t ep, std::size_t ix) {
        std::vector<Plain> out(kDictionary);
        for (auto& p : out) {
            for (std::size_t k = 0; k < ep; ++k) {
                p.ep[k] = rd.u8();
            }
            for (std::size_t k = 0; k < ix; ++k) {
                p.ix[k] = rd.u8();
            }
        }
        return out;
    };
    t->bc1 = plain_entries(6, 16);
    t->bc3 = plain_entries(8, 32);
    t->bc4 = plain_entries(2, 16);
    t->bc5 = plain_entries(4, 32);
    t->bc6.resize(kDictionary);
    for (auto& [ep, ix] : t->bc6) {

        rd.u8();
        for (auto& row : ep.e) {
            for (auto& v : row) {
                v = rd.u16();
            }
        }
        ix.mode = rd.u8();
        for (auto& v : ix.ix) {
            v = static_cast<std::int16_t>(rd.u16());
        }
    }
    t->bc7.resize(kDictionary);
    for (auto& [ep, ix] : t->bc7) {
        ep.ctrl = rd.u16();
        for (auto& row : ep.rows) {
            for (auto& v : row) {
                v = rd.u16();
            }
        }
        ix.bits[0] = rd.u8();
        ix.bits[1] = rd.u8();

        rd.u16();

        for (std::size_t k = 0; k < 31; ++k) {
            ix.ix[k / 16][k % 16] = static_cast<std::int16_t>(rd.u16());
        }
    }
    return t;
}

const Tables* builtin(std::string* error) {
    struct Parsed {
        std::unique_ptr<Tables> tables;
        std::string error;
    };
    static const Parsed parsed = [] {
        Parsed p;
        p.tables = parse(kBuiltinTables, &p.error);
        return p;
    }();
    if (parsed.tables == nullptr && error != nullptr) {
        *error = parsed.error;
    }
    return parsed.tables.get();
}

std::optional<std::string> decode(std::uint8_t bc, std::span<const std::uint8_t> src, std::span<std::uint8_t> dst,
                                  const Tables& tables) {
    std::size_t block;
    switch (bc) {
    case 1:
    case 4:
        block = 8;
        break;
    case 3:
    case 5:
    case 6:
    case 7:
        block = 16;
        break;
    default:
        return message("unsupported BC format");
    }
    if (dst.empty() || dst.size() % block != 0) {
        return message("destination is not a whole number of blocks");
    }
    const std::size_t blocks = dst.size() / block;
    static constexpr std::uint8_t kW1[] = {7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7};
    static constexpr std::uint8_t kW3[] = {3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7};
    static constexpr std::uint8_t kW4[] = {7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
    static constexpr std::uint8_t kW5[] = {3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
    static constexpr std::uint8_t kW6[] = {3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4};
    static constexpr std::uint8_t kW7[] = {3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4};
    std::span<const std::uint8_t> lane_widths;
    std::size_t per = 2;
    switch (bc) {
    case 1:
        lane_widths = kW1;
        per = 1;
        break;
    case 3:
        lane_widths = kW3;
        break;
    case 4:
        lane_widths = kW4;
        per = 1;
        break;
    case 5:
        lane_widths = kW5;
        break;
    case 6:
        lane_widths = kW6;
        break;
    default:
        lane_widths = kW7;
        break;
    }
    const std::size_t stride = lane_widths.size() * per;
    std::vector<std::uint8_t> records(blocks * stride);
    std::size_t next = 0;
    const auto fetch = [&](std::size_t) -> int { return next < src.size() ? src[next++] : -1; };
    if (!run_lanes(tables, lane_widths, per, blocks, fetch, records.data())) {
        return message("the stream ends early or is corrupt");
    }
    const Tables& t = tables;
    switch (bc) {
    case 1:
        plain(records.data(), stride, dst, block, t.bc1, kBc1, [&](const std::uint8_t* s) { return bc1_residual(t, s); },
              bc1_pack);
        break;
    case 3:
        plain(records.data(), stride, dst, block, t.bc3, kBc3, [&](const std::uint8_t* s) { return bc3_residual(t, s); },
              bc3_pack);
        break;
    case 4:
        plain(records.data(), stride, dst, block, t.bc4, kBc4, [&](const std::uint8_t* s) { return bc4_residual(t, s); },
              bc4_pack);
        break;
    case 5:
        plain(records.data(), stride, dst, block, t.bc5, kBc5, [&](const std::uint8_t* s) { return bc5_residual(t, s); },
              bc5_pack);
        break;
    case 6:
        if (!bc6(records.data(), stride, dst, t.bc6)) {
            return message("BC6H mode out of range");
        }
        break;
    default:
        if (!bc7(records.data(), stride, dst, t)) {
            return message("unused BC7 control code");
        }
        break;
    }
    return std::nullopt;
}

namespace detail {

bool lanes(const Tables& t, std::span<const std::uint8_t> widths, std::size_t per, std::size_t blocks,
           const std::function<int(std::size_t)>& fetch, std::span<std::uint8_t> records) {
    return run_lanes(t, widths, per, blocks, fetch, records.data());
}

std::size_t bucket_of(const Tables& t, std::uint8_t model, std::uint32_t slot) {
    return bcpack::bucket_of(t, model, slot);
}

std::pair<int, std::uint32_t> payload_step(const Tables& t, std::uint8_t model, std::uint32_t x) {
    Lane lane;
    lane.phase = 3;
    lane.model = model;
    lane.x = x;
    lane.left = 1;
    step(lane, t);
    return {lane.has_out ? lane.out : -1, lane.x};
}

}   

}   
