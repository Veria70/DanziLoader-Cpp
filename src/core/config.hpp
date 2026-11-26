// SPDX-License-Identifier: GPL-3.0-or-later
// Where the user's files live (`victory-core/src/config.rs`; DanziLoader /
// victory-perf, see NOTICE.md).
//
// Resolution order for each value: environment variable, then the config
// file (`VICTORY_CONFIG`, else `./victory.toml`), then the default. See
// `victory.example.toml` in the reference.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace danzi::config {

namespace fs = std::filesystem;


class Error : public std::runtime_error {
public:
    explicit Error(std::vector<std::string> chain) : std::runtime_error(chain.empty() ? std::string() : chain.front()), chain_(std::move(chain)) {}

    static Error context(std::string context, const std::exception& inner);
    const std::vector<std::string>& chain() const { return chain_; }

private:
    std::vector<std::string> chain_;
};


std::string display_chain(const std::vector<std::string>& chain);

std::string display_alternate(const std::vector<std::string>& chain);


struct FilePaths {
    std::optional<fs::path> game_dir, cache_dir, data_dir;
    std::vector<fs::path> test_titles;
};

struct Config {

    std::optional<fs::path> game_dir;

    fs::path cache_dir;


    fs::path data_dir;

    std::vector<fs::path> test_titles;

    std::optional<fs::path> source;


    static Config load();

    static Config resolve(FilePaths file, std::optional<fs::path> source, const std::function<std::optional<std::wstring>(const wchar_t*)>& env);

    const fs::path& require_game_dir() const;

    fs::path cache_subdir(std::string_view sub) const;

    fs::path data_subdir(std::string_view sub) const;
};


FilePaths parse_file(std::string_view text);


std::string display(const fs::path& p);

}   
