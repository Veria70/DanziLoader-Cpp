// SPDX-License-Identifier: GPL-3.0-or-later
// XMA2 packets and frames (`victory-formats/src/xma.rs`; DanziLoader /
// victory-perf, see NOTICE.md); see xma.hpp. The frame decoder follows
// FFmpeg's `wmaprodec.c` (LGPL-2.1-or-later, used under GPL-3.0-or-later).
//
// Float results match the reference's bit for bit: no contraction into
// FMAs, and `sin`, `cos` and `powf` are the C runtime's own functions
// called one value at a time (the reference's calls), never vectorised or
// merged copies.
#include "formats/xma.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

#pragma fp_contract(off)

namespace danzi::formats::xma {

namespace {

struct Pair {
    std::uint16_t symbol;
    std::uint8_t len;
};
#include "formats/xma_tables.inc"

constexpr std::size_t PACKET_BITS = PACKET_BYTES * 8;
constexpr std::size_t HEADER_BITS = 32;
constexpr std::uint32_t LENGTH_BITS = 15;

constexpr std::size_t MIN_SUBFRAME = SUBFRAME_SAMPLES;


double (*volatile const SIN)(double) = static_cast<double (*)(double)>(&::sin);
double (*volatile const COS)(double) = static_cast<double (*)(double)>(&::cos);
float (*volatile const POWF)(float, float) = static_cast<float (*)(float, float)>(&::powf);
float (*volatile const ROUNDF)(float) = static_cast<float (*)(float)>(&::roundf);

}   


std::optional<Packet> packet(std::span<const std::uint8_t> data, std::size_t index) {
    if (index > SIZE_MAX / PACKET_BYTES) {
        return std::nullopt;
    }
    std::size_t start = index * PACKET_BYTES;
    if (start > data.size() || data.size() - start < 4) {
        return std::nullopt;
    }
    const std::uint8_t* b = data.data() + start;
    std::uint32_t h = static_cast<std::uint32_t>(b[0]) << 24 | static_cast<std::uint32_t>(b[1]) << 16 | static_cast<std::uint32_t>(b[2]) << 8 | b[3];
    return Packet{h >> 26, (h >> 11) & 0x7fff, h & 0xff};
}

namespace {


struct Cursor {
    std::span<const std::uint8_t> data;
    std::size_t pos;

    std::optional<std::uint32_t> bit() {
        if (pos % PACKET_BITS == 0) {

            std::size_t previous = pos / PACKET_BITS;
            if (previous > 0) {
                auto p = packet(data, previous - 1);
                if (!p) {
                    return std::nullopt;
                }
                pos = (previous + p->skip) * PACKET_BITS;
            }
            pos += HEADER_BITS;
        }
        if (pos / 8 >= data.size()) {
            return std::nullopt;
        }
        std::uint8_t byte = data[pos / 8];
        std::uint32_t b = (byte >> (7 - pos % 8)) & 1;
        pos += 1;
        return b;
    }

    std::optional<std::uint32_t> bits(std::uint32_t n) {
        std::uint32_t v = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            auto b = bit();
            if (!b) {
                return std::nullopt;
            }
            v = v << 1 | *b;
        }
        return v;
    }
};


std::optional<std::size_t> next_frame(std::span<const std::uint8_t> data, std::size_t start, std::size_t end) {
    std::size_t index = start / PACKET_BITS;
    auto header = packet(data, index);
    if (!header) {
        return std::nullopt;
    }
    std::size_t at = index * PACKET_BITS + HEADER_BITS + header->first_frame_bit;
    std::uint32_t number = 0;
    while (at < start) {
        Cursor cursor{data, at};
        auto bits = cursor.bits(LENGTH_BITS);
        if (!bits) {
            return std::nullopt;
        }


        if (*bits <= LENGTH_BITS) {
            return std::nullopt;
        }
        at += *bits;
        number += 1;
    }
    bool last = number + 1 >= header->frames;
    if (end % PACKET_BITS == 0 || (last && end / PACKET_BITS == index)) {

        auto here = packet(data, (end - 1) / PACKET_BITS);
        if (!here) {
            return std::nullopt;
        }
        std::size_t next = (end - 1) / PACKET_BITS + 1 + here->skip;
        auto h = packet(data, next);
        if (!h) {
            return std::nullopt;
        }
        end = next * PACKET_BITS + HEADER_BITS + h->first_frame_bit;
    }
    if (end / 8 < data.size()) {
        return end;
    }
    return std::nullopt;
}

}   

std::optional<Frame> read_frame(std::span<const std::uint8_t> data, std::size_t start) {
    Cursor first{data, start};
    auto length = first.bits(LENGTH_BITS);
    if (!length) {
        return std::nullopt;
    }
    std::size_t bits = *length;
    if (bits <= LENGTH_BITS || bits == 0x7fff) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out((bits + 7) / 8, 0);
    Cursor cursor{data, start};
    for (std::size_t i = 0; i < bits; ++i) {
        auto b = cursor.bit();
        if (!b) {
            return std::nullopt;
        }
        out[i / 8] |= static_cast<std::uint8_t>(*b << (7 - i % 8));
    }
    std::size_t end = cursor.pos;
    auto next = next_frame(data, start, end);
    return Frame{std::move(out), bits, start, next};
}


struct Bits {
    std::span<const std::uint8_t> data;
    std::size_t pos;
    std::size_t end;

    std::uint32_t peek32() const {
        std::uint64_t v = 0;
        std::size_t byte = pos / 8;
        for (std::size_t i = 0; i < 5; ++i) {
            std::size_t at = byte + i;
            v = v << 8 | (at < data.size() ? data[at] : 0);
        }
        return static_cast<std::uint32_t>(v << (24 + pos % 8) >> 32);
    }
    std::uint32_t read(std::uint32_t n) {
        if (n == 0) {
            return 0;
        }
        std::uint32_t v = peek32() >> (32 - n);
        pos += n;
        return v;
    }
    bool bit() { return read(1) != 0; }
    std::int32_t signed_(std::uint32_t n) { return static_cast<std::int32_t>(read(n) << (32 - n)) >> (32 - n); }
    std::size_t consumed() const { return pos; }
    bool overread() const { return pos > end; }
};

namespace {


struct Vlc {
    struct Code {
        std::uint32_t code;
        std::uint8_t len;
        std::int32_t symbol;
    };

    std::vector<Code> codes;

    void add(std::int32_t symbol, std::uint8_t len, std::uint64_t& code) {
        codes.push_back({static_cast<std::uint32_t>(code), len, symbol});
        code += std::uint64_t{1} << (32 - len);
    }
    std::int32_t read(Bits& bits) const {
        std::uint32_t v = bits.peek32();
        auto it = std::partition_point(codes.begin(), codes.end(), [v](const Code& c) { return c.code <= v; });
        const Code& c = *(it - 1);
        bits.pos += c.len;
        return c.symbol;
    }
};

template <std::size_t N>
Vlc pairs(const Pair (&t)[N], std::int32_t offset) {
    Vlc v;
    std::uint64_t code = 0;
    for (const Pair& p : t) {
        v.add(static_cast<std::int32_t>(p.symbol) + offset, p.len, code);
    }
    return v;
}

struct Codes {
    Vlc scale, scale_rl;
    Vlc coef[2];
    Vlc vec4, vec2, vec1;
};


const Codes& codes() {
    static const Codes& CODES = *new Codes([] {
        Codes c;
        c.scale = pairs(SCALE, -60);
        c.scale_rl = pairs(SCALE_RL, 0);
        std::uint64_t code = 0;
        for (std::size_t i = 0; i < std::size(COEF0_SYMS) && i < std::size(COEF0_LENS); ++i) {
            c.coef[0].add(COEF0_SYMS[i], COEF0_LENS[i], code);
        }
        c.coef[1] = pairs(COEF1, 0);
        code = 0;
        for (std::size_t i = 0; i < std::size(VEC4_SYMS) && i < std::size(VEC4_LENS); ++i) {
            c.vec4.add(static_cast<std::int32_t>(VEC4_SYMS[i]) - 1, VEC4_LENS[i], code);
        }
        c.vec2 = pairs(VEC2, -1);
        c.vec1 = pairs(VEC1, 0);
        return c;
    }());
    return CODES;
}


std::uint32_t large_value(Bits& bits) {
    std::uint32_t n = 8;
    if (bits.bit()) {
        n += 8;
        if (bits.bit()) {
            n += 8;
            if (bits.bit()) {
                n += 7;
            }
        }
    }
    return bits.read(n);
}

std::uint32_t log2(std::size_t v) { return 63 - static_cast<std::uint32_t>(std::countl_zero(static_cast<std::uint64_t>(std::max<std::size_t>(v, 1)))); }


void fft(std::vector<double>& re, std::vector<double>& im) {
    std::size_t n = re.size();
    std::size_t j = 0;
    for (std::size_t i = 1; i < n; ++i) {
        std::size_t bit = n >> 1;
        while ((j & bit) != 0) {
            j ^= bit;
            bit >>= 1;
        }
        j |= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    std::size_t len = 2;
    while (len <= n) {
        double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
        double ws = SIN(angle);
        double wc = COS(angle);
        for (std::size_t start = 0; start < n; start += len) {
            double cr = 1.0, ci = 0.0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                std::size_t a = start + k, b = start + k + len / 2;
                double tr = re[b] * cr - im[b] * ci;
                double ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] = re[a] + tr;
                im[a] = im[a] + ti;
                double ncr = cr * wc - ci * ws;
                double nci = cr * ws + ci * wc;
                cr = ncr;
                ci = nci;
            }
        }
        len <<= 1;
    }
}


void imdct_half(const float* input, std::size_t n, float* out, float scale) {
    std::size_t m = n / 2;
    std::vector<double> re(m, 0.0), im(m, 0.0);
    for (std::size_t k = 0; k < m; ++k) {
        double a = input[2 * k], b = input[n - 1 - 2 * k];
        double angle = -std::numbers::pi * static_cast<double>(k) / static_cast<double>(n);
        double s = SIN(angle);
        double c = COS(angle);
        re[k] = a * c - b * s;
        im[k] = a * s + b * c;
    }
    fft(re, im);
    for (std::size_t k = 0; k < m; ++k) {
        double angle = -std::numbers::pi * static_cast<double>(4 * k + 1) / static_cast<double>(4 * n);
        double s = SIN(angle);
        double c = COS(angle);
        double wr = re[k] * c - im[k] * s;
        double wi = re[k] * s + im[k] * c;

        out[n - 1 - 2 * k] = static_cast<float>(wr) * scale;
        out[2 * k] = -static_cast<float>(wi) * scale;
    }
}


std::vector<float> sine_window(std::size_t n) {
    std::vector<float> w(n);
    for (std::size_t i = 0; i < n; ++i) {
        w[i] = static_cast<float>(SIN((static_cast<double>(i) + 0.5) * std::numbers::pi / (2.0 * static_cast<double>(n))));
    }
    return w;
}


const std::array<std::vector<float>, 3>& windows() {
    static const auto& WINDOWS = *new std::array<std::vector<float>, 3>{sine_window(128), sine_window(256), sine_window(512)};
    return WINDOWS;
}


void window_overlap(float* block, std::size_t block_len, const std::vector<float>& window) {
    std::size_t len = block_len / 2;
    for (std::size_t k = 0; k < len; ++k) {
        std::size_t i = k, j = 2 * len - 1 - k;
        float s0 = block[i], s1 = block[2 * len - 1 - k];
        float wi = window[i], wj = window[j];
        block[i] = s0 * wj - s1 * wi;
        block[j] = s0 * wi + s1 * wj;
    }
}

std::int16_t to_i16(float v) {
    float r = ROUNDF(v * 32768.0f);
    if (std::isnan(r)) {
        return 0;
    }
    return static_cast<std::int16_t>(std::clamp(r, -32768.0f, 32767.0f));
}

}   


Decoder::Decoder(std::size_t channels, std::uint32_t sample_rate) {
    channels_ = std::clamp<std::size_t>(channels, 1, 2);
    std::size_t rate = sample_rate > 44'100 ? 48'000 : sample_rate > 32'000 ? 44'100 : sample_rate > 24'000 ? 32'000 : 24'000;
    for (std::size_t i = 0; i < BLOCK_SIZES; ++i) {
        std::size_t subframe_len = FRAME_SAMPLES >> i;
        auto& offsets = sfb_offsets_[i];
        std::size_t band = 1;
        for (std::size_t f = 0; f < MAX_BANDS - 1 && f < std::size(CRITICAL_FREQ); ++f) {
            if (offsets[band - 1] >= subframe_len) {
                break;
            }
            std::size_t offset = ((subframe_len * 2 * CRITICAL_FREQ[f]) / rate + 2) & ~std::size_t{3};
            if (offset > offsets[band - 1]) {
                offsets[band] = offset;
                band += 1;
            }
            if (offset >= subframe_len) {
                break;
            }
        }
        offsets[band - 1] = subframe_len;
        num_sfb_[i] = band - 1;
    }
    for (std::size_t i = 0; i < BLOCK_SIZES; ++i) {
        for (std::size_t b = 0; b < num_sfb_[i]; ++b) {
            std::size_t offset = ((sfb_offsets_[i][b] + sfb_offsets_[i][b + 1] - 1) << i) >> 1;
            for (std::size_t x = 0; x < BLOCK_SIZES; ++x) {
                std::size_t v = 0;
                while (v + 1 < MAX_BANDS && (sfb_offsets_[x][v + 1] << x) < offset) {
                    v += 1;
                }
                sf_offsets_[i][x][b] = v;
            }
        }
    }
}

void Decoder::reset() { ch_ = {Channel{}, Channel{}}; }

bool Decoder::decode(std::span<const std::uint8_t> data, std::size_t bits, std::vector<std::int16_t>& out, const char** error) {
    Bits b{data, 0, bits};
    std::size_t len = b.read(LENGTH_BITS);
    const char* result = frame(b, len);
    if (result) {

        for (std::size_t c = 0; c < channels_; ++c) {
            ch_[c].out.fill(0.0f);
        }
    }
    for (std::size_t i = 0; i < FRAME_SAMPLES; ++i) {
        for (std::size_t c = 0; c < channels_; ++c) {
            out.push_back(to_i16(ch_[c].out[i]));
        }
    }
    for (std::size_t c = 0; c < channels_; ++c) {
        auto& o = ch_[c].out;
        std::copy(o.begin() + FRAME_SAMPLES, o.end(), o.begin());
    }
    if (error) {
        *error = result;
    }
    return result == nullptr;
}

void Decoder::flush(std::vector<std::int16_t>& out) {
    for (std::size_t i = 0; i < FRAME_SAMPLES / 2; ++i) {
        for (std::size_t c = 0; c < channels_; ++c) {
            out.push_back(to_i16(ch_[c].out[i]));
        }
    }
    reset();
}

const char* Decoder::frame(Bits& b, std::size_t len) {
    if (const char* e = tile_header(b)) {
        return e;
    }

    if (channels_ > 1 && b.bit() && b.bit()) {
        b.read(static_cast<std::uint32_t>(channels_ * channels_ * 4));
    }

    b.read(8);
    if (b.bit()) {

        if (b.bit()) {
            b.read(log2(FRAME_SAMPLES * 2));
        }
        if (b.bit()) {
            b.read(log2(FRAME_SAMPLES * 2));
        }
    }
    parsed_all_subframes_ = false;
    for (std::size_t c = 0; c < channels_; ++c) {
        ch_[c].decoded_samples = 0;
        ch_[c].cur_subframe = 0;
        ch_[c].reuse_sf = false;
    }
    while (!parsed_all_subframes_) {
        if (const char* e = subframe(b)) {
            return e;
        }
    }

    if (b.overread() || b.consumed() > len) {
        return "frame overread";
    }
    return nullptr;
}

const char* Decoder::subframe_length(Bits& b, std::size_t offset, std::size_t& len) {
    if (offset == FRAME_SAMPLES - MIN_SUBFRAME) {
        len = MIN_SUBFRAME;
        return nullptr;
    }

    std::uint32_t shift = b.bit() ? 1 + b.read(1) : 0;
    len = FRAME_SAMPLES >> shift;
    if (len < MIN_SUBFRAME) {
        return "subframe length";
    }
    return nullptr;
}

const char* Decoder::tile_header(Bits& b) {
    std::size_t num_samples[2] = {0, 0};
    bool contains[2] = {false, false};
    std::size_t channels_for_cur_subframe = channels_;
    std::size_t min_channel_len = 0;
    for (std::size_t c = 0; c < channels_; ++c) {
        ch_[c].num_subframes = 0;
    }
    bool fixed = b.bit();
    for (;;) {
        for (std::size_t c = 0; c < channels_; ++c) {
            contains[c] = num_samples[c] == min_channel_len &&
                          (fixed || channels_for_cur_subframe == 1 || min_channel_len == FRAME_SAMPLES - MIN_SUBFRAME || b.bit());
        }
        std::size_t subframe_len = 0;
        if (const char* e = subframe_length(b, min_channel_len, subframe_len)) {
            return e;
        }
        min_channel_len += subframe_len;
        for (std::size_t c = 0; c < channels_; ++c) {
            Channel& ch = ch_[c];
            if (contains[c]) {
                if (ch.num_subframes >= MAX_SUBFRAMES) {
                    return "too many subframes";
                }
                ch.subframe_len[ch.num_subframes] = subframe_len;
                num_samples[c] += subframe_len;
                ch.num_subframes += 1;
                if (num_samples[c] > FRAME_SAMPLES) {
                    return "channel longer than a frame";
                }
            } else if (num_samples[c] <= min_channel_len) {
                if (num_samples[c] < min_channel_len) {
                    channels_for_cur_subframe = 0;
                    min_channel_len = num_samples[c];
                }
                channels_for_cur_subframe += 1;
            }
        }
        if (min_channel_len >= FRAME_SAMPLES || b.overread()) {
            break;
        }
    }
    if (b.overread()) {
        return "tile header";
    }
    return nullptr;
}

const char* Decoder::channel_transform(Bits& b) {
    transform_ = false;
    if (channels_ == 1) {
        return nullptr;
    }
    if (b.bit()) {
        return "channel transform bit";
    }

    if (channels_for_cur_subframe_ == 2) {
        if (b.bit()) {
            if (b.bit()) {
                return "channel transform type";
            }
        } else {
            transform_ = true;
        }
    }
    if (transform_) {
        if (!b.bit()) {
            for (std::size_t i = 0; i < num_bands_; ++i) {
                transform_band_[i] = b.bit();
            }
        } else {
            transform_band_.fill(true);
        }
    }
    return nullptr;
}

const char* Decoder::coefficients(Bits& b, std::size_t c) {
    const Codes& codes = xma::codes();
    std::size_t table = b.bit() ? 1 : 0;
    const std::uint16_t* run = table == 1 ? COEF1_RUN : COEF0_RUN;
    const float* level = table == 1 ? COEF1_LEVEL : COEF0_LEVEL;
    std::size_t subframe_len = subframe_len_;
    Channel& ch = ch_[c];
    bool rl_mode = false;
    std::size_t cur = 0;
    std::size_t num_zeros = 0;
    while ((transmit_num_vec_coeffs_ || !rl_mode) && cur + 3 < ch.num_vec_coeffs) {
        std::uint32_t vals[4] = {0, 0, 0, 0};
        std::int32_t idx = codes.vec4.read(b);
        if (idx < 0) {
            for (std::size_t i = 0; i < 4; i += 2) {
                std::int32_t idx2 = codes.vec2.read(b);
                if (idx2 < 0) {
                    for (std::size_t k = i; k < i + 2; ++k) {
                        std::uint32_t x = static_cast<std::uint32_t>(codes.vec1.read(b));
                        if (x == std::size(VEC1) - 1) {
                            x += large_value(b);
                        }
                        vals[k] = x;
                    }
                } else {
                    vals[i] = static_cast<std::uint32_t>(idx2 >> 4);
                    vals[i + 1] = static_cast<std::uint32_t>(idx2 & 0xf);
                }
            }
        } else {
            vals[0] = static_cast<std::uint32_t>(idx >> 12);
            vals[1] = static_cast<std::uint32_t>((idx >> 8) & 0xf);
            vals[2] = static_cast<std::uint32_t>((idx >> 4) & 0xf);
            vals[3] = static_cast<std::uint32_t>(idx & 0xf);
        }
        for (std::uint32_t v : vals) {
            if (v != 0) {
                bool negative = !b.bit();
                float f = static_cast<float>(v);
                ch.coeffs[cur] = negative ? -f : f;
                num_zeros = 0;
            } else {
                ch.coeffs[cur] = 0.0f;
                num_zeros += 1;
                rl_mode |= num_zeros > subframe_len >> 8;
            }
            cur += 1;
        }
        if (b.overread()) {
            return "vector coefficients";
        }
    }
    if (cur < subframe_len) {
        std::fill(ch.coeffs.begin() + cur, ch.coeffs.begin() + subframe_len, 0.0f);

        std::size_t mask = subframe_len - 1;
        std::size_t offset = cur;
        while (offset < subframe_len) {
            std::int32_t code = codes.coef[table].read(b);
            if (code > 1) {
                offset += run[code];
                bool negative = !b.bit();
                float v = level[code];
                ch.coeffs[offset & mask] = negative ? -v : v;
            } else if (code == 1) {
                break;
            } else {
                std::int32_t value = static_cast<std::int32_t>(large_value(b));
                if (b.bit()) {
                    if (b.bit()) {
                        if (b.bit()) {


                            break;
                        }
                        offset += static_cast<std::size_t>(b.read(esc_len_)) + 4;
                    } else {
                        offset += static_cast<std::size_t>(b.read(2)) + 1;
                    }
                }
                bool negative = !b.bit();
                ch.coeffs[offset & mask] = static_cast<float>(negative ? static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(value)) : value);
            }
            if (b.overread()) {
                return "run level coefficients";
            }
            offset += 1;
        }
    }
    return nullptr;
}

const char* Decoder::scale_factors(Bits& b) {
    const Codes& codes = xma::codes();
    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        std::size_t c = channel_indexes_[i];
        auto sf_offsets = sf_offsets_[table_idx_][ch_[c].table_idx];
        Channel& ch = ch_[c];
        std::size_t target = 1 - ch.scale_factor_idx;
        ch.scale_factors = target;
        if (ch.reuse_sf) {
            auto from = ch.saved_scale_factors[ch.scale_factor_idx];
            for (std::size_t k = 0; k < num_bands_ && k < MAX_BANDS; ++k) {
                ch.saved_scale_factors[target][k] = from[sf_offsets[k]];
            }
        }
        if (ch.cur_subframe == 0 || b.bit()) {
            if (!ch.reuse_sf) {
                ch.scale_factor_step = static_cast<std::int32_t>(b.read(2)) + 1;
                std::int32_t val = 45 / ch.scale_factor_step;
                for (std::size_t band = 0; band < num_bands_; ++band) {
                    val = static_cast<std::int32_t>(static_cast<std::uint32_t>(val) + static_cast<std::uint32_t>(codes.scale.read(b)));
                    ch.saved_scale_factors[target][band] = val;
                }
            } else {
                std::size_t band = 0;
                while (band < num_bands_) {
                    std::int32_t idx = codes.scale_rl.read(b);
                    std::uint32_t skip;
                    std::int32_t val;
                    bool negative;
                    if (idx == 0) {
                        std::uint32_t code = b.read(14);
                        skip = (code & 0x3f) >> 1;
                        val = static_cast<std::int32_t>(code >> 6);
                        negative = (code & 1) == 0;
                    } else if (idx == 1) {
                        break;
                    } else {
                        skip = SCALE_RL_RUN[idx];
                        val = SCALE_RL_LEVEL[idx];
                        negative = !b.bit();
                    }
                    band += skip;
                    if (band >= num_bands_) {
                        return "scale factor run";
                    }
                    std::int32_t& sf = ch.saved_scale_factors[target][band];
                    sf = static_cast<std::int32_t>(static_cast<std::uint32_t>(sf) + static_cast<std::uint32_t>(negative ? -val : val));
                    band += 1;
                }
            }
            ch.scale_factor_idx = target;
            ch.table_idx = table_idx_;
            ch.reuse_sf = true;
        }
        const auto& sf = ch.saved_scale_factors[ch.scale_factors];
        std::int32_t max = 0;
        for (std::size_t band = 0; band < num_bands_; ++band) {
            max = band == 0 ? sf[band] : std::max(max, sf[band]);
        }
        ch.max_scale_factor = max;
    }
    return nullptr;
}

const char* Decoder::subframe(Bits& b) {
    std::size_t offset = FRAME_SAMPLES;
    std::size_t subframe_len = FRAME_SAMPLES;
    std::size_t total_samples = FRAME_SAMPLES * channels_;
    for (std::size_t c = 0; c < channels_; ++c) {
        const Channel& ch = ch_[c];
        if (offset > ch.decoded_samples) {
            offset = ch.decoded_samples;
            subframe_len = ch.subframe_len[std::min(ch.cur_subframe, MAX_SUBFRAMES - 1)];
        }
    }
    channels_for_cur_subframe_ = 0;
    for (std::size_t c = 0; c < channels_; ++c) {
        Channel& ch = ch_[c];
        total_samples -= ch.decoded_samples;
        if (offset == ch.decoded_samples && ch.cur_subframe < ch.num_subframes && subframe_len == ch.subframe_len[ch.cur_subframe]) {
            total_samples -= subframe_len;
            ch.decoded_samples += subframe_len;
            channel_indexes_[channels_for_cur_subframe_] = c;
            channels_for_cur_subframe_ += 1;
        }
    }
    if (channels_for_cur_subframe_ == 0) {
        return "no channel in subframe";
    }
    if (total_samples == 0) {
        parsed_all_subframes_ = true;
    }
    table_idx_ = log2(FRAME_SAMPLES / subframe_len);
    num_bands_ = num_sfb_[table_idx_];
    subframe_len_ = subframe_len;
    esc_len_ = log2(subframe_len - 1) + 1;


    if (b.bit()) {
        std::size_t fill = b.read(2);
        if (fill == 0) {
            std::uint32_t len = b.read(4);
            fill = static_cast<std::size_t>(b.read(len)) + 1;
        }
        b.pos += fill;
    }
    if (b.bit()) {
        return "reserved bit";
    }
    if (const char* e = channel_transform(b)) {
        return e;
    }

    bool transmit_coeffs = false;
    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        std::size_t c = channel_indexes_[i];
        ch_[c].transmit_coefs = b.bit();
        transmit_coeffs |= ch_[c].transmit_coefs;
    }
    if (transmit_coeffs) {
        transmit_num_vec_coeffs_ = b.bit();
        if (transmit_num_vec_coeffs_) {
            std::uint32_t num_bits = log2((subframe_len + 3) / 4) + 1;
            for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
                std::size_t c = channel_indexes_[i];
                std::size_t n = static_cast<std::size_t>(b.read(num_bits) << 2);
                if (n > subframe_len) {
                    return "vector coefficient count";
                }
                ch_[c].num_vec_coeffs = n;
            }
        } else {
            for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
                ch_[channel_indexes_[i]].num_vec_coeffs = subframe_len;
            }
        }

        std::int32_t quant_step = 90;
        std::int32_t step = b.signed_(6);
        quant_step += step;
        if (step == -32 || step == 31) {
            bool negative = step == -32;
            std::int32_t quant = 0;
            while (b.pos + 5 < b.end) {
                step = static_cast<std::int32_t>(b.read(5));
                if (step != 31) {
                    break;
                }
                quant += 31;
            }
            quant_step += negative ? -(quant + step) : quant + step;
        }
        if (channels_for_cur_subframe_ == 1) {
            ch_[channel_indexes_[0]].quant_step = quant_step;
        } else {
            std::uint32_t modifier_len = b.read(3);
            for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
                std::size_t c = channel_indexes_[i];
                ch_[c].quant_step = quant_step;
                if (b.bit()) {
                    ch_[c].quant_step += modifier_len > 0 ? static_cast<std::int32_t>(b.read(modifier_len)) + 1 : 1;
                }
            }
        }
        if (const char* e = scale_factors(b)) {
            return e;
        }
    }

    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        std::size_t c = channel_indexes_[i];
        if (ch_[c].transmit_coefs && b.pos < b.end) {
            if (const char* e = coefficients(b, c)) {
                return e;
            }
        } else {
            std::fill(ch_[c].coeffs.begin(), ch_[c].coeffs.begin() + subframe_len, 0.0f);
        }
    }


    std::size_t at = offset + FRAME_SAMPLES / 2;
    if (transmit_coeffs) {
        inverse_channel_transform();

        float scale = 2.0f / static_cast<float>(subframe_len) / 32768.0f;
        const auto sfb = sfb_offsets_[table_idx_];
        float tmp[FRAME_SAMPLES] = {};
        for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
            Channel& ch = ch_[channel_indexes_[i]];
            const auto sf = ch.saved_scale_factors[ch.scale_factors];
            for (std::size_t band = 0; band < num_bands_; ++band) {
                std::size_t end = std::min(sfb[band + 1], subframe_len);
                std::int32_t exp = ch.quant_step - (ch.max_scale_factor - sf[band]) * ch.scale_factor_step;
                float quant = POWF(10.0f, static_cast<float>(exp) / 20.0f);
                for (std::size_t k = sfb[band]; k < end; ++k) {
                    tmp[k] = ch.coeffs[k] * quant;
                }
            }
            imdct_half(tmp, subframe_len, ch.out.data() + at, scale);
        }
    } else {
        for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
            auto& o = ch_[channel_indexes_[i]].out;
            std::fill(o.begin() + at, o.begin() + at + subframe_len, 0.0f);
        }
    }


    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        Channel& ch = ch_[channel_indexes_[i]];
        std::size_t winlen = ch.prev_block_len;
        std::size_t start = at - winlen / 2;
        if (subframe_len < winlen) {
            start += (winlen - subframe_len) / 2;
            winlen = subframe_len;
        }
        window_overlap(ch.out.data() + start, winlen, windows()[log2(winlen) - 7]);
        ch.prev_block_len = subframe_len;
    }

    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        Channel& ch = ch_[channel_indexes_[i]];
        if (ch.cur_subframe >= ch.num_subframes) {
            return "subframe overrun";
        }
        ch.cur_subframe += 1;
    }
    if (b.overread()) {
        return "subframe overread";
    }
    return nullptr;
}

void Decoder::inverse_channel_transform() {
    if (!transform_ || channels_for_cur_subframe_ != 2) {
        return;
    }
    const auto& sfb = sfb_offsets_[table_idx_];
    Channel& a = ch_[0];
    Channel& b = ch_[1];
    for (std::size_t band = 0; band < num_bands_; ++band) {
        std::size_t from = sfb[band], to = std::min(sfb[band + 1], subframe_len_);
        if (transform_band_[band]) {
            for (std::size_t y = from; y < to; ++y) {
                float l = a.coeffs[y], r = b.coeffs[y];
                a.coeffs[y] = l - r;
                b.coeffs[y] = l + r;
            }
        } else {
            for (std::size_t y = from; y < to; ++y) {
                a.coeffs[y] *= 181.0f / 128.0f;
                b.coeffs[y] *= 181.0f / 128.0f;
            }
        }
    }
}

}   
