// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/rebar.rs (see rebar.hpp).
#include "rebar.hpp"

#include <atomic>
#include <mutex>
#include <string>

#include "core/env.hpp"

namespace danzi::gpu::rebar {

namespace {

std::atomic<int> SUPPORTED{-1};   
std::atomic<std::uint64_t> PLACED[3]{};

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
    SUPPORTED.compare_exchange_strong(expected, device.features().gpu_upload_heap ? 1 : 0);
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
            PLACED[2].fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    PLACED[0].fetch_add(size, std::memory_order_relaxed);
    PLACED[1].fetch_add(1, std::memory_order_relaxed);
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
