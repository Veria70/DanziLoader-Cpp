// SPDX-License-Identifier: GPL-3.0-or-later
// Process-wide state (`victory-hle/src/runtime.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See runtime.hpp.
#include "runtime.hpp"

#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <charconv>
#include <mutex>

#pragma comment(lib, "winmm.lib")

namespace danzi::hle::runtime {

namespace fs = std::filesystem;

namespace {

std::atomic<std::uintptr_t> MAIN_IMAGE_BASE{0};

std::mutex PATHS_LOCK;
std::vector<std::pair<std::uintptr_t, fs::path>> IMAGE_PATHS;

std::mutex LOADER_LOCK;
std::optional<TitleDllLoader> TITLE_DLL_LOADER;

std::mutex TITLE_LOCK;
std::optional<Title> TITLE;

bool ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && ascii_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && ascii_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    std::string out;
    for (;;) {
        std::size_t k = s.find(from, at);
        if (k == std::string::npos) {
            out.append(s, at, std::string::npos);
            return out;
        }
        out.append(s, at, k - at);
        out += to;
        at = k + from.size();
    }
}

bool ieq_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t k = 0; k < a.size(); ++k) {
        char x = a[k], y = b[k];
        if (x >= 'A' && x <= 'Z') {
            x = static_cast<char>(x + 32);
        }
        if (y >= 'A' && y <= 'Z') {
            y = static_cast<char>(y + 32);
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}


template <class T>
std::optional<T> parse_unsigned(std::string_view v, int base) {
    if (!v.empty() && v.front() == '+') {
        v.remove_prefix(1);
    }
    if (v.empty()) {
        return std::nullopt;
    }
    T out{};
    auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out, base);
    if (ec != std::errc() || end != v.data() + v.size()) {
        return std::nullopt;
    }
    return out;
}

}   

void set_main_image(std::uintptr_t base) { MAIN_IMAGE_BASE.store(base, std::memory_order_release); }

std::optional<std::uintptr_t> main_image() {
    std::uintptr_t b = MAIN_IMAGE_BASE.load(std::memory_order_acquire);
    return b ? std::optional(b) : std::nullopt;
}

void set_image_path(std::uintptr_t base, const fs::path& path) {
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    if (ec) {
        p = path;
    }
    std::lock_guard l(PATHS_LOCK);
    std::erase_if(IMAGE_PATHS, [&](const auto& e) { return e.first == base; });
    IMAGE_PATHS.emplace_back(base, p);
}

void forget_image(std::uintptr_t base) {
    std::lock_guard l(PATHS_LOCK);
    std::erase_if(IMAGE_PATHS, [&](const auto& e) { return e.first == base; });
}

std::optional<fs::path> image_path(std::uintptr_t base) {
    std::lock_guard l(PATHS_LOCK);
    for (const auto& [b, p] : IMAGE_PATHS) {
        if (b == base) {
            return p;
        }
    }
    return std::nullopt;
}

std::optional<std::uintptr_t> image_named(std::string_view file) {
    std::lock_guard l(PATHS_LOCK);
    for (const auto& [b, p] : IMAGE_PATHS) {
        if (p.has_filename()) {
            auto u8 = p.filename().u8string();
            if (ieq_ascii(std::string_view(reinterpret_cast<const char*>(u8.data()), u8.size()), file)) {
                return b;
            }
        }
    }
    return std::nullopt;
}

void set_title_dll_loader(TitleDllLoader loader) {
    std::lock_guard l(LOADER_LOCK);
    if (!TITLE_DLL_LOADER) {
        TITLE_DLL_LOADER = std::move(loader);
    }
}

std::pair<std::optional<std::uintptr_t>, std::optional<std::string>> load_title_dll(const std::string& file) {
    TitleDllLoader* f = nullptr;
    {
        std::lock_guard l(LOADER_LOCK);
        if (TITLE_DLL_LOADER) {
            f = &*TITLE_DLL_LOADER;
        }
    }
    if (!f) {
        return {std::nullopt, std::nullopt};
    }
    return (*f)(file);
}

std::vector<std::pair<std::string, std::string>> elements(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::string_view> path;
    std::string_view rest = text;
    for (;;) {
        std::size_t open = rest.find('<');
        if (open == std::string_view::npos) {
            break;
        }
        rest = rest.substr(open);
        std::size_t end;
        if (rest.starts_with("<!--")) {
            end = rest.find("-->");
            end = end == std::string_view::npos ? end : end + 3;
        } else if (rest.starts_with("<![CDATA[")) {
            end = rest.find("]]>");
            end = end == std::string_view::npos ? end : end + 3;
        } else {
            end = rest.find('>');
            end = end == std::string_view::npos ? end : end + 1;
        }
        if (end == std::string_view::npos) {
            break;
        }
        std::string_view tag = rest.substr(1, end - 2);
        rest = rest.substr(end);
        if (!tag.empty() && (tag.front() == '!' || tag.front() == '?')) {
            continue;
        }
        if (!tag.empty() && tag.front() == '/') {
            std::string_view name = trim(tag.substr(1));
            for (std::size_t at = path.size(); at-- > 0;) {
                if (path[at] == name) {
                    path.resize(at);
                    break;
                }
            }
            continue;
        }
        bool closed = !tag.empty() && tag.back() == '/';
        std::string_view stripped = tag;
        while (!stripped.empty() && stripped.back() == '/') {
            stripped.remove_suffix(1);
        }

        std::size_t s = 0;
        while (s < stripped.size() && ascii_space(stripped[s])) {
            ++s;
        }
        std::size_t e = s;
        while (e < stripped.size() && !ascii_space(stripped[e])) {
            ++e;
        }
        path.push_back(stripped.substr(s, e - s));
        std::size_t lt = rest.find('<');
        std::string body(trim(rest.substr(0, lt == std::string_view::npos ? rest.size() : lt)));
        for (auto [entity, c] : {std::pair{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}}) {
            body = replace_all(body, entity, c);
        }
        std::string joined;
        for (std::size_t k = 0; k < path.size(); ++k) {
            if (k) {
                joined += '/';
            }
            joined += path[k];
        }
        out.emplace_back(std::move(joined), std::move(body));
        if (closed) {
            path.pop_back();
        }
    }
    return out;
}

Title Title::from_manifest(std::string_view text) {
    auto els = elements(text);
    auto get = [&](std::string_view p) -> std::optional<std::string_view> {
        for (const auto& [path, body] : els) {
            if (path == p) {
                return std::string_view(body);
            }
        }
        return std::nullopt;
    };
    Title t;
    if (auto v = get("Game/TitleId")) {
        t.id = parse_unsigned<std::uint32_t>(*v, 16).value_or(0);
    }
    if (auto v = get("Game/StoreId")) {
        t.store_id = std::string(*v);
    }
    if (auto v = get("Game/PersistentLocalStorage/SizeMB")) {
        t.storage_bytes = parse_unsigned<std::uint64_t>(*v, 10).value_or(0) << 20;
    }
    return t;
}

fs::path Title::saves() const {
    fs::path p = storage;
    return p.replace_filename("saves");
}

void set_title(Title title) {
    std::lock_guard l(TITLE_LOCK);
    TITLE = std::move(title);
}

Title title() {
    std::lock_guard l(TITLE_LOCK);
    return TITLE.value_or(Title{});
}

void use_console_timers() {
    PROCESS_POWER_THROTTLING_STATE keep{};
    keep.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    keep.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    keep.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &keep, sizeof keep);
    timeBeginPeriod(1);
}

}   
