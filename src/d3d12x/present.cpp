// SPDX-License-Identifier: GPL-3.0-or-later
// Presentation (`host.rs` `present`, `presenter`, `title_window`, `window`;
// `over.rs`; DanziLoader / victory-perf, see NOTICE.md). See present.hpp.
#include "present.hpp"

#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/shadow.hpp"
#include "gpu/card.hpp"
#include "host.hpp"
#include "host_heap.hpp"
#include "pipeline.hpp"
#include "texture.hpp"

namespace danzi::d3d12x {

namespace {

using Microsoft::WRL::ComPtr;

std::optional<DXGI_FORMAT> swap_format(DXGI_FORMAT f) {
    auto v = static_cast<std::uint32_t>(f);
    if (v >= 27 && v <= 32) {
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (v == 87 || v == 90 || v == 91) {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    if (v >= 23 && v <= 25) {
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    if (v >= 9 && v <= 14) {
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    return std::nullopt;
}

constexpr UINT BUFFERS = 2;
constexpr const wchar_t* CAPTION = L"DanziLoader V 1.0 (C++)";


HWND title_window() {
    struct Found {
        HWND hwnd = nullptr;
    } found;
    EnumWindows(
        [](HWND hwnd, LPARAM at) -> BOOL {
            DWORD process = 0;
            GetWindowThreadProcessId(hwnd, &process);
            if (process == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
                reinterpret_cast<Found*>(at)->hwnd = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found.hwnd;
}

HWND make_window(UINT width, UINT height) {
    static const wchar_t* CLASS = L"VIctoryCpp";
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW c{};
    c.cbSize = sizeof c;
    c.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT { return DefWindowProcW(h, m, w, l); };
    c.hInstance = instance;
    c.lpszClassName = CLASS;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&c);
    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, CLASS, CAPTION, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                           nullptr, nullptr, instance, nullptr);
}


HWND window(UINT width, UINT height) {
    std::mutex lock;
    std::condition_variable made;
    HWND hwnd = nullptr;
    bool done = false;
    std::thread([&, width, height] {
        HWND h = make_window(width, height);
        {
            std::lock_guard g(lock);
            hwnd = h;
            done = true;
        }
        made.notify_one();
        if (h) {
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }).detach();
    std::unique_lock g(lock);
    made.wait(g, [&] { return done; });
    return hwnd;
}


void frame_lines(double frames, bool five) {
    constexpr const char* HOST = "victory_gpu::host";
    char b[2048];
    {
        auto [largest, groups] = take_mesh_groups();
        std::snprintf(b, sizeof b,
                      "indirect mesh dispatches: the largest group count asked for %llu (run: at most 65535), %.0f groups asked per frame",
                      static_cast<unsigned long long>(largest), static_cast<double>(groups) / frames);
        log::info(HOST, b);
    }
    batch_line(frames);
    shadow::back_line(frames);
    {
        auto c = gpu::take_card_passes();
        double passes = c[0] / frames, pieces = c[1] / frames, compares = c[2] / frames;
        if (passes + compares > 0.0) {
            std::snprintf(b, sizeof b,
                          "our passes over copies in the card's memory per frame: %.0f hash passes over %.0f pieces after ranges were sent up "
                          "(VICTORY_CARD_REHASH_RUNS=1 joins them: now %s), %.0f compare passes",
                          passes, pieces, gpu::rehash_runs() ? "on" : "off", compares);
            log::info(HOST, b);
        }
    }
    {
        auto [through, left] = take_unlocked();
        if (through + left > 0) {
            std::snprintf(b, sizeof b,
                          "texture refreshes that did not wait for title memory to hold still, per frame (VICTORY_TEXTURE_NO_WAIT=1): %.1f read "
                          "through the kernel, %.1f left for the next refresh (a page was not there)",
                          static_cast<double>(through) / frames, static_cast<double>(left) / frames);
            log::info(HOST, b);
        }
    }
    auto card = shadow::card_stats();
    if (!five || !log::enabled(log::Level::Info, HOST)) {
        return;
    }
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    auto slots = slot_counts();
    std::snprintf(b, sizeof b, "placed texture views: %llu writes of another view of the texture a slot showed (it kept the old view until the new was made)",
                  u(slots[0]));
    log::info(HOST, b);
    auto q = heap_texture_queue();
    std::snprintf(b, sizeof b,
                  "textures picked by index, the queue: %llu slots, %llu of them due (the oldest written %llu frames ago), %llu waiting for their "
                  "memory, %llu uploaded in part; since the start: %llu slots found uploaded already by the draws that read them through tables, "
                  "%llu writes of tables' slots (due some frames later), %llu slots taken out as read through a table, %llu uploads of textures "
                  "not all mapped (%llu of them without their first page), %llu looks that found no more mapped; %llu slots made stale by a newer "
                  "view of other memory there, %llu ranges of memory mapped, unmapped or written under texture views (%llu dropped), %llu slots "
                  "made due by them, %llu made slots found unmapped and made null",
                  u(q[0]), u(q[1]), u(q[4]), u(q[2]), u(q[3]), u(q[8]), u(slots[1]), u(slots[2]), u(q[6]), u(q[7]), u(q[5]), u(slots[3]),
                  u(slots[4]), u(slots[5]), u(slots[6]), u(slots[7]));
    log::info(HOST, b);
    if (shadow::cards()) {
        auto per_frame = [&](int i) { return static_cast<double>(card[i]) / frames; };
        std::snprintf(b, sizeof b,
                      "host copies in the card's memory: %llu copies %llu MB, their bytes from before %llu MB; per frame: %.1f MB sent up in %.0f "
                      "ranges, %.0f pieces compared on the card, %.0f of them fetched (%.1f MB) with %.1f more waits for the card (%.1f ms)",
                      u(card[7]), u(card[8] >> 20), u(card[9] >> 20), per_frame(0) / 1e6, per_frame(1), per_frame(2), per_frame(3),
                      per_frame(4) / 1e6, per_frame(5), per_frame(6) / 1e3);
        log::info(HOST, b);
    }
}

struct Meter {
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    std::uint32_t frames = 0;
    std::uint64_t logged_frames = 0;
    std::chrono::steady_clock::time_point logged = since;

    void frame(HWND hwnd) {
        ++frames;
        ++logged_frames;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - since).count();
        if (elapsed < 1.0) {
            return;
        }
        double fps = frames / elapsed;
        bool five = now - logged >= std::chrono::seconds(5);
        if (five) {
            logged = now;
        }
        frame_lines(static_cast<double>(frames), five);
        if (five) {
            memcopy_line(static_cast<double>(std::max<std::uint64_t>(std::exchange(logged_frames, 0), 1)));
        }
        since = now;
        frames = 0;
        wchar_t text[128];
        swprintf(text, 128, L"%s - %.1f fps (%.2f ms)", CAPTION, fps, 1000.0 / fps);
        static std::mutex pending_lock;
        static std::optional<std::pair<HWND, std::wstring>> pending;
        static std::once_flag setter;
        {
            std::lock_guard g(pending_lock);
            pending = std::pair{hwnd, std::wstring(text)};
        }
        std::call_once(setter, [] {
            std::thread([] {
                for (;;) {
                    std::optional<std::pair<HWND, std::wstring>> next;
                    {
                        std::lock_guard g(pending_lock);
                        next.swap(pending);
                    }
                    if (next) {
                        SetWindowTextW(next->first, next->second.c_str());
                    } else {
                        Sleep(100);
                    }
                }
            }).detach();
        });
    }
};


constexpr char OVER_SOURCE[] = R"HLSL(
Texture2D<float4> plane : register(t0);
SamplerState bilinear : register(s0);
struct Corner { float4 position : SV_Position; float2 at : TEXCOORD0; };
Corner corner(uint i : SV_VertexID) {
    Corner c;
    c.at = float2(float(i & 1) * 2.0, float(i >> 1) * 2.0);
    c.position = float4(c.at.x * 2.0 - 1.0, 1.0 - c.at.y * 2.0, 0.0, 1.0);
    return c;
}
float4 over(Corner c) : SV_Target { return plane.Sample(bilinear, c.at); }
)HLSL";

constexpr UINT RING = 16;

struct Pass {
    DXGI_FORMAT format;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> targets, views;
    UINT target_step = 0, view_step = 0, next = 0;
};


std::unique_ptr<Pass> make_pass(ID3D12Device* device, DXGI_FORMAT format, HRESULT& hr) {
    ComPtr<ID3DBlob> vs, ps, err;

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& code) {
        hr = D3DCompile(OVER_SOURCE, sizeof OVER_SOURCE - 1, "over", nullptr, nullptr, entry, target, 0, 0, &code, &err);
        if (FAILED(hr) && err) {
            log::warn("victory_gpu::host", "a shader of ours: " + std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()));
        }
        return SUCCEEDED(hr);
    };
    if (!compile("corner", "vs_5_1", vs) || !compile("over", "ps_5_1", ps)) {
        return nullptr;
    }
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable = {1, &range};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{1, &param, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    auto pass = std::make_unique<Pass>();
    if (FAILED(hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&pass->root)))) {
        return nullptr;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = pass->root.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc = {1, 0};
    auto& b = desc.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_ONE;
    b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pass->pipeline)))) {
        return nullptr;
    }
    D3D12_DESCRIPTOR_HEAP_DESC td{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RING, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC vd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, RING, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(hr = device->CreateDescriptorHeap(&td, IID_PPV_ARGS(&pass->targets))) || FAILED(hr = device->CreateDescriptorHeap(&vd, IID_PPV_ARGS(&pass->views)))) {
        return nullptr;
    }
    pass->format = format;
    pass->target_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    pass->view_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return pass;
}

std::mutex PASS_LOCK;
std::unique_ptr<Pass> PASS;

bool over_record(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* back, DXGI_FORMAT format, UINT width, UINT height,
                 ID3D12Resource* plane, DXGI_FORMAT plane_format) {
    std::lock_guard lock(PASS_LOCK);
    if (!PASS || PASS->format != format) {
        HRESULT hr = S_OK;
        PASS = make_pass(device, format, hr);
        if (!PASS) {
            static std::once_flag told;
            std::call_once(told, [&] { log::warn("victory_gpu::over", "no pass to draw a present's second plane: it is not shown", {{"error", log::hresult(hr)}}); });
            return false;
        }
        log::info("victory_gpu::over", "a present's second plane is drawn over the first", {{"format", log::debug(static_cast<std::int64_t>(format))}});
    }
    Pass& p = *PASS;
    UINT slot = p.next % RING;
    p.next += 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = plane_format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    D3D12_RENDER_TARGET_VIEW_DESC target_desc{};
    target_desc.Format = format;
    target_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE target{p.targets->GetCPUDescriptorHandleForHeapStart().ptr + std::size_t{slot} * p.target_step};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{p.views->GetCPUDescriptorHandleForHeapStart().ptr + std::size_t{slot} * p.view_step};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{p.views->GetGPUDescriptorHandleForHeapStart().ptr + std::uint64_t{slot} * p.view_step};
    device->CreateRenderTargetView(back, &target_desc, target);
    device->CreateShaderResourceView(plane, &view, cpu);
    ID3D12DescriptorHeap* heaps[] = {p.views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(p.root.Get());
    list->SetPipelineState(p.pipeline.Get());
    list->SetGraphicsRootDescriptorTable(0, gpu);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    D3D12_RECT sc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    return true;
}


struct Presenter {
    ComPtr<IDXGISwapChain3> swap_chain;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::unique_ptr<HostList> copy;


    std::vector<std::pair<std::unique_ptr<HostList>, std::uint64_t>> ahead;
    HWND hwnd = nullptr;
    Meter meter;
};

std::mutex PRESENTER_LOCK;
std::unique_ptr<Presenter> PRESENTER;
std::atomic<HWND> WINDOW{nullptr};

std::unique_ptr<Presenter> make_presenter(Host& h, UINT width, UINT height, DXGI_FORMAT format, HRESULT& error) {
    HWND hwnd = title_window();
    if (!hwnd) {
        hwnd = window(width, height);
    }
    if (!hwnd) {
        error = E_FAIL;
        return nullptr;
    }
    h.device().log_video_memory("right before the swap chain is made");
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(error = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.SampleDesc = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = BUFFERS;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    if (FAILED(error = factory->CreateSwapChainForHwnd(h.device().queue(), hwnd, &desc, nullptr, nullptr, &chain))) {
        return nullptr;
    }
    auto p = std::make_unique<Presenter>();
    if (FAILED(error = chain.As(&p->swap_chain))) {
        return nullptr;
    }
    p->copy = h.new_list();
    if (!p->copy) {
        error = E_FAIL;
        return nullptr;
    }
    p->copy->close();
    log::info("victory_gpu::host", "presenting to a window",
              {{"width", log::debug(width)}, {"height", log::debug(height)}, {"format", log::debug(static_cast<int>(format))}});
    h.device().log_video_memory("right after the swap chain is made");
    p->width = width;
    p->height = height;
    p->format = format;
    p->hwnd = hwnd;
    WINDOW.store(hwnd);
    return p;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return b;
}

}   

void* present_window() { return WINDOW.load(); }

bool present_texture(ID3D12Resource* texture, std::uint32_t state_bits, std::uint64_t frame, std::optional<std::pair<ID3D12Resource*, std::uint32_t>> over) {
    Host* h = host();
    if (!h) {
        return false;
    }
    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    auto format = swap_format(desc.Format);
    if (!format) {
        log::warn("victory_gpu::host", "cannot present this format", {{"format", log::debug(static_cast<int>(desc.Format))}});
        return false;
    }
    auto width = static_cast<UINT>(desc.Width), height = desc.Height;
    std::lock_guard lock(PRESENTER_LOCK);
    if (!PRESENTER) {
        HRESULT error = S_OK;
        PRESENTER = make_presenter(*h, width, height, *format, error);
        if (!PRESENTER) {
            log::warn("victory_gpu::host", "no swap chain", {{"error", log::hresult(error)}});
            return false;
        }
    }
    Presenter& p = *PRESENTER;
    if (p.width != width || p.height != height || p.format != *format) {

        settle_for_present();
        if (HRESULT hr = p.swap_chain->ResizeBuffers(BUFFERS, width, height, *format, 0); FAILED(hr)) {
            log::warn("victory_gpu::host", "swap chain not resized",
                      {{"error", log::hresult(hr)}, {"width", log::debug(width)}, {"height", log::debug(height)}});
            return false;
        }
        p.width = width;
        p.height = height;
        p.format = *format;
        log::info("victory_gpu::host", "the swap chain's buffers were made again for another size or format",
                  {{"width", log::debug(width)}, {"height", log::debug(height)}, {"format", log::debug(static_cast<int>(*format))},
                   {"frame", log::debug(frame)}});
        h->device().log_video_memory("right after the swap chain's buffers were made again");
    }
    auto state = static_cast<D3D12_RESOURCE_STATES>(state_bits);
    UINT index = p.swap_chain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> back;
    if (FAILED(p.swap_chain->GetBuffer(index, IID_PPV_ARGS(&back)))) {
        return false;
    }

    std::optional<std::size_t> slot;
    if (late_free_present()) {
        std::uint64_t finished = Host::finished();
        for (std::size_t i = 0; i < p.ahead.size(); ++i) {
            if (p.ahead[i].second <= finished) {
                slot = i;
                break;
            }
        }
        if (!slot) {
            if (auto list = h->new_list()) {
                list->close();
                p.ahead.emplace_back(std::move(list), 0);
                slot = p.ahead.size() - 1;
            }
        }
    }
    HostList& copy = slot ? *p.ahead[*slot].first : *p.copy;
    copy.reset();
    ID3D12GraphicsCommandList* l = copy.list();
    D3D12_RESOURCE_BARRIER first[2] = {transition(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                       transition(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
    l->ResourceBarrier(2, first);
    l->CopyResource(back.Get(), texture);
    std::optional<std::pair<ID3D12Resource*, std::pair<D3D12_RESOURCE_STATES, DXGI_FORMAT>>> plane;
    if (over) {
        if (auto f = swap_format(over->first->GetDesc().Format)) {
            plane = std::pair{over->first, std::pair{static_cast<D3D12_RESOURCE_STATES>(over->second), *f}};
        }
    }
    constexpr auto READABLE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (plane) {
        auto [r, sf] = *plane;
        auto [was, plane_format] = sf;
        D3D12_RESOURCE_BARRIER b[3] = {transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state),
                                       transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        UINT n = 2;
        bool moved = (was & READABLE) == 0;
        if (moved) {
            b[n++] = transition(r, was, READABLE);
        }
        l->ResourceBarrier(n, b);
        over_record(h->device().device(), l, back.Get(), *format, width, height, r, plane_format);
        D3D12_RESOURCE_BARRIER a[2] = {transition(back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT)};
        n = 1;
        if (moved) {
            a[n++] = transition(r, READABLE, was);
        }
        l->ResourceBarrier(n, a);
    } else {
        D3D12_RESOURCE_BARRIER b[2] = {transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state),
                                       transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)};
        l->ResourceBarrier(2, b);
    }
    copy.close();
    if (slot) {
        p.ahead[*slot].second = h->execute_own(copy);
    } else {
        HostList* runs[1] = {&copy};
        h->execute(runs);
    }
    p.swap_chain->Present(0, 0);
    p.meter.frame(p.hwnd);
    return true;
}

}   
