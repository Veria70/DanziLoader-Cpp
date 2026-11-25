// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/device.rs and console.rs (see device.hpp).
#include "device.hpp"

#include <windows.h>

#include <algorithm>

namespace danzi::d3d12x {

namespace {


constexpr HRESULT UNSUPPORTED = static_cast<HRESULT>(0x887A0004u);

constexpr std::uint32_t HIGHEST_FEATURE_LEVEL = 0xc200;


constexpr std::array<Iid, 9> LEVEL_IIDS = {
    guid(0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}),
    guid(0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}),
    guid(0x81dadc15, 0x2bad, 0x4392, {0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98}),
    guid(0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}),
    guid(0x8b4f173b, 0x2fea, 0x4b80, {0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d}),
    guid(0xc70b221b, 0x40e4, 0x4a17, {0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc}),
    guid(0x5c014b53, 0x68a1, 0x4b9b, {0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b}),
    guid(0x728f1fa2, 0xea11, 0x4a9f, {0x9b, 0x52, 0x9b, 0x42, 0xa4, 0x84, 0xd5, 0xc9}),
    guid(0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}),
};


struct Device {
    Object object;
    std::uint64_t null_constant_buffer;
    std::uint32_t out_of_line[16];
};
static_assert(sizeof(Device) == 0x58);
static_assert(offsetof(Device, out_of_line) == 0x18);

std::atomic<bool> NEWER{false};
std::atomic<bool> SERIES_S{false};

HRESULT __stdcall query_interface(This self, const Iid* iid, This* out) {
    This found = is_device(iid) ? self : is(iid, DXGI_DEVICE_IIDS) ? dxgi_device() : nullptr;
    return answer(DEVICE, iid, found, out);
}

std::uint32_t __stdcall node_count(This) { return 1; }


std::uint32_t __stdcall increment_size(This, std::uint32_t kind) {
    constexpr std::uint32_t SIZES[4] = {32, 16, 128, 128};
    return kind < 4 ? SIZES[kind] : 0;
}


constexpr std::uint32_t OPTIONS[16] = {1, 1, 2, 3, 3, 1, 1, 1, 3, 40, 0, 0, 0, 0, 1, 2};

struct FeatureLevels {
    std::uint32_t count;
    const std::uint32_t* requested;
    std::uint32_t highest;
};

HRESULT __stdcall check_feature_support(This, std::uint32_t feature, void* data, std::uint32_t size) {
    std::size_t expected;
    switch (feature) {
        case 0: expected = sizeof OPTIONS; break;
        case 1: expected = 16; break;
        case 2: expected = sizeof(FeatureLevels); break;
        case 3: expected = 12; break;
        case 4: expected = 16; break;
        default: return E_NOTIMPL_;
    }
    if (!data || size != expected) {
        return E_INVALIDARG_;
    }
    switch (feature) {
        case 0:
            std::memcpy(data, OPTIONS, sizeof OPTIONS);
            return S_OK_;
        case 1: {
            auto* a = static_cast<std::uint32_t*>(data);
            if (a[0] != 0) {
                return E_INVALIDARG_;   
            }

            a[1] = 0;
            a[2] = 1;
            a[3] = 1;
            return S_OK_;
        }
        case 3: {
            auto* q = static_cast<std::uint32_t*>(data);
            q[1] = 0x7fffffff;
            q[2] = 0xffff;
            return S_OK_;
        }
        case 4: {
            auto* q = static_cast<std::uint32_t*>(data);
            q[3] = (q[1] == 1 || q[1] == 2 || q[1] == 4 || q[1] == 8) ? 1 : 0;
            return S_OK_;
        }
        default: {
            auto* q = static_cast<FeatureLevels*>(data);
            if (!q->requested) {
                return E_INVALIDARG_;
            }
            bool any = false;
            std::uint32_t best = 0;
            for (std::uint32_t i = 0; i < q->count; ++i) {
                if (q->requested[i] <= HIGHEST_FEATURE_LEVEL && (!any || q->requested[i] > best)) {
                    best = q->requested[i];
                    any = true;
                }
            }
            if (!any) {
                return UNSUPPORTED;
            }
            q->highest = best;
            return S_OK_;
        }
    }
}


HRESULT __stdcall unknown_pipeline(This, const std::uint8_t*, const Iid*, This* out) {
    if (out) {
        *out = nullptr;
    }
    return E_NOTIMPL_;
}

const void* const* functions() {


    static const Table<160> table = [] {
        auto f = d3d12_object(missing_table<DEVICE, 160>());
        f[0] = fn(&query_interface);
        f[7] = fn(&node_count);
        f[13] = fn(&check_feature_support);
        f[15] = fn(&increment_size);
        f[57] = fn(&unknown_pipeline);
        f[67] = fn(&unknown_pipeline);
        f[69] = fn(&ignore);   
        return f;
    }();
    return table.data();
}

}   

bool is_device(const Iid* iid) { return is(iid, &DEVICE_IID, 1) || is(iid, LEVEL_IIDS); }

This device() {
    static Device d{Object{functions()}, 0, {1}};
    return &d.object;
}

bool newer() { return NEWER.load(std::memory_order_relaxed); }

HRESULT __stdcall create_device(const void*, const std::uint32_t* parameters, const Iid* iid, This* out) {
    if (!parameters || !iid) {
        return E_INVALIDARG_;
    }
    if (!is_device(iid)) {
        return E_NOINTERFACE_;
    }
    if (!out) {
        return S_FALSE_;
    }
    if (*iid == LEVEL_IIDS[8]) {
        NEWER.store(true, std::memory_order_relaxed);
    }
    return answer(DEVICE, iid, device(), out);
}

HRESULT __stdcall configure_virtual_memory(VirtualMemory* config) {
    if (!config) {
        return E_INVALIDARG_;
    }

    SYSTEM_INFO info;
    GetSystemInfo(&info);
    config->lowest = reinterpret_cast<std::uint64_t>(info.lpMinimumApplicationAddress);
    config->highest = reinterpret_cast<std::uint64_t>(info.lpMaximumApplicationAddress);
    return S_OK_;
}

void set_console(Console c) { SERIES_S.store(c == Console::SeriesS, std::memory_order_relaxed); }

Console current_console() { return SERIES_S.load(std::memory_order_relaxed) ? Console::SeriesS : Console::SeriesX; }

std::pair<std::size_t, std::size_t> title_memory(Console c) {
    constexpr std::size_t GIB = std::size_t{1} << 30;
    return c == Console::SeriesX ? std::pair{10 * GIB, 7 * GIB / 2} : std::pair{std::size_t{0}, std::size_t{7776} << 20};
}

}   
