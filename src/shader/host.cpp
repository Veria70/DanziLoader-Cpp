// SPDX-License-Identifier: GPL-3.0-or-later
// Host D3D12 checks for emitted shaders (`crates/victory-shader/src/emit/
// host.rs`, its non-test part; DanziLoader / victory-perf, see NOTICE.md).
// See host.hpp.
#include "host.hpp"

#include <dxgi1_6.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "dxbc.hpp"
#include "dxil.hpp"
#include "error.hpp"

namespace danzi::shader::emit::host {

using container::SigElement;
using Microsoft::WRL::ComPtr;

namespace {

std::string utf8(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}


std::string err(HRESULT code) {
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD id = static_cast<DWORD>(code);
    HMODULE module = nullptr;
    if ((id & 0x1000'0000u) != 0) {
        id ^= 0x1000'0000u;
        flags |= FORMAT_MESSAGE_FROM_HMODULE;
        module = LoadLibraryExA("ntdll.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    }
    wchar_t* text = nullptr;
    DWORD n = FormatMessageW(flags, module, id, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message;
    if (text) {
        message.assign(text, n);
        LocalFree(text);
    }
    while (!message.empty() && (message.back() == L' ' || (message.back() >= 9 && message.back() <= 13))) {
        message.pop_back();
    }
    return utf8(message);
}


std::string from_utf8_lossy(std::span<const std::uint8_t> b) {
    std::string out;
    std::size_t i = 0;
    while (i < b.size()) {
        std::uint8_t c = b[i];
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        std::size_t need = 0;
        std::uint8_t lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            lo = c == 0xE0 ? 0xA0 : 0x80;
            hi = c == 0xED ? 0x9F : 0xBF;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            lo = c == 0xF0 ? 0x90 : 0x80;
            hi = c == 0xF4 ? 0x8F : 0xBF;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        std::size_t k = 1;
        for (; k <= need && i + k < b.size(); ++k) {
            std::uint8_t x = b[i + k];
            if (x < (k == 1 ? lo : 0x80) || x > (k == 1 ? hi : 0xBF)) {
                break;
            }
        }
        if (k == need + 1) {
            out.append(reinterpret_cast<const char*>(b.data() + i), need + 1);
            i += need + 1;
        } else {
            out += "\xEF\xBF\xBD";
            i += k;
        }
    }
    return out;
}

bool env_is(const wchar_t* name, std::wstring_view value) {
    wchar_t buf[64];
    DWORD n = GetEnvironmentVariableW(name, buf, 64);
    return n > 0 && n < 64 && std::wstring_view(buf, n) == value;
}

bool env_set(const wchar_t* name) {
    SetLastError(0);
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    return n > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}


std::optional<std::vector<SigElement>> sig(std::span<const std::uint8_t> dxil, std::uint32_t code, std::string& error) {
    try {
        Container c = Container::parse(dxil);
        if (const ContainerPart* p = c.part(code)) {
            return part_signature(p->data);
        }
        return std::vector<SigElement>{};
    } catch (const ShaderError& e) {
        error = e.display();
        return std::nullopt;
    }
}

DXGI_FORMAT format(const SigElement& e) {
    std::size_t i = static_cast<std::size_t>(std::clamp<std::uint8_t>(e.cols, 1, 4)) - 1;
    static const DXGI_FORMAT F[4] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    static const DXGI_FORMAT S[4] = {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT};
    static const DXGI_FORMAT U[4] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT};
    switch (e.comp) {
        case 9: return F[i];
        case 4: return S[i];
        default: return U[i];
    }
}

DXGI_FORMAT target_format(const SigElement& e) {
    switch (e.comp) {
        case 9: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case 4: return DXGI_FORMAT_R32G32B32A32_SINT;
        default: return DXGI_FORMAT_R32G32B32A32_UINT;
    }
}


std::optional<std::string> root_signature_for(const Host& h, std::span<const std::pair<const HostShader*, D3D12_SHADER_VISIBILITY>> stages,
                                              ComPtr<ID3D12RootSignature>& out) {
    std::vector<D3D12_ROOT_PARAMETER1> params;
    for (const auto& [s, vis] : stages) {
        D3D12_ROOT_PARAMETER1 p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants.ShaderRegister = 0;
        p.Constants.RegisterSpace = dxil::USER_DATA_SPACE;
        p.Constants.Num32BitValues = std::max<std::uint32_t>(s->user_data_dwords(), 1);
        p.ShaderVisibility = vis;
        params.push_back(p);
    }

    std::vector<std::pair<std::uint8_t, std::uint8_t>> views;
    for (const auto& [s, vis] : stages) {
        for (auto r : s->root_cbvs) {
            views.emplace_back(r, std::uint8_t{0});
        }
        for (auto r : s->root_srvs) {
            views.emplace_back(r, std::uint8_t{1});
        }
        for (auto r : s->root_uavs) {
            views.emplace_back(r, std::uint8_t{2});
        }
    }
    std::sort(views.begin(), views.end());
    views.erase(std::unique(views.begin(), views.end()), views.end());
    for (const auto& [slot, kind] : views) {
        D3D12_ROOT_PARAMETER1 p{};
        p.ParameterType = kind == 1 ? D3D12_ROOT_PARAMETER_TYPE_SRV : kind == 2 ? D3D12_ROOT_PARAMETER_TYPE_UAV : D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = slot;
        p.Descriptor.RegisterSpace = dxil::ROOT_CBV_SPACE;
        p.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    for (const auto& [s, vis] : stages) {
        for (std::size_t n = 0; n < s->static_samplers.size(); ++n) {
            const StaticSampler& d = s->static_samplers[n];
            D3D12_STATIC_SAMPLER_DESC x{};
            x.Filter = static_cast<D3D12_FILTER>(d.filter);
            x.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.address[0]);
            x.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.address[1]);
            x.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.address[2]);
            x.MipLODBias = d.mip_lod_bias;
            x.MaxAnisotropy = d.max_anisotropy;
            x.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(d.comparison);
            x.BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(d.border);
            x.MinLOD = d.min_lod;
            x.MaxLOD = d.max_lod;
            x.ShaderRegister = static_cast<UINT>(n);
            x.RegisterSpace = dxil::SAMPLER_SPACE;
            x.ShaderVisibility = vis;
            samplers.push_back(x);
        }
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
    desc.Desc_1_1.pParameters = params.data();
    desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    desc.Desc_1_1.pStaticSamplers = samplers.data();
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                          D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
    if (FAILED(hr)) {
        return err(hr);
    }
    if (!blob) {
        return std::string("no root signature blob");
    }
    hr = h.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
    if (FAILED(hr)) {
        return err(hr);
    }
    return std::nullopt;
}


std::string explained(const Host& h, HRESULT e) {
    std::string text = err(e);
    ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(h.device.As(&queue))) {
        return text;
    }
    UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T len = 0;
        queue->GetMessage(i, nullptr, &len);
        std::vector<std::uint64_t> buffer((len + 7) / 8);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (SUCCEEDED(queue->GetMessage(i, message, &len))) {
            SIZE_T n = message->DescriptionByteLength > 0 ? message->DescriptionByteLength - 1 : 0;
            text += " | ";
            text += from_utf8_lossy(std::span(reinterpret_cast<const std::uint8_t*>(message->pDescription), n));
        }
    }
    queue->ClearStoredMessages();
    return text;
}

D3D12_RASTERIZER_DESC rasterizer() {
    D3D12_RASTERIZER_DESC r{};
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = D3D12_CULL_MODE_NONE;
    r.DepthClipEnable = TRUE;
    return r;
}


std::optional<std::string> pipeline_with_gs(const Host& h, ID3D12RootSignature* root, std::span<const std::uint8_t> vs,
                                            std::optional<std::span<const std::uint8_t>> gs, std::span<const std::uint8_t> ps) {
    std::string error;
    auto inputs = sig(vs, fourcc("ISG1"), error);
    if (!inputs) {
        return error;
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    for (const auto& e : *inputs) {
        if (e.kind != 0) {
            continue;
        }
        D3D12_INPUT_ELEMENT_DESC d{};
        d.SemanticName = e.name.c_str();
        d.SemanticIndex = e.index;
        d.Format = format(e);
        d.InputSlot = 0;
        d.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
        d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        d.InstanceDataStepRate = 0;
        layout.push_back(d);
    }
    auto outputs = sig(ps, fourcc("OSG1"), error);
    if (!outputs) {
        return error;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    if (gs) {
        desc.GS = {gs->data(), gs->size()};
    }
    desc.SampleMask = UINT32_MAX;
    desc.RasterizerState = rasterizer();
    desc.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc = {1, 0};
    for (auto& rt : desc.BlendState.RenderTarget) {
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    for (const auto& o : *outputs) {
        if (o.kind != 16) {
            continue;
        }
        std::size_t n = o.row.value_or(0);
        desc.RTVFormats[n] = target_format(o);
        desc.NumRenderTargets = std::max<UINT>(desc.NumRenderTargets, static_cast<UINT>(n) + 1);
    }
    if (std::any_of(outputs->begin(), outputs->end(), [](const SigElement& o) { return o.kind >= 17 && o.kind <= 19; })) {
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    if (std::any_of(outputs->begin(), outputs->end(), [](const SigElement& o) { return o.kind == 20; })) {
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        desc.DepthStencilState.StencilEnable = TRUE;
    }
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = h.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        return err(hr);
    }
    return std::nullopt;
}


#pragma warning(push)
#pragma warning(disable : 4324)  // (padded: as meant)
template <class T>
struct alignas(8) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;
};
#pragma warning(pop)

struct Stream {
    Sub<void*> root;
    Sub<D3D12_SHADER_BYTECODE> ms;
    Sub<D3D12_SHADER_BYTECODE> ps;
    Sub<D3D12_BLEND_DESC> blend;
    Sub<std::uint32_t> mask;
    Sub<D3D12_RASTERIZER_DESC> raster;
    Sub<D3D12_DEPTH_STENCIL_DESC> depth;
    Sub<D3D12_RT_FORMAT_ARRAY> targets;
    Sub<DXGI_FORMAT> dsv;
    Sub<DXGI_SAMPLE_DESC> samples;
};


std::optional<std::string> mesh_pipeline_with(const Host& h, ID3D12RootSignature* root, std::span<const std::uint8_t> ms,
                                              std::span<const std::uint8_t> ps) {
    std::string error;
    auto outputs = sig(ps, fourcc("OSG1"), error);
    if (!outputs) {
        return error;
    }
    D3D12_BLEND_DESC blend{};
    for (auto& rt : blend.RenderTarget) {
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    D3D12_RT_FORMAT_ARRAY targets{};
    for (const auto& o : *outputs) {
        if (o.kind != 16) {
            continue;
        }
        std::size_t n = o.row.value_or(0);
        targets.RTFormats[n] = target_format(o);
        targets.NumRenderTargets = std::max<UINT>(targets.NumRenderTargets, static_cast<UINT>(n) + 1);
    }
    D3D12_DEPTH_STENCIL_DESC depth{};
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    if (std::any_of(outputs->begin(), outputs->end(), [](const SigElement& o) { return o.kind >= 17 && o.kind <= 19; })) {
        dsv = DXGI_FORMAT_D32_FLOAT;
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    if (std::any_of(outputs->begin(), outputs->end(), [](const SigElement& o) { return o.kind == 20; })) {
        dsv = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        depth.StencilEnable = TRUE;
    }
    Stream stream{};
    stream.root = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root};
    stream.ms = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, {ms.data(), ms.size()}};
    stream.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {ps.data(), ps.size()}};
    stream.blend = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend};
    stream.mask = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT32_MAX};
    stream.raster = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, rasterizer()};
    stream.depth = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, depth};
    stream.targets = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, targets};
    stream.dsv = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, dsv};
    stream.samples = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, {1, 0}};
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(Stream), &stream};
    ComPtr<ID3D12Device2> device;
    HRESULT hr = h.device.As(&device);
    if (FAILED(hr)) {
        return err(hr);
    }
    ComPtr<ID3D12PipelineState> pso;
    hr = device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        return err(hr);
    }
    return std::nullopt;
}

HostShader stub(bool pixel, std::vector<SigElement> outputs) {
    using bitcode::Attr;
    using bitcode::Term;
    using bitcode::V;
    dxil::Shader s(pixel);
    std::size_t b = s.entry;
    for (std::size_t id = 0; id < outputs.size(); ++id) {
        const SigElement& e = outputs[id];
        for (std::uint8_t col = 0; col < e.cols; ++col) {
            const char* name = e.comp == 9 ? "storeOutput.f32" : "storeOutput.i32";
            V zero = e.comp == 9 ? s.m.f32(0) : s.m.i32(0);
            V a0 = s.m.i32(static_cast<std::uint32_t>(id));
            V a1 = s.m.i32(0);
            V a2 = s.m.i8(col);
            s.op(b, name, Attr::NoUnwind, s.t.void_, 5, {a0, a1, a2, zero});
        }
    }
    for (auto& e : outputs) {
        e.usage = static_cast<std::uint8_t>((1u << e.cols) - 1);
    }
    s.outputs = std::move(outputs);
    s.m.terminate(b, Term::ret());
    HostShader h;
    h.stage = pixel ? HwStage::Ps : HwStage::Vs;
    h.dxil = s.finish();
    return h;
}

}   

std::optional<Host> make_host(bool warp, std::string& error) {
    warp = warp || env_is(L"VICTORY_WARP", L"1");


    if (env_set(L"VICTORY_D3D12_DEBUG")) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) {
            debug->EnableDebugLayer();
        }
    }
    ComPtr<ID3D12Device> device;
    HRESULT hr;
    if (warp) {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
            error = err(hr);
            return std::nullopt;
        }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
            error = err(hr);
            return std::nullopt;
        }
        if (FAILED(hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            error = err(hr);
            return std::nullopt;
        }
    } else {
        if (FAILED(hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            error = err(hr);
            return std::nullopt;
        }


        if (device) {
            ComPtr<IDXGIFactory4> factory;
            if (FAILED(hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
                error = err(hr);
                return std::nullopt;
            }
            ComPtr<IDXGIAdapter1> adapter;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) && SUCCEEDED(adapter->GetDesc1(&desc)) &&
                ((desc.Flags & 2) != 0 || (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c))) {
                error = "the default device is the software adapter";
                return std::nullopt;
            }
        }
    }
    if (!device) {
        error = "no device";
        return std::nullopt;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    if (FAILED(hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) {
        error = err(hr);
        return std::nullopt;
    }
    if (sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
        char text[64];
        std::snprintf(text, sizeof text, "the device supports SM 0x%x only", static_cast<unsigned>(sm.HighestShaderModel));
        error = text;
        return std::nullopt;
    }
    return Host{std::move(device)};
}

std::optional<std::string> compute_pipeline(const Host& h, const HostShader& cs) {
    std::pair<const HostShader*, D3D12_SHADER_VISIBILITY> stages[] = {{&cs, D3D12_SHADER_VISIBILITY_ALL}};
    ComPtr<ID3D12RootSignature> root;
    if (auto e = root_signature_for(h, stages, root)) {
        return e;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.CS = {cs.dxil.data(), cs.dxil.size()};
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = h.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        return explained(h, hr);
    }
    return std::nullopt;
}

std::optional<std::string> pipeline(const Host& h, const HostShader& vs, const HostShader& ps) {
    std::pair<const HostShader*, D3D12_SHADER_VISIBILITY> stages[] = {{&vs, D3D12_SHADER_VISIBILITY_VERTEX}, {&ps, D3D12_SHADER_VISIBILITY_PIXEL}};
    ComPtr<ID3D12RootSignature> root;
    if (auto e = root_signature_for(h, stages, root)) {
        return e;
    }
    return pipeline_with_gs(h, root.Get(), vs.dxil, std::nullopt, ps.dxil);
}

std::optional<std::string> mesh_pipeline(const Host& h, const HostShader& ms, const HostShader& ps) {
    std::pair<const HostShader*, D3D12_SHADER_VISIBILITY> stages[] = {{&ms, D3D12_SHADER_VISIBILITY_MESH}, {&ps, D3D12_SHADER_VISIBILITY_PIXEL}};
    ComPtr<ID3D12RootSignature> root;
    if (auto e = root_signature_for(h, stages, root)) {
        return e;
    }
    return mesh_pipeline_with(h, root.Get(), ms.dxil, ps.dxil);
}

std::optional<HostShader> stub_vs(const HostShader& ps, std::string& error) {
    auto inputs = sig(ps.dxil, fourcc("ISG1"), error);
    if (!inputs) {
        return std::nullopt;
    }
    std::vector<SigElement> outputs;
    for (auto& e : *inputs) {
        if (e.kind == 0 || e.kind == 3 || e.kind == 6 || e.kind == 7) {
            outputs.push_back(std::move(e));
        }
    }
    if (std::none_of(outputs.begin(), outputs.end(), [](const SigElement& e) { return e.kind == 3; })) {
        std::uint32_t row = 0;
        for (const auto& e : outputs) {
            if (e.row) {
                row = std::max(row, *e.row + 1);
            }
        }
        SigElement pos;
        pos.name = "SV_Position";
        pos.index = 0;
        pos.kind = 3;
        pos.comp = 9;
        pos.interp = 4;
        pos.row = row;
        pos.col = 0;
        pos.cols = 4;
        pos.usage = 0;
        pos.half = false;
        outputs.push_back(std::move(pos));
    }
    return stub(false, std::move(outputs));
}

HostShader stub_ps() {
    SigElement target;
    target.name = "SV_Target";
    target.index = 0;
    target.kind = 16;
    target.comp = 9;
    target.interp = 0;
    target.row = 0;
    target.col = 0;
    target.cols = 4;
    target.usage = 0;
    target.half = false;
    return stub(true, {target});
}

}   
