// SPDX-License-Identifier: GPL-3.0-or-later
// Transfer paths the card path chooses between (milestone 3), measured on
// the adapter at hand:
//   cpu-write   the CPU writes 64 MiB into each kind of heap
//   gpu-copy    the card copies 256 MiB between system memory and VRAM
//   cpu-read    the CPU reads 64 MiB the card produced, from each kind of heap
//   shader-read a compute shader reads 256 MiB from VRAM / system memory
// Usage: gpu-bench [warp] [runs]
#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "gpu/device.hpp"

#pragma comment(lib, "d3dcompiler.lib")

using danzi::gpu::ComPtr;
using danzi::gpu::Device;

namespace {

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double time_ms(const std::function<void()>& f) {
    auto t = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void report(const char* what, std::uint64_t bytes_count, std::vector<double> ms) {
    double bytes = static_cast<double>(bytes_count);
    double m = median(ms);
    std::printf("  %-44s %9.3f ms  %7.1f GB/s\n", what, m, bytes / (m / 1e3) / 1e9);
}

void copy_list(Device& d, ID3D12Resource* dst, ID3D12Resource* src, std::uint64_t n) {
    auto p = d.make_prelude();
    p->allocator->Reset();
    p->list->Reset(p->allocator.Get(), nullptr);
    p->list->CopyBufferRegion(dst, 0, src, 0, n);
    p->list->Close();
    d.execute_and_wait({p->list.Get()});
}

}   

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool warp = argc > 1 && std::string(argv[1]) == "warp";
    int runs = argc > 2 ? std::atoi(argv[2]) : 9;
    auto d = Device::create(warp ? Device::Kind::Warp : Device::Kind::Hardware);
    const auto& a = d->adapter();
    const auto& f = d->features();
    std::printf("adapter: %ls (vendor 0x%04x, %llu MiB VRAM); GPU upload heap: %s; wave lanes %u-%u; runs %d (median shown)\n", a.name.c_str(), a.vendor,
                static_cast<unsigned long long>(a.dedicated_vram >> 20), f.gpu_upload_heap ? "yes" : "no", f.wave_lanes_min, f.wave_lanes_max, runs);
    if (auto vm = d->video_memory()) {
        std::printf("budget: %llu MiB of the card's memory, %llu MiB in use\n", static_cast<unsigned long long>(vm->budget >> 20),
                    static_cast<unsigned long long>(vm->usage >> 20));
    }
    const std::uint64_t SMALL = 64ull << 20, BIG = warp ? 64ull << 20 : 256ull << 20;
    std::vector<std::uint8_t> host(SMALL);
    for (std::uint64_t i = 0; i < SMALL; ++i) {
        host[i] = static_cast<std::uint8_t>(i * 31);
    }
    auto shared = d->shared_buffer(BIG);
    auto upload = d->upload_buffer(BIG);
    auto readback = d->readback_buffer(BIG);
    auto vram = d->uav_buffer(BIG);
    auto gpu_up = d->gpu_upload_buffer(SMALL);
    if (!shared || !upload || !readback || !vram) {
        std::printf("buffers could not be made\n");
        return 1;
    }

    std::printf("cpu-write (64 MiB memcpy into the heap)\n");
    {
        std::vector<double> t;
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { std::memcpy(shared->second, host.data(), SMALL); }));
        report("shared (custom L0, write-back)", SMALL, t);
        t.clear();
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { std::memcpy(upload->second, host.data(), SMALL); }));
        report("upload heap (write-combined)", SMALL, t);
        if (gpu_up) {
            t.clear();
            for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { std::memcpy(gpu_up->second, host.data(), SMALL); }));
            report("GPU upload heap (VRAM over the bar)", SMALL, t);
        }
    }

    std::printf("gpu-copy (%llu MiB CopyBufferRegion, wall time incl. submit and fence)\n", static_cast<unsigned long long>(BIG >> 20));
    {
        std::vector<double> t;
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { copy_list(*d, vram.Get(), shared->first.Get(), BIG); }));
        report("shared -> VRAM", BIG, t);
        t.clear();
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { copy_list(*d, vram.Get(), upload->first.Get(), BIG); }));
        report("upload -> VRAM", BIG, t);
        t.clear();
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { copy_list(*d, readback->first.Get(), vram.Get(), BIG); }));
        report("VRAM -> readback", BIG, t);
        t.clear();
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { copy_list(*d, shared->first.Get(), vram.Get(), BIG); }));
        report("VRAM -> shared", BIG, t);
        t.clear();
        for (int r = 0; r < runs; ++r) {
            t.push_back(time_ms([&] {
                auto p = d->make_prelude(D3D12_COMMAND_LIST_TYPE_COPY);
                p->allocator->Reset();
                p->list->Reset(p->allocator.Get(), nullptr);
                p->list->CopyBufferRegion(shared->first.Get(), 0, vram.Get(), 0, BIG);
                p->list->Close();
                d->execute_and_wait({p->list.Get()}, d->copy_queue());
            }));
        }
        report("VRAM -> shared, on the copy queue", BIG, t);
    }

    std::printf("cpu-read (sum of 64 MiB the card wrote)\n");
    {
        auto sum = [](const std::uint8_t* p, std::uint64_t n) {
            std::uint64_t s = 0;
            auto* w = reinterpret_cast<const std::uint64_t*>(p);
            for (std::uint64_t i = 0; i < n / 8; ++i) s += w[i];
            return s;
        };
        volatile std::uint64_t sink = 0;
        std::vector<double> t;
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { sink = sink + sum(host.data(), SMALL); }));
        report("ordinary memory (baseline)", SMALL, t);
        t.clear();
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { sink = sink + sum(shared->second, SMALL); }));
        report("shared (write-back)", SMALL, t);
        t.clear();
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { sink = sink + sum(readback->second, SMALL); }));
        report("readback heap", SMALL, t);
        t.clear();
        for (int r = 0; r < std::min(runs, 3); ++r) t.push_back(time_ms([&] { sink = sink + sum(upload->second, SMALL); }));
        report("upload heap (write-combined: never do this)", SMALL, t);
        if (gpu_up) {


            t.clear();
            for (int r = 0; r < std::min(runs, 3); ++r) t.push_back(time_ms([&] { sink = sink + sum(gpu_up->second, SMALL); }));
            report("GPU upload heap (VRAM over the bar: the rebar copies)", SMALL, t);

            t.clear();
            for (int r = 0; r < std::min(runs, 3); ++r) {
                t.push_back(time_ms([&] {
                    copy_list(*d, readback->first.Get(), gpu_up->first.Get(), SMALL);
                    sink = sink + sum(readback->second, SMALL);
                }));
            }
            report("GPU upload heap, copied to a read-back heap, then read", SMALL, t);
        }
    }

    std::printf("shader-read (a compute shader sums %llu MiB)\n", static_cast<unsigned long long>(BIG >> 20));
    {
        static const char* src = R"hlsl(
#define RS "RootConstants(num32BitConstants=1, b0), SRV(t0), UAV(u0)"
cbuffer C : register(b0) { uint words; };
ByteAddressBuffer src_buf : register(t0);
RWByteAddressBuffer out_buf : register(u0);
[RootSignature(RS)] [numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 g : SV_GroupID) {
    uint4 s = 0;
    for (uint i = id.x * 4; i < words; i += 256 * 1024 * 4) s += src_buf.Load4(i * 4);
    out_buf.Store(id.x * 4, s.x ^ s.y ^ s.z ^ s.w);
})hlsl";
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(src, std::strlen(src), "sum", nullptr, nullptr, "main", "cs_5_1", 0, 0, &code, &errors))) {
            std::printf("compile failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            return 1;
        }
        ComPtr<ID3D12RootSignature> root;
        d->device()->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.Get();
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pso;
        d->device()->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        auto out = d->uav_buffer(256 * 1024 * 4);
        constexpr int PASSES = 20;
        auto run = [&](ID3D12Resource* src_res) {
            auto p = d->make_prelude();
            p->allocator->Reset();
            p->list->Reset(p->allocator.Get(), pso.Get());
            p->list->SetComputeRootSignature(root.Get());
            p->list->SetComputeRoot32BitConstant(0, static_cast<UINT>(BIG / 4), 0);
            p->list->SetComputeRootShaderResourceView(1, src_res->GetGPUVirtualAddress());
            p->list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());


            for (int k = 0; k < PASSES; ++k) {
                p->list->Dispatch(1024, 1, 1);
            }
            p->list->Close();
            d->execute_and_wait({p->list.Get()});
        };
        run(vram.Get());   
        std::vector<double> t;
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { run(vram.Get()); }) / PASSES);
        report("from VRAM (default heap), per pass", BIG, t);
        t.clear();
        for (int r = 0; r < runs; ++r) t.push_back(time_ms([&] { run(shared->first.Get()); }) / PASSES);
        report("from system memory (custom L0, zero-copy), per pass", BIG, t);
    }


    std::printf("shader-write (64 MiB stored by a compute shader, as the inline ring stores pieces)\n");
    {
        static const char* src = R"hlsl(
#define RS "RootConstants(num32BitConstants=1, b0), UAV(u0)"
cbuffer C : register(b0) { uint seed; };
RWByteAddressBuffer out_buf : register(u0);
[RootSignature(RS)] [numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint base = gid.x * 65536u;
    [loop]
    for (uint off = gtid.x * 16u; off < 65536u; off += 1024u) {
        out_buf.Store4(base + off, uint4(off, seed, gid.x, gtid.x));
    }
})hlsl";
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(src, std::strlen(src), "store", nullptr, nullptr, "main", "cs_5_1", 0, 0, &code, &errors))) {
            std::printf("compile failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            return 1;
        }
        ComPtr<ID3D12RootSignature> root;
        d->device()->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.Get();
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pso;
        d->device()->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        D3D12_HEAP_PROPERTIES wc{};
        wc.Type = D3D12_HEAP_TYPE_CUSTOM;
        wc.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        wc.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        ComPtr<ID3D12Resource> wc_buf;
        auto wc_desc = danzi::gpu::buffer_desc(SMALL, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        d->device()->CreateCommittedResource(&wc, D3D12_HEAP_FLAG_NONE, &wc_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&wc_buf));
        auto run = [&](ID3D12Resource* dst, UINT groups) {
            auto p = d->make_prelude();
            p->allocator->Reset();
            p->list->Reset(p->allocator.Get(), pso.Get());
            p->list->SetComputeRootSignature(root.Get());
            p->list->SetComputeRoot32BitConstant(0, 7, 0);
            p->list->SetComputeRootUnorderedAccessView(1, dst->GetGPUVirtualAddress());
            p->list->Dispatch(groups, 1, 1);
            p->list->Close();
            d->execute_and_wait({p->list.Get()});
        };
        const UINT pieces = static_cast<UINT>(SMALL / 65536);
        struct Target {
            const char* what;
            ID3D12Resource* r;
        } targets[] = {{"into VRAM (default heap)", vram.Get()},
                       {"into shared (custom L0, write-back): the ring", shared->first.Get()},
                       {"into custom L0, write-combined", wc_buf.Get()}};
        for (const auto& tg : targets) {
            if (!tg.r) {
                continue;
            }
            run(tg.r, 1);   
            std::vector<double> t;
            for (int r = 0; r < std::min(runs, 5); ++r) t.push_back(time_ms([&] { run(tg.r, pieces); }));
            report(tg.what, SMALL, t);
        }

        for (UINT n : {1u, 4u, 16u, 20u, 24u, 28u, 32u, 40u, 48u, 64u, 128u, 256u, 512u}) {
            std::vector<double> ts;
            run(shared->first.Get(), n);
            for (int r = 0; r < std::min(runs, 5); ++r) ts.push_back(time_ms([&] { run(shared->first.Get(), n); }));
            char what[96];
            std::snprintf(what, sizeof what, "into shared, %u pieces", n);
            report(what, std::uint64_t{n} * 65536, ts);
            ts.clear();
            for (int r = 0; r < std::min(runs, 5); ++r) {
                ts.push_back(time_ms([&] {
                    run(vram.Get(), n);
                    copy_list(*d, shared->first.Get(), vram.Get(), std::uint64_t{n} * 65536);
                }));
            }
            std::snprintf(what, sizeof what, "into VRAM + copy down, %u pieces", n);
            report(what, std::uint64_t{n} * 65536, ts);
        }


        {
            auto preds = d->uav_buffer(128 * 8);   
            auto ones = d->uav_buffer(128 * 8);
            auto down = d->shared_buffer(128ull << 20);
            constexpr int REPS = 20;
            auto list_ms = [&](auto&& record) {
                std::vector<double> ts;
                for (int r = 0; r < std::min(runs, 5); ++r) {
                    ts.push_back(time_ms([&] {
                        auto p = d->make_prelude();
                        p->allocator->Reset();
                        p->list->Reset(p->allocator.Get(), pso.Get());
                        record(p->list.Get());
                        p->list->Close();
                        d->execute_and_wait({p->list.Get()});
                    }) / REPS);
                }
                return ts;
            };
            D3D12_RESOURCE_BARRIER to_pred[2] = {}, uav{};
            for (int i = 0; i < 2; ++i) {
                to_pred[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                to_pred[i].Transition.pResource = i == 0 ? preds.Get() : ones.Get();
                to_pred[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                to_pred[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                to_pred[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PREDICATION;
            }
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            auto skipped = list_ms([&](ID3D12GraphicsCommandList* l) {
                l->ResourceBarrier(2, to_pred);
                for (int k = 0; k < REPS; ++k) {
                    for (UINT b = 0; b < 128; ++b) {
                        l->SetPredication(preds.Get(), b * 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
                        l->CopyBufferRegion(down->first.Get(), std::uint64_t{b} << 20, vram.Get(), std::uint64_t{b} << 20, 1 << 20);
                    }
                    l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                }
            });
            report("128 predicated copies, all skipped (per list)", 0, skipped);

        }


        {
            static const char cmp[] =
#include "../src/gpu/card_compare.inc"
                ;
            ComPtr<ID3DBlob> ccode;
            if (SUCCEEDED(D3DCompile(cmp, std::strlen(cmp), "card compare", nullptr, nullptr, "main", "cs_5_1", 0, 0, &ccode, nullptr))) {
                ComPtr<ID3D12RootSignature> croot;
                d->device()->CreateRootSignature(0, ccode->GetBufferPointer(), ccode->GetBufferSize(), IID_PPV_ARGS(&croot));
                D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
                cd.pRootSignature = croot.Get();
                cd.CS = {ccode->GetBufferPointer(), ccode->GetBufferSize()};
                ComPtr<ID3D12PipelineState> cpso;
                d->device()->CreateComputePipelineState(&cd, IID_PPV_ARGS(&cpso));
                const UINT pieces = static_cast<UINT>(BIG / 65536);
                auto hashes = d->uav_buffer(std::uint64_t{pieces + 2} * 16);
                auto res_sys = d->shared_buffer(std::uint64_t{pieces} * 4);
                auto res_vram = d->uav_buffer(std::uint64_t{pieces} * 4);
                auto ringb = d->uav_buffer(1 << 20);
                for (int where = 0; where < 2; ++where) {
                    ID3D12Resource* res = where == 0 ? res_sys->first.Get() : res_vram.Get();
                    std::vector<double> ts;
                    constexpr int PASSES = 10;
                    for (int r = 0; r < std::min(runs, 5); ++r) {
                        ts.push_back(time_ms([&] {
                            auto p = d->make_prelude();
                            p->allocator->Reset();
                            p->list->Reset(p->allocator.Get(), cpso.Get());
                            p->list->SetComputeRootSignature(croot.Get());
                            std::uint32_t c[7] = {0, 0, static_cast<std::uint32_t>(BIG), pieces, 0, 1, 0};
                            p->list->SetComputeRoot32BitConstants(0, 7, c, 0);
                            p->list->SetComputeRootShaderResourceView(1, vram->GetGPUVirtualAddress());
                            p->list->SetComputeRootUnorderedAccessView(2, hashes->GetGPUVirtualAddress());
                            p->list->SetComputeRootUnorderedAccessView(3, res->GetGPUVirtualAddress());
                            p->list->SetComputeRootUnorderedAccessView(4, ringb->GetGPUVirtualAddress());
                            for (int k = 0; k < PASSES; ++k) {
                                p->list->Dispatch(pieces, 1, 1);
                            }
                            p->list->Close();
                            d->execute_and_wait({p->list.Get()});
                        }) / PASSES);
                    }
                    report(where == 0 ? "CARD_COMPARE 256 MiB, results in PC memory, per pass" : "CARD_COMPARE 256 MiB, results in VRAM, per pass", BIG, ts);
                }
            }
        }

        std::vector<double> t;
        for (int r = 0; r < std::min(runs, 5); ++r) {
            t.push_back(time_ms([&] {
                run(vram.Get(), pieces);
                copy_list(*d, shared->first.Get(), vram.Get(), SMALL);
            }));
        }
        report("into VRAM, then one copy to shared (the fetch)", SMALL, t);
    }
    return 0;
}
