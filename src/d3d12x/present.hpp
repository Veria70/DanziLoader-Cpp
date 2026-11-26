// SPDX-License-Identifier: GPL-3.0-or-later
// Presentation (`host.rs` `present`, `presenter`, `title_window`, `window`;
// `over.rs`; DanziLoader / victory-perf, see NOTICE.md): a present's first
// plane copied into the back buffer of a swap chain on our window (the
// title's, if it made one), a second plane drawn over it blended by its
// alpha, the frame rate in the window's caption.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <optional>

namespace danzi::d3d12x {


bool present_texture(ID3D12Resource* texture, std::uint32_t state, std::uint64_t frame, std::optional<std::pair<ID3D12Resource*, std::uint32_t>> over);


void* present_window();

}   
