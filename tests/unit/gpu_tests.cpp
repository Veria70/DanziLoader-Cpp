// SPDX-License-Identifier: GPL-3.0-or-later
// The D3D12 device layer (milestone 3), on the hardware adapter and on WARP.
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "check.hpp"
#include "gpu/card.hpp"
#include "gpu/inplace_heap.hpp"
#include "gpu/device.hpp"

#pragma comment(lib, "d3dcompiler.lib")

using danzi::gpu::ComPtr;
using danzi::gpu::Device;

namespace {

void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    l->ResourceBarrier(1, &b);
}


void round_trip(Device& d) {
    constexpr std::uint64_t N = 1 << 20;
    auto shared = d.shared_buffer(N);
    auto vram = d.uav_buffer(N);
    auto back = d.readback_buffer(N);
    auto shared2 = d.shared_buffer(N);
    CHECK(shared && vram && back && shared2);
    for (std::uint64_t i = 0; i < N; ++i) {
        shared->second[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    auto p = d.make_prelude();
    CHECK(p.has_value());
    p->allocator->Reset();
    p->list->Reset(p->allocator.Get(), nullptr);

    p->list->CopyBufferRegion(vram.Get(), 0, shared->first.Get(), 0, N);
    barrier(p->list.Get(), vram.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    p->list->CopyBufferRegion(back->first.Get(), 0, vram.Get(), 0, N);
    p->list->CopyBufferRegion(shared2->first.Get(), 0, vram.Get(), 0, N);
    CHECK(SUCCEEDED(p->list->Close()));
    d.execute_and_wait({p->list.Get()});
    CHECK(std::memcmp(shared->second, back->second, N) == 0);
    CHECK(std::memcmp(shared->second, shared2->second, N) == 0);
}


void compute(Device& d) {
    static const char* src = R"hlsl(
#define RS "RootConstants(num32BitConstants=1, b0), UAV(u0)"
cbuffer C : register(b0) { uint seed; };
RWByteAddressBuffer out_buf : register(u0);
[RootSignature(RS)] [numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { out_buf.Store(id.x * 4, id.x * 2654435761u ^ seed); }
)hlsl";
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src, std::strlen(src), "test", nullptr, nullptr, "main", "cs_5_1", 0, 0, &code, &errors);
    if (FAILED(hr) && errors) {
        check::fail(__FILE__, __LINE__, static_cast<const char*>(errors->GetBufferPointer()));
    }
    ComPtr<ID3D12RootSignature> root;
    CHECK(SUCCEEDED(d.device()->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(SUCCEEDED(d.device()->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))));
    constexpr UINT THREADS = 64 * 256;
    auto out = d.shared_buffer(THREADS * 4);
    CHECK(out.has_value());
    auto p = d.make_prelude();
    p->allocator->Reset();
    p->list->Reset(p->allocator.Get(), pso.Get());
    p->list->SetComputeRootSignature(root.Get());
    p->list->SetComputeRoot32BitConstant(0, 0x1234u, 0);
    p->list->SetComputeRootUnorderedAccessView(1, out->first->GetGPUVirtualAddress());
    p->list->Dispatch(THREADS / 64, 1, 1);
    CHECK(SUCCEEDED(p->list->Close()));
    d.execute_and_wait({p->list.Get()});
    auto* words = reinterpret_cast<const std::uint32_t*>(out->second);
    for (UINT i = 0; i < THREADS; ++i) {
        if (words[i] != ((i * 2654435761u) ^ 0x1234u)) {
            check::fail(__FILE__, __LINE__, "shader word " + std::to_string(i));
        }
    }
}


void predicated_copies(Device& d) {
    constexpr std::uint64_t BLOCK = 64 << 10, BLOCKS = 4;
    static const char* src = R"hlsl(
#define RS "RootConstants(num32BitConstants=1, b0), UAV(u0)"
cbuffer C : register(b0) { uint mask; };
RWByteAddressBuffer preds : register(u0);
[RootSignature(RS)] [numthreads(4, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { preds.Store2(id.x * 8, uint2((mask >> id.x) & 1u, 0u)); }
)hlsl";
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src, std::strlen(src), "preds", nullptr, nullptr, "main", "cs_5_1", 0, 0, &code, &errors);
    if (FAILED(hr) && errors) {
        check::fail(__FILE__, __LINE__, static_cast<const char*>(errors->GetBufferPointer()));
    }
    ComPtr<ID3D12RootSignature> root;
    CHECK(SUCCEEDED(d.device()->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(SUCCEEDED(d.device()->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))));
    auto staging = d.shared_buffer(BLOCK * BLOCKS);
    auto vram = d.uav_buffer(BLOCK * BLOCKS);
    auto preds = d.uav_buffer(8 * BLOCKS);
    auto out = d.shared_buffer(BLOCK * BLOCKS);
    CHECK(staging && vram && preds && out);
    for (std::uint64_t i = 0; i < BLOCK * BLOCKS; ++i) {
        staging->second[i] = static_cast<std::uint8_t>(i * 13 + 1);
    }
    std::memset(out->second, 0, BLOCK * BLOCKS);
    auto p = d.make_prelude();
    p->allocator->Reset();
    p->list->Reset(p->allocator.Get(), pso.Get());
    p->list->CopyBufferRegion(vram.Get(), 0, staging->first.Get(), 0, BLOCK * BLOCKS);
    p->list->SetComputeRootSignature(root.Get());
    p->list->SetComputeRoot32BitConstant(0, 0b0101u, 0);   
    p->list->SetComputeRootUnorderedAccessView(1, preds->GetGPUVirtualAddress());
    p->list->Dispatch(1, 1, 1);
    barrier(p->list.Get(), preds.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PREDICATION);
    barrier(p->list.Get(), vram.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (std::uint64_t b = 0; b < BLOCKS; ++b) {
        p->list->SetPredication(preds.Get(), b * 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
        p->list->CopyBufferRegion(out->first.Get(), b * BLOCK, vram.Get(), b * BLOCK, BLOCK);
    }
    p->list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    CHECK(SUCCEEDED(p->list->Close()));
    d.execute_and_wait({p->list.Get()});
    for (std::uint64_t b = 0; b < BLOCKS; ++b) {
        bool copied = std::memcmp(out->second + b * BLOCK, staging->second + b * BLOCK, BLOCK) == 0;
        bool zero = std::all_of(out->second + b * BLOCK, out->second + (b + 1) * BLOCK, [](std::uint8_t v) { return v == 0; });
        if (b % 2 == 0 ? !copied : !zero) {
            check::fail(__FILE__, __LINE__, "predicated copy of block " + std::to_string(b) + (b % 2 == 0 ? " did not run" : " ran"));
        }
    }
}

}   

TEST(gpu_predicated_copies_hardware) {
    auto d = Device::create(Device::Kind::Hardware);
    predicated_copies(*d);
}

TEST(gpu_predicated_copies_warp) {
    auto d = Device::create(Device::Kind::Warp);
    predicated_copies(*d);
}

TEST(gpu_device_hardware) {
    auto d = Device::create(Device::Kind::Hardware);
    const auto& a = d->adapter();
    const auto& f = d->features();
    std::printf("        adapter: %ls (vendor 0x%04x, %llu MiB VRAM), SM %x, heap tier %d, waves %u-%u, GPU upload heap %s\n", a.name.c_str(), a.vendor,
                static_cast<unsigned long long>(a.dedicated_vram >> 20), f.shader_model, f.heap_tier, f.wave_lanes_min, f.wave_lanes_max,
                f.gpu_upload_heap ? "yes" : "no");
    CHECK(!a.software);
    auto vm = d->video_memory();
    CHECK(vm.has_value() && vm->budget > 0);
    if (a.nvidia()) {

        CHECK_EQ(f.wave_lanes_max, 32u);
    }
    round_trip(*d);
    compute(*d);
}

TEST(gpu_device_warp) {
    auto d = Device::create(Device::Kind::Warp);
    CHECK(d->adapter().software);
    round_trip(*d);
    compute(*d);
}


namespace {
struct TestBudget {
    explicit TestBudget(const wchar_t* mb) { SetEnvironmentVariableW(L"VICTORY_TEST_VRAM_BUDGET_MB", mb); }
    ~TestBudget() { SetEnvironmentVariableW(L"VICTORY_TEST_VRAM_BUDGET_MB", nullptr); }
};
}   

TEST(card_budget_places_the_ring) {
    {
        TestBudget b(L"1024");   
        auto d = Device::create(Device::Kind::Hardware);
        danzi::gpu::CardHost host(*d);
        CHECK(host.card_ready());
        CHECK(host.own_vram() < (std::uint64_t{16} << 20));
    }
    {
        TestBudget b(L"3072");   
        auto d = Device::create(Device::Kind::Hardware);
        danzi::gpu::CardHost host(*d);
        CHECK(host.card_ready());
        CHECK(host.own_vram() > (std::uint64_t{128} << 20));
    }
}

TEST(card_budget_refuses_past_the_share) {
    TestBudget b(L"512");   
    auto d = Device::create(Device::Kind::Hardware);
    danzi::gpu::CardHost host(*d);
    constexpr std::uint64_t MB48 = std::uint64_t{48} << 20;
    std::vector<std::uint8_t> title(3 * MB48);
    auto base = reinterpret_cast<std::uint64_t>(title.data());
    auto a = host.make_carded(base, MB48);
    auto c = host.make_carded(base + MB48, MB48);
    auto e = host.make_carded(base + 2 * MB48, MB48);
    CHECK(a && c && e);
    CHECK(a->carded() && c->carded());
    CHECK(!e->carded());
    CHECK(e->buffer() != nullptr);   
}


namespace {
void in_place(Device& d) {
    constexpr std::size_t MB = 1 << 20;
    auto* region = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, MB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(region != nullptr);
    for (std::size_t i = 0; i < MB; ++i) {
        region[i] = static_cast<std::uint8_t>(i * 5 + 1);
    }
    auto start = reinterpret_cast<std::uint64_t>(region) + 0x10007;
    HRESULT hr = S_OK;
    auto placed = danzi::gpu::inplace::place_part(d, start, 0x20000, &hr);
    if (!placed) {
        char b[64];
        std::snprintf(b, sizeof b, "not placed: 0x%08lx", static_cast<unsigned long>(hr));
        check::fail(__FILE__, __LINE__, b);
    }
    CHECK_EQ(placed->bias, 7u);

    auto src = d.shared_buffer(4096);
    for (int i = 0; i < 4096; ++i) {
        src->second[i] = static_cast<std::uint8_t>(0xA0 ^ i);
    }
    auto p = d.make_prelude();
    p->allocator->Reset();
    p->list->Reset(p->allocator.Get(), nullptr);
    p->list->CopyBufferRegion(placed->buffer.Get(), placed->bias, src->first.Get(), 0, 4096);
    CHECK(SUCCEEDED(p->list->Close()));
    d.execute_and_wait({p->list.Get()});
    CHECK(std::memcmp(region + 0x10007, src->second, 4096) == 0);
    CHECK_EQ(region[0x10006], static_cast<std::uint8_t>(0x10006 * 5 + 1));

    CHECK(!VirtualFree(region + 0x10000, 0x10000, MEM_DECOMMIT));
    placed.reset();
    CHECK(VirtualFree(region + 0x10000, 0x10000, MEM_DECOMMIT));
    VirtualFree(region, 0, MEM_RELEASE);
}
}   

TEST(gpu_in_place_hardware) {
    auto d = Device::create(Device::Kind::Hardware);
    in_place(*d);
}

TEST(gpu_in_place_warp) {
    auto d = Device::create(Device::Kind::Warp);
    in_place(*d);
}
