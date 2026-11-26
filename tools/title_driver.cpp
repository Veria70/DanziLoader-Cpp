// SPDX-License-Identifier: GPL-3.0-or-later
// Milestone 4's title driver: plays the title's part against a backend DLL
// (the reference's `victory-gpu` built unchanged, `victory_ref.dll`, or the
// port's) through the console objects' function tables, from a scenario
// file. Both backends export the same entry points (`d3d12_xs.dll`'s, with a
// `backend_` prefix, and the runtime's hand-over), so the same calls reach
// both by construction.
//
// Usage: title-driver <backend.dll> <scenario.txt> <out-dir>
// Writes <out-dir>/log.txt (what the title sees) and timing.txt (per
// operation: its number, name and nanoseconds), as the dirty-range runners
// do, so `dirty-diff compare` and `bench` take them.
//
// Objects are logged by name, never by address (the two backends place
// them differently): each new object returned gets the name the scenario
// gives it, or `#n`.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <dwmapi.h>
#include <psapi.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>


static std::atomic<bool> PROFILE_SAMPLING{true};

namespace fs = std::filesystem;

namespace {

using HR = HRESULT;

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string hex(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::string hr_text(HR hr) {
    char b[16];
    std::snprintf(b, sizeof b, "%08lx", static_cast<unsigned long>(hr));
    return b;
}

std::uint64_t num(const std::string& s) {
    try {
        bool hexed = s.rfind("0x", 0) == 0;
        size_t used = 0;
        std::uint64_t v = std::stoull(hexed ? s.substr(2) : s, &used, hexed ? 16 : 10);
        if (used + (hexed ? 2 : 0) != s.size()) {
            throw Failure("not a number: " + s);
        }
        return v;
    } catch (const std::invalid_argument&) {
        throw Failure("not a number: " + s);
    } catch (const std::out_of_range&) {
        throw Failure("not a number: " + s);
    }
}

long long now_ns() {
    static const long long freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<long long>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(freq));
}


std::ostringstream* RUNTIME_LOG = nullptr;
bool RUNTIME_QUIET = false;

void* __stdcall runtime_alloc(std::size_t size, std::uint64_t attributes) {
    if (RUNTIME_LOG && !RUNTIME_QUIET) {
        *RUNTIME_LOG << "  runtime alloc " << size << " attributes " << std::hex << "0x" << attributes << std::dec << "\n";
    }
    return VirtualAlloc(nullptr, size ? size : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void __stdcall runtime_free(void* at, std::uint64_t attributes) {
    if (RUNTIME_LOG && !RUNTIME_QUIET) {
        *RUNTIME_LOG << "  runtime free attributes " << std::hex << "0x" << attributes << std::dec << "\n";
    }
    VirtualFree(at, 0, MEM_RELEASE);
}


int __stdcall runtime_map(std::uint64_t to, std::uint64_t from, std::uint64_t len) {
    if (RUNTIME_LOG && !RUNTIME_QUIET) {
        *RUNTIME_LOG << "  runtime map to " << std::hex << "0x" << to << " from 0x" << from << " len 0x" << len << std::dec << "\n";
    }
    return to == 0xbad0000 ? 0 : 1;
}


struct Backend {
    HMODULE dll = nullptr;
    void(__stdcall* use_runtime)(void*, void*, void*) = nullptr;
    HR(__stdcall* configure_vm)(void*) = nullptr;
    HR(__stdcall* create_device)(const void*, const std::uint32_t*, const GUID*, void**) = nullptr;
    const GUID*(__stdcall* device_iid)() = nullptr;
    void(__stdcall* set_console)(std::uint32_t) = nullptr;
    void(__stdcall* title_memory_remapped)(std::size_t, std::size_t) = nullptr;
    HR(__stdcall* serialize_root)(const void*, UINT, void**, void**) = nullptr;
    HR(__stdcall* serialize_root_versioned)(const void*, void**, void**) = nullptr;
    HR(__stdcall* root_deserializer)(const void*, std::size_t, const GUID*, void**) = nullptr;
    void(__stdcall* use_shader_cache)(const wchar_t*) = nullptr;

    void(__stdcall* shader_build)(std::uint8_t*) = nullptr;
    void(__stdcall* use_shader_build)(const std::uint8_t*) = nullptr;
    void(__stdcall* use_pipeline_cache)(const wchar_t*) = nullptr;
    void(__stdcall* save_pipeline_cache)() = nullptr;

    void(__stdcall* pipeline_counts)(std::uint64_t*) = nullptr;
    void(__stdcall* sweep_counts)(std::uint64_t*) = nullptr;
    void(__stdcall* heap_counts)(std::uint64_t*) = nullptr;
    std::uint64_t(__stdcall* library_trimmed)() = nullptr;
    int(__stdcall* precompile_init)(std::uint32_t*) = nullptr;
    int(__stdcall* precompile_shader)(const std::uint8_t*, std::size_t, std::uint32_t*) = nullptr;
    int(__stdcall* precompile_compute)(std::uint32_t, const char*, std::size_t) = nullptr;


    std::string name;

    explicit Backend(const fs::path& path) {
        name = path.stem().string();
        dll = LoadLibraryW(path.c_str());
        if (!dll) {
            throw Failure("cannot load " + path.string() + " (error " + std::to_string(GetLastError()) + ")");
        }
        auto get = [&](const char* name) {
            FARPROC p = GetProcAddress(dll, name);
            if (!p) {
                throw Failure(std::string("backend lacks ") + name);
            }
            return p;
        };
        use_runtime = reinterpret_cast<decltype(use_runtime)>(get("backend_use_runtime"));
        configure_vm = reinterpret_cast<decltype(configure_vm)>(get("backend_D3D12ConfigureVirtualMemoryX"));
        create_device = reinterpret_cast<decltype(create_device)>(get("backend_D3D12XboxCreateDevice"));
        device_iid = reinterpret_cast<decltype(device_iid)>(get("backend_device_iid"));
        set_console = reinterpret_cast<decltype(set_console)>(get("backend_set_console"));
        title_memory_remapped = reinterpret_cast<decltype(title_memory_remapped)>(get("backend_title_memory_remapped"));
        serialize_root = reinterpret_cast<decltype(serialize_root)>(get("backend_D3D12SerializeRootSignature"));
        serialize_root_versioned = reinterpret_cast<decltype(serialize_root_versioned)>(get("backend_D3D12SerializeVersionedRootSignature"));
        root_deserializer = reinterpret_cast<decltype(root_deserializer)>(get("backend_D3D12CreateRootSignatureDeserializer"));
        use_shader_cache = reinterpret_cast<decltype(use_shader_cache)>(get("backend_use_shader_cache"));
        shader_build = reinterpret_cast<decltype(shader_build)>(GetProcAddress(dll, "oracle_shader_build"));
        use_shader_build = reinterpret_cast<decltype(use_shader_build)>(GetProcAddress(dll, "backend_use_shader_build"));
        use_pipeline_cache = reinterpret_cast<decltype(use_pipeline_cache)>(get("backend_use_pipeline_cache"));
        save_pipeline_cache = reinterpret_cast<decltype(save_pipeline_cache)>(get("backend_save_pipeline_cache"));
        pipeline_counts = reinterpret_cast<decltype(pipeline_counts)>(GetProcAddress(dll, "backend_pipeline_counts"));
        sweep_counts = reinterpret_cast<decltype(sweep_counts)>(GetProcAddress(dll, "backend_sweep_counts"));
        heap_counts = reinterpret_cast<decltype(heap_counts)>(GetProcAddress(dll, "backend_heap_counts"));
        library_trimmed = reinterpret_cast<decltype(library_trimmed)>(GetProcAddress(dll, "backend_library_trimmed"));
        precompile_init = reinterpret_cast<decltype(precompile_init)>(GetProcAddress(dll, "backend_precompile_init"));
        precompile_shader = reinterpret_cast<decltype(precompile_shader)>(GetProcAddress(dll, "backend_precompile_shader"));
        precompile_compute = reinterpret_cast<decltype(precompile_compute)>(GetProcAddress(dll, "backend_precompile_compute"));
    }
};


template <class F>
F slot(void* object, unsigned index) {
    auto table = *static_cast<void***>(object);
    return reinterpret_cast<F>(table[index]);
}


struct VirtualMemory {
    std::uint32_t flags, page_table_pages, preserved_bytes;
    std::uint64_t lowest, highest;
};
static_assert(sizeof(VirtualMemory) == 0x20);

const std::map<std::string, GUID>& iids() {
    static const std::map<std::string, GUID> m = {
        {"unknown", __uuidof(IUnknown)},
        {"device", __uuidof(ID3D12Device)},
        {"device1", __uuidof(ID3D12Device1)},
        {"device2", __uuidof(ID3D12Device2)},
        {"device3", __uuidof(ID3D12Device3)},
        {"device4", __uuidof(ID3D12Device4)},
        {"device5", __uuidof(ID3D12Device5)},
        {"device6", __uuidof(ID3D12Device6)},
        {"device7", __uuidof(ID3D12Device7)},
        {"device8", __uuidof(ID3D12Device8)},
        {"device9", __uuidof(ID3D12Device9)},
        {"device11", __uuidof(ID3D12Device11)},
        {"dxgi_object", __uuidof(IDXGIObject)},
        {"dxgi_device", __uuidof(IDXGIDevice)},
        {"dxgi_device1", __uuidof(IDXGIDevice1)},
        {"dxgi_device2", __uuidof(IDXGIDevice2)},
        {"dxgi_device3", __uuidof(IDXGIDevice3)},
        {"dxgi_device4", __uuidof(IDXGIDevice4)},
        {"dxgi_adapter", __uuidof(IDXGIAdapter)},
        {"dxgi_adapter1", __uuidof(IDXGIAdapter1)},
        {"dxgi_adapter2", __uuidof(IDXGIAdapter2)},
        {"dxgi_factory", __uuidof(IDXGIFactory)},
        {"dxgi_factory1", __uuidof(IDXGIFactory1)},
        {"dxgi_factory2", __uuidof(IDXGIFactory2)},
        {"dxgi_output", __uuidof(IDXGIOutput)},

        {"pd1", GUID{0x11111111, 0x2222, 0x3333, {4, 4, 4, 4, 4, 4, 4, 4}}},
        {"pd2", GUID{0x55555555, 0x6666, 0x7777, {8, 8, 8, 8, 8, 8, 8, 8}}},
        {"zero", GUID{}},
        {"fence", __uuidof(ID3D12Fence)},
        {"fence1", __uuidof(ID3D12Fence1)},
        {"pageable", __uuidof(ID3D12Pageable)},
        {"device_child", __uuidof(ID3D12DeviceChild)},
        {"d3d12_object", __uuidof(ID3D12Object)},
        {"queue", __uuidof(ID3D12CommandQueue)},
        {"root", __uuidof(ID3D12RootSignature)},
        {"deserializer", __uuidof(ID3D12RootSignatureDeserializer)},
        {"vdeserializer", __uuidof(ID3D12VersionedRootSignatureDeserializer)},
        {"cmdsig", __uuidof(ID3D12CommandSignature)},
        {"queryheap", __uuidof(ID3D12QueryHeap)},
        {"allocator", __uuidof(ID3D12CommandAllocator)},
        {"list", __uuidof(ID3D12GraphicsCommandList)},
        {"list1", __uuidof(ID3D12GraphicsCommandList1)},
        {"list4", __uuidof(ID3D12GraphicsCommandList4)},
        {"cmdlist", __uuidof(ID3D12CommandList)},
    };
    return m;
}

struct World {
    Backend& backend;
    std::ostringstream log;
    std::ostringstream timing;
    size_t op_number = 0;
    std::map<std::string, void*> objects;
    std::map<void*, std::string> names;

    std::map<std::string, HANDLE> events;


    bool newer = false;
    std::map<void*, bool> newer_queue;

    std::map<void*, bool> newer_list;


    unsigned list_slot(void* l, unsigned older) {
        auto it = newer_list.find(l);
        return it != newer_list.end() && it->second && older >= 59 ? older + 2 : older;
    }


    std::uint64_t address(const std::string& text) {
        auto plus = text.find('+');
        std::string base = text.substr(0, plus);
        std::uint64_t offset = plus == std::string::npos ? 0 : num(text.substr(plus + 1));
        if (auto it = title.find(base); it != title.end()) {
            return reinterpret_cast<std::uint64_t>(it->second) + offset;
        }
        if (objects.count(base)) {
            void* r = object(base);
            return slot<std::uint64_t(__stdcall*)(void*)>(r, 11)(r) + offset;
        }
        return num(text);
    }


    unsigned queue_slot(void* q, unsigned older) {
        auto it = newer_queue.find(q);
        return it != newer_queue.end() && it->second && older >= 14 ? older + 2 : older;
    }

    HANDLE event(const std::string& name) {
        auto it = events.find(name);
        if (it == events.end()) {
            HANDLE e = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!e) {
                throw Failure("no event");
            }
            it = events.emplace(name, e).first;
        }
        return it->second;
    }


    void gone(void* p) {
        if (auto it = names.find(p); it != names.end()) {
            objects.erase(it->second);
            names.erase(it);
        }
        newer_queue.erase(p);
    }


    std::map<std::string, ID3DBlob*> blobs;


    std::string build_file;


    std::map<std::string, std::vector<std::uint8_t>> shaders;

    std::vector<std::uint8_t>& shader_named(const std::string& name) {
        auto it = shaders.find(name);
        if (it == shaders.end()) {
            throw Failure("no shader " + name);
        }
        return it->second;
    }

    ID3DBlob* blob_named(const std::string& name) {
        auto it = blobs.find(name);
        if (it == blobs.end()) {
            throw Failure("no blob " + name);
        }
        return it->second;
    }


    std::map<std::string, std::uint8_t*> title;


    D3D12_RESOURCE_DESC parse_desc(const std::string& text) {
        D3D12_RESOURCE_DESC d{};
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        std::istringstream in(text);
        std::string f;
        while (std::getline(in, f, '/')) {
            if (f.empty()) {
                continue;
            }
            char k = f[0];
            std::string v = f.substr(1);
            if (k == 'B') {
                d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                d.Width = num(v);
                d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            } else if (k == 'T') {
                d.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(num(v));
            } else if (k >= '0' && k <= '9') {
                std::istringstream sizes(f);
                std::string part;
                int i = 0;
                while (std::getline(sizes, part, 'x')) {
                    std::uint64_t n = num(part);
                    if (i == 0) {
                        d.Width = n;
                    } else if (i == 1) {
                        d.Height = static_cast<UINT>(n);
                    } else {
                        d.DepthOrArraySize = static_cast<UINT16>(n);
                    }
                    ++i;
                }
            } else if (k == 'm') {
                d.MipLevels = static_cast<UINT16>(num(v));
            } else if (k == 'f') {
                d.Format = static_cast<DXGI_FORMAT>(num(v));
            } else if (k == 's') {
                d.SampleDesc.Count = static_cast<UINT>(num(v));
            } else if (k == 'q') {
                d.SampleDesc.Quality = static_cast<UINT>(num(v));
            } else if (k == 'l') {
                d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(num(v));
            } else if (k == 'F') {
                d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(num(v));
            } else if (k == 'a') {
                d.Alignment = num(v);
            } else {
                throw Failure("bad description field " + f);
            }
        }
        return d;
    }


    static std::vector<std::uint32_t> words(const std::string& text, size_t n) {
        if (text == "null") {
            return {};
        }
        if (text.rfind("w:", 0) != 0) {
            throw Failure("words are w:<a>,<b>,...: " + text);
        }
        std::vector<std::uint32_t> w(n, 0);
        std::istringstream in(text.substr(2));
        std::string x;
        size_t i = 0;
        while (std::getline(in, x, ',') && i < n) {
            w[i++] = static_cast<std::uint32_t>(num(x));
        }
        return w;
    }


    std::uint64_t handle(const std::string& text) {
        auto colon = text.find(':');
        if (colon == std::string::npos) {
            return num(text);
        }
        void* h = object(text.substr(0, colon));
        UINT d[4];
        slot<void(__stdcall*)(void*, void*)>(h, 8)(h, d);
        const std::uint64_t sizes[4] = {32, 16, 128, 128};
        std::uint64_t start = slot<std::uint64_t(__stdcall*)(void*)>(h, 9)(h);
        return start + num(text.substr(colon + 1)) * (d[0] < 4 ? sizes[d[0]] : 0);
    }

    static std::string desc_text(const D3D12_RESOURCE_DESC& d) {
        std::ostringstream s;
        s << "dim " << d.Dimension << " align " << d.Alignment << " " << d.Width << "x" << d.Height << "x" << d.DepthOrArraySize << " mips " << d.MipLevels
          << " format " << d.Format << " samples " << d.SampleDesc.Count << "," << d.SampleDesc.Quality << " layout 0x" << std::hex << d.Layout
          << " flags 0x" << d.Flags << std::dec;
        return s.str();
    }

    void* object(const std::string& name) {
        auto it = objects.find(name);
        if (it == objects.end()) {
            throw Failure("unknown object " + name);
        }
        return it->second;
    }


    std::string seen(void* p, const std::string& name) {
        if (!p) {
            return "null";
        }
        if (auto it = names.find(p); it != names.end()) {
            return it->second;
        }
        std::string n = name.empty() ? "#" + std::to_string(names.size() + 1) : name;
        names[p] = n;
        objects[n] = p;
        return n;
    }

    const GUID& iid(const std::string& name) {
        auto it = iids().find(name);
        if (it == iids().end()) {
            throw Failure("unknown interface " + name);
        }
        return it->second;
    }


    std::uint64_t repeat = 1;

    template <class F>
    auto timed(const char* what, F&& f) {
        long long t = now_ns();
        auto r = f();
        for (std::uint64_t k = 1; k < repeat; ++k) {
            r = f();
        }
        long long ns = now_ns() - t;
        timing << op_number << " " << what << " " << (repeat > 1 ? static_cast<double>(ns) / static_cast<double>(repeat) : static_cast<double>(ns)) << "\n";
        return r;
    }


    D3D12_TEXTURE_COPY_LOCATION copy_location(const std::string& text) {
        std::vector<std::string> f;
        std::istringstream in(text);
        std::string x;
        while (std::getline(in, x, ':')) {
            f.push_back(x);
        }
        D3D12_TEXTURE_COPY_LOCATION c{};
        c.pResource = static_cast<ID3D12Resource*>(object(f.at(0)));
        if (f.at(1) == "sub") {
            c.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            c.SubresourceIndex = static_cast<UINT>(num(f.at(2)));
        } else {
            c.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            c.PlacedFootprint.Offset = num(f.at(2));
            c.PlacedFootprint.Footprint = {static_cast<DXGI_FORMAT>(num(f.at(3))), static_cast<UINT>(num(f.at(4))), static_cast<UINT>(num(f.at(5))),
                                           static_cast<UINT>(num(f.at(6))), static_cast<UINT>(num(f.at(7)))};
        }
        return c;
    }


    static std::vector<std::uint8_t> console_cs(const std::vector<std::uint32_t>& code, const std::vector<std::array<std::uint8_t, 20>>& layout,
                                                const std::vector<std::array<std::uint8_t, 20>>& used, std::uint32_t rsrc2,
                                                std::array<std::uint32_t, 3> threads, std::uint32_t vgprs, std::uint32_t sgprs) {
        std::vector<std::uint8_t> stage(0x40, 0);
        auto set32 = [](std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
            for (int i = 0; i < 4; ++i) {
                v[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
            }
        };
        set32(stage, 0x24, rsrc2);
        for (int i = 0; i < 3; ++i) {
            set32(stage, 0x28 + 4 * i, threads[i]);
        }
        return console_shader(0x60, code, layout, used, stage, vgprs, sgprs, {});
    }


    struct SigEntry {
        const char* name;
        std::uint32_t index, system, reg;
        std::uint8_t mask;
    };
    static std::vector<std::uint8_t> signature_part(const std::vector<SigEntry>& entries) {
        std::vector<std::uint8_t> out;
        auto put32 = [&](std::uint32_t x) {
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
            }
        };
        put32(static_cast<std::uint32_t>(entries.size()));
        put32(8);
        std::uint32_t names = 8 + 32 * static_cast<std::uint32_t>(entries.size());
        std::vector<std::uint32_t> name_at;
        std::string blob;
        for (const auto& e : entries) {
            name_at.push_back(names + static_cast<std::uint32_t>(blob.size()));
            blob += e.name;
            blob.push_back('\0');
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            put32(0);
            put32(name_at[i]);
            put32(e.index);
            put32(e.system);
            put32(3);
            put32(e.reg);
            out.push_back(e.mask);
            out.push_back(0);
            out.push_back(0);
            out.push_back(0);
            put32(0);
        }
        out.insert(out.end(), blob.begin(), blob.end());
        while (out.size() % 4) {
            out.push_back(0);
        }
        return out;
    }


    static std::vector<std::uint8_t> console_shader(std::size_t slot, const std::vector<std::uint32_t>& code,
                                                    const std::vector<std::array<std::uint8_t, 20>>& layout,
                                                    const std::vector<std::array<std::uint8_t, 20>>& used, const std::vector<std::uint8_t>& stage_data,
                                                    std::uint32_t vgprs, std::uint32_t sgprs,
                                                    const std::vector<std::pair<std::array<char, 4>, std::vector<std::uint8_t>>>& more) {
        auto put32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
            for (int i = 0; i < 4; ++i) {
                v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
            }
        };
        auto put64 = [](std::vector<std::uint8_t>& v, std::uint64_t x) {
            for (int i = 0; i < 8; ++i) {
                v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
            }
        };
        auto set64 = [](std::vector<std::uint8_t>& v, std::size_t at, std::uint64_t x) {
            for (int i = 0; i < 8; ++i) {
                v[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
            }
        };
        auto set32 = [](std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
            for (int i = 0; i < 4; ++i) {
                v[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
            }
        };

        std::vector<std::uint8_t> payload;
        put64(payload, 6);
        payload.resize(0x68, 0);
        for (std::size_t at : {0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x48, 0x60}) {
            set64(payload, at, 0xDEADBEEFDEADBEEFull);
        }
        std::size_t record = payload.size();
        set64(payload, slot, record);
        constexpr std::size_t RECORD = 0x54;
        payload.resize(record + RECORD, 0);
        payload.insert(payload.end(), stage_data.begin(), stage_data.end());
        std::size_t layout_at = payload.size();
        for (const auto& e : layout) {
            payload.insert(payload.end(), e.begin(), e.end());
        }
        std::size_t used_at = payload.size();
        for (const auto& e : used) {
            payload.insert(payload.end(), e.begin(), e.end());
        }
        while (payload.size() % 256) {
            payload.push_back(0);
        }
        std::size_t code_at = payload.size();
        for (std::uint32_t w : code) {
            put32(payload, w);
        }
        set64(payload, record + 0x00, code_at);
        set64(payload, record + 0x08, code.size() * 4);
        set32(payload, record + 0x14, 1u << 5);   
        (void)set32;
        set64(payload, record + 0x28, layout_at);
        set64(payload, record + 0x30, used_at);
        set32(payload, record + 0x38, static_cast<std::uint32_t>(layout.size()));
        set32(payload, record + 0x3c, static_cast<std::uint32_t>(used.size()));
        set32(payload, record + 0x50, vgprs | sgprs << 20);
        std::vector<std::uint8_t> fssm{'F', 'S', 'S', 'M'};
        put32(fssm, 0x101);
        put64(fssm, payload.size());
        fssm.insert(fssm.end(), payload.begin(), payload.end());

        std::vector<std::uint8_t> z{0x78, 0x01};
        for (std::size_t at = 0; at < fssm.size() || at == 0;) {
            std::size_t n = std::min<std::size_t>(fssm.size() - at, 65535);
            bool last = at + n == fssm.size();
            z.push_back(last ? 1 : 0);
            z.push_back(static_cast<std::uint8_t>(n));
            z.push_back(static_cast<std::uint8_t>(n >> 8));
            z.push_back(static_cast<std::uint8_t>(~n));
            z.push_back(static_cast<std::uint8_t>(~n >> 8));
            z.insert(z.end(), fssm.begin() + at, fssm.begin() + at + n);
            at += n;
            if (last) {
                break;
            }
        }
        std::uint32_t a = 1, b = 0;
        for (std::uint8_t c : fssm) {
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
        std::uint32_t adler = b << 16 | a;
        for (int i = 3; i >= 0; --i) {
            z.push_back(static_cast<std::uint8_t>(adler >> (8 * i)));
        }
        std::vector<std::uint8_t> xbch{'X', 'B', 'C', 'H'};
        put32(xbch, 3);
        put64(xbch, z.size());
        put64(xbch, fssm.size());
        xbch.insert(xbch.end(), z.begin(), z.end());

        std::vector<std::uint8_t> dxil(0x18, 0);
        dxil.insert(dxil.end(), {'X', 'D', 'A', 'T'});
        put32(dxil, 1);
        put32(dxil, 0x24);
        put32(dxil, 0x0d);
        put32(dxil, 0x30);
        put32(dxil, static_cast<std::uint32_t>(xbch.size()));
        dxil.insert(dxil.end(), xbch.begin(), xbch.end());

        std::vector<std::pair<std::array<char, 4>, std::vector<std::uint8_t>>> parts{{{'D', 'X', 'I', 'L'}, dxil}};
        parts.insert(parts.end(), more.begin(), more.end());
        std::vector<std::uint8_t> c{'D', 'X', 'B', 'C'};
        c.resize(20, 0);
        c.push_back(1);
        c.push_back(0);
        c.push_back(0);
        c.push_back(0);
        std::uint32_t table = 0x20 + 4 * static_cast<std::uint32_t>(parts.size());
        std::uint32_t total = table;
        for (const auto& [fourcc, data] : parts) {
            total += 8 + static_cast<std::uint32_t>(data.size());
        }
        put32(c, total);
        put32(c, static_cast<std::uint32_t>(parts.size()));
        std::uint32_t at = table;
        for (const auto& [fourcc, data] : parts) {
            put32(c, at);
            at += 8 + static_cast<std::uint32_t>(data.size());
        }
        for (const auto& [fourcc, data] : parts) {
            c.insert(c.end(), fourcc.begin(), fourcc.end());
            put32(c, static_cast<std::uint32_t>(data.size()));
            c.insert(c.end(), data.begin(), data.end());
        }
        return c;
    }


    static std::array<std::uint8_t, 20> entry(std::initializer_list<std::uint32_t> words, std::initializer_list<std::uint8_t> tail) {
        std::array<std::uint8_t, 20> e{};
        std::size_t at = 0;
        for (std::uint32_t w : words) {
            for (int i = 0; i < 4; ++i) {
                e[at++] = static_cast<std::uint8_t>(w >> (8 * i));
            }
        }
        for (std::uint8_t b : tail) {
            e[at++] = b;
        }
        return e;
    }

    void op(const std::vector<std::string>& t) {
        auto need = [&](size_t n) {
            if (t.size() != n) {
                throw Failure(t[0] + " takes " + std::to_string(n - 1) + " arguments");
            }
        };
        const std::string& o = t[0];
        if (o == "repeat") {
            if (t.size() < 3) {
                throw Failure("repeat <n> <operation...>");
            }
            repeat = num(t[1]);
            std::vector<std::string> rest(t.begin() + 2, t.end());
            RUNTIME_QUIET = repeat > 1;
            try {
                op(rest);
            } catch (...) {
                repeat = 1;
                RUNTIME_QUIET = false;
                throw;
            }
            repeat = 1;
            RUNTIME_QUIET = false;
            return;
        }
        if (o == "runtime") {
            backend.use_runtime(reinterpret_cast<void*>(&runtime_alloc), reinterpret_cast<void*>(&runtime_free), reinterpret_cast<void*>(&runtime_map));
        } else if (o == "configure_vm") {
            need(1);
            VirtualMemory vm{0, 1024, 0, 0, 0};
            HR hr = backend.configure_vm(&vm);
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            log << "  hr " << hr_text(hr) << " lowest_is_user " << (vm.lowest == reinterpret_cast<std::uint64_t>(si.lpMinimumApplicationAddress))
                << " highest_is_user " << (vm.highest == reinterpret_cast<std::uint64_t>(si.lpMaximumApplicationAddress)) << "\n";
        } else if (o == "device_iid") {
            need(1);
            log << "  same " << (std::memcmp(backend.device_iid(), &__uuidof(ID3D12Device), 16) == 0) << "\n";
        } else if (o == "create_device") {

            if (t.size() < 3 || t.size() > 5) {
                throw Failure("create_device <name> <iid> [version|none] [noout]");
            }
            std::uint32_t version = 0x00020004;
            bool with_version = true;
            if (t.size() > 3) {
                if (t[3] == "none") {
                    with_version = false;
                } else {
                    version = static_cast<std::uint32_t>(num(t[3]));
                }
            }
            bool with_out = !(t.size() > 4 && t[4] == "noout");
            void* out = nullptr;
            GUID id = iid(t[2]);
            HR hr = timed("create_device", [&] { return backend.create_device(nullptr, with_version ? &version : nullptr, &id, with_out ? &out : nullptr); });
            if (hr == S_OK && id == __uuidof(ID3D12Device11)) {
                newer = true;
            }
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[1]) << "\n";
        } else if (o == "query") {
            need(4);
            void* obj = object(t[1]);
            GUID id = iid(t[2]);
            void* out = nullptr;
            HR hr = timed("query", [&] { return slot<HR(__stdcall*)(void*, const GUID*, void**)>(obj, 0)(obj, &id, &out); });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[3]) << "\n";
        } else if (o == "addref" || o == "release") {
            need(2);
            void* obj = object(t[1]);
            ULONG n = timed(o == "addref" ? "addref" : "release", [&] { return slot<ULONG(__stdcall*)(void*)>(obj, o == "addref" ? 1 : 2)(obj); });
            log << "  count " << n << "\n";
            if (o == "release" && n == 0) {
                gone(obj);
            }
        } else if (o == "node_count") {
            need(2);
            void* obj = object(t[1]);
            log << "  nodes " << slot<UINT(__stdcall*)(void*)>(obj, 7)(obj) << "\n";
        } else if (o == "increment_size") {
            need(3);
            void* obj = object(t[1]);
            UINT kind = static_cast<UINT>(num(t[2]));
            UINT n = timed("increment_size", [&] { return slot<UINT(__stdcall*)(void*, UINT)>(obj, 15)(obj, kind); });
            log << "  bytes " << n << "\n";
        } else if (o == "feature") {


            if (t.size() < 4) {
                throw Failure("feature <device> <feature> <size> [dwords...]");
            }
            void* obj = object(t[1]);
            UINT feature = static_cast<UINT>(num(t[2]));
            UINT size = static_cast<UINT>(num(t[3]));
            std::vector<std::uint8_t> data(std::max<UINT>(size, 4), 0xcd);
            for (size_t i = 4; i < t.size(); ++i) {
                auto v = static_cast<std::uint32_t>(num(t[i]));
                std::memcpy(data.data() + 4 * (i - 4), &v, 4);
            }
            HR hr = timed("feature", [&] { return slot<HR(__stdcall*)(void*, UINT, void*, UINT)>(obj, 13)(obj, feature, data.data(), size); });
            log << "  hr " << hr_text(hr) << " data";
            for (UINT i = 0; i < size; ++i) {
                char b[4];
                std::snprintf(b, sizeof b, "%02x", data[i]);
                log << (i % 4 == 0 ? " " : "") << b;
            }
            log << "\n";
        } else if (o == "feature_levels") {


            if (t.size() < 3) {
                throw Failure("feature_levels <device> <level>...");
            }
            void* obj = object(t[1]);
            std::vector<std::uint32_t> levels;
            for (size_t i = 2; i < t.size(); ++i) {
                levels.push_back(static_cast<std::uint32_t>(num(t[i])));
            }
            struct {
                std::uint32_t count;
                const std::uint32_t* levels;
                std::uint32_t highest;
            } data{static_cast<std::uint32_t>(levels.size()), levels.data(), 0xcdcdcdcd};
            static_assert(sizeof(data) == 24);
            HR hr = slot<HR(__stdcall*)(void*, UINT, void*, UINT)>(obj, 13)(obj, 2, &data, sizeof data);
            log << "  hr " << hr_text(hr) << " highest " << hex(data.highest) << "\n";
        } else if (o == "console") {
            need(2);
            backend.set_console(t[1] == "S" ? 1 : 0);
        } else if (o == "get_object") {


            need(4);
            void* obj = object(t[1]);
            void* out = nullptr;
            HR hr = slot<HR(__stdcall*)(void*, void**)>(obj, static_cast<unsigned>(num(t[2])))(obj, &out);
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[3]) << "\n";
        } else if (o == "get_parent") {


            need(5);
            void* obj = object(t[1]);
            GUID id = iid(t[3]);
            void* out = nullptr;
            HR hr = slot<HR(__stdcall*)(void*, const GUID*, void**)>(obj, static_cast<unsigned>(num(t[2])))(obj, &id, &out);
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[4]) << "\n";
        } else if (o == "get_indexed") {


            need(5);
            void* obj = object(t[1]);
            void* out = nullptr;
            HR hr = slot<HR(__stdcall*)(void*, UINT, void**)>(obj, static_cast<unsigned>(num(t[2])))(obj, static_cast<UINT>(num(t[3])), &out);
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[4]) << "\n";
        } else if (o == "adapter_desc") {
            need(2);
            void* obj = object(t[1]);
            struct {
                wchar_t name[128];
                std::uint32_t vendor, device, subsystem, revision;
                std::size_t video, system, shared;
                std::uint32_t luid[2];
            } d{};
            static_assert(sizeof(d) == 0x130);
            HR hr = timed("adapter_desc", [&] { return slot<HR(__stdcall*)(void*, void*)>(obj, 8)(obj, &d); });
            std::string name;
            for (int i = 0; i < 128 && d.name[i]; ++i) {
                name.push_back(static_cast<char>(d.name[i]));
            }
            log << "  hr " << hr_text(hr) << " name '" << name << "' vendor " << hex(d.vendor) << " device " << d.device << " video " << hex(d.video)
                << " system " << hex(d.system) << " shared " << hex(d.shared) << " luid " << d.luid[0] << "," << d.luid[1] << "\n";
        } else if (o == "modes") {

            need(3);
            void* obj = object(t[1]);
            UINT count = t[2] == "null" || t[2] == "nocount" ? 0 : static_cast<UINT>(num(t[2]));
            std::vector<std::uint32_t> modes(8 * 4, 0xcdcdcdcd);
            HR hr = slot<HR(__stdcall*)(void*, UINT, UINT, UINT*, void*)>(obj, 21)(obj, 28, 0, t[2] == "nocount" ? nullptr : &count,
                                                                                       t[2] == "null" ? nullptr : modes.data());
            log << "  hr " << hr_text(hr) << " count " << count << " first";
            for (int i = 0; i < 8; ++i) {
                log << " " << modes[i];
            }
            log << "\n";
        } else if (o == "set_private") {

            need(4);
            void* obj = object(t[1]);
            GUID id = iid(t[2]);
            std::vector<std::uint8_t> bytes;
            if (t[3] != "null") {
                for (size_t i = 0; i + 1 < t[3].size(); i += 2) {
                    bytes.push_back(static_cast<std::uint8_t>(std::stoul(t[3].substr(i, 2), nullptr, 16)));
                }
            }
            HR hr = timed("set_private", [&] {
                return slot<HR(__stdcall*)(void*, const GUID*, UINT, const void*)>(obj, 4)(obj, &id, static_cast<UINT>(bytes.size()),
                                                                                    t[3] == "null" ? nullptr : bytes.data());
            });
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "get_private") {

            need(4);
            void* obj = object(t[1]);
            GUID id = iid(t[2]);
            UINT size = t[3] == "null" ? 0 : static_cast<UINT>(num(t[3]));
            std::vector<std::uint8_t> data(64, 0xcd);
            HR hr = timed("get_private", [&] {
                return slot<HR(__stdcall*)(void*, const GUID*, UINT*, void*)>(obj, 3)(obj, &id, &size, t[3] == "null" ? nullptr : data.data());
            });
            log << "  hr " << hr_text(hr) << " size " << size << " data ";
            for (UINT i = 0; i < std::min<UINT>(size, 16); ++i) {
                char b[4];
                std::snprintf(b, sizeof b, "%02x", data[i]);
                log << b;
            }
            log << "\n";
        } else if (o == "set_name") {
            need(2);
            void* obj = object(t[1]);
            HR hr = slot<HR(__stdcall*)(void*, const wchar_t*)>(obj, 6)(obj, L"a name");
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "missing") {

            need(3);
            void* obj = object(t[1]);
            HR hr = slot<HR(__stdcall*)()>(obj, static_cast<unsigned>(num(t[2])))();
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "create_fence") {

            if (t.size() < 4 || t.size() > 6) {
                throw Failure("create_fence <device> <name> <initial> [flags] [iid|noout]");
            }
            void* dev = object(t[1]);
            std::uint64_t initial = num(t[3]);
            UINT flags = t.size() > 4 ? static_cast<UINT>(num(t[4])) : 0;
            bool with_out = !(t.size() > 5 && t[5] == "noout");
            GUID id = t.size() > 5 && t[5] != "noout" ? iid(t[5]) : __uuidof(ID3D12Fence);
            void* out = nullptr;
            HR hr = timed("create_fence", [&] {
                return slot<HR(__stdcall*)(void*, std::uint64_t, UINT, const GUID*, void**)>(dev, 36)(dev, initial, flags, &id, with_out ? &out : nullptr);
            });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "fence_completed") {
            need(2);
            void* f = object(t[1]);
            std::uint64_t v = timed("fence_completed", [&] { return slot<std::uint64_t(__stdcall*)(void*)>(f, 8)(f); });
            log << "  value " << v << "\n";
        } else if (o == "fence_signal") {


            need(3);
            void* f = object(t[1]);
            auto completed = slot<std::uint64_t(__stdcall*)(void*)>(f, 8);
            auto signal = slot<HR(__stdcall*)(void*, std::uint64_t)>(f, 10);
            bool next = t[2] == "+1";
            std::uint64_t v = next ? 0 : num(t[2]);
            HR hr = timed("fence_signal", [&] { return signal(f, next ? completed(f) + 1 : v); });
            log << "  hr " << hr_text(hr) << " value " << completed(f) << "\n";
        } else if (o == "fence_event") {

            need(4);
            void* f = object(t[1]);
            std::uint64_t v = num(t[2]);
            HANDLE e = t[3] == "null" ? nullptr : event(t[3]);
            HR hr = timed("fence_event", [&] { return slot<HR(__stdcall*)(void*, std::uint64_t, HANDLE)>(f, 9)(f, v, e); });
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "fill") {

            need(5);
            auto* p = reinterpret_cast<std::uint8_t*>(address(t[1]));
            std::uint64_t n = num(t[2]), m = num(t[3]), a = num(t[4]);
            for (std::uint64_t i = 0; i < n; ++i) {
                p[i] = static_cast<std::uint8_t>(i * m + a);
            }
        } else if (o == "hash") {

            need(3);
            const auto* p = reinterpret_cast<const std::uint8_t*>(address(t[1]));
            std::uint64_t n = num(t[2]), h = 1469598103934665603ull;
            for (std::uint64_t i = 0; i < n; ++i) {
                h = (h ^ p[i]) * 1099511628211ull;
            }
            log << "  hash " << hex(h) << "\n";
        } else if (o == "copy_tex") {


            if (t.size() < 7) {
                throw Failure("copy_tex <list> <dst> <x> <y> <z> <src> [box:..] [x]");
            }
            void* l = object(t[1]);
            auto dst = copy_location(t[2]);
            auto src = copy_location(t[6]);
            D3D12_BOX box{};
            bool with_box = false, x_form = false;
            for (size_t i = 7; i < t.size(); ++i) {
                if (t[i].rfind("box:", 0) == 0) {
                    auto w = words(std::string("w:") + t[i].substr(4), 6);
                    box = {w[0], w[1], w[2], w[3], w[4], w[5]};
                    with_box = true;
                } else if (t[i] == "x") {
                    x_form = true;
                }
            }
            UINT x = static_cast<UINT>(num(t[3])), y = static_cast<UINT>(num(t[4])), z = static_cast<UINT>(num(t[5]));
            auto copy_x = slot<void(__stdcall*)(void*, const void*, UINT, UINT, UINT, const void*, const void*, UINT)>(l, list_slot(l, 83));
            auto copy = slot<void(__stdcall*)(void*, const void*, UINT, UINT, UINT, const void*, const void*)>(l, 16);
            timed("copy_tex", [&] {
                if (x_form) {
                    copy_x(l, &dst, x, y, z, &src, with_box ? &box : nullptr, 0);
                } else {
                    copy(l, &dst, x, y, z, &src, with_box ? &box : nullptr);
                }
                return 0;
            });
        } else if (o == "clear_rtv") {

            if (t.size() < 4) {
                throw Failure("clear_rtv <list> <handle> <r,g,b,a> [l,t,r,b]");
            }
            void* l = object(t[1]);
            std::uint64_t h = handle(t[2]);
            float c[4] = {};
            {
                std::istringstream in(t[3]);
                std::string x;
                for (int i = 0; i < 4 && std::getline(in, x, ','); ++i) {
                    c[i] = std::stof(x);
                }
            }
            D3D12_RECT r{};
            bool rect = t.size() > 4;
            if (rect) {
                auto w = words(std::string("w:") + t[4], 4);
                r = {static_cast<LONG>(w[0]), static_cast<LONG>(w[1]), static_cast<LONG>(w[2]), static_cast<LONG>(w[3])};
            }
            auto clear = slot<void(__stdcall*)(void*, std::uint64_t, const float*, UINT, const D3D12_RECT*)>(l, 52);
            timed("clear_rtv", [&] {
                clear(l, h, c, rect ? 1 : 0, rect ? &r : nullptr);
                return 0;
            });
        } else if (o == "clear_dsv") {

            need(6);
            void* l = object(t[1]);
            auto clear = slot<void(__stdcall*)(void*, std::uint64_t, UINT, float, std::uint8_t, UINT, const D3D12_RECT*)>(l, 51);
            std::uint64_t h = handle(t[2]);
            UINT flags = static_cast<UINT>(num(t[3]));
            float depth = std::stof(t[4]);
            auto stencil = static_cast<std::uint8_t>(num(t[5]));
            timed("clear_dsv", [&] {
                clear(l, h, flags, depth, stencil, 0, nullptr);
                return 0;
            });
        } else if (o == "resolve") {

            need(7);
            void* l = object(t[1]);
            auto resolve = slot<void(__stdcall*)(void*, void*, UINT, void*, UINT, UINT)>(l, 19);
            void* dst = object(t[2]);
            void* src = object(t[4]);
            auto ds = static_cast<UINT>(num(t[3]));
            auto ss = static_cast<UINT>(num(t[5]));
            auto format = static_cast<UINT>(num(t[6]));
            timed("resolve", [&] {
                resolve(l, dst, ds, src, ss, format);
                return 0;
            });
        } else if (o == "submit_cycle") {


            need(10);
            void* q = object(t[1]);
            void* l = object(t[2]);
            void* a = object(t[3]);
            void* f = object(t[4]);
            std::uint64_t copies = num(t[5]), bytes = num(t[6]);
            void* dst = object(t[7]);
            void* src = object(t[8]);
            std::uint64_t values = address(t[9]);
            auto reset = slot<HR(__stdcall*)(void*, void*, void*)>(l, 10);
            auto copy = slot<void(__stdcall*)(void*, void*, std::uint64_t, void*, std::uint64_t, std::uint64_t)>(l, 15);
            auto write = slot<void(__stdcall*)(void*, std::uint64_t, UINT, UINT, UINT)>(l, list_slot(l, 69));
            auto close = slot<HR(__stdcall*)(void*)>(l, 9);
            auto execute = slot<void(__stdcall*)(void*, UINT, void* const*)>(q, 10);
            auto signal = slot<HR(__stdcall*)(void*, void*, std::uint64_t)>(q, queue_slot(q, 14));
            auto completed = slot<std::uint64_t(__stdcall*)(void*)>(f, 8);
            auto wait = slot<HR(__stdcall*)(void*, std::uint64_t, HANDLE)>(f, 9);
            std::uint32_t counter = 0;
            HR hr = timed("submit_cycle", [&] {
                reset(l, a, nullptr);
                for (std::uint64_t k = 0; k < copies; ++k) {
                    copy(l, dst, k * bytes, src, k * bytes, bytes);
                }
                write(l, values, ++counter, 0, 0);
                close(l);
                execute(q, 1, &l);
                std::uint64_t v = completed(f) + 1;
                signal(q, f, v);
                return wait(f, v, nullptr);
            });
            log << "  hr " << hr_text(hr) << " value " << *reinterpret_cast<std::uint32_t*>(values) << "\n";
        } else if (o == "shader_cache") {


            if (t.size() < 2 || t.size() > 3) {
                throw Failure("shader_cache <dir> [build-file]");
            }
            fs::path dir = fs::absolute(t[1]);
            fs::create_directories(dir);
            backend.use_shader_cache(dir.wstring().c_str());
            if (t.size() == 3) {
                if (backend.shader_build) {


                    build_file = t[2];
                } else if (backend.use_shader_build) {
                    std::uint8_t build[32] = {};
                    std::ifstream(t[2], std::ios::binary).read(reinterpret_cast<char*>(build), 32);
                    backend.use_shader_build(build);
                }
            }
        } else if (o == "pipeline_cache") {


            if (t.size() != 2 && !(t.size() == 3 && t[2] == "fresh")) {
                throw Failure("pipeline_cache <dir> [fresh]");
            }
            std::string text = t[1];
            if (auto at = text.find("{backend}"); at != std::string::npos) {
                text.replace(at, 9, backend.name);
            }
            fs::path dir = fs::absolute(text);
            if (t.size() == 3) {
                fs::remove_all(dir);
            }
            fs::create_directories(dir);
            backend.use_pipeline_cache(dir.wstring().c_str());
        } else if (o == "pipeline_save") {
            backend.save_pipeline_cache();
        } else if (o == "sleep") {

            need(2);
            Sleep(static_cast<DWORD>(num(t[1])));
        } else if (o == "sweep_counts") {

            if (backend.sweep_counts) {
                std::uint64_t c[3] = {};
                backend.sweep_counts(c);
                log << "  windows " << c[0] << " staging " << c[1] << " textures " << c[2] << "\n";
            }
        } else if (o == "pipeline_counts") {

            if (backend.pipeline_counts) {
                std::uint64_t c[3] = {};
                backend.pipeline_counts(c);
                log << "  made " << c[0] << " loaded " << c[1] << " early " << c[2] << "\n";
            }
        } else if (o == "cs_store") {


            need(3);
            auto value = static_cast<std::uint32_t>(num(t[2]));
            std::vector<std::uint32_t> code = {
                0xBE8203C1,               
                0xBE8303FF, 0x31014FAC,   
                0x34020082,               
                0x7E0402FF, value,        
                0xE0701000, 0x80000201,   
                0xBF810000,               
            };

            std::vector<std::array<std::uint8_t, 20>> layout = {entry({1, 0, 2, 0}, {0, 0, 0, 0})};

            std::vector<std::array<std::uint8_t, 20>> used = {entry({0x0103, 0, 0, 0}, {4, 0, 0, 0})};
            shaders[t[1]] = console_cs(code, layout, used, 2u << 1, {64, 1, 1}, 4, 8);
            log << "  bytes " << shaders[t[1]].size() << "\n";
        } else if (o == "cs_const" || o == "cs_table" || o == "cs_cbv") {


            need(o == "cs_table" ? 3 : 2);
            constexpr std::uint32_t V3 = 0x31014FAC;   
            std::vector<std::uint32_t> code;
            std::vector<std::array<std::uint8_t, 20>> layout, used;
            std::uint32_t user = 0;
            if (o == "cs_const") {
                code = {
                    0xBE840300, 0xBE850301, 0xBE8603C1, 0xBE8703FF, V3,   
                    0x34020082,                                           
                    0x7E040202,                                           
                    0xE0701000, 0x80010201,                               
                    0xBF810000,                                           
                };
                layout = {entry({1, 0, 2, 0}, {0, 0, 0, 0}), entry({0, 0, 1, 0}, {2, 1, 0, 0})};
                used = {entry({0x0103, 0, 0, 0}, {4, 0, 0, 0})};
                user = 3;
            } else if (o == "cs_table") {
                auto value = static_cast<std::uint32_t>(num(t[2]));
                code = {
                    0xBE810380,               
                    0xF4080100, 0xFA000000,   
                    0xBF8CC07F,               
                    0x34020082,               
                    0x7E0402FF, value,        
                    0xE0701000, 0x80010201,   
                    0xBF810000,               
                };
                layout = {entry({3, 0, 1, 0}, {0, 0, 0, 0})};
                used = {entry({0x0303, 0, 0, 0}, {4, 0, 0, 0})};
                user = 1;
            } else {
                code = {
                    0xBE830380,               
                    0xF4080101, 0xFA000000,   
                    0xBF8CC07F,               
                    0xF4200202, 0xFA000000,   
                    0xBF8CC07F,               
                    0xBE8C0300, 0xBE8D0301, 0xBE8E03C1, 0xBE8F03FF, V3,   
                    0x34020082,               
                    0x7E040208,               
                    0xE0701000, 0x80030201,   
                    0xBF810000,               
                };
                layout = {entry({1, 0, 2, 0}, {0, 0, 0, 0}), entry({3, 0, 1, 0}, {2, 1, 0, 0})};
                used = {entry({0x0103, 0, 0, 0}, {4, 0, 0, 0}), entry({0x0302, 0, 0, 0}, {4, 2, 1, 0})};
                user = 3;
            }
            shaders[t[1]] = console_cs(code, layout, used, user << 1, {64, 1, 1}, 4, 16);
            log << "  bytes " << shaders[t[1]].size() << "\n";
        } else if (o == "vs_fullscreen") {


            need(2);
            std::vector<std::uint32_t> code = {
                0x36020081,               
                0x2C040081,               
                0x7E020D01,               
                0x7E040D02,               
                0x100202F6, 0x060202F3,   
                0x100404F6, 0x060404F3,   
                0x7E060280,               
                0x7E0802F2,               
                0xF80008CF, 0x04030201,   
                0xBF810000,               
            };
            std::vector<std::uint8_t> stage(0x190, 0);
            shaders[t[1]] = console_shader(0x28, code, {}, {}, stage, 8, 8,
                                           {{{'O', 'S', 'G', '1'}, signature_part({{"SV_Position", 0, 1, 0, 0xF}})}});
            log << "  bytes " << shaders[t[1]].size() << "\n";
        } else if (o == "vs_input") {


            need(2);
            std::vector<std::uint32_t> code = {
                0xF80008CF, 0x07060504,   
                0xBF810000,               
            };
            std::vector<std::uint8_t> stage(0x190, 0);

            stage[0x24] = 6;
            stage[0x25] = 0;
            stage[0x26] = 0xF;
            stage[0x27] = 4;
            stage[0x28] = 3;
            stage[0xc4] = 1;
            shaders[t[1]] = console_shader(0x28, code, {}, {}, stage, 8, 8,
                                           {{{'I', 'S', 'G', '1'}, signature_part({{"POSITION", 0, 0, 0, 0xF}})},
                                            {{'O', 'S', 'G', '1'}, signature_part({{"SV_Position", 0, 1, 0, 0xF}})}});
            log << "  bytes " << shaders[t[1]].size() << "\n";
        } else if (o == "ps_color") {


            need(6);
            auto f = [&](const std::string& x) {
                float v = std::stof(x);
                std::uint32_t u;
                std::memcpy(&u, &v, 4);
                return u;
            };
            std::vector<std::uint32_t> code = {
                0x7E0002FF, f(t[2]),      
                0x7E0202FF, f(t[3]),      
                0x7E0402FF, f(t[4]),      
                0x7E0602FF, f(t[5]),      
                0xF800180F, 0x03020100,   
                0xBF810000,               
            };
            std::vector<std::uint8_t> stage(0x14c + 8 * 404 + 16, 0);
            for (int mrt = 0; mrt < 8; ++mrt) {
                std::size_t at = 0x14c + mrt * 404;
                for (int i = 0; i < 4; ++i) {
                    stage[at + i] = 0xFF;
                }
            }
            shaders[t[1]] = console_shader(0x30, code, {}, {}, stage, 8, 8,
                                           {{{'O', 'S', 'G', '1'}, signature_part({{"SV_Target", 0, 64, 0, 0xF}})}});
            log << "  bytes " << shaders[t[1]].size() << "\n";
        } else if (o == "graphics_pipeline") {


            if (t.size() < 6) {
                throw Failure("graphics_pipeline <device> <name> <vs> <ps> <format> [cull=N] [topology=N]");
            }
            void* dev = object(t[1]);
            auto& vs = shader_named(t[3]);
            auto& ps = shader_named(t[4]);
            UINT format = static_cast<UINT>(num(t[5]));
            UINT cull = 1, topology = 3, depth_format = 0;
            bool input = false;
            for (size_t i = 6; i < t.size(); ++i) {
                if (t[i].rfind("cull=", 0) == 0) {
                    cull = static_cast<UINT>(num(t[i].substr(5)));
                } else if (t[i].rfind("topology=", 0) == 0) {
                    topology = static_cast<UINT>(num(t[i].substr(9)));
                } else if (t[i].rfind("depth=", 0) == 0) {
                    depth_format = static_cast<UINT>(num(t[i].substr(6)));
                } else if (t[i] == "input") {
                    input = true;
                }
            }
            std::vector<std::uint8_t> stream;
            auto sub = [&](std::uint32_t kind, const void* data, std::size_t bytes, std::size_t size, std::size_t data_at) {
                std::size_t at = stream.size();
                stream.resize(at + size, 0);
                std::memcpy(stream.data() + at, &kind, 4);
                std::memcpy(stream.data() + at + data_at, data, bytes);
            };
            struct Code {
                const void* p;
                std::size_t n;
            } vcode{vs.data(), vs.size()}, pcode{ps.data(), ps.size()};

            bool mesh = std::find(t.begin() + 6, t.end(), "mesh") != t.end();
            sub(mesh ? 24 : 1, &vcode, sizeof vcode, 24, 8);
            sub(2, &pcode, sizeof pcode, 24, 8);
            D3D12_RASTERIZER_DESC r{};
            r.FillMode = D3D12_FILL_MODE_SOLID;
            r.CullMode = static_cast<D3D12_CULL_MODE>(cull);
            r.DepthClipEnable = TRUE;
            sub(10, &r, sizeof r, 48, 4);
            D3D12_DEPTH_STENCIL_DESC ds{};
            if (depth_format) {

                ds.DepthEnable = TRUE;
                ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            }
            sub(11, &ds, sizeof ds, 56, 4);
            if (depth_format) {
                sub(16, &depth_format, 4, 8, 4);
            }

            static const D3D12_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
            if (input) {
                D3D12_INPUT_LAYOUT_DESC layout{&element, 1};
                sub(12, &layout, sizeof layout, 24, 8);
            }
            sub(14, &topology, 4, 8, 4);
            D3D12_RT_FORMAT_ARRAY rts{};
            rts.NumRenderTargets = 1;
            rts.RTFormats[0] = static_cast<DXGI_FORMAT>(format);
            sub(15, &rts, sizeof rts, 40, 4);
            DXGI_SAMPLE_DESC sd{1, 0};
            sub(17, &sd, sizeof sd, 16, 4);
            struct Desc {
                std::size_t size;
                const void* stream;
            } desc{stream.size(), stream.data()};
            GUID id = __uuidof(ID3D12PipelineState);
            void* out = nullptr;
            HR hr = timed("graphics_pipeline", [&] {
                if (out && repeat > 1) {
                    slot<ULONG(__stdcall*)(void*)>(out, 2)(out);
                    out = nullptr;
                }
                return slot<HR(__stdcall*)(void*, const Desc*, const GUID*, void**)>(dev, 86)(dev, &desc, &id, &out);
            });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "targets") {

            if (t.size() < 3) {
                throw Failure("targets <list> <handle|none> [depth]");
            }
            void* l = object(t[1]);
            std::vector<std::uint64_t> handles;
            if (t[2] != "none") {
                std::istringstream in(t[2]);
                std::string x;
                while (std::getline(in, x, ',')) {
                    handles.push_back(handle(x));
                }
            }
            std::uint64_t depth = t.size() > 3 ? handle(t[3]) : 0;
            timed("targets", [&] {
                slot<void(__stdcall*)(void*, UINT, const std::uint64_t*, BOOL, const std::uint64_t*)>(l, 50)(
                    l, static_cast<UINT>(handles.size()), handles.empty() ? nullptr : handles.data(), FALSE, depth ? &depth : nullptr);
                return 0;
            });
        } else if (o == "execute_indirect") {


            if (t.size() != 6 && t.size() != 8) {
                throw Failure("execute_indirect <list> <signature> <max> <arguments> <offset> [<count> <offset>]");
            }
            void* l = object(t[1]);
            void* sig = object(t[2]);
            auto max = static_cast<UINT>(num(t[3]));
            void* args = object(t[4]);
            std::uint64_t off = num(t[5]);
            void* count = t.size() == 8 ? object(t[6]) : nullptr;
            std::uint64_t count_off = t.size() == 8 ? num(t[7]) : 0;
            timed("execute_indirect", [&] {
                slot<void(__stdcall*)(void*, void*, UINT, void*, std::uint64_t, void*, std::uint64_t)>(l, list_slot(l, 59))(l, sig, max, args, off, count,
                                                                                                                         count_off);
                return 0;
            });
        } else if (o == "fill_u32") {

            if (t.size() < 3) {
                throw Failure("fill_u32 <address> <value>...");
            }
            auto* p = reinterpret_cast<std::uint32_t*>(address(t[1]));
            for (size_t i = 2; i < t.size(); ++i) {
                p[i - 2] = static_cast<std::uint32_t>(num(t[i]));
            }
        } else if (o == "vertex_buffer") {

            need(6);
            void* l = object(t[1]);
            struct View {
                std::uint64_t address;
                UINT size, stride;
            } v{address(t[3]), static_cast<UINT>(num(t[4])), static_cast<UINT>(num(t[5]))};
            auto start = static_cast<UINT>(num(t[2]));
            timed("vertex_buffer", [&] {
                slot<void(__stdcall*)(void*, UINT, UINT, const View*)>(l, 48)(l, start, 1, &v);
                return 0;
            });
        } else if (o == "index_buffer") {

            need(5);
            void* l = object(t[1]);
            struct View {
                std::uint64_t address;
                UINT size, format;
            } v{address(t[2]), static_cast<UINT>(num(t[3])), static_cast<UINT>(num(t[4]))};
            timed("index_buffer", [&] {
                slot<void(__stdcall*)(void*, const View*)>(l, 47)(l, &v);
                return 0;
            });
        } else if (o == "draw_indexed") {

            if (t.size() < 4) {
                throw Failure("draw_indexed <list> <indices> <instances> [first] [base]");
            }
            void* l = object(t[1]);
            auto n = static_cast<UINT>(num(t[2])), k = static_cast<UINT>(num(t[3]));
            UINT first = t.size() > 4 ? static_cast<UINT>(num(t[4])) : 0;
            INT base = t.size() > 5 ? static_cast<INT>(std::stoll(t[5])) : 0;
            timed("draw_indexed", [&] {
                slot<void(__stdcall*)(void*, UINT, UINT, UINT, INT, UINT)>(l, 13)(l, n, k, first, base, 0);
                return 0;
            });
        } else if (o == "fill_floats") {

            if (t.size() < 3) {
                throw Failure("fill_floats <address> <float>...");
            }
            auto* p = reinterpret_cast<float*>(address(t[1]));
            for (size_t i = 2; i < t.size(); ++i) {
                p[i - 2] = std::stof(t[i]);
            }
        } else if (o == "fill_u16") {

            if (t.size() < 3) {
                throw Failure("fill_u16 <address> <value>...");
            }
            auto* p = reinterpret_cast<std::uint16_t*>(address(t[1]));
            for (size_t i = 2; i < t.size(); ++i) {
                p[i - 2] = static_cast<std::uint16_t>(num(t[i]));
            }
        } else if (o == "viewport") {

            need(6);
            void* l = object(t[1]);
            D3D12_VIEWPORT v{std::stof(t[2]), std::stof(t[3]), std::stof(t[4]), std::stof(t[5]), 0.0f, 1.0f};
            timed("viewport", [&] {
                slot<void(__stdcall*)(void*, UINT, const D3D12_VIEWPORT*)>(l, 21)(l, 1, &v);
                return 0;
            });
        } else if (o == "scissor") {

            need(6);
            void* l = object(t[1]);
            D3D12_RECT r{static_cast<LONG>(num(t[2])), static_cast<LONG>(num(t[3])), static_cast<LONG>(num(t[4])), static_cast<LONG>(num(t[5]))};
            timed("scissor", [&] {
                slot<void(__stdcall*)(void*, UINT, const D3D12_RECT*)>(l, 22)(l, 1, &r);
                return 0;
            });
        } else if (o == "topology") {
            need(3);
            void* l = object(t[1]);
            auto k = static_cast<UINT>(num(t[2]));
            timed("topology", [&] {
                slot<void(__stdcall*)(void*, UINT)>(l, 20)(l, k);
                return 0;
            });
        } else if (o == "draw") {

            if (t.size() < 4) {
                throw Failure("draw <list> <vertices> <instances> [first] [first instance]");
            }
            void* l = object(t[1]);
            auto v = static_cast<UINT>(num(t[2])), n = static_cast<UINT>(num(t[3]));
            UINT first = t.size() > 4 ? static_cast<UINT>(num(t[4])) : 0, first_i = t.size() > 5 ? static_cast<UINT>(num(t[5])) : 0;
            timed("draw", [&] {
                slot<void(__stdcall*)(void*, UINT, UINT, UINT, UINT)>(l, 12)(l, v, n, first, first_i);
                return 0;
            });
        } else if (o == "compute_table" || o == "graphics_table") {

            need(4);
            void* l = object(t[1]);
            auto root = static_cast<UINT>(num(t[2]));
            std::uint64_t h = handle(t[3]);
            int s = o == "compute_table" ? 35 : 36;
            timed(o.c_str(), [&] {
                slot<void(__stdcall*)(void*, UINT, std::uint64_t)>(l, s)(l, root, h);
                return 0;
            });
        } else if (o == "compute_constant" || o == "graphics_constant") {

            need(5);
            void* l = object(t[1]);
            auto root = static_cast<UINT>(num(t[2])), value = static_cast<UINT>(num(t[3])), offset = static_cast<UINT>(num(t[4]));
            int s = o == "compute_constant" ? 37 : 38;
            timed(o.c_str(), [&] {
                slot<void(__stdcall*)(void*, UINT, UINT, UINT)>(l, s)(l, root, value, offset);
                return 0;
            });
        } else if (o == "dispatch_cycle") {


            need(9);
            void* q = object(t[1]);
            void* l = object(t[2]);
            void* a = object(t[3]);
            void* f = object(t[4]);
            void* heap = object(t[5]);
            void* pso = object(t[6]);
            std::uint64_t uav = address(t[7]);
            std::uint64_t n = num(t[8]);
            auto reset = slot<HR(__stdcall*)(void*, void*, void*)>(l, 10);
            auto heaps = slot<void(__stdcall*)(void*, UINT, void* const*)>(l, 32);
            auto set = slot<void(__stdcall*)(void*, void*)>(l, 25);
            auto view = slot<void(__stdcall*)(void*, UINT, std::uint64_t)>(l, 45);
            auto run = slot<void(__stdcall*)(void*, UINT, UINT, UINT)>(l, 14);
            auto close = slot<HR(__stdcall*)(void*)>(l, 9);
            auto execute = slot<void(__stdcall*)(void*, UINT, void* const*)>(q, 10);
            auto signal = slot<HR(__stdcall*)(void*, void*, std::uint64_t)>(q, queue_slot(q, 14));
            auto completed = slot<std::uint64_t(__stdcall*)(void*)>(f, 8);
            auto wait = slot<HR(__stdcall*)(void*, std::uint64_t, HANDLE)>(f, 9);
            HR hr = timed("dispatch_cycle", [&] {
                reset(l, a, nullptr);
                heaps(l, 1, &heap);
                set(l, pso);
                view(l, 0, uav);
                for (std::uint64_t k = 0; k < n; ++k) {
                    run(l, 1, 1, 1);
                }
                close(l);
                execute(q, 1, &l);
                std::uint64_t v = completed(f) + 1;
                signal(q, f, v);
                return wait(f, v, nullptr);
            });
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "draw_cycle" || o == "mesh_cycle") {


            need(10);
            void* q = object(t[1]);
            void* l = object(t[2]);
            void* a = object(t[3]);
            void* f = object(t[4]);
            void* heap = object(t[5]);
            void* pso = object(t[6]);
            std::uint64_t rtv = handle(t[7]);
            float size = std::stof(t[8]);
            std::uint64_t n = num(t[9]);
            auto reset = slot<HR(__stdcall*)(void*, void*, void*)>(l, 10);
            auto heaps = slot<void(__stdcall*)(void*, UINT, void* const*)>(l, 32);
            auto set = slot<void(__stdcall*)(void*, void*)>(l, 25);
            auto targets = slot<void(__stdcall*)(void*, UINT, const std::uint64_t*, BOOL, const std::uint64_t*)>(l, 50);
            auto viewports = slot<void(__stdcall*)(void*, UINT, const D3D12_VIEWPORT*)>(l, 21);
            auto scissors = slot<void(__stdcall*)(void*, UINT, const D3D12_RECT*)>(l, 22);
            auto topology = slot<void(__stdcall*)(void*, UINT)>(l, 20);
            auto draw = slot<void(__stdcall*)(void*, UINT, UINT, UINT, UINT)>(l, 12);
            auto mesh = slot<void(__stdcall*)(void*, UINT, UINT, UINT)>(l, list_slot(l, 127));
            bool meshes = o == "mesh_cycle";
            auto close = slot<HR(__stdcall*)(void*)>(l, 9);
            auto execute = slot<void(__stdcall*)(void*, UINT, void* const*)>(q, 10);
            auto signal = slot<HR(__stdcall*)(void*, void*, std::uint64_t)>(q, queue_slot(q, 14));
            auto completed = slot<std::uint64_t(__stdcall*)(void*)>(f, 8);
            auto wait = slot<HR(__stdcall*)(void*, std::uint64_t, HANDLE)>(f, 9);
            D3D12_VIEWPORT vp{0, 0, size, size, 0, 1};
            D3D12_RECT sc{0, 0, static_cast<LONG>(size), static_cast<LONG>(size)};
            HR hr = timed(o.c_str(), [&] {
                reset(l, a, nullptr);
                heaps(l, 1, &heap);
                set(l, pso);
                targets(l, 1, &rtv, FALSE, nullptr);
                viewports(l, 1, &vp);
                scissors(l, 1, &sc);
                topology(l, 4);
                for (std::uint64_t k = 0; k < n; ++k) {
                    if (meshes) {
                        mesh(l, 1, 1, 1);
                    } else {
                        draw(l, 3, 1, 0, 0);
                    }
                }
                close(l);
                execute(q, 1, &l);
                std::uint64_t v = completed(f) + 1;
                signal(q, f, v);
                return wait(f, v, nullptr);
            });
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "compute_pipeline") {

            need(4);
            void* dev = object(t[1]);
            auto& code = shader_named(t[3]);
            struct Head {
                void* root;
                const void* code;
                std::size_t size;
            } head{nullptr, code.data(), code.size()};
            GUID id = __uuidof(ID3D12PipelineState);
            void* out = nullptr;
            HR hr = timed("compute_pipeline", [&] {
                if (out && repeat > 1) {
                    slot<ULONG(__stdcall*)(void*)>(out, 2)(out);
                    out = nullptr;
                }
                return slot<HR(__stdcall*)(void*, const Head*, const GUID*, void**)>(dev, 11)(dev, &head, &id, &out);
            });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "set_pipeline") {
            need(3);
            void* l = object(t[1]);
            void* pso = t[2] == "null" ? nullptr : object(t[2]);
            timed("set_pipeline", [&] {
                slot<void(__stdcall*)(void*, void*)>(l, 25)(l, pso);
                return 0;
            });
        } else if (o == "compute_uav" || o == "compute_srv" || o == "compute_cbv") {

            need(4);
            void* l = object(t[1]);
            auto root = static_cast<UINT>(num(t[2]));
            std::uint64_t at = address(t[3]);
            int s = o == "compute_uav" ? 45 : o == "compute_srv" ? 43 : 41;
            timed(o.c_str(), [&] {
                slot<void(__stdcall*)(void*, UINT, std::uint64_t)>(l, s)(l, root, at);
                return 0;
            });
        } else if (o == "set_heaps") {

            if (t.size() < 2) {
                throw Failure("set_heaps <list> <heap>...");
            }
            void* l = object(t[1]);
            std::vector<void*> heaps;
            for (size_t i = 2; i < t.size(); ++i) {
                heaps.push_back(object(t[i]));
            }
            timed("set_heaps", [&] {
                slot<void(__stdcall*)(void*, UINT, void* const*)>(l, 32)(l, static_cast<UINT>(heaps.size()), heaps.data());
                return 0;
            });
        } else if (o == "dispatch") {

            need(5);
            void* l = object(t[1]);
            auto x = static_cast<UINT>(num(t[2])), y = static_cast<UINT>(num(t[3])), z = static_cast<UINT>(num(t[4]));
            timed("dispatch", [&] {
                slot<void(__stdcall*)(void*, UINT, UINT, UINT)>(l, 14)(l, x, y, z);
                return 0;
            });
        } else if (o == "texture_cycle") {


            need(9);
            void* q = object(t[1]);
            void* l = object(t[2]);
            void* a = object(t[3]);
            void* f = object(t[4]);
            auto dst = copy_location(t[5]);
            auto src = copy_location(t[6]);
            std::uint64_t touch_bytes = num(t[8]);
            auto* touch = touch_bytes ? reinterpret_cast<std::uint8_t*>(address(t[7])) : nullptr;
            auto reset = slot<HR(__stdcall*)(void*, void*, void*)>(l, 10);
            auto copy = slot<void(__stdcall*)(void*, const void*, UINT, UINT, UINT, const void*, const void*)>(l, 16);
            auto close = slot<HR(__stdcall*)(void*)>(l, 9);
            auto execute = slot<void(__stdcall*)(void*, UINT, void* const*)>(q, 10);
            auto signal = slot<HR(__stdcall*)(void*, void*, std::uint64_t)>(q, queue_slot(q, 14));
            auto completed = slot<std::uint64_t(__stdcall*)(void*)>(f, 8);
            auto wait = slot<HR(__stdcall*)(void*, std::uint64_t, HANDLE)>(f, 9);
            std::uint8_t counter = 0;
            HR hr = timed("texture_cycle", [&] {
                if (touch) {
                    ++counter;
                    for (std::uint64_t i = 0; i < touch_bytes; i += 64) {
                        touch[i] = counter;
                    }
                }
                reset(l, a, nullptr);
                copy(l, &dst, 0, 0, 0, &src, nullptr);
                close(l);
                execute(q, 1, &l);
                std::uint64_t v = completed(f) + 1;
                signal(q, f, v);
                return wait(f, v, nullptr);
            });
            log << "  hr " << hr_text(hr) << "\n";
        } else {
            op_more(t);
        }
    }


    void op_more(const std::vector<std::string>& t) {
        auto need = [&](size_t n) {
            if (t.size() != n) {
                throw Failure(t[0] + " takes " + std::to_string(n - 1) + " arguments");
            }
        };
        const std::string& o = t[0];
        if (o == "precompile_init") {


            need(1);
            std::uint32_t out[2] = {};
            int r = backend.precompile_init ? backend.precompile_init(out) : -2;
            log << "  result " << r << " view_heap_limit " << out[0] << " mesh " << out[1] << "\n";
        } else if (o == "precompile") {


            if (t.size() != 2 && t.size() != 3) {
                throw Failure("precompile <shader> [label]");
            }
            auto& code = shader_named(t[1]);
            std::uint32_t out[7] = {};
            int r = timed("precompile", [&] { return backend.precompile_shader ? backend.precompile_shader(code.data(), code.size(), out) : -2; });
            if (r != 0) {
                log << "  result " << r << "\n";
                return;
            }
            char name[17] = {};
            std::memcpy(name, out, 16);
            log << "  stage " << name << " compute " << out[4];
            if (out[4]) {
                std::string label = t.size() == 3 ? t[2] : t[1];
                int loaded = timed("precompile_compute", [&] { return backend.precompile_compute(out[6], label.data(), label.size()); });
                log << " loaded " << loaded;
            }
            log << "\n";
        } else if (o == "cs_memory" || o == "cs_memory_far" || o == "cs_memory_store") {


            need(o == "cs_memory_store" ? 3 : 2);
            constexpr std::uint32_t V3 = 0x31014FAC;   
            std::vector<std::uint32_t> code;
            if (o == "cs_memory_far") {
                code.insert(code.end(), {
                    0x8002FF02, 0x01000000,   
                    0x82038003,               
                });
            }
            code.insert(code.end(), {
                0x34020082,               
                0x7E060203,               
                0xD70F6A02, 0x00020202,   
                0x50060680,               
            });
            if (o == "cs_memory_store") {
                code.insert(code.end(), {
                    0x7E0802FF, static_cast<std::uint32_t>(num(t[2])),   
                    0xDC708000, 0x007D0402,                             
                    0xBF810000,                                         
                });
            } else {
                code.insert(code.end(), {
                    0xDC308000, 0x047D0002,   
                    0xBF8C3F70,               
                    0xBE8C0300, 0xBE8D0301, 0xBE8E03C1, 0xBE8F03FF, V3,   
                    0xE0701000, 0x80030401,   
                    0xBF810000,               
                });
            }
            std::vector<std::array<std::uint8_t, 20>> layout = {entry({1, 0, 2, 0}, {0, 0, 0, 0}), entry({0, 0, 2, 0}, {2, 1, 0, 0})};
            std::vector<std::array<std::uint8_t, 20>> used = {entry({0x0103, 0, 0, 0}, {4, 0, 0, 0})};
            shaders[t[1]] = console_cs(code, layout, used, 4u << 1, {64, 1, 1}, 8, 24);
            log << "  bytes " << shaders[t[1]].size() << "\n";
        } else if (o == "cs_spilled") {


            need(2);
            std::vector<std::uint32_t> code = {
                0x8701FF01, 0x000000FF,   
                0xF4040100, 0xFA000000,   
                0xF4000200, 0xFA000008,   
                0xBF8CC07F,               
                0xBE8603C1,               
                0xBE8703FF, 0x31014FAC,   
                0x34020082,               
                0x7E040208,               
                0xE0701000, 0x80010201,   
                0xBF810000,               
            };
            std::vector<std::array<std::uint8_t, 20>> layout = {entry({1, 0, 2, 0}, {16, 0, 0, 0}), entry({0, 0, 1, 0}, {18, 1, 0, 0})};
            std::vector<std::array<std::uint8_t, 20>> used = {entry({0x0103, 0, 0, 0}, {4, 16, 0, 0})};
            shaders[t[1]] = console_cs(code, layout, used, 2u << 1, {64, 1, 1}, 4, 16);
            log << "  bytes " << shaders[t[1]].size() << "\n";
        } else if (o == "compute_address") {


            need(4);
            void* l = object(t[1]);
            auto root = static_cast<UINT>(num(t[2]));
            std::uint64_t a = address(t[3]);
            UINT v[2] = {static_cast<UINT>(a), static_cast<UINT>(a >> 32)};
            timed("compute_address", [&] {
                slot<void(__stdcall*)(void*, UINT, UINT, const void*, UINT)>(l, 39)(l, root, 2, v, 0);
                return 0;
            });
        } else if (o == "ms_triangle") {


            if (t.size() != 2 && t.size() != 3) {
                throw Failure("ms_triangle <name> [z]");
            }
            std::vector<std::uint32_t> code = {
                0xD7650001, 0x000100C1,   
                0x36040281,               
                0x36060282,               
                0x7E040D02,               
                0x7E060D03,               
                0x100804F6,               
                0x060808F3,               
                0x100A06F4,               
                0x060A0AF3,               
                0x7E0C02F0,               
                0x7E0E02F2,               
                0x7E1202FF, 0x00200400,   
                0xF8000941, 0x00000009,   
                0xF80008CF, 0x07060504,   
                0xBF810000,               
            };
            if (t.size() == 3) {
                float z = std::stof(t[2]);
                std::uint32_t bits;
                std::memcpy(&bits, &z, 4);
                auto at = std::find(code.begin(), code.end(), 0x7E0C02F0u);   
                *at = 0x7E0C02FF;                                             
                code.insert(at + 1, bits);
            }


            std::vector<std::uint8_t> stage(0x450, 0);
            stage[0x448] = 32;
            stage[0x44a] = 32;
            stage[0x44c] = 32;
            stage[0x44f] = 2;
            shaders[t[1]] = console_shader(0x48, code, {}, {}, stage, 10, 8,
                                           {{{'O', 'S', 'G', '1'}, signature_part({{"SV_Position", 0, 1, 0, 0xF}})}});
            log << "  bytes " << shaders[t[1]].size() << "\n";
        } else if (o == "dispatch_mesh") {

            need(3);
            void* l = object(t[1]);
            auto x = static_cast<UINT>(num(t[2]));
            timed("dispatch_mesh", [&] {
                slot<void(__stdcall*)(void*, UINT, UINT, UINT)>(l, list_slot(l, 127))(l, x, 1, 1);
                return 0;
            });
        } else if (o == "cs_heap" || o == "cs_heap_tex") {


            need(2);
            constexpr std::uint32_t V3 = 0x31014FAC;   
            std::vector<std::uint32_t> code = {
                0x8F088503,   
                0xBE830380,   
            };
            if (o == "cs_heap") {
                code.insert(code.end(), {
                    0xF4080101, 0x10000000,   
                    0xBF8CC07F,               
                    0x34020082,               
                    0xE0301000, 0x80010201,   
                    0xBF8C3F70,               
                    0xBE8C0300, 0xBE8D0301, 0xBE8E03C1, 0xBE8F03FF, V3,   
                    0xE0701000, 0x80030201,   
                    0xBF810000,               
                });
            } else {
                code.insert(code.end(), {
                    0xF40C0401, 0x10000000,   
                    0xBF8CC07F,               
                    0x36020087,               
                    0x2C040083,               
                    0xF0001108, 0x00040301,   
                    0xBF8C3F70,               
                    0x34020082,               
                    0xBE8C0300, 0xBE8D0301, 0xBE8E03C1, 0xBE8F03FF, V3,   
                    0xE0701000, 0x80030301,   
                    0xBF810000,               
                });
            }
            std::vector<std::array<std::uint8_t, 20>> layout = {entry({1, 0, 2, 0}, {0, 0, 0, 0}), entry({5, 0, 1, 0}, {2, 2, 0, 0}),
                                                                entry({0, 0, 1, 0}, {3, 1, 0, 0})};

            std::vector<std::array<std::uint8_t, 20>> used = {entry({0x0103, 0, 0, 0}, {4, 0, 0, 0})};
            shaders[t[1]] = console_cs(code, layout, used, 4u << 1, {64, 1, 1}, 8, 24);
            log << "  bytes " << shaders[t[1]].size() << "\n";
        } else if (o == "library_trimmed") {


            if (backend.library_trimmed) {
                Sleep(200);
                log << "  trimmed " << backend.library_trimmed() << "\n";
            }
        } else if (o == "heap_counts") {

            if (backend.heap_counts) {
                std::uint64_t c[6] = {};
                backend.heap_counts(c);
                log << "  buffer passes " << c[0] << " textures " << c[1] << " bytes " << c[2] << " early " << c[3] << " bake " << c[4]
                    << " tabled " << c[5] << "\n";
            }
        } else if (o == "wait_event") {


            if (t.size() < 2 || t.size() > 3) {
                throw Failure("wait_event <name> [ms]");
            }
            HANDLE e = event(t[1]);
            DWORD ms = t.size() > 2 ? static_cast<DWORD>(num(t[2])) : 5000;
            bool set = WaitForSingleObject(e, ms) == WAIT_OBJECT_0;
            ResetEvent(e);
            log << "  set " << set << "\n";
        } else if (o == "event") {

            need(2);
            HANDLE e = event(t[1]);
            bool set = WaitForSingleObject(e, 0) == WAIT_OBJECT_0;
            ResetEvent(e);
            log << "  set " << set << "\n";
        } else if (o == "fence_flags") {
            need(2);
            void* f = object(t[1]);
            log << "  flags " << slot<UINT(__stdcall*)(void*)>(f, 11)(f) << "\n";
        } else if (o == "fence_memory") {

            need(2);
            void* f = object(t[1]);
            std::uint64_t at = slot<std::uint64_t(__stdcall*)(void*)>(f, 12)(f);
            log << "  value " << *reinterpret_cast<volatile std::uint64_t*>(at) << "\n";
        } else if (o == "create_queue" || o == "create_queue_x") {


            bool x = o == "create_queue_x";
            size_t base = x ? 5 : 4;
            if (t.size() < base || t.size() > base + 1) {
                throw Failure(o + " <device> <name> <kind>" + (x ? " <pipe>" : "") + " [iid|noout|nodesc]");
            }
            void* dev = object(t[1]);
            UINT kind = static_cast<UINT>(num(t[3]));
            std::string extra = t.size() > base ? t[base] : "";
            GUID id = !extra.empty() && extra != "noout" && extra != "nodesc" ? iid(extra) : __uuidof(ID3D12CommandQueue);
            void* out = nullptr;
            void** outp = extra == "noout" ? nullptr : &out;
            HR hr;
            if (x) {
                UINT d[4] = {kind, 0, static_cast<UINT>(num(t[4])), 0};
                hr = timed("create_queue_x", [&] {
                    return slot<HR(__stdcall*)(void*, const void*, const GUID*, void**)>(dev, 48)(dev, extra == "nodesc" ? nullptr : d, &id, outp);
                });
            } else {
                D3D12_COMMAND_QUEUE_DESC d{static_cast<D3D12_COMMAND_LIST_TYPE>(kind), 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
                hr = timed("create_queue", [&] {
                    return slot<HR(__stdcall*)(void*, const void*, const GUID*, void**)>(dev, 8)(dev, extra == "nodesc" ? nullptr : &d, &id, outp);
                });
            }
            if (out) {
                newer_queue[out] = newer;
            }
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]);
            if (out) {
                log << " kind " << *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(out) + 0x10);
            }
            log << "\n";
        } else if (o == "queue_signal" || o == "queue_signal2" || o == "queue_wait") {

            need(4);
            void* q = object(t[1]);
            void* f = t[2] == "null" ? nullptr : object(t[2]);
            std::uint64_t v = num(t[3]);
            HR hr;
            if (o == "queue_signal2") {
                hr = timed("queue_signal2", [&] {
                    return slot<HR(__stdcall*)(void*, void*, std::uint64_t, const void*, const void*)>(q, queue_slot(q, 32))(q, f, v, nullptr, nullptr);
                });
            } else {
                unsigned s = queue_slot(q, o == "queue_wait" ? 15 : 14);
                hr = timed(o == "queue_wait" ? "queue_wait" : "queue_signal", [&] { return slot<HR(__stdcall*)(void*, void*, std::uint64_t)>(q, s)(q, f, v); });
            }
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "handoff") {


            need(4);
            void* q = object(t[1]);
            void* a = object(t[2]);
            void* b = object(t[3]);
            auto completed = slot<std::uint64_t(__stdcall*)(void*)>(a, 8);
            auto fence_signal = slot<HR(__stdcall*)(void*, std::uint64_t)>(a, 10);
            auto wait = slot<HR(__stdcall*)(void*, void*, std::uint64_t)>(q, queue_slot(q, 15));
            auto signal = slot<HR(__stdcall*)(void*, void*, std::uint64_t)>(q, queue_slot(q, 14));
            bool held = true;
            bool released = timed("handoff", [&] {
                std::uint64_t v = completed(a) + 1;
                wait(q, a, v);
                signal(q, b, v);
                held = held && completed(b) < v;
                fence_signal(a, v);
                return completed(b) == v;
            });
            log << "  held " << held << " released " << released << " a " << completed(a) << " b " << completed(b) << "\n";
        } else if (o == "queue_desc") {
            need(2);
            void* q = object(t[1]);
            std::uint32_t d[4] = {0xcdcdcdcd, 0xcdcdcdcd, 0xcdcdcdcd, 0xcdcdcdcd};
            slot<void(__stdcall*)(void*, void*)>(q, queue_slot(q, 26))(q, d);
            log << "  desc " << d[0] << " " << static_cast<std::int32_t>(d[1]) << " " << d[2] << " " << d[3] << "\n";
        } else if (o == "timestamp_frequency") {
            need(2);
            void* q = object(t[1]);
            std::uint64_t f = 0;
            HR hr = slot<HR(__stdcall*)(void*, std::uint64_t*)>(q, queue_slot(q, 16))(q, &f);
            log << "  hr " << hr_text(hr) << " frequency " << f << "\n";
        } else if (o == "marker") {
            need(2);
            void* q = object(t[1]);
            for (unsigned s = 11; s <= 13; ++s) {
                slot<void(__stdcall*)(void*)>(q, s)(q);
            }
        } else if (o == "present") {


            if (t.size() < 2 || t.size() > 4) {
                throw Failure("present <queue> [<plane> [<second plane>]]");
            }
            void* q = object(t[1]);
            struct Plane {
                std::uint64_t token;
                UINT count;
                void* const* resources;
                std::uint8_t rest[0x48 - 24];
            };
            static_assert(sizeof(Plane) == 0x48);
            void* r[2] = {t.size() > 2 ? object(t[2]) : nullptr, t.size() > 3 ? object(t[3]) : nullptr};
            Plane planes[2] = {};
            planes[0].count = 1;
            planes[0].resources = &r[0];
            planes[1].count = 1;
            planes[1].resources = &r[1];
            UINT n = static_cast<UINT>(t.size() - 2);
            HR hr = timed("present", [&] {
                return slot<HR(__stdcall*)(void*, UINT, const void*, const void*)>(q, queue_slot(q, 30))(q, n, n ? planes : nullptr, nullptr);
            });
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "screenshot") {


            need(1);
            HWND found = nullptr;
            for (int tries = 0; tries < 50 && !found; ++tries) {
                EnumWindows(
                    [](HWND w, LPARAM at) -> BOOL {
                        DWORD process = 0;
                        GetWindowThreadProcessId(w, &process);
                        if (process == GetCurrentProcessId() && IsWindowVisible(w)) {
                            *reinterpret_cast<HWND*>(at) = w;
                            return FALSE;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&found));
                if (!found) {
                    Sleep(10);
                }
            }
            if (!found) {
                log << "  no window\n";
                return;
            }
            for (int k = 0; k < 4; ++k) {
                DwmFlush();
            }
            Sleep(50);
            RECT rc{};
            GetClientRect(found, &rc);
            int w = rc.right, h = rc.bottom;
            HDC wdc = GetDC(found);
            HDC mdc = CreateCompatibleDC(wdc);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(mdc, bmp);
            PrintWindow(found, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
            const auto* px = static_cast<const std::uint8_t*>(bits);
            std::uint64_t hsh = 1469598103934665603ull;
            for (int i = 0; i < w * h * 4; ++i) {
                hsh = (hsh ^ px[i]) * 1099511628211ull;
            }
            log << "  size " << w << "x" << h << " hash " << hex(hsh) << " first " << std::hex << std::setw(8) << std::setfill('0')
                << *reinterpret_cast<const std::uint32_t*>(px) << " middle " << std::setw(8)
                << *reinterpret_cast<const std::uint32_t*>(px + 4 * ((h / 2) * w + w / 2)) << std::dec << std::setfill(' ') << "\n";
            SelectObject(mdc, old);
            DeleteObject(bmp);
            DeleteDC(mdc);
            ReleaseDC(found, wdc);
        } else if (o == "set_interval") {

            need(3);
            void* dev = object(t[1]);
            HR hr = slot<HR(__stdcall*)(void*, void*, UINT, UINT, UINT)>(dev, 78)(dev, nullptr, static_cast<UINT>(num(t[2])), 1, 0);
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "schedule") {
            need(2);
            void* dev = object(t[1]);
            HR hr = slot<HR(__stdcall*)(void*, UINT, UINT, const void*, UINT)>(dev, 79)(dev, 0, 0, nullptr, 0);
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "frame_wait") {

            if (t.size() < 5 || t.size() > 6) {
                throw Failure("frame_wait <device> <kind> <timeout> <flags> [notoken]");
            }
            void* dev = object(t[1]);
            UINT kind = static_cast<UINT>(num(t[2]));
            UINT timeout = t[3] == "forever" ? 0xffffffffu : static_cast<UINT>(num(t[3]));
            UINT flags = static_cast<UINT>(num(t[4]));
            std::uint64_t token = 0xcdcd;
            bool with_token = !(t.size() > 5 && t[5] == "notoken");
            HR hr = timed("frame_wait", [&] {
                return slot<HR(__stdcall*)(void*, UINT, UINT, const void*, UINT, std::uint64_t*)>(dev, 80)(dev, kind, timeout, nullptr, flags,
                                                                                                         with_token ? &token : nullptr);
            });
            log << "  hr " << hr_text(hr) << " token " << token << "\n";
        } else if (o == "frame_pace") {


            need(4);
            void* dev = object(t[1]);
            std::uint64_t us = num(t[2]);
            std::uint64_t frames = num(t[3]);
            auto wait = slot<HR(__stdcall*)(void*, UINT, UINT, const void*, UINT, std::uint64_t*)>(dev, 80);
            slot<HR(__stdcall*)(void*, void*, UINT, UINT, UINT)>(dev, 78)(dev, nullptr, static_cast<UINT>(us), 3, 0);
            std::uint64_t token = 0;
            long long before = 0;
            double off = 0;
            bool all_ok = true;
            std::uint64_t counted = 0;
            for (std::uint64_t i = 0; i < frames; ++i) {
                wait(dev, 0, 0, nullptr, 2, &token);
                HR hr = wait(dev, 2, 0xffffffffu, nullptr, 0, &token);
                long long at = now_ns();
                all_ok = all_ok && hr == S_OK;
                if (i > 0) {
                    long long gap = at - before;
                    off += static_cast<double>(std::llabs(gap - static_cast<long long>(us * 1000)));
                    ++counted;
                }
                before = at;
            }
            timing << op_number << " frame_pace " << (counted ? off / static_cast<double>(counted) : 0.0) << "\n";
            log << "  ok " << all_ok << "\n";
        } else if (o == "frame_statistics") {
            need(2);
            void* dev = object(t[1]);
            UINT count = 77;
            HR hr = slot<HR(__stdcall*)(void*, std::uint64_t, UINT, UINT*, void*)>(dev, 81)(dev, 1, 1, &count, nullptr);
            log << "  hr " << hr_text(hr) << " count " << count << "\n";
        } else if (o == "churn_committed" || o == "churn_placed" || o == "churn_heap") {


            void* dev = object(t[1]);
            GUID rid = __uuidof(ID3D12Resource), hid = __uuidof(ID3D12Heap);
            HR first = S_OK;
            if (o == "churn_heap") {
                need(3);
                D3D12_HEAP_DESC d{};
                d.SizeInBytes = num(t[2]);
                d.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                first = timed("churn_heap", [&] {
                    void* out = nullptr;
                    HR hr = slot<HR(__stdcall*)(void*, const void*, const GUID*, void**)>(dev, 28)(dev, &d, &hid, &out);
                    if (out) {
                        slot<ULONG(__stdcall*)(void*)>(out, 2)(out);
                    }
                    return hr;
                });
            } else if (o == "churn_committed") {
                need(4);
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = static_cast<D3D12_HEAP_TYPE>(num(t[2]));
                D3D12_RESOURCE_DESC d = parse_desc(t[3]);
                first = timed("churn_committed", [&] {
                    void* out = nullptr;
                    HR hr = slot<HR(__stdcall*)(void*, const void*, UINT, const void*, UINT, const void*, const GUID*, void**)>(dev, 27)(dev, &heap, 0, &d, 0,
                                                                                                                                     nullptr, &rid, &out);
                    if (out) {
                        slot<ULONG(__stdcall*)(void*)>(out, 2)(out);
                    }
                    return hr;
                });
            } else {
                need(5);
                void* heap = object(t[2]);
                std::uint64_t offset = num(t[3]);
                D3D12_RESOURCE_DESC d = parse_desc(t[4]);
                first = timed("churn_placed", [&] {
                    void* out = nullptr;
                    HR hr = slot<HR(__stdcall*)(void*, void*, std::uint64_t, const void*, UINT, const void*, const GUID*, void**)>(dev, 29)(
                        dev, heap, offset, &d, 0, nullptr, &rid, &out);
                    if (out) {
                        slot<ULONG(__stdcall*)(void*)>(out, 2)(out);
                    }
                    return hr;
                });
            }
            log << "  hr " << hr_text(first) << "\n";
        } else if (o == "resource_calls") {


            need(2);
            void* r = object(t[1]);
            auto map = slot<HR(__stdcall*)(void*, UINT, const void*, void**)>(r, 8);
            auto unmap = slot<void(__stdcall*)(void*, UINT, const void*)>(r, 9);
            auto desc = slot<void(__stdcall*)(void*, void*)>(r, 10);
            auto va = slot<std::uint64_t(__stdcall*)(void*)>(r, 11);
            bool same = timed("resource_calls", [&] {
                void* p = nullptr;
                D3D12_RESOURCE_DESC d;
                map(r, 0, nullptr, &p);
                unmap(r, 0, nullptr);
                desc(r, &d);
                return reinterpret_cast<std::uint64_t>(p) == va(r);
            });
            log << "  same " << same << "\n";
        } else if (o == "slot_addresses") {


            need(4);
            void* r = object(t[1]);
            for (unsigned i = static_cast<unsigned>(num(t[2])); i <= num(t[3]); ++i) {
                auto p = reinterpret_cast<std::uintptr_t>(slot<void*>(r, i));
                std::fprintf(stderr, "slot %u at %llx (mod 64: %llu)", i, static_cast<unsigned long long>(p), static_cast<unsigned long long>(p % 64));
                const auto* b = reinterpret_cast<const std::uint8_t*>(p);
                for (int k = 0; k < 48; ++k) {
                    std::fprintf(stderr, " %02x", b[k]);
                }
                std::fprintf(stderr, "\n");
            }
        } else if (o == "resource_call") {

            need(3);
            void* r = object(t[1]);
            std::string which = t[2];
            std::uint64_t sink = 0;
            if (which == "map") {
                auto f = slot<HR(__stdcall*)(void*, UINT, const void*, void**)>(r, 8);
                timed("call_map", [&] {
                    void* p = nullptr;
                    f(r, 0, nullptr, &p);
                    return p != nullptr;
                });
            } else if (which == "unmap") {
                auto f = slot<void(__stdcall*)(void*, UINT, const void*)>(r, 9);
                timed("call_unmap", [&] {
                    f(r, 0, nullptr);
                    return true;
                });
            } else if (which == "desc") {
                auto f = slot<void(__stdcall*)(void*, void*)>(r, 10);
                timed("call_desc", [&] {
                    D3D12_RESOURCE_DESC d;
                    f(r, &d);
                    return d.Width != 0;
                });
            } else {
                auto f = slot<std::uint64_t(__stdcall*)(void*)>(r, 11);
                sink = timed("call_va", [&] { return f(r); });
            }
            (void)sink;
            log << "  done\n";
        } else if (o == "desc_heap") {


            if (t.size() < 5) {
                throw Failure("desc_heap <device> <name> <kind> <count> [flags=N] [place=<mem>+<off>] [placesize=N] [iid=..] [noout]");
            }
            void* dev = object(t[1]);
            struct {
                UINT kind, count, flags, nodes;
                std::uint64_t address, size;
            } d{static_cast<UINT>(num(t[3])), static_cast<UINT>(num(t[4])), 0, 0, 0, 0};
            GUID id = __uuidof(ID3D12DescriptorHeap);
            bool noout = false;
            std::uint64_t place_size = ~0ull;
            for (size_t i = 5; i < t.size(); ++i) {
                const std::string& a = t[i];
                if (a.rfind("flags=", 0) == 0) {
                    d.flags = static_cast<UINT>(num(a.substr(6)));
                } else if (a.rfind("place=", 0) == 0) {
                    auto plus = a.find('+');
                    d.address = reinterpret_cast<std::uint64_t>(title.at(a.substr(6, plus - 6))) + (plus == std::string::npos ? 0 : num(a.substr(plus + 1)));
                } else if (a.rfind("placesize=", 0) == 0) {
                    place_size = num(a.substr(10));
                } else if (a.rfind("iid=", 0) == 0) {
                    id = iid(a.substr(4));
                } else if (a == "noout") {
                    noout = true;
                } else {
                    throw Failure("bad desc_heap argument " + a);
                }
            }
            if (d.address != 0) {
                const UINT sizes[4] = {32, 16, 128, 128};
                d.size = place_size != ~0ull ? place_size : std::uint64_t{d.kind < 4 ? sizes[d.kind] : 0} * std::max(d.count, 1u);
            }
            void* out = nullptr;
            HR hr = timed("desc_heap", [&] { return slot<HR(__stdcall*)(void*, const void*, const GUID*, void**)>(dev, 14)(dev, &d, &id, noout ? nullptr : &out); });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "desc_heap_info") {


            if (t.size() < 2 || t.size() > 3) {
                throw Failure("desc_heap_info <heap> [title memory]");
            }
            void* h = object(t[1]);
            UINT d[4] = {0xcdcd, 0xcdcd, 0xcdcd, 0xcdcd};
            slot<void(__stdcall*)(void*, void*)>(h, 8)(h, d);
            std::uint64_t cpu = slot<std::uint64_t(__stdcall*)(void*)>(h, 9)(h);
            std::uint64_t gpu = slot<std::uint64_t(__stdcall*)(void*)>(h, 10)(h);
            log << "  desc " << d[0] << " " << d[1] << " " << d[2] << " " << d[3] << " same " << (cpu == gpu) << " start%64 " << cpu % 64;
            if (t.size() == 3) {
                log << " from " << t[2] << " " << static_cast<long long>(cpu - reinterpret_cast<std::uint64_t>(title.at(t[2])));
            }
            log << "\n";
        } else if (o == "cbv" || o == "srv" || o == "uav" || o == "rtv" || o == "dsv" || o == "sampler" || o == "sampler_x" || o == "placed_srv" ||
                   o == "placed_uav") {


            if (t.size() < 4) {
                throw Failure(o + " <device> <handle> ...");
            }
            void* dev = object(t[1]);
            std::uint64_t h = handle(t[2]);
            auto res = [&](const std::string& n) { return n == "null" ? nullptr : object(n); };
            if (o == "cbv") {
                need(5);
                std::uint64_t at = t[3] == "null" ? 0 : address(t[3]);
                UINT d[4] = {static_cast<UINT>(at), static_cast<UINT>(at >> 32), static_cast<UINT>(num(t[4])), 0};
                timed("cbv", [&] {
                    slot<void(__stdcall*)(void*, const void*, std::uint64_t)>(dev, 17)(dev, d, h);
                    return 0;
                });
            } else if (o == "srv" || o == "rtv" || o == "dsv") {
                void* r = res(t[3]);
                auto w = t.size() > 4 ? words(t[4], o == "srv" ? 10 : 6) : std::vector<std::uint32_t>{};
                unsigned s = o == "srv" ? 18 : o == "rtv" ? 20 : 21;
                timed(o.c_str(), [&] {
                    slot<void(__stdcall*)(void*, void*, const void*, std::uint64_t)>(dev, s)(dev, r, w.empty() ? nullptr : w.data(), h);
                    return 0;
                });
            } else if (o == "uav") {
                if (t.size() < 5) {
                    throw Failure("uav <device> <handle> <resource|null> <counter|null> [w:...]");
                }
                void* r = res(t[3]);
                void* c = res(t[4]);
                auto w = t.size() > 5 ? words(t[5], 10) : std::vector<std::uint32_t>{};
                timed("uav", [&] {
                    slot<void(__stdcall*)(void*, void*, void*, const void*, std::uint64_t)>(dev, 19)(dev, r, c, w.empty() ? nullptr : w.data(), h);
                    return 0;
                });
            } else if (o == "sampler" || o == "sampler_x") {
                auto w = words(t[3], o == "sampler" ? 13 : 27);
                unsigned s = o == "sampler" ? 22 : 61;
                timed(o.c_str(), [&] {
                    slot<void(__stdcall*)(void*, const void*, std::uint64_t)>(dev, s)(dev, w.empty() ? nullptr : w.data(), h);
                    return 0;
                });
            } else {
                need(5);
                auto r = words(t[3], 14);
                auto v = words(t[4], 16);
                unsigned s = o == "placed_srv" ? 70 : 71;
                timed(o.c_str(), [&] {
                    slot<void(__stdcall*)(void*, const void*, const void*, std::uint64_t)>(dev, s)(dev, r.empty() ? nullptr : r.data(),
                                                                                                  v.empty() ? nullptr : v.data(), h);
                    return 0;
                });
            }
        } else if (o == "desc_copy1") {

            need(6);
            void* dev = object(t[1]);
            UINT count = static_cast<UINT>(num(t[2]));
            std::uint64_t dst = handle(t[3]), src = handle(t[4]);
            UINT kind = static_cast<UINT>(num(t[5]));
            timed("desc_copy1", [&] {
                slot<void(__stdcall*)(void*, UINT, std::uint64_t, std::uint64_t, UINT)>(dev, 24)(dev, count, dst, src, kind);
                return 0;
            });
        } else if (o == "desc_copy") {


            if (t.size() < 5) {
                throw Failure("desc_copy <device> <kind> d:<handle>x<n>,... s:<handle>x<n>,... [nodstsizes] [nosrcsizes]");
            }
            void* dev = object(t[1]);
            UINT kind = static_cast<UINT>(num(t[2]));
            auto ranges = [&](const std::string& spec, std::vector<std::uint64_t>& starts, std::vector<UINT>& sizes) {
                std::istringstream in(spec.substr(2));
                std::string r;
                while (std::getline(in, r, ',')) {

                    auto x = r.rfind('x');
                    bool counted = x != std::string::npos && r.find(':') != std::string::npos && x > r.find(':');
                    starts.push_back(handle(counted ? r.substr(0, x) : r));
                    sizes.push_back(counted ? static_cast<UINT>(num(r.substr(x + 1))) : 1);
                }
            };
            std::vector<std::uint64_t> ds, ss;
            std::vector<UINT> dn, sn;
            ranges(t[3], ds, dn);
            ranges(t[4], ss, sn);
            bool no_d = std::find(t.begin(), t.end(), "nodstsizes") != t.end();
            bool no_s = std::find(t.begin(), t.end(), "nosrcsizes") != t.end();
            timed("desc_copy", [&] {
                slot<void(__stdcall*)(void*, UINT, const void*, const void*, UINT, const void*, const void*, UINT)>(dev, 23)(
                    dev, static_cast<UINT>(ds.size()), ds.data(), no_d ? nullptr : dn.data(), static_cast<UINT>(ss.size()), ss.data(),
                    no_s ? nullptr : sn.data(), kind);
                return 0;
            });
        } else if (o == "slot") {


            need(2);
            auto* p = reinterpret_cast<const std::uint32_t*>(handle(t[1]));
            if (p[0] == 0x45495676) {
                log << "  view " << p[1] << " 0x" << std::hex << p[2] << std::dec << "\n";
            } else if (p[0] == 0x56545276 || p[0] == 0x56534476) {
                void* r;
                std::memcpy(&r, p + 2, sizeof r);
                log << "  " << (p[0] == 0x56545276 ? "rtv" : "dsv") << " described " << p[1] << " resource " << (r ? seen(r, "") : "null") << " desc";
                for (int i = 4; i < 10; ++i) {
                    log << " " << p[i];
                }
                log << "\n";
            } else {
                log << "  raw " << std::hex << p[0] << " " << p[1] << " " << p[2] << std::dec << "\n";
            }
        } else if (o == "root_serialize") {


            if (t.size() < 5) {
                throw Failure("root_serialize <blob> <plain|versioned|null|nullversioned> <version> <flags> [param...]");
            }
            const std::string& entry = t[2];
            UINT version = static_cast<UINT>(num(t[3]));
            UINT flags = static_cast<UINT>(num(t[4]));
            std::vector<D3D12_ROOT_PARAMETER> p0;
            std::vector<D3D12_ROOT_PARAMETER1> p1;
            std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> r0;
            std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> r1;
            UINT samplers = 0;
            for (size_t i = 5; i < t.size(); ++i) {
                if (t[i].rfind("samplers=", 0) == 0) {
                    samplers = static_cast<UINT>(num(t[i].substr(9)));
                    continue;
                }
                std::vector<std::string> f;
                std::istringstream in(t[i]);
                std::string x;
                while (std::getline(in, x, ':')) {
                    f.push_back(x);
                }
                D3D12_ROOT_PARAMETER a{};
                D3D12_ROOT_PARAMETER1 b{};
                a.ShaderVisibility = b.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                if (f[0] == "c") {
                    a.ParameterType = b.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                    a.Constants = b.Constants = {static_cast<UINT>(num(f[1])), static_cast<UINT>(num(f[2])), static_cast<UINT>(num(f[3]))};
                } else if (f[0] == "cbv" || f[0] == "srv" || f[0] == "uav") {
                    a.ParameterType = b.ParameterType = f[0] == "cbv"   ? D3D12_ROOT_PARAMETER_TYPE_CBV
                                                        : f[0] == "srv" ? D3D12_ROOT_PARAMETER_TYPE_SRV
                                                                        : D3D12_ROOT_PARAMETER_TYPE_UAV;
                    a.Descriptor = {static_cast<UINT>(num(f[1])), static_cast<UINT>(num(f[2]))};
                    b.Descriptor = {static_cast<UINT>(num(f[1])), static_cast<UINT>(num(f[2])), D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
                } else if (f[0] == "table") {

                    std::vector<D3D12_DESCRIPTOR_RANGE> ra;
                    std::vector<D3D12_DESCRIPTOR_RANGE1> rb;
                    std::istringstream rs(t[i].substr(6));
                    std::string range;
                    while (std::getline(rs, range, '/')) {
                        std::vector<std::string> g;
                        std::istringstream gs(range);
                        while (std::getline(gs, x, ':')) {
                            g.push_back(x);
                        }
                        auto type = static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(num(g[0]));
                        UINT n = static_cast<UINT>(num(g[1])), reg = static_cast<UINT>(num(g[2])), space = static_cast<UINT>(num(g[3]));
                        ra.push_back({type, n, reg, space, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND});
                        rb.push_back({type, n, reg, space, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND});
                    }
                    r0.push_back(std::move(ra));
                    r1.push_back(std::move(rb));
                    a.ParameterType = b.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                } else {
                    throw Failure("bad root parameter " + t[i]);
                }
                p0.push_back(a);
                p1.push_back(b);
            }

            for (size_t i = 0, k = 0; i < p0.size(); ++i) {
                if (p0[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                    p0[i].DescriptorTable = {static_cast<UINT>(r0[k].size()), r0[k].data()};
                    p1[i].DescriptorTable = {static_cast<UINT>(r1[k].size()), r1[k].data()};
                    ++k;
                }
            }
            std::vector<D3D12_STATIC_SAMPLER_DESC> s0(samplers);
            for (UINT i = 0; i < samplers; ++i) {
                s0[i] = {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0,
                         1, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, 0, D3D12_FLOAT32_MAX, i, 0, D3D12_SHADER_VISIBILITY_ALL};
            }
            ID3DBlob* blob = nullptr;
            ID3DBlob* error = nullptr;
            HR hr;
            if (entry == "plain" || entry == "null") {
                D3D12_ROOT_SIGNATURE_DESC d{static_cast<UINT>(p0.size()), p0.data(), samplers, s0.data(), static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags)};
                hr = timed("root_serialize", [&] {
                    if (blob) {
                        blob->Release();
                        blob = nullptr;
                    }
                    return backend.serialize_root(entry == "null" ? nullptr : &d, version, reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&error));
                });
            } else {
                D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{};
                d.Version = static_cast<D3D_ROOT_SIGNATURE_VERSION>(version);
                if (version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
                    d.Desc_1_0 = {static_cast<UINT>(p0.size()), p0.data(), samplers, s0.data(), static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags)};
                } else {
                    d.Desc_1_1 = {static_cast<UINT>(p1.size()), p1.data(), samplers, s0.data(), static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags)};
                }
                hr = timed("root_serialize_versioned", [&] {
                    if (blob) {
                        blob->Release();
                        blob = nullptr;
                    }
                    return backend.serialize_root_versioned(entry == "nullversioned" ? nullptr : &d, reinterpret_cast<void**>(&blob),
                                                            reinterpret_cast<void**>(&error));
                });
            }
            std::uint64_t hash = 1469598103934665603ull;
            if (blob) {
                const auto* b = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
                for (SIZE_T i = 0; i < blob->GetBufferSize(); ++i) {
                    hash = (hash ^ b[i]) * 1099511628211ull;
                }
            }
            log << "  hr " << hr_text(hr) << " blob " << (blob ? blob->GetBufferSize() : 0) << " hash " << hex(hash) << " error " << (error ? "yes" : "no") << "\n";
            if (error) {
                error->Release();
            }
            if (blobs.count(t[1])) {
                blobs[t[1]]->Release();
                blobs.erase(t[1]);
            }
            if (blob) {
                blobs[t[1]] = blob;
            }
        } else if (o == "root_create") {

            if (t.size() < 4) {
                throw Failure("root_create <device> <name> <blob|null> [size=N] [iid=..] [noout]");
            }
            void* dev = object(t[1]);
            const void* data = nullptr;
            std::size_t size = 0;
            if (t[3] != "null") {
                ID3DBlob* b = blob_named(t[3]);
                data = b->GetBufferPointer();
                size = b->GetBufferSize();
            }
            GUID id = __uuidof(ID3D12RootSignature);
            bool noout = false;
            for (size_t i = 4; i < t.size(); ++i) {
                if (t[i].rfind("size=", 0) == 0) {
                    size = static_cast<std::size_t>(num(t[i].substr(5)));
                } else if (t[i].rfind("iid=", 0) == 0) {
                    id = iid(t[i].substr(4));
                } else if (t[i] == "noout") {
                    noout = true;
                }
            }
            void* out = nullptr;
            HR hr = timed("root_create", [&] {
                if (out && repeat > 1) {
                    slot<ULONG(__stdcall*)(void*)>(out, 2)(out);
                    out = nullptr;
                }
                return slot<HR(__stdcall*)(void*, UINT, const void*, std::size_t, const GUID*, void**)>(dev, 16)(dev, 0, data, size, &id,
                                                                                                             noout ? nullptr : &out);
            });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "root_deserialize") {

            if (t.size() < 2 || t.size() > 3) {
                throw Failure("root_deserialize <blob> [iid]");
            }
            ID3DBlob* b = blob_named(t[1]);
            GUID id = t.size() > 2 ? iid(t[2]) : __uuidof(ID3D12RootSignatureDeserializer);
            void* out = nullptr;
            HR hr = backend.root_deserializer(b->GetBufferPointer(), b->GetBufferSize(), &id, &out);
            log << "  hr " << hr_text(hr);
            if (hr == S_OK && out && id == __uuidof(ID3D12RootSignatureDeserializer)) {
                auto* d = static_cast<ID3D12RootSignatureDeserializer*>(out);
                const D3D12_ROOT_SIGNATURE_DESC* rd = d->GetRootSignatureDesc();
                log << " parameters " << rd->NumParameters << " samplers " << rd->NumStaticSamplers << " flags 0x" << std::hex << rd->Flags << std::dec << " types";
                for (UINT i = 0; i < rd->NumParameters; ++i) {
                    log << " " << rd->pParameters[i].ParameterType;
                }
            }
            if (out) {
                static_cast<IUnknown*>(out)->Release();
            }
            log << "\n";
        } else if (o == "cmd_signature") {

            if (t.size() < 6) {
                throw Failure("cmd_signature <device> <name> <stride> <root|null> <argument|none>... [iid=..]");
            }
            void* dev = object(t[1]);
            void* root = t[4] == "null" ? nullptr : object(t[4]);
            std::vector<std::array<UINT, 4>> args;
            GUID id = __uuidof(ID3D12CommandSignature);
            bool null_args = false;
            for (size_t i = 5; i < t.size(); ++i) {
                if (t[i].rfind("iid=", 0) == 0) {
                    id = iid(t[i].substr(4));
                } else if (t[i] == "nullargs") {
                    null_args = true;
                } else if (t[i] != "none") {
                    std::array<UINT, 4> a{};
                    std::istringstream in(t[i]);
                    std::string x;
                    for (int k = 0; k < 4 && std::getline(in, x, ':'); ++k) {
                        a[k] = static_cast<UINT>(num(x));
                    }
                    args.push_back(a);
                }
            }
            struct {
                UINT stride, count;
                const void* arguments;
                UINT nodes;
            } d{static_cast<UINT>(num(t[3])), null_args ? 2u : static_cast<UINT>(args.size()), null_args ? nullptr : args.data(), 0};
            void* out = nullptr;
            HR hr = timed("cmd_signature", [&] {
                if (out && repeat > 1) {
                    slot<ULONG(__stdcall*)(void*)>(out, 2)(out);
                    out = nullptr;
                }
                return slot<HR(__stdcall*)(void*, const void*, void*, const GUID*, void**)>(dev, 41)(dev, &d, root, &id, &out);
            });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "query_heap") {

            if (t.size() < 5) {
                throw Failure("query_heap <device> <name> <kind> <count> [iid=..|noout|nodesc]");
            }
            void* dev = object(t[1]);
            UINT d[3] = {static_cast<UINT>(num(t[3])), static_cast<UINT>(num(t[4])), 0};
            GUID id = __uuidof(ID3D12QueryHeap);
            bool noout = false, nodesc = false;
            for (size_t i = 5; i < t.size(); ++i) {
                if (t[i].rfind("iid=", 0) == 0) {
                    id = iid(t[i].substr(4));
                } else if (t[i] == "noout") {
                    noout = true;
                } else if (t[i] == "nodesc") {
                    nodesc = true;
                }
            }
            void* out = nullptr;
            HR hr = timed("query_heap", [&] {
                if (out && repeat > 1) {
                    slot<ULONG(__stdcall*)(void*)>(out, 2)(out);
                    out = nullptr;
                }
                return slot<HR(__stdcall*)(void*, const void*, const GUID*, void**)>(dev, 39)(dev, nodesc ? nullptr : d, &id, noout ? nullptr : &out);
            });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "allocator") {

            need(4);
            void* dev = object(t[1]);
            void* out = nullptr;
            GUID id = __uuidof(ID3D12CommandAllocator);
            HR hr = slot<HR(__stdcall*)(void*, UINT, const GUID*, void**)>(dev, 9)(dev, static_cast<UINT>(num(t[3])), &id, &out);
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "list" || o == "list_x") {

            if (t.size() < 5) {
                throw Failure(o + " <device> <name> <kind> <allocator|null> [iid=..]");
            }
            void* dev = object(t[1]);
            UINT kind = static_cast<UINT>(num(t[3]));
            void* alloc = t[4] == "null" ? nullptr : object(t[4]);
            GUID id = t.size() > 5 && t[5].rfind("iid=", 0) == 0 ? iid(t[5].substr(4)) : __uuidof(ID3D12GraphicsCommandList);
            void* out = nullptr;
            HR hr;
            if (o == "list") {
                hr = timed("list", [&] {
                    return slot<HR(__stdcall*)(void*, UINT, UINT, void*, void*, const GUID*, void**)>(dev, 12)(dev, 0, kind, alloc, nullptr, &id, &out);
                });
            } else {
                UINT d[4] = {kind, 0, 0, 0};
                hr = slot<HR(__stdcall*)(void*, const void*, void*, void*, const GUID*, void**)>(dev, 73)(dev, d, alloc, nullptr, &id, &out);
            }
            if (out) {
                newer_list[out] = newer;
            }
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "list_close" || o == "list_type") {
            need(2);
            void* l = object(t[1]);
            if (o == "list_close") {
                HR hr = timed("list_close", [&] { return slot<HR(__stdcall*)(void*)>(l, 9)(l); });
                log << "  hr " << hr_text(hr) << "\n";
            } else {
                log << "  kind " << slot<UINT(__stdcall*)(void*)>(l, 8)(l) << "\n";
            }
        } else if (o == "list_reset") {

            if (t.size() < 3) {
                throw Failure("list_reset <list> <allocator|null> [x]");
            }
            void* l = object(t[1]);
            void* a = t[2] == "null" ? nullptr : object(t[2]);
            HR hr;
            if (t.size() > 3 && t[3] == "x") {
                hr = slot<HR(__stdcall*)(void*, void*, void*, UINT)>(l, list_slot(l, 137))(l, a, nullptr, 0);
            } else {
                hr = timed("list_reset", [&] { return slot<HR(__stdcall*)(void*, void*, void*)>(l, 10)(l, a, nullptr); });
            }
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "copy_region") {

            need(7);
            void* l = object(t[1]);
            void* dst = object(t[2]);
            void* src = object(t[4]);
            std::uint64_t doff = num(t[3]), soff = num(t[5]), n = num(t[6]);
            timed("copy_region", [&] {
                slot<void(__stdcall*)(void*, void*, std::uint64_t, void*, std::uint64_t, std::uint64_t)>(l, 15)(l, dst, doff, src, soff, n);
                return 0;
            });
        } else if (o == "copy_res") {

            if (t.size() < 4) {
                throw Failure("copy_res <list> <dst> <src> [x]");
            }
            void* l = object(t[1]);
            void* dst = object(t[2]);
            void* src = object(t[3]);
            if (t.size() > 4 && t[4] == "x") {
                slot<void(__stdcall*)(void*, void*, void*, UINT)>(l, list_slot(l, 84))(l, dst, src, 0);
            } else {
                slot<void(__stdcall*)(void*, void*, void*)>(l, 17)(l, dst, src);
            }
        } else if (o == "copy_mem") {

            need(5);
            void* l = object(t[1]);
            std::uint64_t dst = address(t[2]), src = address(t[3]), n = num(t[4]);
            timed("copy_mem", [&] {
                slot<void(__stdcall*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, UINT)>(l, list_slot(l, 60))(l, dst, src, n, 0);
                return 0;
            });
        } else if (o == "write_value") {

            if (t.size() < 4) {
                throw Failure("write_value <list> <address> <value> [64|gpu|system]");
            }
            void* l = object(t[1]);
            std::uint64_t at = address(t[2]), v = num(t[3]);
            std::string how = t.size() > 4 ? t[4] : "32";
            timed("write_value", [&] {
                if (how == "32") {
                    slot<void(__stdcall*)(void*, std::uint64_t, UINT, UINT, UINT)>(l, list_slot(l, 69))(l, at, static_cast<UINT>(v), 0, 0);
                } else {
                    UINT flags = how == "gpu" ? 1 : how == "system" ? 2 : 0;
                    slot<void(__stdcall*)(void*, std::uint64_t, std::uint64_t, UINT, UINT)>(l, list_slot(l, 70))(l, at, v, 0, flags);
                }
                return 0;
            });
        } else if (o == "wait_value") {

            if (t.size() < 5) {
                throw Failure("wait_value <list> <address> <comparison> <reference> [64]");
            }
            void* l = object(t[1]);
            std::uint64_t at = address(t[2]);
            UINT cmp = static_cast<UINT>(num(t[3]));
            std::uint64_t ref = num(t[4]);
            unsigned s = list_slot(l, t.size() > 5 ? 72 : 71);
            slot<void(__stdcall*)(void*, std::uint64_t, UINT, std::uint64_t)>(l, s)(l, at, cmp, ref);
        } else if (o == "barrier") {

            if (t.size() < 3) {
                throw Failure("barrier <list> <kind>:<resource|null>...");
            }
            void* l = object(t[1]);
            std::vector<D3D12_RESOURCE_BARRIER> bs;
            for (size_t i = 2; i < t.size(); ++i) {
                std::vector<std::string> f;
                std::istringstream in(t[i]);
                std::string x;
                while (std::getline(in, x, ':')) {
                    f.push_back(x);
                }
                D3D12_RESOURCE_BARRIER b{};
                b.Type = static_cast<D3D12_RESOURCE_BARRIER_TYPE>(num(f[0]));
                auto* r = static_cast<ID3D12Resource*>(f.size() > 1 && f[1] != "null" ? object(f[1]) : nullptr);
                if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {

                    auto after = f.size() > 2 ? static_cast<D3D12_RESOURCE_STATES>(num(f[2])) : D3D12_RESOURCE_STATE_COPY_DEST;
                    b.Transition = {r, 0xffffffff, D3D12_RESOURCE_STATE_COMMON, after};
                } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                    b.Aliasing = {r, static_cast<ID3D12Resource*>(f.size() > 2 && f[2] != "null" ? object(f[2]) : nullptr)};
                } else {
                    b.UAV = {r};
                }
                bs.push_back(b);
            }
            UINT r = slot<UINT(__stdcall*)(void*, UINT, const void*)>(l, 26)(l, static_cast<UINT>(bs.size()), bs.data());
            log << "  actions " << r << "\n";
        } else if (o == "resolve_queries") {

            need(6);
            void* l = object(t[1]);
            void* buffer = object(t[4]);
            slot<void(__stdcall*)(void*, void*, UINT, UINT, UINT, void*, std::uint64_t)>(l, 30)(l, nullptr, static_cast<UINT>(num(t[2])), 0,
                                                                                            static_cast<UINT>(num(t[3])), buffer, num(t[5]));
        } else if (o == "upload_alloc") {

            need(4);
            void* l = object(t[1]);
            std::uint64_t n = num(t[2]), a = num(t[3]);
            auto* p = slot<std::uint8_t*(__stdcall*)(void*, std::uint64_t, std::uint64_t)>(l, 148)(l, n, a);
            bool zero = true;
            for (std::uint64_t i = 0; p && i < n; ++i) {
                zero = zero && p[i] == 0;
            }
            log << "  null " << (p == nullptr) << " aligned " << (p && a > 0 ? reinterpret_cast<std::uintptr_t>(p) % std::max<std::uint64_t>(a, 16) == 0 : 0)
                << " zero " << zero << "\n";
        } else if (o == "execute") {

            if (t.size() < 2) {
                throw Failure("execute <queue> <list>...");
            }
            void* q = t[1] == "null" ? nullptr : object(t[1]);
            void* through = q ? q : object("q");
            std::vector<void*> ls;
            for (size_t i = 2; i < t.size(); ++i) {
                ls.push_back(t[i] == "null" ? nullptr : object(t[i]));
            }
            timed("execute", [&] {
                slot<void(__stdcall*)(void*, UINT, void* const*)>(through, 10)(q, static_cast<UINT>(ls.size()), ls.data());
                return 0;
            });
        } else if (o == "title_memory") {


            if (t.size() != 3 && t.size() != 4) {
                throw Failure("title_memory <name> <bytes> [address]");
            }
            void* want = t.size() == 4 ? reinterpret_cast<void*>(num(t[3])) : nullptr;
            title[t[1]] = static_cast<std::uint8_t*>(VirtualAlloc(want, static_cast<SIZE_T>(num(t[2])), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!title[t[1]]) {
                throw Failure("title_memory: no memory at that address");
            }
        } else if (o == "heap") {

            if (t.size() < 4 || t.size() > 8) {
                throw Failure("heap <device> <name> <bytes> [kind] [alignment] [flags] [iid|noout]");
            }
            void* dev = object(t[1]);
            D3D12_HEAP_DESC d{};
            d.SizeInBytes = num(t[3]);
            d.Properties.Type = static_cast<D3D12_HEAP_TYPE>(t.size() > 4 ? num(t[4]) : 1);
            d.Alignment = t.size() > 5 ? num(t[5]) : 0;
            d.Flags = static_cast<D3D12_HEAP_FLAGS>(t.size() > 6 ? num(t[6]) : 0);
            std::string extra = t.size() > 7 ? t[7] : "";
            GUID id = !extra.empty() && extra != "noout" ? iid(extra) : __uuidof(ID3D12Heap);
            void* out = nullptr;
            HR hr = timed("heap", [&] {
                return slot<HR(__stdcall*)(void*, const void*, const GUID*, void**)>(dev, 28)(dev, &d, &id, extra == "noout" ? nullptr : &out);
            });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "heap_desc") {
            need(2);
            void* h = object(t[1]);
            D3D12_HEAP_DESC d{};
            slot<void(__stdcall*)(void*, void*)>(h, 8)(h, &d);
            log << "  size " << d.SizeInBytes << " kind " << d.Properties.Type << " cpu " << d.Properties.CPUPageProperty << " pool "
                << d.Properties.MemoryPoolPreference << " alignment " << d.Alignment << " flags 0x" << std::hex << d.Flags << std::dec << "\n";
        } else if (o == "committed" || o == "committed_x" || o == "committed3") {

            if (t.size() < 5 || t.size() > 8) {
                throw Failure(o + " <device> <name> <heap kind> <desc> [state] [clear format|none] [iid|noout|nodesc]");
            }
            void* dev = object(t[1]);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = static_cast<D3D12_HEAP_TYPE>(num(t[3]));
            heap.CreationNodeMask = 1;
            heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC d = parse_desc(t[4]);
            UINT state = t.size() > 5 ? static_cast<UINT>(num(t[5])) : 0;
            std::uint32_t clear[5] = {0, 0x3f800000, 0, 0, 0x3f800000};
            bool with_clear = t.size() > 6 && t[6] != "none";
            if (with_clear) {
                clear[0] = static_cast<std::uint32_t>(num(t[6]));
            }
            std::string extra = t.size() > 7 ? t[7] : "";
            GUID id = !extra.empty() && extra != "noout" && extra != "nodesc" ? iid(extra) : __uuidof(ID3D12Resource);
            void* out = nullptr;
            void** outp = extra == "noout" ? nullptr : &out;
            const void* dp = extra == "nodesc" ? nullptr : &d;
            HR hr;
            if (o == "committed3") {
                hr = timed("committed3", [&] {
                    return slot<HR(__stdcall*)(void*, const void*, UINT, const void*, UINT, const void*, void*, UINT, const void*, const GUID*, void**)>(
                        dev, 127)(dev, &heap, 0, dp, state, with_clear ? clear : nullptr, nullptr, 0, nullptr, &id, outp);
                });
            } else {
                unsigned s = o == "committed" ? 27 : 76;
                hr = timed(o.c_str(), [&] {
                    return slot<HR(__stdcall*)(void*, const void*, UINT, const void*, UINT, const void*, const GUID*, void**)>(dev, s)(
                        dev, &heap, 0, dp, state, with_clear ? clear : nullptr, &id, outp);
                });
            }
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "placed") {

            if (t.size() < 6 || t.size() > 7) {
                throw Failure("placed <device> <name> <heap> <offset> <desc> [iid|noout]");
            }
            void* dev = object(t[1]);
            void* heap = t[3] == "null" ? nullptr : object(t[3]);
            std::uint64_t offset = num(t[4]);
            D3D12_RESOURCE_DESC d = parse_desc(t[5]);
            std::string extra = t.size() > 6 ? t[6] : "";
            GUID id = !extra.empty() && extra != "noout" ? iid(extra) : __uuidof(ID3D12Resource);
            void* out = nullptr;
            HR hr = timed("placed", [&] {
                return slot<HR(__stdcall*)(void*, void*, std::uint64_t, const void*, UINT, const void*, const GUID*, void**)>(dev, 29)(
                    dev, heap, offset, &d, 0, nullptr, &id, extra == "noout" ? nullptr : &out);
            });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "at_address") {

            need(6);
            void* dev = object(t[1]);
            std::uint64_t base = t[3] == "0" ? 0 : reinterpret_cast<std::uint64_t>(title.at(t[3]));
            std::uint64_t at = base == 0 ? 0 : base + num(t[4]);
            D3D12_RESOURCE_DESC d = parse_desc(t[5]);
            GUID id = __uuidof(ID3D12Resource);
            void* out = nullptr;
            HR hr = timed("at_address", [&] {
                return slot<HR(__stdcall*)(void*, std::uint64_t, const void*, UINT, const void*, const GUID*, void**)>(dev, 133)(dev, at, &d, 0, nullptr,
                                                                                                                             &id, &out);
            });
            log << "  hr " << hr_text(hr) << " object " << seen(out, t[2]) << "\n";
        } else if (o == "resource") {


            need(2);
            void* r = object(t[1]);
            D3D12_RESOURCE_DESC d{};
            slot<void(__stdcall*)(void*, void*)>(r, 10)(r, &d);
            void* mapped = nullptr;
            HR hr = slot<HR(__stdcall*)(void*, UINT, const void*, void**)>(r, 8)(r, 0, nullptr, &mapped);
            slot<void(__stdcall*)(void*, UINT, const void*)>(r, 9)(r, 0, nullptr);
            std::uint64_t va = slot<std::uint64_t(__stdcall*)(void*)>(r, 11)(r);
            D3D12_HEAP_PROPERTIES hp{};
            D3D12_HEAP_FLAGS hf{};
            HR hr2 = slot<HR(__stdcall*)(void*, void*, void*)>(r, 14)(r, &hp, &hf);
            log << "  " << desc_text(d) << "\n  map " << hr_text(hr) << " same " << (reinterpret_cast<std::uint64_t>(mapped) == va) << " va%64k 0x" << std::hex
                << (va & 0xffff) << std::dec << " heap " << hr_text(hr2) << " " << hp.Type << "," << hp.CPUPageProperty << "," << hp.MemoryPoolPreference << ","
                << hp.CreationNodeMask << "," << hp.VisibleNodeMask << " flags 0x" << std::hex << hf << std::dec << "\n";
        } else if (o == "va_delta") {


            need(3);
            void* a = object(t[1]);
            std::uint64_t va = slot<std::uint64_t(__stdcall*)(void*)>(a, 11)(a);
            std::uint64_t vb;
            if (auto it = title.find(t[2]); it != title.end()) {
                vb = reinterpret_cast<std::uint64_t>(it->second);
            } else {
                void* b = object(t[2]);
                vb = slot<std::uint64_t(__stdcall*)(void*)>(b, 11)(b);
            }
            log << "  delta " << static_cast<long long>(va - vb) << "\n";
        } else if (o == "poke" || o == "peek") {

            need(4);
            void* r = object(t[1]);
            auto* p = reinterpret_cast<std::uint8_t*>(slot<std::uint64_t(__stdcall*)(void*)>(r, 11)(r)) + num(t[2]);
            if (o == "poke") {
                for (size_t i = 0; i + 1 < t[3].size(); i += 2) {
                    *p++ = static_cast<std::uint8_t>(std::stoul(t[3].substr(i, 2), nullptr, 16));
                }
            } else {
                log << "  bytes ";
                for (std::uint64_t i = 0; i < num(t[3]); ++i) {
                    char b[4];
                    std::snprintf(b, sizeof b, "%02x", p[i]);
                    log << b;
                }
                log << "\n";
            }
        } else if (o == "alloc_info" || o == "alloc_info2") {

            if (t.size() < 3) {
                throw Failure(o + " <device> <desc>...");
            }
            void* dev = object(t[1]);
            bool null_descs = t[2] == "null";
            std::vector<D3D12_RESOURCE_DESC1> descs;
            for (size_t i = 2; i < t.size() && !null_descs; ++i) {
                D3D12_RESOURCE_DESC1 d1{};
                std::memcpy(&d1, &static_cast<const D3D12_RESOURCE_DESC&>(parse_desc(t[i])), sizeof(D3D12_RESOURCE_DESC));
                descs.push_back(d1);
            }
            UINT count = null_descs ? 2 : static_cast<UINT>(descs.size());
            struct Info {
                std::uint64_t size, alignment;
            } info{0xcdcd, 0xcdcd};
            if (o == "alloc_info") {
                std::vector<D3D12_RESOURCE_DESC> plain;
                for (const auto& d1 : descs) {
                    D3D12_RESOURCE_DESC d;
                    std::memcpy(&d, &d1, sizeof d);
                    plain.push_back(d);
                }
                timed("alloc_info", [&] {
                    return slot<Info*(__stdcall*)(Info*, void*, UINT, UINT, const void*)>(dev, 25)(&info, dev, 0, count, null_descs ? nullptr : plain.data());
                });
                log << "  size " << static_cast<long long>(info.size) << " alignment " << info.alignment << "\n";
            } else {
                std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> each(std::max<size_t>(descs.size(), 1), {0xcdcd, 0xcdcd, 0xcdcd});
                timed("alloc_info2", [&] {
                    return slot<Info*(__stdcall*)(Info*, void*, UINT, UINT, const void*, void*)>(dev, 108)(&info, dev, 0, count,
                                                                                                         null_descs ? nullptr : descs.data(), each.data());
                });
                log << "  size " << static_cast<long long>(info.size) << " alignment " << info.alignment << " each";
                for (size_t i = 0; i < descs.size(); ++i) {
                    log << " " << static_cast<long long>(each[i].Offset) << "," << each[i].Alignment << "," << static_cast<long long>(each[i].SizeInBytes);
                }
                log << "\n";
            }
        } else if (o == "footprints") {

            need(6);
            void* dev = object(t[1]);
            D3D12_RESOURCE_DESC d{};
            bool null_desc = t[2] == "null";
            if (!null_desc) {
                d = parse_desc(t[2]);
            }
            UINT first = static_cast<UINT>(num(t[3])), count = static_cast<UINT>(num(t[4]));
            std::uint64_t base = num(t[5]);
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
            std::vector<UINT> rows(count);
            std::vector<UINT64> sizes(count);
            UINT64 total = 0;
            timed("footprints", [&] {
                slot<void(__stdcall*)(void*, const void*, UINT, UINT, UINT64, void*, UINT*, UINT64*, UINT64*)>(dev, 38)(
                    dev, null_desc ? nullptr : &d, first, count, base, layouts.data(), rows.data(), sizes.data(), &total);
                return 0;
            });
            log << "  total " << static_cast<long long>(total) << "\n";
            for (UINT i = 0; i < count && i < 16; ++i) {
                const auto& f = layouts[i];
                log << "  " << static_cast<long long>(f.Offset) << " f" << f.Footprint.Format << " " << f.Footprint.Width << "x" << f.Footprint.Height << "x"
                    << f.Footprint.Depth << " pitch " << f.Footprint.RowPitch << " rows " << static_cast<int>(rows[i]) << " row "
                    << static_cast<long long>(sizes[i]) << "\n";
            }
        } else if (o == "custom_heap") {
            need(4);
            void* dev = object(t[1]);
            D3D12_HEAP_PROPERTIES p{};
            auto* r = slot<D3D12_HEAP_PROPERTIES*(__stdcall*)(void*, void*, UINT, UINT)>(dev, 26)(&p, dev, static_cast<UINT>(num(t[2])),
                                                                                                 static_cast<UINT>(num(t[3])));
            log << "  same " << (r == &p) << " " << p.Type << "," << p.CPUPageProperty << "," << p.MemoryPoolPreference << "," << p.CreationNodeMask << ","
                << p.VisibleNodeMask << "\n";
        } else if (o == "page_pool") {

            if (t.size() < 4 || t.size() > 5) {
                throw Failure("page_pool <device> <address> <pages> [noout]");
            }
            void* dev = object(t[1]);
            std::size_t handle = 0xcdcd;
            HR hr = slot<HR(__stdcall*)(void*, std::uint64_t, UINT, std::size_t*)>(dev, 74)(dev, num(t[2]), static_cast<UINT>(num(t[3])),
                                                                                         t.size() > 4 ? nullptr : &handle);
            log << "  hr " << hr_text(hr) << " handle " << handle << "\n";
        } else if (o == "unpage_pool") {
            need(3);
            void* dev = object(t[1]);
            HR hr = slot<HR(__stdcall*)(void*, std::size_t)>(dev, 75)(dev, static_cast<std::size_t>(num(t[2])));
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "page_map") {


            if (t.size() < 5) {
                throw Failure("page_map <queue> <pool> <pool pages> <destination|nobatch> <kind:start:count>...");
            }
            void* q = object(t[1]);
            struct Range {
                UINT kind, start, count;
            };
            struct Batch {
                std::uint64_t destination;
                UINT count;
                const Range* ranges;
            };
            std::vector<Range> ranges;
            for (size_t i = 5; i < t.size(); ++i) {
                std::istringstream in(t[i]);
                std::string a, b, c;
                std::getline(in, a, ':');
                std::getline(in, b, ':');
                std::getline(in, c, ':');
                ranges.push_back({static_cast<UINT>(num(a)), static_cast<UINT>(num(b)), static_cast<UINT>(num(c))});
            }
            bool none = t[4] == "nobatch";
            Batch batch{none ? 0 : num(t[4]), static_cast<UINT>(ranges.size()), ranges.data()};
            HR hr = slot<HR(__stdcall*)(void*, UINT, const Batch*, std::uint64_t, UINT, UINT)>(q, queue_slot(q, 29))(q, 1, none ? nullptr : &batch, num(t[2]),
                                                                                                                 static_cast<UINT>(num(t[3])), 0);
            log << "  hr " << hr_text(hr) << "\n";
        } else if (o == "quiet") {


            PROFILE_SAMPLING.store(true, std::memory_order_relaxed);
        } else if (o == "end") {
            if (!build_file.empty() && backend.shader_build) {
                std::uint8_t build[32] = {};
                backend.shader_build(build);
                std::ofstream(build_file, std::ios::binary).write(reinterpret_cast<const char*>(build), 32);
            }
        } else {
            throw Failure("unknown operation " + o);
        }
    }
};

std::vector<std::vector<std::string>> parse(const std::string& text) {
    std::vector<std::vector<std::string>> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (auto c = line.find('#'); c != std::string::npos) {
            line.resize(c);
        }
        std::istringstream words(line);
        std::vector<std::string> t;
        std::string w;
        while (words >> w) {
            t.push_back(w);
        }
        if (!t.empty()) {
            lines.push_back(std::move(t));
        }
    }
    return lines;
}

}   


struct Sampler {
    HANDLE target = nullptr;
    std::atomic<bool> stop{false};
    std::vector<DWORD64> samples;
    std::thread worker;

    bool callers = false;
    DWORD64 span_start = 0, span_end = 0;

    void start() {
        callers = std::string(std::getenv("DRIVER_PROFILE")) == "caller";
        if (std::getenv("DRIVER_PROFILE_QUIET")) {
            PROFILE_SAMPLING.store(false, std::memory_order_relaxed);
        }
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS);
        samples.reserve(1 << 22);
        worker = std::thread([this] {

            while (!stop.load(std::memory_order_relaxed)) {
                if (!PROFILE_SAMPLING.load(std::memory_order_relaxed)) {
                    Sleep(1);
                    continue;
                }
                if (SuspendThread(target) != DWORD(-1)) {
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(target, &c)) {
                        DWORD64 rip = c.Rip;


                        if (callers && span_end != 0) {
                            for (int depth = 0; depth < 24; ++depth) {
                                if (c.Rip >= span_start && c.Rip < span_end) {
                                    rip = c.Rip;
                                    break;
                                }
                                DWORD64 base = 0;
                                auto* f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                                if (!f) {
                                    c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                                    c.Rsp += 8;
                                    continue;
                                }
                                void* handler = nullptr;
                                DWORD64 frame = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler, &frame, nullptr);
                                if (!c.Rip) {
                                    break;
                                }
                            }
                        }
                        samples.push_back(rip);
                    }
                    ResumeThread(target);
                }
                if (callers && span_end == 0) {
                    HMODULE m = GetModuleHandleA("danzi_backend.dll");
                    if (!m) {
                        m = GetModuleHandleA("victory_ref.dll");
                    }
                    MODULEINFO info{};
                    if (m && GetModuleInformation(GetCurrentProcess(), m, &info, sizeof info)) {
                        span_start = reinterpret_cast<DWORD64>(info.lpBaseOfDll);
                        span_end = span_start + info.SizeOfImage;
                    }
                }

                auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
                while (std::chrono::steady_clock::now() < until) {
                    YieldProcessor();
                }
            }
        });
    }

    void report() {
        stop = true;
        worker.join();
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        std::map<std::string, std::uint64_t> by;
        for (DWORD64 rip : samples) {
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 disp = 0;
            std::string name;
            if (SymFromAddr(process, rip, &disp, sym)) {
                name = sym->Name;

                if (std::string(std::getenv("DRIVER_PROFILE")) == "addr") {
                    char off[64];
                    std::snprintf(off, sizeof off, "+0x%llx (rva 0x%llx)", static_cast<unsigned long long>(disp),
                                  static_cast<unsigned long long>(rip - SymGetModuleBase64(process, rip)));
                    name += off;
                }
            } else {
                char mod[MAX_PATH] = "?";
                HMODULE m = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(rip), &m)) {
                    GetModuleFileNameA(m, mod, MAX_PATH);
                }
                name = std::string("[") + std::filesystem::path(mod).filename().string() + "]";
            }
            ++by[name];
        }
        std::vector<std::pair<std::uint64_t, std::string>> sorted;
        for (auto& [n, c] : by) {
            sorted.emplace_back(c, n);
        }
        std::sort(sorted.rbegin(), sorted.rend());
        std::fprintf(stderr, "profile: %zu samples\n", samples.size());
        for (std::size_t i = 0; i < sorted.size() && i < 45; ++i) {
            std::fprintf(stderr, "%6.2f%%  %s\n", 100.0 * static_cast<double>(sorted[i].first) / static_cast<double>(samples.size()), sorted[i].second.c_str());
        }
    }
};

int main(int argc, char** argv) {
    std::unique_ptr<Sampler> sampler;
    if (std::getenv("DRIVER_PROFILE")) {
        sampler = std::make_unique<Sampler>();
        sampler->start();
    }
    struct Report {
        Sampler* s;
        ~Report() {
            if (s) {
                s->report();
            }
        }
    } report{sampler.get()};
#ifdef DANZI_FIXED_BACKEND


    std::string fixed = DANZI_FIXED_BACKEND;
    std::vector<char*> args{argv[0], fixed.data()};
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }
    argc = static_cast<int>(args.size());
    argv = args.data();
#endif
    if (argc != 4) {
        std::fprintf(stderr, "usage: title-driver <backend.dll> <scenario.txt> <out-dir>\n");
        return 2;
    }
    try {
        fs::path out = argv[3];
        fs::create_directories(out);
        std::ifstream f(argv[2], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        auto lines = parse(ss.str());

        for (const auto& t : lines) {
            if (t[0] == "env" && t.size() == 3) {
                SetEnvironmentVariableA(t[1].c_str(), t[2].c_str());
                _putenv_s(t[1].c_str(), t[2].c_str());
            }
        }
        Backend backend(fs::absolute(argv[1]));
        World w{backend};
        RUNTIME_LOG = &w.log;
        int code = 0;
        std::ofstream partial(out / "log.txt", std::ios::binary);
        std::size_t written = 0;
        for (size_t n = 0; n < lines.size(); ++n) {
            const auto& t = lines[n];
            std::string joined;
            for (const auto& x : t) {
                joined += (joined.empty() ? "" : " ") + x;
            }
            w.log << "#" << n << " " << joined << "\n";


            {
                std::string so_far = w.log.str();
                partial.write(so_far.data() + written, static_cast<std::streamsize>(so_far.size() - written));
                partial.flush();
                written = so_far.size();
            }
            w.op_number = n;
            if (t[0] == "env") {
                continue;
            }
            try {
                w.op(t);
            } catch (const Failure& e) {
                w.log << "  error " << e.what() << "\n";
                code = 1;
                break;
            }
        }
        partial.close();
        std::ofstream(out / "log.txt", std::ios::binary) << w.log.str();
        std::ofstream(out / "timing.txt", std::ios::binary) << w.timing.str();
        return code;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "title-driver: %s\n", e.what());
        return 1;
    }
}
