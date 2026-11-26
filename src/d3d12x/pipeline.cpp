// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/pipeline.rs and pipeline/{host,make,
// library}.rs (DanziLoader / victory-perf, see NOTICE.md). See pipeline.hpp.
#include "pipeline.hpp"

#include <blake3.h>
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <thread>
#include <unordered_set>
#include <unordered_map>

#include "core/env.hpp"
#include "core/log.hpp"
#include "shader/dxbc.hpp"
#include "shader/emit.hpp"
#include "shader/error.hpp"
#include "host.hpp"
#include "host_heap.hpp"
#include "resource.hpp"
#include "resource_iids.hpp"
#include "sync.hpp"

namespace danzi::d3d12x {

namespace {

const std::array<Iid, 4> PIPELINE_IIDS = {
    guid(0x765a30f3, 0xf624, 0x4c6f, {0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45}),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};

std::string hex(const std::uint8_t* b, std::size_t n) {
    static constexpr char H[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(H[b[i] >> 4]);
        s.push_back(H[b[i] & 15]);
    }
    return s;
}


std::mutex SHADERS_LOCK;
std::unordered_map<std::string, std::shared_ptr<Shader>> SHADERS;

std::shared_ptr<Shader> shader_of(const char* stage, const std::uint8_t* bytes, std::size_t size) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, bytes, size);
    std::uint8_t full[32];
    blake3_hasher_finalize(&h, full, 32);
    std::string key(reinterpret_cast<const char*>(full), 32);
    std::lock_guard lock(SHADERS_LOCK);
    auto& s = SHADERS[key];
    if (!s) {
        s = std::make_shared<Shader>();
        s->stage = stage;
        s->hash = hex(full, 8);
        s->container.assign(bytes, bytes + size);
    }
    return s;
}

struct Bytecode {
    const std::uint8_t* code;
    std::size_t size;
};

std::shared_ptr<Shader> copy(const char* stage, const Bytecode& code) {
    if (!code.code || code.size == 0) {
        return nullptr;
    }
    return shader_of(stage, code.code, code.size);
}


std::atomic<std::uint64_t> MADE{0}, LOADED{0}, EARLY_STARTED{0};
std::mutex DIR_LOCK;
std::optional<std::filesystem::path> DIR;

std::optional<std::filesystem::path> pipeline_dir() {
    std::lock_guard lock(DIR_LOCK);
    return DIR;
}

constexpr const wchar_t* LIBRARY_FILE = L"host-pipelines.bin";
constexpr const wchar_t* NEXT_FILE = L"host-pipelines.next";
constexpr const wchar_t* EARLY_FILE = L"early-pipelines.txt";
constexpr std::size_t EARLY_MAX = 8192;


struct Mapped {
    void* at = nullptr;
    std::size_t len = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    ~Mapped() {
        if (at) {
            UnmapViewOfFile(at);
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }
    static std::unique_ptr<Mapped> map(HANDLE file, std::size_t len, bool write) {
        if (len == 0) {
            return nullptr;
        }
        HANDLE mapping = CreateFileMappingW(file, nullptr, write ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) {
            return nullptr;
        }
        void* at = MapViewOfFile(mapping, write ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, 0);
        CloseHandle(mapping);
        if (!at) {
            return nullptr;
        }
        auto m = std::make_unique<Mapped>();
        m->at = at;
        m->len = len;
        return m;
    }
    static std::unique_ptr<Mapped> open(const std::filesystem::path& path) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            return nullptr;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f, &size) || size.QuadPart == 0) {
            CloseHandle(f);
            return nullptr;
        }
        auto m = map(f, static_cast<std::size_t>(size.QuadPart), false);
        if (!m) {
            CloseHandle(f);
            return nullptr;
        }
        m->file = f;
        return m;
    }
    static std::unique_ptr<Mapped> create(const std::filesystem::path& path, std::size_t len) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            return nullptr;
        }
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>(len);
        if (!SetFilePointerEx(f, at, nullptr, FILE_BEGIN) || !SetEndOfFile(f)) {
            CloseHandle(f);
            return nullptr;
        }
        auto m = map(f, len, true);
        CloseHandle(f);
        return m;
    }
};


std::atomic<std::uintptr_t> VIEW_AT{0};
std::atomic<std::size_t> VIEW_LEN{0};
std::atomic<std::uint64_t> LOADED_AT{0}, TRIMMED_BYTES{0};
std::atomic<bool> TRIMMED{false};


std::atomic<std::uint64_t> TRIMMED_AT{0};
std::atomic<std::uint64_t> REPLACED[2]{};


struct RunningCall {
    std::uint32_t thread;
    const char* what;
    std::string name;
    std::uint64_t since;
};
std::mutex CALLS_LOCK;
std::vector<RunningCall> CALLS;

thread_local std::string LABEL;


struct LibraryCall {
    std::uint32_t thread = thread_id();
    std::uint64_t since = now_us();
    LibraryCall(const char* what, const std::string& name) {
        std::lock_guard lock(CALLS_LOCK);
        CALLS.push_back({thread, what, name + " " + LABEL, since});
    }
    LibraryCall(const LibraryCall&) = delete;
    LibraryCall& operator=(const LibraryCall&) = delete;
    ~LibraryCall() {
        std::lock_guard lock(CALLS_LOCK);
        std::erase_if(CALLS, [&](const RunningCall& c) { return c.thread == thread && c.since == since; });
    }
};

struct Library {
    ComPtr<ID3D12PipelineLibrary1> library;
    std::unique_ptr<Mapped> blob;
    std::atomic<std::uint64_t> added{0};
    std::shared_mutex saving;
};


std::string os_error(DWORD code) {
    return std::system_category().message(static_cast<int>(code)) + " (os error " + std::to_string(code) + ")";
}

Library* library() {
    static Library* const made = []() -> Library* {
        Host* h = host();
        if (!h) {
            return nullptr;
        }
        ComPtr<ID3D12Device1> d1;
        if (FAILED(h->device().device()->QueryInterface(IID_PPV_ARGS(&d1)))) {
            return nullptr;
        }
        auto* l = new Library;
        if (auto dir = pipeline_dir()) {
            auto path = *dir / LIBRARY_FILE;
            auto next = *dir / NEXT_FILE;
            std::error_code ec;
            if (std::filesystem::exists(next, ec) && !MoveFileExW(next.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                log::warn("victory_gpu::pipeline::library", "the host pipeline library saved last could not take the old one's place");
            }
            l->blob = Mapped::open(path);
        }
        if (l->blob) {
            HRESULT hr = d1->CreatePipelineLibrary(l->blob->at, l->blob->len, IID_PPV_ARGS(&l->library));
            if (SUCCEEDED(hr)) {
                log::info("victory_gpu::pipeline::library", "host pipeline library loaded", {{"bytes", log::debug(l->blob->len)}, {"mapped", log::debug(true)}});
                VIEW_LEN.store(l->blob->len, std::memory_order_relaxed);
                VIEW_AT.store(reinterpret_cast<std::uintptr_t>(l->blob->at), std::memory_order_relaxed);
                LOADED_AT.store(now_ms(), std::memory_order_relaxed);
                return l;
            }

            log::info("victory_gpu::pipeline::library", "host pipeline library rejected; starting an empty one", {{"error", log::hresult(hr)}});
            l->blob.reset();
        } else {
            log::info("victory_gpu::pipeline::library", "no host pipeline library yet; starting an empty one");
        }
        if (FAILED(d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&l->library)))) {
            log::warn("victory_gpu::pipeline::library", "no host pipeline library: pipelines are not kept");
            delete l;
            return nullptr;
        }
        return l;
    }();
    return made;
}


std::mutex& same_name(const std::wstring& name) {
    static std::mutex loading[64];
    std::size_t h = 0;
    for (wchar_t c : name) {
        h = h * 31 + static_cast<std::size_t>(c);
    }
    return loading[h % 64];
}


std::mutex USED_LOCK;
std::optional<std::unordered_set<std::string>> USED;
std::atomic<bool> USED_CHANGED{false};

std::unordered_set<std::string>& used_locked() {
    if (!USED) {
        USED.emplace();
        if (auto dir = pipeline_dir()) {
            std::ifstream in(*dir / EARLY_FILE);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty()) {
                    USED->insert(line);
                }
            }
        }
        log::info("victory_gpu::pipeline::library", "title pipelines to start early", {{"pipelines", log::debug(USED->size())}});
    }
    return *USED;
}

bool wanted_early(const std::string& key) {
    std::lock_guard lock(USED_LOCK);
    return used_locked().contains(key);
}

void note_used(const std::string& key) {
    std::lock_guard lock(USED_LOCK);
    auto& set = used_locked();
    if (set.size() < EARLY_MAX && set.insert(key).second) {
        USED_CHANGED.store(true, std::memory_order_relaxed);
    }
}


DWORD write_atomically(const std::filesystem::path& path, const std::string& bytes) {
    auto tmp = path;
    tmp.replace_extension(L".tmp");
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    DWORD written = 0;
    bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    DWORD error = ok ? 0 : GetLastError();
    CloseHandle(f);
    if (!ok) {
        return error;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) ? 0 : GetLastError();
}


void save(bool now) {
    static std::mutex last_lock;
    static std::optional<std::chrono::steady_clock::time_point> last;
    auto dir = pipeline_dir();
    Library* lib = library();
    if (!dir || !lib) {
        return;
    }
    {
        std::lock_guard lock(last_lock);
        auto t = std::chrono::steady_clock::now();
        if (!now && last && t - *last < std::chrono::seconds(20)) {
            return;
        }
        last = t;
    }
    std::error_code ec;
    std::filesystem::create_directories(*dir, ec);
    if (ec) {
        log::warn("victory_gpu::pipeline::library", "host pipeline library not saved", {{"error", os_error(static_cast<DWORD>(ec.value()))}});
        return;
    }
    auto tmp = *dir / L"host-pipelines.tmp";
    std::size_t bytes = 0;
    bool write_early;
    {
        std::unique_lock alone(lib->saving);
        std::uint64_t added = lib->added.load(std::memory_order_relaxed);
        bool used_changed = USED_CHANGED.exchange(false, std::memory_order_relaxed);
        if (!used_changed && added == 0) {
            return;
        }
        if (added > 0) {
            std::size_t size = lib->library->GetSerializedSize();
            LibraryCall call("serialise", "the whole library");
            auto out = Mapped::create(tmp, size);
            std::string error = out ? std::string() : os_error(GetLastError());
            if (out) {
                if (HRESULT hr = lib->library->Serialize(out->at, size); FAILED(hr)) {
                    error = log::hresult(hr);
                }
            }
            out.reset();
            if (!error.empty()) {
                if (used_changed) {
                    USED_CHANGED.store(true, std::memory_order_relaxed);
                }
                log::warn("victory_gpu::pipeline::library", "host pipeline library not serialised", {{"error", error}});
                DeleteFileW(tmp.c_str());
                return;
            }
            lib->added.store(0, std::memory_order_relaxed);
            bytes = size;
        }
        write_early = used_changed;
    }
    DWORD written = 0;
    if (bytes > 0) {
        auto path = *dir / LIBRARY_FILE;
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) &&
            !MoveFileExW(tmp.c_str(), (*dir / NEXT_FILE).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            written = GetLastError();
        }
    }
    if (bytes > 0 && written == 0) {

        std::error_code exists_ec;
        bool in_place = !std::filesystem::exists(*dir / NEXT_FILE, exists_ec);
        if (in_place && VIEW_AT.load(std::memory_order_relaxed) != 0) {
            REPLACED[1].store(now_ms(), std::memory_order_relaxed);
            REPLACED[0].fetch_add(1, std::memory_order_relaxed);
        }
        trail([&] {
            return "pipeline library: saved " + std::to_string(bytes) + " bytes, " +
                   (in_place ? "renamed over the library's file" : "left beside the library's file for the next start");
        });
    }
    std::size_t early = 0;
    if (write_early) {
        std::string list;
        {
            std::lock_guard lock(USED_LOCK);
            for (const auto& k : used_locked()) {
                list += k;
                list.push_back('\n');
                ++early;
            }
        }
        if (!list.empty()) {
            list.pop_back();
        }
        if (written == 0) {
            written = write_atomically(*dir / EARLY_FILE, list);
        }
    } else {
        std::lock_guard lock(USED_LOCK);
        early = USED ? USED->size() : 0;
    }
    if (written == 0) {
        log::info("victory_gpu::pipeline::library", "host pipeline library saved",
                  {{"bytes", log::debug(bytes)}, {"early", log::debug(early)}, {"made", log::debug(MADE.load(std::memory_order_relaxed))},
                   {"loaded", log::debug(LOADED.load(std::memory_order_relaxed))}, {"started_early", log::debug(EARLY_STARTED.load(std::memory_order_relaxed))}});
    } else {
        log::warn("victory_gpu::pipeline::library", "host pipeline library not saved", {{"error", os_error(written)}});
    }
}


std::wstring library_name(const char* kind, std::span<const std::uint8_t> root, std::span<const std::span<const std::uint8_t>> code, std::string_view state) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    auto part = [&](const void* p, std::size_t n) {
        std::uint64_t len = n;
        blake3_hasher_update(&h, &len, 8);
        blake3_hasher_update(&h, p, n);
    };
    part(kind, std::strlen(kind));
    part(root.data(), root.size());
    for (auto c : code) {
        part(c.data(), c.size());
    }
    part(state.data(), state.size());
    std::uint8_t out[32];
    blake3_hasher_finalize(&h, out, 32);
    std::string s = hex(out, 16);
    return std::wstring(s.begin(), s.end());
}

template <class Load, class Create>
ComPtr<ID3D12PipelineState> library_pipeline(const std::wstring& name, Load load, Create create, bool* loaded = nullptr) {
    Library* lib = library();
    if (!lib) {
        MADE.fetch_add(1, std::memory_order_relaxed);
        return create();
    }
    auto began = std::chrono::steady_clock::now();
    auto took_ms = [&] {
        char b[32];
        std::snprintf(b, sizeof b, "%.1f", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
        return std::string(b);
    };
    std::string narrow;
    for (wchar_t c : name) {
        narrow.push_back(static_cast<char>(c));
    }
    HRESULT refused = E_FAIL;
    {
        std::lock_guard same(same_name(name));
        LibraryCall call("load", narrow);
        auto pso = load(lib->library.Get(), name.c_str(), refused);

        LOADED_AT.store(now_ms(), std::memory_order_relaxed);
        TRIMMED.store(false, std::memory_order_relaxed);
        if (pso) {
            LOADED.fetch_add(1, std::memory_order_relaxed);
            trail([&] { return "pipeline library: loaded " + LABEL + " in " + took_ms() + " ms"; });
            if (loaded) {
                *loaded = true;
            }
            return pso;
        }
    }

    ComPtr<ID3D12PipelineState> pso;
    {
        LibraryCall call("make", narrow);
        pso = create();
    }
    if (!pso) {
        return nullptr;
    }
    MADE.fetch_add(1, std::memory_order_relaxed);
    trail([&] { return "pipeline library: made " + LABEL + " in " + took_ms() + " ms (the library did not load it: " + log::hresult(refused) + ")"; });
    std::shared_lock stores(lib->saving);
    LibraryCall call("store", narrow);


    if (SUCCEEDED(lib->library->StorePipeline(name.c_str(), pso.Get()))) {
        lib->added.fetch_add(1, std::memory_order_relaxed);
    }
    return pso;
}


constexpr std::int32_t RECTANGLES_TYPE = 19, QUADS_TYPE = 32;

struct GraphicsState {
    D3D12_BLEND_DESC blend{};
    std::uint32_t sample_mask = UINT32_MAX;
    D3D12_RASTERIZER_DESC rasterizer{};
    D3D12_DEPTH_STENCIL_DESC depth{};
    bool depth_bounds = false;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
    std::vector<std::string> names;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    D3D12_RT_FORMAT_ARRAY targets{};
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC samples{1, 0};

    GraphicsState() {
        D3D12_RENDER_TARGET_BLEND_DESC target{};
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        for (auto& t : blend.RenderTarget) {
            t = target;
        }
        rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
        rasterizer.CullMode = D3D12_CULL_MODE_BACK;
        rasterizer.DepthClipEnable = TRUE;
        D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        depth.StencilReadMask = 0xff;
        depth.StencilWriteMask = 0xff;
        depth.FrontFace = keep;
        depth.BackFace = keep;
    }


    void read(std::uint32_t kind, const std::uint8_t* at) {
        const std::uint8_t* data = at + 4;
        switch (kind) {
            case 8: std::memcpy(&blend, data, sizeof blend); break;
            case 9: std::memcpy(&sample_mask, data, 4); break;
            case 10: std::memcpy(&rasterizer, data, sizeof rasterizer); break;
            case 11:
                std::memcpy(&depth, data, sizeof depth);
                depth_bounds = false;
                break;
            case 21: {
                D3D12_DEPTH_STENCIL_DESC1 d;
                std::memcpy(&d, data, sizeof d);
                depth_bounds = d.DepthBoundsTestEnable != 0;
                depth = {d.DepthEnable, d.DepthWriteMask, d.DepthFunc, d.StencilEnable, d.StencilReadMask, d.StencilWriteMask, d.FrontFace, d.BackFace};
                break;
            }
            case 12: {
                D3D12_INPUT_LAYOUT_DESC layout;
                std::memcpy(&layout, at + 8, sizeof layout);
                if (!layout.pInputElementDescs) {
                    return;
                }
                names.clear();
                inputs.clear();
                for (UINT i = 0; i < layout.NumElements; ++i) {
                    names.emplace_back(layout.pInputElementDescs[i].SemanticName);
                }
                for (UINT i = 0; i < layout.NumElements; ++i) {
                    D3D12_INPUT_ELEMENT_DESC e = layout.pInputElementDescs[i];
                    e.SemanticName = names[i].c_str();
                    e.Format = static_cast<DXGI_FORMAT>(pc_format(static_cast<std::uint32_t>(e.Format)));
                    inputs.push_back(e);
                }
                break;
            }
            case 13: {
                std::int32_t v;
                std::memcpy(&v, data, 4);
                strip_cut = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(v);
                break;
            }
            case 14: {
                std::int32_t v;
                std::memcpy(&v, data, 4);
                topology = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(v);
                break;
            }
            case 15:
                std::memcpy(&targets, data, sizeof targets);
                for (auto& f : targets.RTFormats) {
                    f = static_cast<DXGI_FORMAT>(target_format(pc_format(static_cast<std::uint32_t>(f))));
                }
                break;
            case 16: {
                std::uint32_t f;
                std::memcpy(&f, data, 4);
                depth_format = static_cast<DXGI_FORMAT>(pc_format(f));
                break;
            }
            case 17: std::memcpy(&samples, data, sizeof samples); break;
            default: break;
        }
    }


    std::string describe() const {
        std::string out;
        auto add = [&](const void* p, std::size_t n) { out.append(static_cast<const char*>(p), n); };
        add(&blend, sizeof blend);
        add(&sample_mask, 4);
        add(&rasterizer, sizeof rasterizer);
        add(&depth, sizeof depth);
        out.push_back(depth_bounds ? 1 : 0);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            out += names[i];
            out.push_back(0);
            add(&inputs[i].SemanticIndex, sizeof(D3D12_INPUT_ELEMENT_DESC) - offsetof(D3D12_INPUT_ELEMENT_DESC, SemanticIndex));
        }
        add(&strip_cut, 4);
        add(&topology, 4);
        add(&targets, sizeof targets);
        add(&depth_format, 4);
        add(&samples, sizeof samples);
        return out;
    }

    D3D12_DEPTH_STENCIL_DESC1 depth1() const {
        return {depth.DepthEnable, depth.DepthWriteMask, depth.DepthFunc,   depth.StencilEnable, depth.StencilReadMask,
                depth.StencilWriteMask, depth.FrontFace, depth.BackFace, depth_bounds ? TRUE : FALSE};
    }

    std::uint32_t slots() const {
        std::uint32_t m = 0;
        for (const auto& e : inputs) {
            m |= 1u << std::min<UINT>(e.InputSlot, 31);
        }
        return m;
    }

    std::vector<Input> elements() const {
        std::uint32_t next[32] = {};
        std::vector<Input> out;
        for (const auto& e : inputs) {
            std::uint32_t slot = std::min<UINT>(e.InputSlot, 31);
            auto format = static_cast<std::uint32_t>(e.Format);
            std::uint32_t offset = e.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? next[slot] : e.AlignedByteOffset;
            std::uint64_t bytes = 0, block = 0;
            if (!element(format, bytes, block)) {
                bytes = 0;
            }
            next[slot] = offset + static_cast<std::uint32_t>(bytes);
            out.push_back({slot, offset, format, e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA});
        }
        return out;
    }
};


struct Collected {
    std::vector<D3D12_ROOT_PARAMETER1> params;
    std::vector<std::unique_ptr<D3D12_DESCRIPTOR_RANGE1>> ranges;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
};

Stage collect_stage_bindings(const std::shared_ptr<Shader>& shader, const HostShader& t, D3D12_SHADER_VISIBILITY visibility, Collected& c) {
    Stage s;
    s.shader = shader;
    std::uint32_t dwords = t.user_data_dwords();
    if (dwords > 0) {
        s.user_data = std::pair{static_cast<std::uint32_t>(c.params.size()), dwords};
        D3D12_ROOT_PARAMETER1 p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants = {0, USER_DATA_SPACE, dwords};
        p.ShaderVisibility = visibility;
        c.params.push_back(p);
    }
    struct Kind {
        const std::vector<std::uint8_t>* slots;
        D3D12_ROOT_PARAMETER_TYPE type;
        std::vector<std::pair<std::uint8_t, std::uint32_t>>* out;
    };
    for (const Kind& k : {Kind{&t.root_cbvs, D3D12_ROOT_PARAMETER_TYPE_CBV, &s.root_cbvs}, Kind{&t.root_srvs, D3D12_ROOT_PARAMETER_TYPE_SRV, &s.root_srvs},
                          Kind{&t.root_uavs, D3D12_ROOT_PARAMETER_TYPE_UAV, &s.root_uavs}}) {
        for (std::uint8_t slot : *k.slots) {
            bool uav = k.type == D3D12_ROOT_PARAMETER_TYPE_UAV;
            bool ours = uav ? slot == SLOT_FAULTS : slot == SLOT_STRIDES || slot == SLOT_MEMORY;
            if ((uav || k.type == D3D12_ROOT_PARAMETER_TYPE_SRV) && !ours && bounded_root_srvs()) {

                auto range = std::make_unique<D3D12_DESCRIPTOR_RANGE1>();
                range->RangeType = uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                range->NumDescriptors = 1;
                range->BaseShaderRegister = slot;
                range->RegisterSpace = ROOT_CBV_SPACE;
                range->Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                range->OffsetInDescriptorsFromTableStart = 0;
                k.out->emplace_back(slot, static_cast<std::uint32_t>(c.params.size()));
                D3D12_ROOT_PARAMETER1 p{};
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                p.DescriptorTable = {1, range.get()};
                p.ShaderVisibility = visibility;
                c.params.push_back(p);
                c.ranges.push_back(std::move(range));
                continue;
            }
            k.out->emplace_back(slot, static_cast<std::uint32_t>(c.params.size()));
            D3D12_ROOT_PARAMETER1 p{};
            p.ParameterType = k.type;
            p.Descriptor = {slot, ROOT_CBV_SPACE, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
            p.ShaderVisibility = visibility;
            c.params.push_back(p);
        }
    }
    for (std::size_t n = 0; n < t.static_samplers.size(); ++n) {
        const StaticSampler& x = t.static_samplers[n];
        D3D12_STATIC_SAMPLER_DESC d{};
        d.Filter = static_cast<D3D12_FILTER>(x.filter);
        d.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(x.address[0]);
        d.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(x.address[1]);
        d.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(x.address[2]);
        d.MipLODBias = x.mip_lod_bias;
        d.MaxAnisotropy = x.max_anisotropy;
        d.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(x.comparison);
        d.BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(x.border);
        d.MinLOD = x.min_lod;
        d.MaxLOD = x.max_lod;
        d.ShaderRegister = static_cast<UINT>(n);
        d.RegisterSpace = SAMPLER_SPACE;
        d.ShaderVisibility = visibility;
        c.samplers.push_back(d);
    }
    return s;
}

std::optional<std::pair<ComPtr<ID3D12RootSignature>, std::vector<std::uint8_t>>> root_signature_made(ID3D12Device* device, const Collected& c, bool compute,
                                                                                                    HRESULT* error);


std::optional<std::pair<ComPtr<ID3D12RootSignature>, std::vector<std::uint8_t>>> root_signature(ID3D12Device* device, const Collected& c, bool compute,
                                                                                               HRESULT* error = nullptr) {
    std::string key;
    auto add = [&](const void* p, std::size_t n) { key.append(static_cast<const char*>(p), n); };
    key.push_back(compute ? 1 : 0);
    for (const auto& p : c.params) {
        add(&p.ParameterType, 4);
        add(&p.ShaderVisibility, 4);
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            add(&p.DescriptorTable.NumDescriptorRanges, 4);
            for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
                add(&p.DescriptorTable.pDescriptorRanges[r], sizeof(D3D12_DESCRIPTOR_RANGE1));
            }
        } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            add(&p.Constants, sizeof p.Constants);
        } else {
            add(&p.Descriptor, sizeof p.Descriptor);
        }
    }
    for (const auto& s : c.samplers) {
        add(&s, sizeof s);
    }
    static std::mutex lock;
    static std::unordered_map<std::string, std::pair<ComPtr<ID3D12RootSignature>, std::vector<std::uint8_t>>> made;
    {
        std::lock_guard g(lock);
        if (auto it = made.find(key); it != made.end()) {
            return it->second;
        }
    }
    auto r = root_signature_made(device, c, compute, error);
    if (r) {
        std::lock_guard g(lock);
        made.emplace(std::move(key), *r);
    }
    return r;
}

std::optional<std::pair<ComPtr<ID3D12RootSignature>, std::vector<std::uint8_t>>> root_signature_made(ID3D12Device* device, const Collected& c, bool compute,
                                                                                                    HRESULT* error) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(c.params.size());
    desc.Desc_1_1.pParameters = c.params.data();
    desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(c.samplers.size());
    desc.Desc_1_1.pStaticSamplers = c.samplers.data();
    desc.Desc_1_1.Flags = (compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) |
                          D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob, text;
    if (HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &text); FAILED(hr)) {
        if (text) {
            log::warn("victory_gpu::pipeline::host",
                      "root signature: " + std::string(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize()));
        }
        if (error) {
            *error = hr;
        }
        return std::nullopt;
    }
    ComPtr<ID3D12RootSignature> root;
    if (HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)); FAILED(hr)) {
        if (error) {
            *error = hr;
        }
        return std::nullopt;
    }
    const auto* b = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
    return std::pair{root, std::vector<std::uint8_t>(b, b + blob->GetBufferSize())};
}

std::string names(const std::vector<std::shared_ptr<Shader>>& shaders) {
    std::string s = "[";
    for (std::size_t i = 0; i < shaders.size(); ++i) {
        s += (i ? ", " : "") + std::string(shaders[i]->stage) + " " + shaders[i]->hash;
    }
    return s + "]";
}


std::nullopt_t refuse(const char* reason, const std::vector<std::shared_ptr<Shader>>& shaders) {
    static std::mutex lock;
    static std::vector<const char*> seen;
    std::lock_guard g(lock);
    if (std::find(seen.begin(), seen.end(), reason) == seen.end()) {
        seen.push_back(reason);
        log::warn("victory_gpu::pipeline::host", std::string("no host pipeline: ") + reason + " (logged once)", {{"shaders", names(shaders)}});
    }
    return std::nullopt;
}

std::optional<HostPipeline> make_graphics_pipeline(const std::vector<std::shared_ptr<Shader>>& shaders, const GraphicsState* g);


template <class Depth>
ComPtr<ID3D12PipelineState> mesh_pipeline(ID3D12RootSignature* root, D3D12_SHADER_BYTECODE ms, D3D12_SHADER_BYTECODE ps, const GraphicsState& g,
                                          const Depth& depth, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE depth_kind, const std::wstring& name, HRESULT& error) {
    Host* h = host();
    ComPtr<ID3D12Device2> d2;
    if (!h) {
        return nullptr;
    }
    if (HRESULT hr = h->device().device()->QueryInterface(IID_PPV_ARGS(&d2)); FAILED(hr)) {
        error = hr;
        return nullptr;
    }
    struct alignas(void*) Stream {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t0 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
        ID3D12RootSignature* root;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t1 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
        D3D12_SHADER_BYTECODE ms;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t2 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
        D3D12_SHADER_BYTECODE ps;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t3 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
        D3D12_BLEND_DESC blend;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t4 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK;
        UINT mask;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t5 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
        D3D12_RASTERIZER_DESC raster;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t6;
        Depth depth;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t7 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
        D3D12_RT_FORMAT_ARRAY targets;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t8 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT;
        DXGI_FORMAT dsv;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t9 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC;
        DXGI_SAMPLE_DESC samples;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t10 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
    } stream;
    stream.root = root;
    stream.ms = ms;
    stream.ps = ps;
    stream.blend = g.blend;
    stream.mask = g.sample_mask;
    stream.raster = g.rasterizer;
    stream.t6 = depth_kind;
    stream.depth = depth;
    stream.targets = g.targets;
    stream.dsv = g.depth_format;
    stream.samples = g.samples;
    stream.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof stream, &stream};
    return library_pipeline(
        name,
        [&](ID3D12PipelineLibrary1* l, const wchar_t* n, HRESULT& refused) {
            ComPtr<ID3D12PipelineState> made;
            return SUCCEEDED(refused = l->LoadPipeline(n, &sd, IID_PPV_ARGS(&made))) ? made : nullptr;
        },
        [&] {
            ComPtr<ID3D12PipelineState> made;
            error = d2->CreatePipelineState(&sd, IID_PPV_ARGS(&made));
            return SUCCEEDED(error) ? made : nullptr;
        });
}

std::optional<HostPipeline> make_host_pipeline(const std::vector<std::shared_ptr<Shader>>& shaders, const GraphicsState* g) {


    LABEL = names(shaders);
    Host* h = host();
    if (!h) {
        return std::nullopt;
    }
    bool compute = shaders.size() == 1 && std::strcmp(shaders[0]->stage, "compute") == 0;
    if (!compute) {
        return make_graphics_pipeline(shaders, g);
    }
    const Translated* t = shaders[0]->translated();
    if (!t) {
        return std::nullopt;
    }
    Collected c;
    HostPipeline p;
    p.compute = true;
    p.stages.push_back(collect_stage_bindings(shaders[0], t->host, D3D12_SHADER_VISIBILITY_ALL, c));
    HRESULT error = S_OK;
    auto root = root_signature(h->device().device(), c, true, &error);
    if (!root) {
        log::warn("victory_gpu::pipeline::host", "host root signature refused", {{"error", log::hresult(error)}, {"shaders", names(shaders)}});
        return std::nullopt;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root->first.Get();
    desc.CS = {t->host.dxil.data(), t->host.dxil.size()};
    std::span<const std::uint8_t> code(t->host.dxil);
    auto name = library_name("compute", root->second, std::span(&code, 1), "");
    auto pso = library_pipeline(
        name,
        [&](ID3D12PipelineLibrary1* l, const wchar_t* n, HRESULT& refused) {
            ComPtr<ID3D12PipelineState> s;
            return SUCCEEDED(refused = l->LoadComputePipeline(n, &desc, IID_PPV_ARGS(&s))) ? s : nullptr;
        },
        [&] {
            ComPtr<ID3D12PipelineState> s;
            error = h->device().device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&s));
            return SUCCEEDED(error) ? s : nullptr;
        });
    if (!pso) {
        log::warn("victory_gpu::pipeline::host", "host pipeline refused", {{"error", log::hresult(error)}, {"shaders", names(shaders)}});
        return std::nullopt;
    }
    log::info("victory_gpu::pipeline::host", "host pipeline", {{"shaders", names(shaders)}});
    p.pso = pso;
    p.root = root->first;
    p.name = names(shaders);
    name_pipeline(p.pso.Get(), p.name);
    p.bake = is_bake_pipeline(p);
    return p;
}

std::optional<HostPipeline> make_graphics_pipeline(const std::vector<std::shared_ptr<Shader>>& shaders, const GraphicsState* g) {
    Host* h = host();
    auto find = [&](const char* stage) -> std::shared_ptr<Shader> {
        for (const auto& s : shaders) {
            if (std::strcmp(s->stage, stage) == 0) {
                return s;
            }
        }
        return nullptr;
    };
    for (const auto& s : shaders) {
        if (std::strcmp(s->stage, "vertex") != 0 && std::strcmp(s->stage, "pixel") != 0 && std::strcmp(s->stage, "mesh") != 0) {
            return refuse("only vertex, mesh, pixel and compute stages are translated", shaders);
        }
    }
    auto mesh = find("mesh");
    auto vs = mesh ? mesh : find("vertex");
    if (!vs) {
        return refuse("no vertex or mesh shader", shaders);
    }
    if (mesh && !host_mesh_shaders()) {
        return refuse("the host GPU has no mesh shaders (or no shader model 6.6)", shaders);
    }
    std::vector<std::pair<std::shared_ptr<Shader>, D3D12_SHADER_VISIBILITY>> stages{
        {vs, mesh ? D3D12_SHADER_VISIBILITY_MESH : D3D12_SHADER_VISIBILITY_VERTEX}};
    if (auto ps = find("pixel")) {
        stages.emplace_back(ps, D3D12_SHADER_VISIBILITY_PIXEL);
    }
    for (const auto& [s, v] : stages) {
        if (!s->translated()) {
            return std::nullopt;
        }
    }
    Collected c;
    HostPipeline p;
    for (const auto& [s, v] : stages) {
        p.stages.push_back(collect_stage_bindings(s, s->translated()->host, v, c));
    }
    HRESULT error = S_OK;
    auto root = root_signature(h->device().device(), c, false, &error);
    if (!root) {
        log::warn("victory_gpu::pipeline::host", "host root signature refused", {{"error", log::hresult(error)}, {"shaders", names(shaders)}});
        return std::nullopt;
    }
    if (!g) {
        return refuse("graphics state not kept (plain description)", shaders);
    }
    bool depth_bounds = g->depth_bounds && host_depth_bounds();
    if (g->depth_bounds && !depth_bounds) {
        static std::atomic<std::uint64_t> seen{0};
        if (seen.fetch_add(1, std::memory_order_relaxed) < 4) {
            log::warn("victory_gpu::pipeline::host",
                      "a pipeline enables the depth bounds test, which the host GPU does not support: its draws pass at every depth");
        }
    }
    std::string state = g->describe();
    auto code = [&](std::size_t i) -> D3D12_SHADER_BYTECODE {
        if (i >= stages.size()) {
            return {};
        }
        const auto& dxil = stages[i].first->translated()->host.dxil;
        return {dxil.data(), dxil.size()};
    };
    if (mesh) {


        std::span<const std::uint8_t> codes[2];
        for (std::size_t i = 0; i < 2; ++i) {
            auto b = code(i);
            codes[i] = std::span(static_cast<const std::uint8_t*>(b.pShaderBytecode), b.BytecodeLength);
        }
        auto name = library_name("mesh triangles", root->second, std::span(codes, 2), state);
        auto pso = depth_bounds ? mesh_pipeline(root->first.Get(), code(0), code(1), *g, g->depth1(), D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, name, error)
                                : mesh_pipeline(root->first.Get(), code(0), code(1), *g, g->depth, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, name, error);
        if (!pso) {
            log::warn("victory_gpu::pipeline::host", "host mesh pipeline refused", {{"error", log::hresult(error)}, {"shaders", names(shaders)}});
            return std::nullopt;
        }
        log::info("victory_gpu::pipeline::host", "host mesh pipeline", {{"shaders", names(shaders)}});
        p.pso = pso;
        p.root = root->first;
        p.mesh = true;
        p.name = names(shaders);
        name_pipeline(p.pso.Get(), p.name);
        p.slots = 0;
        p.rects = false;
        p.bake = is_bake_pipeline(p);
        return p;
    }


    std::optional<std::vector<std::uint8_t>> rect_gs;
    if (g->topology == RECTANGLES_TYPE) {
        const Translated* pst = shaders.size() > 1 ? shaders[1]->translated() : nullptr;
        const Translated* vst = shaders[0]->translated();
        if (vst) {
            try {
                std::optional<std::span<const std::uint8_t>> ps;
                if (pst) {
                    ps = std::span<const std::uint8_t>(pst->host.dxil);
                }
                rect_gs = shader::emit::rect_list_gs(vst->host.dxil, ps);
            } catch (const shader::ShaderError& e) {
                log::warn("victory_gpu::pipeline::host", "no rectangle geometry stage", {{"error", e.display()}, {"shaders", names(shaders)}});
            }
        }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root->first.Get();
    desc.VS = code(0);
    desc.PS = code(1);
    if (rect_gs) {
        desc.GS = {rect_gs->data(), rect_gs->size()};
    }
    desc.BlendState = g->blend;
    desc.SampleMask = g->sample_mask;
    desc.RasterizerState = g->rasterizer;
    desc.DepthStencilState = g->depth;
    desc.InputLayout = {g->inputs.data(), static_cast<UINT>(g->inputs.size())};
    desc.IBStripCutValue = g->strip_cut;
    desc.PrimitiveTopologyType = g->topology == RECTANGLES_TYPE || g->topology == QUADS_TYPE ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : g->topology;
    desc.NumRenderTargets = g->targets.NumRenderTargets;
    std::memcpy(desc.RTVFormats, g->targets.RTFormats, sizeof desc.RTVFormats);
    desc.DSVFormat = g->depth_format;
    desc.SampleDesc = g->samples;


    auto pipeline_name = [&](bool with_gs) {
        std::span<const std::uint8_t> codes[3];
        std::size_t ncodes = 0;
        for (std::size_t i = 0; i < 2; ++i) {
            auto b = code(i);
            codes[ncodes++] = std::span(static_cast<const std::uint8_t*>(b.pShaderBytecode), b.BytecodeLength);
        }
        if (with_gs) {
            codes[ncodes++] = std::span<const std::uint8_t>(*rect_gs);
            return library_name("graphics rectangles", root->second, std::span(codes, ncodes), state);
        }
        return library_name("graphics", root->second, std::span(codes, ncodes), state);
    };
    auto make = [&](bool with_gs) -> ComPtr<ID3D12PipelineState> {
        auto name = pipeline_name(with_gs);
        ComPtr<ID3D12PipelineState> pso;
        if (depth_bounds) {

            ComPtr<ID3D12Device2> d2;
            if (error = h->device().device()->QueryInterface(IID_PPV_ARGS(&d2)); SUCCEEDED(error)) {
                struct alignas(void*) Stream {
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t0 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
                    ID3D12RootSignature* root;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t1 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
                    D3D12_SHADER_BYTECODE vs;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t2 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
                    D3D12_SHADER_BYTECODE ps;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t3 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
                    D3D12_BLEND_DESC blend;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t4 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK;
                    UINT mask;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t5 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
                    D3D12_RASTERIZER_DESC raster;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t6 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1;
                    D3D12_DEPTH_STENCIL_DESC1 depth;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t7 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT;
                    D3D12_INPUT_LAYOUT_DESC layout;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t8 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE;
                    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t9 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
                    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t10 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
                    D3D12_RT_FORMAT_ARRAY targets;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t11 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT;
                    DXGI_FORMAT dsv;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t12 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC;
                    DXGI_SAMPLE_DESC samples;
                    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t13 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS;
                    D3D12_SHADER_BYTECODE gs;
                } stream;
                stream.root = desc.pRootSignature;
                stream.vs = desc.VS;
                stream.ps = desc.PS;
                stream.blend = desc.BlendState;
                stream.mask = desc.SampleMask;
                stream.raster = desc.RasterizerState;
                stream.depth = g->depth1();
                stream.layout = desc.InputLayout;
                stream.cut = desc.IBStripCutValue;
                stream.topology = desc.PrimitiveTopologyType;
                D3D12_RT_FORMAT_ARRAY rts{};
                std::memcpy(rts.RTFormats, desc.RTVFormats, sizeof rts.RTFormats);
                rts.NumRenderTargets = desc.NumRenderTargets;
                stream.targets = rts;
                stream.dsv = desc.DSVFormat;
                stream.samples = desc.SampleDesc;
                stream.gs = desc.GS;


                std::size_t size = with_gs ? sizeof stream : offsetof(Stream, t13);
                D3D12_PIPELINE_STATE_STREAM_DESC sd{size, &stream};
                error = d2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso));
            }
            return pso;
        }
        return library_pipeline(
            name,
            [&](ID3D12PipelineLibrary1* l, const wchar_t* n, HRESULT& refused) {
                ComPtr<ID3D12PipelineState> s;
                return SUCCEEDED(refused = l->LoadGraphicsPipeline(n, &desc, IID_PPV_ARGS(&s))) ? s : nullptr;
            },
            [&] {
                ComPtr<ID3D12PipelineState> s;
                error = h->device().device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&s));
                return SUCCEEDED(error) ? s : nullptr;
            });
    };
    ComPtr<ID3D12PipelineState> pso = make(rect_gs.has_value());
    if (!pso && rect_gs) {

        log::warn("victory_gpu::pipeline::host", "host pipeline refused with the rectangle geometry stage", {{"shaders", names(shaders)}});
        desc.GS = {};
        rect_gs.reset();
        pso = make(false);
    }
    if (!pso) {
        log::warn("victory_gpu::pipeline::host", "host pipeline refused", {{"error", log::hresult(error)}, {"shaders", names(shaders)}});
        return std::nullopt;
    }
    log::info("victory_gpu::pipeline::host", "host pipeline", {{"shaders", names(shaders)}});
    p.pso = pso;
    p.root = root->first;
    p.name = names(shaders);
    name_pipeline(p.pso.Get(), p.name);
    p.slots = g->slots();
    p.inputs = g->elements();
    p.rects = rect_gs.has_value();
    p.bake = is_bake_pipeline(p);
    return p;
}


constexpr std::uint8_t NOT_QUEUED = 0, EARLY = 1, REQUESTED = 2, MAKING = 3;

struct Pending {
    std::vector<std::shared_ptr<Shader>> shaders;
    std::unique_ptr<GraphicsState> graphics;

    std::string key;
    std::atomic<std::uint8_t> queue_state{NOT_QUEUED};
    std::once_flag once;
    std::atomic<bool> done{false};
    std::optional<HostPipeline> made;
    std::atomic<bool> used{false};
    std::atomic<std::int64_t> begun_ms{-1};

    bool computes() const { return shaders.size() == 1 && std::strcmp(shaders[0]->stage, "compute") == 0; }
    void make();
    bool slow() const;
};

std::mutex DONE_LOCK;
std::condition_variable DONE;
std::atomic<std::uint64_t> WAITING{0};

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::chrono::milliseconds wait_budget() {
    static const auto budget = [] {
        std::uint64_t ms = 12'000;
        wchar_t v[32] = {};
        DWORD n = GetEnvironmentVariableW(L"VICTORY_PIPELINE_WAIT_MS", v, 32);
        if (n > 0 && n < 32) {
            ms = std::wcstoull(v, nullptr, 10);
        }
        log::info("victory_gpu::pipeline::make", "host pipeline wait budget per frame, per kind", {{"millis", log::debug(ms)}});
        return std::chrono::milliseconds(ms);
    }();
    return budget;
}

std::chrono::seconds loading_allowance() {
    static const auto s = [] {
        std::uint64_t secs = 120;
        wchar_t v[32] = {};
        DWORD n = GetEnvironmentVariableW(L"VICTORY_PIPELINE_GATE_S", v, 32);
        if (n > 0 && n < 32) {
            secs = std::wcstoull(v, nullptr, 10);
        }
        log::info("victory_gpu::pipeline::make", "host pipeline loading allowance per kind", {{"seconds", log::debug(secs)}});
        return std::chrono::seconds(secs);
    }();
    return s;
}

bool held_too_long(std::chrono::milliseconds held, bool compute, std::chrono::milliseconds base) {
    auto most = std::chrono::milliseconds(compute ? 10'000 : 3'000);
    return base < most && held > most;
}

bool Pending::slow() const {
    std::int64_t at = begun_ms.load(std::memory_order_relaxed);
    return at >= 0 && held_too_long(std::chrono::milliseconds(steady_ms() - at), computes(), wait_budget());
}

void Pending::make() {
    std::call_once(once, [&] {
        begun_ms.store(steady_ms(), std::memory_order_relaxed);
        made = make_host_pipeline(shaders, graphics.get());
        done.store(true, std::memory_order_release);
    });
    std::lock_guard lock(DONE_LOCK);
    DONE.notify_all();
}

struct Queue {
    std::deque<std::shared_ptr<Pending>> early, requested;
    std::optional<bool> push(const std::shared_ptr<Pending>& p, std::uint8_t priority) {
        std::uint8_t prev = p->queue_state.load(std::memory_order_relaxed);
        if (prev >= priority) {
            return std::nullopt;
        }
        p->queue_state.store(priority, std::memory_order_relaxed);
        (priority == REQUESTED ? requested : early).push_back(p);
        return prev == NOT_QUEUED;
    }
    std::shared_ptr<Pending> pop() {
        std::shared_ptr<Pending> p;
        if (!requested.empty()) {
            p = std::move(requested.front());
            requested.pop_front();
        } else {
            while (!early.empty()) {
                auto e = std::move(early.front());
                early.pop_front();
                if (e->queue_state.load(std::memory_order_relaxed) == EARLY) {
                    p = std::move(e);
                    break;
                }
            }
        }
        if (p) {
            p->queue_state.store(MAKING, std::memory_order_relaxed);
        }
        return p;
    }
};

std::mutex QUEUE_LOCK;
Queue QUEUE;
std::condition_variable AVAILABLE;


std::atomic<std::uint64_t> PEAK{0};

bool workers() {
    static const bool running = [] {
        bool any = false;
        unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u);
        for (unsigned i = 0; i < n; ++i) {
            try {
                std::thread([] {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    for (;;) {
                        std::shared_ptr<Pending> p;
                        {
                            std::unique_lock lock(QUEUE_LOCK);
                            AVAILABLE.wait(lock, [&] { return (p = QUEUE.pop()) != nullptr; });
                        }
                        p->make();


                        if (WAITING.fetch_sub(1, std::memory_order_relaxed) == 1 && PEAK.exchange(0, std::memory_order_relaxed) >= 10) {
                            log::info("victory_gpu::pipeline::make", "host pipelines all made",
                                      {{"made", log::debug(MADE.load(std::memory_order_relaxed))}, {"loaded", log::debug(LOADED.load(std::memory_order_relaxed))},
                                       {"started_early", log::debug(EARLY_STARTED.load(std::memory_order_relaxed))}});
                        }
                        save(false);
                    }
                }).detach();
                any = true;
            } catch (const std::system_error& e) {
                log::warn("victory_gpu::pipeline::make", "no pipeline worker", {{"error", os_error(static_cast<DWORD>(e.code().value()))}});
            }
        }
        return any;
    }();
    return running;
}

void start(const std::shared_ptr<Pending>& p, std::uint8_t priority) {
    if (p->queue_state.load(std::memory_order_relaxed) >= priority) {
        return;
    }
    if (!workers()) {
        if (p->queue_state.exchange(MAKING, std::memory_order_relaxed) == NOT_QUEUED) {
            p->make();
        }
        return;
    }
    std::lock_guard lock(QUEUE_LOCK);
    if (auto fresh = QUEUE.push(p, priority)) {
        if (*fresh) {
            std::uint64_t waiting = WAITING.fetch_add(1, std::memory_order_relaxed) + 1;
            std::uint64_t peak = PEAK.load(std::memory_order_relaxed);
            while (peak < waiting && !PEAK.compare_exchange_weak(peak, waiting, std::memory_order_relaxed)) {
            }
        }
        AVAILABLE.notify_one();
    }
}


struct FrameWait {
    std::uint64_t frame = UINT64_MAX;
    std::chrono::nanoseconds spent{0};
    std::uint32_t waiting = 0;
    std::optional<std::chrono::steady_clock::time_point> since;

    std::chrono::nanoseconds begin(std::uint64_t f, std::chrono::nanoseconds budget, std::chrono::steady_clock::time_point now) {
        if (frame != f) {
            *this = FrameWait{};
            frame = f;
        }
        auto open = since ? now - *since : std::chrono::nanoseconds(0);
        auto used = spent + open;
        auto left = budget > used ? budget - used : std::chrono::nanoseconds(0);
        if (left.count() > 0) {
            if (waiting == 0) {
                since = now;
            }
            ++waiting;
        }
        return left;
    }
    void end(std::uint64_t f, std::chrono::steady_clock::time_point now) {
        if (frame != f) {
            return;
        }
        waiting = waiting > 0 ? waiting - 1 : 0;
        if (waiting == 0 && since) {
            spent += now - *since;
            since.reset();
        }
    }
};

void wait(Pending& p, std::uint64_t frame) {
    using namespace std::chrono;
    auto base = duration_cast<nanoseconds>(wait_budget());
    if (base.count() == 0 || p.slow()) {
        return;
    }
    static std::atomic<std::uint64_t> PAST[2]{};
    bool compute = p.computes();
    auto& past = PAST[compute ? 1 : 0];
    auto loading = compute ? seconds(12) : seconds(4);
    auto allowance = duration_cast<nanoseconds>(loading_allowance());
    auto spent_past = duration_cast<nanoseconds>(milliseconds(past.load(std::memory_order_relaxed)));
    auto rest_allowance = allowance > spent_past ? allowance - spent_past : nanoseconds(0);
    auto budget = std::max(base, std::min(duration_cast<nanoseconds>(loading), rest_allowance));
    static std::mutex FRAMES_LOCK[2];
    static FrameWait FRAMES[2];
    auto started = steady_clock::now();
    nanoseconds left;
    {
        std::lock_guard lock(FRAMES_LOCK[compute ? 1 : 0]);
        left = FRAMES[compute ? 1 : 0].begin(frame, budget, started);
    }
    if (left.count() == 0) {
        return;
    }
    {
        std::unique_lock lock(DONE_LOCK);
        while (!p.done.load(std::memory_order_acquire) && !p.slow()) {
            auto elapsed = steady_clock::now() - started;
            if (elapsed >= left) {
                break;
            }
            auto rest = std::min<nanoseconds>(left - elapsed, milliseconds(250));
            DONE.wait_for(lock, rest);
        }
    }
    auto waited = std::min<nanoseconds>(steady_clock::now() - started, left);
    if (waited > base) {
        past.fetch_add(static_cast<std::uint64_t>(duration_cast<milliseconds>(waited - base).count()), std::memory_order_relaxed);
    }
    std::lock_guard lock(FRAMES_LOCK[compute ? 1 : 0]);
    FRAMES[compute ? 1 : 0].end(frame, steady_clock::now());
}

const HostPipeline* ready(const std::shared_ptr<Pending>& p) {

    if (!p->used.load(std::memory_order_relaxed) && !p->used.exchange(true, std::memory_order_relaxed)) {
        note_used(p->key);
    }
    if (!p->done.load(std::memory_order_acquire)) {
        start(p, REQUESTED);
        wait(*p, frame_current());
    }
    if (!p->done.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return p->made ? &*p->made : nullptr;
}


struct Pipeline {
    Object object;
    std::uint32_t packet[8] = {};
    std::vector<std::shared_ptr<Shader>> shaders;
    std::atomic<bool> bound{false};
    std::shared_ptr<Pending> host;

    bool builder = false;
};


struct Builder {
    std::mutex lock;
    std::vector<std::shared_ptr<Pending>> pipelines;

    bool whole = false, off = false;
};
Builder& builder() {
    static Builder b;
    return b;
}


bool of_builder(const std::vector<std::shared_ptr<Shader>>& shaders) {
    if (shaders.size() != 1 || std::strcmp(shaders[0]->stage, "compute") != 0) {
        return false;
    }
    auto parts = shader::container_parts(shaders[0]->container);
    if (!parts) {
        return false;
    }
    for (const shader::Part& part : *parts) {
        if (part.fourcc == shader::fourcc("DXIL")) {
            auto programs = shader::native_programs(part.data);
            return programs && std::any_of(programs->begin(), programs->end(), [](const auto& p) { return shader::is_spilled(p); });
        }
    }
    return false;
}
static_assert(offsetof(Pipeline, packet) == 0x10);

HRESULT __stdcall pipeline_query(This self, const Iid* iid, This* out) { return answer(PIPELINE, iid, is(iid, PIPELINE_IIDS) ? self : nullptr, out); }
std::uint32_t __stdcall pipeline_release(This self) { return release_boxed<Pipeline>(self); }

const void* const* pipeline_functions() {

    static const Table<24> table = [] {
        auto f = d3d12_object(missing_table<PIPELINE, 24>());
        f[0] = fn(&pipeline_query);
        f[2] = fn(&pipeline_release);
        f[7] = fn(&parent_device);
        return f;
    }();
    return table.data();
}

HRESULT hand_out_pipeline(std::vector<std::shared_ptr<Shader>> shaders, const Iid* iid, This* out, std::unique_ptr<GraphicsState> graphics = nullptr) {
    auto pending = std::make_shared<Pending>();
    pending->shaders = shaders;
    pending->graphics = std::move(graphics);
    {
        blake3_hasher h;
        blake3_hasher_init(&h);
        for (const auto& sh : shaders) {
            blake3_hasher_update(&h, sh->stage, std::strlen(sh->stage));
            blake3_hasher_update(&h, sh->hash.data(), sh->hash.size());
        }
        if (pending->graphics) {
            auto d = pending->graphics->describe();
            blake3_hasher_update(&h, d.data(), d.size());
        }
        std::uint8_t k[32];
        blake3_hasher_finalize(&h, k, 32);
        pending->key = hex(k, 16);
    }

    if (!pending->shaders.empty() && wanted_early(pending->key)) {
        EARLY_STARTED.fetch_add(1, std::memory_order_relaxed);
        start(pending, EARLY);
    }
    auto* p = new Pipeline{Object{pipeline_functions()}};
    p->builder = of_builder(shaders);
    if (p->builder) {

        Builder& b = builder();
        std::lock_guard lock(b.lock);
        b.pipelines.push_back(pending);
        b.whole = false;
    }
    p->shaders = std::move(shaders);
    p->host = std::move(pending);
    return hand_out(PIPELINE, p, iid, PIPELINE_IIDS, out);
}

Pipeline& pipeline(This self) { return *reinterpret_cast<Pipeline*>(self); }

}   

const Translated* Shader::translated() {
    std::call_once(once_, [&] {

        probe_view_limit();


        auto failed = [&](const std::string& error) {
            log::warn("victory_gpu::pipeline", "shader does not translate; its pipelines' draws are dropped",
                      {{"stage", log::debug(stage)}, {"hash", hash}, {"error", error}});
        };
        auto parts = shader::container_parts(container);
        if (!parts) {
            std::string error = "malformed shader";
            try {
                shader::Container::parse(container);
            } catch (const shader::ShaderError& e) {
                error = e.display();
            }
            failed(error);
            return;
        }
        std::vector<shader::NativeProgram> programs;
        for (const shader::Part& part : *parts) {
            if (part.fourcc == shader::fourcc("DXIL")) {
                auto p = shader::native_programs(part.data);
                if (!p) {
                    std::string error = "malformed shader";
                    try {
                        shader::native_programs_checked(part.data);
                    } catch (const shader::ShaderError& e) {
                        error = e.display();
                    }
                    failed(error);
                    return;
                }
                programs = std::move(*p);
                break;
            }
        }
        auto t = host_shader(container);
        if (!t.shader) {
            failed(t.error);
            return;
        }
        log::info("victory_gpu::pipeline", "shader translated",
                  {{"stage", log::debug(stage)}, {"hash", hash}, {"user_data", log::debug(t.shader->user_data.size())}, {"samplers", log::debug(t.shader->samplers.size())}});
        Translated out;
        out.host = std::move(*t.shader);
        out.stores = programs.empty();
        for (const auto& p : programs) {
            out.stores = out.stores || shader::program_stores(p);
        }
        out.writes = out.stores;
        for (const auto& p : programs) {
            out.writes = out.writes || shader::program_image_stores(p);
        }
        for (const auto& p : programs) {
            if (p.stage == shader::HwStage::Vs || p.stage == shader::HwStage::Ps || p.stage == shader::HwStage::Ms || p.stage == shader::HwStage::Cs) {
                out.used = p.used;
                break;
            }
        }
        translated_ = std::move(out);
    });
    return translated_ ? &*translated_ : nullptr;
}

bool bounded_root_srvs() {
    static const bool on = !danzi::env::get(L"VICTORY_ROOT_SRV_DIRECT").has_value();
    return on;
}

const HostPipeline* host_pipeline(This self) {
    if (!self) {
        return nullptr;
    }
    return ready(pipeline(self).host);
}

void pipeline_bound(This self) {
    if (!self) {
        return;
    }
    Pipeline& p = pipeline(self);
    if (p.bound.load(std::memory_order_relaxed) || p.bound.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::uint64_t frame = frame_current();
    for (const auto& shader : p.shaders) {
        log::info("victory_gpu::pipeline", "first bind",
                  {{"pipeline", log::hex(reinterpret_cast<std::uintptr_t>(self))}, {"frame", log::debug(frame)}, {"stage", log::debug(shader->stage)},
                   {"size", log::debug(shader->container.size())}, {"hash", shader->hash}});
    }
}

void trim_library() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_LIBRARY_TRIM", L"0");
    std::uintptr_t at = VIEW_AT.load(std::memory_order_relaxed);
    std::size_t len = VIEW_LEN.load(std::memory_order_relaxed);
    if (at == 0 || TRIMMED.load(std::memory_order_relaxed) || !on) {
        return;
    }
    if (now_ms() < LOADED_AT.load(std::memory_order_relaxed) + ram_idle_ms() || TRIMMED.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    try {


        std::thread([at, len] {
            SetThreadDescription(GetCurrentThread(), L"victory-library-trim");
            VirtualUnlock(reinterpret_cast<void*>(at), len);
            TRIMMED_BYTES.fetch_add(len, std::memory_order_relaxed);
            TRIMMED_AT.store(now_ms(), std::memory_order_relaxed);
            trail([len] { return "pipeline library: the mapped file's pages let go from memory (" + std::to_string(len) + " bytes)"; });
        }).detach();
    } catch (...) {
        TRIMMED.store(false, std::memory_order_relaxed);
    }
}

std::uint64_t library_trimmed() { return TRIMMED_BYTES.load(std::memory_order_relaxed); }

std::string library_removal_note() {
    std::uint64_t now = now_us();
    std::string text = "pipeline library: ";
    char b[128];
    {
        std::lock_guard lock(CALLS_LOCK);
        if (CALLS.empty()) {
            text += "no call running";
        }
        for (std::size_t i = 0; i < CALLS.size(); ++i) {
            const RunningCall& c = CALLS[i];
            std::snprintf(b, sizeof b, " running for %.1f ms on thread %u", static_cast<double>(now > c.since ? now - c.since : 0) / 1000.0,
                          static_cast<unsigned>(c.thread));
            text += std::string(i == 0 ? "" : ", ") + c.what + " of " + c.name + b;
        }
    }
    std::uintptr_t at = VIEW_AT.load(std::memory_order_relaxed);
    std::size_t len = VIEW_LEN.load(std::memory_order_relaxed);
    if (at == 0) {
        return text + "; its file is not mapped";
    }
    auto ago = [](std::uint64_t ms) {
        std::uint64_t n = now_ms();
        return static_cast<double>(n > ms ? n - ms : 0) / 1000.0;
    };

    text += "; its file is mapped (" + std::to_string(len) + " bytes), the handle kept open";
    if (std::uint64_t n = REPLACED[0].load(std::memory_order_relaxed); n == 0) {
        text += "; this run has not replaced the file";
    } else {
        std::snprintf(b, sizeof b, " times, last %.1f s ago", ago(REPLACED[1].load(std::memory_order_relaxed)));
        text += "; this run replaced the file under the mapping " + std::to_string(n) + b;
    }
    if (TRIMMED_BYTES.load(std::memory_order_relaxed) > 0) {
        std::snprintf(b, sizeof b, "; its pages were last let go %.1f s ago", ago(TRIMMED_AT.load(std::memory_order_relaxed)));
        text += b;
    }

    auto started = std::chrono::steady_clock::now();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(at);
    std::uint64_t zero = 0, looked = 0;
    std::optional<std::size_t> first;
    for (std::size_t n = 0; n * 4096 < len; ++n) {
        if (n % 4096 == 0 && std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) {
            break;
        }
        looked += 1;
        std::size_t end = std::min(len, (n + 1) * 4096);
        if (std::all_of(bytes + n * 4096, bytes + end, [](std::uint8_t v) { return v == 0; })) {
            zero += 1;
            if (!first) {
                first = n;
            }
        }
    }
    text += "; " + std::to_string(zero) + " of the " + std::to_string(looked) + " pages looked at read as zeros now";
    if (first) {
        text += " (the first is page " + std::to_string(*first) + "): the library's contents were lost there";
    }
    return text;
}

int precompile_compute(const HostShader& shader, std::string_view label) {
    LABEL = label;
    Host* h = host();
    if (!h) {
        return -1;
    }
    Collected c;
    collect_stage_bindings(nullptr, shader, D3D12_SHADER_VISIBILITY_ALL, c);
    auto root = root_signature(h->device().device(), c, true);
    if (!root) {
        return -1;   
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root->first.Get();
    desc.CS = {shader.dxil.data(), shader.dxil.size()};
    std::span<const std::uint8_t> code(shader.dxil);
    auto name = library_name("compute", root->second, std::span(&code, 1), "");
    bool loaded = false;
    auto pso = library_pipeline(
        name,
        [&](ID3D12PipelineLibrary1* l, const wchar_t* n, HRESULT& refused) {
            ComPtr<ID3D12PipelineState> s;
            return SUCCEEDED(refused = l->LoadComputePipeline(n, &desc, IID_PPV_ARGS(&s))) ? s : nullptr;
        },
        [&] {
            ComPtr<ID3D12PipelineState> s;
            return SUCCEEDED(h->device().device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&s))) ? s : nullptr;
        },
        &loaded);
    if (!pso) {
        return -1;   
    }
    if (!loaded) {
        save(false);
    }
    return loaded ? 1 : 0;
}

bool is_builder(This self) { return self && pipeline(self).builder; }

bool builder_quarantined(This self) {
    static const bool on = danzi::env::is(L"VICTORY_BVH_QUARANTINE", L"1");
    return on && is_builder(self);
}

void builder_off(const char* why) {
    Builder& b = builder();
    std::lock_guard lock(b.lock);
    if (!b.off) {
        b.off = true;
        log::warn("victory_gpu::pipeline", "the ray-tracing structure builder is turned off for the rest of the run: its stages run all or none", {{"why", log::debug(why)}});
    }
}

bool builder_runs() {
    static const bool allowed = danzi::env::is(L"VICTORY_BVH_BUILDER", L"1");
    if (!allowed) {
        return false;
    }
    Builder& b = builder();
    std::lock_guard lock(b.lock);
    if (b.off) {
        return false;
    }
    if (b.whole) {
        return true;
    }
    std::size_t missing = 0;
    for (const auto& p : b.pipelines) {

        p->make();
        if (!p->made) {
            ++missing;
        }
    }
    if (missing > 0) {
        b.off = true;
        log::warn("victory_gpu::pipeline",
                  "the ray-tracing structure builder stays off: not all of its programs have a host pipeline, and its stages run all or none",
                  {{"missing", log::debug(missing)}, {"of", log::debug(b.pipelines.size())}});
        return false;
    }
    log::info("victory_gpu::pipeline", "the ray-tracing structure builder runs: all of its programs have host pipelines",
              {{"pipelines", log::debug(b.pipelines.size())}});
    b.whole = true;
    return true;
}

std::uint64_t pipelines_making() { return WAITING.load(std::memory_order_relaxed); }
std::array<std::uint64_t, 3> pipelines_counts() { return {MADE.load(), LOADED.load(), EARLY_STARTED.load()}; }

void use_pipeline_cache(const wchar_t* dir) {
    if (!dir) {
        return;
    }
    std::lock_guard lock(DIR_LOCK);
    if (!DIR) {
        DIR = std::filesystem::path(dir);
    }
}

void save_pipeline_cache() { save(true); }

HRESULT __stdcall create_graphics_pipeline(This, const void* desc, const Iid* iid, This* out) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    struct Head {
        This root;
        Bytecode stages[5];
    };
    static const char* const STAGES[5] = {"vertex", "pixel", "domain", "hull", "geometry"};
    const auto* head = static_cast<const Head*>(desc);
    std::vector<std::shared_ptr<Shader>> shaders;
    for (int i = 0; i < 5; ++i) {
        if (auto s = copy(STAGES[i], head->stages[i])) {
            shaders.push_back(std::move(s));
        }
    }
    return hand_out_pipeline(std::move(shaders), iid, out);
}

HRESULT __stdcall create_compute_pipeline(This, const void* desc, const Iid* iid, This* out) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    struct Head {
        This root;
        Bytecode stage;
    };
    const auto* head = static_cast<const Head*>(desc);
    std::vector<std::shared_ptr<Shader>> shaders;
    if (auto s = copy("compute", head->stage)) {
        shaders.push_back(std::move(s));
    }
    return hand_out_pipeline(std::move(shaders), iid, out);
}


std::optional<std::pair<std::size_t, const char*>> subobject(std::uint32_t kind) {
    switch (kind) {
        case 1: return std::pair<std::size_t, const char*>{24, "vertex"};
        case 2: return std::pair<std::size_t, const char*>{24, "pixel"};
        case 3: return std::pair<std::size_t, const char*>{24, "domain"};
        case 4: return std::pair<std::size_t, const char*>{24, "hull"};
        case 5: return std::pair<std::size_t, const char*>{24, "geometry"};
        case 6: return std::pair<std::size_t, const char*>{24, "compute"};
        case 23: return std::pair<std::size_t, const char*>{24, "amplification"};
        case 24: return std::pair<std::size_t, const char*>{24, "mesh"};
        case 0: return std::pair<std::size_t, const char*>{16, nullptr};
        case 12:
        case 19: return std::pair<std::size_t, const char*>{24, nullptr};
        case 7:
        case 15: return std::pair<std::size_t, const char*>{40, nullptr};
        case 8: return std::pair<std::size_t, const char*>{336, nullptr};
        case 10: return std::pair<std::size_t, const char*>{48, nullptr};
        case 11: return std::pair<std::size_t, const char*>{56, nullptr};
        case 17: return std::pair<std::size_t, const char*>{16, nullptr};
        case 21: return std::pair<std::size_t, const char*>{64, nullptr};
        case 22: return std::pair<std::size_t, const char*>{32, nullptr};
        case 9:
        case 13:
        case 14:
        case 16:
        case 18:
        case 20: return std::pair<std::size_t, const char*>{8, nullptr};
        default: return std::nullopt;
    }
}

HRESULT __stdcall create_pipeline_stream(This, const void* desc_, const Iid* iid, This* out) {
    struct StreamDesc {
        std::size_t size;
        const std::uint8_t* stream;
    };
    const auto* desc = static_cast<const StreamDesc*>(desc_);
    if (!desc || !desc->stream) {
        return E_INVALIDARG_;
    }
    std::vector<std::shared_ptr<Shader>> shaders;
    auto graphics = std::make_unique<GraphicsState>();
    std::size_t at = 0;
    while (at + 8 <= desc->size) {
        std::uint32_t kind;
        std::memcpy(&kind, desc->stream + at, 4);
        auto sub = subobject(kind);
        if (!sub) {
            log::warn("victory_gpu::pipeline", "pipeline subobject of unknown size; the rest is ignored", {{"kind", log::debug(kind)}, {"at", log::debug(at)}});
            break;
        }
        if (sub->second) {
            Bytecode code;
            std::memcpy(&code, desc->stream + at + 8, sizeof code);
            if (auto s = copy(sub->second, code)) {
                shaders.push_back(std::move(s));
            }
        }
        graphics->read(kind, desc->stream + at);
        at += sub->first;
    }
    return hand_out_pipeline(std::move(shaders), iid, out, std::move(graphics));
}

bool host_mesh_shaders() {
    static const bool on = [] {
        Host* h = host();
        if (!h) {
            return false;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o{};
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
        bool mesh = SUCCEEDED(h->device().device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o, sizeof o)) &&
                    SUCCEEDED(h->device().device()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof model)) &&
                    o.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED && model.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
        log::info("victory_gpu::host", "host mesh shader support", {{"mesh", log::debug(mesh)}});
        return mesh;
    }();
    return on;
}

bool host_depth_bounds() {
    static const bool on = [] {
        Host* h = host();
        if (!h) {
            return false;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS2 o{};
        bool supported = SUCCEEDED(h->device().device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &o, sizeof o)) && o.DepthBoundsTestSupported;
        log::info("victory_gpu::host", "host depth bounds support", {{"supported", log::debug(supported)}});
        return supported;
    }();
    return on;
}

HRESULT __stdcall create_compute_pipeline_x(This self, const void* desc, std::uint32_t, const std::uint8_t*, const Iid* iid, This* out) {
    return create_compute_pipeline(self, desc, iid, out);
}

}   
