// SPDX-License-Identifier: GPL-3.0-or-later
// Emulation of the console CPU's user-mode instructions that the host CPU
// lacks (the console is AMD Zen 2; the host may be any x86-64 from Intel
// Haswell or AMD Zen on). Port-only: an independent implementation written
// for this port from the AMD64 Architecture Programmer's Manual (vol. 3, and
// vol. 4 for SSE4a) and the Intel SDM (vol. 2). It is not derived from XCL
// nor from the reference's `console_insn*.rs` (which state they are ported
// from XCL, GPL-2.0-only); see NOTICE.md.
//
// `crash.cpp`'s vectored handler calls `emulate` for every
// EXCEPTION_ILLEGAL_INSTRUCTION, before the reference's own emulation. It
// handles, each only where CPUID says the host lacks it:
//
//   clzero (and whole `clzero` loops at once), monitorx, mwaitx, rdpru,
//   SSE4a (extrq, insertq, movntss, movntsd), rdpid, clwb, clflushopt,
//   SHA (sha1rnds4, sha1nexte, sha1msg1, sha1msg2, sha256rnds2,
//   sha256msg1, sha256msg2), ADX (adcx, adox) and rdseed.
//
// Where a same-length host-native equivalent exists (clwb, clflushopt and
// monitorx: a NOP; mwaitx: `pause; nop`; movntss/movntsd: movss/movsd) the
// site in the title's image is rewritten once, atomically, so that it no
// longer traps (`VICTORY_INSN_PATCH=0` turns this off).
#pragma once

#include <windows.h>

#include <cstdint>

namespace danzi::loader::emulate {

enum class Outcome {


    Pass,


    Done,


    Refused,
};


Outcome emulate(CONTEXT& context);


enum Feature : std::uint32_t {
    CLZERO = 1u << 0,
    SSE4A = 1u << 1,
    MONITORX = 1u << 2,
    RDPRU = 1u << 3,
    RDPID = 1u << 4,
    CLWB = 1u << 5,
    CLFLUSHOPT = 1u << 6,
    SHA = 1u << 7,
    ADX = 1u << 8,
    RDSEED = 1u << 9,
    ALL_FEATURES = (1u << 10) - 1,
};

std::uint32_t host_features();


std::uint32_t missing();

void set_missing(std::uint32_t features);


bool patching();
void set_patching(bool on);


void set_loop_fusion(bool on);

enum class Kind : unsigned {
    Clzero,
    ClzeroLoop,
    CacheLoop,
    Monitorx,
    Mwaitx,
    Rdpru,
    Extrq,
    Insertq,
    Movntss,
    Movntsd,
    Rdpid,
    Clwb,
    Clflushopt,
    Sha,
    Adcx,
    Adox,
    Rdseed,
    Count,
};
const char* name(Kind kind);

struct Stats {
    std::uint64_t by_kind[static_cast<unsigned>(Kind::Count)];
    std::uint64_t total;

    std::uint64_t patched;

    std::uint64_t clzero_bytes;
};
Stats stats();

}   
