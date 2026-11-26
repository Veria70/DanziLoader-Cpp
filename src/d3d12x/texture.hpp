// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/texture.rs (DanziLoader / victory-perf, see
// NOTICE.md): texture contents the title writes itself (streamed pages,
// layout-library copies, movie frames, glyph atlases). Such a texture lives
// in title memory in its tiled layout; before a submission samples its host
// copy, the memory is detiled into a staging buffer when its content
// changed, and the staging buffer is copied into the host texture.
//
// "Changed": a page of the memory was written since the last detiling (the
// write watch), in write-watched memory; in view memory the guard, and a
// content hash taken by turns (`VICTORY_TEXTURE_HASH_EVERY`).
#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "host.hpp"
#include "object.hpp"

namespace danzi::d3d12x {

struct Staging;


void upload(HostList& list, This resource, ID3D12Resource* texture);
void upload_from(HostList& list, This resource, ID3D12Resource* texture, std::uint32_t first);


bool title_written(ID3D12Resource* texture);

void host_written(This resource);


void placed_in_host_written(std::uint64_t address, const std::uint32_t* desc);
bool host_wrote(std::uintptr_t resource);

void texture_forget(std::uintptr_t resource);


bool alias(HostList& list, This placed, This source);


void unstage(This resource);
void upload_again(This resource);
std::optional<std::pair<std::uint64_t, std::uint64_t>> staged_memory(This resource);
bool is_staged(This resource);


std::optional<std::pair<std::uint64_t, bool>> staged(This resource);

std::uint64_t fingerprint(std::uint64_t at, std::uint64_t len);


void textures_let_go_idle();

std::array<std::uint64_t, 2> staging_let_go();

std::uint64_t texture_hash_every();


std::array<std::uint64_t, 2> take_unlocked();


void refresh(Staging& staging);

std::pair<usize, usize> staging_memory(const Staging& staging);

}   
