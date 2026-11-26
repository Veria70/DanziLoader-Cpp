// SPDX-License-Identifier: GPL-3.0-or-later
// The bundled libopus 1.6.1 against the reference's Opus decoder
// (`opus-pure` 0.2.4, as `xapu` uses it): decodes the corpus
// `build/opus-oracle` made, the way `src/hle/apu.cpp` does (48 kHz,
// `opus_decode_float(packet, 5760, no FEC)`, `OPUS_RESET_STATE` on
// request), and compares frame counts, errors and every float sample's bit
// pattern with the oracle's results.
//
//   opus-check <corpus.bin> <oracle-results.bin>
#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

std::vector<std::uint8_t> read(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Reader {
    const std::vector<std::uint8_t>& d;
    std::size_t at = 0;
    std::uint32_t u32() {
        std::uint32_t v;
        std::memcpy(&v, d.data() + at, 4);
        at += 4;
        return v;
    }
};

}   

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: opus-check <corpus.bin> <oracle-results.bin>\n");
        return 2;
    }
    auto corpus = read(argv[1]);
    auto oracle = read(argv[2]);
    Reader c{corpus}, o{oracle};
    std::uint32_t streams = c.u32();
    std::size_t packets = 0, decoded = 0, errors = 0, differ = 0, samples = 0, sample_differ = 0, count_differ = 0, exact = 0;


    std::size_t buckets[2][5] = {}, exact2[2] = {};   
    double worst2[2] = {1e9, 1e9};
    const std::uint32_t clean = argc > 3 ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 2400;
    double max_abs = 0, worst_snr = 1e9;
    std::vector<float> pcm(5760 * 2);
    for (std::uint32_t s = 0; s < streams; ++s) {
        std::uint32_t channels = c.u32(), n = c.u32();
        int error = OPUS_OK;
        OpusDecoder* dec = opus_decoder_create(48000, static_cast<int>(channels), &error);
        for (std::uint32_t k = 0; k < n; ++k) {
            bool reset = corpus[c.at++] != 0;
            std::uint32_t len = c.u32();
            const std::uint8_t* packet = corpus.data() + c.at;
            c.at += len;
            if (reset) {
                opus_decoder_ctl(dec, OPUS_RESET_STATE);
            }
            ++packets;
            int got = opus_decode_float(dec, packet, static_cast<opus_int32>(len), pcm.data(), 5760, 0);
            int want = static_cast<int>(o.u32());
            bool same = (got < 0) == (want < 0) && (got < 0 || got == want);
            if (want < 0) {
                ++errors;
                count_differ += got >= 0;
            } else {
                ++decoded;
                std::size_t count = static_cast<std::size_t>(want) * channels;
                samples += count;
                std::size_t bad = 0;
                double peak = 0;
                for (std::size_t i = 0; i < count; ++i) {
                    std::uint32_t ref = o.u32(), mine = 0;
                    if (got == want) {
                        std::memcpy(&mine, &pcm[i], 4);
                    }
                    float r, m;
                    std::memcpy(&r, &ref, 4);
                    std::memcpy(&m, &mine, 4);
                    peak = std::max(peak, std::abs(double(r) - m));
                    bad += ref != mine;
                }
                if (got == want) {
                    max_abs = std::max(max_abs, peak);
                }
                if (got != want) {
                    ++count_differ;
                } else if (!bad) {
                    ++exact;
                    ++exact2[s >= clean];
                } else {

                    double dbfs = 20 * std::log10(peak);
                    worst_snr = std::max(-worst_snr, dbfs) * -1;
                    worst2[s >= clean] = std::min(worst2[s >= clean], -dbfs);
                    if (s < clean && dbfs > -80 && std::getenv("OPUS_CHECK_LIST")) {
                        std::printf("loud: stream %u packet %u %.1f dBFS\n", s, k, dbfs);
                    }
                    buckets[s >= clean][dbfs <= -140 ? 0 : dbfs <= -120 ? 1 : dbfs <= -100 ? 2 : dbfs <= -80 ? 3 : 4]++;
                }
                if (bad) {
                    same = false;
                    sample_differ += bad;
                }
            }
            if (!same) {
                if (++differ <= 0) {
                    std::printf("differs: stream %u packet %u (%u bytes): libopus %d, reference %d\n", s, k, len, got, want);
                }
            }
        }
        opus_decoder_destroy(dec);
    }
    std::printf("packets %zu (%zu decoded, %zu errors in the reference), samples %zu; packets that differ %zu, samples that differ %zu\n", packets, decoded,
                errors, samples, differ, sample_differ);
    for (int k = 0; k < 2; ++k) {
        std::printf("%s: %zu bit-identical; others by largest difference: <=-140 dBFS %zu, -140..-120 %zu, -120..-100 %zu, -100..-80 %zu, >-80 %zu; worst %.1f dBFS\n",
                    k ? "mutated and random streams" : "encoded streams", exact2[k], buckets[k][0], buckets[k][1], buckets[k][2], buckets[k][3], buckets[k][4], -worst2[k]);
    }
    std::printf("frame counts or error decisions that differ: %zu\n", count_differ);
    return count_differ ? 1 : 0;
}
