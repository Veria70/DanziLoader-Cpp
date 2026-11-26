// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/fence.rs, queue.rs and frame.rs
// (DanziLoader / victory-perf, see NOTICE.md): fences, command queues (their
// line of what a queue holds behind a wait for a fence) and frame pacing.
//
// What waits for the host (milestone 4.7 and 4.10): running submitted lists
// (`execute`), values lists wait for in memory, submissions sent to the card
// and not waited for (`VICTORY_LATE_WAIT`, the relay), the submit thread
// (`VICTORY_ASYNC_SUBMIT`), and the present's copy to the window. Without
// them a queue's line holds waits for fences and signals, which is what
// orders the title's queues against each other.
#pragma once

#include <cstdint>

#include "object.hpp"

namespace danzi::d3d12x {


extern const std::array<Iid, 4> FENCE_IIDS;

HRESULT __stdcall create_fence(This device, std::uint64_t initial, std::uint32_t flags, const Iid* iid, This* out);

std::uint64_t __stdcall fence_completed(This fence);

void fence_set(This fence, std::uint64_t value);

HRESULT __stdcall fence_signal(This fence, std::uint64_t value);


struct QueueDesc {
    std::uint32_t kind;
    std::int32_t priority;
    std::uint32_t flags;
    std::uint32_t nodes;
};
struct QueueDescX {
    std::uint32_t kind;
    std::uint32_t flags;
    std::uint32_t engine_or_pipe;
    std::uint32_t queue;
};
HRESULT __stdcall create_queue(This device, const QueueDesc* desc, const Iid* iid, This* out);
HRESULT __stdcall create_queue_x(This device, const QueueDescX* desc, const Iid* iid, This* out);


void release_held(bool all);


bool waits();
bool split();
bool cuts();


struct Written {
    enum Kind : std::uint8_t { Value, GpuTimestamp, SystemTimestamp } kind = Value;
    std::uint64_t value = 0;

    void write(std::uint8_t* at, std::size_t bytes) const;
};


HRESULT __stdcall frame_set_interval(This, This output, std::uint32_t length, std::uint32_t period, std::uint32_t flags);
HRESULT __stdcall frame_schedule(This, std::uint32_t kind, std::uint32_t offset, const std::uint8_t* list, std::uint32_t flags);
HRESULT __stdcall frame_wait(This, std::uint32_t kind, std::uint32_t timeout_ms, const std::uint8_t* list, std::uint32_t flags, std::uint64_t* token);
HRESULT __stdcall frame_statistics(This, std::uint64_t token, std::uint32_t kinds, std::uint32_t* count, std::uint8_t* out);

std::uint64_t frame_current();


void sleep_ns(std::uint64_t ns);

}   
