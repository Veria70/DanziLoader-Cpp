// SPDX-License-Identifier: GPL-3.0-or-later
// DXIL emission (`emit/mod.rs`, `emit/sampler.rs`, `emit/threading.rs`;
// DanziLoader / victory-perf, see NOTICE.md): a console vertex, pixel,
// compute or mesh shader as a PC DXIL container (SM 6.6), with what the
// host binds for it. `translate` takes container bytes as the title passes
// them at pipeline creation; the wave64 retries (a program in other forms,
// local waves, wave-wide branches) and the threading retry are the
// reference's, in its order.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "container.hpp"
#include "lower.hpp"
#include "native.hpp"
#include "resource.hpp"

namespace danzi::shader {
struct Container;
namespace ir {
struct Program;
}
namespace structure {
struct Structured;
}
namespace io {
struct Io;
}
}   

namespace danzi::shader::emit {

using lower::UserData;


struct StaticSampler {
    std::uint32_t filter = 0;
    std::array<std::uint32_t, 3> address{};
    float mip_lod_bias = 0.0f;
    std::uint32_t max_anisotropy = 0;
    std::uint32_t comparison = 0;
    std::uint32_t border = 0;
    float min_lod = 0.0f, max_lod = 0.0f;
    static StaticSampler from_words(const std::array<std::uint32_t, 4>& w);
};


struct HostShader {
    HwStage stage = HwStage::Vs;

    std::vector<std::uint8_t> dxil;
    std::vector<UserData> user_data;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::vector<StaticSampler> static_samplers;
    std::vector<std::uint8_t> root_cbvs, root_srvs, root_uavs;
    std::vector<resource::RootIndexedView> root_indexed_views;


    std::uint32_t user_data_dwords() const;
};


HostShader translate(std::span<const std::uint8_t> container);


HostShader emit(const Container& c, const NativeProgram& native, const ir::Program& p, const structure::Structured& st, const io::Io& io,
                const resource::Resources& res);


std::vector<container::SigElement> part_signature(std::span<const std::uint8_t> part);

HostShader translate_code(std::span<const std::uint8_t> container, std::optional<std::vector<std::uint8_t>> code);


std::vector<std::uint8_t> rect_list_gs(std::span<const std::uint8_t> vs, std::optional<std::span<const std::uint8_t>> ps);

}   
