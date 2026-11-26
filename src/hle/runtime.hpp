// SPDX-License-Identifier: GPL-3.0-or-later
// Process-wide state the runtime's implementations need
// (`victory-hle/src/runtime.rs`; DanziLoader / victory-perf, see NOTICE.md).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace danzi::hle::runtime {


void set_main_image(std::uintptr_t base);
std::optional<std::uintptr_t> main_image();


void set_image_path(std::uintptr_t base, const std::filesystem::path& path);

void forget_image(std::uintptr_t base);
std::optional<std::filesystem::path> image_path(std::uintptr_t base);

std::optional<std::uintptr_t> image_named(std::string_view file);


using TitleDllLoader = std::function<std::pair<std::optional<std::uintptr_t>, std::optional<std::string>>(const std::string&)>;

void set_title_dll_loader(TitleDllLoader loader);
std::pair<std::optional<std::uintptr_t>, std::optional<std::string>> load_title_dll(const std::string& file);


struct Title {
    std::uint32_t id = 0;

    std::string store_id;

    std::filesystem::path storage;
    std::uint64_t storage_bytes = 0;


    static Title from_manifest(std::string_view text);

    std::filesystem::path saves() const;
    bool operator==(const Title&) const = default;
};


std::vector<std::pair<std::string, std::string>> elements(std::string_view text);

void set_title(Title title);

Title title();


void use_console_timers();

}   
