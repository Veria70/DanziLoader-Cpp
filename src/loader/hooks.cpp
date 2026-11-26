// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in compatibility hooks for known title builds
// (`victory-loader/src/hooks.rs`; DanziLoader / victory-perf, see
// NOTICE.md; the reference ported them from ZigZagBoi1/ScarlettPort,
// GPL-3.0-or-later). Only per-build offsets and short byte signatures live
// here (nothing of the title itself). Every hook checks the build's hash and
// the exact bytes it replaces, and does nothing otherwise.
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/env.hpp"
#include "core/log.hpp"
#include "loader.hpp"

namespace danzi::loader::hooks {

namespace {


constexpr const char* BUILD_1_0_2_1 = "b99f876b74d4a77f";

bool is_build(const std::array<std::uint8_t, 32>& hash) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string s;
    for (int k = 0; k < 8; ++k) {
        s.push_back(HEX[hash[k] >> 4]);
        s.push_back(HEX[hash[k] & 15]);
    }
    return s == BUILD_1_0_2_1;
}


struct Patch {
    const char* what;
    std::size_t rva;
    std::vector<std::uint8_t> expect, now;
};

bool apply(std::uintptr_t base, const Patch& p) {
    auto* at = reinterpret_cast<std::uint8_t*>(base + p.rva);
    if (p.now.size() != p.expect.size() || std::memcmp(at, p.expect.data(), p.expect.size()) != 0) {
        log::warn("victory_loader::hooks", "hook skipped: the bytes are not the expected ones", {{"what", log::debug(p.what)}});
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(at, p.now.size(), PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(at, p.now.data(), p.now.size());
    VirtualProtect(at, p.now.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, p.now.size());
    log::info("victory_loader::hooks", "hook applied", {{"what", log::debug(p.what)}});
    return true;
}


const std::vector<Patch>& watchdog() {
    static const std::vector<Patch> p = {
        {"watchdog: skip the call and the fallback fault", 0x2215c27, {0x48, 0x8B, 0x05, 0x1A, 0x07, 0xFA, 0x04}, {0xEB, 0x18, 0x90, 0x90, 0x90, 0x90, 0x90}},
        {"watchdog: handler's deliberate fault",
         0x23cf683,
         {0xC7, 0x04, 0x25, 0, 0, 0, 0, 0x21, 0xD4, 0x8B, 0x4D},
         {0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0, 0, 0, 0, 0}},
    };
    return p;
}


const Patch OWN_UPSCALER{"upscaler: always the title's own", 0x7e9426, {0x0F, 0x45, 0xC1}, {0x0F, 0x1F, 0x00}};


constexpr std::pair<float, std::size_t> FSR_SCALES[4] = {{0.5f, 0x5fc6edc}, {0.4f, 0x5fc6f90}, {1.0f / 3.0f, 0x615a240}, {0.75f, 0x6159b04}};

std::vector<std::uint8_t> le(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    return {b, b + n};
}


std::vector<Patch> render_scale_patches(std::uintptr_t base, float scale, bool fsr) {
    std::vector<Patch> patches = {
        {"render scale: own upscaler minimum", 0x28f1e7e, {0xAB, 0xAA, 0x2A, 0x3F}, le(&scale, 4)},
        {"render scale: own upscaler maximum = minimum", 0x28f1e87, {0xC5, 0xFA, 0x5F, 0x86, 0x90, 0, 0, 0}, {0xC5, 0xFA, 0x10, 0x86, 0x90, 0, 0, 0}},
    };
    if (!fsr) {
        return patches;
    }
    for (const auto& [v, constant] : FSR_SCALES) {
        if (std::fabs(v - scale) < 1e-3f) {
            constexpr std::size_t LOAD = 0x28ef6b2;
            auto disp = static_cast<std::int32_t>(static_cast<std::int64_t>(constant) - static_cast<std::int64_t>(LOAD + 8));
            std::vector<std::uint8_t> now{0xC5, 0xFA, 0x10, 0x05};
            auto d = le(&disp, 4);
            now.insert(now.end(), d.begin(), d.end());
            patches.push_back({"render scale: FSR scale", LOAD, {0xC5, 0xFA, 0x10, 0x05, 0xA2, 0xE2, 0x86, 0x03}, now});
            std::uint64_t was = base + 0x28ef6f0, to = base + 0x28ef680;
            patches.push_back({"render scale: FSR maximum = scaled size", 0x6300ba8, le(&was, 8), le(&to, 8)});
            return patches;
        }
    }
    log::warn("victory_loader::hooks", "no FSR constant for this scale (0.5, 0.4, 1/3, 0.75); FSR keeps its own (use VICTORY_OWN_UPSCALER=1)",
              {{"scale", log::debug(scale)}});
    return patches;
}

}   

std::optional<float> parse_scale(std::string_view text) {

    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    float v = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (text.empty() || ec != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    if (!(v >= 0.2f && v <= 1.0f)) {
        return std::nullopt;
    }
    return v;
}

std::size_t disarm_watchdog(std::uintptr_t base, const std::array<std::uint8_t, 32>& hash) {
    if (!is_build(hash)) {
        return 0;
    }
    std::size_t n = 0;
    for (const auto& p : watchdog()) {
        n += apply(base, p) ? 1 : 0;
    }
    return n;
}

std::size_t resolution_levers(std::uintptr_t base, const std::array<std::uint8_t, 32>& hash) {
    auto scale = env::get(L"VICTORY_RENDER_SCALE");
    auto own_text = env::get(L"VICTORY_OWN_UPSCALER");
    bool own = own_text && *own_text != L"0";
    if (!scale && !own) {
        return 0;
    }


    if (!is_build(hash)) {
        return 0;
    }
    std::size_t applied = 0;
    if (own) {
        applied += apply(base, OWN_UPSCALER) ? 1 : 0;
    }
    if (scale) {
        std::string narrow;
        for (wchar_t c : *scale) {
            narrow.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        if (auto s = parse_scale(narrow); !s) {
            log::warn("victory_loader::hooks", "VICTORY_RENDER_SCALE wants a number in 0.2..=1.0", {{"text", log::debug(narrow)}});
        } else {

            for (const auto& p : render_scale_patches(base, *s, !own)) {
                applied += apply(base, p) ? 1 : 0;
            }
        }
    }
    return applied;
}

}   
