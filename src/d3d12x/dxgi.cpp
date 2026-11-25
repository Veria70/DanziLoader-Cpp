// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/dxgi.rs (DanziLoader / victory-perf, see


#include <windows.h>

#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

#include "device.hpp"

namespace danzi::d3d12x {

const std::array<Iid, 3> DXGI_DEVICE_IIDS = {
    guid(0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}),
    guid(0x77db970f, 0x6276, 0x48ba, {0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c}),
    guid(0x05008617, 0xfbfd, 0x4051, {0xa7, 0x90, 0x14, 0x48, 0x84, 0xb4, 0xf6, 0xa9}),
};

namespace {

constexpr std::array<Iid, 3> ADAPTER_IIDS = {
    guid(0x2411e7e1, 0x12ac, 0x4ccf, {0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0}),
    guid(0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}),
    guid(0x0aa1ae0a, 0xfa0e, 0x4b84, {0x86, 0x44, 0xe0, 0x5f, 0xf8, 0xe5, 0xac, 0xb5}),
};
constexpr std::array<Iid, 3> FACTORY_IIDS = {
    guid(0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}),
    guid(0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}),
    guid(0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}),
};
constexpr std::array<Iid, 1> OUTPUT_IIDS = {guid(0xae02eedb, 0xc735, 0x4690, {0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa})};

constexpr std::uint32_t AMD = 0x1002;

This adapter();
This factory();
This output();

HRESULT query(Kind kind, const Iid* iid, const Iid* known, std::size_t n, This found, This* out) {
    return answer(kind, iid, is(iid, known, n) ? found : nullptr, out);
}

HRESULT __stdcall device_query(This self, const Iid* iid, This* out) {
    if (is_device(iid)) {
        return answer(DXGI_DEVICE, iid, device(), out);
    }
    return query(DXGI_DEVICE, iid, DXGI_DEVICE_IIDS.data(), 3, self, out);
}
HRESULT __stdcall device_parent(This, const Iid* iid, This* out) { return query(DXGI_DEVICE, iid, ADAPTER_IIDS.data(), 3, adapter(), out); }
HRESULT __stdcall device_adapter(This, This* out) { return answer(DXGI_DEVICE, &ADAPTER_IIDS[0], adapter(), out); }

HRESULT __stdcall adapter_query(This self, const Iid* iid, This* out) { return query(ADAPTER, iid, ADAPTER_IIDS.data(), 3, self, out); }
HRESULT __stdcall adapter_parent(This, const Iid* iid, This* out) { return query(ADAPTER, iid, FACTORY_IIDS.data(), 3, factory(), out); }
HRESULT __stdcall adapter_outputs(This, std::uint32_t index, This* out) {
    if (index != 0) {
        return NOT_FOUND;
    }
    return answer(ADAPTER, &OUTPUT_IIDS[0], output(), out);
}

struct AdapterDescription {
    std::uint16_t name[128];
    std::uint32_t vendor, device, subsystem, revision;
    std::size_t dedicated_video_memory, dedicated_system_memory, shared_system_memory;
    std::uint32_t luid[2];
};
static_assert(sizeof(AdapterDescription) == 0x130);

AdapterDescription described(Console c) {
    AdapterDescription d{};
    const char* text = "VIctory console GPU";
    for (std::size_t i = 0; text[i]; ++i) {
        d.name[i] = static_cast<std::uint16_t>(text[i]);
    }
    auto [gpu_optimal, standard] = title_memory(c);
    d.vendor = AMD;
    d.dedicated_video_memory = gpu_optimal;
    d.shared_system_memory = standard;
    d.luid[0] = 1;
    return d;
}


const AdapterDescription DESCRIBED[2] = {described(Console::SeriesX), described(Console::SeriesS)};

HRESULT __stdcall adapter_description(This, AdapterDescription* out) {
    if (!out) {
        return E_INVALIDARG_;
    }
    std::memcpy(out, &DESCRIBED[current_console() == Console::SeriesS ? 1 : 0], sizeof *out);
    return S_OK_;
}

HRESULT __stdcall factory_query(This self, const Iid* iid, This* out) { return query(FACTORY, iid, FACTORY_IIDS.data(), 3, self, out); }
HRESULT __stdcall factory_adapters(This, std::uint32_t index, This* out) {
    if (index != 0) {
        return NOT_FOUND;
    }
    return answer(FACTORY, &ADAPTER_IIDS[0], adapter(), out);
}

HRESULT __stdcall output_query(This self, const Iid* iid, This* out) { return query(OUTPUT, iid, OUTPUT_IIDS.data(), 1, self, out); }
HRESULT __stdcall output_parent(This, const Iid* iid, This* out) { return query(OUTPUT, iid, ADAPTER_IIDS.data(), 3, adapter(), out); }


struct Mode {
    std::uint32_t width, height;
    std::uint32_t refresh[2];
    std::uint32_t format, scanlines, scaling, flags;
};
static_assert(sizeof(Mode) == 32);


std::optional<std::uint32_t> parse_u32(std::string s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
    if (!s.empty() && s[0] == '+') {
        s = s.substr(1);
    }
    if (s.empty() || s.size() > 10) {
        return std::nullopt;
    }
    std::uint64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (n > 0xffffffffu) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(n);
}


std::pair<std::uint32_t, std::uint32_t> display_size() {
    static const std::pair<std::uint32_t, std::uint32_t> size = [] {
        char buf[64];
        DWORD n = GetEnvironmentVariableA("VICTORY_DISPLAY_MODE", buf, sizeof buf);
        if (n > 0 && n < sizeof buf) {
            std::string v(buf, n);
            auto b = v.find_first_not_of(" \t\r\n");
            auto e = v.find_last_not_of(" \t\r\n");
            v = b == std::string::npos ? "" : v.substr(b, e - b + 1);
            auto x = v.find_first_of("xX");
            if (x != std::string::npos) {
                auto w = parse_u32(v.substr(0, x));
                auto h = parse_u32(v.substr(x + 1));
                if (w && h && *w >= 640 && *w <= 7680 && *h >= 360 && *h <= 4320) {
                    return std::pair{*w, *h};
                }
            }
        }
        return std::pair<std::uint32_t, std::uint32_t>{3840, 2160};
    }();
    return size;
}

HRESULT __stdcall modes(This, std::uint32_t, std::uint32_t, std::uint32_t* count, Mode* out) {
    if (!count) {
        return E_INVALIDARG_;
    }
    if (!out) {
        *count = 1;
        return S_OK_;
    }
    if (*count < 1) {
        return MORE_DATA;
    }
    *count = 1;
    auto [w, h] = display_size();
    *out = Mode{w, h, {60, 1}, 0, 1, 0, 0};
    return S_OK_;
}

This adapter() {
    static const Table<24> t = [] {
        auto f = missing_table<ADAPTER, 24>();
        f[0] = fn(&adapter_query);
        f[6] = fn(&adapter_parent);
        f[7] = fn(&adapter_outputs);
        f[8] = fn(&adapter_description);
        return f;
    }();
    static Object o{t.data()};
    return &o;
}

This factory() {
    static const Table<32> t = [] {
        auto f = missing_table<FACTORY, 32>();
        f[0] = fn(&factory_query);
        f[7] = fn(&factory_adapters);
        f[12] = fn(&factory_adapters);   
        return f;
    }();
    static Object o{t.data()};
    return &o;
}

This output() {
    static const Table<32> t = [] {
        auto f = missing_table<OUTPUT, 32>();
        f[0] = fn(&output_query);
        f[6] = fn(&output_parent);
        f[21] = fn(&modes);
        return f;
    }();
    static Object o{t.data()};
    return &o;
}

}   

This dxgi_device() {
    static const Table<32> t = [] {
        auto f = missing_table<DXGI_DEVICE, 32>();
        f[0] = fn(&device_query);
        f[6] = fn(&device_parent);
        f[7] = fn(&device_adapter);
        return f;
    }();
    static Object o{t.data()};
    return &o;
}

}   
