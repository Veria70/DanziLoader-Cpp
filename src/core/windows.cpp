// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the window choice of crates/victory-gpu/src/shadow.rs (see
// windows.hpp).
#include "windows.hpp"

#include <algorithm>
#include <string>

#include "env.hpp"

namespace danzi::windows {

namespace {


std::optional<std::uint64_t> env_u64(const wchar_t* name) {
    auto v = env::get(name);
    if (!v) {
        return std::nullopt;
    }
    std::wstring s = *v;
    auto b = s.find_first_not_of(L" \t\r\n");
    auto e = s.find_last_not_of(L" \t\r\n");
    s = b == std::wstring::npos ? L"" : s.substr(b, e - b + 1);
    if (!s.empty() && s[0] == L'+') {
        s = s.substr(1);
    }
    if (s.empty()) {
        return std::nullopt;
    }
    std::uint64_t n = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') {
            return std::nullopt;
        }
        std::uint64_t d = static_cast<std::uint64_t>(c - L'0');
        if (n > (~std::uint64_t{0} - d) / 10) {
            return std::nullopt;
        }
        n = n * 10 + d;
    }
    return n;
}

}   

std::uint64_t grid(std::optional<std::uint64_t> kb) {
    if (kb && *kb != 0 && (*kb & (*kb - 1)) == 0 && *kb >= 64 && *kb <= 4096) {
        return *kb << 10;
    }
    return WINDOW;
}

std::uint64_t window() {
    static const std::uint64_t g = grid(env_u64(L"VICTORY_WINDOW_KB"));
    return g;
}

Window window_of(std::uint64_t address, std::uint64_t end, std::uint64_t g) {
    std::uint64_t start = address & ~(g - 1);
    std::uint64_t top = (end + g - 1) / g * g;
    return {start, top - start};
}

bool tails() {
    static const bool on = env::is(L"VICTORY_WINDOW_TAILS", L"1");
    return on;
}

Window tail_of(std::uint64_t address, std::uint64_t end) {
    auto [cell, len] = window_of(address, end, WINDOW);
    std::uint64_t start = address & ~(TAIL_STEP - 1);
    return {start, cell + len - start};
}

std::optional<Window> holder(std::span<const Window> windows, std::uint64_t address, std::uint64_t end) {
    std::optional<Window> best;
    for (const auto& w : windows) {
        if (w.first <= address && w.first + w.second >= end) {

            if (!best || w.second > best->second || (w.second == best->second && w.first <= best->first)) {
                best = w;
            }
        }
    }
    return best;
}

bool fine() { return tails() || window() < WINDOW; }

std::uint64_t cover(std::span<const Window> windows) {
    std::uint64_t covered = 0, reached = 0;
    for (const auto& [start, bytes] : windows) {
        std::uint64_t end = start + bytes;
        if (end > reached) {
            covered += end - std::max(start, reached);
            reached = end;
        }
    }
    return covered;
}

std::optional<Window> longest_tail(std::span<const Window> windows, std::uint64_t address, std::uint64_t end) {
    auto [cell, len] = window_of(address, end, WINDOW);
    std::uint64_t step = address & ~(TAIL_STEP - 1);
    std::optional<Window> best;
    for (const auto& w : windows) {
        if (w.first >= cell && w.first <= step && w.first + w.second == cell + len) {
            if (!best || w < *best) {
                best = w;
            }
        }
    }
    return best;
}

std::uint64_t window_card_most() {
    static const std::uint64_t most = env_u64(L"VICTORY_CARD_WINDOWS_MB").value_or(1536) << 20;
    return most;
}

}   
