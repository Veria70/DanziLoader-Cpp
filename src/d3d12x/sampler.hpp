// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/sampler.rs (DanziLoader / victory-perf, see
// NOTICE.md): samplers as the host describes them, from the console's sampler
// description (`CreateSamplerX`), which names the hardware's sampler fields.
#pragma once

#include <array>
#include <cstdint>

namespace danzi::d3d12x {


std::array<std::uint32_t, 13> console_sampler(const std::uint32_t* words);

}   
