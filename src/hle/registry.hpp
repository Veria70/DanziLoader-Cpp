// SPDX-License-Identifier: GPL-3.0-or-later
// Our implementation of the Xbox system API surface (`victory-hle`:
// `lib.rs`, `registry.rs`, `host.rs`; DanziLoader / victory-perf, see
// NOTICE.md). A title imports from three kinds of DLLs:
// - Windows API sets the console shares with PC: the host, overridden here
//   where the console differs (paths, memory);
// - console-only system APIs (`xgameruntime.dll`, `xmem.dll`,
//   `d3d12_xs.dll`): implemented here;
// - anything else: an unimplemented-import stub from the loader.
// Every import resolves to an implementation in the table first, then to
// the host if `HostPolicy` allows that DLL, else it is missing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "loader/loader.hpp"

namespace danzi::hle {


struct HleExport {
    const char* name;
    std::uintptr_t address;
};


struct HleModule {
    const char* name;
    std::vector<const char*> dlls;
    std::vector<HleExport> exports;
    bool serves(std::string_view dll) const;
};


const std::vector<HleModule>& registry();


std::optional<std::uintptr_t> lookup(std::string_view dll, std::string_view name);

std::vector<std::string> not_ported();


struct HostPolicy {

    std::vector<std::string> allow;
    static HostPolicy standard();

    static HostPolicy none() { return {}; }
    bool allows(std::string_view dll) const;
    std::optional<std::uintptr_t> resolve(std::string_view dll, const pe::ImportSymbol& symbol) const;
};


std::optional<std::uintptr_t> host_function(std::string_view dll, std::string_view name);


std::uintptr_t must_host(const char* dll, const char* name);

#define HOST(dll, name, type) ([] { using T_ = type; static const T_ f = reinterpret_cast<T_>(::danzi::hle::must_host(dll, name)); return f; }())


class HleResolver : public loader::ImportResolver {
public:
    explicit HleResolver(HostPolicy host = HostPolicy::standard()) : host_(std::move(host)) {}
    loader::Resolution resolve(std::string_view dll, const pe::ImportSymbol& symbol) const override;

    std::optional<loader::Provenance> classify(std::string_view dll, const pe::ImportSymbol& symbol) const;

private:
    HostPolicy host_;
};


std::vector<HleModule> all_modules();

}   
