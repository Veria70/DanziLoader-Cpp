// SPDX-License-Identifier: GPL-3.0-or-later
// Milestone 6: the port's PE reading as text, the same as `m6-oracle`'s
// (oracle-m6/src/main.rs), for a list of files (one path a line).
//
//   pe-check pe <list.txt> <out.txt>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "pe/image.hpp"
#include "pe/implib.hpp"
#include "loader/prepare.hpp"
#include "core/policy.hpp"
#include "hle/registry.hpp"

#include <blake3.h>

using namespace danzi::pe;

namespace {

std::string esc(const std::string& s) {
    std::string out;
    for (unsigned char b : s) {
        if (b >= 0x21 && b <= 0x7e && b != '\\') {
            out.push_back(static_cast<char>(b));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", b);
            out += buf;
        }
    }
    if (out.empty()) {
        out = "\\e";
    }
    return out;
}

std::string hex(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%" PRIx64, v);
    return buf;
}

std::string opt_hex(std::optional<std::uint32_t> v) { return v ? hex(*v) : "-"; }

std::string sym(const ImportSymbol& s) {
    if (s.by_ordinal) {
        return "o:" + std::to_string(s.ordinal);
    }
    return "n:" + esc(s.name) + ":" + std::to_string(s.hint);
}

void line(std::string& out, const std::string& l) {
    out += l;
    out += '\n';
}

void image_block(std::string& out, const std::vector<std::uint8_t>& data) {
    ImageInfo info;
    try {
        info = ImageInfo::parse(data);
    } catch (const PeError& e) {
        line(out, "err " + esc(e.display()));
        return;
    }
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "ok m=%04x c=%04x ts=%08x sub=%u %s dc=%04x base=%" PRIx64 " size=%x entry=%x stack=%" PRIx64 "/%" PRIx64
                  " reloc=%d exc=%d lc=%d dll=%d imports=%zu",
                  info.machine, info.characteristics, info.timestamp, info.subsystem, info.subsystem_name(), info.dll_characteristics, info.image_base,
                  info.size_of_image, info.entry_rva, info.stack_reserve, info.stack_commit, info.has_relocations ? 1 : 0,
                  info.has_exception_table ? 1 : 0, info.has_load_config ? 1 : 0, info.is_dll() ? 1 : 0, info.import_count());
    line(out, buf);
    for (const auto& s : info.sections) {
        std::snprintf(buf, sizeof buf, " %x %x %x %x %08x off=", s.rva, s.virtual_size, s.file_offset, s.file_size, s.characteristics);
        line(out, "sec " + esc(s.name) + buf + opt_hex(info.rva_to_file_offset(s.rva)) + "," + opt_hex(info.rva_to_file_offset(s.rva + 0x10)));
    }
    line(out, "entry-off " + opt_hex(info.rva_to_file_offset(info.entry_rva)));
    for (int k = 0; k < 2; ++k) {
        const auto& list = k == 0 ? info.imports : info.delay_imports;
        for (const auto& m : list) {
            line(out, std::string(k == 0 ? "imp " : "dimp ") + esc(m.dll) + " " + std::to_string(m.entries.size()));
            for (const auto& e : m.entries) {
                line(out, " i " + sym(e.symbol) + " " + hex(e.iat_slot_rva) + " " + esc(e.symbol.to_string()));
            }
        }
    }
    for (const auto& e : info.exports) {
        line(out, "exp " + (e.name ? esc(*e.name) : "-") + " " + std::to_string(e.ordinal) + " " + opt_hex(e.rva) + " " + (e.forward ? esc(*e.forward) : "-"));
    }
    if (info.tls) {
        std::string rvas;
        for (std::size_t k = 0; k < info.tls->callback_rvas.size(); ++k) {
            rvas += (k ? "," : "") + hex(info.tls->callback_rvas[k]);
        }
        line(out, "tls " + hex(info.tls->directory_rva) + " " + hex(info.tls->callbacks_va) + " [" + rvas + "]");
    }
    if (info.codeview) {
        std::string guid;
        for (std::uint8_t b : info.codeview->guid) {
            std::snprintf(buf, sizeof buf, "%02x", b);
            guid += buf;
        }
        line(out, "cv " + esc(info.codeview->pdb_path) + " " + guid + " " + std::to_string(info.codeview->age) + " " + info.codeview->symbol_key());
    }
}

void lib_block(std::string& out, const std::vector<std::uint8_t>& data) {
    try {
        ImportLibrary lib = ImportLibrary::parse(data);
        line(out, "lib other=" + std::to_string(lib.other_members) + " imports=" + std::to_string(lib.imports.size()));
        for (const auto& i : lib.imports) {
            const char* kind = i.kind == LibImport::Code ? "code" : i.kind == LibImport::Data ? "data" : "const";
            line(out, "li " + esc(i.dll) + " " + sym(i.export_) + " " + kind);
        }
    } catch (const PeError& e) {
        line(out, "err " + esc(e.display()));
    }
}

std::optional<std::vector<std::uint8_t>> read(const std::string& path) {

    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary | std::ios::ate);
    if (!f) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (static_cast<std::size_t>(f.gcount()) != out.size()) {
        return std::nullopt;
    }
    return out;
}

}   

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    if (args.size() >= 4 && args[1] == "pe") {
        std::ifstream list(args[2]);
        std::string out;
        for (std::string path; std::getline(list, path);) {
            while (!path.empty() && (path.back() == '\r' || path.back() == ' ' || path.back() == '\t')) {
                path.pop_back();
            }
            std::size_t start = path.find_first_not_of(" \t");
            if (start == std::string::npos) {
                continue;
            }
            path = path.substr(start);
            line(out, "== " + esc(path));
            auto data = read(path);
            if (!data) {
                line(out, "unreadable");
                continue;
            }
            std::string lower = path;
            for (char& c : lower) {
                c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
            }
            if (lower.size() >= 4 && lower.ends_with(".lib")) {
                lib_block(out, *data);
            } else {
                image_block(out, *data);
            }
        }
        std::ofstream(args[3], std::ios::binary) << out;
        return 0;
    }
    if (args.size() >= 4 && args[1] == "prepare") {
        std::ifstream list(args[2]);
        std::string out;
        for (std::string path; std::getline(list, path);) {
            while (!path.empty() && (path.back() == '\r' || path.back() == ' ' || path.back() == '\t')) {
                path.pop_back();
            }
            std::size_t start = path.find_first_not_of(" \t");
            if (start == std::string::npos) {
                continue;
            }
            path = path.substr(start);
            line(out, "== " + esc(path));
            auto data = read(path);
            if (!data) {
                line(out, "unreadable");
                continue;
            }
            try {
                auto p = danzi::loader::prepare(*data);
                std::uint8_t hash[32];
                blake3_hasher h;
                blake3_hasher_init(&h);
                blake3_hasher_update(&h, p.bytes.data(), p.bytes.size());
                blake3_hasher_finalize(&h, hash, 32);
                std::string hx;
                char b[4];
                for (std::uint8_t x : hash) {
                    std::snprintf(b, sizeof b, "%02x", x);
                    hx += b;
                }
                line(out, "ok " + hx + " " + p.file_name() + " " + std::to_string(p.bytes.size()));
            } catch (const danzi::loader::PrepareError& e) {
                line(out, "err " + esc(e.display()));
            }
        }
        std::ofstream(args[3], std::ios::binary) << out;
        return 0;
    }
    if (args.size() >= 4 && args[1] == "policy") {
        std::ifstream list(args[2]);
        std::string out;
        for (std::string path; std::getline(list, path);) {
            while (!path.empty() && (path.back() == '\r' || path.back() == ' ' || path.back() == '\t')) {
                path.pop_back();
            }
            std::size_t start = path.find_first_not_of(" \t");
            if (start == std::string::npos) {
                continue;
            }
            path = path.substr(start);
            try {
                danzi::policy::ensure_outside_repo(std::filesystem::path(std::u8string(path.begin(), path.end())));
                line(out, esc(path) + " ok");
            } catch (const danzi::policy::PolicyError& e) {
                line(out, esc(path) + " err " + esc(e.what()));
            }
        }
        std::ofstream(args[3], std::ios::binary) << out;
        return 0;
    }
    if (args.size() >= 3 && args[1] == "hle-registry") {
        std::string out;
        for (const auto& m : danzi::hle::registry()) {
            std::string dlls;
            for (std::size_t k = 0; k < m.dlls.size(); ++k) {
                dlls += (k ? "," : "") + std::string(m.dlls[k]);
            }
            line(out, std::string("module ") + m.name + " dlls=" + dlls);
            for (const auto& e : m.exports) {
                line(out, std::string(" export ") + e.name);
            }
        }
        std::ofstream(args[2], std::ios::binary) << out;
        return 0;
    }
    if (args.size() >= 4 && args[1] == "hle-classify") {
        std::ifstream list(args[2]);
        danzi::hle::HleResolver resolver;
        std::string out;
        for (std::string path; std::getline(list, path);) {
            while (!path.empty() && (path.back() == '\r' || path.back() == ' ' || path.back() == '\t')) {
                path.pop_back();
            }
            std::size_t start = path.find_first_not_of(" \t");
            if (start == std::string::npos) {
                continue;
            }
            path = path.substr(start);
            line(out, "== " + esc(path));
            auto data = read(path);
            if (!data) {
                line(out, "unreadable");
                continue;
            }
            ImageInfo info;
            try {
                info = ImageInfo::parse(*data);
            } catch (const PeError&) {
                line(out, "not an image");
                continue;
            }
            for (const auto* list2 : {&info.imports, &info.delay_imports}) {
                for (const auto& m : *list2) {
                    for (const auto& e : m.entries) {
                        auto c = resolver.classify(m.dll, e.symbol);
                        const char* cls = !c ? "missing" : *c == danzi::loader::Provenance::Hle ? "hle" : *c == danzi::loader::Provenance::Host ? "host" : "title";
                        line(out, esc(m.dll) + "!" + esc(e.symbol.to_string()) + " " + cls);
                    }
                }
            }
        }
        std::ofstream(args[3], std::ios::binary) << out;
        return 0;
    }
    std::fprintf(stderr, "usage: pe-check pe|prepare|policy <list.txt> <out.txt> | hle-registry <out.txt> | hle-classify <list.txt> <out.txt>\n");
    return 2;
}
