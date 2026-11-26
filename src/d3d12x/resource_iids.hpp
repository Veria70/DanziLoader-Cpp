// SPDX-License-Identifier: GPL-3.0-or-later
// From crates/victory-gpu/src/resource.rs (DanziLoader / victory-perf, see
// NOTICE.md): the interfaces every pageable object answers to
// (`ID3D12Pageable`, `ID3D12DeviceChild`, `ID3D12Object`). The rest of
// resource.rs comes with milestone 4.4.
#pragma once

#include "object.hpp"

namespace danzi::d3d12x {

inline constexpr std::array<Iid, 3> PAGEABLE_IIDS = {
    guid(0x63ee58fb, 0x1268, 0x4835, {0x86, 0xda, 0xf0, 0x08, 0xce, 0x62, 0xf0, 0xd6}),
    guid(0x905db94b, 0xa00c, 0x4140, {0x9d, 0xf5, 0x2b, 0x64, 0xca, 0x9e, 0xa3, 0x57}),
    guid(0xc4fec28f, 0x7966, 0x4e95, {0x9f, 0x94, 0xf4, 0x31, 0xcb, 0x56, 0xc3, 0xb8}),
};

}   
