// SPDX-License-Identifier: GPL-3.0-or-later
// Milestone 4.4: the port's texture layouts (src/formats/texture.cpp) against
// the reference's (`victory-formats`, through `victory_ref.dll`'s
// `oracle_texture_layout` and `oracle_texture_copy`).
//
//   layout-check fuzz  <victory_ref.dll> <count> <seed>
//     Random descriptions (sizes, formats, modes, mips, samples, dimensions,
//     valid and not): the layout text compared whole; for valid addressable
//     ones of up to 16 MiB, one subresource detiled from random bytes and
//     tiled back from random rows, compared byte for byte.
//   layout-check bench <victory_ref.dll> <repeats> [cpu]
//     Detile and tile of textures the title streams (time per call, median
//     of `repeats`, both sides in this process, alternating, pinned).
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "d3d12x/sampler.hpp"
#include "formats/texture.hpp"

using namespace danzi::formats;

namespace {

struct Query {
    std::uint32_t dimension, width, height, depth_or_array, mips, format, samples, swizzle;
};

using LayoutFn = std::size_t(__stdcall*)(const Query*, std::uint8_t*, std::size_t);
using CopyFn = std::uint32_t(__stdcall*)(const Query*, std::uint32_t, std::uint32_t, std::uint8_t*, std::size_t, std::uint32_t, std::uint32_t,
                                         std::uint8_t*, std::size_t, std::size_t, std::size_t);

struct Oracle {
    LayoutFn layout;
    CopyFn copy;
    std::uint64_t(__stdcall* size)(const Query*);
    explicit Oracle(const char* path) {
        HMODULE dll = LoadLibraryA(path);
        if (!dll) {
            std::fprintf(stderr, "cannot load %s\n", path);
            std::exit(2);
        }
        layout = reinterpret_cast<LayoutFn>(GetProcAddress(dll, "oracle_texture_layout"));
        copy = reinterpret_cast<CopyFn>(GetProcAddress(dll, "oracle_texture_copy"));
        size = reinterpret_cast<decltype(size)>(GetProcAddress(dll, "oracle_texture_size"));
        if (!layout || !copy || !size) {
            std::fprintf(stderr, "the oracle lacks the layout exports\n");
            std::exit(2);
        }
    }
    std::string text(const Query& q) const {
        std::string s(1 << 16, '\0');
        std::size_t n = layout(&q, reinterpret_cast<std::uint8_t*>(s.data()), s.size());
        if (n > s.size()) {
            s.resize(n);
            layout(&q, reinterpret_cast<std::uint8_t*>(s.data()), s.size());
        }
        s.resize(n);
        return s;
    }
};

const char* error_name(Error e) {
    switch (e) {
        case Error::Invalid: return "Invalid";
        case Error::Unsupported: return "Unsupported";
        case Error::TooSmall: return "TooSmall";
        default: return "None";
    }
}

std::uint32_t error_code(Error e) {
    switch (e) {
        case Error::None: return 0;
        case Error::Invalid: return 1;
        case Error::Unsupported: return 2;
        default: return 3;
    }
}

bool texture_of(const Query& q, Texture& t, Error& e) {
    if (q.dimension > 2) {
        return false;
    }
    TextureDesc d{static_cast<Dimension>(q.dimension), q.width, q.height, q.depth_or_array, q.mips, q.format, q.samples, Swizzle{q.swizzle}};
    e = Texture::make(d, t);
    return true;
}


std::string port_text(const Query& q) {
    Texture t;
    Error e;
    if (!texture_of(q, t, e)) {
        return "no dimension\n";
    }
    std::ostringstream s;
    if (e != Error::None) {
        s << "error " << error_name(e) << "\n";
        return s.str();
    }
    auto b = [](bool v) { return v ? "true" : "false"; };
    s << "size " << t.size << " alignment " << t.alignment << " planes " << t.planes.size() << "\n";
    for (const Plane& p : t.planes) {
        const Layout& l = p.layout;
        s << "plane offset " << p.offset << " stencil " << b(p.stencil) << " block [" << l.block[0] << ", " << l.block[1] << ", " << l.block[2] << "] thick "
          << b(l.thick) << " slices " << l.slices << " slice_size " << l.slice_size << " size " << l.size << " alignment " << l.alignment
          << " first_tail_mip " << l.first_tail_mip << " addressable " << b(l.addressable()) << "\n";
        for (std::uint32_t i = 0; i < l.mips.size(); ++i) {
            const Mip& m = l.mips[i];
            s << " mip " << i << " " << m.width << "x" << m.height << "x" << m.depth << " pitch " << m.pitch << " rows " << m.rows << " depth_pitch "
              << m.depth_pitch << " offset " << m.offset << " tail ";
            if (m.tail) {
                s << m.tail->offset << "," << m.tail->x << "," << m.tail->y;
            } else {
                s << "-";
            }
            s << " bytes_from " << l.bytes_from(i) << "\n";
            std::uint32_t w = m.width, h = m.height;
            const std::uint32_t pts[5][2] = {{0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}, {w / 2, h / 2}};
            for (const auto& pt : pts) {
                std::uint64_t o = 0;
                Error oe = l.offset(i, pt[0], pt[1], 0, o);
                s << " ";
                if (oe == Error::None) {
                    s << o;
                } else {
                    s << error_name(oe);
                }
            }
            std::uint32_t last = std::max(q.dimension == 2 ? m.depth : l.slices, 1u) - 1;
            std::uint64_t o = 0;
            Error oe = l.offset(i, 0, 0, last, o);
            s << " z" << last << " ";
            if (oe == Error::None) {
                s << o;
            } else {
                s << error_name(oe);
            }
            s << "\n";
        }
    }
    return s.str();
}

constexpr std::uint32_t FORMATS[] = {2, 10, 16, 20, 24, 28, 29, 40, 41, 45, 49, 53, 54, 56, 61, 62, 71, 72, 74, 77, 80, 83, 87, 91, 95, 98, 116, 118, 0, 5, 100};
constexpr std::uint32_t MODES[] = {0, 1, 2, 5, 6, 9, 10, 17, 18, 21, 22, 23, 24, 25, 26, 27, 3, 31};

Query random_query(std::mt19937_64& rng) {
    auto pick = [&](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
    auto extent = [&]() -> std::uint32_t {
        switch (pick(8)) {
            case 0: return 1 + pick(8);
            case 1: return 1u << pick(15);
            case 2: return (1u << pick(14)) + pick(3);
            case 3: return 16385;
            default: return 1 + pick(2048);
        }
    };
    Query q{};
    q.dimension = pick(20) == 0 ? 3 : (pick(6) == 0 ? 0 : (pick(5) == 0 ? 2 : 1));
    q.width = extent();
    q.height = q.dimension == 0 ? (pick(10) == 0 ? 2 : 1) : extent();
    q.depth_or_array = pick(3) == 0 ? 1 + pick(64) : 1;
    q.mips = pick(4) == 0 ? 0 : pick(17);
    q.format = FORMATS[pick(sizeof FORMATS / sizeof FORMATS[0])];
    q.samples = pick(6) == 0 ? (1u << pick(5)) : (pick(10) == 0 ? 0 : 1);
    q.swizzle = MODES[pick(sizeof MODES / sizeof MODES[0])];
    if (q.dimension == 2 && pick(2) == 0) {

        q.width = std::min(q.width, 256u);
        q.height = std::min(q.height, 256u);
    }
    return q;
}

std::string describe(const Query& q) {
    char b[160];
    std::snprintf(b, sizeof b, "dim %u %ux%ux%u mips %u format %u samples %u swizzle %u", q.dimension, q.width, q.height, q.depth_or_array, q.mips, q.format,
                  q.samples, q.swizzle);
    return b;
}

int fuzz(const Oracle& o, std::uint64_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uint64_t mismatches = 0, valid = 0, copied = 0;
    for (std::uint64_t n = 0; n < count; ++n) {
        Query q = random_query(rng);
        std::string a = o.text(q), b = port_text(q);
        if (a != b) {
            if (++mismatches <= 5) {
                std::printf("MISMATCH %s\n--- reference\n%s--- port\n%s", describe(q).c_str(), a.c_str(), b.c_str());
            }
            continue;
        }
        Texture t;
        Error e;
        if (!texture_of(q, t, e) || e != Error::None) {
            continue;
        }
        ++valid;
        if (t.size > (16u << 20)) {
            continue;
        }
        std::uint32_t plane = static_cast<std::uint32_t>(rng() % t.planes.size());
        const Layout& l = t.planes[plane].layout;
        std::uint32_t mip = static_cast<std::uint32_t>(rng() % l.mips.size());
        std::uint32_t slice = q.dimension == 2 ? 0 : static_cast<std::uint32_t>(rng() % std::max(l.slices, 1u));
        const Mip& m = l.mips[mip];
        std::size_t row_pitch = std::size_t{m.width} * l.desc.element_bytes + (rng() % 3) * 16;
        std::size_t depth_pitch = row_pitch * m.height;
        std::size_t linear_len = depth_pitch * m.depth;
        std::vector<std::uint8_t> tiled(static_cast<std::size_t>(l.size)), lin_a(linear_len, 0), lin_b(linear_len, 0);
        for (auto& x : tiled) {
            x = static_cast<std::uint8_t>(rng());
        }
        Query qq = q;
        std::uint32_t ra = o.copy(&qq, plane, 0, tiled.data(), tiled.size(), mip, slice, lin_a.data(), lin_a.size(), row_pitch, depth_pitch);
        std::uint32_t rb = error_code(l.detile(tiled.data(), tiled.size(), mip, slice, lin_b.data(), lin_b.size(), row_pitch, depth_pitch));
        bool same = ra == rb && lin_a == lin_b;
        std::vector<std::uint8_t> tiled_a(tiled.size(), 0), tiled_b(tiled.size(), 0);
        for (auto& x : lin_a) {
            x = static_cast<std::uint8_t>(rng());
        }
        std::uint32_t ta = o.copy(&qq, plane, 1, tiled_a.data(), tiled_a.size(), mip, slice, lin_a.data(), lin_a.size(), row_pitch, depth_pitch);
        std::uint32_t tb = error_code(l.tile(tiled_b.data(), tiled_b.size(), mip, slice, lin_a.data(), lin_a.size(), row_pitch, depth_pitch));
        same = same && ta == tb && tiled_a == tiled_b;
        if (ra == 0) {
            ++copied;
        }
        if (!same) {
            if (++mismatches <= 5) {
                std::printf("COPY MISMATCH %s plane %u mip %u slice %u: detile %u/%u tile %u/%u\n", describe(q).c_str(), plane, mip, slice, ra, rb, ta, tb);
            }
        }
    }
    std::printf("layout fuzz: %llu descriptions (%llu valid, %llu subresources copied both ways), %llu mismatches\n", static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(valid), static_cast<unsigned long long>(copied), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

double now_ns() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(f.QuadPart);
}

int bench(const Oracle& o, int repeats, DWORD_PTR cpu) {
    SetThreadAffinityMask(GetCurrentThread(), cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    struct Case {
        const char* name;
        Query q;
        std::uint32_t mip;
    };


    const Case cases[] = {
        {"bc7-2048-64k_s-mip0", {1, 2048, 2048, 1, 0, 98, 1, 9}, 0},
        {"bc1-4096-64k_s-mip0", {1, 4096, 4096, 1, 0, 71, 1, 9}, 0},
        {"bc5-1024-4k_s-mip0", {1, 1024, 1024, 1, 0, 83, 1, 5}, 0},
        {"rgba8-1024-64k_d-mip0", {1, 1024, 1024, 1, 1, 28, 1, 10}, 0},
        {"rgba8-1024-4k_s-mip2", {1, 1024, 1024, 1, 0, 28, 1, 5}, 2},
        {"r16-512-4k_s-mip0", {1, 512, 512, 1, 0, 54, 1, 5}, 0},
        {"bc7-2048-64k_s-tail-mip9", {1, 2048, 2048, 1, 0, 98, 1, 9}, 9},
        {"rgba16f-256-vol64-4k_s", {2, 256, 256, 64, 1, 10, 1, 5}, 0},
        {"rgba8-1920-linear", {1, 1920, 1080, 1, 1, 28, 1, 0}, 0},
    };
    std::printf("%-28s %8s | %12s %12s %7s | %12s %12s %7s\n", "case", "MiB", "rust detile", "c++ detile", "", "rust tile", "c++ tile", "");
    for (const Case& c : cases) {
        Texture t;
        Error e;
        if (!texture_of(c.q, t, e) || e != Error::None) {
            std::printf("%s: no layout\n", c.name);
            continue;
        }
        const Layout& l = t.planes[0].layout;
        const Mip& m = l.mips[c.mip];
        std::size_t row_pitch = std::size_t{m.width} * l.desc.element_bytes;
        std::size_t depth_pitch = row_pitch * m.height;


        std::size_t shift = 0;
        if (const char* v = std::getenv("LAYOUT_LINEAR_SHIFT")) {
            shift = std::strtoull(v, nullptr, 10);
        }
        std::vector<std::uint8_t> tiled(static_cast<std::size_t>(l.size)), linear_room(depth_pitch * m.depth + shift);
        struct {
            std::uint8_t* p;
            std::size_t n;
            std::uint8_t* data() { return p; }
            std::size_t size() const { return n; }
        } linear{linear_room.data() + shift, depth_pitch * m.depth};
        std::mt19937_64 rng(1);
        for (auto& x : tiled) {
            x = static_cast<std::uint8_t>(rng());
        }
        Query q = c.q;
        {


            constexpr int CALLS = 2000;
            std::vector<double> made[3];
            volatile std::uint64_t sink = 0;
            for (int r = 0; r < repeats; ++r) {
                for (int k = 0; k < 3; ++k) {
                    double t0 = now_ns();
                    for (int i = 0; i < CALLS; ++i) {
                        if (k == 0) {
                            sink = o.size(&q);
                        } else {
                            Texture tt;
                            Error te;
                            TextureDesc d{static_cast<Dimension>(q.dimension), q.width, q.height, q.depth_or_array, q.mips, q.format, q.samples, Swizzle{q.swizzle}};
                            te = Texture::make(d, tt, k == 2);
                            sink = tt.size;
                        }
                    }
                    made[k].push_back((now_ns() - t0) / CALLS);
                }
            }
            for (auto& v : made) {
                std::sort(v.begin(), v.end());
            }
            double a = made[0][made[0].size() / 2], b = made[1][made[1].size() / 2], cc = made[2][made[2].size() / 2];
            std::printf("  layout %-28s rust %8.1f ns  c++ sizes %8.1f ns (%5.2fx)  c++ with tables %8.1f ns (%5.2fx)\n", c.name, a, b, a / b, cc, a / cc);
        }
        if (std::getenv("LAYOUT_PROBE")) {

            for (int k = 0; k < 4; ++k) {
                bool rust = (k % 2 == 0) != (std::getenv("LAYOUT_PROBE")[0] == '2');
                std::uint32_t tile = k / 2;
                std::vector<double> ts;
                for (int r = 0; r < repeats; ++r) {
                    double t0 = now_ns();
                    if (rust) {
                        o.copy(&q, 0, tile, tiled.data(), tiled.size(), c.mip, 0, linear.data(), linear.size(), row_pitch, depth_pitch);
                    } else if (tile) {
                        l.tile(tiled.data(), tiled.size(), c.mip, 0, linear.data(), linear.size(), row_pitch, depth_pitch);
                    } else {
                        l.detile(tiled.data(), tiled.size(), c.mip, 0, linear.data(), linear.size(), row_pitch, depth_pitch);
                    }
                    ts.push_back(now_ns() - t0);
                }
                std::sort(ts.begin(), ts.end());
                std::printf("  probe %s %s %s: %.1fus\n", c.name, rust ? "rust" : "c++", tile ? "tile" : "detile", ts[ts.size() / 2] / 1000);
            }
            continue;
        }
        std::vector<double> times[4];
        for (int r = 0; r < repeats; ++r) {
            for (int side = 0; side < 2; ++side) {
                bool rust = (side == 0) == (r % 2 == 0);
                for (int tile = 0; tile < 2; ++tile) {
                    double t0 = now_ns();
                    if (rust) {
                        o.copy(&q, 0, tile, tiled.data(), tiled.size(), c.mip, 0, linear.data(), linear.size(), row_pitch, depth_pitch);
                    } else {


                        Texture tt;
                        Error te;
                        texture_of(q, tt, te);
                        const Layout& ll = tt.planes[0].layout;
                        if (tile) {
                            ll.tile(tiled.data(), tiled.size(), c.mip, 0, linear.data(), linear.size(), row_pitch, depth_pitch);
                        } else {
                            ll.detile(tiled.data(), tiled.size(), c.mip, 0, linear.data(), linear.size(), row_pitch, depth_pitch);
                        }
                    }
                    times[(rust ? 0 : 1) + 2 * tile].push_back(now_ns() - t0);
                }
            }
        }
        double med[4];
        for (int i = 0; i < 4; ++i) {
            std::sort(times[i].begin(), times[i].end());
            med[i] = times[i][times[i].size() / 2];
        }
        auto us = [](double ns) { return ns / 1000.0; };
        std::printf("%-28s %8.2f | %10.1fus %10.1fus %6.2fx | %10.1fus %10.1fus %6.2fx\n", c.name, static_cast<double>(linear.size()) / (1 << 20), us(med[0]),
                    us(med[1]), med[0] / med[1], us(med[2]), us(med[3]), med[2] / med[3]);
    }
    std::printf("bench: median of %d calls each, both sides in one process alternating, thread pinned to mask 0x%llx\n", repeats,
                static_cast<unsigned long long>(cpu));
    return 0;
}

}   


int samplers(const char* dll, std::uint64_t count, std::uint64_t seed) {
    HMODULE m = LoadLibraryA(dll);
    auto oracle = m ? reinterpret_cast<void(__stdcall*)(const std::uint32_t*, std::uint32_t*)>(GetProcAddress(m, "oracle_console_sampler")) : nullptr;
    if (!oracle) {
        std::fprintf(stderr, "the oracle lacks oracle_console_sampler\n");
        return 2;
    }
    std::mt19937_64 rng(seed);
    std::uint64_t mismatches = 0;
    for (std::uint64_t n = 0; n < count; ++n) {
        std::uint32_t w[27];
        for (int i = 0; i < 27; ++i) {
            std::uint32_t r = static_cast<std::uint32_t>(rng());
            switch (rng() % 4) {
                case 0: w[i] = r % 5; break;
                case 1: w[i] = r % 20; break;
                case 2: w[i] = r; break;
                default: {
                    const float f[] = {0.0f, 1.0f, -0.5f, 1000.0f, 3.4e38f, 1e-40f};
                    std::memcpy(&w[i], &f[r % 6], 4);
                }
            }
        }
        std::uint32_t a[13];
        oracle(w, a);
        auto b = danzi::d3d12x::console_sampler(w);
        if (std::memcmp(a, b.data(), sizeof a) != 0 && ++mismatches <= 5) {
            std::printf("MISMATCH");
            for (std::uint32_t x : w) {
                std::printf(" %x", x);
            }
            std::printf("\n");
        }
    }
    std::printf("sampler fuzz: %llu descriptions, %llu mismatches\n", static_cast<unsigned long long>(count), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "samplers") == 0) {
        return samplers(argv[2], std::stoull(argv[3]), std::stoull(argv[4]));
    }
    if (argc >= 5 && std::strcmp(argv[1], "fuzz") == 0) {
        Oracle o(argv[2]);
        return fuzz(o, std::stoull(argv[3]), std::stoull(argv[4]));
    }
    if (argc >= 4 && std::strcmp(argv[1], "bench") == 0) {
        Oracle o(argv[2]);
        return bench(o, std::stoi(argv[3]), argc > 4 ? static_cast<DWORD_PTR>(std::stoull(argv[4], nullptr, 16)) : 0x4);
    }
    std::fprintf(stderr, "usage: layout-check fuzz <victory_ref.dll> <count> <seed>\n       layout-check bench <victory_ref.dll> <repeats> [cpu-mask-hex]\n");
    return 2;
}
