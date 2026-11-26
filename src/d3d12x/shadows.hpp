// SPDX-License-Identifier: GPL-3.0-or-later
// The host copies of title memory as draws and views use them
// (`shadow.rs`'s `find`, `find_any`, `Shadow::gpu`, `offset`, `left`,
// `refresh`, `gpu_writes`; DanziLoader / victory-perf, see NOTICE.md).
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>

#include "core/readback.hpp"
#include "core/shadow.hpp"
#include "gpu/lookup.hpp"
#include "host.hpp"

namespace danzi::d3d12x {


inline std::shared_ptr<shadow::Shadow> shadow_find(std::uint64_t address, std::uint64_t len) {
    Host* h = host();
    return h ? h->registry().find(address, len) : nullptr;
}


inline std::shared_ptr<shadow::Shadow> shadow_find_any(std::uint64_t address, std::uint64_t len) {
    Host* h = host();
    return h ? h->registry().find_any(address, len) : nullptr;
}


inline std::uint64_t shadow_offset(const shadow::Shadow& s, std::uint64_t address) {
    return address - s.start() + (s.placed() ? s.place_bias() : 0);
}


inline std::uint64_t shadow_gpu(const shadow::Shadow& s, std::uint64_t address) {
    return s.buffer()->GetGPUVirtualAddress() + shadow_offset(s, address);
}


inline std::uint64_t shadow_left(const shadow::Shadow& s, std::uint64_t address) {
    std::uint64_t end = s.start() + s.len();
    return end > address ? end - address : 0;
}


inline void shadow_refresh(HostList& list, const std::shared_ptr<shadow::Shadow>& s, std::uint64_t address, std::uint64_t len) {
    len = std::min(len, s->start() + s->len() - address);
    list.defer_refresh(s, static_cast<usize>(address), static_cast<usize>(len));
}


inline void shadow_gpu_writes(HostList& list, const std::shared_ptr<shadow::Shadow>& s, std::uint64_t address, std::uint64_t len) {
    if (s->placed()) {
        return;
    }
    len = std::min(len, shadow_left(*s, address));
    if (!readback::copy_back(address, len)) {
        return;
    }
    list.defer_gpu_write(s, static_cast<usize>(address), static_cast<usize>(len));
}

}   
