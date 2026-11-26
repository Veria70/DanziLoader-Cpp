// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/rebar.rs (see rebar.hpp).
#include "rebar.hpp"

#include <dxgi1_4.h>

#include <atomic>
#include <mutex>
#include <string>

#include "core/env.hpp"
#include "core/log.hpp"

namespace danzi::gpu::rebar {

namespace {

std::atomic<int> SUPPORTED{-1};   
std::atomic<std::uint64_t> PLACED[3]{};

std::atomic<std::uint64_t> NEXT_LINE{std::uint64_t{64} << 20};

std::string_view trim(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) {
        v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) {
        v.remove_suffix(1);
    }
    return v;
}

std::optional<std::string> narrow(const wchar_t* name) {
    auto v = env::get(name);
    if (!v) {
        return std::nullopt;
    }
    std::string s;
    for (wchar_t c : *v) {
        s.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return s;
}


std::optional<std::uint64_t> rebar_mb() {
    auto v = narrow(L"VICTORY_REBAR_MB");
    if (!v) {
        return std::nullopt;
    }
    std::string_view t = trim(*v);
    if (!t.empty() && t.front() == '+') {
        t.remove_prefix(1);
    }
    if (t.empty()) {
        return std::nullopt;
    }
    std::uint64_t n = 0;
    for (char c : t) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (n > (~std::uint64_t{0} - d) / 10) {
            return std::nullopt;
        }
        n = n * 10 + d;
    }
    return n;
}

std::uint64_t most(Device& device) {
    static std::once_flag once;
    static std::uint64_t m = 0;
    std::call_once(once, [&] {
        if (auto mb = rebar_mb()) {
            m = *mb << 20;
        } else {
            auto vm = device.video_memory();
            m = default_most(vm ? std::optional<std::uint64_t>(vm->budget) : std::nullopt);
        }
    });
    return m;
}

}   

const char* name(Decision d) {
    switch (d) {
        case Decision::Rebar: return "Rebar";
        case Decision::Off: return "Off";
        case Decision::Unsupported: return "Unsupported";
        case Decision::NotThisKind: return "NotThisKind";
        case Decision::CpuRead: return "CpuRead";
        case Decision::OverBudget: return "OverBudget";
        case Decision::CardFull: return "CardFull";
    }
    return "?";
}

Decision decide(const Ask& ask) {
    if (ask.level == 0) {
        return Decision::Off;
    }
    if (!ask.supported) {
        return Decision::Unsupported;
    }
    if (ask.cpu_read) {
        return Decision::CpuRead;
    }
    if (ask.kind == Kind::Buffer && ask.level < 2) {
        return Decision::NotThisKind;
    }
    if (saturating_add(ask.placed, ask.size) > ask.most) {
        return Decision::OverBudget;
    }
    if (ask.card && saturating_add(ask.card->second, ask.size) > ask.card->first / 10 * 8) {
        return Decision::CardFull;
    }
    return Decision::Rebar;
}

std::uint8_t level_of(std::optional<std::string_view> value) {
    if (!value) {
        return 0;
    }
    auto v = trim(*value);
    return v == "1" ? 1 : v == "2" ? 2 : 0;
}

std::uint64_t default_most(std::optional<std::uint64_t> card_budget) {
    if (!card_budget) {
        return std::uint64_t{256} << 20;
    }
    return std::min<std::uint64_t>(*card_budget / 4, std::uint64_t{4} << 30);
}

std::uint8_t level() {
    static const std::uint8_t l = [] {
        auto v = narrow(L"VICTORY_REBAR");
        return level_of(v ? std::optional<std::string_view>(*v) : std::nullopt);
    }();
    return l;
}

bool supported() { return SUPPORTED.load(std::memory_order_relaxed) == 1; }

void probe(Device& device) {
    int expected = -1;
    bool supported = device.features().gpu_upload_heap;
    if (!SUPPORTED.compare_exchange_strong(expected, supported ? 1 : 0)) {
        return;
    }


    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
    bool known = SUCCEEDED(device.device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof o16));
    std::optional<std::uint64_t> card_budget;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(device.device()->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO own{};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &own))) {
            card_budget = own.Budget;
        }
    }
    std::uint8_t l = level();
    std::uint64_t most_mb = rebar_mb().value_or(default_most(card_budget) >> 20);
    const char* what = supported ? (l == 0 ? "available, not used (VICTORY_REBAR=1 or 2 places host copies there)"
                                   : l == 1 ? "used for windows of title memory"
                                            : "used for windows of title memory and small title buffers")
                                 : (l == 0 ? "not available"
                                           : "NOT available: VICTORY_REBAR is ignored, copies are made as before (Resizable BAR off in the firmware/driver, or no OS/driver support)");

    std::string message = std::string("Resizable BAR (D3D12 GPU upload heaps): ") + what;
    if (card_budget) {
        log::info("victory_gpu::rebar", message,
                  {{"gpu_upload_heaps", log::debug(supported)},
                   {"options16_known", log::debug(known)},
                   {"card_budget_mb", log::debug(*card_budget >> 20)},
                   {"level", log::debug(std::uint32_t{l})},
                   {"most_mb", log::debug(most_mb)}});
    } else {
        log::info("victory_gpu::rebar", message,
                  {{"gpu_upload_heaps", log::debug(supported)},
                   {"options16_known", log::debug(known)},
                   {"level", log::debug(std::uint32_t{l})},
                   {"most_mb", log::debug(most_mb)}});
    }
}

bool reserve(Device& device, Kind kind, std::uint64_t size, bool cpu_read) {
    std::uint8_t l = level();
    if (l == 0 || !supported()) {
        return false;
    }
    Ask ask;
    ask.level = l;
    ask.supported = true;
    ask.kind = kind;
    ask.size = size;
    ask.cpu_read = cpu_read;
    ask.placed = PLACED[0].load(std::memory_order_relaxed);
    ask.most = most(device);
    if (auto vm = device.video_memory()) {
        ask.card = std::make_pair(vm->budget, vm->usage);
    }
    Decision d = decide(ask);
    if (d != Decision::Rebar) {
        if (d == Decision::OverBudget || d == Decision::CardFull) {
            if (PLACED[2].fetch_add(1, std::memory_order_relaxed) == 0) {
                log::warn("victory_gpu::rebar", "Resizable BAR: a copy stays in the PC's memory (budget or card full)",
                          {{"kind", kind == Kind::Window ? "Window" : "Buffer"}, {"size", log::debug(size)}});
            }
        }
        return false;
    }
    std::uint64_t now = PLACED[0].fetch_add(size, std::memory_order_relaxed) + size;
    PLACED[1].fetch_add(1, std::memory_order_relaxed);
    constexpr std::uint64_t STEP = std::uint64_t{64} << 20;
    std::uint64_t line = NEXT_LINE.load(std::memory_order_relaxed);
    if (now >= line && NEXT_LINE.compare_exchange_strong(line, now / STEP * STEP + STEP, std::memory_order_relaxed)) {
        log::info("victory_gpu::rebar", "Resizable BAR: host copies placed in the card's mapped memory",
                  {{"placed_mb", log::debug(now >> 20)}, {"copies", log::debug(PLACED[1].load(std::memory_order_relaxed))}});
    }
    return true;
}

void released(std::uint64_t size) {
    PLACED[0].fetch_sub(size, std::memory_order_relaxed);
    PLACED[1].fetch_sub(1, std::memory_order_relaxed);
}

std::pair<std::uint64_t, std::uint64_t> placed() {
    return {PLACED[0].load(std::memory_order_relaxed), PLACED[1].load(std::memory_order_relaxed)};
}

}   
