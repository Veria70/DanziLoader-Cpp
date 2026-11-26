// SPDX-License-Identifier: GPL-3.0-or-later
// Port of parts of crates/victory-shader (DanziLoader / victory-perf, see
// NOTICE.md). See native.hpp.
#include "native.hpp"

#include "error.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace danzi::shader {

namespace {

std::optional<std::uint32_t> u32_at(std::span<const std::uint8_t> d, std::size_t at) {
    if (at > d.size() || d.size() - at < 4) {
        return std::nullopt;
    }
    std::uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

std::optional<std::uint64_t> u64_at(std::span<const std::uint8_t> d, std::size_t at) {
    if (at > d.size() || d.size() - at < 8) {
        return std::nullopt;
    }
    std::uint64_t v;
    std::memcpy(&v, d.data() + at, 8);
    return v;
}

std::optional<std::span<const std::uint8_t>> bytes(std::span<const std::uint8_t> d, std::uint64_t offset, std::uint64_t len) {
    if (offset > d.size() || len > d.size() - offset) {
        return std::nullopt;
    }
    return d.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(len));
}


struct BitReader {
    std::span<const std::uint8_t> in;
    std::size_t at = 0;


    std::uint64_t bits = 0;
    std::uint32_t count = 0;
    bool fail = false;
    std::uint32_t need(std::uint32_t n) {
        while (count < n) {
            if (at >= in.size()) {
                fail = true;
                return 0;
            }
            bits |= std::uint64_t{in[at++]} << count;
            count += 8;
        }
        auto v = static_cast<std::uint32_t>(bits & ((std::uint64_t{1} << n) - 1));
        bits >>= n;
        count -= n;
        return v;
    }


    std::uint32_t fill(std::uint32_t n) {
        if (count < n) {
            if (at + 8 <= in.size()) {
                std::uint64_t v;
                std::memcpy(&v, in.data() + at, 8);
                bits |= v << count;
                std::uint32_t take = (63 - count) >> 3;
                at += take;
                count += take * 8;
            } else {
                while (count < n && at < in.size()) {
                    bits |= std::uint64_t{in[at++]} << count;
                    count += 8;
                }
            }
        }
        return count;
    }

    void align() {
        at -= count / 8;
        bits = 0;
        count = 0;
    }
};


struct Huffman {
    std::array<std::uint16_t, 16> counts{};
    std::vector<std::uint16_t> symbols;


    static constexpr std::uint32_t FAST = 10;
    std::array<std::uint16_t, 1u << FAST> fast{};

    bool make(const std::uint8_t* lengths, std::size_t n) {
        counts.fill(0);
        for (std::size_t i = 0; i < n; ++i) {
            ++counts[lengths[i]];
        }
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= counts[len];
            if (left < 0) {
                return false;
            }
        }
        std::array<std::uint16_t, 16> offs{};
        for (int len = 1; len < 15; ++len) {
            offs[len + 1] = static_cast<std::uint16_t>(offs[len] + counts[len]);
        }
        symbols.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            if (lengths[i] != 0) {
                symbols[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
            }
        }


        fast.fill(0);
        std::uint32_t code = 0, index = 0;
        for (std::uint32_t len = 1; len <= FAST; ++len) {
            for (std::uint32_t k = 0; k < counts[len]; ++k, ++index, ++code) {
                std::uint32_t rev = 0;
                for (std::uint32_t b = 0; b < len; ++b) {
                    rev |= (code >> b & 1) << (len - 1 - b);
                }
                auto entry = static_cast<std::uint16_t>(symbols[index] << 4 | len);
                for (std::uint32_t at = rev; at < (1u << FAST); at += 1u << len) {
                    fast[at] = entry;
                }
            }
            code <<= 1;
        }
        return true;
    }
    int decode(BitReader& r) const {
        if (r.fill(FAST) >= FAST) {
            std::uint16_t e = fast[r.bits & ((1u << FAST) - 1)];
            if (e != 0) {
                std::uint32_t len = e & 15;
                r.bits >>= len;
                r.count -= len;
                return e >> 4;
            }
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(r.need(1));
            if (r.fail) {
                return -1;
            }
            int count = counts[len];
            if (code - count < first) {
                return symbols[static_cast<std::size_t>(index + (code - first))];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr std::uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t DIST_BASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(BitReader& r, std::vector<std::uint8_t>& out, std::size_t most, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = lit.decode(r);
        if (sym < 0) {
            return false;
        }
        if (sym < 256) {
            if (out.size() >= most) {
                return false;
            }
            out.push_back(static_cast<std::uint8_t>(sym));
            continue;
        }
        if (sym == 256) {
            return true;
        }
        sym -= 257;
        if (sym >= 29) {
            return false;
        }
        std::size_t len = LEN_BASE[sym] + r.need(LEN_EXTRA[sym]);
        int d = dist.decode(r);
        if (d < 0 || d >= 30 || r.fail) {
            return false;
        }
        std::size_t back = DIST_BASE[d] + r.need(DIST_EXTRA[d]);
        if (r.fail || back > out.size() || out.size() + len > most) {
            return false;
        }
        std::size_t from = out.size() - back, to = out.size();
        out.resize(to + len);
        for (std::size_t k = 0; k < len; ++k) {
            out[to + k] = out[from + k];
        }
    }
}

bool inflate_raw(BitReader& r, std::vector<std::uint8_t>& out, std::size_t most) {
    static const auto fixed = [] {
        std::array<std::uint8_t, 288> lengths{};
        std::fill(lengths.begin(), lengths.begin() + 144, std::uint8_t{8});
        std::fill(lengths.begin() + 144, lengths.begin() + 256, std::uint8_t{9});
        std::fill(lengths.begin() + 256, lengths.begin() + 280, std::uint8_t{7});
        std::fill(lengths.begin() + 280, lengths.end(), std::uint8_t{8});
        std::pair<Huffman, Huffman> h;
        h.first.make(lengths.data(), 288);
        std::array<std::uint8_t, 30> d;
        d.fill(5);
        h.second.make(d.data(), 30);
        return h;
    }();
    for (;;) {
        std::uint32_t last = r.need(1);
        std::uint32_t type = r.need(2);
        if (r.fail) {
            return false;
        }
        if (type == 0) {
            r.align();
            if (r.at + 4 > r.in.size()) {
                return false;
            }
            std::uint32_t len = r.in[r.at] | r.in[r.at + 1] << 8;
            std::uint32_t nlen = r.in[r.at + 2] | r.in[r.at + 3] << 8;
            r.at += 4;
            if (len != (~nlen & 0xFFFF) || r.at + len > r.in.size() || out.size() + len > most) {
                return false;
            }
            out.insert(out.end(), r.in.begin() + static_cast<std::ptrdiff_t>(r.at), r.in.begin() + static_cast<std::ptrdiff_t>(r.at + len));
            r.at += len;
        } else if (type == 1) {
            if (!codes(r, out, most, fixed.first, fixed.second)) {
                return false;
            }
        } else if (type == 2) {
            std::uint32_t nlen = r.need(5) + 257, ndist = r.need(5) + 1, ncode = r.need(4) + 4;
            if (r.fail || nlen > 286 || ndist > 30) {
                return false;
            }
            static constexpr std::uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::array<std::uint8_t, 320> lengths{};
            for (std::uint32_t i = 0; i < ncode; ++i) {
                lengths[ORDER[i]] = static_cast<std::uint8_t>(r.need(3));
            }
            Huffman lencode;
            if (r.fail || !lencode.make(lengths.data(), 19)) {
                return false;
            }
            std::uint32_t index = 0;
            while (index < nlen + ndist) {
                int sym = lencode.decode(r);
                if (sym < 0) {
                    return false;
                }
                if (sym < 16) {
                    lengths[index++] = static_cast<std::uint8_t>(sym);
                    continue;
                }
                std::uint8_t value = 0;
                std::uint32_t repeat;
                if (sym == 16) {
                    if (index == 0) {
                        return false;
                    }
                    value = lengths[index - 1];
                    repeat = 3 + r.need(2);
                } else if (sym == 17) {
                    repeat = 3 + r.need(3);
                } else {
                    repeat = 11 + r.need(7);
                }
                if (r.fail || index + repeat > nlen + ndist) {
                    return false;
                }
                while (repeat--) {
                    lengths[index++] = value;
                }
            }
            if (lengths[256] == 0) {
                return false;
            }
            Huffman lit, dist;
            if (!lit.make(lengths.data(), nlen) || !dist.make(lengths.data() + nlen, ndist)) {
                return false;
            }
            if (!codes(r, out, most, lit, dist)) {
                return false;
            }
        } else {
            return false;
        }
        if (last) {
            return true;
        }
    }
}


constexpr std::size_t XDAT_OFFSET = 0x18;
constexpr std::size_t MAX_XDAT_ENTRIES = 64;
constexpr std::uint32_t KIND_NATIVE = 0x0d;
constexpr std::uint32_t XBCH_VERSION = 3;
constexpr std::size_t XBCH_HEADER = 0x18;
constexpr std::uint32_t FSSM_VERSION = 0x101;
constexpr std::size_t FSSM_HEADER = 0x10;
constexpr std::uint64_t GRAPHICS_SLOT_COUNT = 6;
constexpr std::pair<HwStage, std::size_t> SLOTS[8] = {{HwStage::Ls, 0x08}, {HwStage::Hs, 0x10}, {HwStage::Es, 0x18}, {HwStage::Gs, 0x20},
                                                      {HwStage::Vs, 0x28}, {HwStage::Ps, 0x30}, {HwStage::Ms, 0x48}, {HwStage::Cs, 0x60}};
constexpr std::uint64_t ABSENT = 0xDEADBEEFDEADBEEFull;
constexpr std::uint64_t RECORD_SIZE = 0x54;
constexpr std::size_t RECORD_FLAGS = 0x14;
constexpr std::uint32_t FLAG_WAVE32 = 1u << 5;
constexpr std::size_t RECORD_REGS = 0x50;
constexpr std::size_t RECORD_LAYOUT = 0x28;
constexpr std::size_t RECORD_USED = 0x30;
constexpr std::size_t RECORD_COUNTS = 0x38;
constexpr std::uint64_t ENTRY_SIZE = 20;
constexpr std::uint32_t MAX_SEMANTICS = 32;
constexpr std::size_t PS_INPUTS = 0x24, PS_INPUT_COUNT = 0x124, PS_INPUT_ADDR = 0x134, PS_DB_SHADER_CONTROL = 0x138;
constexpr std::size_t PS_PATCH_SIZE = 0x148, PS_PATCHES = 0x14c;
constexpr std::size_t PATCH_SLOT_DWORDS = 10, PATCH_FULL = 9, PATCH_BYTES = 4 + 10 * PATCH_SLOT_DWORDS * 4, MRTS = 8;
constexpr std::size_t VS_INPUTS = 0x24, VS_INPUT_COUNT = 0xc4, VS_OUTPUTS = 0xc8, VS_OUTPUT_COUNT = 0x188;
constexpr std::size_t CS_RSRC2 = 0x24, CS_THREADS = 0x28;
constexpr std::size_t MS_CLIP_CNTL = 0x194, MS_INFO = 0x448;
constexpr std::uint64_t MAX_UNPACKED = 16u << 20;

std::uint32_t u32_or(std::span<const std::uint8_t> d, std::size_t at) {
    auto v = u32_at(d, at);
    if (!v) {
        malformed("truncated");
    }
    return *v;
}

std::uint64_t u64_or(std::span<const std::uint8_t> d, std::size_t at) {
    auto v = u64_at(d, at);
    if (!v) {
        malformed("truncated");
    }
    return *v;
}


std::span<const std::uint8_t> semantics(std::span<const std::uint8_t> data, std::size_t at, std::size_t size, std::size_t count_at) {
    std::uint32_t count = u32_or(data, count_at);
    if (count > MAX_SEMANTICS) {
        malformed("implausible semantic count");
    }
    std::size_t len = size * count;
    if (at > data.size() || len > data.size() - at) {
        malformed("stage data out of bounds");
    }
    return data.subspan(at, len);
}

StageIo ps_io(std::span<const std::uint8_t> data, std::size_t code_len) {
    StageIo io;
    io.kind = StageIo::Ps;
    auto entries = semantics(data, PS_INPUTS, 8, PS_INPUT_COUNT);
    for (std::size_t k = 0; k + 8 <= entries.size(); k += 8) {
        const std::uint8_t* e = entries.data() + k;
        io.ps_inputs.push_back(PsInput{Usage{e[0]}, e[1], static_cast<std::uint8_t>(e[2] & 0xf), e[3], (e[7] & 1) != 0, (e[7] & 8) != 0,
                                       (e[7] & 0x10) != 0, e[5]});
    }
    std::size_t size = u32_or(data, PS_PATCH_SIZE);
    if (size > PATCH_SLOT_DWORDS) {
        malformed("implausible export patch size");
    }
    for (std::size_t mrt = 0; mrt < MRTS; ++mrt) {
        std::size_t at = PS_PATCHES + mrt * PATCH_BYTES;
        std::uint32_t offset = u32_or(data, at);
        if (offset == UINT32_MAX) {
            continue;
        }
        if ((std::uint64_t{offset} + size) * 4 > code_len) {
            malformed("export patch outside the code");
        }
        std::size_t full = at + 4 + PATCH_FULL * PATCH_SLOT_DWORDS * 4;
        ExportPatch patch{static_cast<std::uint8_t>(mrt), offset, {}};
        for (std::size_t i = 0; i < size; ++i) {
            patch.code.push_back(u32_or(data, full + 4 * i));
        }
        io.patches.push_back(std::move(patch));
    }
    io.input_addr = u32_or(data, PS_INPUT_ADDR);
    io.db_shader_control = u32_or(data, PS_DB_SHADER_CONTROL);
    return io;
}

StageIo vs_io(std::span<const std::uint8_t> data) {
    StageIo io;
    io.kind = StageIo::Vs;
    auto inputs = semantics(data, VS_INPUTS, 5, VS_INPUT_COUNT);
    for (std::size_t k = 0; k + 5 <= inputs.size(); k += 5) {
        const std::uint8_t* e = inputs.data() + k;
        io.vs_inputs.push_back(VsInput{Usage{e[0]}, e[1], static_cast<std::uint8_t>(e[2] & 0xf), e[3]});
    }
    auto outputs = semantics(data, VS_OUTPUTS, 6, VS_OUTPUT_COUNT);
    for (std::size_t k = 0; k + 6 <= outputs.size(); k += 6) {
        const std::uint8_t* e = outputs.data() + k;
        io.vs_outputs.push_back(VsOutput{Usage{e[0]}, e[1], e[2], (e[5] & 1) != 0, (e[5] & 2) != 0});
    }
    return io;
}

StageIo ms_io(std::span<const std::uint8_t> data, std::uint32_t lds_bytes) {
    StageIo vs = vs_io(data);
    if (MS_INFO > data.size() || data.size() - MS_INFO < 8) {
        malformed("stage data out of bounds");
    }
    const std::uint8_t* info = data.data() + MS_INFO;
    auto half = [&](std::size_t at) { return std::uint32_t{info[at]} | std::uint32_t{info[at + 1]} << 8; };
    StageIo io;
    io.kind = StageIo::Ms;
    io.ms.outputs = std::move(vs.vs_outputs);
    io.ms.clip_cntl = u32_or(data, MS_CLIP_CNTL);
    io.ms.threads = half(0);
    io.ms.max_vertices = half(2);
    io.ms.max_primitives = half(4);
    io.ms.topology = info[7];
    io.ms.lds_bytes = lds_bytes;
    return io;
}

std::vector<std::uint8_t> unpack(std::span<const std::uint8_t> blob) {
    if (blob.size() < 4 || std::memcmp(blob.data(), "XBCH", 4) != 0) {
        malformed("native blob lacks XBCH magic");
    }
    if (u32_or(blob, 4) != XBCH_VERSION) {
        malformed("unknown XBCH version");
    }
    std::uint64_t packed = u64_or(blob, 8);
    std::uint64_t unpacked = u64_or(blob, 16);
    if (unpacked > MAX_UNPACKED) {
        malformed("implausible XBCH unpacked size");
    }
    auto stream = bytes(blob, XBCH_HEADER, packed);
    if (!stream) {
        malformed("XBCH stream out of bounds");
    }
    auto out = inflate_zlib_exact(*stream, static_cast<std::size_t>(unpacked));
    if (!out) {
        malformed("XBCH stream does not inflate to its stated size");
    }
    return std::move(*out);
}

std::vector<NativeProgram> programs(std::span<const std::uint8_t> fssm) {
    if (fssm.size() < 4 || std::memcmp(fssm.data(), "FSSM", 4) != 0) {
        malformed("native blob lacks FSSM magic");
    }
    if (u32_or(fssm, 4) != FSSM_VERSION) {
        malformed("unknown FSSM version");
    }
    auto payload = fssm.subspan(std::min(FSSM_HEADER, fssm.size()));
    if (u64_or(fssm, 8) != payload.size()) {
        malformed("FSSM payload size disagrees");
    }
    if (u64_or(payload, 0) != GRAPHICS_SLOT_COUNT) {
        malformed("unexpected FSSM slot count");
    }
    std::vector<NativeProgram> out;
    for (const auto& [stage, at] : SLOTS) {
        std::uint64_t start = u64_or(payload, at);
        if (start == ABSENT) {
            continue;
        }
        auto record = bytes(payload, start, RECORD_SIZE);
        if (!record) {
            malformed("FSSM record out of bounds");
        }
        std::uint64_t code_at = u64_or(*record, 0), code_len = u64_or(*record, 8);
        auto code = bytes(payload, code_at, code_len);
        if (!code) {
            malformed("native code out of bounds");
        }
        auto vgprs = static_cast<std::uint16_t>(u32_or(*record, RECORD_REGS) & 0x3FF);
        if (vgprs > 256) {
            malformed("implausible VGPR count");
        }
        auto table = [&](std::size_t table_at, std::size_t count_at) -> std::span<const std::uint8_t> {
            std::uint64_t len = ENTRY_SIZE * std::uint64_t{u32_or(*record, count_at)};
            if (len == 0) {
                return {};
            }
            auto where = bytes(payload, u64_or(*record, table_at), len);
            if (!where) {
                malformed("FSSM root table out of bounds");
            }
            return *where;
        };
        NativeProgram p;
        p.stage = stage;
        p.vgprs = vgprs;
        auto layout = table(RECORD_LAYOUT, RECORD_COUNTS);
        for (std::size_t e = 0; e + ENTRY_SIZE <= layout.size(); e += ENTRY_SIZE) {
            const std::uint8_t* x = layout.data() + e;
            RootParam r;
            std::memcpy(&r.root, x, 4);
            std::memcpy(&r.table_offset_dwords, x + 4, 4);
            std::memcpy(&r.sgprs, x + 8, 4);
            std::memcpy(&r.base_register, x + 12, 4);
            r.user_data = x[16];
            r.slot = x[17];
            p.layout.push_back(r);
        }
        auto used = table(RECORD_USED, RECORD_COUNTS + 4);
        for (std::size_t e = 0; e + ENTRY_SIZE <= used.size(); e += ENTRY_SIZE) {
            const std::uint8_t* x = used.data() + e;
            UsedResource u;
            u.kind = x[0];
            u.root = x[1];
            std::memcpy(&u.table_offset_dwords, x + 4, 4);
            std::memcpy(&u.lower_bound, x + 8, 4);
            std::memcpy(&u.stride, x + 12, 4);
            u.dwords = x[16];
            u.user_data = x[17];
            u.slot = x[18];
            p.used.push_back(u);
        }

        auto stage_data = payload.subspan(static_cast<std::size_t>(start + RECORD_SIZE));
        switch (stage) {
            case HwStage::Ps: p.io = ps_io(stage_data, code->size()); break;
            case HwStage::Vs: p.io = vs_io(stage_data); break;
            case HwStage::Cs:
                p.io.kind = StageIo::Cs;
                p.io.cs.rsrc2 = u32_or(stage_data, CS_RSRC2);
                p.io.cs.threads[0] = u32_or(stage_data, CS_THREADS);
                p.io.cs.threads[1] = u32_or(stage_data, CS_THREADS + 4);
                p.io.cs.threads[2] = u32_or(stage_data, CS_THREADS + 8);
                p.rsrc2 = p.io.cs.rsrc2;
                break;
            case HwStage::Ms: p.io = ms_io(stage_data, u32_or(*record, 0x4c)); break;
            default: break;
        }
        p.wave64 = (u32_or(*record, RECORD_FLAGS) & FLAG_WAVE32) == 0;
        p.code.assign(code->begin(), code->end());
        out.push_back(std::move(p));
    }
    return out;
}

}   

std::optional<std::vector<Part>> container_parts(std::span<const std::uint8_t> data) {
    constexpr std::size_t HEADER = 0x20, MAX_PARTS = 256;
    if (data.size() < HEADER || std::memcmp(data.data(), "DXBC", 4) != 0) {
        return std::nullopt;
    }
    auto total = u32_at(data, 0x18);
    if (!total || *total > data.size() || *total < HEADER) {
        return std::nullopt;
    }
    data = data.first(*total);
    auto count = u32_at(data, 0x1C);
    if (!count || *count > MAX_PARTS) {
        return std::nullopt;
    }
    std::vector<Part> parts;
    for (std::uint32_t i = 0; i < *count; ++i) {
        auto offset = u32_at(data, HEADER + 4 * i);
        if (!offset) {
            return std::nullopt;
        }
        auto fourcc = u32_at(data, *offset);
        auto size = u32_at(data, std::size_t{*offset} + 4);
        if (!fourcc || !size) {
            return std::nullopt;
        }
        auto body = bytes(data, std::uint64_t{*offset} + 8, *size);
        if (!body) {
            return std::nullopt;
        }
        parts.push_back({*fourcc, *body});
    }
    return parts;
}

std::optional<std::vector<std::uint8_t>> inflate_zlib_exact(std::span<const std::uint8_t> source, std::size_t expected) {
    if (source.size() < 2) {
        return std::nullopt;
    }
    std::uint8_t cmf = source[0], flg = source[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || (std::uint32_t{cmf} << 8 | flg) % 31 != 0 || (flg & 0x20) != 0) {
        return std::nullopt;
    }
    BitReader r{source.subspan(2)};
    std::vector<std::uint8_t> out;
    out.reserve(expected);

    if (!inflate_raw(r, out, expected + 1) || out.size() != expected) {
        return std::nullopt;
    }

    r.align();
    if (r.at + 4 > r.in.size()) {
        return std::nullopt;
    }

    std::uint32_t a = 1, b = 0;
    for (std::size_t at = 0; at < out.size();) {
        std::size_t end = std::min(out.size(), at + 5552);
        for (; at < end; ++at) {
            a += out[at];
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    std::uint32_t stored = std::uint32_t{r.in[r.at]} << 24 | std::uint32_t{r.in[r.at + 1]} << 16 | std::uint32_t{r.in[r.at + 2]} << 8 | r.in[r.at + 3];
    if (stored != (b << 16 | a)) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> inflate_raw_exact(std::span<const std::uint8_t> source, std::size_t expected) {
    BitReader r{source};
    std::vector<std::uint8_t> out;
    out.reserve(expected);
    if (!inflate_raw(r, out, expected + 1) || out.size() != expected) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::size_t> inflate_zlib_into(std::span<const std::uint8_t> source, std::span<std::uint8_t> out) {
    auto copy = [&](const std::vector<std::uint8_t>& got) { std::memcpy(out.data(), got.data(), std::min(got.size(), out.size())); };
    if (source.size() < 2) {
        return std::nullopt;
    }
    std::uint8_t cmf = source[0], flg = source[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || (std::uint32_t{cmf} << 8 | flg) % 31 != 0 || (flg & 0x20) != 0) {
        return std::nullopt;
    }
    BitReader r{source.subspan(2)};
    std::vector<std::uint8_t> got;
    got.reserve(out.size());
    bool ok = inflate_raw(r, got, out.size());

    copy(got);
    if (!ok) {
        return std::nullopt;
    }
    r.align();
    if (r.at + 4 > r.in.size()) {
        return std::nullopt;
    }
    std::uint32_t a = 1, b = 0;
    for (std::size_t at = 0; at < got.size();) {
        std::size_t end = std::min(got.size(), at + 5552);
        for (; at < end; ++at) {
            a += got[at];
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    std::uint32_t stored = std::uint32_t{r.in[r.at]} << 24 | std::uint32_t{r.in[r.at + 1]} << 16 | std::uint32_t{r.in[r.at + 2]} << 8 | r.in[r.at + 3];
    if (stored != (b << 16 | a)) {
        return std::nullopt;
    }
    return got.size();
}

bool is_spilled(const NativeProgram& p) {
    constexpr std::uint32_t FIRST = 16;
    if (p.stage != HwStage::Cs) {
        return false;
    }
    std::uint32_t registers = p.user_sgprs();
    std::uint32_t least = UINT32_MAX;
    for (const RootParam& e : p.layout) {
        if (e.sgprs > 0 && e.user_data >= registers) {
            least = std::min<std::uint32_t>(least, e.user_data);
        }
    }
    return least == FIRST && registers >= 2;
}

std::vector<NativeProgram> native_programs_checked(std::span<const std::uint8_t> part) {
    if (part.size() < XDAT_OFFSET + 4 || std::memcmp(part.data() + XDAT_OFFSET, "XDAT", 4) != 0) {
        return {};
    }
    std::uint32_t count = u32_or(part, XDAT_OFFSET + 4);
    if (count > MAX_XDAT_ENTRIES) {
        malformed("implausible XDAT entry count");
    }
    std::vector<NativeProgram> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::size_t at = XDAT_OFFSET + 12 + 12 * i;
        if (u32_or(part, at) != KIND_NATIVE) {
            continue;
        }
        std::uint32_t offset = u32_or(part, at + 4), size = u32_or(part, at + 8);
        auto blob = bytes(part, offset, size);
        if (!blob) {
            malformed("XDAT entry out of bounds");
        }
        for (auto& x : programs(unpack(*blob))) {
            out.push_back(std::move(x));
        }
    }
    return out;
}

std::optional<std::vector<NativeProgram>> native_programs(std::span<const std::uint8_t> part) {
    try {
        return native_programs_checked(part);
    } catch (const ShaderError&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> NativeProgram::patched_code() const {
    std::vector<std::uint8_t> out = code;
    if (io.kind == StageIo::Ps) {
        for (const ExportPatch& p : io.patches) {
            std::size_t at = std::size_t{p.offset} * 4;
            for (std::size_t i = 0; i < p.code.size(); ++i) {
                std::memcpy(out.data() + at + 4 * i, &p.code[i], 4);
            }
        }
    }
    return out;
}

std::optional<std::vector<Instruction>> decode(std::span<const std::uint8_t> code) {
    try {
        return decode_checked(code);
    } catch (const ShaderError&) {
        return std::nullopt;
    }
}

bool program_stores(const NativeProgram& program) {
    auto code = decode(program.code);
    if (!code) {
        return true;
    }
    for (const Instruction& i : *code) {
        auto n = i.name();
        switch (i.encoding) {
            case Encoding::Mubuf:
            case Encoding::Mtbuf:
            case Encoding::Smem:
            case Encoding::Flat:
            case Encoding::Global:
                if (n.empty() || ((n.find("store") != std::string_view::npos || n.find("atomic") != std::string_view::npos) &&
                                  n.find("scratch") == std::string_view::npos)) {
                    return true;
                }
                break;
            case Encoding::Sop1:
                if (n.empty() || n == "s_setpc_b64" || n == "s_swappc_b64") {
                    return true;
                }
                break;
            default: break;
        }
    }
    return false;
}

bool program_image_stores(const NativeProgram& program) {
    auto code = decode(program.code);
    if (!code) {
        return true;
    }
    for (const Instruction& i : *code) {
        if (i.encoding == Encoding::Mimg) {
            auto n = i.name();
            if (n.empty() || n.find("store") != std::string_view::npos || n.find("atomic") != std::string_view::npos) {
                return true;
            }
        }
    }
    return false;
}

}   
