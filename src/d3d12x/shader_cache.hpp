// SPDX-License-Identifier: GPL-3.0-or-later
// Translated shaders kept on disk (`shader_cache.rs`; DanziLoader /
// victory-perf, see NOTICE.md): entries of `victory-pipeline`'s
// content-addressed cache (`<dir>/<2 hex>/<62 hex>`, keyed by the BLAKE3 of
// the converter's id and version, the translator's build fingerprint and the
// container), in the reference's encoding of `HostShader`. Until milestone 5
// ports the translator, the port reads the entries the reference's
// translator wrote, under the fingerprint it is given
// (`backend_use_shader_build`); a container without an entry does not
// translate.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace danzi::d3d12x {

void use_shader_cache(const wchar_t* dir);
void use_shader_build(const std::uint8_t* build);


enum class RootKind : std::uint8_t { Constants, Descriptor, Table, ImmediateSampler, ResourceHeap, SamplerHeap, Other };

struct UserData {


    std::uint32_t index;
    std::uint8_t slot;
    RootKind root;
    std::uint32_t other;
    std::uint32_t dword;
};

struct StaticSampler {
    std::uint32_t filter;
    std::array<std::uint32_t, 3> address;
    float mip_lod_bias;
    std::uint32_t max_anisotropy, comparison, border;
    float min_lod, max_lod;
};

struct RootIndexedView {
    std::uint8_t table_slot, constant_slot;
    std::uint32_t constant_dword, byte_offset;
    std::uint8_t shift;
};


struct HostShader {
    std::uint32_t stage = 0;   
    std::vector<std::uint8_t> dxil;
    std::vector<UserData> user_data;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::vector<StaticSampler> static_samplers;
    std::vector<std::uint8_t> root_cbvs, root_srvs, root_uavs;
    std::vector<RootIndexedView> root_indexed_views;


    std::uint32_t user_data_dwords() const;
};


struct Translation {
    std::optional<HostShader> shader;
    std::string error;
    bool cached = false;
};
Translation host_shader(std::span<const std::uint8_t> container);


std::array<std::uint64_t, 2> shader_cache_counts();


constexpr std::uint32_t USER_DATA_SPACE = 0;
constexpr std::uint32_t SAMPLER_SPACE = 0;
constexpr std::uint32_t ROOT_CBV_SPACE = 1;
constexpr std::uint8_t SLOT_STRIDES = 255;
constexpr std::uint8_t SLOT_MEMORY = 254;
constexpr std::uint8_t SLOT_FAULTS = 253;

}   
