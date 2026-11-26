// SPDX-License-Identifier: GPL-3.0-or-later
// `emulate-check [bench]`: the emulation of console instructions
// (src/loader/emulate.cpp) against the instructions themselves, executed
// natively by a CPU that has them all (an AMD Zen 4/5 does). Port-only.
//
// Each case is one instruction form with random registers, flags, XMM
// registers and memory (fixed seeds): a generated stub loads the state,
// runs the bytes natively and stores the state back; then the same bytes,
// in the same place, are given to `emulate` with the same state in a
// CONTEXT (every instruction set treated as missing). Registers, XMM
// registers, the status flags, the memory and Rip must agree. Loops of
// `clzero` run whole natively against the emulation (finished natively
// from wherever the emulation stopped). The handler path is run through a
// real exception: a `ud2` before the instruction, moved past by a first
// vectored handler, so that `crash.cpp`'s handler emulates (and rewrites)
// the instruction; the rewritten bytes then run natively.
//
// `bench` measures the emulation's cost per instruction, the `clzero` loop
// at once against line by line, and a trap's round trip.
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "loader/crash.hpp"
#include "loader/emulate.hpp"

namespace em = danzi::loader::emulate;

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using Rng = std::mt19937_64;

constexpr u32 CF = 1, PF = 4, AF = 0x10, ZF = 0x40, SF = 0x80, OF = 0x800;
constexpr u32 STATUS = CF | PF | AF | ZF | SF | OF;

struct alignas(16) State {
    u64 r[16];
    u64 flags;
    u64 pad;
    M128A x[16];
};
static_assert(offsetof(State, flags) == 128 && offsetof(State, x) == 144);


alignas(16) volatile u64 SCRATCH[4];


constexpr std::size_t LOW_SIZE = 16u << 20;
u8* LOW = nullptr;   
u8* STUB = nullptr;
constexpr std::size_t STUB_SIZE = 0x8000;
u8* BAILOUT = nullptr;
u8* DATA = nullptr;
constexpr std::size_t DATA_SIZE = 0x4000;
u8* LOOPBUF = nullptr;
constexpr std::size_t LOOPBUF_SIZE = 12u << 20;


#pragma section(".emut", read, execute)
__declspec(allocate(".emut")) const unsigned char IMAGE_AREA[8192] = {0xcc};
u8* IMG = nullptr;

bool alloc_low() {
    for (u64 at = 0x10000000; at < 0x70000000; at += 0x1000000) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(at), LOW_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) {
            LOW = static_cast<u8*>(p);
            STUB = LOW;
            BAILOUT = LOW + STUB_SIZE - 0x400;
            DATA = LOW + 0x10000;
            LOOPBUF = LOW + (2u << 20);
            return true;
        }
    }
    return false;
}


struct Asm {
    std::vector<u8> b;
    void put(std::initializer_list<u8> l) { b.insert(b.end(), l); }
    void d32(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void d64(u64 v) {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void modrm32(int reg, int base, i32 disp) {
        b.push_back(static_cast<u8>(0x80 | ((reg & 7) << 3) | (base & 7)));
        if ((base & 7) == 4) b.push_back(0x24);
        d32(static_cast<u32>(disp));
    }

    void load(int r, int base, i32 disp) {
        b.push_back(static_cast<u8>(0x48 | ((r >> 3) << 2) | (base >> 3)));
        b.push_back(0x8b);
        modrm32(r, base, disp);
    }
    void store(int base, i32 disp, int r) {
        b.push_back(static_cast<u8>(0x48 | ((r >> 3) << 2) | (base >> 3)));
        b.push_back(0x89);
        modrm32(r, base, disp);
    }
    void movdqu(bool to_mem, int x, int base, i32 disp) {
        b.push_back(0xf3);
        if (x >= 8 || base >= 8) b.push_back(static_cast<u8>(0x40 | ((x >> 3) << 2) | (base >> 3)));
        put({0x0f, static_cast<u8>(to_mem ? 0x7f : 0x6f)});
        modrm32(x, base, disp);
    }
    void rax_to(volatile u64* at) {
        put({0x48, 0xa3});
        d64(reinterpret_cast<u64>(at));
    }
    void rax_from(volatile u64* at) {
        put({0x48, 0xa1});
        d64(reinterpret_cast<u64>(at));
    }
    void epilogue_restore() {
        for (int i = 0; i < 10; ++i) movdqu(false, 6 + i, 4, 16 * i);
        put({0x48, 0x81, 0xc4});
        d32(0xa8);
        put({0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5e, 0x5f, 0x5d, 0x5b, 0xc3});
    }
};

struct Stub {
    u8* base = nullptr;
    std::size_t code = 0;    
    std::size_t entry = 0;   
};


Stub build_stub(u8* mem, const std::vector<u8>& code, bool load_rsp, bool trap) {
    Asm a;
    a.put({0x53, 0x55, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
    a.put({0x48, 0x81, 0xec});
    a.d32(0xa8);
    for (int i = 0; i < 10; ++i) a.movdqu(true, 6 + i, 4, 16 * i);
    a.put({0x48, 0x89, 0xe0});
    a.rax_to(&SCRATCH[0]);
    a.put({0x48, 0x89, 0xc8});
    a.rax_to(&SCRATCH[1]);
    for (int i = 0; i < 16; ++i) a.movdqu(false, i, 1, 144 + 16 * i);
    a.put({0xff, 0xb1});
    a.d32(128);
    a.put({0x9d});
    for (int r = 0; r < 16; ++r) {
        if (r == 1 || (r == 4 && !load_rsp)) continue;
        a.load(r, 1, 8 * r);
    }
    a.load(1, 1, 8);
    a.put({0xff, 0x25, 0, 0, 0, 0});
    Stub s;
    s.base = mem;
    s.entry = a.b.size();
    a.d64(0);
    if (trap) a.put({0x0f, 0x0b});
    s.code = a.b.size();
    a.b.insert(a.b.end(), code.begin(), code.end());
    a.rax_to(&SCRATCH[2]);
    a.rax_from(&SCRATCH[1]);
    for (int r = 1; r < 16; ++r) a.store(0, 8 * r, r);
    a.put({0x48, 0x89, 0xc1});
    a.rax_from(&SCRATCH[2]);
    a.store(1, 0, 0);
    a.rax_from(&SCRATCH[0]);
    a.put({0x48, 0x89, 0xc4, 0x9c, 0x58});
    a.store(1, 128, 0);
    for (int i = 0; i < 16; ++i) a.movdqu(true, i, 1, 144 + 16 * i);
    a.epilogue_restore();
    std::memcpy(mem, a.b.data(), a.b.size());
    u64 entry = reinterpret_cast<u64>(mem) + (trap ? s.code - 2 : s.code);
    std::memcpy(mem + s.entry, &entry, 8);
    FlushInstructionCache(GetCurrentProcess(), mem, a.b.size());
    return s;
}

void set_entry(const Stub& s, u64 entry) {
    std::memcpy(s.base + s.entry, &entry, 8);
    FlushInstructionCache(GetCurrentProcess(), s.base + s.entry, 8);
}

void build_bailout() {
    Asm a;
    a.rax_from(&SCRATCH[0]);
    a.put({0x48, 0x89, 0xc4});
    a.epilogue_restore();
    std::memcpy(BAILOUT, a.b.data(), a.b.size());
}


bool native(const Stub& s, State& st) {
    SCRATCH[3] = 0;
    reinterpret_cast<void (*)(State*)>(s.base)(&st);
    return SCRATCH[3] == 0;
}


volatile u64 TRAP_FROM = 0, TRAP_TO = 0;
volatile u64 WATCHED = 0;   
volatile long WATCH_HITS = 0;

LONG CALLBACK first_handler(EXCEPTION_POINTERS* info) {
    EXCEPTION_RECORD* r = info->ExceptionRecord;
    CONTEXT* c = info->ContextRecord;
    u64 at = reinterpret_cast<u64>(r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION && TRAP_FROM && at == TRAP_FROM) {
        c->Rip = TRAP_TO;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && WATCHED && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1 &&
        (r->ExceptionInformation[1] & ~0xfffull) == WATCHED) {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(WATCHED), 4096, PAGE_READWRITE, &old);
        InterlockedIncrement(&WATCH_HITS);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (at >= reinterpret_cast<u64>(STUB) && at < reinterpret_cast<u64>(STUB) + STUB_SIZE) {
        SCRATCH[3] = r->ExceptionCode;
        c->Rip = reinterpret_cast<u64>(BAILOUT);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK last_handler(EXCEPTION_POINTERS* info) {
    u64 at = reinterpret_cast<u64>(info->ExceptionRecord->ExceptionAddress);
    if (IMG && at >= reinterpret_cast<u64>(IMG) && at < reinterpret_cast<u64>(IMG) + sizeof IMAGE_AREA) {
        SCRATCH[3] = info->ExceptionRecord->ExceptionCode;
        info->ContextRecord->Rip = reinterpret_cast<u64>(BAILOUT);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}


struct Tally {
    const char* name;
    u64 cases = 0, failed = 0, skipped = 0;
};
std::vector<Tally> TALLY;
Tally* CUR = nullptr;
u64 FAILED = 0;

void family(const char* name) {
    TALLY.push_back(Tally{name});
    CUR = &TALLY.back();
}

std::string hexbytes(const u8* p, std::size_t n) {
    std::string s;
    char b[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += (i ? " " : "") + std::string(b);
    }
    return s;
}

void fail(const std::string& what, const std::vector<u8>& code) {
    ++CUR->failed;
    ++FAILED;
    if (CUR->failed <= 5) {
        std::printf("  FAIL %s #%llu: %s  [%s]\n", CUR->name, static_cast<unsigned long long>(CUR->cases), what.c_str(),
                    hexbytes(code.data(), code.size()).c_str());
    }
}

void to_context(const State& s, CONTEXT& c) {
    std::memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_FULL | CONTEXT_FLOATING_POINT;
    std::memcpy(&c.Rax, s.r, sizeof s.r);
    c.EFlags = static_cast<DWORD>(s.flags);
    c.MxCsr = 0x1f80;
    c.FltSave.MxCsr = 0x1f80;
    std::memcpy(c.FltSave.XmmRegisters, s.x, sizeof s.x);
}

void from_context(const CONTEXT& c, State& s) {
    std::memcpy(s.r, &c.Rax, sizeof s.r);
    s.flags = c.EFlags;
    std::memcpy(s.x, c.FltSave.XmmRegisters, sizeof s.x);
}

const char* h64(u64 v) {
    static char b[4][24];
    static int k = 0;
    k = (k + 1) & 3;
    std::snprintf(b[k], sizeof b[k], "%llx", static_cast<unsigned long long>(v));
    return b[k];
}


std::string differ(const State& a, const State& b, u32 flag_mask, u32 skip = 0) {
    for (int r = 0; r < 16; ++r) {
        if ((skip >> r) & 1) continue;
        if (a.r[r] != b.r[r]) return "r" + std::to_string(r) + " native " + h64(a.r[r]) + " emulated " + h64(b.r[r]);
    }
    if ((a.flags ^ b.flags) & flag_mask) return std::string("flags native ") + h64(a.flags & STATUS) + " emulated " + h64(b.flags & STATUS);
    for (int i = 0; i < 16; ++i) {
        if (std::memcmp(&a.x[i], &b.x[i], 16)) {
            return "xmm" + std::to_string(i) + " native " + h64(static_cast<u64>(a.x[i].High)) + ":" + h64(a.x[i].Low) + " emulated " +
                   h64(static_cast<u64>(b.x[i].High)) + ":" + h64(b.x[i].Low);
        }
    }
    return {};
}

void fill(Rng& g, u8* p, std::size_t n) {
    for (std::size_t i = 0; i + 8 <= n; i += 8) {
        u64 v = g();
        std::memcpy(p + i, &v, 8);
    }
}

void random_state(Rng& g, State& s) {
    for (auto& r : s.r) r = g();
    s.r[4] = reinterpret_cast<u64>(DATA) + DATA_SIZE / 2;
    s.flags = (g() & STATUS) | 0x202;
    for (auto& x : s.x) {
        x.Low = g();
        x.High = static_cast<i64>(g());
    }
}


struct MemOp {
    std::vector<u8> bytes;   
    u8 rex = 0;              
    int disp_at = -1;        
};


MemOp gen_mem(Rng& g, u64 want, bool a32, State& st, u32 busy) {
    for (;;) {
        int form = static_cast<int>(g() % 10);
        auto pick = [&](bool rsp_ok) {
            for (;;) {
                int r = static_cast<int>(g() % 16);
                if ((busy >> r) & 1) continue;
                if (r == 4 && !rsp_ok) continue;
                return r;
            }
        };
        int base = -1, index = -1, scale = 0, mod = 0, disp_len = 0;
        bool sib = false, rip = false;
        switch (form) {
            case 0: case 1: case 2:
                base = pick(false);
                if ((base & 7) == 4 || (form == 0 && (base & 7) == 5)) continue;
                mod = form;
                break;
            case 3: case 4: case 5:
                sib = true;
                base = pick(true);
                index = pick(false);
                if (base == index) continue;
                scale = static_cast<int>(g() % 4);
                mod = form - 3;
                if (mod == 0 && (base & 7) == 5) continue;
                break;
            case 6:
                sib = true;
                index = pick(false);
                scale = static_cast<int>(g() % 4);
                disp_len = 4;
                break;
            case 7:
                sib = true;
                base = pick(true);
                mod = static_cast<int>(g() % 3);
                if (mod == 0 && (base & 7) == 5) continue;
                break;
            case 8:
                sib = true;
                disp_len = 4;
                break;
            default:
                rip = true;
                disp_len = 4;
                break;
        }
        if (mod == 1) disp_len = 1;
        if (mod == 2) disp_len = 4;
        i64 disp = 0;
        if (disp_len == 1) disp = static_cast<i8>(g());
        if (disp_len == 4) disp = static_cast<i64>(g() % 0x20000) - 0x10000;
        u64 ival = index >= 0 ? (g() & 0xffff) : 0;
        if (base < 0 && !rip) {
            i64 d = static_cast<i64>(want) - static_cast<i64>(ival << scale);
            if (d < INT32_MIN || d > INT32_MAX) continue;
            disp = d;
        }
        if (base >= 0) {
            u64 bv = want - (ival << scale) - static_cast<u64>(disp);
            if (a32) bv = (bv & 0xffffffffull) | (g() << 32);
            st.r[base] = bv;
        }
        if (index >= 0) st.r[index] = a32 ? (ival | (g() << 32)) : ival;
        MemOp m;
        u8 rm = rip ? 5 : sib ? 4 : static_cast<u8>(base & 7);
        m.bytes.push_back(static_cast<u8>((mod << 6) | rm));
        if (sib) {
            u8 sb = static_cast<u8>((scale << 6) | ((index < 0 ? 4 : index & 7) << 3) | (base < 0 ? 5 : base & 7));
            m.bytes.push_back(sb);
        }
        if (rip) m.disp_at = static_cast<int>(m.bytes.size());
        for (int i = 0; i < disp_len; ++i) m.bytes.push_back(static_cast<u8>(static_cast<u64>(disp) >> (8 * i)));
        m.rex = static_cast<u8>((index >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
        return m;
    }
}


struct Insn {
    std::vector<u8> code;
    int rip_fix = -1;
    u64 rip_want = 0;
};

Insn assemble(const std::vector<u8>& prefixes, u8 rex, bool force_rex, const std::vector<u8>& opcode, u8 reg, const MemOp* mem, u8 rm,
              const std::vector<u8>& imm, u64 want = 0) {
    Insn in;
    in.code = prefixes;
    u8 r = static_cast<u8>(rex | (mem ? mem->rex : static_cast<u8>(rm >> 3)) | ((reg >> 3) << 2));
    if (r || force_rex) in.code.push_back(static_cast<u8>(0x40 | r));
    in.code.insert(in.code.end(), opcode.begin(), opcode.end());
    std::size_t modrm_at = in.code.size();
    if (mem) {
        in.code.insert(in.code.end(), mem->bytes.begin(), mem->bytes.end());
        in.code[modrm_at] |= static_cast<u8>((reg & 7) << 3);
        if (mem->disp_at >= 0) {
            in.rip_fix = static_cast<int>(modrm_at + mem->disp_at);
            in.rip_want = want;
        }
    } else {
        in.code.push_back(static_cast<u8>(0xc0 | ((reg & 7) << 3) | (rm & 7)));
    }
    in.code.insert(in.code.end(), imm.begin(), imm.end());
    return in;
}

std::vector<u8> seg_prefix(Rng& g) {
    static const u8 segs[] = {0x2e, 0x3e, 0x26, 0x36};
    switch (g() % 4) {
        case 0: return {segs[g() % 4]};
        default: return {};
    }
}


struct Case {
    Insn insn;
    State in{};
    u32 flag_mask = STATUS;
    u32 skip = 0;                 
    u8* mem = nullptr;            
    std::size_t mem_len = 0;
    em::Outcome expect = em::Outcome::Done;
    std::function<std::string(const State& nat, const State& emu)> extra;
};

u64 CODE_AT = 0;   


u64 code_at() {
    if (!CODE_AT) {
        Stub s = build_stub(STUB, {}, true, false);
        CODE_AT = reinterpret_cast<u64>(s.base) + s.code;
    }
    return CODE_AT;
}

std::vector<u8> mem_before, mem_native;

void run_case(Case& c) {
    ++CUR->cases;
    std::vector<u8>& code = c.insn.code;
    if (c.insn.rip_fix >= 0) {
        i64 d = static_cast<i64>(c.insn.rip_want) - static_cast<i64>(code_at() + code.size());
        i32 d32 = static_cast<i32>(d);
        std::memcpy(code.data() + c.insn.rip_fix, &d32, 4);
    }
    Stub s = build_stub(STUB, code, true, false);
    u64 at = reinterpret_cast<u64>(s.base) + s.code;
    if (c.mem) mem_before.assign(c.mem, c.mem + c.mem_len);
    State nat = c.in;
    if (!native(s, nat)) {
        ++CUR->skipped;
        fail("native execution faulted (code " + std::string(h64(SCRATCH[3])) + ")", code);
        if (c.mem) std::memcpy(c.mem, mem_before.data(), c.mem_len);
        return;
    }
    if (c.mem) {
        mem_native.assign(c.mem, c.mem + c.mem_len);
        std::memcpy(c.mem, mem_before.data(), c.mem_len);
    }
    alignas(16) CONTEXT ctx;
    to_context(c.in, ctx);
    ctx.Rip = at;
    em::Outcome o = em::emulate(ctx);
    if (o != c.expect) {
        fail("outcome " + std::to_string(static_cast<int>(o)), code);
        return;
    }
    if (o != em::Outcome::Done) return;
    if (ctx.Rip != at + code.size()) {
        fail("rip advanced by " + std::to_string(static_cast<i64>(ctx.Rip - at)) + " not " + std::to_string(code.size()), code);
        return;
    }
    State emu;
    from_context(ctx, emu);
    std::string d = differ(nat, emu, c.flag_mask, c.skip);
    if (d.empty() && c.extra) d = c.extra(nat, emu);
    if (d.empty() && c.mem && std::memcmp(c.mem, mem_native.data(), c.mem_len)) {
        std::size_t k = 0;
        while (c.mem[k] == mem_native[k]) ++k;
        d = "memory at +" + std::to_string(k);
    }
    if (!d.empty()) fail(d, code);
}


constexpr u64 TEB_OFFSETS[] = {0x00, 0x08, 0x18, 0x20, 0x28, 0x30, 0x38};

void sse4a(Rng& g, int n) {
    family("sse4a extrq/insertq");
    for (int k = 0; k < n; ++k) {
        Case c;
        random_state(g, c.in);
        int kind = static_cast<int>(g() % 4);   
        u8 a = static_cast<u8>(g() % 16), b = static_cast<u8>(g() % 16);
        u8 len = static_cast<u8>(g()), at = static_cast<u8>(g());
        if (g() % 2) {   
            len &= 63;
            at &= 63;
            if ((len ? len : 64) + at > 64) at = static_cast<u8>(64 - (len ? len : 64));
        }
        std::vector<u8> pre = seg_prefix(g);
        if (g() % 4 == 0) pre.push_back(0x67);
        pre.push_back(kind < 2 ? 0x66 : 0xf2);
        u8 rex = static_cast<u8>((g() % 3 == 0) ? 8 : 0);
        if (kind == 0) {
            c.insn = assemble(pre, rex, false, {0x0f, 0x78}, 0, nullptr, a, {len, at});
        } else if (kind == 1) {
            c.in.x[b].Low = (c.in.x[b].Low & ~0x3f3full) | (len & 0x3f) | (static_cast<u64>(at & 0x3f) << 8);
            c.insn = assemble(pre, rex, false, {0x0f, 0x79}, a, nullptr, b, {});
        } else if (kind == 2) {
            c.insn = assemble(pre, rex, false, {0x0f, 0x78}, a, nullptr, b, {len, at});
        } else {
            u64 hi = static_cast<u64>(c.in.x[b].High);
            c.in.x[b].High = static_cast<i64>((hi & ~0x3f3full) | (len & 0x3f) | (static_cast<u64>(at & 0x3f) << 8));
            c.insn = assemble(pre, rex, false, {0x0f, 0x79}, a, nullptr, b, {});
        }
        run_case(c);
    }
}

void movnt(Rng& g, int n) {
    family("sse4a movntss/movntsd");
    for (int k = 0; k < n; ++k) {
        Case c;
        random_state(g, c.in);
        fill(g, DATA, DATA_SIZE);
        bool a32 = g() % 4 == 0;
        u64 want = reinterpret_cast<u64>(DATA) + 64 + g() % (DATA_SIZE - 128);
        if (g() % 2) want &= ~7ull;
        std::vector<u8> pre = seg_prefix(g);
        if (a32) pre.push_back(0x67);
        pre.push_back(g() % 2 ? 0xf3 : 0xf2);
        MemOp m = gen_mem(g, want, a32, c.in, 0);
        u8 x = static_cast<u8>(g() % 16);
        c.insn = assemble(pre, static_cast<u8>(g() % 4 == 0 ? 8 : 0), false, {0x0f, 0x2b}, x, &m, 0, {}, want);
        c.mem = DATA;
        c.mem_len = DATA_SIZE;
        run_case(c);
    }
}

void cache_hints(Rng& g, int n) {
    family("clwb/clflushopt");
    for (int k = 0; k < n; ++k) {
        Case c;
        random_state(g, c.in);
        bool a32 = g() % 4 == 0, gs = !a32 && g() % 6 == 0;
        u64 want = gs ? TEB_OFFSETS[g() % 7] : reinterpret_cast<u64>(DATA) + g() % DATA_SIZE;
        std::vector<u8> pre = gs ? std::vector<u8>{0x65} : seg_prefix(g);
        if (a32) pre.push_back(0x67);
        pre.push_back(0x66);
        MemOp m = gen_mem(g, want, a32, c.in, 0);
        c.insn = assemble(pre, static_cast<u8>(g() % 4 == 0 ? 8 : 0), false, {0x0f, 0xae}, static_cast<u8>(g() % 2 ? 6 : 7), &m, 0, {}, want);
        run_case(c);
    }
}

void clzero_single(Rng& g, int n) {
    family("clzero (single, no loop)");
    em::set_loop_fusion(false);
    for (int k = 0; k < n; ++k) {
        Case c;
        random_state(g, c.in);
        fill(g, DATA, DATA_SIZE);
        bool a32 = g() % 3 == 0;
        u64 want = reinterpret_cast<u64>(DATA) + g() % DATA_SIZE;
        c.in.r[0] = a32 ? (want | (g() << 32)) : want;
        std::vector<u8> pre = seg_prefix(g);
        if (a32) pre.push_back(0x67);
        if (g() % 3 == 0) pre.push_back(static_cast<u8>(0x40 | (g() & 15)));
        pre.insert(pre.end(), {0x0f, 0x01, 0xfc});
        c.insn.code = pre;
        c.mem = DATA;
        c.mem_len = DATA_SIZE;
        run_case(c);
    }
    em::set_loop_fusion(true);
}

void monitor_wait(Rng& g, int n) {
    family("monitorx/mwaitx");
    for (int k = 0; k < n; ++k) {
        Case c;
        random_state(g, c.in);
        bool wait = g() % 2;
        bool a32 = g() % 4 == 0;
        std::vector<u8> pre = seg_prefix(g);
        if (a32) pre.push_back(0x67);
        if (g() % 3 == 0) pre.push_back(static_cast<u8>(0x40 | (g() & 15)));
        if (wait) {
            c.in.r[0] = 0;               
            c.in.r[1] = 2;               
            c.in.r[3] = 200 + g() % 2000;   
        } else {
            u64 at = reinterpret_cast<u64>(DATA) + g() % DATA_SIZE;
            c.in.r[0] = a32 ? (at | (g() << 32)) : at;
            c.in.r[1] = 0;
            c.in.r[2] = 0;
        }
        pre.insert(pre.end(), {0x0f, 0x01, static_cast<u8>(wait ? 0xfb : 0xfa)});
        c.insn.code = pre;
        run_case(c);
    }
}

void rdpru(Rng& g, int n) {
    family("rdpru");
    u64 last = 0;
    for (int k = 0; k < n; ++k) {
        Case c;
        random_state(g, c.in);
        u32 which = k % 4 == 0 ? 0 : k % 4 == 1 ? 1 : k % 4 == 2 ? 2 + static_cast<u32>(g() % 8) : static_cast<u32>(g());
        c.in.r[1] = (g() << 32) | which;
        c.insn.code = {0x0f, 0x01, 0xfd};
        c.skip = 1u << 0 | 1u << 2;
        c.extra = [&last, which](const State& nat, const State& emu) -> std::string {
            if (nat.r[0] >> 32 || nat.r[2] >> 32 || emu.r[0] >> 32 || emu.r[2] >> 32) return "upper halves of rax/rdx not zero";
            bool valid = which <= 1;
            u64 v = (emu.r[2] << 32) | emu.r[0], nv = (nat.r[2] << 32) | nat.r[0];
            if (!valid && (v || nv)) return "unsupported register gave a value (native " + std::string(h64(nv)) + ")";
            if (valid) {
                if (!nv) return "native gave 0";
                if (v < last) return "not monotonic";
                last = v;
            }
            return {};
        };
        run_case(c);
    }
}

void rdpid(Rng& g, int n) {
    family("rdpid");
    DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    HANDLE t = GetCurrentThread();
    for (int k = 0; k < n; ++k) {
        DWORD cpu = static_cast<DWORD>(g() % std::min<DWORD>(cpus, 64));
        DWORD_PTR old = SetThreadAffinityMask(t, DWORD_PTR{1} << cpu);
        Case c;
        random_state(g, c.in);
        u8 r = static_cast<u8>(g() % 16);
        if (r == 4) r = 0;
        std::vector<u8> pre = seg_prefix(g);
        pre.push_back(0xf3);
        c.insn = assemble(pre, static_cast<u8>(g() % 3 == 0 ? 8 : 0), false, {0x0f, 0xc7}, 7, nullptr, r, {});
        run_case(c);
        SetThreadAffinityMask(t, old);
    }
}

void rdseed(Rng& g, int n) {
    family("rdseed");
    for (int k = 0; k < n; ++k) {
        Case c;
        random_state(g, c.in);
        u8 r = static_cast<u8>(g() % 16);
        if (r == 4) r = 3;
        int size = static_cast<int>(g() % 3);   
        std::vector<u8> pre = seg_prefix(g);
        if (size == 0) pre.push_back(0x66);
        c.insn = assemble(pre, static_cast<u8>(size == 2 ? 8 : 0), false, {0x0f, 0xc7}, 7, nullptr, r, {});
        c.skip = 1u << r;
        c.flag_mask = 0;
        u64 before = c.in.r[r];
        c.extra = [r, size, before](const State& nat, const State& emu) -> std::string {
            if ((emu.flags & STATUS) != CF) return "emulated flags " + std::string(h64(emu.flags & STATUS));
            if (!(nat.flags & CF)) return {};   
            if ((nat.flags & STATUS) != CF) return "native flags " + std::string(h64(nat.flags & STATUS));
            u64 keep = size == 0 ? ~0xffffull : size == 1 ? ~0xffffffffull : 0;
            u64 want_hi = size == 0 ? (before & keep) : 0;
            if ((nat.r[r] & keep) != want_hi || (emu.r[r] & keep) != want_hi) return "the destination's other bits";
            return {};
        };
        run_case(c);
    }
}

void sha(Rng& g, int n) {
    family("sha");
    for (int k = 0; k < n; ++k) {
        Case c;
        random_state(g, c.in);
        int op = static_cast<int>(g() % 7);   
        u8 dst = static_cast<u8>(g() % 16);
        std::vector<u8> opcode = op < 6 ? std::vector<u8>{0x0f, 0x38, static_cast<u8>(0xc8 + op)} : std::vector<u8>{0x0f, 0x3a, 0xcc};
        std::vector<u8> imm;
        if (op == 6) imm.push_back(static_cast<u8>(g()));
        bool memory = g() % 3 == 0;
        u8 rex = static_cast<u8>(g() % 4 == 0 ? 8 : 0);
        if (memory) {
            fill(g, DATA, DATA_SIZE);
            bool gs = g() % 5 == 0;
            bool a32 = !gs && g() % 4 == 0;
            u64 want = gs ? (g() % 2 ? 0x20 : 0x30) : (reinterpret_cast<u64>(DATA) + (g() % (DATA_SIZE / 16)) * 16);
            std::vector<u8> pre = gs ? std::vector<u8>{0x65} : seg_prefix(g);
            if (a32) pre.push_back(0x67);
            MemOp m = gen_mem(g, want, a32, c.in, 0);
            c.insn = assemble(pre, rex, false, opcode, dst, &m, 0, imm, want);
        } else {
            c.insn = assemble(seg_prefix(g), rex, false, opcode, dst, nullptr, static_cast<u8>(g() % 16), imm);
        }
        run_case(c);
    }
}

void adx(Rng& g, int n) {
    family("adx adcx/adox");
    for (int k = 0; k < n; ++k) {
        Case c;
        random_state(g, c.in);
        bool ox = g() % 2;
        bool w64 = g() % 2;
        u8 dst = static_cast<u8>(g() % 16);
        if (dst == 4) dst = 5;

        if (g() % 4 == 0) c.in.r[dst] = ~0ull;
        std::vector<u8> pre = seg_prefix(g);
        bool memory = g() % 3 == 0;
        if (memory) {
            fill(g, DATA, DATA_SIZE);
            bool gs = g() % 5 == 0;
            bool a32 = !gs && g() % 4 == 0;
            u64 want = gs ? TEB_OFFSETS[g() % 7] : reinterpret_cast<u64>(DATA) + g() % (DATA_SIZE - 8);
            if (gs) pre = {0x65};
            if (a32) pre.push_back(0x67);
            pre.push_back(ox ? 0xf3 : 0x66);
            MemOp m = gen_mem(g, want, a32, c.in, 0);
            c.insn = assemble(pre, static_cast<u8>(w64 ? 8 : 0), false, {0x0f, 0x38, 0xf6}, dst, &m, 0, {}, want);
        } else {
            pre.push_back(ox ? 0xf3 : 0x66);
            u8 src = static_cast<u8>(g() % 16);
            if (src == 4) src = 6;
            if (g() % 4 == 0) c.in.r[src] = ~0ull;
            c.insn = assemble(pre, static_cast<u8>(w64 ? 8 : 0), false, {0x0f, 0x38, 0xf6}, dst, nullptr, src, {});
        }
        run_case(c);
    }
}


void fuzz(Rng& g, int n) {
    family("random encodings (differential)");
    static const u8 prefixes[] = {0x66, 0xf2, 0xf3, 0x67, 0x2e, 0x3e, 0x26, 0x36, 0x65, 0x64, 0xf0, 0x40, 0x41, 0x44, 0x48, 0x4c, 0x4f, 0x42};
    static const u8 mandatory[] = {0x66, 0xf2, 0xf3};
    u64 done = 0, refused = 0;
    em::set_loop_fusion(false);
    for (int k = 0; k < n; ++k) {
        u8 b[15];
        for (u8& x : b) x = static_cast<u8>(g());
        std::size_t i = 0;
        int np = static_cast<int>(g() % 4);
        for (int p = 0; p < np; ++p) b[i++] = prefixes[g() % sizeof prefixes];
        if (g() % 2) b[i++] = mandatory[g() % 3];
        if (g() % 2) b[i++] = static_cast<u8>(0x40 | (g() & 15));
        b[i++] = 0x0f;
        switch (g() % 9) {
            case 0: b[i++] = 0x01; b[i++] = g() % 2 ? 0xfa : 0xfc; break;   
            case 1: b[i++] = 0x2b; break;
            case 2: b[i++] = 0x78; break;
            case 3: b[i++] = 0x79; break;
            case 4: b[i++] = 0xae; break;
            case 5: b[i++] = 0x38; b[i++] = static_cast<u8>(0xc8 + g() % 6); break;
            case 6: b[i++] = 0x38; b[i++] = 0xf6; break;
            case 7: b[i++] = 0x3a; b[i++] = 0xcc; break;
            default: b[i++] = 0x01; b[i++] = 0xfc; break;
        }
        Case c;
        random_state(g, c.in);
        for (int r = 0; r < 16; ++r) {
            if (r != 4) c.in.r[r] = reinterpret_cast<u64>(DATA) + 0x1000 + (g() % 0x800) * 4 + (g() % 4 == 0 ? (g() << 32) : 0);
        }

        Stub s = build_stub(STUB, std::vector<u8>(b, b + 15), true, false);
        u64 at = reinterpret_cast<u64>(s.base) + s.code;
        fill(g, DATA, DATA_SIZE);
        mem_before.assign(DATA, DATA + DATA_SIZE);
        alignas(16) CONTEXT ctx;
        to_context(c.in, ctx);
        ctx.Rip = at;
        em::Outcome o = em::emulate(ctx);
        if (o == em::Outcome::Refused) ++refused;
        if (o != em::Outcome::Done) {
            std::memcpy(DATA, mem_before.data(), DATA_SIZE);
            continue;
        }
        ++done;
        ++CUR->cases;
        std::size_t len = static_cast<std::size_t>(ctx.Rip - at);
        std::vector<u8> code(b, b + std::min<std::size_t>(len, 15));
        State emu;
        from_context(ctx, emu);
        std::vector<u8> mem_emu(DATA, DATA + DATA_SIZE);
        std::memcpy(DATA, mem_before.data(), DATA_SIZE);
        s = build_stub(STUB, code, true, false);
        State nat = c.in;
        if (!native(s, nat)) {


            bool hint = false;
            for (std::size_t q = 0; q + 2 < code.size(); ++q) {
                if (code[q] == 0x0f && (code[q + 1] == 0xae || (code[q + 1] == 0x01 && code[q + 2] == 0xfa))) hint = true;
            }
            if (hint) {
                ++CUR->skipped;
                continue;
            }
            fail("emulated, but natively faults (" + std::string(h64(SCRATCH[3])) + ")", code);
            continue;
        }
        std::string d = differ(nat, emu, STATUS);
        if (d.empty() && std::memcmp(DATA, mem_emu.data(), DATA_SIZE)) d = "memory";
        if (!d.empty()) fail(d, code);
    }
    em::set_loop_fusion(true);
    std::printf("  %llu random encodings: %llu emulated (compared), %llu refused, the rest not handled\n", static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(done), static_cast<unsigned long long>(refused));
}


struct LoopCase {
    std::vector<u8> code;
    std::size_t entry = 0;    
    State in{};
    u64 lo = 0, hi = 0;       
    bool expect_whole = true;   
};


struct L {
    std::vector<u8> b;
    void rex(bool w, int reg, int rm) {
        u8 v = static_cast<u8>((w ? 8 : 0) | ((reg >> 3) << 2) | (rm >> 3));
        if (v) b.push_back(static_cast<u8>(0x40 | v));
    }
    void clzero(bool a32 = false) {
        if (a32) b.push_back(0x67);
        b.insert(b.end(), {0x0f, 0x01, 0xfc});
    }
    void clwb_rax() { b.insert(b.end(), {0x66, 0x0f, 0xae, 0x30}); }
    void add_imm(int r, i32 v, bool w = true) {
        rex(w, 0, r);
        if (v >= -128 && v <= 127) {
            b.insert(b.end(), {0x83, static_cast<u8>(0xc0 | (r & 7)), static_cast<u8>(v)});
        } else {
            b.insert(b.end(), {0x81, static_cast<u8>(0xc0 | (r & 7))});
            for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(static_cast<u32>(v) >> (8 * i)));
        }
    }
    void sub_imm(int r, i32 v, bool w = true) {
        rex(w, 0, r);
        b.insert(b.end(), {0x83, static_cast<u8>(0xe8 | (r & 7)), static_cast<u8>(v)});
    }
    void lea_disp(int d, int s, i8 disp) {
        rex(true, d, s);
        b.insert(b.end(), {0x8d, static_cast<u8>(0x40 | ((d & 7) << 3) | (s & 7))});
        if ((s & 7) == 4) b.push_back(0x24);
        b.push_back(static_cast<u8>(disp));
    }
    void lea_add(int d, int base, int index) {   
        rex(true, d, base);
        if (index >= 8) b.back() |= 2;
        b.insert(b.end(), {0x8d, static_cast<u8>(0x04 | ((d & 7) << 3)), static_cast<u8>(((index & 7) << 3) | (base & 7))});
    }
    void cmp_rr(int a, int c, bool w = true) {   
        rex(w, c, a);
        b.insert(b.end(), {0x39, static_cast<u8>(0xc0 | ((c & 7) << 3) | (a & 7))});
    }
    void cmp_rr3b(int a, int c) {   
        rex(true, a, c);
        b.insert(b.end(), {0x3b, static_cast<u8>(0xc0 | ((a & 7) << 3) | (c & 7))});
    }
    void cmp_imm(int r, i32 v) {
        if (r == 0) {
            b.insert(b.end(), {0x48, 0x3d});
        } else {
            rex(true, 0, r);
            b.insert(b.end(), {0x81, static_cast<u8>(0xf8 | (r & 7))});
        }
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(static_cast<u32>(v) >> (8 * i)));
    }
    void dec(int r, bool w = true) {
        rex(w, 0, r);
        b.insert(b.end(), {0xff, static_cast<u8>(0xc8 | (r & 7))});
    }
    void test_rr(int r, bool w) {
        rex(w, r, r);
        b.insert(b.end(), {0x85, static_cast<u8>(0xc0 | ((r & 7) << 3) | (r & 7))});
    }
    void mov_rr(int d, int s) {
        rex(true, s, d);
        b.insert(b.end(), {0x89, static_cast<u8>(0xc0 | ((s & 7) << 3) | (d & 7))});
    }
    void nop4() { b.insert(b.end(), {0x0f, 0x1f, 0x40, 0x00}); }
    void nop2() { b.insert(b.end(), {0x66, 0x90}); }
    void xor_rr(int r) {
        rex(true, r, r);
        b.insert(b.end(), {0x31, static_cast<u8>(0xc0 | ((r & 7) << 3) | (r & 7))});
    }
    void jcc(u8 cc, std::size_t top, bool near32 = false) {
        if (near32) {
            i32 rel = static_cast<i32>(static_cast<i64>(top) - static_cast<i64>(b.size() + 6));
            b.insert(b.end(), {0x0f, static_cast<u8>(0x80 | cc)});
            for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(static_cast<u32>(rel) >> (8 * i)));
        } else {
            i8 rel = static_cast<i8>(static_cast<i64>(top) - static_cast<i64>(b.size() + 2));
            b.insert(b.end(), {static_cast<u8>(0x70 | cc), static_cast<u8>(rel)});
        }
    }
};

int other_reg(Rng& g, u32 avoid) {
    for (;;) {
        int r = static_cast<int>(g() % 16);
        if (r == 4 || r == 0 || ((avoid >> r) & 1)) continue;
        return r;
    }
}

constexpr u8 JB = 2, JAE = 3, JE = 4, JNE = 5, JBE = 6, JA = 7, JS = 8, JL = 0xc, JGE = 0xd, JLE = 0xe, JG = 0xf;


LoopCase make_loop(Rng& g, int shape, u64 lines, u64 room) {
    LoopCase c;
    random_state(g, c.in);
    u64 base = reinterpret_cast<u64>(LOOPBUF) + 4096;
    u64 start = base + 64 * (g() % 64) + (g() % 2 ? g() % 64 : 0);
    u64 n = lines;
    L a;
    c.in.r[0] = start;
    int lim = other_reg(g, 0);
    c.lo = base - 4096;
    c.hi = base + room;
    switch (shape) {
        case 0: {   
            u8 cc = g() % 2 ? JB : JNE;
            if (cc == JNE && n == 0) n = 1;
            c.in.r[lim] = start + 64 * n;
            a.clzero();
            a.add_imm(0, 64);
            if (g() % 2) a.cmp_rr(0, lim); else a.cmp_rr3b(0, lim);
            a.jcc(cc, 0, g() % 3 == 0);
            break;
        }
        case 1: {   
            u8 cc = g() % 2 ? JB : JBE;
            a.clzero();
            if (g() % 2) a.lea_disp(0, 0, 64); else a.sub_imm(0, -64);
            a.cmp_imm(g() % 2 ? 0 : 0, static_cast<i32>(start + 64 * n));
            a.jcc(cc, 0);
            break;
        }
        case 2: {   
            if (n == 0) n = 1;
            int cnt = lim;
            bool w = g() % 2;
            c.in.r[cnt] = w ? n : ((g() << 32) | n);
            a.clzero();
            a.add_imm(0, 64);
            if (g() % 2) a.dec(cnt, w); else a.sub_imm(cnt, 1, w);
            a.jcc(JNE, 0);
            break;
        }
        case 3: {   
            int p = other_reg(g, 1u << lim);
            c.in.r[p] = start;
            c.in.r[0] = g();
            c.in.r[lim] = start + 64 * n;
            a.mov_rr(0, p);
            c.entry = a.b.size();
            a.clzero();
            a.add_imm(p, 64);
            a.cmp_rr(p, lim);
            a.jcc(JB, 0);
            c.in.r[0] = start;   
            break;
        }
        case 4: {   
            u64 top = start + 64 * n;
            c.in.r[0] = top;
            c.in.r[lim] = start;
            a.clzero();
            a.sub_imm(0, 64);
            a.cmp_rr(0, lim);
            a.jcc(JAE, 0);
            break;
        }
        case 5: {   
            if (n < 2) n = 2;
            n &= ~1ull;
            c.in.r[lim] = start + 64 * n;
            a.clzero();
            a.add_imm(0, 64);
            std::size_t second = a.b.size();
            a.clzero();
            a.add_imm(0, 64);
            a.cmp_rr(0, lim);
            a.jcc(JB, 0);
            if (g() % 2) {
                c.entry = second;
                c.in.r[0] = start + 64;
            }
            break;
        }
        case 6: {   
            c.in.r[lim] = start + 64 * n;
            a.clzero();
            a.add_imm(0, 64);
            a.cmp_rr(lim, 0);
            a.jcc(JA, 0);
            break;
        }
        case 7: {   
            int r = lim, r2 = other_reg(g, 1u << lim);
            i64 s0 = -static_cast<i64>(g() % 1000);
            c.in.r[r] = static_cast<u64>(s0);
            u8 cc = g() % 2 ? JL : JLE;
            c.in.r[r2] = static_cast<u64>(s0 + static_cast<i64>(n) - (cc == JLE ? 1 : 0));
            a.clzero();
            a.add_imm(0, 64);
            a.add_imm(r, 1);
            a.cmp_rr(r, r2);
            a.jcc(cc, 0);
            break;
        }
        case 8: {   
            if (n == 0) n = 1;
            c.in.r[lim] = (g() << 32) | n;
            a.clzero();
            a.add_imm(0, 64);
            a.dec(lim, false);
            a.test_rr(lim, false);
            a.jcc(g() % 2 ? JNE : JG, 0);
            break;
        }
        case 9: {   
            c.in.r[0] = start | (g() << 32);
            c.in.r[lim] = (start + 64 * n) | (g() << 32);
            a.clzero(true);
            a.add_imm(0, 64, false);
            a.cmp_rr(0, lim, false);
            a.jcc(JB, 0);
            break;
        }
        case 10: {   
            c.in.r[lim] = start + 128 * n;
            a.clzero();
            a.add_imm(0, 128);
            a.cmp_rr(0, lim);
            a.jcc(JB, 0);
            break;
        }
        case 11: {   
            c.in.r[lim] = start + 64 * n;
            a.clzero();
            a.nop4();
            a.add_imm(0, 64);
            a.nop2();
            a.cmp_rr(0, lim);
            a.jcc(JB, 0, true);
            break;
        }
        case 12: {   
            int st = other_reg(g, 1u << lim);
            c.in.r[st] = 64;
            c.in.r[lim] = start + 64 * n;
            a.clzero();
            a.lea_add(0, 0, st);
            a.cmp_rr(0, lim);
            a.jcc(JB, 0);
            break;
        }
        case 13: {   
            c.in.r[lim] = start + 64 * n;
            a.clwb_rax();
            a.add_imm(0, 64);
            a.cmp_rr(0, lim);
            a.jcc(JB, 0);
            break;
        }
        case 14: {   
            int z = other_reg(g, 1u << lim);
            c.in.r[lim] = start + 64 * n;
            a.clzero();
            a.xor_rr(z);
            a.add_imm(0, 64);
            a.cmp_rr(0, lim);
            a.jcc(JB, 0);
            c.expect_whole = false;
            break;
        }
        default: {   
            if (n == 0) n = 1;
            c.in.r[lim] = n - 1;
            a.clzero();
            a.add_imm(0, 64);
            a.dec(lim);
            a.test_rr(lim, true);
            a.jcc(9  , 0);
            break;
        }
    }
    c.code = a.b;
    return c;
}

constexpr int SHAPES = 16;

u64 fast_fill = 0x9e3779b97f4a7c15ull;
void quick_fill(u64 lo, u64 hi) {
    for (u64 p = lo; p + 8 <= hi; p += 8) {
        fast_fill ^= fast_fill << 13;
        fast_fill ^= fast_fill >> 7;
        fast_fill ^= fast_fill << 17;
        *reinterpret_cast<u64*>(p) = fast_fill | 1;
    }
}


void run_loop(LoopCase& c, int shape, u64* whole) {
    ++CUR->cases;
    Stub s = build_stub(STUB, c.code, true, false);
    u64 code = reinterpret_cast<u64>(s.base) + s.code;
    set_entry(s, code + c.entry);
    std::size_t len = static_cast<std::size_t>(c.hi - c.lo);
    quick_fill(c.lo, c.hi);
    mem_before.assign(reinterpret_cast<u8*>(c.lo), reinterpret_cast<u8*>(c.lo) + len);
    State nat = c.in;
    if (!native(s, nat)) {
        fail("native loop faulted", c.code);
        return;
    }
    mem_native.assign(reinterpret_cast<u8*>(c.lo), reinterpret_cast<u8*>(c.lo) + len);
    std::memcpy(reinterpret_cast<void*>(c.lo), mem_before.data(), len);
    alignas(16) CONTEXT ctx;
    to_context(c.in, ctx);
    ctx.Rip = code + c.entry;
    em::Outcome o = em::emulate(ctx);
    if (o != em::Outcome::Done) {
        fail("shape " + std::to_string(shape) + " outcome " + std::to_string(static_cast<int>(o)), c.code);
        return;
    }
    State emu;
    from_context(ctx, emu);
    u64 end = code + c.code.size();
    if (ctx.Rip == end) {
        if (whole) ++*whole;
    } else {
        if (c.expect_whole) fail("shape " + std::to_string(shape) + " not done in one (stopped at +" + std::to_string(ctx.Rip - code) + ")", c.code);
        set_entry(s, ctx.Rip);
        if (!native(s, emu)) {
            fail("finishing natively faulted", c.code);
            return;
        }
    }
    std::string d = differ(nat, emu, STATUS);
    if (d.empty() && std::memcmp(reinterpret_cast<void*>(c.lo), mem_native.data(), len)) {
        std::size_t k = 0;
        while (reinterpret_cast<u8*>(c.lo)[k] == mem_native[k]) ++k;
        d = "memory at +" + std::to_string(k);
    }
    if (!d.empty()) fail("shape " + std::to_string(shape) + ": " + d, c.code);
}

void loops(Rng& g, int n) {
    family("clzero/clwb loops");
    u64 whole = 0;
    for (int k = 0; k < n; ++k) {
        int shape = k % SHAPES;
        u64 lines = g() % 8 == 0 ? g() % 3 : g() % 400;
        LoopCase c = make_loop(g, shape, lines, 64 * 1024);
        run_loop(c, shape, &whole);
    }

    for (int shape = 0; shape < SHAPES; ++shape) {
        u64 lines = shape == 10 ? 40000 : 80000;
        LoopCase c = make_loop(g, shape, lines, 11u << 20);
        run_loop(c, shape, &whole);
    }
    std::printf("  loops done by one call: %llu of %llu\n", static_cast<unsigned long long>(whole), static_cast<unsigned long long>(CUR->cases));

    family("clzero loops, fusion off");
    em::set_loop_fusion(false);
    for (int k = 0; k < n / 8; ++k) {
        int shape = k % SHAPES;
        LoopCase c = make_loop(g, shape, g() % 50, 64 * 1024);
        c.expect_whole = false;
        run_loop(c, shape, nullptr);
    }
    em::set_loop_fusion(true);
}


void faults() {
    family("faults, write tracking, last error");
    u8* region = static_cast<u8*>(VirtualAlloc(nullptr, 16 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD old;
    u8* hole = region + 8 * 4096;
    VirtualProtect(hole, 4096, PAGE_NOACCESS, &old);
    auto check = [](bool ok, const char* what) {
        ++CUR->cases;
        if (!ok) fail(what, {});
    };

    {
        std::vector<u8> code = {0x0f, 0x01, 0xfc};
        Stub s = build_stub(STUB, code, true, false);
        alignas(16) CONTEXT ctx;
        State st{};
        st.r[0] = reinterpret_cast<u64>(hole) + 100;
        to_context(st, ctx);
        ctx.Rip = reinterpret_cast<u64>(s.base) + s.code;
        alignas(16) CONTEXT before = ctx;
        check(em::emulate(ctx) == em::Outcome::Refused && std::memcmp(&ctx, &before, sizeof ctx) == 0, "clzero into no-access: refused, unchanged");
    }

    {
        std::vector<u8> code = {0xf2, 0x0f, 0x2b, 0x00};   
        Stub s = build_stub(STUB, code, true, false);
        alignas(16) CONTEXT ctx;
        State st{};
        st.r[0] = reinterpret_cast<u64>(hole) + 8;
        to_context(st, ctx);
        ctx.Rip = reinterpret_cast<u64>(s.base) + s.code;
        check(em::emulate(ctx) == em::Outcome::Refused, "movntsd into no-access: refused");
    }


    {
        L a;
        a.clzero();
        a.add_imm(0, 64);
        a.cmp_rr(0, 1);
        a.jcc(JB, 0);
        Stub s = build_stub(STUB, a.b, true, false);
        u64 code = reinterpret_cast<u64>(s.base) + s.code;
        std::memset(region, 0xab, 8 * 4096);
        alignas(16) CONTEXT ctx;
        State st{};
        st.r[0] = reinterpret_cast<u64>(region) + 4096;
        st.r[1] = reinterpret_cast<u64>(region) + 12 * 4096;
        st.flags = 0x202;
        to_context(st, ctx);
        ctx.Rip = code;
        em::Outcome o = em::Outcome::Done;
        int calls = 0;
        while (calls < 10000) {
            ++calls;
            o = em::emulate(ctx);
            if (o != em::Outcome::Done || ctx.Rip != code) {
                break;
            }
        }
        bool zeroed = true;
        for (u8* p = region + 4096; p < hole; ++p) zeroed &= *p == 0;
        check(o == em::Outcome::Refused && ctx.Rax == reinterpret_cast<u64>(hole) && ctx.Rip == code && zeroed && region[4095] == 0xab,
              "loop into no-access: zeroed up to it, refused there");
        std::printf("  loop into a no-access page: %d calls\n", calls);
    }

    {
        u8* page = region + 2 * 4096;
        std::memset(region, 0xcd, 8 * 4096);
        VirtualProtect(page, 4096, PAGE_READONLY, &old);
        WATCHED = reinterpret_cast<u64>(page);
        WATCH_HITS = 0;
        std::vector<u8> code = {0x0f, 0x01, 0xfc};
        Stub s = build_stub(STUB, code, true, false);
        alignas(16) CONTEXT ctx;
        State st{};
        st.r[0] = reinterpret_cast<u64>(page) + 200;
        st.flags = 0x202;
        to_context(st, ctx);
        ctx.Rip = reinterpret_cast<u64>(s.base) + s.code;
        em::set_loop_fusion(false);
        em::Outcome o = em::emulate(ctx);
        em::set_loop_fusion(true);
        bool ok = o == em::Outcome::Done && WATCH_HITS == 1 && page[192] == 0 && page[255] == 0 && page[191] == 0xcd && page[256] == 0xcd;
        check(ok, "clzero into a write-tracked page: the tracking opened it, line zeroed");
        VirtualProtect(page, 4096, PAGE_READONLY, &old);
        WATCH_HITS = 0;
        L a;
        a.clzero();
        a.add_imm(0, 64);
        a.cmp_rr(0, 1);
        a.jcc(JB, 0);
        Stub s2 = build_stub(STUB, a.b, true, false);
        u64 code2 = reinterpret_cast<u64>(s2.base) + s2.code;
        st.r[0] = reinterpret_cast<u64>(region);
        st.r[1] = reinterpret_cast<u64>(region) + 6 * 4096;
        to_context(st, ctx);
        ctx.Rip = code2;
        int calls = 0;
        while (ctx.Rip != code2 + a.b.size() && calls < 1000) {
            ++calls;
            if (em::emulate(ctx) != em::Outcome::Done) break;
        }
        bool zero = true;
        for (u8* p = region; p < region + 6 * 4096; ++p) zero &= *p == 0;
        check(ctx.Rip == code2 + a.b.size() && zero && region[6 * 4096] == 0xcd && WATCH_HITS == 1 && ctx.Rax == st.r[1],
              "loop over a write-tracked page: done, the tracking opened it once");
        std::printf("  loop over a write-tracked page: %d calls\n", calls);
        WATCHED = 0;
    }


    {
        u8* fresh = static_cast<u8*>(VirtualAlloc(nullptr, 8 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        std::memset(fresh, 0xcd, 8 * 4096);
        u8* page = fresh + 3 * 4096;
        VirtualProtect(page, 4096, PAGE_READONLY, &old);
        WATCHED = reinterpret_cast<u64>(page);
        WATCH_HITS = 0;
        L a;
        a.clzero();
        a.add_imm(0, 64);
        a.cmp_rr(0, 1);
        a.jcc(JB, 0);
        Stub s = build_stub(STUB, a.b, true, false);
        u64 code = reinterpret_cast<u64>(s.base) + s.code;
        alignas(16) CONTEXT ctx;
        State st{};
        st.r[0] = reinterpret_cast<u64>(fresh) + 64;
        st.r[1] = reinterpret_cast<u64>(fresh) + 7 * 4096;
        st.flags = 0x202;
        to_context(st, ctx);
        ctx.Rip = code;
        int calls = 0;
        while (ctx.Rip != code + a.b.size() && calls < 1000) {
            ++calls;
            if (em::emulate(ctx) != em::Outcome::Done) break;
        }
        bool zero = true;
        for (u8* p = fresh + 64; p < fresh + 7 * 4096; ++p) zero &= *p == 0;
        check(ctx.Rip == code + a.b.size() && zero && fresh[63] == 0xcd && fresh[7 * 4096] == 0xcd && WATCH_HITS == 1 && calls == 2,
              "loop over a write-tracked page (fresh): up to it, then the rest");
        std::printf("  loop over a write-tracked page (fresh region): %d calls\n", calls);
        WATCHED = 0;
        VirtualFree(fresh, 0, MEM_RELEASE);
    }

    {
        u8* fresh = static_cast<u8*>(VirtualAlloc(nullptr, 8 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        std::memset(fresh, 0xef, 8 * 4096);
        u8* gap = fresh + 2 * 4096;
        VirtualProtect(gap, 4096, PAGE_NOACCESS, &old);
        L a;
        a.clzero();
        a.sub_imm(0, 64);
        a.cmp_rr(0, 1);
        a.jcc(JAE, 0);
        Stub s = build_stub(STUB, a.b, true, false);
        u64 code = reinterpret_cast<u64>(s.base) + s.code;
        alignas(16) CONTEXT ctx;
        State st{};
        st.r[0] = reinterpret_cast<u64>(fresh) + 6 * 4096 + 128;
        st.r[1] = reinterpret_cast<u64>(fresh);
        st.flags = 0x202;
        to_context(st, ctx);
        ctx.Rip = code;
        em::Outcome o = em::Outcome::Done;
        int calls = 0;
        while (calls < 1000) {
            ++calls;
            o = em::emulate(ctx);
            if (o != em::Outcome::Done || ctx.Rip != code) break;
        }
        bool zero = true;
        for (u8* p = gap + 4096; p < fresh + 6 * 4096 + 192; ++p) zero &= *p == 0;
        check(o == em::Outcome::Refused && ctx.Rax == reinterpret_cast<u64>(gap) + 4096 - 64 && zero && fresh[6 * 4096 + 192] == 0xef && calls <= 2,
              "loop downwards into no-access: down to it, refused there");
        std::printf("  loop downwards into a no-access page: %d calls\n", calls);
        VirtualFree(fresh, 0, MEM_RELEASE);
    }

    {
        std::vector<u8> code = {0xf3, 0x0f, 0xc7, 0xf8};   
        Stub s = build_stub(STUB, code, true, false);
        alignas(16) CONTEXT ctx;
        State st{};
        to_context(st, ctx);
        ctx.Rip = reinterpret_cast<u64>(s.base) + s.code;
        SetLastError(4242);
        em::emulate(ctx);
        check(GetLastError() == 4242, "last error kept");
    }


    {
        auto pass = [&](std::vector<u8> code, const char* what) {
            Stub s = build_stub(STUB, code, true, false);
            alignas(16) CONTEXT ctx;
            State st{};
            to_context(st, ctx);
            ctx.Rip = reinterpret_cast<u64>(s.base) + s.code;
            check(em::emulate(ctx) == em::Outcome::Pass, what);
        };
        pass({0x0f, 0x0b}, "ud2 passes");
        pass({0xf0, 0x0f, 0x01, 0xfc}, "lock clzero passes");
        pass({0x66, 0x0f, 0xae, 0xf0}, "clwb with a register (tpause) passes");
        pass({0x66, 0x0f, 0x78, 0xc8, 1, 2}, "extrq /1 passes");
        pass({0x0f, 0x38, 0xf6, 0xc1}, "0f 38 f6 without 66/f3 passes");
        pass({0x66, 0x0f, 0x38, 0xc8, 0xc1}, "sha with 66 passes");
        u32 m = em::missing();
        em::set_missing(0);
        pass({0x0f, 0x01, 0xfc}, "clzero passes when the host has it");
        em::set_missing(m);
        em::set_missing(em::ALL_FEATURES & ~em::RDPRU);
        pass({0x0f, 0x01, 0xfd}, "rdpru passes when only it is present");
        em::set_missing(m);
    }
    VirtualFree(region, 0, MEM_RELEASE);
}


u8 img_byte(std::size_t at) { return *reinterpret_cast<const volatile u8*>(IMG + at); }


bool through_handler(const std::vector<u8>& code, std::size_t pad, State& st, Stub& s) {
    std::vector<u8> padded(pad, 0x90);
    padded.insert(padded.end(), code.begin(), code.end());
    s = build_stub(IMG, padded, false, true);
    TRAP_FROM = reinterpret_cast<u64>(IMG) + s.code - 2;
    TRAP_TO = reinterpret_cast<u64>(IMG) + s.code + pad;
    bool ok = native(s, st);
    TRAP_FROM = 0;
    return ok;
}

void handler_path(Rng& g) {
    family("handler path and rewriting");
    DWORD_PTR affinity = SetThreadAffinityMask(GetCurrentThread(), 1ull << 3);
    DWORD old;
    VirtualProtect(IMG, sizeof IMAGE_AREA, PAGE_EXECUTE_READWRITE, &old);
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(IMG, &mbi, sizeof mbi);
    if (mbi.Type != MEM_IMAGE) std::printf("  (the test area is not MEM_IMAGE?)\n");
    struct Site {
        const char* what;
        std::vector<u8> code;
        std::size_t changed;   
        std::vector<u8> repl;
        std::size_t pad;
        bool patched;
        bool memory;   
    };
    u64 d = reinterpret_cast<u64>(DATA);
    std::vector<Site> sites = {
        {"clwb [rax]", {0x66, 0x0f, 0xae, 0x30}, 2, {0x1f, 0x00}, 0, true, false},
        {"clflushopt [r9+disp8]", {0x66, 0x41, 0x0f, 0xae, 0x79, 0x10}, 3, {0x1f, 0x41}, 3, true, false},

        {"clwb, 16-byte window", {0x66, 0x0f, 0xae, 0x30}, 2, {0x1f, 0x00}, 0, true, false},

        {"clwb, across 16", {0x66, 0x0f, 0xae, 0x30}, 2, {0x1f, 0x00}, 0, false, false},
        {"monitorx", {0x0f, 0x01, 0xfa}, 1, {0x1f, 0x00}, 5, true, false},
        {"mwaitx", {0x0f, 0x01, 0xfb}, 0, {0xf3, 0x90, 0x90}, 9, true, false},
        {"mwaitx with REX.W", {0x48, 0x0f, 0x01, 0xfb}, 1, {0xf3, 0x90, 0x90}, 0, true, false},
        {"movntss [rax], xmm3", {0xf3, 0x0f, 0x2b, 0x18}, 2, {0x11}, 1, true, true},
        {"movntsd [rdx+rcx*8], xmm9", {0xf2, 0x44, 0x0f, 0x2b, 0x0c, 0xca}, 3, {0x11}, 6, true, true},
        {"rdpid rcx (not rewritable)", {0xf3, 0x0f, 0xc7, 0xf9}, 0, {}, 0, false, false},
        {"adcx rax, rbx", {0x66, 0x48, 0x0f, 0x38, 0xf6, 0xc3}, 0, {}, 0, false, false},
        {"sha256rnds2 xmm1, xmm2", {0x0f, 0x38, 0xcb, 0xca}, 0, {}, 0, false, false},
        {"extrq xmm2, 8, 4", {0x66, 0x0f, 0x78, 0xc2, 8, 4}, 0, {}, 0, false, false},
    };
    std::size_t base_code = build_stub(IMG, {}, false, true).code;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        Site& s = sites[i];


        u64 want = i == 2 ? 7 : i == 3 ? 15 : (s.patched ? 3 : 0);
        u64 at0 = reinterpret_cast<u64>(IMG) + base_code + s.changed;
        s.pad = static_cast<std::size_t>((want - at0) & 15);
        if (s.patched && i != 2 && i != 3 && s.repl.size() + ((at0 + s.pad) & 7) > 8) s.pad = static_cast<std::size_t>((3 - at0) & 7);
    }
    for (bool patching : {false, true}) {
        em::set_patching(patching);
        for (Site& s : sites) {
            ++CUR->cases;
            State in;
            random_state(g, in);
            in.r[0] = d + 64 * (g() % 16);
            in.r[1] = patching ? 2 : 2;   
            in.r[3] = 500;                
            in.r[2] = d + 512;            
            in.r[9] = d + 1024;
            if (std::string(s.what).starts_with("monitorx")) in.r[1] = in.r[2] = 0;
            if (std::string(s.what).starts_with("mwaitx")) in.r[0] = 0;

            std::vector<u8> code = s.code;
            Stub plain = build_stub(STUB, code, false, false);
            fill(g, DATA, DATA_SIZE);
            mem_before.assign(DATA, DATA + DATA_SIZE);
            State nat = in;
            if (!native(plain, nat)) {
                fail(std::string(s.what) + ": the expectation's native run faulted", code);
                continue;
            }
            mem_native.assign(DATA, DATA + DATA_SIZE);
            std::memcpy(DATA, mem_before.data(), DATA_SIZE);
            u64 patched_before = em::stats().patched;
            State emu = in;
            Stub st;
            u32 skip = 1u << 4;
            bool rd = std::string(s.what).starts_with("rdpid");
            if (rd) skip |= 1u << 1;   
            if (!through_handler(code, s.pad, emu, st)) {
                fail(std::string(s.what) + ": the handler did not take it (" + h64(SCRATCH[3]) + ")", code);
                continue;
            }
            std::string diff = differ(nat, emu, STATUS, skip);
            if (diff.empty() && s.memory && std::memcmp(DATA, mem_native.data(), DATA_SIZE)) diff = "memory";
            if (!diff.empty()) {
                fail(std::string(s.what) + " (patching " + (patching ? "on" : "off") + "): " + diff, code);
                continue;
            }
            std::size_t at = st.code + s.pad;
            bool rewritten = !s.repl.empty() && img_byte(at + s.changed) == s.repl[0];
            bool expect = patching && s.patched;
            if (rewritten != expect || (expect && em::stats().patched != patched_before + 1)) {
                fail(std::string(s.what) + ": rewritten " + (rewritten ? "yes" : "no") + ", expected " + (expect ? "yes" : "no"), code);
                continue;
            }
            if (expect) {
                for (std::size_t k = 0; k < s.repl.size(); ++k) {
                    if (img_byte(at + s.changed + k) != s.repl[k]) fail(std::string(s.what) + ": rewritten bytes", code);
                }


                std::vector<u8> now;
                for (std::size_t k = 0; k < code.size(); ++k) now.push_back(img_byte(at + k));
                Stub again = build_stub(STUB, now, false, false);
                std::memcpy(DATA, mem_before.data(), DATA_SIZE);
                State re = in;
                if (!native(again, re)) {
                    fail(std::string(s.what) + ": the rewritten bytes fault", now);
                    continue;
                }
                std::string d2 = differ(nat, re, STATUS, skip);
                if (d2.empty() && s.memory && std::memcmp(DATA, mem_native.data(), DATA_SIZE)) d2 = "memory";
                if (!d2.empty()) fail(std::string(s.what) + ": rewritten bytes run natively: " + d2, now);


                std::memcpy(DATA, mem_before.data(), DATA_SIZE);
                State emu2 = in;
                TRAP_FROM = reinterpret_cast<u64>(IMG) + st.code - 2;
                TRAP_TO = reinterpret_cast<u64>(IMG) + at;
                bool ok2 = native(st, emu2);
                TRAP_FROM = 0;
                std::string d3 = ok2 ? differ(nat, emu2, STATUS, skip) : "faulted";
                if (!d3.empty()) fail(std::string(s.what) + ": trapping at the rewritten site: " + d3, now);
            }
        }
    }

    {
        ++CUR->cases;
        L a;
        a.clzero();
        a.add_imm(0, 64);
        a.cmp_rr(0, 1);
        a.jcc(JB, 0);
        State in{};
        in.r[0] = reinterpret_cast<u64>(LOOPBUF);
        in.r[1] = reinterpret_cast<u64>(LOOPBUF) + (1u << 20);
        in.flags = 0x202;
        quick_fill(reinterpret_cast<u64>(LOOPBUF), reinterpret_cast<u64>(LOOPBUF) + (1u << 20) + 4096);
        u64 traps = em::stats().by_kind[static_cast<unsigned>(em::Kind::ClzeroLoop)];
        State out = in;
        Stub st;
        bool ok = through_handler(a.b, 0, out, st);
        bool zero = true;
        for (u64* p = reinterpret_cast<u64*>(LOOPBUF); p < reinterpret_cast<u64*>(LOOPBUF + (1u << 20)); ++p) zero &= *p == 0;
        bool after = *reinterpret_cast<u64*>(LOOPBUF + (1u << 20)) != 0;
        if (!ok || !zero || !after || out.r[0] != in.r[1] || em::stats().by_kind[static_cast<unsigned>(em::Kind::ClzeroLoop)] != traps + 1) {
            fail("clzero loop (1 MiB) through the handler", a.b);
        }
    }
    em::set_patching(true);
    SetThreadAffinityMask(GetCurrentThread(), affinity);
}


double now_ns() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1e9 / static_cast<double>(f.QuadPart);
}

void bench() {
    SetThreadAffinityMask(GetCurrentThread(), 1ull << 2);
    em::set_missing(em::ALL_FEATURES);
    em::set_patching(false);
    struct B {
        const char* what;
        std::vector<u8> code;
    };
    std::vector<B> list = {
        {"clzero (one line)", {0x0f, 0x01, 0xfc}},
        {"monitorx", {0x0f, 0x01, 0xfa}},
        {"mwaitx (short timer)", {0x0f, 0x01, 0xfb}},
        {"rdpru", {0x0f, 0x01, 0xfd}},
        {"rdpid", {0xf3, 0x0f, 0xc7, 0xf8}},
        {"rdseed r64", {0x48, 0x0f, 0xc7, 0xf8}},
        {"extrq imm", {0x66, 0x0f, 0x78, 0xc1, 8, 4}},
        {"insertq reg", {0xf2, 0x0f, 0x79, 0xca}},
        {"movntsd [rax]", {0xf2, 0x0f, 0x2b, 0x00}},
        {"clwb [rax]", {0x66, 0x0f, 0xae, 0x30}},
        {"adcx r64, [rax]", {0x66, 0x48, 0x0f, 0x38, 0xf6, 0x18}},
        {"adox r64, r64", {0xf3, 0x48, 0x0f, 0x38, 0xf6, 0xd9}},
        {"sha1rnds4", {0x0f, 0x3a, 0xcc, 0xca, 1}},
        {"sha256rnds2", {0x0f, 0x38, 0xcb, 0xca}},
        {"sha256msg2 [rax]", {0x0f, 0x38, 0xcd, 0x08}},
    };
    std::printf("emulate() called directly (ns per instruction, 200000 calls):\n");
    em::set_loop_fusion(false);
    for (auto& b : list) {
        Stub s = build_stub(STUB, b.code, true, false);
        alignas(16) CONTEXT ctx;
        State st{};
        st.r[0] = reinterpret_cast<u64>(DATA);
        st.r[1] = 2;
        st.r[3] = 100;
        st.flags = 0x202;
        to_context(st, ctx);
        u64 rip = reinterpret_cast<u64>(s.base) + s.code;
        constexpr int N = 200000;
        double best = 1e300;
        for (int rep = 0; rep < 3; ++rep) {
            double t0 = now_ns();
            for (int i = 0; i < N; ++i) {
                ctx.Rip = rip;
                ctx.Rax = reinterpret_cast<u64>(DATA);
                em::emulate(ctx);
            }
            best = std::min(best, (now_ns() - t0) / N);
        }
        std::printf("  %-22s %8.1f\n", b.what, best);
    }
    em::set_loop_fusion(true);

    std::printf("clzero loop (clzero; add rax, 64; cmp rax, rcx; jb):\n");
    for (u64 size : {64ull << 10, 1ull << 20, 8ull << 20}) {
        L a;
        a.clzero();
        a.add_imm(0, 64);
        a.cmp_rr(0, 1);
        a.jcc(JB, 0);
        Stub s = build_stub(STUB, a.b, true, false);
        u64 code = reinterpret_cast<u64>(s.base) + s.code;
        u64 lo = reinterpret_cast<u64>(LOOPBUF);
        State st{};
        st.r[0] = lo;
        st.r[1] = lo + size;
        st.r[4] = reinterpret_cast<u64>(DATA) + DATA_SIZE / 2;
        st.flags = 0x202;
        double fused = 1e300, lines = 1e300, nat = 1e300, ms = 1e300;
        for (int rep = 0; rep < 5; ++rep) {
            quick_fill(lo, lo + size);
            alignas(16) CONTEXT ctx;
            to_context(st, ctx);
            ctx.Rip = code;
            double t0 = now_ns();
            em::emulate(ctx);
            fused = std::min(fused, now_ns() - t0);
            quick_fill(lo, lo + size);
            em::set_loop_fusion(false);
            t0 = now_ns();
            for (u64 p = lo; p < lo + size; p += 64) {
                ctx.Rip = code;
                ctx.Rax = p;
                em::emulate(ctx);
            }
            lines = std::min(lines, now_ns() - t0);
            em::set_loop_fusion(true);
            quick_fill(lo, lo + size);
            State n = st;
            t0 = now_ns();
            native(s, n);
            nat = std::min(nat, now_ns() - t0);
            quick_fill(lo, lo + size);
            t0 = now_ns();
            std::memset(reinterpret_cast<void*>(lo), 0, size);
            ms = std::min(ms, now_ns() - t0);
        }
        std::printf("  %5llu KiB: at once %9.1f us, line by line (no traps counted) %9.1f us, native clzero loop %9.1f us, memset %9.1f us\n",
                    static_cast<unsigned long long>(size >> 10), fused / 1e3, lines / 1e3, nat / 1e3, ms / 1e3);
    }


    {
        DWORD old;
        VirtualProtect(IMG, sizeof IMAGE_AREA, PAGE_EXECUTE_READWRITE, &old);
        std::vector<u8> code = {0xf3, 0x0f, 0xc7, 0xf9};   
        Stub plain = build_stub(STUB, code, false, false);
        State st{};
        st.flags = 0x202;
        constexpr int N = 20000;
        double t0 = now_ns();
        for (int i = 0; i < N; ++i) {
            State x = st;
            native(plain, x);
        }
        double base = (now_ns() - t0) / N;
        Stub s;
        State x = st;
        through_handler(code, 0, x, s);
        TRAP_FROM = reinterpret_cast<u64>(IMG) + s.code - 2;
        TRAP_TO = reinterpret_cast<u64>(IMG) + s.code;
        t0 = now_ns();
        for (int i = 0; i < N; ++i) {
            State y = st;
            native(s, y);
        }
        double trap = (now_ns() - t0) / N;
        TRAP_FROM = 0;
        std::printf("trap round trip (ud2 -> vectored handlers -> emulate rdpid -> continue): %.0f ns per instruction (native stub %.0f ns)\n",
                    trap - base, base);
    }
    em::set_patching(true);
}

}   

int main(int argc, char** argv) {
    bool do_bench = argc > 1 && std::string(argv[1]) == "bench";
    if (!alloc_low()) {
        std::printf("no low memory\n");
        return 2;
    }
    build_bailout();
    IMG = const_cast<u8*>(reinterpret_cast<const u8*>(IMAGE_AREA));
    AddVectoredExceptionHandler(1, first_handler);
    u32 host = em::host_features();
    std::printf("host features %03x (all: %03x); rewriting sites (VICTORY_INSN_PATCH): %s\n", host, static_cast<unsigned>(em::ALL_FEATURES),
                em::patching() ? "on" : "off");
    if (host != em::ALL_FEATURES) {
        std::printf("this CPU lacks some of them: the native comparison needs a CPU that has all (an AMD Zen 4 or later)\n");
        return 2;
    }
    em::set_missing(em::ALL_FEATURES);
    Rng g(20261126);
    if (!do_bench) {


        faults();
        fuzz(g, 400000);
    }
    danzi::loader::crash::report_crashes();
    AddVectoredExceptionHandler(0, last_handler);
    if (do_bench) {
        bench();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "probe") {

        for (u32 which : {0u, 1u, 2u, 0x7fu}) {
            for (u64 f : {0x202ull, 0x202ull | STATUS}) {
                State st{};
                st.r[1] = which;
                st.r[0] = st.r[2] = 0x1234567812345678ull;
                st.r[4] = reinterpret_cast<u64>(DATA) + DATA_SIZE / 2;
                st.flags = f;
                Stub s = build_stub(STUB, {0x0f, 0x01, 0xfd}, true, false);
                bool ok = native(s, st);
                std::printf("rdpru ecx=%x flags in %03llx: %s edx:eax %llx:%llx flags out %03llx\n", which, f & STATUS, ok ? "ran" : "faulted",
                            static_cast<unsigned long long>(st.r[2]), static_cast<unsigned long long>(st.r[0]),
                            static_cast<unsigned long long>(st.flags & STATUS));
            }
        }
        return 0;
    }
    sse4a(g, 12000);
    movnt(g, 6000);
    cache_hints(g, 6000);
    clzero_single(g, 6000);
    monitor_wait(g, 2000);
    rdpru(g, 2000);
    rdpid(g, 2000);
    rdseed(g, 3000);
    sha(g, 14000);
    adx(g, 10000);
    loops(g, 4000);
    handler_path(g);
    u64 total = 0;
    for (const Tally& t : TALLY) {
        std::printf("%-36s %7llu cases, %llu failed\n", t.name, static_cast<unsigned long long>(t.cases), static_cast<unsigned long long>(t.failed));
        total += t.cases;
    }
    em::Stats s = em::stats();
    std::printf("%llu cases; emulated %llu instructions, %llu sites rewritten, %llu bytes zeroed by clzero\n", static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(s.total), static_cast<unsigned long long>(s.patched), static_cast<unsigned long long>(s.clzero_bytes));
    std::printf(FAILED ? "FAILED\n" : "ok\n");
    return FAILED ? 1 : 0;
}
