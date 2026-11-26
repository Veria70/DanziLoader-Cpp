// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/sampler.rs (see sampler.hpp).
#include "sampler.hpp"

#include <algorithm>

namespace danzi::d3d12x {

std::array<std::uint32_t, 13> console_sampler(const std::uint32_t* w) {

    const std::uint32_t mag = w[1], min = w[2];

    const std::uint32_t mip = w[3];

    const std::uint32_t mode = w[5];
    const std::uint32_t anisotropy = w[13], compare = w[16];
    auto linear = [](std::uint32_t f) { return f == 1 || f == 3 ? 1u : 0u; };
    bool aniso = mag >= 2 || min >= 2;


    constexpr std::uint32_t LESS = 2, GREATER_EQUAL = 7;
    bool compares = compare >= LESS && compare <= GREATER_EQUAL;
    std::uint32_t reduction = mode == 1 ? 2 : mode == 2 ? 3 : (compares ? 1 : 0);
    std::uint32_t filter = aniso ? 0x55 : linear(min) << 4 | linear(mag) << 2 | (mip == 2 ? 1u : 0u);
    return {
        reduction << 7 | filter,
        w[6],
        w[7],
        w[8],
        w[11],   
        aniso ? std::clamp(anisotropy, 1u, 16u) : 1u,
        std::max(compare, 1u),
        w[17],
        w[18],
        w[19],
        w[20],   
        w[9],    

        mip == 0 ? w[9] : w[10],
    };
}

}   
