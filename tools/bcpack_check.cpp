// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the BCPack port: the reference's unit tests (`mod tests` of
// `victory-formats/src/bcpack.rs`; DanziLoader / victory-perf, see
// NOTICE.md), and the port's half of the differential test against the
// reference (`build/bcpack-oracle`, compared by tools/check_bcpack.py).
//
// Usage: bcpack-check                      the unit tests
//        bcpack-check run <corpus> <out>   decodes a corpus written by the oracle
#include "formats/bcpack.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace bp = danzi::formats::bcpack;

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

constexpr std::uint32_t kLow = 1u << 24;


std::vector<std::uint8_t> tables() {
    std::vector<std::uint8_t> t;
    for (std::size_t m = 0; m < bp::kModels; ++m) {
        const std::size_t g = m / 16;
        for (std::size_t b = 0; b < bp::kBuckets; ++b) {
            const std::uint16_t f = b < g + 2 ? static_cast<std::uint16_t>(1u << (15 - g)) : 0;
            t.push_back(static_cast<std::uint8_t>(f));
            t.push_back(static_cast<std::uint8_t>(f >> 8));
        }
    }
    for (unsigned v = 0; v < 256; ++v) t.push_back(static_cast<std::uint8_t>(v & 0x77));   
    for (unsigned v = 0; v < 64; ++v) t.push_back(static_cast<std::uint8_t>(v));           
    for (unsigned v = 0; v < 256; ++v) t.push_back(static_cast<std::uint8_t>(v));          
    for (unsigned v = 0; v < 64; ++v) t.push_back(static_cast<std::uint8_t>((v & 7) | (v >> 3) << 4));   
    for (unsigned v = 0; v < 64; ++v) t.push_back(static_cast<std::uint8_t>(v));           
    for (unsigned code = 0; code < 512; ++code) {

        const std::uint16_t ctrl = code == 0 ? 0x46 : 0;
        t.push_back(static_cast<std::uint8_t>(ctrl));
        t.push_back(static_cast<std::uint8_t>(ctrl >> 8));
    }
    t.resize(bp::kTablesSize, 0);
    return t;
}

std::unique_ptr<bp::Tables> parsed(const std::vector<std::uint8_t>& bytes) {
    std::string error;
    auto t = bp::parse(bytes, &error);
    if (t == nullptr) {
        std::fprintf(stderr, "tables do not parse: %s\n", error.c_str());
        std::exit(2);
    }
    return t;
}


std::vector<std::uint8_t> fragment(std::uint8_t width, const std::vector<std::uint8_t>& symbols) {
    const std::uint32_t f = 1u << (15 - width);
    std::uint32_t x = kLow;
    std::vector<std::uint8_t> bytes;
    for (auto it = symbols.rbegin(); it != symbols.rend(); ++it) {
        const std::uint8_t s = *it;
        const std::uint32_t freq = s == 0 ? f - 1 : f;
        const std::uint32_t start = s == 0 ? 1 : std::uint32_t{s} * f;
        while (std::uint64_t{x} >= std::uint64_t{freq} << 16) {
            bytes.push_back(static_cast<std::uint8_t>(x));
            x >>= 8;
        }
        x = ((x / freq) << 16) + x % freq + start;
    }
    for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<std::uint8_t>(x >> (8 * k)));
    std::vector<std::uint8_t> out = {0};
    if (bytes.size() > 256) {
        out[0] |= 0x20;
        const std::size_t n = bytes.size() - 257;
        out.push_back(static_cast<std::uint8_t>(n));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
    } else {
        out.push_back(static_cast<std::uint8_t>(bytes.size() - 1));
    }
    out.insert(out.end(), bytes.rbegin(), bytes.rend());
    return out;
}

std::vector<std::uint8_t> concat(const std::vector<std::vector<std::uint8_t>>& records) {
    std::vector<std::uint8_t> all;
    for (const auto& r : records) all.insert(all.end(), r.begin(), r.end());
    return all;
}


std::vector<std::uint8_t> stream(const bp::Tables& t, const std::vector<std::uint8_t>& widths, std::size_t per,
                                 const std::vector<std::vector<std::uint8_t>>& records) {
    const std::size_t stride = widths.size() * per;
    std::vector<std::deque<std::uint8_t>> queues;
    for (std::size_t j = 0; j < widths.size(); ++j) {
        std::vector<std::uint8_t> syms;
        for (const auto& r : records) syms.insert(syms.end(), r.begin() + j * per, r.begin() + j * per + per);
        auto bytes = fragment(widths[j], syms);
        bytes.insert(bytes.end(), {0, 1, 0, 0});
        queues.emplace_back(bytes.begin(), bytes.end());
    }
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> check(records.size() * stride);
    const std::function<int(std::size_t)> fetch = [&](std::size_t j) -> int {
        if (queues[j].empty()) {
            std::fprintf(stderr, "a parked lane asked for input\n");
            std::exit(2);
        }
        const std::uint8_t b = queues[j].front();
        queues[j].pop_front();
        out.push_back(b);
        return b;
    };
    CHECK(bp::detail::lanes(t, widths, per, records.size(), fetch, check));
    CHECK(check == concat(records));
    return out;
}

const std::vector<std::uint8_t> kBc4Lanes = {7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};

std::uint8_t zigzag(int v) { return static_cast<std::uint8_t>(v >= 0 ? 2 * v : -2 * v - 1); }


struct Bits {
    std::uint8_t v[16] = {};
    unsigned at = 0;
    void put(std::uint64_t value, unsigned n) {
        for (unsigned k = 0; k < n; ++k, ++at) {
            if (at < 128 && (value >> k & 1) != 0) v[at / 8] |= static_cast<std::uint8_t>(1u << (at % 8));
        }
    }
};

constexpr std::uint64_t kBc4Code[8] = {0, 2, 3, 4, 5, 6, 7, 1};

void the_shipped_tables_parse() {
    std::string error;
    CHECK(bp::builtin(&error) != nullptr);
}

void bc4_blocks_with_and_without_references() {
    const auto t = parsed(tables());

    std::vector<std::uint8_t> b0 = {0, 0, zigzag(10), zigzag(-3)};
    for (unsigned k = 0; k < 8; ++k) b0.push_back(static_cast<std::uint8_t>(((2 * k) & 7) | ((2 * k + 1) & 7) << 3));

    std::vector<std::uint8_t> b1 = {0, 1, zigzag(1), zigzag(1)};
    for (unsigned k = 0; k < 8; ++k) b1.push_back(1 | 1 << 3);
    const auto src = stream(*t, kBc4Lanes, 1, {b0, b1});
    std::uint8_t dst[16] = {};
    CHECK(!bp::decode(4, src, dst, *t));
    const auto block = [](std::uint8_t e0, std::uint8_t e1, auto ix) {
        std::uint64_t bits = 0;
        for (unsigned p = 0; p < 16; ++p) bits |= kBc4Code[ix(p)] << (16 + 3 * p);
        const std::uint64_t v = std::uint64_t{e0} | std::uint64_t{e1} << 8 | bits;
        std::vector<std::uint8_t> out;
        for (int k = 0; k < 8; ++k) out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        return out;
    };
    CHECK(std::vector<std::uint8_t>(dst, dst + 8) == block(138, 125, [](unsigned p) { return p & 7; }));
    CHECK(std::vector<std::uint8_t>(dst + 8, dst + 16) == block(139, 126, [](unsigned p) { return (p + 1) & 7; }));
}

void lanes_take_bytes_in_schedule_order_and_long_fragments() {
    const auto t = parsed(tables());

    std::vector<std::vector<std::uint8_t>> records;
    for (std::uint32_t i = 0; i < 400; ++i) {
        std::vector<std::uint8_t> r = {0, 0, static_cast<std::uint8_t>(i * 7), static_cast<std::uint8_t>(i * 13)};
        for (std::uint32_t k = 0; k < 8; ++k) r.push_back(static_cast<std::uint8_t>((i + k) * 5 % 64));
        records.push_back(r);
    }
    const auto src = stream(*t, kBc4Lanes, 1, records);
    std::vector<std::uint8_t> got(records.size() * 12);
    std::size_t next = 0;
    CHECK(bp::detail::lanes(*t, kBc4Lanes, 1, records.size(),
                            [&](std::size_t) { return next < src.size() ? int{src[next++]} : -1; }, got));
    CHECK(got == concat(records));
}

void bc7_mode6_block() {
    const auto t = parsed(tables());

    std::vector<std::uint8_t> s(32, 0);
    s[4] = zigzag(0x55);   
    s[5] = zigzag(-1);     
    s[6] = zigzag(3);
    s[10] = zigzag(0x7f >> 1);
    const auto src = stream(*t, {3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4}, 2, {s});
    std::uint8_t dst[16] = {};
    CHECK(!bp::decode(7, src, dst, *t));
    Bits w;
    w.put(1 << 6, 7);
    for (const auto& [v, n] : {std::pair<std::uint64_t, unsigned>{0x2a, 7}, {0x7f, 7}, {3, 7}, {0, 7}, {0, 7},
                               {0, 7}, {0x3f, 7}, {0, 7}}) {
        w.put(v, n);
    }
    w.put(1, 1);
    w.put(1, 1);
    CHECK(std::equal(dst, dst + 16, w.v));
}

void malformed_input_is_an_error() {
    const auto t = parsed(tables());
    std::vector<std::vector<std::uint8_t>> records;
    for (std::uint8_t i = 0; i < 64; ++i) records.push_back({0, 0, i, i, 1, 2, 3, 4, 5, 6, 7, 8});
    const auto src = stream(*t, kBc4Lanes, 1, records);
    std::vector<std::uint8_t> dst(64 * 8);
    CHECK(!bp::decode(4, src, dst, *t));
    const std::span<const std::uint8_t> all(src);

    CHECK(bp::decode(4, all.first(src.size() / 2), dst, *t).has_value());


    std::vector<std::uint8_t> headers(8, 0x30);
    headers.insert(headers.end(), 8, 0xff);
    CHECK(bp::decode(4, headers, std::span(dst).first(8), *t).has_value());
    CHECK(bp::decode(4, {}, dst, *t).has_value());

    CHECK(bp::decode(4, src, std::span(dst).first(12), *t).has_value());
    CHECK(bp::decode(4, src, {}, *t).has_value());
    std::vector<std::uint8_t> more(65 * 8);
    CHECK(bp::decode(4, src, more, *t).has_value());

    CHECK(bp::decode(2, src, dst, *t) == std::optional<std::string>("BCPack: unsupported BC format"));
    std::string error;
    CHECK(bp::parse(std::vector<std::uint8_t>(16, 0), &error) == nullptr);
    CHECK(error == "BCPack: table file has the wrong size");
    auto no_tiling = tables();
    no_tiling[0] = 1;
    CHECK(bp::parse(no_tiling, &error) == nullptr);
    CHECK(error == "BCPack: a model does not tile 65536");
}

void noise_never_panics() {
    const auto t = parsed(tables());
    std::uint64_t seed = 0x2545'f491'4f6c'dd1dull;
    for (std::size_t round = 0; round < 300; ++round) {
        const std::size_t len = round * 7 % 900;
        std::vector<std::uint8_t> src(len);
        for (auto& b : src) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            b = static_cast<std::uint8_t>(seed);
        }
        for (const std::uint8_t bc : std::initializer_list<std::uint8_t>{1, 3, 4, 5, 6, 7}) {
            std::vector<std::uint8_t> dst(16 * (1 + round % 40));
            (void)bp::decode(bc, src, dst, *t);
        }
    }
}


void bucket_lookup_matches_a_scan_of_the_starts() {
    auto narrow = tables();
    for (std::size_t m = 0; m < bp::kModels; ++m) {

        const std::size_t used = m / 16 + 2;
        std::uint32_t rest = 1u << 16;
        for (std::size_t b = 1; b < used; ++b) rest -= 1u << (b - 1);
        for (std::size_t b = 0; b < used; ++b) {
            const std::uint32_t f = b == 0 ? rest : 1;
            narrow[(m * bp::kBuckets + b) * 2] = static_cast<std::uint8_t>(f);
            narrow[(m * bp::kBuckets + b) * 2 + 1] = static_cast<std::uint8_t>(f >> 8);
        }
    }
    const bp::Tables* all[3] = {bp::builtin(nullptr), nullptr, nullptr};
    const auto ours = parsed(tables());
    const auto thin = parsed(narrow);
    all[1] = ours.get();
    all[2] = thin.get();
    for (const bp::Tables* t : all) {
        for (std::size_t model = 0; model < bp::kModels; ++model) {
            const auto& start = t->start[model];
            for (std::uint32_t slot = 0; slot < 1u << 16; ++slot) {
                std::size_t scan = 0;
                for (std::size_t b = 1; b < bp::kBuckets + 1; ++b) scan += start[b] <= slot;
                if (bp::detail::bucket_of(*t, static_cast<std::uint8_t>(model), slot) != scan) {
                    std::fprintf(stderr, "bucket_of: model %zu slot %u\n", model, slot);
                    ++failures;
                    return;
                }
            }
        }
    }
}


std::pair<int, std::uint32_t> reference_decode(const bp::Tables& t, std::uint8_t model, std::uint32_t x) {
    const auto& freq = t.freq[model];
    const auto& start = t.start[model];
    const std::uint32_t slot = x & 0xffff;
    std::size_t b = 0;
    for (std::size_t k = 1; k < bp::kBuckets; ++k) b += start[k] <= slot;
    const auto sym = static_cast<std::uint8_t>(b == 0 ? 0 : b == 1 ? 1 : (1u << (b - 1)) + (slot - start[b]) / freq[b]);
    std::uint32_t f, c;
    if (sym == 0) {
        f = freq[0] - 1;
        c = 1;
    } else {
        std::size_t k = 0;
        while ((sym >> k) != 0) ++k;
        f = freq[k];
        c = ((std::uint32_t{sym} - (1u << (k - 1))) * f + start[k]) & 0xffff;
    }
    return {sym, (x >> 16) * f - c + slot};
}


void payload_step_matches_the_reference_decode() {
    const auto ours = parsed(tables());
    for (const bp::Tables* t : {bp::builtin(nullptr), static_cast<const bp::Tables*>(ours.get())}) {
        for (std::size_t model = 0; model < bp::kModels; ++model) {
            for (const std::uint32_t high : {0x100u, 0x1234u, 0xffffu}) {
                for (std::uint32_t slot = 0; slot < 1u << 16; ++slot) {
                    const std::uint32_t x = high << 16 | slot;
                    if (x <= kLow) continue;
                    const auto got = bp::detail::payload_step(*t, static_cast<std::uint8_t>(model), x);
                    if (got != reference_decode(*t, static_cast<std::uint8_t>(model), x)) {
                        std::fprintf(stderr, "payload step: model %zu x %#x\n", model, x);
                        ++failures;
                        return;
                    }
                }
            }
        }
    }
}


std::vector<std::vector<std::uint8_t>> random_records(const std::vector<std::uint8_t>& widths, std::size_t per,
                                                      std::size_t blocks, bool bc7) {
    std::uint64_t seed = 0x9e37'79b9'7f4a'7c15ull;
    std::vector<std::vector<std::uint8_t>> out;
    for (std::size_t i = 0; i < blocks; ++i) {
        std::vector<std::uint8_t> r(widths.size() * per);
        for (std::size_t k = 0; k < r.size(); ++k) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            const std::uint32_t alphabet = 1u << (widths[k / per] + 1);
            r[k] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(seed) % alphabet);
        }
        if (bc7) {
            r[30] = 0;
            r[31] = 0;
        }
        out.push_back(r);
    }
    return out;
}


void every_layout_round_trips_through_the_lanes() {
    const auto t = parsed(tables());
    const std::pair<std::vector<std::uint8_t>, std::size_t> layouts[6] = {
        {{7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7}, 1},
        {{3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7}, 2},
        {kBc4Lanes, 1},
        {{3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5}, 2},
        {{3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4}, 2},
        {{3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4}, 2},
    };
    for (const auto& [widths, per] : layouts) {
        for (const std::size_t blocks : {1, 2, 17, 300, 1000}) {
            const auto records = random_records(widths, per, blocks, false);
            const auto src = stream(*t, widths, per, records);
            std::vector<std::uint8_t> got(concat(records).size());
            std::size_t next = 0;
            CHECK(bp::detail::lanes(*t, widths, per, blocks,
                                    [&](std::size_t) { return next < src.size() ? int{src[next++]} : -1; }, got));
            CHECK(got == concat(records));
        }
    }
}


void bc7_dictionary_indices_line_up() {
    const bp::Tables* t = bp::builtin(nullptr);
    for (const auto& [ep, ix] : t->bc7) {
        bool empty = true;
        for (const auto v : ix.ix[1]) empty = empty && v == 0;
        const bool anchor = ix.ix[0][0] >= 0 && ix.ix[0][0] < (1 << (ix.bits[0] - 1));
        CHECK((ix.bits[1] != 0 || empty) && anchor);
    }
}


struct In {
    std::vector<std::uint8_t> data;
    std::size_t at = 0;
    std::uint8_t u8() { return data.at(at++); }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= std::uint32_t{u8()} << (8 * k);
        return v;
    }
    std::vector<std::uint8_t> bytes(std::size_t n) {
        if (n > data.size() - at) throw std::out_of_range("corpus");
        std::vector<std::uint8_t> v(data.begin() + at, data.begin() + at + n);
        at += n;
        return v;
    }
};

void put_status(std::vector<std::uint8_t>& out, const std::optional<std::string>& error) {
    out.push_back(error ? 1 : 0);
    const std::string text = error.value_or("");
    out.push_back(static_cast<std::uint8_t>(text.size()));
    out.push_back(static_cast<std::uint8_t>(text.size() >> 8));
    out.insert(out.end(), text.begin(), text.end());
}

int run(const char* corpus_path, const char* out_path) {
    std::ifstream f(corpus_path, std::ios::binary);
    In in{std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {})};
    std::vector<std::unique_ptr<bp::Tables>> owned;
    std::vector<const bp::Tables*> sets;
    for (std::uint32_t n = in.u32(), k = 0; k < n; ++k) {
        if (in.u8() == 0) {
            sets.push_back(bp::builtin(nullptr));
        } else {
            const auto bytes = in.bytes(in.u32());
            owned.push_back(bp::parse(bytes, nullptr));
            sets.push_back(owned.back().get());
        }
    }
    std::vector<std::uint8_t> out;
    std::size_t cases = 0, ok = 0;
    for (std::uint32_t n = in.u32(), k = 0; k < n; ++k, ++cases) {
        const std::uint8_t kind = in.u8();
        out.push_back(kind);
        if (kind == 0) {
            const std::uint32_t table = in.u32();
            const std::uint8_t bc = in.u8();
            const std::uint8_t fill = in.u8();
            const std::uint32_t dst_len = in.u32();
            const auto src = in.bytes(in.u32());
            std::vector<std::uint8_t> dst(dst_len, fill);
            const auto error = bp::decode(bc, src, dst, *sets.at(table));
            ok += !error;
            put_status(out, error);
            for (int b = 0; b < 4; ++b) out.push_back(static_cast<std::uint8_t>(dst_len >> (8 * b)));
            out.insert(out.end(), dst.begin(), dst.end());
        } else {
            const auto bytes = in.bytes(in.u32());
            std::string error;
            const auto t = bp::parse(bytes, &error);
            put_status(out, t ? std::nullopt : std::optional<std::string>(error));
        }
    }
    std::ofstream o(out_path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    std::printf("bcpack-check: %zu cases, %zu decoded\n", cases, ok);
    return o.good() ? 0 : 1;
}

}   

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "run") {
        return run(argv[2], argv[3]);
    }
    the_shipped_tables_parse();
    bc4_blocks_with_and_without_references();
    lanes_take_bytes_in_schedule_order_and_long_fragments();
    bc7_mode6_block();
    malformed_input_is_an_error();
    noise_never_panics();
    bucket_lookup_matches_a_scan_of_the_starts();
    payload_step_matches_the_reference_decode();
    every_layout_round_trips_through_the_lanes();
    bc7_dictionary_indices_line_up();
    std::printf("bcpack-check: %s (%d failures)\n", failures == 0 ? "all tests pass" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
