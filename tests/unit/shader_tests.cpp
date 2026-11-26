// SPDX-License-Identifier: GPL-3.0-or-later
// The translator's writer and passes on synthetic modules (milestone 5):
// the bytes written are pinned to the reference's own pins
// (`emit/bitcode.rs`, `written_bytes_unchanged`).
#include <cstdint>
#include <vector>

#include <span>

#include "check.hpp"
#include "inflate_fixtures.hpp"
#include "shader/bitcode.hpp"
#include "shader/native.hpp"

namespace bc = danzi::shader::bitcode;

namespace {

std::uint64_t fnv(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint8_t b : bytes) {
        h = (h ^ b) * 0x100000001b3ull;
    }
    return h;
}


bc::Module synthetic(std::size_t regs, std::size_t stages, std::size_t read) {
    bc::Module m;
    bc::Ty i32t = m.int_(32);
    bc::Ty void_t = m.void_();
    bc::Ty fty = m.func_ty(i32t, {i32t, i32t});
    bc::V f = m.function("dx.op.synthetic", fty, bc::Attr::ReadNone, false);
    bc::Ty main_ty = m.func_ty(void_t, {});
    m.function("main", main_ty, std::nullopt, true);
    std::size_t b = m.new_block();
    std::vector<bc::V> cur;
    for (std::size_t r = 0; r < regs; ++r) {
        cur.push_back(m.i32(static_cast<std::uint32_t>(r)));
    }
    for (std::size_t s = 0; s < stages; ++s) {
        bc::V c = m.cmp(b, bc::pred::EQ, cur[0], cur[s % regs]);
        std::size_t t = m.new_block(), e = m.new_block(), j = m.new_block();
        m.terminate(b, bc::Term::cond_br(c, t, e));
        bc::V x = m.call(t, f, {cur[s % regs], cur[(s + 1) % regs]});
        m.terminate(t, bc::Term::br(j));
        m.terminate(e, bc::Term::br(j));
        std::vector<bc::V> next;
        for (std::size_t r = 0; r < regs; ++r) {
            bc::V p = m.phi(j, i32t);
            bc::V tv = r == s % regs ? x : cur[r];
            m.set_phi(p, {{tv, t}, {cur[r], e}});
            next.push_back(p);
        }
        cur = std::move(next);
        b = j;
    }
    std::size_t head = m.new_block(), body = m.new_block(), latch = m.new_block(), exit = m.new_block();
    m.terminate(b, bc::Term::br(head));
    std::vector<bc::V> phis;
    for (std::size_t r = 0; r < regs; ++r) {
        phis.push_back(m.phi(head, i32t));
    }
    bc::V one = m.i32(1);
    std::vector<bc::V> stepped;
    for (bc::V p : phis) {
        stepped.push_back(m.bin(body, bc::Bin::Add, p, one));
    }
    bc::V c = m.cmp(body, bc::pred::IULT, stepped[0], cur[0]);
    m.terminate(head, bc::Term::br(body));
    m.terminate(body, bc::Term::cond_br(c, latch, exit));
    m.terminate(latch, bc::Term::br(head));
    for (std::size_t r = 0; r < regs; ++r) {
        m.set_phi(phis[r], {{cur[r], b}, {stepped[r], latch}});
    }
    bc::V acc = stepped[0];
    for (std::size_t k = 1; k < std::max<std::size_t>(read, 1); ++k) {
        acc = m.bin(exit, bc::Bin::Add, acc, stepped[k]);
    }
    m.call(exit, f, {acc, cur[0]});
    m.terminate(exit, bc::Term::ret());
    return m;
}

}   

TEST(shader_bitcode_written_bytes_match_the_reference_pins) {
    struct Pin {
        std::size_t regs, stages, read, len;
        std::uint64_t hash;
    };
    for (const Pin& p : {Pin{64, 64, 64, 38916, 0x9586730cf9534a45ull}, Pin{128, 256, 32, 295952, 0x89e5de2d64f08785ull}}) {
        auto bytes = synthetic(p.regs, p.stages, p.read).write();
        CHECK_EQ(bytes.size(), p.len);
        CHECK_EQ(fnv(bytes), p.hash);
    }
}

TEST(shader_bitcode_unread_phis_are_dropped) {
    bc::Module m;
    bc::Ty i32t = m.int_(32);
    std::size_t entry = m.new_block(), join = m.new_block();
    bc::V a = m.i32(1), b = m.i32(2);
    m.terminate(entry, bc::Term::br(join));
    bc::V read = m.phi(join, i32t);
    bc::V unread = m.phi(join, i32t);
    m.set_phi(read, {{a, entry}});
    m.set_phi(unread, {{b, entry}});
    m.bin(join, bc::Bin::Add, read, b);
    m.terminate(join, bc::Term::ret());


    auto with_one = m.write();
    bc::Module m2;
    bc::Ty i32t2 = m2.int_(32);
    std::size_t e2 = m2.new_block(), j2 = m2.new_block();
    bc::V a2 = m2.i32(1), b2 = m2.i32(2);
    m2.terminate(e2, bc::Term::br(j2));
    bc::V r2 = m2.phi(j2, i32t2);
    m2.set_phi(r2, {{a2, e2}});
    m2.bin(j2, bc::Bin::Add, r2, b2);
    m2.terminate(j2, bc::Term::ret());
    CHECK(with_one == m2.write());
}

namespace {


std::vector<std::uint8_t> fixture_text(std::size_t n, std::uint32_t seed) {
    static const std::vector<std::vector<std::uint8_t>> WORDS = [] {
        std::vector<std::vector<std::uint8_t>> w;
        for (const char* t : {"lane ", "mask ", "wave ", "exec ", "vgpr ", "sgpr ", "scalar ", "vector ", "export ", "image ", "sample ",
                              "buffer "}) {
            w.emplace_back(t, t + std::char_traits<char>::length(t));
        }
        w.push_back({0x00, 0x01, 0x02});
        w.push_back({0xff, 0xfe});
        w.push_back({'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'});
        w.push_back({'\n'});
        return w;
    }();
    std::vector<std::uint8_t> out;
    std::uint32_t x = seed;
    while (out.size() < n) {
        x = x * 1103515245u + 12345u;
        const auto& w = WORDS[(x >> 16) & 15];
        out.insert(out.end(), w.begin(), w.end());
    }
    out.resize(n);
    return out;
}

void check_stream(std::span<const std::uint8_t> z, std::uint32_t size, std::uint32_t seed) {
    using danzi::shader::inflate_zlib_exact;
    auto want = fixture_text(size, seed);
    auto got = inflate_zlib_exact(z, size);
    CHECK(got.has_value());
    CHECK(got && *got == want);

    CHECK(!inflate_zlib_exact(z, size - 1).has_value());
    CHECK(!inflate_zlib_exact(z, size + 1).has_value());

    for (std::size_t cut : {std::size_t{1}, std::size_t{4}, std::size_t{5}, z.size() / 2}) {
        CHECK(!inflate_zlib_exact(z.first(z.size() - cut), size).has_value());
    }
    std::vector<std::uint8_t> bad(z.begin(), z.end());
    bad.back() ^= 1;
    CHECK(!inflate_zlib_exact(bad, size).has_value());
}

}   


TEST(shader_inflate_synthetic_streams) {
    check_stream(STORED_ZLIB, STORED_SIZE, STORED_SEED);
    check_stream(FIXED_ZLIB, FIXED_SIZE, FIXED_SEED);
    check_stream(DYNAMIC_ZLIB, DYNAMIC_SIZE, DYNAMIC_SEED);
}
