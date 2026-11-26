// SPDX-License-Identifier: GPL-3.0-or-later
// The translator's errors (`lib.rs` `ShaderError`; DanziLoader /
// victory-perf, see NOTICE.md), thrown where the reference returns `Err`:
// their texts are the reference's, since the GPU side and the oracle
// comparisons read them.
#pragma once

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace danzi::shader {

struct ShaderError : std::exception {
    enum Kind : std::uint8_t { NotDxbc, Malformed, Isa, Unsupported } kind;

    const char* text = "";
    std::size_t offset = 0;

    ShaderError(Kind k, const char* t, std::size_t at = 0) : kind(k), text(t), offset(at) {}
    const char* what() const noexcept override { return text; }


    std::string display() const {
        char at[32];
        std::snprintf(at, sizeof at, "%#zx", offset);
        if (offset == 0) {
            std::snprintf(at, sizeof at, "0x0");
        }
        switch (kind) {
            case NotDxbc: return "not a DXBC container";
            case Malformed: return std::string("malformed shader: ") + text;
            case Isa: return std::string("GFX10 code at ") + at + ": " + text;
            case Unsupported: return std::string("GFX10 code at ") + at + ": " + text + " is not supported";
        }
        return {};
    }
    bool operator==(const ShaderError& o) const { return kind == o.kind && std::string_view(text) == o.text && offset == o.offset; }
};

[[noreturn]] inline void malformed(const char* why) { throw ShaderError(ShaderError::Malformed, why); }
[[noreturn]] inline void isa(std::size_t offset, const char* why) { throw ShaderError(ShaderError::Isa, why, offset); }
[[noreturn]] inline void unsupported(std::size_t offset, const char* what) { throw ShaderError(ShaderError::Unsupported, what, offset); }

}   
