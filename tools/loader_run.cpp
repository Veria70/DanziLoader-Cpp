// SPDX-License-Identifier: GPL-3.0-or-later
// Milestone 6: the port's loader runs the synthetic title
// (tests/m6/fixture), as `m6-oracle run` runs it with the reference's. The
// harness's resolver: `fixture_host.dll!Log` and `!Value` are the harness's
// own functions (the "runtime"), its other names are left unresolved, the
// title's DLLs come from `fixture_*.dll` beside it, and everything else goes
// to the host. Every line the title logs, the binding reports, the entry
// point's result and the notices at process exit go to <out.txt>, line by
// line as they happen.
//
//   loader-run <title dir> <title exe> <cache dir> <out.txt> [trap|crash1|crash2]
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

#include "loader/crash.hpp"
#include "loader/loader.hpp"

using namespace danzi;

namespace {

std::mutex OUT_LOCK;
std::string OUT_PATH;

void out_line(const std::string& line) {
    std::lock_guard l(OUT_LOCK);
    std::ofstream f(OUT_PATH, std::ios::binary | std::ios::app);
    f << line << "\n";
}

void __stdcall harness_log(const char* text) { out_line(std::string("log ") + text); }
unsigned __stdcall harness_value(unsigned x) { return x + 1000; }
unsigned MODE = 0;
unsigned __stdcall harness_mode() { return MODE; }

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return o;
}

class TestResolver : public loader::ImportResolver {
public:
    loader::Resolution resolve(std::string_view dll, const pe::ImportSymbol& symbol) const override {
        std::string l = lower(dll);
        if (l == "fixture_host.dll") {
            if (!symbol.by_ordinal && symbol.name == "Log") {
                return loader::Resolution::at(reinterpret_cast<std::uintptr_t>(&harness_log), loader::Provenance::Hle);
            }
            if (!symbol.by_ordinal && symbol.name == "Mode") {
                return loader::Resolution::at(reinterpret_cast<std::uintptr_t>(&harness_mode), loader::Provenance::Hle);
            }
            if (!symbol.by_ordinal && symbol.name == "Value") {
                return loader::Resolution::at(reinterpret_cast<std::uintptr_t>(&harness_value), loader::Provenance::Hle);
            }
            return loader::Resolution::missing();
        }
        if (l.starts_with("fixture_")) {
            return loader::Resolution::missing();
        }
        HMODULE h = LoadLibraryA(std::string(dll).c_str());
        if (!h) {
            return loader::Resolution::missing();
        }
        FARPROC p = symbol.by_ordinal ? GetProcAddress(h, reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(symbol.ordinal)))
                                      : GetProcAddress(h, symbol.name.c_str());
        return p ? loader::Resolution::at(reinterpret_cast<std::uintptr_t>(p), loader::Provenance::Host) : loader::Resolution::missing();
    }
};

std::string report_line(const std::string& who, const loader::BindReport& r) {
    std::string s = "bind " + who + ": hle=" + std::to_string(r.hle) + " host=" + std::to_string(r.host) + " title=" + std::to_string(r.title) +
                    " missing=[";
    for (std::size_t k = 0; k < r.missing.size(); ++k) {
        s += (k ? ", " : "") + r.missing[k].dll + "!" + r.missing[k].symbol;
    }
    return s + "]";
}

}   

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: loader-run <title dir> <title exe> <cache dir> <out.txt>\n");
        return 2;
    }
    std::filesystem::path title_dir = argv[1], cache = argv[3];
    OUT_PATH = argv[4];

    bool trap = argc > 5 && std::string(argv[5]) == "trap";


    if (argc > 5 && std::string(argv[5]).starts_with("crash")) {
        MODE = static_cast<unsigned>(argv[5][5] - '0');
        loader::crash::log_crashes_to(OUT_PATH + ".crash");
        loader::crash::report_crashes();
    }
    auto policy = trap ? loader::UnimplementedPolicy::Trap : loader::UnimplementedPolicy::ReturnZero;
    if (trap) {
        loader::crash::log_crashes_to(OUT_PATH + ".crash");
    }
    std::ofstream(OUT_PATH, std::ios::binary | std::ios::trunc).close();
    try {
        std::ifstream in(title_dir / argv[2], std::ios::binary | std::ios::ate);
        std::vector<std::uint8_t> original(static_cast<std::size_t>(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(original.data()), static_cast<std::streamsize>(original.size()));
        auto prepared = loader::prepare(original);
        auto image = loader::load(prepared, cache);
        auto dlls = loader::load_title_dlls(image->info(), title_dir, cache, [](const std::string& n) { return n.starts_with("fixture_dll"); });
        TestResolver host;
        loader::WithTitleDlls resolver(host, dlls);
        for (const auto& [name, dll] : dlls) {
            out_line(report_line(name, dll->bind_imports(resolver, policy)));
        }
        out_line(report_line("main", image->bind_imports(resolver, policy)));
        auto main_thread = loader::spawn_main(image, dlls);
        std::uint32_t code = main_thread.join();
        out_line("exit " + std::to_string(code));
        out_line("unimplemented calls " + std::to_string(loader::unimplemented_call_count()));
    } catch (const std::exception& e) {
        out_line(std::string("error ") + e.what());
        return 1;
    }
    out_line("process exit");
    return 0;
}
