// SPDX-License-Identifier: GPL-3.0-or-later
// The PC container around emitted DXIL (`emit/container.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See container.hpp.
#include "container.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace danzi::shader::container {

namespace {

void u32s(std::vector<std::uint8_t>& out, std::initializer_list<std::uint32_t> vs) {
    for (std::uint32_t v : vs) {
        for (int k = 0; k < 4; ++k) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    }
}

void put32(std::uint8_t* at, std::uint32_t v) { std::memcpy(at, &v, 4); }


constexpr std::uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
    0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
    0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
    0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
    0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr std::uint32_t S[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

void md5_block(std::uint32_t (&state)[4], const std::uint8_t* block) {
    std::uint32_t x[16];
    std::memcpy(x, block, 64);
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t f, g;
        switch (i / 16) {
            case 0:
                f = (b & c) | (~b & d);
                g = i;
                break;
            case 1:
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
                break;
            case 2:
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
                break;
            default:
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
                break;
        }
        std::uint32_t rotated = std::rotl(a + f + K[i] + x[g], static_cast<int>(S[(i / 16) * 4 + i % 4]));
        std::uint32_t nd = c, nc = b, nb = b + rotated;
        a = d;
        b = nb;
        c = nc;
        d = nd;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

std::uint32_t max_row_plus_one(const std::vector<SigElement>& els) {
    std::uint32_t m = 0;
    for (const auto& e : els) {
        if (e.row) {
            m = std::max(m, *e.row + 1);
        }
    }
    return m;
}

std::uint32_t scalars(const std::vector<SigElement>& els) {
    std::uint32_t m = 0;
    for (const auto& e : els) {
        if (e.row) {
            m = std::max(m, *e.row * 4 + e.col + e.cols);
        }
    }
    return m;
}

}   

std::uint32_t SigElement::system_value() const {
    switch (kind) {
        case 1: return 6;
        case 2: return 8;
        case 3: return 1;
        case 4: return 4;
        case 5: return 5;
        case 6: return 2;
        case 7: return 3;
        case 10: return 7;
        case 12: return 10;
        case 13: return 9;
        case 14: return 66;
        case 15: return 70;
        case 16: return 64;
        case 17: return 65;
        case 18: return 68;
        case 19: return 67;
        case 20: return 69;
        case 28: return 23;
        case 30: return 25;
        default: return 0;
    }
}

std::uint32_t SigElement::sig_comp() const {
    switch (comp) {
        case 1:
        case 5: return 1;
        case 4: return 2;
        case 9: return 3;
        default: return 0;
    }
}

std::vector<std::uint8_t> signature(const std::vector<SigElement>& elements, bool input) {
    std::vector<std::uint8_t> out;
    u32s(out, {static_cast<std::uint32_t>(elements.size()), 8});
    std::vector<std::uint32_t> offsets;
    auto next = static_cast<std::uint32_t>(8 + 32 * elements.size());
    std::vector<const std::string*> names;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const SigElement& e = elements[i];
        std::optional<std::size_t> shared;
        if (e.kind != 0) {
            for (std::size_t j = 0; j < i; ++j) {
                if (elements[j].kind == e.kind) {
                    shared = j;
                    break;
                }
            }
        }
        if (shared) {
            offsets.push_back(offsets[*shared]);
        } else {
            offsets.push_back(next);
            next += static_cast<std::uint32_t>(e.name.size() + 1);
            names.push_back(&e.name);
        }
    }
    std::vector<std::size_t> order(elements.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
        auto kx = std::pair{elements[x].row.value_or(UINT32_MAX), elements[x].col};
        auto ky = std::pair{elements[y].row.value_or(UINT32_MAX), elements[y].col};
        return kx < ky;
    });
    for (std::size_t i : order) {
        const SigElement& e = elements[i];
        auto used = static_cast<std::uint8_t>(e.usage << e.col);
        std::uint8_t rw = input ? used : static_cast<std::uint8_t>(~used & 0xf);
        u32s(out, {0, offsets[i], e.index, e.system_value(), e.sig_comp(), e.row.value_or(UINT32_MAX)});
        out.insert(out.end(), {e.mask(), rw, 0, 0});
        u32s(out, {0});
    }
    for (const std::string* name : names) {
        out.insert(out.end(), name->begin(), name->end());
        out.push_back(0);
    }
    return out;
}

std::uint64_t mesh_output_bytes(const std::vector<SigElement>& vertex, const std::vector<SigElement>& primitive, std::uint32_t max_vertices,
                                std::uint32_t max_primitives) {
    auto sum = [](const std::vector<SigElement>& els) {
        std::uint64_t s = 0;
        for (const auto& e : els) {
            s += e.cols;
        }
        return s;
    };
    auto align = [](std::uint32_t n) { return (std::uint64_t{n} + 31) / 32 * 32; };
    return 4 * (sum(vertex) * align(max_vertices) + sum(primitive) * align(max_primitives));
}

std::vector<std::uint8_t> psv(const Psv& p) {
    std::uint32_t in_vectors = max_row_plus_one(*p.inputs), out_vectors = max_row_plus_one(*p.outputs);
    std::vector<std::uint8_t> out;
    u32s(out, {48});
    std::uint8_t info[48] = {};
    info[0] = p.flag ? 1 : 0;
    if (p.stage == 0) {
        bool any = false;
        for (const auto& e : *p.inputs) {
            any |= e.interp == 6 || e.interp == 7 || e.kind == 12;
        }
        info[1] = any ? 1 : 0;
    }
    std::uint32_t min = p.wave_size ? *p.wave_size : 0, max = p.wave_size ? *p.wave_size : UINT32_MAX;
    put32(info + 16, min);
    put32(info + 20, max);
    info[24] = p.stage;
    for (int k = 0; k < 3; ++k) {
        put32(info + 36 + 4 * k, p.threads[k]);
    }
    info[28] = static_cast<std::uint8_t>(p.inputs->size());
    info[29] = static_cast<std::uint8_t>(p.outputs->size());
    info[31] = static_cast<std::uint8_t>(in_vectors);
    info[32] = static_cast<std::uint8_t>(out_vectors);
    if (p.mesh) {
        const auto& [mesh, shared] = *p.mesh;
        put32(info, shared);
        auto mv = static_cast<std::uint16_t>(mesh.max_vertices), mp = static_cast<std::uint16_t>(mesh.max_primitives);
        std::memcpy(info + 12, &mv, 2);
        std::memcpy(info + 14, &mp, 2);
        info[26] = static_cast<std::uint8_t>(max_row_plus_one(*p.prims));
        info[27] = mesh.topology;
        info[30] = static_cast<std::uint8_t>(p.prims->size());
    }
    if (p.geometry) {
        put32(info, 3);
        put32(info + 4, 5);
        put32(info + 8, 1);
        info[12] = p.flag ? 1 : 0;
        auto mv = static_cast<std::uint16_t>(*p.geometry);
        std::memcpy(info + 26, &mv, 2);
    }
    out.insert(out.end(), info, info + 48);
    u32s(out, {static_cast<std::uint32_t>(p.bindings->size())});
    if (!p.bindings->empty()) {
        u32s(out, {24});
        for (const Binding& b : *p.bindings) {
            u32s(out, {b.kind, b.space, b.reg, b.reg, b.dxil_kind, 0});
        }
    }
    std::vector<std::uint8_t> strings{0};
    std::vector<std::uint32_t> indices;
    struct El {
        std::uint32_t name, index;
        std::uint8_t start, cols;
        const SigElement* e;
    };
    std::vector<El> elements;
    auto add = [&](const SigElement& e) {
        std::size_t name = 0;
        if (e.kind == 0) {
            std::vector<std::uint8_t> needle(e.name.begin(), e.name.end());
            needle.push_back(0);
            auto it = std::search(strings.begin(), strings.end(), needle.begin(), needle.end());
            if (it != strings.end()) {
                name = static_cast<std::size_t>(it - strings.begin());
            } else {
                strings.insert(strings.end(), needle.begin(), needle.end());
                name = strings.size() - needle.size();
            }
        }
        std::size_t index;
        auto it = std::find(indices.begin(), indices.end(), e.index);
        if (it != indices.end()) {
            index = static_cast<std::size_t>(it - indices.begin());
        } else {
            indices.push_back(e.index);
            index = indices.size() - 1;
        }
        std::uint8_t start = 0, alloc = 0;
        if (e.row) {
            start = static_cast<std::uint8_t>(*e.row);
            alloc = static_cast<std::uint8_t>(0x40 | (e.col << 4));
        }
        elements.push_back(El{static_cast<std::uint32_t>(name), static_cast<std::uint32_t>(index), start, static_cast<std::uint8_t>(e.cols | alloc), &e});
    };
    for (const auto& e : *p.inputs) {
        add(e);
    }
    for (const auto& e : *p.outputs) {
        add(e);
    }
    for (const auto& e : *p.prims) {
        add(e);
    }
    strings.resize((strings.size() + 3) / 4 * 4, 0);
    u32s(out, {static_cast<std::uint32_t>(strings.size())});
    out.insert(out.end(), strings.begin(), strings.end());
    u32s(out, {static_cast<std::uint32_t>(indices.size())});
    for (std::uint32_t i : indices) {
        u32s(out, {i});
    }
    if (!elements.empty()) {
        u32s(out, {16});
        for (const El& el : elements) {
            u32s(out, {el.name, el.index});
            out.insert(out.end(), {1, el.start, el.cols, el.e->kind, static_cast<std::uint8_t>(el.e->sig_comp()), el.e->interp, 0, 0});
        }
    }
    if (in_vectors > 0 && out_vectors > 0) {
        std::uint32_t ni = scalars(*p.inputs), no = scalars(*p.outputs);
        std::uint32_t per = (out_vectors * 4 + 31) / 32;
        for (std::uint32_t i = 0; i < in_vectors * 4; ++i) {
            for (std::uint32_t k = 0; k < per; ++k) {
                std::uint32_t bits = 0;
                if (i < ni) {
                    bits = std::min<std::uint32_t>(no > 32 * k ? no - 32 * k : 0, 32);
                }
                std::uint32_t m = bits == 32 ? UINT32_MAX : (1u << bits) - 1;
                u32s(out, {m});
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> build(const std::vector<std::pair<const char*, std::span<const std::uint8_t>>>& parts) {
    std::size_t table = 0x20 + 4 * parts.size();
    std::size_t total = table;
    for (const auto& [_, d] : parts) {
        total += 8 + d.size();
    }
    std::vector<std::uint8_t> out;
    out.reserve(total);
    out.insert(out.end(), {'D', 'X', 'B', 'C'});
    out.resize(20, 0);
    out.insert(out.end(), {1, 0, 0, 0});
    u32s(out, {static_cast<std::uint32_t>(total), static_cast<std::uint32_t>(parts.size())});
    std::size_t offset = table;
    for (const auto& [_, d] : parts) {
        u32s(out, {static_cast<std::uint32_t>(offset)});
        offset += 8 + d.size();
    }
    for (const auto& [fourcc, d] : parts) {
        out.insert(out.end(), fourcc, fourcc + 4);
        u32s(out, {static_cast<std::uint32_t>(d.size())});
        out.insert(out.end(), d.begin(), d.end());
    }
    auto digest = hash(std::span(out).subspan(20));
    std::memcpy(out.data() + 4, digest.data(), 16);
    return out;
}

std::array<std::uint8_t, 16> hash(std::span<const std::uint8_t> data) {
    std::size_t len = data.size();
    std::size_t left = len % 64;
    std::vector<std::uint8_t> tail;
    tail.reserve(128);
    auto bits = static_cast<std::uint32_t>(len) << 3;
    auto push32 = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            tail.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    };
    if (left < 56) {
        push32(bits);
        tail.insert(tail.end(), data.end() - static_cast<std::ptrdiff_t>(left), data.end());
        tail.push_back(0x80);
        tail.resize(60, 0);
    } else {
        tail.insert(tail.end(), data.end() - static_cast<std::ptrdiff_t>(left), data.end());
        tail.push_back(0x80);
        tail.resize(64, 0);
        push32(bits);
        tail.resize(124, 0);
    }
    push32(static_cast<std::uint32_t>(len) << 1 | 1);
    std::uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    for (std::size_t at = 0; at + 64 <= len - left; at += 64) {
        md5_block(state, data.data() + at);
    }
    for (std::size_t at = 0; at + 64 <= tail.size(); at += 64) {
        md5_block(state, tail.data() + at);
    }
    std::array<std::uint8_t, 16> out{};
    std::memcpy(out.data(), state, 16);
    return out;
}

}   
