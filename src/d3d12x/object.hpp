// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/object.rs (DanziLoader / victory-perf, see


#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

namespace danzi::d3d12x {

using HRESULT = std::int32_t;
inline constexpr HRESULT S_OK_ = 0;
inline constexpr HRESULT S_FALSE_ = 1;
inline constexpr HRESULT E_NOTIMPL_ = static_cast<HRESULT>(0x80004001u);
inline constexpr HRESULT E_NOINTERFACE_ = static_cast<HRESULT>(0x80004002u);
inline constexpr HRESULT E_POINTER_ = static_cast<HRESULT>(0x80004003u);
inline constexpr HRESULT E_INVALIDARG_ = static_cast<HRESULT>(0x80070057u);
inline constexpr HRESULT E_OUTOFMEMORY_ = static_cast<HRESULT>(0x8007000Eu);

inline constexpr HRESULT NOT_FOUND = static_cast<HRESULT>(0x887A0002u);
inline constexpr HRESULT MORE_DATA = static_cast<HRESULT>(0x887A0003u);

using Iid = std::array<std::uint8_t, 16>;


constexpr Iid guid(std::uint32_t a, std::uint16_t b, std::uint16_t c, std::array<std::uint8_t, 8> d) {
    return {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(a >> 8), static_cast<std::uint8_t>(a >> 16), static_cast<std::uint8_t>(a >> 24),
            static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(b >> 8), static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(c >> 8),
            d[0],
            d[1],
            d[2],
            d[3],
            d[4],
            d[5],
            d[6],
            d[7]};
}

struct Object {
    const void* const* functions;
    std::uint32_t bits = 0;   
    std::atomic<std::uint32_t> refs{0};
};
static_assert(sizeof(Object) == 16);

using This = Object*;

enum Kind : int {
    DEVICE,
    DXGI_DEVICE,
    ADAPTER,
    FACTORY,
    OUTPUT,
    HEAP,
    RESOURCE,
    FENCE,
    QUEUE,
    ALLOCATOR,
    LIST,
    DESCRIPTOR_HEAP,
    ROOT_SIGNATURE,
    QUERY_HEAP,
    COMMAND_SIGNATURE,
    PIPELINE,
    KINDS
};

std::uint32_t __stdcall add_ref(This self);
std::uint32_t __stdcall release(This self);


inline HRESULT answer(Kind, const Iid*, This found, This* out) {
    if (!out) {
        return E_POINTER_;
    }
    if (found) {
        found->refs.fetch_add(1, std::memory_order_relaxed);
        *out = found;
        return S_OK_;
    }
    *out = nullptr;
    return E_NOINTERFACE_;
}

inline bool same(const Iid& a, const Iid& b) {
    std::uint64_t x[2], y[2];
    std::memcpy(x, a.data(), 16);
    std::memcpy(y, b.data(), 16);
    return ((x[0] ^ y[0]) | (x[1] ^ y[1])) == 0;
}

inline bool is(const Iid* iid, const Iid* known, std::size_t count) {
    if (!iid) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (same(known[i], *iid)) {
            return true;
        }
    }
    return false;
}
template <std::size_t N>
bool is(const Iid* iid, const std::array<Iid, N>& known) {
    return is(iid, known.data(), N);
}


HRESULT __stdcall get_private_data(This self, const Iid* id, std::uint32_t* size, std::uint8_t* data);
HRESULT __stdcall set_private_data(This self, const Iid* id, std::uint32_t size, const std::uint8_t* data);
HRESULT __stdcall set_name(This self, const wchar_t* name);

void forget_private_data(This self);

HRESULT __stdcall parent_device(This self, const Iid* iid, This* out);

HRESULT __stdcall ignore();


template <std::size_t N>
using Table = std::array<const void*, N>;

namespace detail {
void report_missing(Kind kind, std::size_t slot);
template <int KIND, std::size_t SLOT>
HRESULT __stdcall missing() {
    report_missing(static_cast<Kind>(KIND), SLOT);
    return E_NOTIMPL_;
}
template <int KIND, std::size_t... I>
Table<sizeof...(I)> missing_table(std::index_sequence<I...>) {
    return {{reinterpret_cast<const void*>(&missing<KIND, I>)...}};
}
}   

template <int KIND, std::size_t N>
Table<N> missing_table() {
    static_assert(N <= 160);
    auto t = detail::missing_table<KIND>(std::make_index_sequence<N>{});
    t[1] = reinterpret_cast<const void*>(&add_ref);
    t[2] = reinterpret_cast<const void*>(&release);
    return t;
}


template <std::size_t N>
Table<N> d3d12_object(Table<N> t) {
    t[3] = reinterpret_cast<const void*>(&get_private_data);
    t[4] = reinterpret_cast<const void*>(&set_private_data);
    t[6] = reinterpret_cast<const void*>(&set_name);
    return t;
}

template <class F>
const void* fn(F f) {
    return reinterpret_cast<const void*>(f);
}

}   
