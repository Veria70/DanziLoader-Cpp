// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the view registry of crates/victory-gpu/src/inplace.rs.
#include "inplace.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>

#include "env.hpp"
#include "watch.hpp"

namespace danzi::inplace {

namespace {

constexpr usize K64 = usize{64} << 10;

struct View {
    usize bytes;
    usize section;
    std::uint64_t offset;
};

struct Views {

    std::map<usize, View> views;


    std::map<std::pair<usize, std::uint64_t>, std::vector<usize>> pages;

    std::vector<std::tuple<usize, std::uint64_t, bool>> sections;
};

SrwLock VIEWS_LOCK;
Views VIEWS;


std::map<usize, View>::const_iterator at_or_below(const std::map<usize, View>& m, usize at) {
    auto it = m.upper_bound(at);
    if (it == m.begin()) {
        return m.end();
    }
    return std::prev(it);
}

}   

void title_memory_section(usize section, std::uint64_t bytes, bool committed) {
    std::lock_guard lock(VIEWS_LOCK);
    auto& s = VIEWS.sections;
    s.erase(std::remove_if(s.begin(), s.end(), [&](const auto& t) { return std::get<0>(t) == section; }), s.end());
    s.emplace_back(section, bytes, committed);
}

void title_memory_view(usize at, usize len, usize section, std::uint64_t offset) {
    {
        std::lock_guard lock(VIEWS_LOCK);
        VIEWS.views.insert_or_assign(at, View{len, section, offset});
        for (usize k = 0; k < div_ceil(len, K64); ++k) {
            VIEWS.pages[{section, offset + static_cast<std::uint64_t>(k * K64)}].push_back(at + k * K64);
        }
    }


    watch::views_mapped(section, offset, len);
}

void views_gone(usize at, usize len) {
    std::lock_guard lock(VIEWS_LOCK);
    Views& v = VIEWS;
    if (v.views.empty()) {
        return;
    }
    usize end = saturating_add(at, len);
    std::vector<usize> gone;
    if (auto it = at_or_below(v.views, at); it != v.views.end() && it->first + it->second.bytes > at) {
        gone.push_back(it->first);
    }
    usize from = saturating_add(at, 1);
    if (from < end) {
        for (auto it = v.views.lower_bound(from); it != v.views.end() && it->first < end; ++it) {
            gone.push_back(it->first);
        }
    }
    for (usize base : gone) {
        auto it = v.views.find(base);
        if (it == v.views.end()) {
            continue;
        }
        View view = it->second;
        v.views.erase(it);
        for (usize k = 0; k < div_ceil(view.bytes, K64); ++k) {
            auto key = std::make_pair(view.section, view.offset + static_cast<std::uint64_t>(k * K64));
            auto p = v.pages.find(key);
            if (p != v.pages.end()) {
                usize page = base + k * K64;
                auto& shown = p->second;
                shown.erase(std::remove(shown.begin(), shown.end(), page), shown.end());
                if (shown.empty()) {
                    v.pages.erase(p);
                }
            }
        }
    }
}

std::vector<usize> pages_of(usize page) {
    std::lock_guard lock(VIEWS_LOCK);
    auto it = at_or_below(VIEWS.views, page);
    if (it == VIEWS.views.end()) {
        return {};
    }
    usize base = it->first;
    const View& view = it->second;
    usize within = page - base;   
    if (within >= view.bytes || within % K64 != 0) {
        return {};
    }
    auto p = VIEWS.pages.find({view.section, view.offset + static_cast<std::uint64_t>(within)});
    return p == VIEWS.pages.end() ? std::vector<usize>{} : p->second;
}

AliasBlocks alias_blocks_in(usize from_block, usize to_block, usize max_views) {
    AliasBlocks out;
    usize start = from_block > std::numeric_limits<usize>::max() / K64 ? std::numeric_limits<usize>::max() : from_block * K64;
    usize end = to_block > std::numeric_limits<usize>::max() / K64 ? std::numeric_limits<usize>::max() : to_block * K64;
    if (start >= end) {
        return out;
    }
    std::lock_guard lock(VIEWS_LOCK);
    const Views& v = VIEWS;
    if (v.views.empty()) {
        return out;
    }
    std::vector<std::pair<usize, View>> overlapping;
    if (auto it = at_or_below(v.views, start); it != v.views.end() && it->first + it->second.bytes > start) {
        overlapping.emplace_back(it->first, it->second);
    }
    for (auto it = v.views.lower_bound(saturating_add(start, 1)); it != v.views.end() && it->first < end; ++it) {
        overlapping.emplace_back(it->first, it->second);
    }
    for (const auto& [base, view] : overlapping) {
        usize first = std::max(base / K64, from_block);
        usize last = std::min(div_ceil(saturating_add(base, view.bytes), K64), to_block);
        for (usize block = first; block < last; ++block) {
            usize page = block * K64;
            usize within = page - base;
            if (within >= view.bytes || within % K64 != 0) {
                continue;
            }
            auto p = v.pages.find({view.section, view.offset + static_cast<std::uint64_t>(within)});
            if (p == v.pages.end()) {
                continue;
            }
            const auto& shown = p->second;
            if (shown.size() < 2 || shown.size() > max_views) {
                continue;
            }
            out.blocks_counted += 1;
            for (usize alias_addr : shown) {
                usize alias = alias_addr / K64;
                out.all_aliased.push_back(alias);
                if (alias != block) {
                    out.others.push_back(alias);
                }
            }
        }
    }
    std::sort(out.others.begin(), out.others.end());
    out.others.erase(std::unique(out.others.begin(), out.others.end()), out.others.end());
    return out;
}

std::vector<std::pair<usize, usize>> views_of_section(usize section, std::uint64_t offset, usize len) {
    if (len == 0) {
        return {};
    }
    std::lock_guard lock(VIEWS_LOCK);
    std::uint64_t end = offset > std::numeric_limits<std::uint64_t>::max() - len ? std::numeric_limits<std::uint64_t>::max() : offset + len;
    std::vector<usize> pages;
    std::uint64_t at = offset - offset % K64;
    while (at < end) {
        auto p = VIEWS.pages.find({section, at});
        if (p != VIEWS.pages.end() && p->second.size() <= watch::MANY_VIEWS) {
            pages.insert(pages.end(), p->second.begin(), p->second.end());
        }
        if (at > std::numeric_limits<std::uint64_t>::max() - K64) {
            break;
        }
        at += K64;
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    std::vector<std::pair<usize, usize>> runs;
    runs.reserve(pages.size());
    for (usize page : pages) {
        if (!runs.empty() && runs.back().first + runs.back().second == page) {
            runs.back().second += K64;
        } else {
            runs.emplace_back(page, K64);
        }
    }
    return runs;
}


std::atomic<std::uint64_t> STATS[8]{};

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

FreeingHook FREEING_HOOK = nullptr;
void* FREEING_CONTEXT = nullptr;

}   

std::uint8_t level() {

    static const std::uint8_t l = [] {
        auto v = env_u64(L"VICTORY_INPLACE");
        return v && *v <= 255 ? static_cast<std::uint8_t>(*v) : std::uint8_t{0};
    }();
    return l;
}

std::uint64_t most() {
    static const std::uint64_t m = env_u64(L"VICTORY_INPLACE_MB").value_or(4096) << 20;
    return m;
}

void gone(bool window, std::uint64_t bytes) {
    usize at = window ? 0 : 2;
    STATS[at].fetch_sub(1, std::memory_order_relaxed);
    STATS[at + 1].fetch_sub(bytes, std::memory_order_relaxed);
}

Cover cover(usize at, usize len) {
    std::lock_guard lock(VIEWS_LOCK);
    const auto& views = VIEWS.views;
    usize end = at + std::max<usize>(len, 1);
    auto it = at_or_below(views, at);
    if (it == views.end() || it->first + it->second.bytes <= at) {
        return {};
    }
    usize first_section = it->second.section;
    std::uint64_t first_offset = it->second.offset + static_cast<std::uint64_t>(at - it->first);
    usize reached = it->first + it->second.bytes;
    bool one = true;
    while (reached < end) {
        auto next = views.find(reached);
        if (next == views.end()) {
            return {CoverKind::Holes, 0, 0};
        }
        one = one && next->second.section == first_section && next->second.offset == first_offset + static_cast<std::uint64_t>(reached - at);
        reached += next->second.bytes;
    }
    return one ? Cover{CoverKind::Run, first_section, first_offset} : Cover{CoverKind::Scattered, 0, 0};
}

bool private_whole(usize at, usize len) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof info) == 0) {
        return false;
    }
    return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.Protect == PAGE_READWRITE &&
           saturating_add(reinterpret_cast<usize>(info.BaseAddress), info.RegionSize) >= saturating_add(at, len);
}

void set_freeing_hook(FreeingHook hook, void* context) {
    FREEING_CONTEXT = context;
    FREEING_HOOK = hook;
}

void title_memory_freeing(usize at, usize len) {
    if (STATS[0].load(std::memory_order_relaxed) + STATS[2].load(std::memory_order_relaxed) == 0 || !FREEING_HOOK) {
        return;
    }
    std::uint64_t n = FREEING_HOOK(FREEING_CONTEXT, at, len);
    STATS[7].fetch_add(n, std::memory_order_relaxed);
}

}   
