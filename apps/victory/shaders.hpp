// SPDX-License-Identifier: GPL-3.0-or-later
// The shader commands of the command line (`apps/victory/src/shader.rs`,
// `translate.rs`, `precompile.rs`; DanziLoader / victory-perf, see
// NOTICE.md): `shader-info`, `translate` and `precompile`, and what they
// share (the shader containers in a file, the files under a path).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "formats/sgd5.hpp"

namespace danzi::victory {

namespace fs = std::filesystem;


using DefaultPaths = std::vector<fs::path> (*)(std::vector<fs::path>);


constexpr std::uint64_t MAX_FILE = std::uint64_t{2} << 30;


struct Source {
    bool packed = false;

    std::size_t offset = 0;
    std::span<const std::uint8_t> data;

    sgd5::Record record;


    std::optional<std::span<const std::uint8_t>> bytes(std::vector<std::uint8_t>& storage) const;
    std::string label() const;
};


std::vector<Source> sources(std::span<const std::uint8_t> data);


struct WalkEntry {
    fs::path path;
    std::uint64_t len;
};
std::vector<WalkEntry> walk_files(const fs::path& root);


std::optional<std::vector<std::uint8_t>> read_whole(const fs::path& path);


std::size_t available_parallelism();


std::optional<std::uint32_t> parse_u32(std::string_view s);
std::optional<std::size_t> parse_usize(std::string_view s, std::string* error = nullptr);


void binary_output();


void say(const std::string& line);


std::array<std::uint8_t, 32> blake3_of(std::span<const std::uint8_t> data);
std::string hex_prefix(const std::array<std::uint8_t, 32>& hash, std::size_t n);


void shader_info(std::vector<fs::path> paths, DefaultPaths default_paths, bool verbose, bool disasm);


void translate_env();

void translate(std::vector<fs::path> paths, DefaultPaths default_paths, bool validate, bool pipelines, bool list, std::optional<std::size_t> jobs);

enum class StageFilter { Vs, Ps, Ms, Cs };


void precompile(std::vector<fs::path> paths, DefaultPaths default_paths, bool pipelines, std::optional<StageFilter> only, std::optional<std::size_t> limit,
                std::optional<std::size_t> jobs, std::optional<std::size_t> pipeline_jobs);

}   
