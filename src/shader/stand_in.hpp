// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-ins for passes whose results the host cannot produce yet
// (`emit/stand_in.rs`; DanziLoader / victory-perf, see NOTICE.md): the
// title's own program translated with every image store writing one
// constant in each output's own encoding. Off unless a plan is installed
// for the translation on this thread.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace danzi::shader::stand_in {

struct Kind {
    enum Tag : std::uint8_t { Float, Rgb9e5, Mask, Zero, Bits } tag = Float;
    std::uint32_t bits = 0;
    static std::optional<Kind> parse(std::string_view s);
};

struct Plan {
    float value = 0.0f;
    Kind fallback;
    std::vector<std::pair<std::uint32_t, Kind>> slots;
    std::vector<std::optional<std::uint32_t>> seen;

    explicit Plan(float v) : value(v) {}
    Plan with_overrides(std::string_view spec) &&;
    std::array<std::uint32_t, 4> bits(std::optional<std::uint32_t> slot);
};

std::uint32_t rgb9e5(std::array<float, 3> rgb);


std::optional<Plan>& store_plan();

std::optional<std::array<std::uint32_t, 4>> store_plan_bits(std::optional<std::uint32_t> slot);


std::vector<std::uint8_t> exposure_code(float value);

}   
