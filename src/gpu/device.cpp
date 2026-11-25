// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.hpp"

#include "core/env.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace danzi::gpu {

namespace {

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char b[160];
        std::snprintf(b, sizeof b, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(b);
    }
}

}   

D3D12_RESOURCE_DESC buffer_desc(std::uint64_t size, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<std::uint64_t>(size, 1);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc = {1, 0};
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

std::unique_ptr<Device> Device::create(Kind kind, bool debug) {
    if (debug) {
        ComPtr<ID3D12Debug> d;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) {
            d->EnableDebugLayer();
        }
    }
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    std::unique_ptr<Device> d(new Device());
    ComPtr<IDXGIAdapter1> adapter;
    if (kind == Kind::Warp) {
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d->device_)), "D3D12CreateDevice (WARP)");
    } else {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            a->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d->device_)))) {
                adapter = a;
                break;
            }
        }
        if (!adapter) {
            throw std::runtime_error("no hardware adapter with D3D12");
        }
    }
    adapter.As(&d->dxgi_adapter_);
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    d->adapter_.name = desc.Description;
    d->adapter_.vendor = desc.VendorId;
    d->adapter_.device_id = desc.DeviceId;
    d->adapter_.dedicated_vram = desc.DedicatedVideoMemory;
    d->adapter_.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || kind == Kind::Warp;


    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_8};
    while (FAILED(d->device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) && sm.HighestShaderModel > D3D_SHADER_MODEL_5_1) {
        sm.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(sm.HighestShaderModel - 1);
    }
    d->features_.shader_model = sm.HighestShaderModel;
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    if (SUCCEEDED(d->device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o))) {
        d->features_.heap_tier = o.ResourceHeapTier;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    if (SUCCEEDED(d->device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1))) {
        d->features_.wave_lanes_min = o1.WaveLaneCountMin;
        d->features_.wave_lanes_max = o1.WaveLaneCountMax;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
    if (SUCCEEDED(d->device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof o16))) {
        d->features_.gpu_upload_heap = o16.GPUUploadHeapSupported != FALSE;
    }
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    if (SUCCEEDED(d->device_->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch))) {
        d->features_.uma = arch.UMA != FALSE;
        d->features_.cache_coherent_uma = arch.CacheCoherentUMA != FALSE;
    }

    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(d->device_->CreateCommandQueue(&q, IID_PPV_ARGS(&d->queue_)), "CreateCommandQueue (direct)");
    q.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    check(d->device_->CreateCommandQueue(&q, IID_PPV_ARGS(&d->copy_queue_)), "CreateCommandQueue (copy)");
    check(d->device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d->fence_)), "CreateFence");
    d->event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return d;
}

Device::~Device() {
    if (fence_ && queue_) {
        wait(signal());
    }
    if (event_) {
        CloseHandle(event_);
    }
}

std::optional<VideoMemory> Device::video_memory() const {
    if (!dxgi_adapter_) {
        return std::nullopt;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO own{}, shared{};
    if (FAILED(dxgi_adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &own)) ||
        FAILED(dxgi_adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared))) {
        return std::nullopt;
    }


    std::uint64_t budget = own.Budget;
    if (auto v = env::get(L"VICTORY_TEST_VRAM_BUDGET_MB")) {
        wchar_t* end = nullptr;
        unsigned long long mb = std::wcstoull(v->c_str(), &end, 10);
        if (end != v->c_str()) {
            budget = std::min<std::uint64_t>(budget, static_cast<std::uint64_t>(mb) << 20);
        }
    }
    return VideoMemory{budget, own.CurrentUsage, shared.CurrentUsage};
}

std::optional<Prelude> Device::make_prelude(D3D12_COMMAND_LIST_TYPE type) const {
    Prelude p;
    if (FAILED(device_->CreateCommandAllocator(type, IID_PPV_ARGS(&p.allocator))) ||
        FAILED(device_->CreateCommandList(0, type, p.allocator.Get(), nullptr, IID_PPV_ARGS(&p.list))) || FAILED(p.list->Close())) {
        return std::nullopt;
    }
    return p;
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::mapped(const D3D12_HEAP_PROPERTIES& heap, std::uint64_t size,
                                                                                D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) const {
    ComPtr<ID3D12Resource> buffer;
    auto desc = buffer_desc(size, flags);
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)))) {
        return std::nullopt;
    }
    void* data = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(buffer->Map(0, heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD ? &none : nullptr, &data))) {
        return std::nullopt;
    }
    return std::make_pair(buffer, static_cast<std::uint8_t*>(data));
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::shared_buffer(std::uint64_t size) const {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    return mapped(heap, size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
}

ComPtr<ID3D12Resource> Device::uav_buffer(std::uint64_t size) const {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> buffer;
    auto desc = buffer_desc(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::upload_buffer(std::uint64_t size) const {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    return mapped(heap, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::readback_buffer(std::uint64_t size) const {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    return mapped(heap, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::gpu_upload_buffer(std::uint64_t size) const {
    if (!features_.gpu_upload_heap) {
        return std::nullopt;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;
    return mapped(heap, size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
}

std::uint64_t Device::signal(ID3D12CommandQueue* queue) {
    ++fence_value_;
    check((queue ? queue : queue_.Get())->Signal(fence_.Get(), fence_value_), "Signal");
    return fence_value_;
}

void Device::wait(std::uint64_t value) {
    if (fence_->GetCompletedValue() >= value) {
        return;
    }
    check(fence_->SetEventOnCompletion(value, event_), "SetEventOnCompletion");
    WaitForSingleObject(event_, INFINITE);
}

void Device::execute_and_wait(std::initializer_list<ID3D12CommandList*> lists, ID3D12CommandQueue* queue) {
    ID3D12CommandQueue* q = queue ? queue : queue_.Get();
    q->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.begin());
    wait(signal(q));
}

}   
