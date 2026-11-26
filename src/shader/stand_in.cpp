// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-ins (`emit/stand_in.rs`; DanziLoader / victory-perf, see NOTICE.md).
#include "stand_in.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace danzi::shader::stand_in {

namespace {
thread_local std::optional<Plan> t_plan;

std::string trim_lower(std::string_view s) {
    std::string out;
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front()))) {
        out.erase(out.begin());
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    return out;
}
}   

std::optional<Kind> Kind::parse(std::string_view sv) {
    std::string s = trim_lower(sv);
    if (s == "f" || s == "float") {
        return Kind{Float};
    }
    if (s == "rgb9e5" || s == "e5") {
        return Kind{Rgb9e5};
    }
    if (s == "mask" || s == "ones") {
        return Kind{Mask};
    }
    if (s == "zero" || s == "0") {
        return Kind{Zero};
    }
    try {
        std::size_t used = 0;
        unsigned long v = s.starts_with("0x") ? std::stoul(s.substr(2), &used, 16) : std::stoul(s, &used, 10);
        if (used != (s.starts_with("0x") ? s.size() - 2 : s.size()) || v > 0xFFFFFFFFul) {
            return std::nullopt;
        }
        return Kind{Bits, static_cast<std::uint32_t>(v)};
    } catch (...) {
        return std::nullopt;
    }
}

Plan Plan::with_overrides(std::string_view spec) && {
    std::size_t at = 0;
    while (at <= spec.size()) {
        std::size_t end = at;
        while (end < spec.size() && spec[end] != ',' && !std::isspace(static_cast<unsigned char>(spec[end]))) {
            ++end;
        }
        std::string_view part = spec.substr(at, end - at);
        at = end + 1;
        if (part.empty()) {
            continue;
        }
        auto eq = part.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        auto kind = Kind::parse(part.substr(eq + 1));
        if (!kind) {
            continue;
        }
        std::string k = trim_lower(part.substr(0, eq));
        if (k == "*" || k == "default") {
            fallback = *kind;
        } else {
            try {
                std::size_t used = 0;
                unsigned long slot = std::stoul(k, &used, 10);
                if (used == k.size() && slot <= 0xFFFFFFFFul) {
                    auto sl = static_cast<std::uint32_t>(slot);
                    std::erase_if(slots, [&](const auto& x) { return x.first == sl; });
                    slots.emplace_back(sl, *kind);
                }
            } catch (...) {
            }
        }
    }
    return std::move(*this);
}

std::array<std::uint32_t, 4> Plan::bits(std::optional<std::uint32_t> slot) {
    seen.push_back(slot);
    Kind kind = fallback;
    if (slot) {
        for (const auto& [sl, kd] : slots) {
            if (sl == *slot) {
                kind = kd;
                break;
            }
        }
    }
    std::uint32_t b;
    switch (kind.tag) {
        case Kind::Float: b = std::bit_cast<std::uint32_t>(value); break;
        case Kind::Rgb9e5: b = rgb9e5({value, value, value}); break;
        case Kind::Mask: b = UINT32_MAX; break;
        case Kind::Zero: b = 0; break;
        default: b = kind.bits; break;
    }
    return {b, b, b, b};
}

std::uint32_t rgb9e5(std::array<float, 3> rgb) {
    constexpr float MAX = 65408.0f;
    float c[3];
    for (int i = 0; i < 3; ++i) {
        float x = rgb[i];
        c[i] = (std::isnan(x) || x < 0.0f) ? 0.0f : std::min(x, MAX);
    }
    float top = std::max(std::max(c[0], c[1]), c[2]);
    std::int32_t exp = static_cast<std::int32_t>(std::max(std::floor(std::log2(top)), -16.0f)) + 1 + 15;
    auto scale = [](std::int32_t e) { return std::pow(2.0f, static_cast<float>(e - 15 - 9)); };
    if (static_cast<std::uint32_t>(std::floor(top / scale(exp) + 0.5f)) == 512) {
        ++exp;
    }
    float sc = scale(exp);
    std::uint32_t m[3];
    for (int i = 0; i < 3; ++i) {
        m[i] = std::min<std::uint32_t>(static_cast<std::uint32_t>(std::floor(c[i] / sc + 0.5f)), 511);
    }
    return static_cast<std::uint32_t>(exp) << 27 | m[2] << 18 | m[1] << 9 | m[0];
}

std::optional<Plan>& store_plan() { return t_plan; }

std::optional<std::array<std::uint32_t, 4>> store_plan_bits(std::optional<std::uint32_t> slot) {
    if (!t_plan) {
        return std::nullopt;
    }
    return t_plan->bits(slot);
}

std::vector<std::uint8_t> exposure_code(float value) {
    std::uint32_t k = std::bit_cast<std::uint32_t>(value);
    constexpr std::uint32_t lit = 0xFF;
    auto v_mov = [](std::uint32_t dst, std::uint32_t src) { return 0x7E000000u | (dst << 17) | (1u << 9) | src; };
    std::vector<std::uint32_t> c{0xBE840305u, 0xBE850380u, 0x7D840080u, 0xBE80246Au};
    std::size_t branch = c.size();
    c.push_back(0);
    c.insert(c.end(), {0xF40C0202u, 0xFA000080u});
    c.insert(c.end(), {0xF4080002u, 0xFA000040u});
    c.push_back(0xBF8CC07Fu);
    for (std::uint32_t v : {4u, 5u, 6u, 7u, 0u, 1u, 2u, 3u}) {
        c.insert(c.end(), {v_mov(v, lit), k});
    }
    c.push_back(v_mov(10, 6));
    c.push_back(v_mov(11, 7));
    c.insert(c.end(), {0xF0201F08u, 0x0002040Au});
    c.insert(c.end(), {0xE01C0000u, 0x80000000u});
    std::size_t end = c.size();
    c[branch] = 0xBF880000u | static_cast<std::uint32_t>(end - branch - 1);
    c.push_back(0xBF810000u);
    std::vector<std::uint8_t> out(c.size() * 4);
    std::memcpy(out.data(), c.data(), out.size());
    return out;
}

}   
