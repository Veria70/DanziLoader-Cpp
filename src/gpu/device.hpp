// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "core/win.hpp"

namespace danzi::gpu {

using Microsoft::WRL::ComPtr;

struct AdapterInfo {
    std::wstring name;
    UINT vendor = 0;   
    UINT device_id = 0;
    std::uint64_t dedicated_vram = 0;
    bool software = false;
    bool nvidia() const { return vendor == 0x10DE; }
};

struct Features {
    D3D_SHADER_MODEL shader_model = D3D_SHADER_MODEL_5_1;
    D3D12_RESOURCE_HEAP_TIER heap_tier = D3D12_RESOURCE_HEAP_TIER_1;
    UINT wave_lanes_min = 0, wave_lanes_max = 0;   
    bool gpu_upload_heap = false;                  
    bool uma = false;
    bool cache_coherent_uma = false;
};

struct VideoMemory {
    std::uint64_t budget = 0, usage = 0;   
    std::uint64_t shared_usage = 0;        
};


struct Prelude {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};

class Device {
public:
    enum class Kind { Hardware, Warp };


    static std::unique_ptr<Device> create(Kind kind = Kind::Hardware, bool debug = false);
    ~Device();

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    ID3D12CommandQueue* copy_queue() const { return copy_queue_.Get(); }
    const AdapterInfo& adapter() const { return adapter_; }
    const Features& features() const { return features_; }

    std::optional<VideoMemory> video_memory() const;

    std::optional<Prelude> make_prelude(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) const;


    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> shared_buffer(std::uint64_t size) const;

    ComPtr<ID3D12Resource> uav_buffer(std::uint64_t size) const;

    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> upload_buffer(std::uint64_t size) const;
    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> readback_buffer(std::uint64_t size) const;


    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> gpu_upload_buffer(std::uint64_t size) const;


    void execute_and_wait(std::initializer_list<ID3D12CommandList*> lists, ID3D12CommandQueue* queue = nullptr);

    std::uint64_t signal(ID3D12CommandQueue* queue = nullptr);
    void wait(std::uint64_t value);

private:
    Device() = default;
    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> mapped(const D3D12_HEAP_PROPERTIES& heap, std::uint64_t size,
                                                                          D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) const;

    ComPtr<IDXGIAdapter3> dxgi_adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_, copy_queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    std::uint64_t fence_value_ = 0;
    AdapterInfo adapter_;
    Features features_;
};


D3D12_RESOURCE_DESC buffer_desc(std::uint64_t size, D3D12_RESOURCE_FLAGS flags);

}   
