// SPDX-License-Identifier: GPL-3.0-or-later
// The C++ side of the TOML differential test (tools/check_toml.py): parses
// every document of a corpus with `danzi::toml::parse` and prints what
// build/toml-oracle prints for the reference's `toml` crate: the table in a
// canonical dump plus `to_string` of it, or the error's display.
//
// Corpus and output files are sequences of records: u32 LE length + bytes.
//
// usage: toml-check [config] <corpus.bin> <out.bin>
// (`config`: `config::parse_file`, the reference's config structs)

#include "core/config.hpp"
#include "core/toml.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace danzi;

void esc(std::string_view s, std::string& out) {
    out += '"';
    for (char ch : s) {
        const auto b = static_cast<unsigned char>(ch);
        if (b >= 0x20 && b <= 0x7E && b != '"' && b != '\\') {
            out += ch;
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", b);
            out += buf;
        }
    }
    out += '"';
}

void indent(std::size_t n, std::string& out) { out.append(2 * n, ' '); }

void dump_table(const toml::Table& t, std::size_t depth, std::string& out);

void dump(const toml::Value& v, std::size_t depth, std::string& out) {
    char buf[64];
    if (auto s = v.as_string()) {
        out += "str ";
        esc(*s, out);
        out += '\n';
    } else if (auto i = v.as_integer()) {
        out += "int " + std::to_string(*i) + "\n";
    } else if (auto f = v.as_float()) {
        unsigned long long bits = 0;
        std::memcpy(&bits, f, sizeof bits);
        std::snprintf(buf, sizeof buf, "float %016llx\n", bits);
        out += buf;
    } else if (auto b = v.as_bool()) {
        out += *b ? "bool true\n" : "bool false\n";
    } else if (v.is_datetime()) {
        out += "dt " + toml::to_string(std::get<toml::Datetime>(v.v)) + "\n";
    } else if (auto a = v.as_array()) {
        out += "array " + std::to_string(a->size()) + "\n";
        for (const auto& e : *a) {
            indent(depth + 1, out);
            dump(e, depth + 1, out);
        }
    } else {
        dump_table(*v.as_table(), depth, out);
    }
}

void dump_table(const toml::Table& t, std::size_t depth, std::string& out) {
    out += "table " + std::to_string(t.entries.size()) + "\n";
    for (const auto& [k, v] : t.entries) {
        indent(depth + 1, out);
        esc(k, out);
        out += " => ";
        dump(v, depth + 1, out);
    }
}

std::string run(std::string_view input) {
    std::string out;
    auto parsed = toml::parse(input);
    if (auto* e = std::get_if<toml::ParseError>(&parsed)) {
        out += "ERR\n";
        out += toml::display(*e, input);
        return out;
    }
    auto& table = std::get<toml::Table>(parsed);
    out += "OK\n";
    dump_table(table, 0, out);
    out += "DISPLAY ";
    out += toml::to_string(toml::Value{std::move(table)});
    out += '\n';
    return out;
}

void opt_path(const char* name, const std::optional<config::fs::path>& p, std::string& out) {
    out += name;
    if (p) {
        out += ' ';
        esc(config::display(*p), out);
    } else {
        out += " none";
    }
    out += '\n';
}


std::string run_config(std::string_view input) {
    std::string out;
    try {
        const config::FilePaths f = config::parse_file(input);
        out += "OK\n";
        opt_path("game_dir", f.game_dir, out);
        opt_path("cache_dir", f.cache_dir, out);
        opt_path("data_dir", f.data_dir, out);
        out += "test_titles " + std::to_string(f.test_titles.size()) + "\n";
        for (const auto& t : f.test_titles) {
            out += "  ";
            esc(config::display(t), out);
            out += '\n';
        }
    } catch (const config::Error& e) {
        out += "ERR\n";
        out += e.chain().front();   
    }
    return out;
}

}   

int main(int argc, char** argv) {

    const bool config_mode = argc == 4 && std::string_view(argv[1]) == "config";
    if (config_mode) {
        ++argv;
        --argc;
    }
    if (argc != 3) {
        std::fprintf(stderr, "usage: toml-check [config] <corpus.bin> <out.bin>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::string corpus((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    std::size_t at = 0;
    while (at + 4 <= corpus.size()) {
        std::uint32_t len = 0;
        std::memcpy(&len, corpus.data() + at, 4);
        const std::string_view text(corpus.data() + at + 4, len);
        at += 4 + len;
        const std::string result = config_mode ? run_config(text) : run(text);
        const auto n = static_cast<std::uint32_t>(result.size());
        out.append(reinterpret_cast<const char*>(&n), 4);
        out += result;
    }
    std::ofstream(argv[2], std::ios::binary).write(out.data(), static_cast<std::streamsize>(out.size()));
    return 0;
}
