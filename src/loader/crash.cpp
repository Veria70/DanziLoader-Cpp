// SPDX-License-Identifier: GPL-3.0-or-later
// The crash log, the reports of what ends the process, and the emulation of
// AMD-only instructions (`victory-loader/src/crash.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See crash.hpp.
#include "crash.hpp"
#include "emulate.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <algorithm>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace danzi::loader::crash {

namespace {

std::mutex LOG_LOCK;
std::optional<std::filesystem::path> LOG;

std::mutex SAID_LOCK;
std::deque<std::string> SAID;
constexpr std::size_t SAID_LINES = 8;

void append_to_log(const std::string& text) {
    std::optional<std::filesystem::path> file;
    {
        std::lock_guard l(LOG_LOCK);
        file = LOG;
    }
    if (!file) {
        return;
    }
    auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::ofstream f(*file, std::ios::binary | std::ios::app);
    if (f) {
        f << "unix time " << now << ", process " << GetCurrentProcessId() << "\n" << text << "\n";
    }
}

}   

void log_crashes_to(const std::filesystem::path& file) {
    std::lock_guard l(LOG_LOCK);
    if (!LOG) {
        LOG = file;
    }
}

void log_crash(std::string_view text) {
    std::string t = std::string(text) + "\n" + title_output();
    std::fwrite(t.data(), 1, t.size(), stderr);
    append_to_log(t);
}

void note_title_output(std::string_view line) {
    if (line.empty()) {
        return;
    }
    std::lock_guard l(SAID_LOCK);
    if (SAID.size() == SAID_LINES) {
        SAID.pop_front();
    }
    SAID.emplace_back(line);
}

std::string title_output() {
    std::unique_lock l(SAID_LOCK, std::try_to_lock);
    if (!l.owns_lock()) {
        return {};
    }
    std::string out;
    for (const auto& s : SAID) {
        out += "  title said: " + s + "\n";
    }
    return out;
}


namespace {

constexpr LONG CONTINUE_SEARCH = 0;
constexpr LONG CONTINUE_EXECUTION = -1;
constexpr std::size_t MAX_FRAMES = 40;
constexpr DWORD FATAL[6] = {EXCEPTION_ACCESS_VIOLATION,    EXCEPTION_ILLEGAL_INSTRUCTION, EXCEPTION_PRIV_INSTRUCTION,
                            EXCEPTION_INT_DIVIDE_BY_ZERO, EXCEPTION_STACK_OVERFLOW,      EXCEPTION_BREAKPOINT};

std::string hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}


std::optional<std::size_t> clwb_len(const std::uint8_t* code) {
    std::size_t at = (code[1] & 0xf0) == 0x40 ? 2 : 1;
    if (code[0] != 0x66 || code[at] != 0x0f || code[at + 1] != 0xae) {
        return std::nullopt;
    }
    std::uint8_t modrm = code[at + 2];
    std::uint8_t mode = modrm >> 6, rm = modrm & 7;
    if (mode == 3 || ((modrm >> 3) & 7) != 6) {
        return std::nullopt;
    }
    std::size_t len = at + 3;
    std::uint8_t base = rm;
    if (rm == 4) {
        base = code[len] & 7;
        ++len;
    }
    if (mode == 0 && (rm == 5 || (rm == 4 && base == 5))) {
        len += 4;
    } else if (mode == 1) {
        len += 1;
    } else if (mode == 2) {
        len += 4;
    }
    return len;
}

struct ClwbLoop {
    std::uint8_t base, limit;
    std::size_t len;
};


std::optional<ClwbLoop> clwb_loop(const std::uint8_t* code, std::size_t n) {
    auto at_ = [&](std::size_t i) -> std::optional<std::uint8_t> { return i < n ? std::optional(code[i]) : std::nullopt; };
    auto c1 = at_(1);
    if (!c1) {
        return std::nullopt;
    }
    std::uint8_t rex = 0;
    std::size_t at = 1;
    if (*c1 == 0x40 || *c1 == 0x41) {
        rex = *c1;
        at = 2;
    }
    if (at + 2 > n || code[0] != 0x66 || code[at] != 0x0f || code[at + 1] != 0xae) {
        return std::nullopt;
    }
    auto m = at_(at + 2);
    if (!m) {
        return std::nullopt;
    }
    std::uint8_t modrm = *m, rm = modrm & 7;
    if ((modrm >> 6) != 0 || ((modrm >> 3) & 7) != 6 || rm == 4 || rm == 5) {
        return std::nullopt;
    }
    std::uint8_t base = static_cast<std::uint8_t>(rm | (rex & 1) << 3);
    std::size_t add = at + 3;
    if (add + 4 > n || code[add] != (0x48 | base >> 3) || code[add + 1] != 0x83 || code[add + 2] != (0xc0 | (base & 7)) || code[add + 3] != 0x40) {
        return std::nullopt;
    }
    std::size_t cmp = add + 4;
    if (cmp + 3 > n) {
        return std::nullopt;
    }
    std::uint8_t cmp_rex = code[cmp], cmp_modrm = code[cmp + 2];
    if (cmp_rex < 0x48 || cmp_rex > 0x4f || code[cmp + 1] != 0x39) {
        return std::nullopt;
    }
    if ((cmp_modrm >> 6) != 3 || ((cmp_modrm & 7) | (cmp_rex & 1) << 3) != base) {
        return std::nullopt;
    }
    std::uint8_t limit = static_cast<std::uint8_t>(((cmp_modrm >> 3) & 7) | (cmp_rex & 4) << 1);
    std::size_t jump = cmp + 3, len = jump + 2;
    auto j0 = at_(jump), j1 = at_(jump + 1);
    if (!j0 || !j1 || *j0 != 0x76 || static_cast<std::int64_t>(static_cast<std::int8_t>(*j1)) != -static_cast<std::int64_t>(len)) {
        return std::nullopt;
    }
    if (limit == base) {
        return std::nullopt;
    }
    return ClwbLoop{base, limit, len};
}

DWORD64& gpr(CONTEXT& c, std::uint8_t n) {
    switch (n & 15) {
        case 0: return c.Rax;
        case 1: return c.Rcx;
        case 2: return c.Rdx;
        case 3: return c.Rbx;
        case 4: return c.Rsp;
        case 5: return c.Rbp;
        case 6: return c.Rsi;
        case 7: return c.Rdi;
        case 8: return c.R8;
        case 9: return c.R9;
        case 10: return c.R10;
        case 11: return c.R11;
        case 12: return c.R12;
        case 13: return c.R13;
        case 14: return c.R14;
        default: return c.R15;
    }
}


std::uint32_t cmp_flags(std::uint64_t a, std::uint64_t b) {
    std::uint64_t r = a - b;
    return (a < b ? 1u : 0u) | ((std::popcount(static_cast<std::uint8_t>(r)) % 2 == 0) ? 1u : 0u) << 2 |
           (static_cast<std::uint32_t>((a ^ b ^ r) >> 4) & 1) << 4 | (r == 0 ? 1u : 0u) << 6 | static_cast<std::uint32_t>(r >> 63) << 7 |
           static_cast<std::uint32_t>(((a ^ b) & (a ^ r)) >> 63) << 11;
}
constexpr std::uint32_t CMP_FLAGS = 0x8d5;


std::optional<std::size_t> sse4a(const std::uint8_t* code, M128A* xmm) {
    std::uint8_t prefix = code[0];
    std::uint8_t rex = 0;
    std::size_t at = 1;
    if ((code[1] & 0xf0) == 0x40) {
        rex = code[1];
        at = 2;
    }
    if ((prefix != 0x66 && prefix != 0xf2) || code[at] != 0x0f || (code[at + 1] != 0x78 && code[at + 1] != 0x79)) {
        return std::nullopt;
    }
    std::uint8_t modrm = code[at + 2];
    if ((modrm >> 6) != 3) {
        return std::nullopt;
    }
    std::size_t dst = ((modrm >> 3) & 7) | (rex & 4) << 1;
    std::size_t src = (modrm & 7) | (rex & 1) << 3;
    bool immediate = code[at + 1] == 0x78;
    std::uint8_t f0, f1;
    std::size_t len;
    if (immediate) {
        f0 = code[at + 3];
        f1 = code[at + 4];
        len = at + 5;
    } else {

        std::uint64_t f = prefix == 0x66 ? xmm[src].Low : static_cast<std::uint64_t>(xmm[src].High);
        f0 = static_cast<std::uint8_t>(f);
        f1 = static_cast<std::uint8_t>(f >> 8);
        len = at + 3;
    }
    std::uint32_t n = f0 & 63, at_bit = f1 & 63;
    std::uint32_t bits = n == 0 ? 64 : n;
    std::uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    if (prefix == 0x66) {
        if (immediate && ((modrm >> 3) & 7) != 0) {
            return std::nullopt;
        }
        std::size_t target = immediate ? src : dst;
        xmm[target].Low = (xmm[target].Low >> at_bit) & mask;
    } else {
        std::uint64_t value = (xmm[src].Low & mask) << at_bit;
        xmm[dst].Low = (xmm[dst].Low & ~(mask << at_bit)) | value;
    }
    return len;
}


bool emulate_amd_only(EXCEPTION_POINTERS* info) {
    EXCEPTION_RECORD& record = *info->ExceptionRecord;
    CONTEXT& context = *info->ContextRecord;
    if (record.ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return false;
    }
    const auto* code = reinterpret_cast<const std::uint8_t*>(context.Rip);
    if (code[0] == 0x0f && code[1] == 0x01 && code[2] == 0xfc) {
        std::memset(reinterpret_cast<void*>(context.Rax & ~63ull), 0, 64);
    } else if (code[0] == 0x0f && code[1] == 0x01 && code[2] == 0xfa) {
    } else if (code[0] == 0x0f && code[1] == 0x01 && code[2] == 0xfb) {
        SwitchToThread();
    } else {


        std::size_t room = 0x1000 - (context.Rip & 0xfff);
        std::uint8_t bytes[32] = {};
        std::size_t n = std::min<std::size_t>(room, sizeof bytes);
        std::memcpy(bytes, code, n);
        if (auto pass = clwb_loop(bytes, n)) {
            DWORD64 limit = gpr(context, pass->limit);
            DWORD64& base = gpr(context, pass->base);
            base = base <= limit ? base + ((limit - base) / 64 + 1) * 64 : base + 64;
            context.EFlags = (context.EFlags & ~CMP_FLAGS) | cmp_flags(base, limit);
            context.Rip += pass->len;
            return true;
        }
        if (n >= 15) {
            if (auto len = clwb_len(bytes)) {
                context.Rip += *len;
                return true;
            }
        }
        auto len = sse4a(code, context.FltSave.XmmRegisters);
        if (!len) {
            return false;
        }
        context.Rip += *len;
        return true;
    }
    context.Rip += 3;
    return true;
}

std::optional<std::string> module_of(std::uintptr_t address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address),
                            &module)) {
        return std::nullopt;
    }
    wchar_t path[260];
    DWORD len = GetModuleFileNameW(module, path, 260);
    std::wstring p(path, len);
    std::wstring name = p.substr(p.find_last_of(L'\\') == std::wstring::npos ? 0 : p.find_last_of(L'\\') + 1);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), narrow.data(), bytes, nullptr, nullptr);
    return narrow + "+" + hex(address - reinterpret_cast<std::uintptr_t>(module));
}

std::string memory_at(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof info) == 0) {
        return "outside the address space";
    }
    std::string protect;
    if (info.State == MEM_FREE) {
        return "nothing mapped there";
    }
    if (info.State == MEM_RESERVE) {
        return "reserved, never committed";
    }
    if (info.Protect & PAGE_GUARD) {
        return "a guard page";
    }
    switch (info.Protect & 0xff) {
        case PAGE_NOACCESS: protect = "no access"; break;
        case PAGE_READONLY: protect = "read-only"; break;
        case PAGE_READWRITE: protect = "read-write"; break;
        case PAGE_WRITECOPY: protect = "copy-on-write"; break;
        case PAGE_EXECUTE: protect = "execute-only"; break;
        case PAGE_EXECUTE_READ: protect = "read-execute"; break;
        case PAGE_EXECUTE_READWRITE: protect = "read-write-execute"; break;
        case PAGE_EXECUTE_WRITECOPY: protect = "copy-on-write, executable"; break;
        default: protect = "protection " + hex(info.Protect); break;
    }
    if (info.Type == MEM_IMAGE) {
        if (auto m = module_of(address)) {
            return protect + ", in " + *m;
        }
    }
    return protect;
}


std::string instruction_bytes(std::uintptr_t at) {
    std::size_t n = std::min<std::size_t>(0x1000 - (at & 0xfff), 15);
    std::string out;
    for (std::size_t k = 0; k < n; ++k) {
        char b[4];
        std::snprintf(b, sizeof b, "%02x", reinterpret_cast<const std::uint8_t*>(at)[k]);
        out += (k ? " " : "") + std::string(b);
    }
    return out;
}

constexpr DWORD CPP_EXCEPTION = 0xE06D7363;
constexpr ULONG_PTR CPP_MAGIC = 0x19930520;


std::string thrown(const ULONG_PTR* p) {
    auto rva = [](std::uintptr_t at) { return static_cast<std::uintptr_t>(*reinterpret_cast<const std::uint32_t*>(at)); };
    std::uintptr_t types = p[3] + rva(p[2] + 12);
    auto name = [&](std::uintptr_t i) {
        std::uintptr_t descriptor = p[3] + rva(p[3] + rva(types + 4 + 4 * i) + 4);
        return std::string(reinterpret_cast<const char*>(descriptor + 16));
    };
    std::string first = name(0);
    std::string value;
    if ((first == ".H" || first == ".I" || first == ".J" || first == ".K") && p[1] != 0) {
        char b[16];
        std::snprintf(b, sizeof b, "0x%08x", *reinterpret_cast<const std::uint32_t*>(p[1]));
        value = std::string(" = ") + b;
    } else {
        for (std::uintptr_t i = 0; i < rva(types); ++i) {
            if (name(i) == ".?AVexception@std@@") {
                const char* message = reinterpret_cast<const char* const*>(p[1])[1];
                if (message) {
                    value = std::string(": ") + message;
                }
                break;
            }
        }
    }
    return first + value;
}

struct alignas(16) Aligned16 {
    CONTEXT c;
};

}   

std::string locate(std::uintptr_t address) {
    auto m = module_of(address);
    return m ? *m : hex(address);
}

void unwind_into(const CONTEXT& start, std::uintptr_t low, std::uintptr_t high, std::vector<std::uint64_t>& frames) {
    Aligned16 aligned{start};
    CONTEXT& context = aligned.c;
    frames.push_back(context.Rip);
    while (frames.size() < MAX_FRAMES) {
        std::uintptr_t rsp = context.Rsp;
        if (rsp < low || rsp + 8 > high) {
            break;
        }
        DWORD64 image = 0;
        auto* entry = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
        if (!entry) {
            context.Rip = *reinterpret_cast<const std::uint64_t*>(rsp);
            context.Rsp += 8;
        } else {
            void* data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, entry, &context, &data, &frame, nullptr);
        }
        if (context.Rip == 0) {
            break;
        }
        frames.push_back(context.Rip);
    }
}

namespace {

void report(const char* what, const EXCEPTION_POINTERS* info, bool to_file) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    const CONTEXT& context = *info->ContextRecord;
    DWORD code = record.ExceptionCode;
    auto at = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
    const ULONG_PTR* p = record.ExceptionInformation;
    std::string detail;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        const char* kind = p[0] == 0 ? "read" : p[0] == 1 ? "write" : "execute";
        detail = std::string(" (") + kind + " " + hex(p[1]) + ": " + memory_at(p[1]) + ")";
    } else if (code == CPP_EXCEPTION && record.NumberParameters >= 4 && p[0] == CPP_MAGIC) {
        detail = " (C++ " + thrown(p) + " thrown)";
    } else if (code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION) {
        detail = " (code " + instruction_bytes(at) + ")";
    }
    std::string text;
    auto line = [&](const std::string& l) {
        std::fprintf(stderr, "%s\n", l.c_str());
        text += l + "\n";
    };
    char c[16];
    std::snprintf(c, sizeof c, "0x%08x", static_cast<unsigned>(code));
    line(std::string("VIctory: ") + what + " " + c + " at " + locate(at) + detail);
    line("  rax " + hex(context.Rax) + " rbx " + hex(context.Rbx) + " rcx " + hex(context.Rcx) + " rdx " + hex(context.Rdx) + " rsi " + hex(context.Rsi) +
         " rdi " + hex(context.Rdi) + " rsp " + hex(context.Rsp) + " rbp " + hex(context.Rbp));
    line("  r8 " + hex(context.R8) + " r9 " + hex(context.R9) + " r10 " + hex(context.R10) + " r11 " + hex(context.R11) + " r12 " + hex(context.R12) +
         " r13 " + hex(context.R13) + " r14 " + hex(context.R14) + " r15 " + hex(context.R15));
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    std::vector<std::uint64_t> frames;
    frames.reserve(MAX_FRAMES);
    unwind_into(context, low, high, frames);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        char idx[8];
        std::snprintf(idx, sizeof idx, "%-2zu", i);
        line(std::string("  #") + idx + " " + locate(frames[i]));
    }
    if (to_file) {
        std::string said = title_output();
        std::fwrite(said.data(), 1, said.size(), stderr);
        text += said;
        append_to_log(text);
    }
}

LONG WINAPI handler(EXCEPTION_POINTERS* info) {


    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        emulate::Outcome done = emulate::emulate(*info->ContextRecord);
        if (done == emulate::Outcome::Done || (done == emulate::Outcome::Pass && emulate_amd_only(info))) {
            return CONTINUE_EXECUTION;
        }
    }
    DWORD code = info->ExceptionRecord->ExceptionCode;
    for (DWORD f : FATAL) {
        if (f == code) {
            report("first-chance exception", info, false);
            break;
        }
    }
    return CONTINUE_SEARCH;
}

LONG WINAPI filter(EXCEPTION_POINTERS* info) {
    report_unhandled(info);
    return CONTINUE_SEARCH;
}

}   

void report_unhandled(const EXCEPTION_POINTERS* info) { report("unhandled exception", info, true); }

void report_crashes() {
    AddVectoredExceptionHandler(0, handler);
    SetUnhandledExceptionFilter(filter);
}

}   
