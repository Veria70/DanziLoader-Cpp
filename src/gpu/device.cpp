// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.hpp"

#include "core/env.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace danzi::gpu {

namespace {


std::atomic<std::uint64_t (*)()> FRAME_CLOCK{nullptr};

void CALLBACK validation_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                                 D3D12_MESSAGE_ID id, LPCSTR message, void*) {
    if (severity > D3D12_MESSAGE_SEVERITY_ERROR) return;
    static std::atomic_bool seen[D3D12_MESSAGE_ID_D3D12_MESSAGES_END]{};
    const auto index = static_cast<unsigned>(id);
    if (index < std::size(seen) && seen[index].exchange(true)) return;
    std::fprintf(stderr, "D3D12 validation id=%u %s\n", index, message);
    std::fflush(stderr);
}

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
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&d)), "D3D12 debug interface");
        d->EnableDebugLayer();
    }
    if (debug || env::is(L"VICTORY_D3D12_DRED", L"1")) {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
            settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            std::fprintf(stderr, "D3D12 auto-breadcrumbs active\n");
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
    if (debug) {
        ComPtr<ID3D12InfoQueue1> info;
        check(d->device_.As(&info), "device info queue");
        DWORD cookie{};
        check(info->RegisterMessageCallback(validation_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie), "debug callback");
        std::fprintf(stderr, "D3D12 debug layer and validation callback active\n");
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
    d->uploads_ = std::make_shared<BufferPool>(d->device_, d->fence_, &d->fence_value_, D3D12_HEAP_TYPE_UPLOAD);
    d->readbacks_ = std::make_shared<BufferPool>(d->device_, d->fence_, &d->fence_value_, D3D12_HEAP_TYPE_READBACK);
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


    static std::atomic<std::uint64_t> LAST[3]{};
    const std::uint64_t now[3] = {own.Budget, own.CurrentUsage, shared.CurrentUsage};
    std::uint64_t last[3];
    for (int i = 0; i < 3; ++i) {
        last[i] = LAST[i].exchange(now[i], std::memory_order_relaxed);
    }
    if (last[0] != 0 && (last[0] > own.Budget ? last[0] - own.Budget : own.Budget - last[0]) >= (std::uint64_t{256} << 20)) {
        auto clock = FRAME_CLOCK.load(std::memory_order_relaxed);
        char b[320];
        std::snprintf(b, sizeof b,
                      "the system changed the budget of the card's memory from %llu MB to %llu MB (frame %llu); at the last look %llu MB of the card's and "
                      "%llu MB of the PC's shared memory were in use",
                      static_cast<unsigned long long>(last[0] >> 20), static_cast<unsigned long long>(own.Budget >> 20),
                      static_cast<unsigned long long>(clock ? clock() : 0), static_cast<unsigned long long>(last[1] >> 20),
                      static_cast<unsigned long long>(last[2] >> 20));
        log::info("victory_gpu::host", b);
        log_video_memory("right after that change");
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

void Device::set_frame_clock(std::uint64_t (*clock)()) { FRAME_CLOCK.store(clock, std::memory_order_relaxed); }

void Device::log_video_memory(const char* when) const {
    if (!dxgi_adapter_) {
        return;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO own{}, shared{};
    if (FAILED(dxgi_adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &own)) ||
        FAILED(dxgi_adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared))) {
        return;
    }
    auto mb = [](std::uint64_t v) { return static_cast<unsigned long long>(v >> 20); };
    char b[512];
    std::snprintf(b, sizeof b,
                  "video memory as the system counts it, %s: the card's own: budget %llu MB, in use %llu MB, %llu MB could still be reserved, %llu MB "
                  "reserved; the PC's shared with the card: budget %llu MB, in use %llu MB, %llu MB could still be reserved, %llu MB reserved (the "
                  "layer reserves none)",
                  when, mb(own.Budget), mb(own.CurrentUsage), mb(own.AvailableForReservation), mb(own.CurrentReservation), mb(shared.Budget),
                  mb(shared.CurrentUsage), mb(shared.AvailableForReservation), mb(shared.CurrentReservation));
    log::info("victory_gpu::host", b);
}

std::optional<Prelude> Device::make_prelude(D3D12_COMMAND_LIST_TYPE type) const {
    Prelude p;
    HRESULT hr;
    if (FAILED(hr = device_->CreateCommandAllocator(type, IID_PPV_ARGS(&p.allocator))) ||
        FAILED(hr = device_->CreateCommandList(0, type, p.allocator.Get(), nullptr, IID_PPV_ARGS(&p.list))) || FAILED(hr = p.list->Close())) {
        log::warn("victory_gpu::host", "no host list for a prelude", {{"error", log::hresult(hr)}});
        return std::nullopt;
    }
    return p;
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::mapped(const D3D12_HEAP_PROPERTIES& heap, std::uint64_t size,
                                                                                D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                                                                const char* what) const {
    ComPtr<ID3D12Resource> buffer;
    auto desc = buffer_desc(size, flags);
    HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    void* data = nullptr;
    D3D12_RANGE none{0, 0};
    if (SUCCEEDED(hr)) {
        hr = buffer->Map(0, heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD ? &none : nullptr, &data);
    }
    if (FAILED(hr)) {
        log::warn("victory_gpu::host", what, {{"error", log::hresult(hr)}, {"size", log::debug(size)}});
        return std::nullopt;
    }
    return std::make_pair(buffer, static_cast<std::uint8_t*>(data));
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::shared_buffer(std::uint64_t size) const {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    return mapped(heap, size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, "no shared buffer");
}

ComPtr<ID3D12Resource> Device::uav_buffer(std::uint64_t size) const {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> buffer;
    auto desc = buffer_desc(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer));
        FAILED(hr)) {
        log::warn("victory_gpu::host", "no unordered-access buffer", {{"error", log::hresult(hr)}, {"size", log::debug(size)}});
        return nullptr;
    }
    return buffer;
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::upload_buffer(std::uint64_t size) const {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    return mapped(heap, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "no upload buffer");
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::readback_buffer(std::uint64_t size) const {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    return mapped(heap, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, "no read-back buffer");
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> Device::gpu_upload_buffer(std::uint64_t size) const {
    if (!features_.gpu_upload_heap) {
        return std::nullopt;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;
    return mapped(heap, size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, "no GPU upload buffer (ReBAR)");
}

std::uint64_t Device::signal(ID3D12CommandQueue* queue) {
    std::uint64_t v = fence_value_.load(std::memory_order_relaxed) + 1;
    check((queue ? queue : queue_.Get())->Signal(fence_.Get(), v), "Signal");
    fence_value_.store(v, std::memory_order_release);
    return v;
}

void Pooled::give_back() {
    if (buffer_ && pool_ && bucket_ != BufferPool::UNPOOLED) {
        pool_->give(std::move(buffer_), data_, bucket_);
    }
    buffer_.Reset();
    pool_.reset();
}

std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> BufferPool::make(std::uint64_t size) const {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type_;
    ComPtr<ID3D12Resource> buffer;
    auto desc = buffer_desc(size, D3D12_RESOURCE_FLAG_NONE);
    D3D12_RESOURCE_STATES state = type_ == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    void* data = nullptr;
    D3D12_RANGE none{0, 0};
    if (SUCCEEDED(hr)) {
        hr = buffer->Map(0, type_ == D3D12_HEAP_TYPE_UPLOAD ? &none : nullptr, &data);
    }
    if (FAILED(hr)) {
        log::warn("victory_gpu::host", type_ == D3D12_HEAP_TYPE_UPLOAD ? "no upload buffer" : "no read-back buffer",
                  {{"error", log::hresult(hr)}, {"size", log::debug(size)}});
        return std::nullopt;
    }
    return std::make_pair(std::move(buffer), static_cast<std::uint8_t*>(data));
}

std::optional<Pooled> BufferPool::take(std::uint64_t size) {
    std::uint32_t bucket = 0;
    while (bucket < CLASSES && (std::uint64_t{65536} << bucket) < size) {
        ++bucket;
    }
    if (bucket == CLASSES) {
        auto made = make(size);
        if (!made) {
            return std::nullopt;
        }
        return Pooled(nullptr, std::move(made->first), made->second, UNPOOLED);
    }
    {
        std::lock_guard lock(lock_);
        auto& list = free_[bucket];
        if (!list.empty()) {

            for (std::size_t i = list.size(); i-- > 0;) {
                if (list[i].stamp > completed_) {
                    completed_ = fence_->GetCompletedValue();
                    if (list[i].stamp > completed_) {
                        continue;
                    }
                }
                Entry e = std::move(list[i]);
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                kept_ -= std::uint64_t{65536} << bucket;
                return Pooled(shared_from_this(), std::move(e.buffer), e.data, bucket);
            }
        }
    }
    auto made = make(std::uint64_t{65536} << bucket);
    if (!made) {
        return std::nullopt;
    }
    return Pooled(shared_from_this(), std::move(made->first), made->second, bucket);
}

void BufferPool::give(ComPtr<ID3D12Resource> buffer, std::uint8_t* data, std::uint32_t bucket) {
    std::uint64_t bytes = std::uint64_t{65536} << bucket;
    std::uint64_t stamp = signalled_->load(std::memory_order_acquire);
    std::lock_guard lock(lock_);
    if (kept_ + bytes > KEPT) {
        return;
    }
    kept_ += bytes;
    free_[bucket].push_back({std::move(buffer), data, stamp});
}

void Device::wait(std::uint64_t value) { check(wait_for(value), "SetEventOnCompletion"); }

HRESULT Device::wait_for(std::uint64_t value) {
    if (fence_->GetCompletedValue() >= value) {
        return S_OK;
    }
    HRESULT hr = fence_->SetEventOnCompletion(value, event_);
    if (SUCCEEDED(hr)) {
        WaitForSingleObject(event_, INFINITE);
    }
    return hr;
}

void Device::execute_and_wait(std::initializer_list<ID3D12CommandList*> lists, ID3D12CommandQueue* queue) {
    ID3D12CommandQueue* q = queue ? queue : queue_.Get();
    q->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.begin());
    wait(signal(q));
}

}   
