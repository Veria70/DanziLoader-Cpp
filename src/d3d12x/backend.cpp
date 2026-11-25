// SPDX-License-Identifier: GPL-3.0-or-later


#include <cstdint>

#include "device.hpp"

using namespace danzi::d3d12x;

#define EXPORT extern "C" __declspec(dllexport)

namespace {


const bool MADE = (device(), dxgi_device(), true);

void* (__stdcall* ALLOC)(std::size_t, std::uint64_t) = nullptr;
void(__stdcall* FREE)(void*, std::uint64_t) = nullptr;
int(__stdcall* MAPPER)(std::uint64_t, std::uint64_t, std::uint64_t) = nullptr;
}   

EXPORT void __stdcall backend_use_runtime(void* alloc, void* free, void* mapper) {
    ALLOC = reinterpret_cast<decltype(ALLOC)>(alloc);
    FREE = reinterpret_cast<decltype(FREE)>(free);
    MAPPER = reinterpret_cast<decltype(MAPPER)>(mapper);
}

EXPORT HRESULT __stdcall backend_D3D12ConfigureVirtualMemoryX(VirtualMemory* config) { return configure_virtual_memory(config); }

EXPORT HRESULT __stdcall backend_D3D12XboxCreateDevice(const void* adapter, const std::uint32_t* parameters, const Iid* iid, This* out) {
    return create_device(adapter, parameters, iid, out);
}

EXPORT const Iid* __stdcall backend_device_iid() { return &DEVICE_IID; }

EXPORT void __stdcall backend_set_console(std::uint32_t series_s) { set_console(series_s ? Console::SeriesS : Console::SeriesX); }


EXPORT void __stdcall backend_title_memory_remapped(std::size_t, std::size_t) {}
