// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/root.rs, signature.rs and query.rs (see
// root.hpp).
#include "root.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "core/log.hpp"
#include "resource_iids.hpp"

namespace danzi::d3d12x {

namespace {


const std::array<Iid, 3> ROOT_SIGNATURE_IIDS = {
    guid(0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}),
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};

constexpr std::uint32_t CONSOLE_FLAGS = 0xff000000u;


FARPROC host_export(const char* name) {
    static const HMODULE module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? GetProcAddress(module, name) : nullptr;
}

using Serialize = HRESULT(__stdcall*)(const RootDesc*, std::uint32_t, void**, void**);
using SerializeVersioned = HRESULT(__stdcall*)(const VersionedRootDesc*, void**, void**);
using Deserialize = HRESULT(__stdcall*)(const std::uint8_t*, std::size_t, const Iid*, void**);

struct RootSignature {
    Object object;

    std::unique_ptr<std::uint8_t[]> blob;
    std::size_t size;
};

HRESULT __stdcall root_query(This self, const Iid* iid, This* out) {
    return answer(ROOT_SIGNATURE, iid, is(iid, ROOT_SIGNATURE_IIDS) ? self : nullptr, out);
}
std::uint32_t __stdcall root_release(This self) { return release_boxed<RootSignature>(self); }

const void* const* root_functions() {

    static const Table<24> table = [] {
        auto f = d3d12_object(missing_table<ROOT_SIGNATURE, 24>());
        f[0] = fn(&root_query);
        f[2] = fn(&root_release);
        f[7] = fn(&parent_device);
        return f;
    }();
    return table.data();
}


const std::array<Iid, 4> SIGNATURE_IIDS = {
    guid(0xc36a797c, 0xec80, 0x4f0a, {0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1}),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};

struct Signature {
    Object object;
    std::uint32_t stride;
    std::uint32_t count;
    std::unique_ptr<std::array<std::uint32_t, 4>[]> arguments;
};

HRESULT __stdcall signature_query(This self, const Iid* iid, This* out) {
    return answer(COMMAND_SIGNATURE, iid, is(iid, SIGNATURE_IIDS) ? self : nullptr, out);
}
std::uint32_t __stdcall signature_release(This self) { return release_boxed<Signature>(self); }

const void* const* signature_functions() {
    static const Table<24> table = [] {
        auto f = d3d12_object(missing_table<COMMAND_SIGNATURE, 24>());
        f[0] = fn(&signature_query);
        f[2] = fn(&signature_release);
        f[7] = fn(&parent_device);
        return f;
    }();
    return table.data();
}


const std::array<Iid, 4> QUERY_HEAP_IIDS = {
    guid(0x0d9658ae, 0xed45, 0x469e, {0xa6, 0x1d, 0x97, 0x0e, 0xc5, 0x83, 0xca, 0xb4}),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};

struct QueryHeap {
    Object object;
    QueryHeapDesc desc;
};

HRESULT __stdcall query_heap_query(This self, const Iid* iid, This* out) {
    return answer(QUERY_HEAP, iid, is(iid, QUERY_HEAP_IIDS) ? self : nullptr, out);
}
std::uint32_t __stdcall query_heap_release(This self) { return release_boxed<QueryHeap>(self); }

const void* const* query_heap_functions() {
    static const Table<24> table = [] {
        auto f = d3d12_object(missing_table<QUERY_HEAP, 24>());
        f[0] = fn(&query_heap_query);
        f[2] = fn(&query_heap_release);
        f[7] = fn(&parent_device);
        return f;
    }();
    return table.data();
}

const bool MADE = (root_functions(), signature_functions(), query_heap_functions(), true);

}   

HRESULT __stdcall serialize_root(const RootDesc* desc, std::uint32_t version, void** blob, void** error) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    static const auto host = reinterpret_cast<Serialize>(host_export("D3D12SerializeRootSignature"));
    if (!host) {
        log::warn("victory_gpu::root", "no host d3d12.dll to serialize root signatures");
        return E_NOTIMPL_;
    }
    RootDesc d = *desc;
    d.flags &= ~CONSOLE_FLAGS;
    return host(&d, version, blob, error);
}

HRESULT __stdcall serialize_root_versioned(const VersionedRootDesc* desc, void** blob, void** error) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    static const auto host = reinterpret_cast<SerializeVersioned>(host_export("D3D12SerializeVersionedRootSignature"));
    if (!host) {
        log::warn("victory_gpu::root", "no host d3d12.dll to serialize root signatures");
        return E_NOTIMPL_;
    }
    VersionedRootDesc d = *desc;
    d.desc.flags &= ~CONSOLE_FLAGS;
    return host(&d, blob, error);
}

HRESULT __stdcall create_root_deserializer(const std::uint8_t* blob, std::size_t size, const Iid* iid, void** out) {
    if (!blob || !out) {
        return E_INVALIDARG_;
    }
    static const auto host = reinterpret_cast<Deserialize>(host_export("D3D12CreateRootSignatureDeserializer"));
    if (!host) {
        log::warn("victory_gpu::root", "no host d3d12.dll to deserialize root signatures");
        return E_NOTIMPL_;
    }
    return host(blob, size, iid, out);
}

HRESULT __stdcall create_root_signature(This, std::uint32_t, const std::uint8_t* blob, std::size_t size, const Iid* iid, This* out) {
    if (!blob || size == 0) {
        return E_INVALIDARG_;
    }

    auto* r = new RootSignature{Object{root_functions()}, std::unique_ptr<std::uint8_t[]>(new std::uint8_t[size]), size};
    std::memcpy(r->blob.get(), blob, size);
    return hand_out(ROOT_SIGNATURE, r, iid, ROOT_SIGNATURE_IIDS, out);
}

std::span<const std::uint8_t> root_blob(This root) {
    if (!root) {
        return {};
    }
    const auto& r = *reinterpret_cast<RootSignature*>(root);
    return {r.blob.get(), r.size};
}

bool signature_layout(This self, std::uint32_t& stride, std::uint32_t& count, const std::array<std::uint32_t, 4>*& arguments) {
    if (!self) {
        return false;
    }
    const auto& sig = *reinterpret_cast<const Signature*>(self);
    stride = sig.stride;
    count = sig.count;
    arguments = sig.arguments.get();
    return true;
}

HRESULT __stdcall create_command_signature(This, const CommandSignatureDesc* desc, This, const Iid* iid, This* out) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    if (!desc->arguments && desc->count > 0) {
        return E_INVALIDARG_;
    }
    auto* s = new Signature{Object{signature_functions()}, desc->stride, desc->count, nullptr};
    if (desc->count > 0) {

        s->arguments.reset(new std::array<std::uint32_t, 4>[desc->count]);
        std::memcpy(s->arguments.get(), desc->arguments, std::size_t{desc->count} * 16);
    }
    return hand_out(COMMAND_SIGNATURE, s, iid, SIGNATURE_IIDS, out);
}

bool command_layout(This signature, std::uint32_t& stride, std::span<const std::array<std::uint32_t, 4>>& arguments) {
    if (!signature) {
        return false;
    }
    const auto& s = *reinterpret_cast<Signature*>(signature);
    stride = s.stride;
    arguments = {s.arguments.get(), s.count};
    return true;
}

HRESULT __stdcall create_query_heap(This, const QueryHeapDesc* desc, const Iid* iid, This* out) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    auto* h = new QueryHeap{Object{query_heap_functions()}, *desc};
    return hand_out(QUERY_HEAP, h, iid, QUERY_HEAP_IIDS, out);
}

}   
