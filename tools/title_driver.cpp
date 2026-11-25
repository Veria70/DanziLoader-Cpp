// SPDX-License-Identifier: GPL-3.0-or-later


#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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


void* __stdcall runtime_alloc(std::size_t size, std::uint64_t  ) {
    return VirtualAlloc(nullptr, size ? size : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void __stdcall runtime_free(void* at, std::uint64_t  ) { VirtualFree(at, 0, MEM_RELEASE); }


int __stdcall runtime_map(std::uint64_t, std::uint64_t, std::uint64_t) { return 0; }


struct Backend {
    HMODULE dll = nullptr;
    void(__stdcall* use_runtime)(void*, void*, void*) = nullptr;
    HR(__stdcall* configure_vm)(void*) = nullptr;
    HR(__stdcall* create_device)(const void*, const std::uint32_t*, const GUID*, void**) = nullptr;
    const GUID*(__stdcall* device_iid)() = nullptr;
    void(__stdcall* set_console)(std::uint32_t) = nullptr;
    void(__stdcall* title_memory_remapped)(std::size_t, std::size_t) = nullptr;

    explicit Backend(const fs::path& path) {
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
            try {
                op(rest);
            } catch (...) {
                repeat = 1;
                throw;
            }
            repeat = 1;
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
        } else if (o == "quiet") {

        } else if (o == "end") {
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

int main(int argc, char** argv) {
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
        int code = 0;
        for (size_t n = 0; n < lines.size(); ++n) {
            const auto& t = lines[n];
            std::string joined;
            for (const auto& x : t) {
                joined += (joined.empty() ? "" : " ") + x;
            }
            w.log << "#" << n << " " << joined << "\n";
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
        std::ofstream(out / "log.txt", std::ios::binary) << w.log.str();
        std::ofstream(out / "timing.txt", std::ios::binary) << w.timing.str();
        return code;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "title-driver: %s\n", e.what());
        return 1;
    }
}
