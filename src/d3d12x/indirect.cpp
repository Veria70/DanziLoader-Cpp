// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/indirect.rs (DanziLoader / victory-perf,
// see NOTICE.md). See indirect.hpp.
#include "indirect.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <mutex>

#include "clamp_hlsl.inc"
#include "core/env.hpp"
#include "core/log.hpp"
#include "host.hpp"

namespace danzi::d3d12x {

namespace {

using Microsoft::WRL::ComPtr;

struct Clamp {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
};

const Clamp* clamp() {
    static const Clamp* const made = []() -> const Clamp* {
        auto refused = [](HRESULT hr) -> const Clamp* {
            log::warn("victory_gpu::indirect", "no argument clamp: indirect execution dropped", {{"error", log::hresult(hr)}});
            return nullptr;
        };
        Host* h = host();
        if (!h) {
            return refused(S_OK);
        }
        ComPtr<ID3DBlob> code, errors;
        if (HRESULT hr = D3DCompile(CLAMP_SOURCE, sizeof CLAMP_SOURCE - 1, "clamp", nullptr, nullptr, "main", "cs_5_1", 0, 0, &code, &errors); FAILED(hr)) {
            if (errors) {
                log::warn("victory_gpu::indirect",
                          "argument clamp: " + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
            }
            return refused(hr);
        }
        if (!code) {
            return refused(S_OK);
        }
        auto* c = new Clamp;
        ID3D12Device* d = h->device().device();
        if (HRESULT hr = d->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&c->root)); FAILED(hr)) {
            delete c;
            return refused(hr);
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = c->root.Get();
        desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        if (HRESULT hr = d->CreateComputePipelineState(&desc, IID_PPV_ARGS(&c->pso)); FAILED(hr)) {
            delete c;
            return refused(hr);
        }
        return c;
    }();
    return made;
}

std::optional<std::uint32_t> min_stride(std::uint32_t kind) {
    switch (kind) {
        case 0: return 16u;
        case 1: return 20u;
        case 2:
        case 10: return 12u;
        default: return std::nullopt;
    }
}

std::optional<std::uint64_t> validate_layout(Spec spec, std::uint64_t src_offset, std::optional<std::uint64_t> count_offset) {
    auto min = min_stride(spec.kind);
    if (!min || spec.stride < *min || spec.stride % 4 != 0 || src_offset % 4 != 0) {
        return std::nullopt;
    }
    if (count_offset && *count_offset % 4 != 0) {
        return std::nullopt;
    }
    std::uint64_t bytes = std::uint64_t{spec.stride} * spec.records;
    if (bytes > UINT32_MAX) {
        return std::nullopt;
    }
    return bytes;
}

bool clamp_scoped() {
    static const bool on = [] {
        bool scoped = danzi::env::not_set_or_not(L"VICTORY_CLAMP_SCOPED", L"0");
        if (!scoped) {
            log::info("victory_gpu::indirect", "indirect clamps wait on every resource (VICTORY_CLAMP_SCOPED=0)");
        }
        return scoped;
    }();
    return on;
}

struct Waits {
    bool before, after, scoped;
};

Waits clamp_waits(HostList& list, bool own) {
    bool scoped = own && clamp_scoped();
    if (!barrier_merge()) {
        return {true, !scoped, scoped};
    }
    return {list.uav_pending(), false, scoped};
}

void clamp_recorded(HostList& list, Waits w, bool recorded) {
    if (!barrier_merge() || !recorded) {
        return;
    }

    if (w.before && !w.scoped) {
        list.set_uav_pending(false);
    }
}

D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    return b;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return b;
}

constexpr std::uint32_t AHEAD = 2, IN_PARTS = 4;

bool record_clamp(const Clamp& c, ID3D12GraphicsCommandList* l, ID3D12Resource* dst, std::uint64_t budget, ID3D12Resource* src, std::uint64_t src_offset,
                  std::optional<std::pair<ID3D12Resource*, std::uint64_t>> count, Spec spec, std::uint32_t how, Waits w) {
    auto bytes = validate_layout(spec, src_offset, count ? std::optional(count->second) : std::nullopt);
    if (!bytes) {
        return false;
    }
    std::uint64_t src_base = src->GetGPUVirtualAddress(), dst_gpu = dst->GetGPUVirtualAddress();
    if (src_base % 4 != 0 || dst_gpu % 4 != 0 || budget % 4 != 0) {
        return false;
    }
    std::uint64_t src_gpu;
    std::uint32_t args_offset;
    if (src_offset + *bytes <= UINT32_MAX) {
        src_gpu = src_base;
        args_offset = static_cast<std::uint32_t>(src_offset);
    } else {
        src_gpu = src_base + src_offset;
        args_offset = 0;
    }
    std::uint64_t count_gpu = src_gpu;
    std::uint32_t has_count = 0, count_offset = 0;
    if (count) {
        std::uint64_t base = count->first->GetGPUVirtualAddress();
        if (base % 4 != 0) {
            return false;
        }
        has_count = 1;
        if (count->second + 4 <= UINT32_MAX) {
            count_gpu = base;
            count_offset = static_cast<std::uint32_t>(count->second);
        } else {
            count_gpu = base + count->second;
            count_offset = 0;
        }
    }
    const std::uint32_t constants[6] = {spec.kind, spec.stride, spec.records, args_offset, has_count | how, count_offset};
    D3D12_RESOURCE_BARRIER first[3];
    UINT first_len = 0;
    if (w.before) {
        if (w.scoped) {
            first[first_len++] = uav_barrier(src);
            if (count && count->first != src) {
                first[first_len++] = uav_barrier(count->first);
            }
        } else {
            first[first_len++] = uav_barrier(nullptr);
        }
    }
    first[first_len++] = transition(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER second[2] = {uav_barrier(nullptr), transition(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};
    UINT skip_second = w.after ? 0 : 1;
    l->ResourceBarrier(first_len, first);
    if (spec.records > 0) {
        l->SetComputeRootSignature(c.root.Get());
        l->SetPipelineState(c.pso.Get());
        l->SetComputeRoot32BitConstants(0, 6, constants, 0);
        l->SetComputeRootShaderResourceView(1, src_gpu);
        l->SetComputeRootShaderResourceView(2, count_gpu);
        l->SetComputeRootUnorderedAccessView(3, dst_gpu);
        l->SetComputeRootUnorderedAccessView(4, budget);
        l->Dispatch((spec.records + 63) / 64, 1, 1);
    }
    l->ResourceBarrier(2 - skip_second, second + skip_second);
    return true;
}

ID3D12Resource* bound_how(HostList& list, ID3D12Resource* src, std::uint64_t offset, std::optional<std::pair<ID3D12Resource*, std::uint64_t>> count,
                          Spec spec, bool own, std::uint32_t how, std::uint64_t scratch_bytes) {
    const Clamp* c = clamp();
    if (!c) {
        return nullptr;
    }
    auto budget = list.budget();
    if (!budget) {
        return nullptr;
    }
    ID3D12Resource* dst = list.indirect_scratch(scratch_bytes);
    if (!dst) {
        return nullptr;
    }
    Waits w = clamp_waits(list, own);
    bool recorded = record_clamp(*c, list.list(), dst, *budget, src, offset, count, spec, how, w);
    clamp_recorded(list, w, recorded);
    return recorded ? dst : nullptr;
}

}   

ID3D12Resource* bound(HostList& list, ID3D12Resource* src, std::uint64_t offset, std::optional<std::pair<ID3D12Resource*, std::uint64_t>> count,
                      Spec spec, bool own) {
    auto bytes = validate_layout(spec, offset, count ? std::optional(count->second) : std::nullopt);
    if (!bytes) {
        return nullptr;
    }
    return bound_how(list, src, offset, count, spec, own, 0, *bytes);
}

ID3D12Resource* bound_ahead(HostList& list, ID3D12Resource* src, std::uint64_t offset, Spec spec, bool own) {
    auto bytes = validate_layout(spec, offset, std::nullopt);
    if (!bytes) {
        return nullptr;
    }
    return bound_how(list, src, offset, std::nullopt, spec, own, AHEAD, *bytes);
}

ID3D12Resource* bound_split(HostList& list, ID3D12Resource* src, std::uint64_t offset, bool own) {
    Spec spec{2, 12, 1};
    if (!validate_layout(spec, offset, std::nullopt)) {
        return nullptr;
    }
    return bound_how(list, src, offset, std::nullopt, spec, own, IN_PARTS, SPLIT * PART_BYTES);
}

void indirect_done(HostList& list, ID3D12Resource* scratch) {
    D3D12_RESOURCE_BARRIER b = transition(scratch, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
    list.list()->ResourceBarrier(1, &b);
}

}   
