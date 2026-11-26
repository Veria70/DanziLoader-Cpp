// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/object.rs (see object.hpp).
#include "object.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <tuple>
#include <mutex>
#include <vector>

#include "core/log.hpp"
#include "core/win.hpp"
#include "device.hpp"

namespace danzi::d3d12x {

namespace {

danzi::FastLock PRIVATE_LOCK;


using Key = std::tuple<std::uintptr_t, std::uint64_t, std::uint64_t>;
std::vector<std::pair<Key, std::vector<std::uint8_t>>> PRIVATE;

std::atomic<std::size_t> PRIVATE_COUNT{0};

auto find_at(const Key& k) {
    return std::lower_bound(PRIVATE.begin(), PRIVATE.end(), k, [](const auto& e, const Key& x) { return e.first < x; });
}

Key key_of(This self, const Iid& id) {
    std::uint64_t w[2];
    std::memcpy(w, id.data(), 16);
    return {reinterpret_cast<std::uintptr_t>(self), w[0], w[1]};
}

constexpr std::size_t MAX_SLOTS = 160;
std::atomic<bool> REPORTED[KINDS][MAX_SLOTS]{};

constexpr const char* KIND_NAMES[KINDS] = {
    "device",
    "DXGI device",
    "adapter",
    "factory",
    "output",
    "heap",
    "resource",
    "fence",
    "command queue",
    "command allocator",
    "command list",
    "descriptor heap",
    "root signature",
    "query heap",
    "command signature",
    "pipeline state",
};


std::string caller_text(const void* at) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<const wchar_t*>(at), &module)) {
        return "+" + log::hex(reinterpret_cast<std::uintptr_t>(at) - reinterpret_cast<std::uintptr_t>(module));
    }
    return log::hex(reinterpret_cast<std::uintptr_t>(at));
}

}   

std::uint32_t __stdcall add_ref(This self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

std::uint32_t __stdcall release(This self) {
    std::uint32_t n = self->refs.load(std::memory_order_relaxed);
    while (n > 0 && !self->refs.compare_exchange_weak(n, n - 1, std::memory_order_relaxed)) {
    }
    return self->refs.load(std::memory_order_relaxed);
}

HRESULT __stdcall get_private_data(This self, const Iid* id, std::uint32_t* size, std::uint8_t* data) {
    if (!id || !size) {
        return E_INVALIDARG_;
    }
    std::lock_guard lock(PRIVATE_LOCK);
    Key k = key_of(self, *id);
    auto it = find_at(k);
    if (it == PRIVATE.end() || it->first != k) {
        *size = 0;
        return NOT_FOUND;
    }
    auto len = static_cast<std::uint32_t>(it->second.size());
    std::uint32_t room = *size;
    *size = len;
    if (!data) {
        return S_OK_;
    }
    if (room < len) {
        return MORE_DATA;
    }
    std::memcpy(data, it->second.data(), len);
    return S_OK_;
}

HRESULT __stdcall set_private_data(This self, const Iid* id, std::uint32_t size, const std::uint8_t* data) {
    if (!id) {
        return E_INVALIDARG_;
    }
    std::lock_guard lock(PRIVATE_LOCK);
    Key key = key_of(self, *id);
    auto it = find_at(key);
    bool there = it != PRIVATE.end() && it->first == key;
    if (!data) {
        if (there) {
            PRIVATE.erase(it);
        }
    } else if (there) {
        it->second.assign(data, data + size);
    } else {
        PRIVATE.insert(it, {key, std::vector<std::uint8_t>(data, data + size)});
    }
    PRIVATE_COUNT.store(PRIVATE.size(), std::memory_order_release);
    return S_OK_;
}

void forget_private_data(This self) {


    if (PRIVATE_COUNT.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::lock_guard lock(PRIVATE_LOCK);
    auto at = reinterpret_cast<std::uintptr_t>(self);
    auto from = find_at(Key{at, 0, 0});
    auto to = from;
    while (to != PRIVATE.end() && std::get<0>(to->first) == at) {
        ++to;
    }
    PRIVATE.erase(from, to);
    PRIVATE_COUNT.store(PRIVATE.size(), std::memory_order_release);
}

HRESULT __stdcall set_name(This, const wchar_t*) { return S_OK_; }   

HRESULT __stdcall parent_device(This, const Iid* iid, This* out) { return answer(DEVICE, iid, is_device(iid) ? device() : nullptr, out); }

HRESULT __stdcall ignore() { return S_OK_; }

std::string guid_text(const Iid& g) {
    std::uint32_t a;
    std::uint16_t b, c;
    std::memcpy(&a, g.data(), 4);
    std::memcpy(&b, g.data() + 4, 2);
    std::memcpy(&c, g.data() + 6, 2);
    char text[40];
    std::snprintf(text, sizeof text, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", a, b, c, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return text;
}

void detail::unknown_interface(Kind kind, const Iid* iid) {
    log::warn("victory_gpu::object", "unknown interface", {{"object", log::debug(KIND_NAMES[kind])}, {"iid", iid ? guid_text(*iid) : std::string()}});
}

void detail::report_missing(Kind kind, std::size_t slot, const void* caller) {
    if (slot < MAX_SLOTS && !REPORTED[kind][slot].exchange(true, std::memory_order_relaxed)) {
        log::warn("victory_gpu::object", "method not implemented",
                  {{"object", log::debug(KIND_NAMES[kind])}, {"slot", log::hex(slot * 8)}, {"caller", log::debug(caller_text(caller))}});
    }
}

}   
