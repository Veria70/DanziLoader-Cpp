// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/root.rs, signature.rs and query.rs
// (DanziLoader / victory-perf, see NOTICE.md): root signatures (serialized by
// the host's `d3d12.dll`, the console's own flags dropped first; the
// serialized form kept), command signatures (the record layout of indirect
// execution, kept for translating it) and query heaps (their description
// only: no query produces a result until lists run).
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "object.hpp"

namespace danzi::d3d12x {


struct RootDesc {
    std::uint32_t parameter_count;
    const void* parameters;
    std::uint32_t sampler_count;
    const void* samplers;
    std::uint32_t flags;
};

struct VersionedRootDesc {
    std::uint32_t version;
    RootDesc desc;
};
static_assert(sizeof(RootDesc) == 40 && sizeof(VersionedRootDesc) == 48);


HRESULT __stdcall serialize_root(const RootDesc* desc, std::uint32_t version, void** blob, void** error);
HRESULT __stdcall serialize_root_versioned(const VersionedRootDesc* desc, void** blob, void** error);


HRESULT __stdcall create_root_deserializer(const std::uint8_t* blob, std::size_t size, const Iid* iid, void** out);

HRESULT __stdcall create_root_signature(This, std::uint32_t nodes, const std::uint8_t* blob, std::size_t size, const Iid* iid, This* out);

std::span<const std::uint8_t> root_blob(This root);


struct CommandSignatureDesc {
    std::uint32_t stride;
    std::uint32_t count;

    const std::uint32_t (*arguments)[4];
    std::uint32_t nodes;
};
HRESULT __stdcall create_command_signature(This, const CommandSignatureDesc* desc, This root, const Iid* iid, This* out);


bool signature_layout(This signature, std::uint32_t& stride, std::uint32_t& count, const std::array<std::uint32_t, 4>*& arguments);

bool command_layout(This signature, std::uint32_t& stride, std::span<const std::array<std::uint32_t, 4>>& arguments);


struct QueryHeapDesc {
    std::uint32_t kind, count, nodes;
};
HRESULT __stdcall create_query_heap(This, const QueryHeapDesc* desc, const Iid* iid, This* out);

}   
