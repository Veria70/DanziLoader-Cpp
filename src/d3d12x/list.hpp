// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/list.rs (DanziLoader / victory-perf, see
// NOTICE.md): command allocators and lists. A title list is laid out as the
// title's inlined code reads it and records into a host list; copies of
// title memory, bottom-of-pipe writes and query results are done at
// submission; waits for memory values cut the recording into parts the
// queue runs in turn.
//
// This part (M4.7a): the list's life (creation, close, reset, the host lists
// taken at submission and kept for reuse), buffer and memory copies,
// barriers on buffers, bottom-of-pipe writes, query results, waits. Draws,
// dispatches, the draw state, clears, texture copies and resolves come with
// M4.7c-d; indirect execution with M4.8.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "host.hpp"
#include "object.hpp"

namespace danzi::d3d12x {


HRESULT __stdcall create_allocator(This, std::uint32_t kind, const Iid* iid, This* out);
HRESULT __stdcall create_list(This, std::uint32_t nodes, std::uint32_t kind, This allocator, This pipeline, const Iid* iid, This* out);
struct ListDesc {
    std::uint32_t kind, flags, nodes, barrier_batch;
};
HRESULT __stdcall create_list_x(This, const ListDesc* desc, This allocator, This pipeline, const Iid* iid, This* out);


void ensure_state(HostList& list, This resource, std::uint32_t state);


HostList* host_list(This list);


void tables_done(std::span<const This> lists);

bool cut_list(This list);


Taken detach(std::span<const This> lists);

struct Part {
    std::vector<ValueWait> waits;
    Taken taken;
};
std::vector<Part> detach_parts(std::span<const This> lists);


void recycle(std::vector<Detached> lists, std::span<const std::size_t> ran);


std::vector<ValueWait> unmet_waits(std::span<const This> lists);
std::vector<std::vector<ValueWait>> waits_by_list(std::span<const This> lists);


std::pair<std::uint64_t, std::uint64_t> take_counts();

std::uint64_t take_indirect();

}   
