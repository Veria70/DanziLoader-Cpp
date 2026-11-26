// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/pipeline.rs and pipeline/{host,make,
// library}.rs (DanziLoader / victory-perf, see NOTICE.md): the title's
// pipeline objects, the shader containers they hold (shared by content),
// their translations (from the shader cache, else translated), and host
// pipelines made by workers off the title's threads,
// waited for within a budget per frame, kept in the host's pipeline library.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "object.hpp"
#include "shader_cache.hpp"
#include "shader/native.hpp"

namespace danzi::d3d12x {

using Microsoft::WRL::ComPtr;


struct Translated {
    HostShader host;
    std::vector<shader::UsedResource> used;


    bool stores = true;
    bool writes = true;
};


struct Shader {
    const char* stage;

    std::string hash;
    std::vector<std::uint8_t> container;

    const Translated* translated();

private:
    std::once_flag once_;
    std::optional<Translated> translated_;
};


struct Stage {
    std::shared_ptr<Shader> shader;

    std::optional<std::pair<std::uint32_t, std::uint32_t>> user_data;

    std::vector<std::pair<std::uint8_t, std::uint32_t>> root_cbvs, root_srvs, root_uavs;
    const Translated& translated() const { return *shader->translated(); }
};


struct Input {
    std::uint32_t slot, offset, format;
    bool per_instance;
};

struct HostPipeline {
    bool compute = false;
    bool mesh = false;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12RootSignature> root;

    std::vector<Stage> stages;

    std::uint32_t slots = 0;
    std::vector<Input> inputs;

    bool rects = false;
    std::string name;


    bool bake = false;
};


bool is_bake_pipeline(const HostPipeline& p);


std::array<std::uint64_t, 6> heap_pass_counts();


std::array<std::uint64_t, 9> heap_texture_queue();


bool bounded_root_srvs();


const HostPipeline* host_pipeline(This pipeline);

void pipeline_bound(This pipeline);


bool host_depth_bounds();
bool host_mesh_shaders();


bool is_builder(This pipeline);


int precompile_compute(const HostShader& shader, std::string_view label = {});


std::string library_removal_note();


void trim_library();
std::uint64_t library_trimmed();
bool builder_quarantined(This pipeline);
void builder_off(const char* why);
bool builder_runs();

std::uint64_t pipelines_making();


std::array<std::uint64_t, 3> pipelines_counts();


void use_pipeline_cache(const wchar_t* dir);
void save_pipeline_cache();


HRESULT __stdcall create_graphics_pipeline(This, const void* desc, const Iid* iid, This* out);
HRESULT __stdcall create_compute_pipeline(This, const void* desc, const Iid* iid, This* out);
HRESULT __stdcall create_pipeline_stream(This, const void* desc, const Iid* iid, This* out);
HRESULT __stdcall create_compute_pipeline_x(This, const void* desc, std::uint32_t extensions, const std::uint8_t* extended, const Iid* iid, This* out);

}   
