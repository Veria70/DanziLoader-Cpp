// SPDX-License-Identifier: GPL-3.0-or-later
// Where the user's files live (`victory-core/src/config.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See config.hpp.
#include "config.hpp"

#include <windows.h>

#include <fstream>
#include <sstream>

#include "env.hpp"
#include "policy.hpp"
#include "toml.hpp"

namespace danzi::config {

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }

    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

[[noreturn]] void fail(std::string message) { throw Error({std::move(message)}); }


class FileReader {
public:
    explicit FileReader(std::string_view text) : text_(text) {}

    FilePaths read(const toml::Node& root) {

        FilePaths out;
        for (const auto& f : root.fields) {
            if (f.key != "paths") {
                error("unknown field `" + f.key + "`, expected `paths`", f.key_start, f.key_end);
            }
            out = paths(f.value);
        }
        return out;
    }

private:
    std::string_view text_;

    [[noreturn]] void error(std::string message, std::size_t start, std::size_t end) const {
        fail(toml::display(toml::error_at(text_, std::move(message), start, end), text_));
    }


    [[noreturn]] void invalid_type(const toml::Node& n, const char* expected) const {
        std::string what, err;
        if (!toml::unexpected(n, what, err)) {
            error(err, n.start, n.end);
        }
        error("invalid type: " + what + ", expected " + expected, n.start, n.end);
    }


    fs::path path(const toml::Node& n) const {
        if (n.kind != toml::Node::Kind::String) {
            invalid_type(n, "path string");
        }
        return from_utf8(n.text);
    }


    std::vector<fs::path> paths_list(const toml::Node& n) const {
        if (n.kind != toml::Node::Kind::Array) {
            invalid_type(n, "a sequence");
        }
        std::vector<fs::path> out;
        for (const auto& item : n.items) {
            out.push_back(path(item));
        }
        return out;
    }


    FilePaths paths(const toml::Node& n) const {
        constexpr const char* kExpected = "expected one of `game_dir`, `cache_dir`, `data_dir`, `test_titles`";
        FilePaths out;
        switch (n.kind) {
        case toml::Node::Kind::Table:
            for (const auto& f : n.fields) {
                if (f.key == "game_dir") {
                    out.game_dir = path(f.value);
                } else if (f.key == "cache_dir") {
                    out.cache_dir = path(f.value);
                } else if (f.key == "data_dir") {
                    out.data_dir = path(f.value);
                } else if (f.key == "test_titles") {
                    out.test_titles = paths_list(f.value);
                } else {
                    error("unknown field `" + f.key + "`, " + kExpected, f.key_start, f.key_end);
                }
            }
            return out;
        case toml::Node::Kind::Datetime:
            error(std::string("unknown field `$__toml_private_datetime`, ") + kExpected, n.start, n.end);
        case toml::Node::Kind::Array: {
            auto element = [&](std::size_t i) -> const toml::Node* {
                if (i < n.items.size()) {
                    return &n.items[i];
                }
                if (i == 3) {
                    return nullptr;   
                }
                error("invalid length " + std::to_string(i) + ", expected struct FilePaths with 4 elements", n.start,
                      n.end);
            };
            out.game_dir = path(*element(0));
            out.cache_dir = path(*element(1));
            out.data_dir = path(*element(2));
            if (const toml::Node* t = element(3)) {
                out.test_titles = paths_list(*t);
            }
            return out;
        }
        default: invalid_type(n, "struct FilePaths");
        }
    }
};

}   

Error Error::context(std::string context, const std::exception& inner) {
    std::vector<std::string> chain{std::move(context)};
    if (auto* e = dynamic_cast<const Error*>(&inner)) {
        chain.insert(chain.end(), e->chain().begin(), e->chain().end());
    } else {
        chain.emplace_back(inner.what());
    }
    return Error(std::move(chain));
}

std::string display_chain(const std::vector<std::string>& chain) {
    if (chain.empty()) {
        return {};
    }
    std::string out = chain[0];
    if (chain.size() == 1) {
        return out;
    }
    out += "\n\nCaused by:";
    bool numbered = chain.size() > 2;
    for (std::size_t i = 1; i < chain.size(); ++i) {
        std::string head = numbered ? "    " + std::to_string(i - 1) + ": " : "    ";
        std::string indent(head.size(), ' ');
        out += "\n" + head;
        const std::string& m = chain[i];
        for (std::size_t k = 0; k < m.size(); ++k) {
            out += m[k];


            if (m[k] == '\n') {
                out += indent;
            }
        }
    }
    return out;
}

std::string display_alternate(const std::vector<std::string>& chain) {
    std::string out;
    for (std::size_t i = 0; i < chain.size(); ++i) {
        out += (i ? ": " : "") + chain[i];
    }
    return out;
}

std::string display(const fs::path& p) { return utf8(p.native()); }

FilePaths parse_file(std::string_view text) {
    auto parsed = toml::parse_document(text);
    if (auto* e = std::get_if<toml::ParseError>(&parsed)) {
        fail(toml::display(*e, text));
    }
    return FileReader(text).read(std::get<toml::Node>(parsed));
}

namespace {


fs::path default_dir(const char* name) {
    std::optional<fs::path> base;
    if (auto v = env::get(L"LOCALAPPDATA")) {
        base = fs::path(*v);
    } else if (auto x = env::get(L"XDG_CACHE_HOME")) {
        base = fs::path(*x);
    } else if (auto h = env::get(L"HOME")) {
        base = fs::path(*h) / L".cache";
    }
    if (!base) {
        fail("cannot determine a user directory; set VICTORY_CACHE_DIR and VICTORY_DATA_DIR");
    }
    return *base / L"victory" / name;
}

fs::path subdir(const fs::path& root, std::string_view sub) {
    fs::path dir = root / from_utf8(std::string(sub));
    policy::ensure_outside_repo(dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw Error({"creating " + display(dir), ec.message() + " (os error " + std::to_string(ec.value()) + ")"});
    }
    return dir;
}

}   

Config Config::resolve(FilePaths file, std::optional<fs::path> source, const std::function<std::optional<std::wstring>(const wchar_t*)>& env) {
    auto env_path = [&](const wchar_t* k) -> std::optional<fs::path> {
        auto v = env(k);
        if (!v || v->empty()) {
            return std::nullopt;
        }
        return fs::path(*v);
    };
    std::vector<fs::path> test_titles;
    if (auto v = env(L"VICTORY_TEST_TITLES"); v && !v->empty()) {

        std::wstring part;
        bool quoted = false;
        auto push = [&] {
            if (!part.empty()) {
                test_titles.emplace_back(part);
            }
            part.clear();
        };
        for (wchar_t c : *v) {
            if (c == L'"') {
                quoted = !quoted;
            } else if (c == L';' && !quoted) {
                push();
            } else {
                part += c;
            }
        }
        push();
    } else {
        test_titles = std::move(file.test_titles);
    }
    Config c;
    auto cache = env_path(L"VICTORY_CACHE_DIR");
    c.cache_dir = cache ? *cache : file.cache_dir ? *file.cache_dir : default_dir("cache");
    auto data = env_path(L"VICTORY_DATA_DIR");
    c.data_dir = data ? *data : file.data_dir ? *file.data_dir : default_dir("data");
    auto game = env_path(L"VICTORY_GAME_DIR");
    c.game_dir = game ? game : file.game_dir;
    c.test_titles = std::move(test_titles);
    c.source = std::move(source);
    return c;
}

Config Config::load() {
    auto explicit_ = env::get(L"VICTORY_CONFIG");
    fs::path path = explicit_ ? fs::path(*explicit_) : fs::path(L"victory.toml");
    std::error_code ec;
    FilePaths file;
    std::optional<fs::path> source;
    if (fs::is_regular_file(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.good() && !in.eof()) {
            fail("reading " + display(path));
        }

        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) == 0 && !text.empty()) {
            throw Error({"reading " + display(path), "stream did not contain valid UTF-8"});
        }
        try {
            file = parse_file(text);
        } catch (const std::exception& e) {
            throw Error::context("parsing " + display(path), e);
        }
        source = path;
    } else if (explicit_) {
        fail("VICTORY_CONFIG points at " + display(path) + ", which does not exist");
    }
    return resolve(std::move(file), std::move(source), [](const wchar_t* k) { return env::get(k); });
}

const fs::path& Config::require_game_dir() const {
    if (!game_dir) {
        fail("no game directory configured: set VICTORY_GAME_DIR or [paths].game_dir in victory.toml (see victory.example.toml)");
    }
    return *game_dir;
}

fs::path Config::cache_subdir(std::string_view sub) const { return subdir(cache_dir, sub); }
fs::path Config::data_subdir(std::string_view sub) const { return subdir(data_dir, sub); }

}   
