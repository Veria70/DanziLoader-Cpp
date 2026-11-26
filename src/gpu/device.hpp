// SPDX-License-Identifier: GPL-3.0-or-later
// The host D3D12 device as the card path needs it (milestone 3): adapter,
// queues, a fence, command lists, the buffers the reference's host copies use
// (`Host::make_prelude`, `shared_buffer`, `uav_buffer`, `video_memory` in
// crates/victory-gpu/src/host.rs; DanziLoader / victory-perf, see NOTICE.md),
// and the features later steps choose by.
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
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

class BufferPool;


class Pooled {
public:
    Pooled() = default;
    Pooled(std::shared_ptr<BufferPool> pool, ComPtr<ID3D12Resource> buffer, std::uint8_t* data, std::uint32_t bucket)
        : pool_(std::move(pool)), buffer_(std::move(buffer)), data_(data), bucket_(bucket) {}
    Pooled(Pooled&&) noexcept = default;
    Pooled& operator=(Pooled&& other) noexcept {
        if (this != &other) {
            give_back();
            pool_ = std::move(other.pool_);
            buffer_ = std::move(other.buffer_);
            data_ = other.data_;
            bucket_ = other.bucket_;
        }
        return *this;
    }
    ~Pooled() { give_back(); }
    ID3D12Resource* get() const { return buffer_.Get(); }
    std::uint8_t* data() const { return data_; }
    explicit operator bool() const { return buffer_ != nullptr; }

private:
    void give_back();
    std::shared_ptr<BufferPool> pool_;
    ComPtr<ID3D12Resource> buffer_;
    std::uint8_t* data_ = nullptr;
    std::uint32_t bucket_ = 0;
};


class BufferPool : public std::enable_shared_from_this<BufferPool> {
public:
    static constexpr std::uint32_t CLASSES = 9, UNPOOLED = 0xFFFFFFFF;
    static constexpr std::uint64_t KEPT = 64ull << 20;
    BufferPool(ComPtr<ID3D12Device> device, ComPtr<ID3D12Fence> fence, const std::atomic<std::uint64_t>* signalled, D3D12_HEAP_TYPE type)
        : device_(std::move(device)), fence_(std::move(fence)), signalled_(signalled), type_(type) {}
    std::optional<Pooled> take(std::uint64_t size);
    void give(ComPtr<ID3D12Resource> buffer, std::uint8_t* data, std::uint32_t bucket);

private:
    struct Entry {
        ComPtr<ID3D12Resource> buffer;
        std::uint8_t* data;
        std::uint64_t stamp;
    };
    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> make(std::uint64_t size) const;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12Fence> fence_;
    const std::atomic<std::uint64_t>* signalled_;
    D3D12_HEAP_TYPE type_;
    std::mutex lock_;
    std::array<std::vector<Entry>, CLASSES> free_;
    std::uint64_t kept_ = 0, completed_ = 0;
};

class Device {
public:
    enum class Kind { Hardware, Warp };


    static std::unique_ptr<Device> create(Kind kind = Kind::Hardware, bool debug = false);
    ~Device();

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    ID3D12CommandQueue* copy_queue() const { return copy_queue_.Get(); }


    ID3D12Fence* fence() const { return fence_.Get(); }
    const AdapterInfo& adapter() const { return adapter_; }
    const Features& features() const { return features_; }

    std::optional<VideoMemory> video_memory() const;


    void log_video_memory(const char* when) const;


    static void set_frame_clock(std::uint64_t (*clock)());

    std::optional<Prelude> make_prelude(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) const;


    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> shared_buffer(std::uint64_t size) const;

    ComPtr<ID3D12Resource> uav_buffer(std::uint64_t size) const;

    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> upload_buffer(std::uint64_t size) const;
    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> readback_buffer(std::uint64_t size) const;

    std::optional<Pooled> pooled_upload(std::uint64_t size) const { return uploads_->take(size); }
    std::optional<Pooled> pooled_readback(std::uint64_t size) const { return readbacks_->take(size); }


    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> gpu_upload_buffer(std::uint64_t size) const;


    void execute_and_wait(std::initializer_list<ID3D12CommandList*> lists, ID3D12CommandQueue* queue = nullptr);

    std::uint64_t signal(ID3D12CommandQueue* queue = nullptr);
    void wait(std::uint64_t value);

    HRESULT wait_for(std::uint64_t value);

private:
    Device() = default;

    std::optional<std::pair<ComPtr<ID3D12Resource>, std::uint8_t*>> mapped(const D3D12_HEAP_PROPERTIES& heap, std::uint64_t size,
                                                                          D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                                                          const char* what) const;

    ComPtr<IDXGIAdapter3> dxgi_adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_, copy_queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    std::atomic<std::uint64_t> fence_value_ = 0;
    std::shared_ptr<BufferPool> uploads_, readbacks_;
    AdapterInfo adapter_;
    Features features_;
};


D3D12_RESOURCE_DESC buffer_desc(std::uint64_t size, D3D12_RESOURCE_FLAGS flags);

}   
