// SPDX-License-Identifier: GPL-3.0-or-later
// `d3d12_xs.dll`: virtual memory setup, device creation, root signature
// serialization (`victory-hle/src/modules/d3d12x.rs`; DanziLoader /
// victory-perf, see NOTICE.md). The device and everything it creates live
// in the port's `d3d12x` (the reference's `victory-gpu`).
#include <string>

#include "d3d12x/device.hpp"
#include "d3d12x/host.hpp"
#include "d3d12x/pipeline.hpp"
#include "d3d12x/resource.hpp"
#include "d3d12x/root.hpp"
#include "d3d12x/shader_cache.hpp"
#include "loader/crash.hpp"
#include "modules.hpp"

namespace danzi::hle::d3d12x_module {

namespace {

namespace gx = danzi::d3d12x;


int __stdcall map_alias(std::uint64_t to, std::uint64_t from, std::uint64_t len) { return xmem::map_alias(to, from, len) ? 1 : 0; }


void host_gpu_failed(const char* what) {
    loader::crash::log_crash(std::string("VIctory: host GPU failure: ") + what);
    constexpr UINT DXGI_ERROR_DEVICE_REMOVED_ = 0x887A0005;
    TerminateProcess(GetCurrentProcess(), DXGI_ERROR_DEVICE_REMOVED_);
}


gx::HRESULT __stdcall create_device(const void* adapter, const std::uint32_t* parameters, const gx::Iid* iid, gx::This* out) {
    gx::use_runtime(&xmem::XMemAlloc, &xmem::XMemFree, &map_alias);
    gx::on_failure(&host_gpu_failed);
    return gx::create_device(adapter, parameters, iid, out);
}

}   

void use_shader_cache(const std::filesystem::path& dir) {
    gx::use_pipeline_cache((dir.parent_path() / L"pipelines").c_str());
    gx::use_shader_cache(dir.c_str());
}

std::vector<HleExport> exports() {
    return {
        {"D3D12ConfigureVirtualMemoryX", reinterpret_cast<std::uintptr_t>(&gx::configure_virtual_memory)},
        {"D3D12XboxCreateDevice", reinterpret_cast<std::uintptr_t>(&create_device)},
        {"D3D12SerializeRootSignature", reinterpret_cast<std::uintptr_t>(&gx::serialize_root)},
        {"D3D12SerializeVersionedRootSignature", reinterpret_cast<std::uintptr_t>(&gx::serialize_root_versioned)},

        {"D3D12CreateRootSignatureDeserializer", reinterpret_cast<std::uintptr_t>(&gx::create_root_deserializer)},

        {"IID_ID3D12Device", reinterpret_cast<std::uintptr_t>(&gx::DEVICE_IID)},
    };
}

}   
