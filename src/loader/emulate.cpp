// SPDX-License-Identifier: GPL-3.0-or-later
// Emulation of the console CPU's user-mode instructions the host lacks. See
// emulate.hpp.
//
// Port-only, and an independent implementation: written for this port from
// the AMD64 Architecture Programmer's Manual (vol. 3: general-purpose and
// system instructions, CPUID; vol. 4: SSE4a) and the Intel 64 and IA-32
// Architectures Software Developer's Manual (vol. 2: instruction encoding,
// SHA, ADX, RDSEED, RDPID). It is not derived from XCL
// (github.com/ivall/xcl, GPL-2.0-only) nor from the reference's
// `victory-loader/src/console_insn.rs` / `console_insn_decode.rs`, which
// state that they are ported from XCL; neither was consulted. See NOTICE.md.
#include "emulate.hpp"

#include <windows.h>

#include <bcrypt.h>
#include <immintrin.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "core/env.hpp"
#include "core/log.hpp"
#include "loader/crash.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace danzi::loader::emulate {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

constexpr std::string_view TARGET = "victory_loader::emulate";
constexpr auto RELAXED = std::memory_order_relaxed;


constexpr u32 CF = 1u << 0, PF = 1u << 2, AF = 1u << 4, ZF = 1u << 6, SF = 1u << 7, OF = 1u << 11;
constexpr u32 STATUS = CF | PF | AF | ZF | SF | OF;

static_assert(offsetof(CONTEXT, R15) - offsetof(CONTEXT, Rax) == 15 * sizeof(DWORD64), "CONTEXT's integer registers in encoding order");


struct Host {
    u32 features = 0;
    bool rdrand = false;
};

Host detect() {
    Host h;
    int r[4];
    __cpuid(r, 0);
    unsigned max = static_cast<unsigned>(r[0]);
    __cpuid(r, static_cast<int>(0x80000000));
    unsigned ext = static_cast<unsigned>(r[0]);
    auto bit = [](int reg, unsigned n) { return (static_cast<unsigned>(reg) >> n) & 1; };
    if (max >= 1) {
        __cpuid(r, 1);
        h.rdrand = bit(r[2], 30);
    }
    if (max >= 7) {
        __cpuidex(r, 7, 0);
        if (bit(r[1], 18)) h.features |= RDSEED;
        if (bit(r[1], 19)) h.features |= ADX;
        if (bit(r[1], 23)) h.features |= CLFLUSHOPT;
        if (bit(r[1], 24)) h.features |= CLWB;
        if (bit(r[1], 29)) h.features |= SHA;
        if (bit(r[2], 22)) h.features |= RDPID;
    }
    if (ext >= 0x80000001) {
        __cpuid(r, static_cast<int>(0x80000001));
        if (bit(r[2], 6)) h.features |= SSE4A;
        if (bit(r[2], 29)) h.features |= MONITORX;
    }
    if (ext >= 0x80000008) {
        __cpuid(r, static_cast<int>(0x80000008));
        if (bit(r[1], 0)) h.features |= CLZERO;
        if (bit(r[1], 4)) h.features |= RDPRU;
    }
    return h;
}

const Host HOST = detect();
std::atomic<u32> MISSING{ALL_FEATURES & ~HOST.features};
std::atomic<int> PATCH{-1};
std::atomic<bool> LOOPS{true};


struct Counter {
    std::atomic<u64> n{0};
    char pad[56];
};
constexpr unsigned KINDS = static_cast<unsigned>(Kind::Count);
Counter COUNTS[KINDS];
Counter TOTAL, PATCHED, ZEROED;
std::atomic<bool> SEEN[KINDS];
std::atomic<bool> SEEN_PATCH[KINDS];
std::atomic<u64> LAST_SUMMARY{0};

void summary(u64 total) {
    std::string kinds;
    for (unsigned k = 0; k < KINDS; ++k) {
        u64 n = COUNTS[k].n.load(RELAXED);
        if (n) {
            kinds += (kinds.empty() ? "" : ", ") + std::string(name(static_cast<Kind>(k))) + " " + std::to_string(n);
        }
    }
    log::info(TARGET, "emulated instructions",
              {{"total", log::debug(total)}, {"patched_sites", log::debug(PATCHED.n.load(RELAXED))},
               {"clzero_bytes", log::debug(ZEROED.n.load(RELAXED))}, {"kinds", kinds}});
}

void note(Kind kind, u64 rip) {
    unsigned k = static_cast<unsigned>(kind);
    COUNTS[k].n.fetch_add(1, RELAXED);
    u64 total = TOTAL.n.fetch_add(1, RELAXED) + 1;
    if (!SEEN[k].load(RELAXED) && !SEEN[k].exchange(true, RELAXED)) {
        log::info(TARGET, "emulating an instruction this CPU lacks", {{"instruction", name(kind)}, {"at", crash::locate(rip)}});
    }


    if (total % 1024 == 0) {
        u64 now = GetTickCount64(), last = LAST_SUMMARY.load(RELAXED);
        if (now - last >= 1000 && LAST_SUMMARY.compare_exchange_strong(last, now, RELAXED)) {
            summary(total);
        }
    }
}


int faults(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == STATUS_GUARD_PAGE_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                                                                       : EXCEPTION_CONTINUE_SEARCH;
}


bool load(void* out, u64 at, std::size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), n);
    } __except (faults(GetExceptionCode())) {
        return false;
    }
    return true;
}

bool store(u64 at, const void* in, std::size_t n) {
    __try {
        std::memcpy(reinterpret_cast<void*>(at), in, n);
    } __except (faults(GetExceptionCode())) {
        return false;
    }
    return true;
}

bool zero(u64 at, std::size_t n) {
    __try {
        std::memset(reinterpret_cast<void*>(at), 0, n);
    } __except (faults(GetExceptionCode())) {
        return false;
    }
    return true;
}


bool copy_in_page(u8* out, u64 from, std::size_t n) {
    __try {
        u64 first = from & ~7ull, last = (from + n - 1) & ~7ull;
        for (u64 q = first; q <= last; q += 8) {
            u64 v = *reinterpret_cast<const volatile u64*>(q);
            for (unsigned b = 0; b < 8; ++b) {
                u64 a = q + b;
                if (a >= from && a < from + n) {
                    out[a - from] = static_cast<u8>(v >> (8 * b));
                }
            }
        }
    } __except (faults(GetExceptionCode())) {
        return false;
    }
    return true;
}

bool readable_page(u64 at) {
    MEMORY_BASIC_INFORMATION m;
    if (VirtualQuery(reinterpret_cast<void*>(at), &m, sizeof m) == 0 || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    DWORD p = m.Protect & 0xff;
    return p != 0 && p != PAGE_EXECUTE;
}


std::size_t read_code(u64 at, u8* out, std::size_t n, u64 known) {
    std::size_t got = 0;
    while (got < n) {
        u64 p = at + got;
        std::size_t take = std::min<std::size_t>(n - got, 0x1000 - (p & 0xfff));
        if ((p >> 12) != (known >> 12) && !readable_page(p)) {
            break;
        }
        if (!copy_in_page(out + got, p, take)) {
            break;
        }
        got += take;
    }
    return got;
}


struct Span {
    u64 lo = 0, hi = 0;
};
SRWLOCK REGIONS_LOCK = SRWLOCK_INIT;
Span REGIONS[16];
unsigned REGIONS_NEXT = 0;

bool writable_protection(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) {
        return false;
    }
    p &= 0xff;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}


u64 writable_run(u64 lo, u64 hi, bool down) {
    if (hi <= lo || hi > (1ull << 47)) {
        return 0;
    }

    u64 at = down ? hi : lo;
    unsigned queries = 0;
    while (down ? at > lo : at < hi) {
        u64 probe = down ? at - 1 : at;
        bool hit = false;
        AcquireSRWLockShared(&REGIONS_LOCK);
        for (const Span& r : REGIONS) {
            if (probe >= r.lo && probe < r.hi) {
                at = down ? r.lo : r.hi;
                hit = true;
                break;
            }
        }
        ReleaseSRWLockShared(&REGIONS_LOCK);
        if (hit) {
            continue;
        }
        MEMORY_BASIC_INFORMATION m;
        if (++queries > 64 || VirtualQuery(reinterpret_cast<void*>(probe), &m, sizeof m) == 0 || m.State != MEM_COMMIT ||
            !writable_protection(m.Protect)) {
            break;
        }
        Span s{reinterpret_cast<u64>(m.BaseAddress), reinterpret_cast<u64>(m.BaseAddress) + m.RegionSize};
        AcquireSRWLockExclusive(&REGIONS_LOCK);
        REGIONS[REGIONS_NEXT++ % 16] = s;
        ReleaseSRWLockExclusive(&REGIONS_LOCK);
        at = down ? s.lo : s.hi;
    }
    return down ? hi - std::max(at, lo) : std::min(at, hi) - lo;
}

void forget_regions() {
    AcquireSRWLockExclusive(&REGIONS_LOCK);
    for (Span& r : REGIONS) {
        r = Span{};
    }
    ReleaseSRWLockExclusive(&REGIONS_LOCK);
}


u64 zero_range(u64 lo, u64 hi, bool down) {
    constexpr u64 CHUNK = 1ull << 16;
    u64 total = hi - lo, done = 0;
    while (done < total) {
        u64 n = std::min(CHUNK, total - done);
        u64 at = down ? hi - done - n : lo + done;
        if (zero(at, static_cast<std::size_t>(n))) {
            done += n;
            continue;
        }
        forget_regions();
        while (done < total) {
            u64 edge = down ? hi - done : lo + done;
            u64 page = down ? ((edge - 1) & ~0xfffull) : (edge & ~0xfffull);
            u64 from = down ? std::max(page, lo) : edge, to = down ? edge : std::min(page + 0x1000, hi);
            if (!zero(from, static_cast<std::size_t>(to - from))) {
                break;
            }
            done += to - from;
        }
        break;
    }
    return done;
}


struct Insn {
    u8 len = 0;
    u8 rex = 0;   
    u8 seg = 0;   
    bool p66 = false, pf2 = false, pf3 = false, lock = false, a32 = false;
    u8 map = 0;   
    u8 op = 0;
    u8 op_at = 0;   
    bool has_modrm = false;
    u8 modrm_at = 0, mod = 0, raw_reg = 0, reg = 0, rm = 0;   

    i8 base = -1, index = -1;
    u8 scale = 0;
    bool rip = false;
    i64 disp = 0;
    u8 imm_len = 0;
    i64 imm = 0;   
};

bool decode(const u8* c, std::size_t n, Insn& d) {
    d = Insn{};
    std::size_t i = 0;
    auto more = [&](std::size_t k) { return i + k <= n && i + k <= 15; };
    for (;; ++i) {
        if (!more(1)) {
            return false;
        }
        u8 b = c[i];
        if ((b & 0xf0) == 0x40) {
            d.rex = b;
            continue;
        }
        if (b == 0xf0) {
            d.lock = true;
        } else if (b == 0xf2) {
            d.pf2 = true;
        } else if (b == 0xf3) {
            d.pf3 = true;
        } else if (b == 0x66) {
            d.p66 = true;
        } else if (b == 0x67) {
            d.a32 = true;
        } else if (b == 0x64 || b == 0x65) {
            d.seg = b;
        } else if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e) {

        } else {
            break;
        }

        d.rex = 0;
    }
    u8 b = c[i++];
    if (b == 0x0f) {
        if (!more(1)) {
            return false;
        }
        b = c[i++];
        if (b == 0x38 || b == 0x3a) {
            if (!more(1)) {
                return false;
            }
            d.map = b == 0x38 ? 2 : 3;
            b = c[i++];
        } else {
            d.map = 1;
        }
    }
    d.op = b;
    d.op_at = static_cast<u8>(i - 1);
    bool modrm = false;
    unsigned imm = 0;
    switch (d.map) {
        case 0:
            switch (b) {
                case 0x01: case 0x03: case 0x29: case 0x2b: case 0x39: case 0x3b: case 0x85: case 0x89: case 0x8b: case 0x8d: case 0xff:
                    modrm = true;
                    break;
                case 0x83:
                    modrm = true;
                    imm = 1;
                    break;
                case 0x81: case 0xc7:
                    modrm = true;
                    imm = d.p66 ? 2 : 4;
                    break;
                case 0x05: case 0x2d: case 0x3d:
                    imm = d.p66 ? 2 : 4;
                    break;
                case 0x90:
                    break;
                default:
                    if (b >= 0x70 && b <= 0x7f) {
                        imm = 1;
                    } else if (b >= 0xb8 && b <= 0xbf) {
                        imm = (d.rex & 8) ? 8 : d.p66 ? 2 : 4;
                    } else {
                        return false;
                    }
            }
            break;
        case 1:
            if (b == 0x01 || b == 0x11 || b == 0x1f || b == 0x2b || b == 0x79 || b == 0xae || b == 0xc7) {
                modrm = true;
            } else if (b == 0x78) {
                modrm = true;
                imm = 2;   
            } else if (b >= 0x80 && b <= 0x8f) {
                imm = 4;
            } else {
                return false;
            }
            break;
        case 2:
            if ((b >= 0xc8 && b <= 0xcd) || b == 0xf6) {
                modrm = true;
            } else {
                return false;
            }
            break;
        default:
            if (b != 0xcc) {
                return false;
            }
            modrm = true;
            imm = 1;
            break;
    }
    if (modrm) {
        if (!more(1)) {
            return false;
        }
        d.has_modrm = true;
        u8 m = c[i];
        d.modrm_at = static_cast<u8>(i++);
        d.mod = m >> 6;
        d.raw_reg = (m >> 3) & 7;
        d.reg = static_cast<u8>(d.raw_reg | ((d.rex & 4) << 1));
        u8 rm = m & 7;
        if (d.mod == 3) {
            d.rm = static_cast<u8>(rm | ((d.rex & 1) << 3));
        } else {
            unsigned disp = 0;
            if (rm == 4) {
                if (!more(1)) {
                    return false;
                }
                u8 s = c[i++];
                d.scale = s >> 6;
                u8 index = static_cast<u8>(((s >> 3) & 7) | ((d.rex & 2) << 2));
                d.index = index == 4 ? -1 : static_cast<i8>(index);
                u8 base = s & 7;
                if (base == 5 && d.mod == 0) {
                    disp = 4;
                } else {
                    d.base = static_cast<i8>(base | ((d.rex & 1) << 3));
                }
            } else if (rm == 5 && d.mod == 0) {
                d.rip = true;
                disp = 4;
            } else {
                d.base = static_cast<i8>(rm | ((d.rex & 1) << 3));
            }
            if (d.mod == 1) {
                disp = 1;
            } else if (d.mod == 2) {
                disp = 4;
            }
            if (!more(disp)) {
                return false;
            }
            if (disp == 1) {
                d.disp = static_cast<i8>(c[i]);
            } else if (disp == 4) {
                i32 v;
                std::memcpy(&v, c + i, 4);
                d.disp = v;
            }
            i += disp;
        }
    }
    if (imm) {
        if (!more(imm)) {
            return false;
        }
        d.imm_len = static_cast<u8>(imm);
        if (imm == 1) {
            d.imm = static_cast<i8>(c[i]);
        } else if (imm == 2) {
            i16 v;
            std::memcpy(&v, c + i, 2);
            d.imm = v;
        } else if (imm == 4) {
            i32 v;
            std::memcpy(&v, c + i, 4);
            d.imm = v;
        } else {
            std::memcpy(&d.imm, c + i, 8);
        }
        i += imm;
    }
    d.len = static_cast<u8>(i);
    return true;
}


bool address(const Insn& d, const u64* regs, u64 next, u64& out) {
    u64 a = static_cast<u64>(d.disp);
    if (d.rip) {
        a += next;
    } else {
        if (d.base >= 0) {
            a += regs[d.base];
        }
        if (d.index >= 0) {
            a += regs[d.index] << d.scale;
        }
    }
    if (d.a32) {
        a = static_cast<u32>(a);
    }
    if (d.seg == 0x64) {
        return false;
    }
    if (d.seg == 0x65) {
        a += __readgsqword(0x30);
    }
    out = a;
    return true;
}


constexpr std::size_t SITES = 4096;
constexpr u64 TOMBSTONE = 1;
std::atomic<u64> SITE[SITES];
SRWLOCK PATCH_LOCK = SRWLOCK_INIT;

std::size_t slot(u64 at) { return static_cast<std::size_t>((at * 0x9e3779b97f4a7c15ull) >> 52); }

bool is_patched(u64 at) {
    for (std::size_t i = 0, s = slot(at); i < SITES; ++i, s = (s + 1) & (SITES - 1)) {
        u64 v = SITE[s].load(std::memory_order_acquire);
        if (v == at) {
            return true;
        }
        if (v == 0) {
            return false;
        }
    }
    return false;
}

bool add_site(u64 at) {
    for (std::size_t i = 0, s = slot(at); i < SITES / 2; ++i, s = (s + 1) & (SITES - 1)) {
        if (SITE[s].load(RELAXED) == 0) {
            SITE[s].store(at, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void drop_site(u64 at) {
    for (std::size_t i = 0, s = slot(at); i < SITES; ++i, s = (s + 1) & (SITES - 1)) {
        u64 v = SITE[s].load(RELAXED);
        if (v == at) {
            SITE[s].store(TOMBSTONE, std::memory_order_release);
            return;
        }
        if (v == 0) {
            return;
        }
    }
}


bool patch(Kind kind, u64 at, const u8* insn, std::size_t len, std::size_t first, const u8* repl, std::size_t count) {
    if (!patching()) {
        return false;
    }
    u64 lo = at + first;
    u64 window;
    std::size_t size;
    if ((lo & 7) + count <= 8) {
        window = lo & ~7ull;
        size = 8;
    } else if ((lo & 15) + count <= 16) {
        window = lo & ~15ull;
        size = 16;
    } else {
        return false;
    }
    MEMORY_BASIC_INFORMATION m;
    if (VirtualQuery(reinterpret_cast<void*>(window), &m, sizeof m) == 0 || m.Type != MEM_IMAGE || m.State != MEM_COMMIT) {
        return false;
    }
    bool ok = false;
    AcquireSRWLockExclusive(&PATCH_LOCK);


    bool listed = is_patched(at);
    if (listed || add_site(at)) {
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(window), size, PAGE_EXECUTE_READWRITE, &old)) {
            alignas(16) u8 now[16];
            for (std::size_t q = 0; q < size; q += 8) {
                u64 v = *reinterpret_cast<const volatile u64*>(window + q);
                std::memcpy(now + q, &v, 8);
            }
            bool same = true;
            for (std::size_t k = 0; k < size; ++k) {
                u64 a = window + k;
                if (a >= at && a < at + len && now[k] != insn[a - at]) {
                    same = false;
                }
            }
            if (same) {
                alignas(16) u8 want[16];
                std::memcpy(want, now, size);
                std::memcpy(want + (lo - window), repl, count);
                if (size == 8) {
                    LONG64 expect, value;
                    std::memcpy(&expect, now, 8);
                    std::memcpy(&value, want, 8);
                    ok = _InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(window), value, expect) == expect;
                } else {
                    alignas(16) LONG64 compare[2], value[2];
                    std::memcpy(compare, now, 16);
                    std::memcpy(value, want, 16);
                    ok = _InterlockedCompareExchange128(reinterpret_cast<volatile LONG64*>(window), value[1], value[0], compare) != 0;
                }
            }
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(window), size, old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(window), size);
        }
        if (!ok && !listed) {
            drop_site(at);
        }
    }
    ReleaseSRWLockExclusive(&PATCH_LOCK);
    if (ok) {
        PATCHED.n.fetch_add(1, RELAXED);
        unsigned k = static_cast<unsigned>(kind);
        if (!SEEN_PATCH[k].load(RELAXED) && !SEEN_PATCH[k].exchange(true, RELAXED)) {
            log::info(TARGET, "instruction rewritten in place into a host-native equivalent", {{"instruction", name(kind)}, {"at", crash::locate(at)}});
        }
    }
    return ok;
}


u32 parity(u64 r) { return (std::popcount(static_cast<u8>(r)) & 1) ? 0 : PF; }


u32 arith(u64 a, u64 b, u64 r, bool w64, bool sub) {
    u64 mask = w64 ? ~0ull : 0xffffffffull;
    unsigned top = w64 ? 63 : 31;
    a &= mask;
    b &= mask;
    r &= mask;
    u32 f = 0;
    if (sub ? a < b : r < a) f |= CF;
    if (((sub ? (a ^ b) & (a ^ r) : ~(a ^ b) & (a ^ r)) >> top) & 1) f |= OF;
    if (((a ^ b ^ r) >> 4) & 1) f |= AF;
    if (r == 0) f |= ZF;
    if ((r >> top) & 1) f |= SF;
    return f | parity(r);
}

u32 logic(u64 r, bool w64) {
    if (!w64) r = static_cast<u32>(r);
    u32 f = parity(r);
    if (r == 0) f |= ZF;
    if ((r >> (w64 ? 63 : 31)) & 1) f |= SF;
    return f;
}


bool condition(u8 cc, u32 f) {
    bool cf = f & CF, zf = f & ZF, sf = f & SF, of = f & OF, pf = f & PF;
    bool r = false;
    switch (cc >> 1) {
        case 0: r = of; break;
        case 1: r = cf; break;
        case 2: r = zf; break;
        case 3: r = cf || zf; break;
        case 4: r = sf; break;
        case 5: r = pf; break;
        case 6: r = sf != of; break;
        default: r = zf || sf != of; break;
    }
    return (cc & 1) ? !r : r;
}


enum class Uop : u8 { Nop, Line, Add, Sub, Cmp, Test, Inc, Dec, Mov, MovImm, Lea };

struct Op {
    Uop kind = Uop::Nop;
    bool w64 = true;
    i8 dst = -1, src = -1;   
    u64 imm = 0;             
    i8 index = -1;           
    u8 scale = 0;
    bool zero = false;       
    Insn mem;                
    u64 at = 0, next = 0;
};

constexpr int MAX_OPS = 24;

struct Loop {
    Op ops[MAX_OPS];
    int n = 0;
    int trap = 0;   
    u8 cc = 0;
    u64 top = 0, end = 0;
};

struct Regs {
    u64 r[16];
    u32 flags;
};

bool cache_line_op(const Insn& d, Op& op) {
    if (d.lock || d.seg == 0x64) {
        return false;
    }
    if (d.map == 1 && d.op == 0x01 && d.has_modrm && d.mod == 3 && d.raw_reg == 7 && (d.rm & 7) == 4 && !d.p66 && !d.pf2 && !d.pf3) {
        op.kind = Uop::Line;
        op.zero = true;
        op.mem = d;
        op.mem.base = 0;
        op.mem.index = -1;
        op.mem.disp = 0;
        op.mem.rip = false;
        return true;
    }
    if (d.map == 1 && d.op == 0xae && d.mod != 3 && (d.raw_reg == 6 || d.raw_reg == 7) && d.p66 && !d.pf2 && !d.pf3) {
        op.kind = Uop::Line;
        op.zero = false;
        op.mem = d;
        return true;
    }
    return false;
}

enum class Class { Fail, Op, Jump };

Class classify(const Insn& d, u64 at, Op& op, u8& cc, i64& rel) {
    op = Op{};
    op.at = at;
    op.next = at + d.len;
    if (cache_line_op(d, op)) {
        return Class::Op;
    }
    if (d.lock || d.pf2 || d.pf3) {
        return Class::Fail;
    }

    if ((d.map == 0 && d.op >= 0x70 && d.op <= 0x7f) || (d.map == 1 && d.op >= 0x80 && d.op <= 0x8f)) {
        if (d.p66 || d.a32 || (d.seg && d.seg != 0x2e && d.seg != 0x3e)) {
            return Class::Fail;
        }
        cc = d.op & 15;
        rel = d.imm;
        return Class::Jump;
    }

    if ((d.map == 0 && d.op == 0x90 && !(d.rex & 1)) || (d.map == 1 && d.op == 0x1f && d.raw_reg == 0)) {
        op.kind = Uop::Nop;
        return Class::Op;
    }
    if (d.p66 || d.map != 0) {
        return Class::Fail;
    }
    op.w64 = (d.rex & 8) != 0;
    auto two = [&](Uop kind, bool rm_is_dst) {
        if (d.mod != 3) {
            return Class::Fail;
        }
        op.kind = kind;
        op.dst = static_cast<i8>(rm_is_dst ? d.rm : d.reg);
        op.src = static_cast<i8>(rm_is_dst ? d.reg : d.rm);
        return Class::Op;
    };
    auto with_imm = [&](u8 ext) {
        if (d.mod != 3) {
            return Class::Fail;
        }
        op.dst = static_cast<i8>(d.rm);
        op.imm = static_cast<u64>(d.imm);
        switch (ext) {
            case 0: op.kind = Uop::Add; return Class::Op;
            case 5: op.kind = Uop::Sub; return Class::Op;
            case 7: op.kind = Uop::Cmp; return Class::Op;
            default: return Class::Fail;
        }
    };
    switch (d.op) {
        case 0x01: return two(Uop::Add, true);
        case 0x03: return two(Uop::Add, false);
        case 0x29: return two(Uop::Sub, true);
        case 0x2b: return two(Uop::Sub, false);
        case 0x39: return two(Uop::Cmp, true);
        case 0x3b: return two(Uop::Cmp, false);
        case 0x85: return two(Uop::Test, true);
        case 0x89: return two(Uop::Mov, true);
        case 0x8b: return two(Uop::Mov, false);
        case 0x05: case 0x2d: case 0x3d:
            op.kind = d.op == 0x05 ? Uop::Add : d.op == 0x2d ? Uop::Sub : Uop::Cmp;
            op.dst = 0;
            op.imm = static_cast<u64>(d.imm);
            return Class::Op;
        case 0x81: case 0x83: return with_imm(d.raw_reg);
        case 0xff:
            if (d.mod != 3 || d.raw_reg > 1) {
                return Class::Fail;
            }
            op.kind = d.raw_reg == 0 ? Uop::Inc : Uop::Dec;
            op.dst = static_cast<i8>(d.rm);
            return Class::Op;
        case 0xc7:
            if (d.mod != 3 || d.raw_reg != 0) {
                return Class::Fail;
            }
            op.kind = Uop::MovImm;
            op.dst = static_cast<i8>(d.rm);
            op.imm = op.w64 ? static_cast<u64>(d.imm) : static_cast<u32>(d.imm);
            return Class::Op;
        case 0x8d:
            if (d.mod == 3 || d.a32) {
                return Class::Fail;
            }
            op.kind = Uop::Lea;
            op.dst = static_cast<i8>(d.reg);
            op.src = d.rip ? static_cast<i8>(-1) : d.base;
            op.index = d.rip ? static_cast<i8>(-1) : d.index;
            op.scale = d.scale;
            op.imm = static_cast<u64>(d.disp) + (d.rip ? op.next : 0);
            return Class::Op;
        default:
            if (d.op >= 0xb8 && d.op <= 0xbf) {
                op.kind = Uop::MovImm;
                op.dst = static_cast<i8>((d.op & 7) | ((d.rex & 1) << 3));
                op.imm = op.w64 ? static_cast<u64>(d.imm) : static_cast<u32>(d.imm);
                return Class::Op;
            }
            return Class::Fail;
    }
}

bool build_loop(u64 rip, Loop& loop) {
    u8 code[160];
    std::size_t n = read_code(rip, code, sizeof code, rip);
    std::size_t at = 0;
    Op fwd[MAX_OPS];
    int nf = 0;
    for (;;) {
        Insn d;
        Op op;
        u8 cc = 0;
        i64 rel = 0;
        if (nf >= MAX_OPS || !decode(code + at, n - at, d)) {
            return false;
        }
        Class k = classify(d, rip + at, op, cc, rel);
        if (k == Class::Fail || (nf == 0 && (k != Class::Op || op.kind != Uop::Line))) {
            return false;
        }
        if (k == Class::Jump) {
            u64 end = rip + at + d.len;
            u64 target = end + static_cast<u64>(rel);
            if (target > rip || rip - target > 128) {
                return false;
            }
            loop.cc = cc;
            loop.top = target;
            loop.end = end;
            break;
        }
        fwd[nf++] = op;
        at += d.len;
    }
    int nb = 0;
    if (loop.top < rip) {
        std::size_t len = static_cast<std::size_t>(rip - loop.top);
        u8 back[128];
        if (read_code(loop.top, back, len, rip) != len) {
            return false;
        }
        std::size_t p = 0;
        while (p < len) {
            Insn d;
            Op op;
            u8 cc = 0;
            i64 rel = 0;
            if (nb + nf >= MAX_OPS || !decode(back + p, len - p, d) || classify(d, loop.top + p, op, cc, rel) != Class::Op) {
                return false;
            }
            loop.ops[nb++] = op;
            p += d.len;
        }
    }
    for (int k = 0; k < nf; ++k) {
        loop.ops[nb + k] = fwd[k];
    }
    loop.trap = nb;
    loop.n = nb + nf;
    return true;
}


int run_ops(const Loop& loop, Regs& s, int from, int to, bool zero_lines, u64& zeroed) {
    for (int i = from; i < to; ++i) {
        const Op& op = loop.ops[i];
        u64 b = op.src >= 0 ? s.r[op.src] : op.imm;
        u64& d = s.r[op.dst & 15];
        auto put = [&](u64 v) { d = op.w64 ? v : static_cast<u32>(v); };
        switch (op.kind) {
            case Uop::Nop: break;
            case Uop::Line:
                if (op.zero && zero_lines) {
                    u64 a = 0;
                    address(op.mem, s.r, op.next, a);
                    if (!zero(a & ~63ull, 64)) {
                        return i;
                    }
                    zeroed += 64;
                }
                break;
            case Uop::Add: {
                u64 r = d + b;
                s.flags = (s.flags & ~STATUS) | arith(d, b, r, op.w64, false);
                put(r);
                break;
            }
            case Uop::Sub: {
                u64 r = d - b;
                s.flags = (s.flags & ~STATUS) | arith(d, b, r, op.w64, true);
                put(r);
                break;
            }
            case Uop::Cmp: s.flags = (s.flags & ~STATUS) | arith(d, b, d - b, op.w64, true); break;
            case Uop::Test: s.flags = (s.flags & ~STATUS) | logic(d & b, op.w64); break;
            case Uop::Inc: {
                u64 r = d + 1;
                s.flags = (s.flags & ~(STATUS & ~CF)) | (arith(d, 1, r, op.w64, false) & ~CF);
                put(r);
                break;
            }
            case Uop::Dec: {
                u64 r = d - 1;
                s.flags = (s.flags & ~(STATUS & ~CF)) | (arith(d, 1, r, op.w64, true) & ~CF);
                put(r);
                break;
            }
            case Uop::Mov: put(b); break;
            case Uop::MovImm: d = op.imm; break;
            case Uop::Lea: {
                u64 v = op.imm;
                if (op.src >= 0) v += s.r[op.src];
                if (op.index >= 0) v += s.r[op.index] << op.scale;
                put(v);
                break;
            }
        }
    }
    return -1;
}

bool run_dry(const Loop& loop, Regs& s) {
    u64 ignored = 0;
    run_ops(loop, s, 0, loop.n, false, ignored);
    return condition(loop.cc, s.flags);
}


struct Sym {
    i8 src = -1;
    u64 off = 0;
    bool z = false;
};


struct Lin {
    u64 a0 = 0;
    i64 step = 0;
    unsigned bits = 64;
};

constexpr u64 KMAX = 1ull << 31;
constexpr i64 MAX_STEP = 1ll << 20;

struct Analysis {
    bool written[16] = {}, read[16] = {}, self[16] = {}, zreg[16] = {};
    u64 step[16] = {};
    int fkind = -1;   
    Sym fx, fy;
    bool fw64 = true;
    Sym line[MAX_OPS];
    u64 line_seg[MAX_OPS] = {};
    int lines = 0;
};

bool symbolic(const Loop& loop, const Regs& s2, Analysis& a) {
    for (int i = 0; i < loop.n; ++i) {
        const Op& op = loop.ops[i];
        if (op.kind == Uop::Add || op.kind == Uop::Sub || op.kind == Uop::Inc || op.kind == Uop::Dec || op.kind == Uop::Mov || op.kind == Uop::MovImm ||
            op.kind == Uop::Lea) {
            a.written[op.dst] = true;
        }
    }
    Sym e[16];
    for (int r = 0; r < 16; ++r) {
        e[r] = Sym{static_cast<i8>(r), 0, false};
    }
    auto use = [&](const Sym& s) {
        if (s.src >= 0) a.read[s.src] = true;
    };
    auto invariant = [&](int r, u64& v) {
        if (a.written[r]) return false;
        a.read[r] = true;
        v = s2.r[r];
        return true;
    };
    auto plus = [](const Sym& s, u64 c, bool w64, Sym& out) {
        if (w64 && s.z) return false;
        out = Sym{s.src, s.off + c, !w64};
        return true;
    };
    for (int i = 0; i < loop.n; ++i) {
        const Op& op = loop.ops[i];
        switch (op.kind) {
            case Uop::Nop: break;
            case Uop::Line:
                if (op.zero) {
                    Sym s = e[0];
                    use(s);
                    if (op.mem.a32) s.z = true;
                    a.line_seg[a.lines] = op.mem.seg == 0x65 ? __readgsqword(0x30) : 0;
                    a.line[a.lines++] = s;
                }
                break;
            case Uop::Add: case Uop::Sub: {
                u64 c = op.imm;
                if (op.src >= 0 && !invariant(op.src, c)) return false;
                Sym before = e[op.dst];
                use(before);
                a.fkind = static_cast<int>(op.kind);
                a.fx = before;
                a.fy = Sym{-1, op.kind == Uop::Sub ? c : 0 - c, false};
                a.fw64 = op.w64;
                if (!plus(before, op.kind == Uop::Sub ? 0 - c : c, op.w64, e[op.dst])) return false;
                break;
            }
            case Uop::Inc: case Uop::Dec: {
                Sym before = e[op.dst];
                use(before);
                a.fkind = static_cast<int>(op.kind);
                a.fx = before;
                a.fy = Sym{-1, op.kind == Uop::Inc ? ~0ull : 1, false};
                a.fw64 = op.w64;
                if (!plus(before, op.kind == Uop::Inc ? 1 : ~0ull, op.w64, e[op.dst])) return false;
                break;
            }
            case Uop::Cmp:
                a.fkind = static_cast<int>(op.kind);
                a.fx = e[op.dst];
                a.fy = op.src >= 0 ? e[op.src] : Sym{-1, op.imm, false};
                a.fw64 = op.w64;
                use(a.fx);
                use(a.fy);
                break;
            case Uop::Test:
                if (op.src != op.dst) return false;
                a.fkind = static_cast<int>(op.kind);
                a.fx = e[op.dst];
                a.fy = Sym{-1, 0, false};
                a.fw64 = op.w64;
                use(a.fx);
                break;
            case Uop::Mov: {
                Sym s = e[op.src];
                use(s);
                if (!op.w64) s.z = true;
                e[op.dst] = s;
                break;
            }
            case Uop::MovImm: e[op.dst] = Sym{-1, op.imm, false}; break;
            case Uop::Lea: {
                Sym base = op.src >= 0 ? e[op.src] : Sym{};
                use(base);
                u64 c = op.imm;
                if (op.index >= 0) {
                    u64 v = 0;
                    if (!invariant(op.index, v)) return false;
                    c += v << op.scale;
                }
                if (!plus(base, c, op.w64, e[op.dst])) return false;
                break;
            }
        }
    }
    for (int r = 0; r < 16; ++r) {
        if (a.written[r] && e[r].src == r) {
            a.self[r] = true;
            a.step[r] = e[r].off;
            a.zreg[r] = e[r].z;
        }


        if (a.read[r] && a.written[r] && !a.self[r]) return false;
    }
    return a.fkind >= 0;
}

bool linear(const Analysis& a, const Regs& s2, const Sym& e, bool w64, Lin& out) {
    if (e.src < 0) {
        bool b32 = e.z || !w64;
        out = Lin{b32 ? static_cast<u32>(e.off) : e.off, 0, b32 ? 32u : 64u};
        return true;
    }
    bool zs = a.self[e.src] && a.zreg[e.src];
    if (zs && !e.z && w64) return false;
    bool b32 = zs || e.z || !w64;
    u64 v = s2.r[e.src] + e.off;
    i64 st = a.self[e.src] ? static_cast<i64>(a.step[e.src]) : 0;
    if (b32) {
        v = static_cast<u32>(v);
        st = static_cast<i32>(static_cast<u32>(st));
    }
    if (st > MAX_STEP || st < -MAX_STEP) return false;
    out = Lin{v, st, b32 ? 32u : 64u};
    return true;
}


constexpr i64 CLAMP = 1ll << 62;

struct Big {
    bool neg;
    u64 mag;
};

Big math(u64 v, unsigned bits, bool sgn) {
    if (sgn) {
        i64 s = bits == 32 ? static_cast<i32>(static_cast<u32>(v)) : static_cast<i64>(v);
        return s < 0 ? Big{true, 0 - static_cast<u64>(s)} : Big{false, static_cast<u64>(s)};
    }
    return Big{false, bits == 32 ? static_cast<u32>(v) : v};
}

i64 difference(Big x, Big y) {
    Big ny{!y.neg && y.mag != 0, y.mag};
    bool neg;
    u64 m;
    if (x.neg == ny.neg) {
        m = x.mag + ny.mag;
        if (m < x.mag) m = ~0ull;
        neg = x.neg;
    } else if (x.mag >= ny.mag) {
        m = x.mag - ny.mag;
        neg = x.neg;
    } else {
        m = ny.mag - x.mag;
        neg = ny.neg;
    }
    i64 v = m >= static_cast<u64>(CLAMP) ? CLAMP : static_cast<i64>(m);
    return neg ? -v : v;
}

bool stays(const Lin& l, u64 t, bool sgn) {
    i64 delta = static_cast<i64>(t) * l.step;
    if (sgn) {
        i64 s = l.bits == 32 ? static_cast<i32>(static_cast<u32>(l.a0)) : static_cast<i64>(l.a0);
        i64 lo = l.bits == 32 ? INT32_MIN : INT64_MIN, hi = l.bits == 32 ? INT32_MAX : INT64_MAX;
        return delta >= 0 ? s <= hi - delta : s >= lo - delta;
    }
    u64 u = l.bits == 32 ? static_cast<u32>(l.a0) : l.a0;
    u64 max = l.bits == 32 ? 0xffffffffull : ~0ull;
    return delta >= 0 ? static_cast<u64>(delta) <= max - u : u >= static_cast<u64>(-delta);
}

enum class Exit { Ge0, Gt0, Lt0, Le0, Eq0, Ne0 };


bool solve(i64 d0, i64 s, Exit e, u64& t) {
    switch (e) {
        case Exit::Lt0: return solve(-d0, -s, Exit::Gt0, t);
        case Exit::Le0: return solve(-d0, -s, Exit::Ge0, t);
        case Exit::Ge0:
            if (d0 >= 0) {
                t = 0;
                return true;
            }
            if (s <= 0) return false;
            t = static_cast<u64>((-d0 + s - 1) / s);
            break;
        case Exit::Gt0:
            if (d0 > 0) {
                t = 0;
                return true;
            }
            if (s <= 0) return false;
            t = static_cast<u64>((1 - d0 + s - 1) / s);
            break;
        case Exit::Eq0: {
            if (d0 == 0) {
                t = 0;
                return true;
            }
            if (s == 0 || (d0 > 0) == (s > 0)) return false;
            u64 m = static_cast<u64>(d0 > 0 ? d0 : -d0), q = static_cast<u64>(s > 0 ? s : -s);
            if (m % q) return false;
            t = m / q;
            break;
        }
        case Exit::Ne0:
            if (d0 != 0) {
                t = 0;
                return true;
            }
            if (s == 0) return false;
            t = 1;
            break;
    }
    return t <= KMAX;
}


bool exit_rule(Uop kind, u8 cc, bool& sgn, Exit& ex) {
    sgn = false;
    if (cc == 4) {
        ex = Exit::Ne0;
        return true;
    }
    if (cc == 5) {
        ex = Exit::Eq0;
        return true;
    }
    if (kind == Uop::Add || kind == Uop::Inc) return false;
    bool carry = kind == Uop::Sub || kind == Uop::Cmp;
    switch (cc) {
        case 0x2: ex = Exit::Ge0; return carry;
        case 0x3: ex = Exit::Lt0; return carry;
        case 0x6: ex = Exit::Gt0; return carry;
        case 0x7: ex = Exit::Le0; return carry;
        case 0x8: sgn = true; ex = Exit::Ge0; return kind == Uop::Test;
        case 0x9: sgn = true; ex = Exit::Lt0; return kind == Uop::Test;
        case 0xc: sgn = true; ex = Exit::Ge0; return true;
        case 0xd: sgn = true; ex = Exit::Lt0; return true;
        case 0xe: sgn = true; ex = Exit::Gt0; return true;
        case 0xf: sgn = true; ex = Exit::Le0; return true;
        default: return false;
    }
}

Regs start_of(const Analysis& a, const Regs& s2, u64 t) {
    Regs r = s2;
    for (int i = 0; i < 16; ++i) {
        if (a.self[i]) {
            u64 v = s2.r[i] + t * a.step[i];
            r.r[i] = a.zreg[i] ? static_cast<u32>(v) : v;
        }
    }
    return r;
}


Regs exact_start(const Loop& loop, const Analysis& a, const Regs& s2, u64 t) {
    if (t == 0) return s2;
    Regs r = start_of(a, s2, t - 1);
    run_dry(loop, r);
    return r;
}


bool fuse(const Loop& loop, const Regs& s2, Regs& out, u64& rip, u64& zeroed) {
    Analysis a;
    if (!symbolic(loop, s2, a)) return false;
    bool sgn;
    Exit ex;
    if (!exit_rule(static_cast<Uop>(a.fkind), loop.cc, sgn, ex)) return false;
    Lin x, y;
    if (!linear(a, s2, a.fx, a.fw64, x) || !linear(a, s2, a.fy, a.fw64, y)) return false;
    unsigned w = a.fw64 ? 64 : 32;
    bool sx = sgn && x.bits == w, sy = sgn && y.bits == w;
    u64 t = 0;
    if (!solve(difference(math(x.a0, x.bits, sx), math(y.a0, y.bits, sy)), x.step - y.step, ex, t)) return false;
    if (!stays(x, t, sx) || !stays(y, t, sy)) return false;

    Lin lines[MAX_OPS];
    u64 lo = ~0ull, hi = 0;
    for (int i = 0; i < a.lines; ++i) {
        if (!linear(a, s2, a.line[i], true, lines[i]) || !stays(lines[i], t, false)) return false;
        u64 vt = lines[i].a0 + t * static_cast<u64>(lines[i].step);
        if (lines[i].bits == 32) vt = static_cast<u32>(vt);
        u64 first = lines[i].a0 + a.line_seg[i], last = vt + a.line_seg[i];
        if (first < a.line_seg[i] || last < a.line_seg[i]) return false;
        lo = std::min({lo, first & ~63ull, last & ~63ull});
        hi = std::max({hi, (first & ~63ull) + 64, (last & ~63ull) + 64});
    }

    Regs fin = start_of(a, s2, t);
    if (run_dry(loop, fin)) return false;
    if (t > 0) {
        Regs before = start_of(a, s2, t - 1);
        if (!run_dry(loop, before)) return false;
    }
    if (a.lines == 0) {
        out = fin;
        rip = loop.end;
        return true;
    }


    bool contiguous = true;
    i64 st = lines[0].step;
    u64 base = ~0ull;
    for (int i = 0; i < a.lines; ++i) {
        contiguous &= lines[i].step == st;
        base = std::min(base, (lines[i].a0 + a.line_seg[i]) & ~63ull);
    }
    i64 span = 64ll * a.lines;
    if (contiguous && (st == span || st == -span)) {
        unsigned seen = 0;
        for (int i = 0; i < a.lines; ++i) {
            u64 k = (((lines[i].a0 + a.line_seg[i]) & ~63ull) - base) / 64;
            if (k >= static_cast<u64>(a.lines) || (seen >> k) & 1) {
                contiguous = false;
                break;
            }
            seen |= 1u << k;
        }
    } else {
        contiguous = false;
    }
    u64 failed_at = ~0ull;
    if (contiguous) {


        bool down = st < 0;
        u64 run = writable_run(lo, hi, down);
        u64 whole = run / static_cast<u64>(span);
        if (whole == 0) return false;
        if (run < hi - lo) {
            failed_at = whole;
            if (down) {
                lo = hi - whole * static_cast<u64>(span);
            } else {
                hi = lo + whole * static_cast<u64>(span);
            }
        }
        u64 done = zero_range(lo, hi, down);
        zeroed += done;
        if (done < hi - lo) failed_at = done / static_cast<u64>(span);
    } else {
        if ((t + 1) * static_cast<u64>(a.lines) > (1ull << 20) || writable_run(lo, hi, false) != hi - lo) return false;
        u64 last = ~0ull;
        for (u64 k = 0; k <= t && failed_at == ~0ull; ++k) {
            for (int i = 0; i < a.lines; ++i) {
                u64 v = lines[i].a0 + k * static_cast<u64>(lines[i].step);
                if (lines[i].bits == 32) v = static_cast<u32>(v);
                u64 line = (v + a.line_seg[i]) & ~63ull;
                if (line == last) continue;
                if (!zero(line, 64)) {
                    failed_at = k;
                    break;
                }
                zeroed += 64;
                last = line;
            }
        }
    }
    if (failed_at != ~0ull) {
        out = exact_start(loop, a, s2, failed_at);
        rip = loop.top;
        return true;
    }
    out = fin;
    rip = loop.end;
    return true;
}

void put_state(CONTEXT& c, const Regs& s, u64 rip) {
    std::memcpy(&c.Rax, s.r, sizeof s.r);
    c.EFlags = (c.EFlags & ~STATUS) | (s.flags & STATUS);
    c.Rip = rip;
}


bool try_loop(CONTEXT& c, Outcome& outcome, u64& zeroed, bool& looped) {
    looped = false;
    if (!LOOPS.load(RELAXED)) return false;
    Loop loop;
    if (!build_loop(c.Rip, loop)) return false;
    Regs s;
    std::memcpy(s.r, &c.Rax, sizeof s.r);
    s.flags = c.EFlags;
    outcome = Outcome::Done;

    int stop = run_ops(loop, s, loop.trap, loop.n, true, zeroed);
    if (stop == loop.trap) {
        outcome = Outcome::Refused;
        return true;
    }
    if (stop >= 0) {
        put_state(c, s, loop.ops[stop].at);
        return true;
    }
    if (!condition(loop.cc, s.flags)) {
        put_state(c, s, loop.end);
        return true;
    }
    looped = true;


    stop = run_ops(loop, s, 0, loop.n, true, zeroed);
    if (stop >= 0) {
        put_state(c, s, loop.ops[stop].at);
        return true;
    }
    if (!condition(loop.cc, s.flags)) {
        put_state(c, s, loop.end);
        return true;
    }
    Regs fin;
    u64 rip = 0;
    if (fuse(loop, s, fin, rip, zeroed)) {
        put_state(c, fin, rip);
    } else {
        put_state(c, s, loop.top);
    }
    return true;
}


u64 gs_base() { return __readgsqword(0x30); }

M128A* xmm(CONTEXT& c) { return c.FltSave.XmmRegisters; }


Outcome cache_line(CONTEXT& c, const Insn& d, const u8* code, Kind kind) {
    u64 rip = c.Rip;
    u64 zeroed = 0;
    bool looped = false;
    Outcome o = Outcome::Done;
    bool clzero = kind == Kind::Clzero;
    if (try_loop(c, o, zeroed, looped)) {
        if (o == Outcome::Refused) return o;
        if (looped) kind = clzero ? Kind::ClzeroLoop : Kind::CacheLoop;
    } else {
        if (clzero) {
            u64 a = d.a32 ? static_cast<u32>(c.Rax) : c.Rax;
            if (d.seg == 0x65) a += gs_base();
            if (!zero(a & ~63ull, 64)) return Outcome::Refused;
            zeroed = 64;
        }
        c.Rip = rip + d.len;
    }
    if (zeroed) ZEROED.n.fetch_add(zeroed, RELAXED);
    note(kind, rip);
    if (!clzero) {

        u8 repl[2] = {0x1f, static_cast<u8>(code[d.modrm_at] & ~0x38)};
        patch(d.raw_reg == 6 ? Kind::Clwb : Kind::Clflushopt, rip, code, d.len, d.op_at, repl, 2);
    }
    return Outcome::Done;
}

u64 nominal_hz() {
    static const u64 hz = [] {
        DWORD mhz = 0, size = sizeof mhz;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &size) !=
                ERROR_SUCCESS ||
            mhz < 500 || mhz > 10000) {
            mhz = 3600;
        }
        return static_cast<u64>(mhz) * 1000000;
    }();
    return hz;
}

u64 qpc_hz() {
    static const u64 hz = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<u64>(f.QuadPart);
    }();
    return hz;
}


u64 perf_counter() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    u64 high = 0;
    u64 low = _umul128(static_cast<u64>(now.QuadPart), nominal_hz(), &high);
    u64 rem = 0;
    return _udiv128(high, low, qpc_hz(), &rem);
}

bool random_bytes(void* out, std::size_t n) {
    if (HOST.rdrand) {
        u64 v = 0;
        for (int tries = 0; tries < 16; ++tries) {
            if (_rdrand64_step(reinterpret_cast<unsigned long long*>(&v))) {
                std::memcpy(out, &v, std::min<std::size_t>(n, 8));
                return true;
            }
        }
    }
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}


u64 field_mask(unsigned len) { return len == 0 ? ~0ull : (1ull << len) - 1; }

u64 extract(u64 v, unsigned len, unsigned at) { return (v >> at) & field_mask(len); }

u64 insert(u64 dst, u64 src, unsigned len, unsigned at) {
    u64 m = field_mask(len) << at;
    return (dst & ~m) | ((src << at) & m);
}

struct V4 {
    u32 w[4];   
};

V4 get(const M128A& x) {
    V4 v;
    std::memcpy(v.w, &x, 16);
    return v;
}

void put(M128A& x, const V4& v) { std::memcpy(&x, v.w, 16); }

u32 rol(u32 v, int n) { return std::rotl(v, n); }
u32 ror(u32 v, int n) { return std::rotr(v, n); }

V4 sha1rnds4(V4 s1, V4 s2, unsigned func) {
    static constexpr u32 K[4] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};
    auto f = [func](u32 b, u32 c, u32 d) -> u32 {
        switch (func & 3) {
            case 0: return (b & c) ^ (~b & d);
            case 2: return (b & c) ^ (b & d) ^ (c & d);
            default: return b ^ c ^ d;
        }
    };
    u32 a = s1.w[3], b = s1.w[2], c = s1.w[1], d = s1.w[0], e = 0;
    u32 w[4] = {s2.w[3], s2.w[2], s2.w[1], s2.w[0]};
    for (int i = 0; i < 4; ++i) {
        u32 na = f(b, c, d) + rol(a, 5) + w[i] + e + K[func & 3];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = na;
    }
    return V4{{d, c, b, a}};
}

V4 sha1nexte(V4 s1, V4 s2) {
    V4 r = s2;
    r.w[3] = s2.w[3] + rol(s1.w[3], 30);
    return r;
}

V4 sha1msg1(V4 s1, V4 s2) {
    u32 w0 = s1.w[3], w1 = s1.w[2], w2 = s1.w[1], w3 = s1.w[0], w4 = s2.w[3], w5 = s2.w[2];
    return V4{{w5 ^ w3, w4 ^ w2, w3 ^ w1, w2 ^ w0}};
}

V4 sha1msg2(V4 s1, V4 s2) {
    u32 w13 = s2.w[2], w14 = s2.w[1], w15 = s2.w[0];
    u32 w16 = rol(s1.w[3] ^ w13, 1);
    u32 w17 = rol(s1.w[2] ^ w14, 1);
    u32 w18 = rol(s1.w[1] ^ w15, 1);
    u32 w19 = rol(s1.w[0] ^ w16, 1);
    return V4{{w19, w18, w17, w16}};
}

u32 big_sigma0(u32 a) { return ror(a, 2) ^ ror(a, 13) ^ ror(a, 22); }
u32 big_sigma1(u32 e) { return ror(e, 6) ^ ror(e, 11) ^ ror(e, 25); }
u32 small_sigma0(u32 w) { return ror(w, 7) ^ ror(w, 18) ^ (w >> 3); }
u32 small_sigma1(u32 w) { return ror(w, 17) ^ ror(w, 19) ^ (w >> 10); }

V4 sha256rnds2(V4 s1, V4 s2, V4 k) {
    u32 a = s2.w[3], b = s2.w[2], c = s1.w[3], d = s1.w[2], e = s2.w[1], f = s2.w[0], g = s1.w[1], h = s1.w[0];
    for (int i = 0; i < 2; ++i) {
        u32 ch = (e & f) ^ (~e & g);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t = ch + big_sigma1(e) + k.w[i] + h;
        u32 na = t + maj + big_sigma0(a);
        u32 ne = t + d;
        h = g;
        g = f;
        f = e;
        e = ne;
        d = c;
        c = b;
        b = a;
        a = na;
    }
    return V4{{f, e, b, a}};
}

V4 sha256msg1(V4 s1, V4 s2) {
    u32 w4 = s2.w[0], w3 = s1.w[3], w2 = s1.w[2], w1 = s1.w[1], w0 = s1.w[0];
    return V4{{w0 + small_sigma0(w1), w1 + small_sigma0(w2), w2 + small_sigma0(w3), w3 + small_sigma0(w4)}};
}

V4 sha256msg2(V4 s1, V4 s2) {
    u32 w14 = s2.w[2], w15 = s2.w[3];
    u32 w16 = s1.w[0] + small_sigma1(w14);
    u32 w17 = s1.w[1] + small_sigma1(w15);
    u32 w18 = s1.w[2] + small_sigma1(w16);
    u32 w19 = s1.w[3] + small_sigma1(w17);
    return V4{{w16, w17, w18, w19}};
}

Outcome dispatch(CONTEXT& c, u32 miss) {
    u64 rip = c.Rip;
    u8 code[16] = {};
    std::size_t n = read_code(rip, code, 15, rip);
    Insn d;
    if (n == 0 || !decode(code, n, d) || d.lock) {
        return Outcome::Pass;
    }
    u64 next = rip + d.len;
    u64* regs = &c.Rax;
    bool plain = !d.p66 && !d.pf2 && !d.pf3;
    auto has = [miss](u32 f) { return (miss & f) != 0; };
    auto done = [&](Kind kind) {
        c.Rip = next;
        note(kind, rip);
        return Outcome::Done;
    };
    if (d.map == 1) {
        switch (d.op) {
            case 0x01: {
                if (!plain || d.mod != 3) return Outcome::Pass;
                u8 m = code[d.modrm_at];
                if (m == 0xfc && has(CLZERO)) {
                    if (d.seg == 0x64) return Outcome::Pass;
                    return cache_line(c, d, code, Kind::Clzero);
                }
                if (m == 0xfa && has(MONITORX)) {

                    Outcome o = done(Kind::Monitorx);
                    u8 repl[2] = {0x1f, 0x00};
                    patch(Kind::Monitorx, rip, code, d.len, d.op_at, repl, 2);
                    return o;
                }
                if (m == 0xfb && has(MONITORX)) {


                    if (!(c.Rcx & 2) || static_cast<u32>(c.Rbx) > 50000) {
                        SwitchToThread();
                    }
                    Outcome o = done(Kind::Mwaitx);
                    u8 repl[3] = {0xf3, 0x90, 0x90};
                    patch(Kind::Mwaitx, rip, code, d.len, static_cast<std::size_t>(d.op_at) - 1, repl, 3);
                    return o;
                }
                if (m == 0xfd && has(RDPRU)) {


                    u32 which = static_cast<u32>(c.Rcx);
                    u64 v = which <= 1 ? perf_counter() : 0;
                    c.Rax = static_cast<u32>(v);
                    c.Rdx = v >> 32;
                    return done(Kind::Rdpru);
                }
                return Outcome::Pass;
            }
            case 0x2b: {

                if (!has(SSE4A) || d.mod == 3 || d.p66 || d.pf2 == d.pf3) return Outcome::Pass;
                u64 at = 0;
                if (!address(d, regs, next, at)) return Outcome::Pass;
                std::size_t size = d.pf3 ? 4 : 8;
                if (!store(at, &xmm(c)[d.reg].Low, size)) return Outcome::Refused;
                Kind kind = d.pf3 ? Kind::Movntss : Kind::Movntsd;
                Outcome o = done(kind);
                u8 repl = 0x11;   
                patch(kind, rip, code, d.len, d.op_at, &repl, 1);
                return o;
            }
            case 0x78: case 0x79: {
                if (!has(SSE4A) || d.mod != 3 || d.pf3 || d.p66 == d.pf2) return Outcome::Pass;
                M128A* x = xmm(c);
                bool imm = d.op == 0x78;
                if (d.p66) {


                    if (imm && d.raw_reg != 0) return Outcome::Pass;
                    unsigned len, at;
                    M128A& dst = imm ? x[d.rm] : x[d.reg];
                    if (imm) {
                        len = static_cast<unsigned>(d.imm) & 63;
                        at = static_cast<unsigned>(d.imm >> 8) & 63;
                    } else {
                        len = x[d.rm].Low & 63;
                        at = (x[d.rm].Low >> 8) & 63;
                    }


                    dst.Low = extract(dst.Low, len, at);
                    dst.High = 0;
                    return done(Kind::Extrq);
                }


                unsigned len, at;
                if (imm) {
                    len = static_cast<unsigned>(d.imm) & 63;
                    at = static_cast<unsigned>(d.imm >> 8) & 63;
                } else {
                    u64 high = static_cast<u64>(x[d.rm].High);
                    len = high & 63;
                    at = (high >> 8) & 63;
                }
                x[d.reg].Low = insert(x[d.reg].Low, x[d.rm].Low, len, at);
                x[d.reg].High = 0;
                return done(Kind::Insertq);
            }
            case 0xae: {
                if (d.mod == 3 || !d.p66 || d.pf2 || d.pf3) return Outcome::Pass;
                if (d.raw_reg == 6 && has(CLWB)) return cache_line(c, d, code, Kind::Clwb);
                if (d.raw_reg == 7 && has(CLFLUSHOPT)) return cache_line(c, d, code, Kind::Clflushopt);
                return Outcome::Pass;
            }
            case 0xc7: {
                if (d.mod != 3 || d.raw_reg != 7 || d.pf2) return Outcome::Pass;
                if (d.pf3) {


                    if (d.p66 || !has(RDPID)) return Outcome::Pass;
                    PROCESSOR_NUMBER p;
                    GetCurrentProcessorNumberEx(&p);
                    regs[d.rm] = (static_cast<u64>(p.Group) << 6) | p.Number;
                    return done(Kind::Rdpid);
                }
                if (!has(RDSEED)) return Outcome::Pass;
                u64 v = 0;
                if (!random_bytes(&v, sizeof v)) return Outcome::Pass;
                u64& r = regs[d.rm];
                if (d.rex & 8) {
                    r = v;
                } else if (d.p66) {
                    r = (r & ~0xffffull) | (v & 0xffff);
                } else {
                    r = static_cast<u32>(v);
                }
                c.EFlags = (c.EFlags & ~STATUS) | CF;
                return done(Kind::Rdseed);
            }
            default: return Outcome::Pass;
        }
    }
    if (d.map == 2 && d.op == 0xf6) {

        if (!has(ADX) || d.pf2 || d.p66 == d.pf3) return Outcome::Pass;
        bool w64 = (d.rex & 8) != 0;
        u64 src = 0;
        if (d.mod == 3) {
            src = regs[d.rm];
        } else {
            u64 at = 0;
            if (!address(d, regs, next, at)) return Outcome::Pass;
            if (!load(&src, at, w64 ? 8 : 4)) return Outcome::Refused;
        }
        u32 flag = d.p66 ? CF : OF;
        unsigned char carry = (c.EFlags & flag) ? 1 : 0;
        u64& dst = regs[d.reg];
        if (w64) {
            unsigned long long r = 0;
            carry = _addcarry_u64(carry, dst, src, &r);
            dst = r;
        } else {
            unsigned int r = 0;
            carry = _addcarry_u32(carry, static_cast<u32>(dst), static_cast<u32>(src), &r);
            dst = r;
        }
        c.EFlags = (c.EFlags & ~flag) | (carry ? flag : 0);
        return done(d.p66 ? Kind::Adcx : Kind::Adox);
    }
    if ((d.map == 2 && d.op >= 0xc8 && d.op <= 0xcd) || (d.map == 3 && d.op == 0xcc)) {
        if (!has(SHA) || !plain) return Outcome::Pass;
        M128A* x = xmm(c);
        V4 s1 = get(x[d.reg]), s2;
        if (d.mod == 3) {
            s2 = get(x[d.rm]);
        } else {

            u64 at = 0;
            if (!address(d, regs, next, at)) return Outcome::Pass;
            if ((at & 15) || !load(s2.w, at, 16)) return Outcome::Refused;
        }
        V4 r;
        if (d.map == 3) {
            r = sha1rnds4(s1, s2, static_cast<unsigned>(d.imm) & 3);
        } else {
            switch (d.op) {
                case 0xc8: r = sha1nexte(s1, s2); break;
                case 0xc9: r = sha1msg1(s1, s2); break;
                case 0xca: r = sha1msg2(s1, s2); break;
                case 0xcb: r = sha256rnds2(s1, s2, get(x[0])); break;
                case 0xcc: r = sha256msg1(s1, s2); break;
                default: r = sha256msg2(s1, s2); break;
            }
        }
        put(x[d.reg], r);
        return done(Kind::Sha);
    }
    return Outcome::Pass;
}


bool is_replacement(u64 at) {
    u8 code[16] = {};
    std::size_t n = read_code(at, code, 15, at);
    Insn d;
    if (n == 0 || !decode(code, n, d) || d.lock) {
        return false;
    }
    if (d.map == 0) {
        return d.op == 0x90 && d.pf3;
    }
    return d.map == 1 && ((d.op == 0x1f && d.raw_reg == 0) || (d.op == 0x11 && d.mod != 3 && d.pf2 != d.pf3 && !d.p66));
}

}   

Outcome emulate(CONTEXT& context) {
    u32 miss = MISSING.load(RELAXED);
    if (miss == 0) {
        return Outcome::Pass;
    }
    DWORD error = GetLastError();
    Outcome o = dispatch(context, miss);
    if (o == Outcome::Pass && is_patched(context.Rip) && is_replacement(context.Rip)) {

        o = Outcome::Done;
    }
    SetLastError(error);
    return o;
}

std::uint32_t host_features() { return HOST.features; }
std::uint32_t missing() { return MISSING.load(RELAXED); }
void set_missing(std::uint32_t features) { MISSING.store(features & ALL_FEATURES, RELAXED); }

bool patching() {
    int v = PATCH.load(RELAXED);
    if (v < 0) {
        v = env::is(L"VICTORY_INSN_PATCH", L"0") ? 0 : 1;
        PATCH.store(v, RELAXED);
    }
    return v != 0;
}
void set_patching(bool on) { PATCH.store(on ? 1 : 0, RELAXED); }
void set_loop_fusion(bool on) { LOOPS.store(on, RELAXED); }

const char* name(Kind kind) {
    switch (kind) {
        case Kind::Clzero: return "clzero";
        case Kind::ClzeroLoop: return "clzero loop";
        case Kind::CacheLoop: return "clwb/clflushopt loop";
        case Kind::Monitorx: return "monitorx";
        case Kind::Mwaitx: return "mwaitx";
        case Kind::Rdpru: return "rdpru";
        case Kind::Extrq: return "extrq";
        case Kind::Insertq: return "insertq";
        case Kind::Movntss: return "movntss";
        case Kind::Movntsd: return "movntsd";
        case Kind::Rdpid: return "rdpid";
        case Kind::Clwb: return "clwb";
        case Kind::Clflushopt: return "clflushopt";
        case Kind::Sha: return "sha";
        case Kind::Adcx: return "adcx";
        case Kind::Adox: return "adox";
        case Kind::Rdseed: return "rdseed";
        default: return "?";
    }
}

Stats stats() {
    Stats s{};
    for (unsigned k = 0; k < KINDS; ++k) {
        s.by_kind[k] = COUNTS[k].n.load(RELAXED);
    }
    s.total = TOTAL.n.load(RELAXED);
    s.patched = PATCHED.n.load(RELAXED);
    s.clzero_bytes = ZEROED.n.load(RELAXED);
    return s;
}

}   
