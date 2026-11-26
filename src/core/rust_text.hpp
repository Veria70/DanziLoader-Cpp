// SPDX-License-Identifier: GPL-3.0-or-later
// Text as the reference's Rust standard library makes and reads it, where a
// ported message or parse depends on it (DanziLoader / victory-perf, see
// NOTICE.md): float `{}`, `str`'s `{:?}`, `trim`, `split_whitespace`,
// `parse::<f32>()` and `parse::<u8>()`. Strings are UTF-8.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace danzi::rust_text {


std::string display(float v);
std::string display(double v);


std::string debug_str(std::string_view s);


bool is_whitespace(char32_t c);

std::string_view trim(std::string_view s);
std::vector<std::string_view> split_whitespace(std::string_view s);


std::optional<float> parse_f32(std::string_view s);

std::optional<std::uint8_t> parse_u8(std::string_view s);


bool valid_utf8(std::string_view s);


std::optional<std::string> utf8_strict(std::wstring_view w);

}   
