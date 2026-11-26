// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/device.rs and console.rs (see device.hpp).
#include "device.hpp"
#include "descriptor.hpp"
#include "list.hpp"
#include "pipeline.hpp"
#include "resource.hpp"
#include "root.hpp"
#include "sync.hpp"
#include "core/log.hpp"
#include "core/title_copy.hpp"

#include <windows.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

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

HRESULT __stdcall query_interface(This self, const Iid* iid, This* out) {
    This found = is_device(iid) ? self : is(iid, DXGI_DEVICE_IIDS) ? dxgi_device() : nullptr;
    return answer(DEVICE, iid, found, out);
}

std::uint32_t __stdcall node_count(This) { return 1; }


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
        default:
            log::warn("victory_gpu::device", "device feature query not implemented", {{"feature", log::debug(feature)}, {"size", log::debug(size)}});
            return E_NOTIMPL_;
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


std::atomic<std::uint32_t> UNKNOWN_PIPELINE_CALLS[2]{};


std::optional<std::vector<std::uint64_t>> words(std::uint64_t at) {
    std::vector<std::uint64_t> w(16);
    if (at == 0 || !host::read_into(reinterpret_cast<const std::uint8_t*>(at), reinterpret_cast<std::uint8_t*>(w.data()), 128)) {
        return std::nullopt;
    }
    return w;
}


std::string words_text(const std::optional<std::vector<std::uint64_t>>& w) {
    if (!w) {
        return "None";
    }
    std::string text = "Some([";
    for (std::size_t i = 0; i < w->size(); ++i) {
        char b[24];
        std::snprintf(b, sizeof b, i == 0 ? "%llx" : ", %llx", static_cast<unsigned long long>((*w)[i]));
        text += b;
    }
    return text + "])";
}

template <std::size_t SLOT>
HRESULT __stdcall unknown_pipeline(This, const std::uint8_t* desc, const Iid*, This* out) {
    std::uint32_t calls = UNKNOWN_PIPELINE_CALLS[SLOT == 0x218 ? 1 : 0].fetch_add(1, std::memory_order_relaxed) + 1;
    if (out) {
        *out = nullptr;
    }
    if ((calls <= 4 || (calls & (calls - 1)) == 0) && log::enabled(log::Level::Warn, "victory_gpu::device")) {
        auto description = words(reinterpret_cast<std::uint64_t>(desc));
        auto first = description ? words((*description)[0]) : std::nullopt;
        log::warn("victory_gpu::device", "a pipeline creation method that is not implemented: the title goes without this pipeline",
                  {{"slot", log::hex(SLOT)}, {"calls", log::debug(calls)}, {"description", words_text(description)}, {"first_points_at", words_text(first)}});
    }
    return E_NOTIMPL_;
}

const void* const* functions() {


    static const Table<160> table = [] {
        auto f = d3d12_object(missing_table<DEVICE, 160>());
        f[0] = fn(&query_interface);
        f[7] = fn(&node_count);
        f[13] = fn(&check_feature_support);
        f[14] = fn(&create_descriptor_heap);
        f[15] = fn(&descriptor_increment_size);
        f[16] = fn(&create_root_signature);
        f[39] = fn(&create_query_heap);
        f[41] = fn(&create_command_signature);
        f[17] = fn(&create_cbv);
        f[18] = fn(&create_srv);
        f[19] = fn(&create_uav);
        f[20] = fn(&create_rtv);
        f[21] = fn(&create_dsv);
        f[22] = fn(&create_sampler);
        f[23] = fn(&copy_descriptors);
        f[24] = fn(&copy_descriptors_simple);
        f[61] = fn(&create_sampler_x);
        f[70] = fn(&create_placed_srv);
        f[71] = fn(&create_placed_uav);
        f[10] = fn(&create_graphics_pipeline);
        f[11] = fn(&create_compute_pipeline);
        f[62] = fn(&create_compute_pipeline_x);
        f[86] = fn(&create_pipeline_stream);
        f[57] = fn(&unknown_pipeline<0x1c8>);
        f[67] = fn(&unknown_pipeline<0x218>);
        f[69] = fn(&ignore);   
        f[8] = fn(&create_queue);
        f[9] = fn(&create_allocator);
        f[12] = fn(&create_list);
        f[73] = fn(&create_list_x);
        f[25] = fn(&allocation_info);
        f[26] = fn(&custom_heap_properties);
        f[27] = fn(&create_committed);
        f[28] = fn(&create_heap);
        f[29] = fn(&create_placed);
        f[38] = fn(&copyable_footprints);
        f[74] = fn(&register_page_pool);
        f[75] = fn(&unregister_page_pool);

        f[76] = fn(&create_committed);
        f[108] = fn(&allocation_info2);
        f[127] = fn(&create_committed3);
        f[133] = fn(&create_at_address);
        f[36] = fn(&create_fence);
        f[48] = fn(&create_queue_x);
        f[78] = fn(&frame_set_interval);
        f[79] = fn(&frame_schedule);
        f[80] = fn(&frame_wait);
        f[81] = fn(&frame_statistics);
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
    std::uint32_t version = *parameters;
    log::info("victory_gpu::device", "D3D12.X device", {{"version", log::hex(version)}});
    if (version != CHECKED_VERSION) {
        log::warn("victory_gpu::device", "device layouts were checked against " + log::hex(CHECKED_VERSION) + " only", {{"version", log::hex(version)}});
    }
    if (!is_device(iid)) {
        log::warn("victory_gpu::device", "device created as an unknown interface", {{"iid", guid_text(*iid)}});
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
    log::info("victory_gpu::device", "graphics virtual memory",
              {{"flags", log::debug(config->flags)}, {"page_table_pages", log::debug(config->page_table_pages)}, {"preserved_bytes", log::debug(config->preserved_bytes)}});

    SYSTEM_INFO info;
    GetSystemInfo(&info);
    config->lowest = reinterpret_cast<std::uint64_t>(info.lpMinimumApplicationAddress);
    config->highest = reinterpret_cast<std::uint64_t>(info.lpMaximumApplicationAddress);
    return S_OK_;
}

std::atomic<bool> detail::SERIES_S{false};

void set_console(Console c) { detail::SERIES_S.store(c == Console::SeriesS, std::memory_order_relaxed); }

std::pair<std::size_t, std::size_t> title_memory(Console c) {
    constexpr std::size_t GIB = std::size_t{1} << 30;
    return c == Console::SeriesX ? std::pair{10 * GIB, 7 * GIB / 2} : std::pair{std::size_t{0}, std::size_t{7776} << 20};
}

}   
