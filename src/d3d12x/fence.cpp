// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/fence.rs (see sync.hpp): a 64-bit value and
// the events waiting for it.
#include <windows.h>

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/win.hpp"
#include "resource_iids.hpp"
#include "host.hpp"
#include "sync.hpp"

namespace danzi::d3d12x {

const std::array<Iid, 4> FENCE_IIDS = {
    guid(0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};

namespace {

struct Fence {
    Object object;

    std::atomic<std::uint64_t> value;
    std::uint32_t flags;


    std::atomic<std::uint32_t> waiting{0};
    danzi::FastLock lock;
    std::vector<std::pair<std::uint64_t, HANDLE>> waiters;
};
static_assert(offsetof(Fence, value) == 0x10);

Fence& fence(This self) { return *reinterpret_cast<Fence*>(self); }

HRESULT __stdcall query(This self, const Iid* iid, This* out) { return answer(FENCE, iid, is(iid, FENCE_IIDS) ? self : nullptr, out); }

std::uint32_t __stdcall release(This self) { return release_boxed<Fence>(self); }

HRESULT __stdcall set_event_on_completion(This self, std::uint64_t value, HANDLE event) {
    Fence& f = fence(self);
    if (f.value.load(std::memory_order_acquire) >= value) {
        if (event) {
            SetEvent(event);
        }
        return S_OK_;
    }
    if (!event) {


        if (late()) {
            settle_for_fence();
            if (f.value.load(std::memory_order_acquire) >= value) {
                return S_OK_;
            }
            log::warn("victory_gpu::fence", "blocking fence wait not implemented", {{"value", log::debug(value)}});
            return E_NOTIMPL_;
        }


        log::warn("victory_gpu::fence", "blocking fence wait not implemented", {{"value", log::debug(value)}});
        return E_NOTIMPL_;
    }
    std::lock_guard lock(f.lock);
    f.waiting.fetch_add(1, std::memory_order_seq_cst);
    if (f.value.load(std::memory_order_seq_cst) >= value) {
        f.waiting.fetch_sub(1, std::memory_order_relaxed);
        SetEvent(event);
        return S_OK_;
    }
    f.waiters.emplace_back(value, event);
    return S_OK_;
}

std::uint32_t __stdcall creation_flags(This self) { return fence(self).flags; }


std::uint64_t __stdcall gpu_address(This self) { return reinterpret_cast<std::uint64_t>(&fence(self).value); }

const void* const* functions() {


    static const Table<24> table = [] {
        auto f = d3d12_object(missing_table<FENCE, 24>());
        f[0] = fn(&query);
        f[2] = fn(&release);
        f[7] = fn(&parent_device);
        f[8] = fn(&fence_completed);
        f[9] = fn(&set_event_on_completion);
        f[10] = fn(&fence_signal);
        f[11] = fn(&creation_flags);
        f[12] = fn(&gpu_address);
        return f;
    }();
    return table.data();
}
const bool MADE = (functions(), true);

}   

HRESULT __stdcall create_fence(This, std::uint64_t initial, std::uint32_t flags, const Iid* iid, This* out) {
    auto* f = new Fence{Object{functions()}, initial, flags};
    return hand_out(FENCE, f, iid, FENCE_IIDS, out);
}

std::uint64_t __stdcall fence_completed(This self) { return fence(self).value.load(std::memory_order_acquire); }

void fence_set(This self, std::uint64_t value) {
    Fence& f = fence(self);
    f.value.store(value, std::memory_order_seq_cst);
    if (f.waiting.load(std::memory_order_seq_cst) == 0) {
        return;
    }
    std::lock_guard lock(f.lock);
    std::erase_if(f.waiters, [&](const std::pair<std::uint64_t, HANDLE>& w) {
        if (w.first <= value) {
            SetEvent(w.second);
            f.waiting.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    });
}

HRESULT __stdcall fence_signal(This self, std::uint64_t value) {
    fence_set(self, value);

    release_held(false);
    return S_OK_;
}

}   
