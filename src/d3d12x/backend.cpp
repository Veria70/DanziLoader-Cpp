// SPDX-License-Identifier: GPL-3.0-or-later
// The port's backend DLL for the milestone 4 title driver: the same exports
// as `oracle-m4` (the reference's victory-gpu): `d3d12_xs.dll`'s, with a
// `backend_` prefix, and the runtime's hand-over.
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#include "device.hpp"
#include "resource.hpp"
#include "root.hpp"
#include "shader_cache.hpp"
#include "pipeline.hpp"
#include "host.hpp"
#include "texture.hpp"
#include "host_heap.hpp"

using namespace danzi::d3d12x;

#define EXPORT extern "C" __declspec(dllexport)

namespace {


const bool MADE = (device(), dxgi_device(), true);

}   

EXPORT void __stdcall backend_use_runtime(void* alloc, void* free, void* mapper) {


    use_runtime(reinterpret_cast<AllocRoutine>(alloc), reinterpret_cast<FreeRoutine>(free), reinterpret_cast<PageMapper>(mapper));
}

EXPORT danzi::d3d12x::HRESULT __stdcall backend_D3D12ConfigureVirtualMemoryX(VirtualMemory* config) { return configure_virtual_memory(config); }

EXPORT danzi::d3d12x::HRESULT __stdcall backend_D3D12XboxCreateDevice(const void* adapter, const std::uint32_t* parameters, const Iid* iid, This* out) {
    return create_device(adapter, parameters, iid, out);
}

EXPORT danzi::d3d12x::HRESULT __stdcall backend_D3D12SerializeRootSignature(const RootDesc* desc, std::uint32_t version, void** blob, void** error) {
    return serialize_root(desc, version, blob, error);
}

EXPORT danzi::d3d12x::HRESULT __stdcall backend_D3D12SerializeVersionedRootSignature(const VersionedRootDesc* desc, void** blob, void** error) {
    return serialize_root_versioned(desc, blob, error);
}

EXPORT danzi::d3d12x::HRESULT __stdcall backend_D3D12CreateRootSignatureDeserializer(const std::uint8_t* blob, std::size_t size, const Iid* iid, void** out) {
    return create_root_deserializer(blob, size, iid, out);
}

EXPORT const Iid* __stdcall backend_device_iid() { return &DEVICE_IID; }

EXPORT void __stdcall backend_set_console(std::uint32_t series_s) { set_console(series_s ? Console::SeriesS : Console::SeriesX); }


EXPORT void __stdcall backend_title_memory_remapped(std::size_t at, std::size_t len) { danzi::d3d12x::remapped(at, len); }


EXPORT void __stdcall backend_use_shader_cache(const wchar_t* dir) { danzi::d3d12x::use_shader_cache(dir); }
EXPORT void __stdcall backend_use_shader_build(const std::uint8_t* build) { danzi::d3d12x::use_shader_build(build); }


EXPORT void __stdcall backend_use_pipeline_cache(const wchar_t* dir) { danzi::d3d12x::use_pipeline_cache(dir); }
EXPORT void __stdcall backend_save_pipeline_cache() { danzi::d3d12x::save_pipeline_cache(); }


EXPORT void __stdcall backend_pipeline_counts(std::uint64_t* out) {
    auto c = danzi::d3d12x::pipelines_counts();
    for (int i = 0; i < 3; ++i) {
        out[i] = c[i];
    }
}


EXPORT int __stdcall backend_precompile_init(std::uint32_t* out) {
    if (!danzi::d3d12x::host()) {
        return -1;
    }
    danzi::d3d12x::probe_view_limit();
    out[0] = danzi::d3d12x::view_heap_limit();
    out[1] = danzi::d3d12x::host_mesh_shaders() ? 1 : 0;
    return 0;
}

namespace {
std::mutex PREPARED_LOCK;
std::vector<danzi::d3d12x::HostShader> PREPARED;
const char* stage_name(std::uint32_t stage) {
    static const char* const names[] = {"domain", "hull", "export", "geometry", "vertex", "pixel", "mesh", "compute"};
    return stage < 8 ? names[stage] : "";
}
}   


EXPORT int __stdcall backend_precompile_shader(const std::uint8_t* container, std::size_t len, std::uint32_t* out) {
    danzi::d3d12x::probe_view_limit();
    auto t = danzi::d3d12x::host_shader(std::span(container, len));
    if (!t.shader) {
        return -1;
    }
    char name[16] = {};
    std::strncpy(name, stage_name(t.shader->stage), 16);
    std::memcpy(out, name, 16);
    out[4] = t.shader->stage == 7 ? 1 : 0;
    out[5] = t.cached ? 1 : 0;
    std::lock_guard lock(PREPARED_LOCK);
    out[6] = static_cast<std::uint32_t>(PREPARED.size());
    PREPARED.push_back(std::move(*t.shader));
    return 0;
}


EXPORT int __stdcall backend_precompile_compute(std::uint32_t id, const char* label, std::size_t label_len) {
    std::lock_guard lock(PREPARED_LOCK);
    if (id >= PREPARED.size() || PREPARED[id].stage != 7) {
        return -1;
    }
    return danzi::d3d12x::precompile_compute(PREPARED[id], label ? std::string_view(label, label_len) : std::string_view{});
}


EXPORT void __stdcall backend_heap_counts(std::uint64_t* out) {
    auto c = danzi::d3d12x::heap_pass_counts();
    for (int i = 0; i < 6; ++i) {
        out[i] = c[i];
    }
}


EXPORT void __stdcall backend_sweep_counts(std::uint64_t* out) {
    auto w = danzi::d3d12x::windows_let_go();
    auto t = danzi::d3d12x::textures_let_go();
    auto s = danzi::d3d12x::staging_let_go();
    out[0] = w[0];
    out[1] = s[0];
    out[2] = t[0];
}

EXPORT std::uint64_t __stdcall backend_library_trimmed() { return danzi::d3d12x::library_trimmed(); }
