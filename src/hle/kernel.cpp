// SPDX-License-Identifier: GPL-3.0-or-later
// Overrides of shared Windows APIs whose console meaning differs
// (`victory-hle/src/modules/kernel.rs`; DanziLoader / victory-perf, see
// NOTICE.md). Only the differences live here; everything else passes
// through to the host (`HostPolicy`). Log lines (the title's exits, refused
// DLLs, the title's debug output): milestone 7.
#include "kernel.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/log.hpp"
#include "loader/crash.hpp"
#include "pe/image.hpp"
#include "registry.hpp"
#include "runtime.hpp"

namespace danzi::hle::kernel {

namespace {

constexpr DWORD ERROR_INSUFFICIENT_BUFFER_ = 122;
constexpr DWORD ERROR_MOD_NOT_FOUND_ = 126;


std::string narrow(const char* text) { return pe::utf8_lossy(std::span(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text))); }


std::string wide(const wchar_t* text) {
    std::size_t len = std::wcslen(text);
    std::string out;
    for (std::size_t i = 0; i < len; ++i) {
        std::uint32_t c = static_cast<std::uint16_t>(text[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<std::uint16_t>(text[i + 1]) - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | c >> 6));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | c >> 12));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | c >> 18));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::wstring to_wide(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
}


template <class T>
DWORD copy_truncated(const T* text, std::size_t len, T* out, DWORD size) {
    if (size == 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER_);
        return 0;
    }
    std::size_t room = size - 1;
    std::size_t n = std::min(len, room);
    std::memcpy(out, text, n * sizeof(T));
    out[n] = T{};
    if (n < len) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER_);
        return size;
    }
    return static_cast<DWORD>(n);
}


std::optional<std::filesystem::path> title_path(void* module) {
    std::uintptr_t base;
    if (!module) {
        auto m = runtime::main_image();
        if (!m) {
            return std::nullopt;
        }
        base = *m;
    } else {
        base = reinterpret_cast<std::uintptr_t>(module);
    }
    return runtime::image_path(base);
}


void* handle(const HleModule& m) { return const_cast<HleModule*>(&m); }


bool runs_code(DWORD flags) { return (flags & (0x1 | 0x2 | 0x20 | 0x40)) == 0; }


std::optional<void*> find_module(const std::string& path, bool load) {
    std::size_t cut = path.find_last_of("\\/");
    std::string file = cut == std::string::npos ? path : path.substr(cut + 1);

    if (file.find('.') == std::string::npos) {
        file += ".dll";
    }
    if (load) {
        auto [base, error] = runtime::load_title_dll(file);
        if (base) {
            return reinterpret_cast<void*>(*base);
        }
        if (error) {


            log::error("victory_hle::modules::kernel", "mapping a title DLL failed", {{"dll", log::debug(file)}, {"error", *error}});
            SetLastError(ERROR_MOD_NOT_FOUND_);
            return nullptr;
        }
    }
    if (auto base = runtime::image_named(file)) {
        return reinterpret_cast<void*>(*base);
    }

    bool host_dll = HostPolicy::standard().allows(file);
    if (!host_dll) {
        for (const auto& m : registry()) {
            if (m.serves(file)) {
                if (load) {
                    log::info("victory_hle::modules::kernel", "loaded as a pseudo module", {{"dll", log::debug(file)}});
                }
                return handle(m);
            }
        }
    }
    if (host_dll) {
        return std::nullopt;
    }


    {
        static std::mutex lock;
        static std::vector<std::string> refused;
        std::string l = file;
        for (char& c : l) {
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        std::lock_guard g(lock);
        if (std::find(refused.begin(), refused.end(), l) == refused.end()) {
            log::warn("victory_hle::modules::kernel", "a host DLL outside the host policy: refused", {{"dll", log::debug(l)}});
            refused.push_back(l);
        }
    }
    SetLastError(ERROR_MOD_NOT_FOUND_);
    return nullptr;
}


std::optional<void*> host_override(void* module, const char* name) {
    if (!module || runtime::image_path(reinterpret_cast<std::uintptr_t>(module))) {
        return std::nullopt;
    }
    std::string symbol = reinterpret_cast<std::uintptr_t>(name) < 0x10000 ? "#" + std::to_string(reinterpret_cast<std::uintptr_t>(name)) : narrow(name);
    wchar_t path[260];
    DWORD len = ::GetModuleFileNameW(static_cast<HMODULE>(module), path, 260);
    std::string p = wide(std::wstring(path, std::min<DWORD>(len, 260)).c_str());
    std::size_t cut = p.rfind('\\');
    std::string file = cut == std::string::npos ? p : p.substr(cut + 1);
    if (auto a = lookup(file, symbol)) {
        return reinterpret_cast<void*>(*a);
    }
    return std::nullopt;
}


std::atomic<std::uintptr_t> TITLE_FILTER{0};

LONG WINAPI report_unhandled(EXCEPTION_POINTERS* info) {
    loader::crash::report_unhandled(info);
    std::uintptr_t f = TITLE_FILTER.load(std::memory_order_acquire);
    if (f == 0) {
        return 0;   
    }
    return reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(f)(info);
}

}   

HMODULE WINAPI GetModuleHandleW(LPCWSTR name) {
    if (!name) {
        return reinterpret_cast<HMODULE>(runtime::main_image().value_or(0));
    }
    if (auto m = find_module(wide(name), false)) {
        return static_cast<HMODULE>(*m);
    }
    return ::GetModuleHandleW(name);
}

HMODULE WINAPI GetModuleHandleA(LPCSTR name) {
    if (!name) {
        return reinterpret_cast<HMODULE>(runtime::main_image().value_or(0));
    }
    if (auto m = find_module(narrow(name), false)) {
        return static_cast<HMODULE>(*m);
    }
    return ::GetModuleHandleA(name);
}

DWORD WINAPI GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size) {
    auto path = title_path(module);
    if (!path) {
        return ::GetModuleFileNameW(module, out, size);
    }
    auto u8 = path->u8string();
    std::wstring w = to_wide(std::string(reinterpret_cast<const char*>(u8.data()), u8.size()));
    return copy_truncated(w.data(), w.size(), out, size);
}

DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR out, DWORD size) {
    auto path = title_path(module);
    if (!path) {
        return ::GetModuleFileNameA(module, out, size);
    }
    auto u8 = path->u8string();
    return copy_truncated(reinterpret_cast<const char*>(u8.data()), u8.size(), out, size);
}

HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    if (name) {
        if (auto m = find_module(narrow(name), runs_code(flags))) {
            return static_cast<HMODULE>(*m);
        }
    }
    return ::LoadLibraryExA(name, file, flags);
}

HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    if (name) {
        if (auto m = find_module(wide(name), runs_code(flags))) {
            return static_cast<HMODULE>(*m);
        }
    }
    return ::LoadLibraryExW(name, file, flags);
}

HMODULE WINAPI LoadLibraryA(LPCSTR name) { return LoadLibraryExA(name, nullptr, 0); }
HMODULE WINAPI LoadLibraryW(LPCWSTR name) { return LoadLibraryExW(name, nullptr, 0); }

FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name) {

    void* mod = module ? static_cast<void*>(module) : reinterpret_cast<void*>(runtime::main_image().value_or(0));
    for (const auto& m : registry()) {
        if (handle(m) != mod) {
            continue;
        }

        if (reinterpret_cast<std::uintptr_t>(name) < 0x10000) {
            log::warn("victory_hle::modules::kernel", "GetProcAddress by ordinal",
                      {{"module", log::debug(m.name)}, {"ordinal", log::debug(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(name)))}});
            return nullptr;
        }
        std::string n = narrow(name);
        for (const auto& e : m.exports) {
            if (n == e.name) {
                return reinterpret_cast<FARPROC>(e.address);
            }
        }
        log::warn("victory_hle::modules::kernel", "GetProcAddress: not implemented", {{"module", log::debug(m.name)}, {"name", n}});
        return nullptr;
    }
    if (auto a = host_override(mod, name)) {
        return reinterpret_cast<FARPROC>(*a);
    }
    return ::GetProcAddress(static_cast<HMODULE>(mod), name);
}

BOOL WINAPI FreeLibrary(HMODULE module) {

    for (const auto& m : registry()) {
        if (handle(m) == module) {
            return TRUE;
        }
    }
    if (runtime::image_path(reinterpret_cast<std::uintptr_t>(module))) {
        return TRUE;
    }
    return ::FreeLibrary(module);
}

BOOL WINAPI DisableThreadLibraryCalls(HMODULE module) {
    if (auto done = loader::disable_thread_calls(reinterpret_cast<std::uintptr_t>(module))) {
        return *done ? TRUE : FALSE;
    }
    return ::DisableThreadLibraryCalls(module);
}

namespace {
std::string trim_end_ascii(const std::string& s) {
    std::size_t b = s.size();
    while (b > 0 && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(0, b);
}
std::string trim_ascii(const std::string& s) {
    std::size_t a = 0, b = s.size();
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    while (a < b && space(s[a])) {
        ++a;
    }
    while (b > a && space(s[b - 1])) {
        --b;
    }
    return s.substr(a, b - a);
}
}   


void WINAPI OutputDebugStringA(LPCSTR text) {
    if (text) {
        std::string t = narrow(text);
        loader::crash::note_title_output(trim_ascii(t));
        log::info("title", trim_end_ascii(t));
    }
}

void WINAPI OutputDebugStringW(LPCWSTR text) {
    if (text) {
        std::string t = wide(text);
        loader::crash::note_title_output(trim_ascii(t));
        log::info("title", trim_end_ascii(t));
    }
}

namespace {


std::string callers() {
    void* frames[12];
    USHORT n = RtlCaptureStackBackTrace(2, 12, frames, nullptr);
    std::string out;
    for (USHORT i = 0; i < n; ++i) {
        out += (i ? " < " : "") + loader::crash::locate(reinterpret_cast<std::uintptr_t>(frames[i]));
    }
    return out;
}


int log_title_exit(const char* call, std::uint32_t code, const std::string& from) {
    log::warn("victory_hle::modules::kernel", "title ended the process: call " + std::string(call) + ", code " + std::to_string(code) + ", from " + from);
    std::fflush(stderr);
    return static_cast<int>(code);
}

}   


void __cdecl exit(int code) { std::exit(log_title_exit("exit", static_cast<std::uint32_t>(code), callers())); }


void WINAPI ExitProcess(UINT code) {
    log_title_exit("ExitProcess", code, callers());
    ::ExitProcess(code);
}


void __cdecl abort() {
    log_title_exit("abort", 3, callers());
    std::abort();
}

void __cdecl terminate() {
    log_title_exit("terminate", 3, callers());
    std::abort();
}

void __cdecl _invalid_parameter_noinfo_noreturn() {
    log_title_exit("_invalid_parameter_noinfo_noreturn", 3, callers());
    std::abort();
}


BOOL WINAPI TerminateProcess(HANDLE process, UINT code) {
    if (process == reinterpret_cast<HANDLE>(-1)) {
        log_title_exit("TerminateProcess", code, callers());
    }
    return ::TerminateProcess(process, code);
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    ::SetUnhandledExceptionFilter(report_unhandled);
    return reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(TITLE_FILTER.exchange(reinterpret_cast<std::uintptr_t>(filter), std::memory_order_acq_rel));
}

}   
