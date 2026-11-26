// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime's table, the host policy and the resolver (`victory-hle`:
// `registry.rs`, `host.rs`, `lib.rs`; DanziLoader / victory-perf, see
// NOTICE.md). See registry.hpp.
#include "registry.hpp"

#include <cstdlib>
#include <map>

#include "loader/crash.hpp"
#include <mutex>

namespace danzi::hle {

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
    }
    return o;
}

bool matches(std::string_view pattern, const std::string& dll) {
    if (!pattern.empty() && pattern.back() == '*') {
        return dll.starts_with(pattern.substr(0, pattern.size() - 1));
    }
    return dll == pattern;
}


std::mutex MODULES_LOCK;
std::map<std::string, HMODULE> MODULES;

HMODULE system_module(std::string_view dll) {
    std::string key = lower(dll);
    std::lock_guard l(MODULES_LOCK);
    if (auto it = MODULES.find(key); it != MODULES.end()) {
        return it->second;
    }
    if (key.find('\0') != std::string::npos) {
        return nullptr;
    }

    HMODULE h = LoadLibraryExA(key.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h) {
        MODULES.emplace(key, h);
    }
    return h;
}

std::optional<std::uintptr_t> host_resolve(std::string_view dll, const pe::ImportSymbol& symbol) {
    HMODULE m = system_module(dll);
    if (!m) {
        return std::nullopt;
    }
    FARPROC p = nullptr;
    if (symbol.by_ordinal) {
        p = GetProcAddress(m, reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(symbol.ordinal)));
    } else if (symbol.name.find('\0') == std::string::npos) {
        p = GetProcAddress(m, symbol.name.c_str());
    }
    return p ? std::optional(reinterpret_cast<std::uintptr_t>(p)) : std::nullopt;
}

}   

bool HleModule::serves(std::string_view dll) const {
    std::string l = lower(dll);
    for (const char* p : dlls) {
        if (matches(p, l)) {
            return true;
        }
    }
    return false;
}

const std::vector<HleModule>& registry() {
    static const std::vector<HleModule> table = [] {
        std::vector<HleModule> all = all_modules();
        for (auto& m : all) {
            for (auto& e : m.exports) {
                if (e.address == 0) {
                    e.address = loader::not_ported_stub(m.name, e.name);
                }
            }
        }
        return all;
    }();
    return table;
}

std::optional<std::uintptr_t> lookup(std::string_view dll, std::string_view name) {
    for (const auto& m : registry()) {
        if (!m.serves(dll)) {
            continue;
        }
        for (const auto& e : m.exports) {
            if (name == e.name) {
                return e.address;
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> not_ported() {
    std::vector<std::string> out;
    for (const auto& m : all_modules()) {
        for (const auto& e : m.exports) {
            if (e.address == 0) {
                out.push_back(std::string(m.name) + "!" + e.name);
            }
        }
    }
    return out;
}

HostPolicy HostPolicy::standard() {
    return HostPolicy{{
        "api-ms-win-core-*",
        "api-ms-win-eventing-*",
        "api-ms-win-security-*",
        "api-ms-win-crt-*",
        "kernel32.dll",
        "kernelbase.dll",
        "ntdll.dll",
        "ucrtbase.dll",
        "vcruntime140*",
        "msvcp140*",

        "ws2_32.dll",
        "bcrypt.dll",
        "crypt32.dll",
        "ncrypt.dll",
        "winhttp.dll",
        "sspicli.dll",
        "cabinet.dll",
        "avrt.dll",


        "ext-ms-win-rtcore-ntuser-*",
        "api-ms-win-shcore-*",
        "msvcp_win.dll",
        "ole32.dll",
        "advapi32.dll",
        "iphlpapi.dll",

        "dbghelp.dll",


        "xaudio2_9.dll",
        "mfplat.dll",
        "mfreadwrite.dll",
    }};
}

bool HostPolicy::allows(std::string_view dll) const {
    std::string l = lower(dll);
    for (const auto& p : allow) {
        if (matches(p, l)) {
            return true;
        }
    }
    return false;
}

std::optional<std::uintptr_t> HostPolicy::resolve(std::string_view dll, const pe::ImportSymbol& symbol) const {
    if (!allows(dll)) {
        return std::nullopt;
    }
    return host_resolve(dll, symbol);
}

std::optional<std::uintptr_t> host_function(std::string_view dll, std::string_view name) {
    return host_resolve(dll, pe::ImportSymbol::named(0, std::string(name)));
}

std::uintptr_t must_host(const char* dll, const char* name) {
    if (auto a = host_function(dll, name)) {
        return *a;
    }
    loader::crash::log_crash(std::string("the host lacks ") + dll + "!" + name);
    std::abort();
}

loader::Resolution HleResolver::resolve(std::string_view dll, const pe::ImportSymbol& symbol) const {

    if (auto a = lookup(dll, symbol.to_string())) {
        return loader::Resolution::at(*a, loader::Provenance::Hle);
    }
    if (auto a = host_.resolve(dll, symbol)) {
        return loader::Resolution::at(*a, loader::Provenance::Host);
    }
    return loader::Resolution::missing();
}

std::optional<loader::Provenance> HleResolver::classify(std::string_view dll, const pe::ImportSymbol& symbol) const {
    auto r = resolve(dll, symbol);
    return r.found ? std::optional(r.provenance) : std::nullopt;
}

}   
