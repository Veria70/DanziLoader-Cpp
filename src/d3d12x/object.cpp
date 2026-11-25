// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/object.rs (see object.hpp).
#include "object.hpp"

#include <windows.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <mutex>
#include <vector>

#include "core/win.hpp"
#include "device.hpp"

namespace danzi::d3d12x {

namespace {

danzi::FastLock PRIVATE_LOCK;


using Key = std::tuple<std::uintptr_t, std::uint64_t, std::uint64_t>;
std::vector<std::pair<Key, std::vector<std::uint8_t>>> PRIVATE;

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
    return S_OK_;
}

void forget_private_data(This self) {
    std::lock_guard lock(PRIVATE_LOCK);
    auto at = reinterpret_cast<std::uintptr_t>(self);
    auto from = find_at(Key{at, 0, 0});
    auto to = from;
    while (to != PRIVATE.end() && std::get<0>(to->first) == at) {
        ++to;
    }
    PRIVATE.erase(from, to);
}

HRESULT __stdcall set_name(This, const wchar_t*) { return S_OK_; }   

HRESULT __stdcall parent_device(This, const Iid* iid, This* out) { return answer(DEVICE, iid, is_device(iid) ? device() : nullptr, out); }

HRESULT __stdcall ignore() { return S_OK_; }

void detail::report_missing(Kind kind, std::size_t slot) {
    if (slot < MAX_SLOTS) {
        REPORTED[kind][slot].exchange(true, std::memory_order_relaxed);   
    }
}

}   
