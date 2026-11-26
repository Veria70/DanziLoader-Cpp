// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/texture.rs (see texture.hpp).
#include "texture.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/env.hpp"
#include "core/log.hpp"
#include "core/memcopy.hpp"
#include "core/title_copy.hpp"
#include "core/watch.hpp"
#include "formats/texture.hpp"
#include "list.hpp"
#include "resource.hpp"
#include "sync.hpp"

namespace danzi::d3d12x {

namespace {
constexpr std::string_view TARGET = "victory_gpu::texture";
}   


struct StagingChunk {
    ComPtr<ID3D12Resource> buffer;
    std::uint8_t* data = nullptr;


    std::uint64_t next = 0, high = 0;
    std::uint32_t live = 0;
};


struct Staging {
    ComPtr<ID3D12Resource> buffer;

    std::shared_ptr<StagingChunk> chunk;
    std::uint8_t* data = nullptr;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
    formats::Layout layout;


    std::uint32_t mips = 0, first = 0;
    usize size = 0;
    std::uint64_t bytes = 0;
    const std::uint8_t* memory = nullptr;


    struct Last {
        std::optional<std::array<std::uint8_t, 32>> hash;
        std::uint64_t asked = 0;
        std::optional<std::uint64_t> partial;
        std::uint64_t unguarded = 0;
    };
    std::mutex last_lock;
    Last last;


    std::atomic<bool> dirty{true}, uploaded{false}, is_view{false};
    std::atomic<std::uint64_t> used{0};

    ~Staging();

    std::pair<std::uint32_t, std::uint32_t> part(usize i) const {
        std::uint32_t m = mips - first;
        return {first + static_cast<std::uint32_t>(i) % m, static_cast<std::uint32_t>(i) / m};
    }
    std::uint32_t subresource(usize i) const {
        auto [mip, slice] = part(i);
        return slice * mips + mip;
    }
    void detile(const std::uint8_t* tiled, usize len);
    usize refresh_timed();
    usize refresh_unlocked(usize size);
    void unseen_write();
};

namespace {

std::mutex STAGING_LOCK;
std::unordered_map<std::uintptr_t, std::shared_ptr<Staging>> STAGING;

std::atomic<std::uint64_t> STAGED[2]{}, LET_GO[2]{};


struct Through {
    ComPtr<ID3D12Resource> buffer;
    std::vector<std::pair<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, D3D12_PLACED_SUBRESOURCE_FOOTPRINT>> parts;
};
constexpr std::uint32_t ALIAS_PARTS = 54;
std::mutex THROUGH_LOCK;
std::unordered_map<std::uintptr_t, std::shared_ptr<Through>> THROUGH;


std::mutex HOST_WRITTEN_LOCK;
std::unordered_set<std::uintptr_t> HOST_WRITTEN;


std::atomic<std::uint64_t> UNLOCKED[2]{};

bool no_wait() {
    static const bool on = danzi::env::is(L"VICTORY_TEXTURE_NO_WAIT", L"1");
    return on;
}


bool through_copy() {
    static const bool on = danzi::env::is(L"VICTORY_TEXTURE_COPY", L"1");
    return on;
}


std::uint8_t* read_room(usize size) {
    thread_local std::vector<std::uint8_t> read;
    if (read.size() < size) {
        read.resize(size);
    }
    return read.data();
}

using Hash = std::array<std::uint8_t, 32>;


Hash content_hash(const std::uint8_t* bytes, usize len) {
    LARGE_INTEGER started, ended, rate;
    QueryPerformanceCounter(&started);
    Hash h = memcopy::lane_hash(bytes, len);
    QueryPerformanceCounter(&ended);
    QueryPerformanceFrequency(&rate);
    memcopy::add(memcopy::M::Hashes, 1);
    memcopy::add(memcopy::M::HashBytes, len);
    memcopy::add(memcopy::M::HashUs, static_cast<std::uint64_t>((ended.QuadPart - started.QuadPart) * 1'000'000 / rate.QuadPart));
    return h;
}

std::uint64_t hash_every() {
    static const std::uint64_t every = [] {
        auto v = danzi::env::get(L"VICTORY_TEXTURE_HASH_EVERY");
        if (!v) {
            return memcopy::hash_every_of(std::nullopt);
        }
        std::string narrow(v->begin(), v->end());
        return memcopy::hash_every_of(narrow);
    }();
    return every;
}


std::pair<std::uint32_t, usize> staged_part(const formats::Layout& layout, std::uint32_t mips, std::uint32_t first) {
    std::uint32_t f = first < mips && layout.bytes_from(first) < layout.size ? first : 0;
    return {f, static_cast<usize>(layout.bytes_from(f))};
}

constexpr std::uint64_t CHUNK_BYTES = 4u << 20, PIECE_MOST = 512u << 10, PIECE_ALIGN = 512;
std::mutex ARENA_LOCK;
std::shared_ptr<StagingChunk> ARENA_CURRENT;
std::vector<std::shared_ptr<StagingChunk>> ARENA_SPARE;

bool arena_on() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_STAGING_ARENA", L"0");
    return on;
}


std::optional<std::pair<std::shared_ptr<StagingChunk>, std::uint64_t>> arena_take(Host& h, std::uint64_t bytes) {
    if (bytes > PIECE_MOST || !arena_on()) {
        return std::nullopt;
    }
    std::uint64_t need = (bytes + PIECE_ALIGN - 1) / PIECE_ALIGN * PIECE_ALIGN;
    std::shared_ptr<StagingChunk> c;
    std::uint64_t at = 0, dirty_end = 0;
    {
        std::lock_guard lock(ARENA_LOCK);
        if (!ARENA_CURRENT || ARENA_CURRENT->next + need > CHUNK_BYTES) {
            if (ARENA_CURRENT && ARENA_CURRENT->live == 0) {
                ARENA_CURRENT->next = 0;   
            } else if (!ARENA_SPARE.empty()) {
                ARENA_CURRENT = std::move(ARENA_SPARE.back());
                ARENA_SPARE.pop_back();
            } else {
                ARENA_CURRENT = nullptr;
            }
        }
        if (ARENA_CURRENT) {
            c = ARENA_CURRENT;
            at = c->next;
            c->next += need;
            dirty_end = std::min(c->high, at + bytes);
            c->high = std::max(c->high, c->next);
            ++c->live;
        }
    }
    if (!c) {
        auto made = h.device().upload_buffer(CHUNK_BYTES);
        if (!made) {
            return std::nullopt;
        }
        c = std::make_shared<StagingChunk>();
        c->buffer = made->first;
        c->data = made->second;
        std::lock_guard lock(ARENA_LOCK);


        at = 0;
        c->next = need;
        c->high = need;
        c->live = 1;
        ARENA_CURRENT = c;
        return std::pair{std::move(c), at};
    }

    if (dirty_end > at) {
        std::memset(c->data + at, 0, static_cast<usize>(dirty_end - at));
    }
    return std::pair{std::move(c), at};
}

void arena_give(const std::shared_ptr<StagingChunk>& chunk) {
    std::lock_guard lock(ARENA_LOCK);
    if (--chunk->live != 0 || chunk == ARENA_CURRENT) {
        return;
    }
    chunk->next = 0;
    if (ARENA_SPARE.size() < 4) {
        ARENA_SPARE.push_back(chunk);
    }

}


std::shared_ptr<Staging> staging_of(This resource, ID3D12Resource* texture, std::uint32_t first) {
    std::lock_guard lock(STAGING_LOCK);
    auto key = reinterpret_cast<std::uintptr_t>(resource);
    if (auto it = STAGING.find(key); it != STAGING.end() && it->second->first <= first) {
        return it->second;
    }
    const std::uint8_t* memory;
    ResourceDesc desc;
    if (!texture_memory(resource, memory, desc)) {
        return nullptr;
    }
    formats::Texture t;
    if (!texture_layout(desc, t, true)) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true, std::memory_order_relaxed)) {
            log::warn(TARGET, "texture contents without a console layout not uploaded", {{"desc", desc_debug(desc)}});
        }
        return nullptr;
    }
    const formats::Layout& layout = t.planes[0].layout;
    if (layout.desc.swizzle.xor_mode()) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true, std::memory_order_relaxed)) {
            log::warn(TARGET, "texture contents in an XOR mode not uploaded", {{"desc", desc_debug(desc)}});
        }
        return nullptr;
    }
    Host* h = host();
    if (!h) {
        return nullptr;
    }
    D3D12_RESOURCE_DESC d = texture->GetDesc();
    auto [f, size] = staged_part(layout, d.MipLevels, first);
    UINT count = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? d.MipLevels : UINT{d.MipLevels} * d.DepthOrArraySize - f;
    auto s = std::make_shared<Staging>();
    s->footprints.resize(count);
    s->rows.resize(count);
    UINT64 total = 0;
    h->device().device()->GetCopyableFootprints(&d, f, count, 0, s->footprints.data(), s->rows.data(), nullptr, &total);
    if (auto piece = arena_take(*h, total)) {

        auto& [chunk, at] = *piece;
        h->device().device()->GetCopyableFootprints(&d, f, count, at, s->footprints.data(), s->rows.data(), nullptr, nullptr);
        s->buffer = chunk->buffer;
        s->data = chunk->data;
        s->chunk = std::move(chunk);
    } else {
        auto upload = h->device().upload_buffer(total);
        if (!upload) {
            return nullptr;
        }
        s->buffer = upload->first;
        s->data = upload->second;
    }
    s->layout = layout;
    s->mips = d.MipLevels;
    s->first = f;
    s->size = size;
    s->bytes = total;
    s->memory = memory;
    s->used.store(moment(), std::memory_order_relaxed);
    STAGED[0].fetch_add(1, std::memory_order_relaxed);
    STAGED[1].fetch_add(total, std::memory_order_relaxed);
    STAGING[key] = s;
    return s;
}


std::uint32_t staged_from(This resource) {
    std::lock_guard lock(STAGING_LOCK);
    auto it = STAGING.find(reinterpret_cast<std::uintptr_t>(resource));
    return it == STAGING.end() ? UINT32_MAX : it->second->first;
}

D3D12_TEXTURE_COPY_LOCATION subresource_location(ID3D12Resource* r, UINT index) {
    D3D12_TEXTURE_COPY_LOCATION l{};
    l.pResource = r;
    l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l.SubresourceIndex = index;
    return l;
}

D3D12_TEXTURE_COPY_LOCATION placed_location(ID3D12Resource* r, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f) {
    D3D12_TEXTURE_COPY_LOCATION l{};
    l.pResource = r;
    l.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    l.PlacedFootprint = f;
    return l;
}

}   

Staging::~Staging() {
    if (chunk) {
        arena_give(chunk);
    }
    STAGED[0].fetch_sub(1, std::memory_order_relaxed);
    STAGED[1].fetch_sub(bytes, std::memory_order_relaxed);
}

void Staging::detile(const std::uint8_t* tiled, usize len) {
    dirty.store(true, std::memory_order_relaxed);
    for (usize i = 0; i < footprints.size(); ++i) {
        auto [mip, slice] = part(i);
        const auto& f = footprints[i].Footprint;
        usize pitch = f.RowPitch;
        usize n = pitch * rows[i] * f.Depth;
        if (auto e = layout.detile(tiled, len, mip, slice, data + footprints[i].Offset, n, pitch, pitch * rows[i]); e != formats::Error::None) {
            static constexpr const char* NAMES[] = {"None", "Invalid", "Unsupported", "TooSmall"};
            log::warn(TARGET, "texture not detiled",
                      {{"e", NAMES[static_cast<std::uint32_t>(e)]}, {"mip", log::debug(mip)}, {"slice", log::debug(slice)}});
            return;
        }
    }
}


void Staging::unseen_write() {
    std::uint64_t frame = frame_current();
    last.unguarded = frame + (frame < last.unguarded ? 120 : 16);
}


usize Staging::refresh_unlocked(usize n) {
    std::uint8_t* read = read_room(n);
    if (!danzi::host::read_into(memory, read, n)) {
        UNLOCKED[1].fetch_add(1, std::memory_order_relaxed);
        last.asked = 0;
        return 0;
    }
    Hash h = content_hash(read, n);
    if (last.hash != h) {
        last.hash = h;
        detile(read, n);
    }
    UNLOCKED[0].fetch_add(1, std::memory_order_relaxed);
    return n;
}

usize Staging::refresh_timed() {
    usize start = reinterpret_cast<usize>(memory);
    std::lock_guard lock(last_lock);
    if (auto w = watch::writes(start, start + size, false)) {
        if (last.asked != 0 && std::all_of(w->written.begin(), w->written.end(), [&](const watch::PageWrite& p) { return p.second <= last.asked; })) {
            return 0;
        }
        last.asked = w->now;

        std::uint8_t* read = read_room(size);
        copy_mapped(read, memory, size, false);
        detile(read, size);
        return size;
    }


    static const bool guard = danzi::env::not_set_or_not(L"VICTORY_TEXTURE_GUARD", L"0");
    bool unseen = false;
    if (guard) {
        std::optional<watch::Writes> w;
        if (is_view.load(std::memory_order_relaxed)) {
            w = watch::writes_guarded(start, start + size);
        } else if (auto k = watch::writes_kind(start, start + size, true)) {
            if (k->is_view) {
                is_view.store(true, std::memory_order_relaxed);
            }
            w = watch::Writes{k->now, std::move(k->written)};
        }
        if (w) {
            std::uint64_t frame = frame_current();
            std::uint64_t every = hash_every();
            bool unwritten = last.asked != 0 && std::all_of(w->written.begin(), w->written.end(), [&](const watch::PageWrite& p) { return p.second <= last.asked; });
            last.asked = w->now;
            bool turn = memcopy::check_turn(frame, start, every);


            bool film = layout.desc.swizzle.mode == formats::Swizzle::LINEAR && layout.desc.mips == 1;
            if (unwritten && !turn && !film && frame >= last.unguarded) {
                if (every != 8 && memcopy::check_turn(frame, start, 8)) {
                    memcopy::add(memcopy::M::ChecksSkipped, 1);
                }
                return 0;
            }
            unseen = unwritten;
        }
    }


    std::optional<watch::Reading> mapped = no_wait() ? watch::title_memory_reading_now() : std::optional<watch::Reading>(watch::title_memory_reading());
    if (!mapped) {
        return refresh_unlocked(size);
    }
    if (!committed(memory, size)) {


        std::uint64_t frame = frame_current();
        if (last.partial && frame < *last.partial + 30) {
            return 0;
        }
        last.partial = frame;
        std::uint8_t* read = read_room(size);
        copy_mapped(read, memory, size, false);
        Hash h = content_hash(read, size);
        if (last.hash == h) {
            return size;
        }
        last.hash = h;
        if (std::all_of(read, read + size, [](std::uint8_t b) { return b == 0; })) {
            return size;
        }
        detile(read, size);
        return size;
    }
    const std::uint8_t* tiled = memory;
    if (through_copy()) {
        std::uint8_t* read = read_room(size);
        copy_mapped(read, memory, size, false);
        tiled = read;
    }
    Hash h = content_hash(tiled, size);
    if (last.hash == h) {
        return size;
    }
    if (unseen && last.hash) {
        unseen_write();
    }
    last.hash = h;
    detile(tiled, size);
    return size;
}

void refresh(Staging& staging) { staging.refresh_timed(); }

std::pair<usize, usize> staging_memory(const Staging& staging) { return {reinterpret_cast<usize>(staging.memory), staging.size}; }

bool title_written(ID3D12Resource* texture) { return (static_cast<std::uint32_t>(texture->GetDesc().Flags) & 0x7) == 0; }

namespace {


struct WrittenAt {
    std::uint64_t bytes;
    std::uint32_t width, height, layers_levels, format;
    std::uintptr_t resource;
};
std::mutex HOST_WRITTEN_AT_LOCK;
std::map<std::uint64_t, WrittenAt> HOST_WRITTEN_AT;

}   

void host_written(This resource) {
    bool fresh;
    {
        std::lock_guard lock(HOST_WRITTEN_LOCK);
        fresh = HOST_WRITTEN.insert(reinterpret_cast<std::uintptr_t>(resource)).second;
    }


    const std::uint8_t* at = nullptr;
    ResourceDesc desc;
    if (fresh && texture_memory(resource, at, desc)) {
        std::uint32_t words[14];
        std::memcpy(words, &desc, sizeof words);
        std::uint64_t bytes = placed_bytes(words).value_or(0);
        if (at && bytes > 0) {
            std::lock_guard lock(HOST_WRITTEN_AT_LOCK);
            HOST_WRITTEN_AT.insert_or_assign(reinterpret_cast<std::uint64_t>(at),
                                             WrittenAt{bytes, words[4], words[6], words[7], words[8], reinterpret_cast<std::uintptr_t>(resource)});
        }
    }
}

void placed_in_host_written(std::uint64_t address, const std::uint32_t* desc) {
    std::optional<std::pair<std::uint64_t, WrittenAt>> hit;
    {
        std::lock_guard lock(HOST_WRITTEN_AT_LOCK);
        auto it = HOST_WRITTEN_AT.upper_bound(address);
        if (it != HOST_WRITTEN_AT.begin()) {
            --it;
            if (address < it->first + it->second.bytes) {
                hit = *it;
            }
        }
    }
    if (!hit) {
        return;
    }
    static std::atomic<std::uint64_t> said{0};
    if (said.fetch_add(1, std::memory_order_relaxed) < 200) {
        const auto& [at, d] = *hit;
        log::warn(TARGET,
                  "HERO LEAD: a placed texture view lies in the memory of a texture the host copied into: it is filled from title memory, which the copy "
                  "never reached",
                  {{"view_address", log::hex(address)},
                   {"view_offset", log::debug(address - at)},
                   {"view_width", log::debug(desc[4])},
                   {"view_height", log::debug(desc[6])},
                   {"view_layers_levels", log::hex(desc[7])},
                   {"view_format", log::debug(desc[8])},
                   {"copied_address", log::hex(at)},
                   {"copied_bytes", log::debug(d.bytes)},
                   {"copied_width", log::debug(d.width)},
                   {"copied_height", log::debug(d.height)},
                   {"copied_layers_levels", log::hex(d.layers_levels)},
                   {"copied_format", log::debug(d.format)}});
    }
}

bool host_wrote(std::uintptr_t resource) {
    std::lock_guard lock(HOST_WRITTEN_LOCK);
    return HOST_WRITTEN.count(resource) != 0;
}

void texture_forget(std::uintptr_t resource) {
    {
        std::lock_guard lock(STAGING_LOCK);
        STAGING.erase(resource);
    }
    {
        std::lock_guard lock(HOST_WRITTEN_LOCK);
        HOST_WRITTEN.erase(resource);
    }
    {
        std::lock_guard lock(HOST_WRITTEN_AT_LOCK);
        std::erase_if(HOST_WRITTEN_AT, [&](const auto& kv) { return kv.second.resource == resource; });
    }
    std::lock_guard lock(THROUGH_LOCK);
    THROUGH.erase(resource);
}

void upload(HostList& list, This resource, ID3D12Resource* texture) { upload_from(list, resource, texture, 0); }

void upload_from(HostList& list, This resource, ID3D12Resource* texture, std::uint32_t first) {


    if (host_wrote(reinterpret_cast<std::uintptr_t>(resource)) || (!list.first_use(reinterpret_cast<usize>(resource)) && staged_from(resource) <= first)) {
        return;
    }
    auto staging = staging_of(resource, texture, first);
    if (!staging) {
        return;
    }
    staging->used.store(moment(), std::memory_order_relaxed);


    if (staging->uploaded.load(std::memory_order_relaxed) && !staging->dirty.load(std::memory_order_relaxed)) {
        list.defer_texture(staging);
        return;
    }
    constexpr std::uint32_t COPY_DEST = 0x400;
    ensure_state(list, resource, COPY_DEST);
    for (usize i = 0; i < staging->footprints.size(); ++i) {
        auto to = subresource_location(texture, staging->subresource(i));
        auto from = placed_location(staging->buffer.Get(), staging->footprints[i]);
        list.list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }


    list.defer_host([staging] {
        staging->uploaded.store(true, std::memory_order_relaxed);
        staging->dirty.store(false, std::memory_order_relaxed);
    });

    list.defer_texture(staging);
}

bool alias(HostList& list, This placed, This source) {
    constexpr std::uint32_t TARGET_OR_UNORDERED = 0x4 | 0x8, COPY_DEST = 0x400, COPY_SOURCE = 0x800;
    ID3D12Resource* to = host_resource(placed);
    ID3D12Resource* from = host_resource(source);
    Host* h = host();
    if (!to || !from || !h) {
        return false;
    }
    std::shared_ptr<Through> through;
    {
        std::lock_guard lock(THROUGH_LOCK);
        auto key = reinterpret_cast<std::uintptr_t>(placed);
        if (auto it = THROUGH.find(key); it != THROUGH.end()) {
            through = it->second;
        } else {
            D3D12_RESOURCE_DESC of_from = from->GetDesc(), of_to = to->GetDesc();
            auto count = [](const D3D12_RESOURCE_DESC& d) {
                UINT layers = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize;
                return UINT{d.MipLevels} * layers;
            };
            UINT parts = count(of_from);
            if (parts == 0 || parts > ALIAS_PARTS || parts != count(of_to)) {
                return false;
            }
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> a(parts), b(parts);
            UINT64 bytes[2] = {};
            h->device().device()->GetCopyableFootprints(&of_from, 0, parts, 0, a.data(), nullptr, nullptr, &bytes[0]);
            h->device().device()->GetCopyableFootprints(&of_to, 0, parts, 0, b.data(), nullptr, nullptr, &bytes[1]);
            bool alike = true;
            for (UINT i = 0; i < parts && alike; ++i) {
                const auto &fa = a[i].Footprint, &fb = b[i].Footprint;
                alike = a[i].Offset == b[i].Offset && fa.Width == fb.Width && fa.Height == fb.Height && fa.Depth == fb.Depth && fa.RowPitch == fb.RowPitch;
            }
            if (bytes[0] == 0 || bytes[0] != bytes[1] || !alike) {
                return false;
            }
            auto buffer = h->device().uav_buffer(bytes[0]);
            if (!buffer) {
                return false;
            }
            log::info(TARGET,
                      "a view placed in the memory of a texture the host writes, in another format: read from that texture by a copy on the host",
                      {{"view", log::hex(reinterpret_cast<std::uintptr_t>(placed))},
                       {"texture", log::hex(reinterpret_cast<std::uintptr_t>(source))},
                       {"width", log::debug(static_cast<std::uint64_t>(of_from.Width))},
                       {"height", log::debug(static_cast<std::uint32_t>(of_from.Height))},
                       {"parts", log::debug(static_cast<std::uint32_t>(parts))},
                       {"written_as", log::debug(static_cast<int>(of_from.Format))},
                       {"read_as", log::debug(static_cast<int>(of_to.Format))}});
            through = std::make_shared<Through>();
            through->buffer = std::move(buffer);
            for (UINT i = 0; i < parts; ++i) {
                through->parts.emplace_back(a[i], b[i]);
            }
            THROUGH.emplace(key, through);
        }
    }
    bool first = list.first_use(reinterpret_cast<usize>(placed));


    std::optional<std::uint32_t> before;
    if (auto t = list.transition(source, COPY_SOURCE)) {
        std::uint32_t state = legal_state(t->first, COPY_SOURCE);
        auto barrier = transition_barrier(t->first, t->second, state);
        list.list()->ResourceBarrier(1, &barrier);
        before = t->second;
    }
    if (!first && !before) {
        return true;
    }
    ensure_state(list, placed, COPY_DEST);
    auto* buffer = through->buffer.Get();
    auto b1 = transition_barrier(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list.list()->ResourceBarrier(1, &b1);
    for (usize i = 0; i < through->parts.size(); ++i) {
        auto dst = placed_location(buffer, through->parts[i].first);
        auto src = subresource_location(from, static_cast<UINT>(i));
        list.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    auto b2 = transition_barrier(buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list.list()->ResourceBarrier(1, &b2);
    for (usize i = 0; i < through->parts.size(); ++i) {
        auto dst = subresource_location(to, static_cast<UINT>(i));
        auto src = placed_location(buffer, through->parts[i].second);
        list.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    auto b3 = transition_barrier(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    list.list()->ResourceBarrier(1, &b3);
    list.keep(ComPtr<IUnknown>(buffer));

    if (before && (*before & TARGET_OR_UNORDERED) != 0) {
        ensure_state(list, source, *before);
    }
    return true;
}

void unstage(This resource) {
    std::lock_guard lock(STAGING_LOCK);
    STAGING.erase(reinterpret_cast<std::uintptr_t>(resource));
}

void upload_again(This resource) {
    std::shared_ptr<Staging> s;
    {
        std::lock_guard lock(STAGING_LOCK);
        auto it = STAGING.find(reinterpret_cast<std::uintptr_t>(resource));
        if (it == STAGING.end()) {
            return;
        }
        s = it->second;
    }
    s->uploaded.store(false, std::memory_order_relaxed);
    std::lock_guard lock(s->last_lock);
    s->last.partial.reset();
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> staged_memory(This resource) {
    std::lock_guard lock(STAGING_LOCK);
    auto it = STAGING.find(reinterpret_cast<std::uintptr_t>(resource));
    if (it == STAGING.end()) {
        return std::nullopt;
    }
    return std::pair{reinterpret_cast<std::uint64_t>(it->second->memory), static_cast<std::uint64_t>(it->second->size)};
}

std::optional<std::pair<std::uint64_t, bool>> staged(This resource) {
    std::shared_ptr<Staging> s;
    {
        std::lock_guard lock(STAGING_LOCK);
        auto it = STAGING.find(reinterpret_cast<std::uintptr_t>(resource));
        if (it == STAGING.end()) {
            return std::nullopt;
        }
        s = it->second;
    }
    return std::pair{static_cast<std::uint64_t>(s->size), committed(s->memory, s->size)};
}

bool is_staged(This resource) {
    std::lock_guard lock(STAGING_LOCK);
    return STAGING.count(reinterpret_cast<std::uintptr_t>(resource)) != 0;
}

std::uint64_t fingerprint(std::uint64_t at, std::uint64_t len) {


    constexpr usize PIECE = 4 << 10, PIECES = 12;
    thread_local std::vector<std::uint8_t> read(PIECES * PIECE);
    usize n = 0;
    auto take = [&](usize from, usize bytes) {
        std::memset(read.data() + n, 0, bytes);
        copy_mapped(read.data() + n, reinterpret_cast<const std::uint8_t*>(at + from), bytes, false);
        n += bytes;
    };
    auto l = static_cast<usize>(len);
    if (l <= PIECES * PIECE) {
        take(0, l);
    } else {
        for (usize k = 0; k < PIECES - 1; ++k) {
            take(((l - PIECE) / (PIECES - 1) * k) & ~usize{0xfff}, PIECE);
        }
        take(l - PIECE, PIECE);
    }
    Hash h = content_hash(read.data(), n);
    std::uint64_t first;
    std::memcpy(&first, h.data(), 8);
    return first | 1;
}

void textures_let_go_idle() {
    constexpr std::uint64_t IDLE_FRAMES = 4;
    std::vector<std::shared_ptr<void>> gone;
    std::uint64_t went = 0, bytes = 0;
    {
        std::lock_guard lock(STAGING_LOCK);
        std::uint64_t idle_ms = ram_idle_ms();
        std::erase_if(STAGING, [&](auto& kv) {
            auto& s = kv.second;
            bool keep = s.use_count() > 1 || !long_ago(s->used.load(std::memory_order_relaxed), idle_ms, IDLE_FRAMES);
            if (!keep) {
                LET_GO[0].fetch_add(1, std::memory_order_relaxed);
                LET_GO[1].fetch_add(s->bytes, std::memory_order_relaxed);
                went += 1;
                bytes += s->bytes;
                gone.push_back(std::move(s));
            }
            return !keep;
        });
    }
    if (went > 0) {
        trail([&] {
            return "sweep: " + std::to_string(went) + " staging buffers of textures let go (" + std::to_string(bytes >> 10) + " KiB), held by no list";
        });
    }

    dispose(std::move(gone));
}

std::array<std::uint64_t, 2> staging_let_go() { return {LET_GO[0].load(), LET_GO[1].load()}; }

std::uint64_t texture_hash_every() { return hash_every(); }

std::array<std::uint64_t, 2> take_unlocked() {
    return {UNLOCKED[0].exchange(0, std::memory_order_relaxed), UNLOCKED[1].exchange(0, std::memory_order_relaxed)};
}

}   
