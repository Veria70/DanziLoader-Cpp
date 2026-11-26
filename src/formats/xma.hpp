// SPDX-License-Identifier: GPL-3.0-or-later
// XMA2, the console's hardware audio codec (`victory-formats/src/xma.rs`;
// DanziLoader / victory-perf, see NOTICE.md): mono or stereo WMA Pro frames
// of 512 samples per channel, packed into 2048-byte packets.
//
// A packet starts with a 32-bit header (MSB first): the number of frames
// that begin in it (6 bits), the number of bits that finish the previous
// frame before the first new one (15), metadata (3) and how many packets of
// other streams follow before this stream's next packet (8). Every frame
// starts with its length in bits (15) and ends with a "more frames" bit.
//
// The frame decoder follows FFmpeg's `wmaprodec.c` (LGPL-2.1-or-later, used
// under GPL-3.0-or-later) restricted to what XMA needs: at most two
// channels, four subframe sizes (512 down to 128), no LFE channel.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace danzi::formats::xma {

constexpr std::size_t PACKET_BYTES = 2048;

constexpr std::size_t FRAME_SAMPLES = 512;

constexpr std::size_t SUBFRAME_SAMPLES = 128;
constexpr std::size_t MAX_BANDS = 29;
constexpr std::size_t MAX_SUBFRAMES = 32;
constexpr std::size_t BLOCK_SIZES = 3;


struct Packet {
    std::uint32_t frames;
    std::uint32_t first_frame_bit;
    std::uint32_t skip;
};
std::optional<Packet> packet(std::span<const std::uint8_t> data, std::size_t index);


struct Frame {

    std::vector<std::uint8_t> data;
    std::size_t bits;


    std::size_t start;
    std::optional<std::size_t> next;
};


std::optional<Frame> read_frame(std::span<const std::uint8_t> data, std::size_t start);

struct Bits;


class Decoder {
public:


    Decoder(std::size_t channels, std::uint32_t sample_rate);
    std::size_t channels() const { return channels_; }

    void reset();


    bool decode(std::span<const std::uint8_t> data, std::size_t bits, std::vector<std::int16_t>& out, const char** error = nullptr);


    void flush(std::vector<std::int16_t>& out);

private:
    struct Channel {
        std::size_t prev_block_len = FRAME_SAMPLES;
        bool transmit_coefs = false;
        std::size_t num_subframes = 0;
        std::array<std::size_t, MAX_SUBFRAMES> subframe_len{};
        std::size_t cur_subframe = 0;
        std::size_t decoded_samples = 0;
        std::int32_t quant_step = 0;
        bool reuse_sf = false;
        std::int32_t scale_factor_step = 1;
        std::int32_t max_scale_factor = 0;
        std::array<std::array<std::int32_t, MAX_BANDS>, 2> saved_scale_factors{};
        std::size_t scale_factor_idx = 0;

        std::size_t scale_factors = 0;
        std::size_t table_idx = 0;
        std::size_t num_vec_coeffs = 0;
        std::array<float, FRAME_SAMPLES> coeffs{};

        std::array<float, FRAME_SAMPLES + FRAME_SAMPLES / 2> out{};
    };


    const char* frame(Bits& b, std::size_t len);
    const char* subframe_length(Bits& b, std::size_t offset, std::size_t& len);
    const char* tile_header(Bits& b);
    const char* channel_transform(Bits& b);
    const char* coefficients(Bits& b, std::size_t c);
    const char* scale_factors(Bits& b);
    const char* subframe(Bits& b);
    void inverse_channel_transform();

    std::size_t channels_;
    std::array<std::size_t, BLOCK_SIZES> num_sfb_{};
    std::array<std::array<std::size_t, MAX_BANDS + 1>, BLOCK_SIZES> sfb_offsets_{};
    std::array<std::array<std::array<std::size_t, MAX_BANDS>, BLOCK_SIZES>, BLOCK_SIZES> sf_offsets_{};
    std::array<Channel, 2> ch_{};

    std::size_t subframe_len_ = FRAME_SAMPLES;
    std::size_t channels_for_cur_subframe_ = 0;
    std::array<std::size_t, 2> channel_indexes_{0, 1};
    std::size_t num_bands_ = 0;
    std::size_t table_idx_ = 0;
    std::uint32_t esc_len_ = 0;
    bool transmit_num_vec_coeffs_ = false;
    bool transform_ = false;
    std::array<bool, MAX_BANDS> transform_band_{};
    bool parsed_all_subframes_ = false;
};

}   
