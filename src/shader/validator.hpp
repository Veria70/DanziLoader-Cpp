// SPDX-License-Identifier: GPL-3.0-or-later
// The DXIL validator in `dxil.dll` (`IDxcValidator`), for the offline batch
// (`crates/victory-shader/src/emit/validator.rs`; DanziLoader /
// victory-perf, see NOTICE.md). Translated shaders do not need it: they
// carry the container hash the translator computes.
//
// The DLL is found at `VICTORY_DXIL` if set, else the newest Windows SDK's
// `bin\<version>\x64\dxil.dll`, else wherever `LoadLibrary` finds it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace danzi::shader::emit::validator {


std::optional<std::filesystem::path> default_path();

class Validator {
public:


    static std::optional<Validator> load(const std::optional<std::filesystem::path>& path, std::string& error);


    std::optional<std::string> validate(std::vector<std::uint8_t>& container);

    Validator(Validator&& o) noexcept : object_(o.object_) { o.object_ = nullptr; }
    Validator& operator=(Validator&& o) noexcept;
    Validator(const Validator&) = delete;
    Validator& operator=(const Validator&) = delete;
    ~Validator();

private:
    explicit Validator(void* object) : object_(object) {}

    void* object_ = nullptr;
};

}   
