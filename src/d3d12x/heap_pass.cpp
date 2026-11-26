// SPDX-License-Identifier: GPL-3.0-or-later
// Shaders that pick resources from the view heap by an index they compute
// (SM 6.6 dynamic resources): the passes that make every buffer view of the
// heap ready and upload the textures the title fills (`draw.rs`
// `heap_buffers`, `heap_textures`, `early_textures`; DanziLoader /
// victory-perf, see NOTICE.md).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/env.hpp"
#include "core/log.hpp"
#include "gpu/device.hpp"
#include "descriptor.hpp"
#include "draw.hpp"
#include "host.hpp"
#include "host_heap.hpp"
#include "pipeline.hpp"
#include "resource.hpp"
#include "shadows.hpp"
#include "sync.hpp"
#include "texture.hpp"

namespace danzi::d3d12x {

namespace {

constexpr std::uint8_t SRV = 0;

constexpr std::string_view TARGET = "victory_gpu::draw";


std::optional<std::string> device_removed() {
    Host* h = host();
    if (!h) {
        return std::nullopt;
    }
    HRESULT reason = h->device().device()->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) {
        return std::nullopt;
    }
    return log::hresult(reason);
}

std::uint64_t env_u64(const wchar_t* name, std::uint64_t otherwise) {
    auto v = danzi::env::get(name);
    if (!v || v->empty()) {
        return otherwise;
    }
    wchar_t* end = nullptr;
    std::uint64_t n = std::wcstoull(v->c_str(), &end, 10);
    return end && *end == 0 ? n : otherwise;
}


std::pair<std::uint64_t, std::uint64_t> early_budgets() {
    static const std::pair<std::uint64_t, std::uint64_t> ms{env_u64(L"VICTORY_EARLY_TEXTURES_MS", 12),
                                                            env_u64(L"VICTORY_BAKE_TEXTURES_MS", 1500)};
    return ms;
}


constexpr const char* BAKE_PIXEL_SHADERS[4] = {"c0a7fe77b1862e10", "0d1575a396dfe513", "b7cfd8dda91ebb6b", "280967ad158cee0b"};

}   

bool is_bake_pipeline(const HostPipeline& p) {
    for (const Stage& s : p.stages) {
        for (const char* h : BAKE_PIXEL_SHADERS) {
            if (s.shader->hash == h) {
                return true;
            }
        }
    }
    return false;
}

namespace {


bool catching_up(std::uint64_t more_ms) {
    static std::atomic<std::uint64_t> SPENT{0};
    static const std::uint64_t TOTAL = env_u64(L"VICTORY_TEXTURE_GATE_S", 60) * 1000;
    return SPENT.fetch_add(more_ms, std::memory_order_relaxed) + more_ms < TOTAL;
}

bool reupload_when_full() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_REUPLOAD_WHEN_FULL", L"0");
    return on;
}

bool heap_recheck() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_HEAP_RECHECK", L"0");
    return on;
}


std::atomic<std::uint64_t> BUFFER_PASSES{0}, BUFFER_MICROS{0}, TEXTURES{0}, TEXTURE_BYTES{0}, TEXTURES_WAITING{0}, TEXTURES_FULL{0},
    TEXTURES_AWAY{0}, TEXTURES_TABLED{0}, REUPLOADED_FULL{0};
std::atomic<std::uint64_t> EARLY_PASSES[2]{};
std::atomic<std::uint64_t> TEXTURES_QUEUE[5]{};
std::atomic<std::uint64_t> TEXTURES_RECHECKED[3]{};
std::atomic<std::uint64_t> TEXTURES_PARTLY[3]{};

std::uint64_t micros_since(std::chrono::steady_clock::time_point t) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count());
}


struct HeapBufferCache {
    struct Entry {
        std::uint64_t key;
        std::shared_ptr<shadow::Shadow> shadow;
        std::uint64_t from, to;
    };
    struct Span {
        std::shared_ptr<shadow::Shadow> shadow;
        std::uint64_t from, to;
    };
    std::uint64_t passes = 0;
    std::unordered_map<std::uint32_t, Entry> buckets[64];
    std::vector<Span> spans[64];

    void rebuild(std::size_t b) {
        std::unordered_map<const shadow::Shadow*, std::size_t> at;
        std::vector<Span>& out = spans[b];
        out.clear();
        for (const auto& [index, e] : buckets[b]) {
            auto [it, fresh] = at.try_emplace(e.shadow.get(), out.size());
            if (fresh) {
                out.push_back({e.shadow, e.from, e.to});
            } else {
                Span& s = out[it->second];
                s.from = std::min(s.from, e.from);
                s.to = std::max(s.to, e.to);
            }
        }
    }
};

std::mutex CACHES_LOCK;
std::unordered_map<const HostHeap*, std::unique_ptr<HeapBufferCache>> CACHES;

}   

std::array<std::uint64_t, 6> heap_pass_counts() {
    return {BUFFER_PASSES.load(), TEXTURES.load(), TEXTURE_BYTES.load(), EARLY_PASSES[0].load(), EARLY_PASSES[1].load(), TEXTURES_TABLED.load()};
}

std::array<std::uint64_t, 9> heap_texture_queue() {
    std::array<std::uint64_t, 9> out{};
    for (int i = 0; i < 5; ++i) {
        out[i] = TEXTURES_QUEUE[i].load(std::memory_order_relaxed);
    }
    for (int i = 0; i < 3; ++i) {
        out[5 + i] = TEXTURES_PARTLY[i].load(std::memory_order_relaxed);
    }
    out[8] = TEXTURES_TABLED.load(std::memory_order_relaxed);
    return out;
}

DescHeap* ListState::view_heap() const {
    for (This h : heaps_) {
        if (h && !heap_samplers(heap_of_object(h))) {
            return &heap_of_object(h);
        }
    }
    return nullptr;
}

std::optional<std::uint64_t> ListState::heap_buffers(HostList& list) {
    DescHeap* heap = view_heap();
    if (!heap) {
        return std::nullopt;
    }
    HostHeap* host = heap_host(*heap);
    if (!host) {
        return std::nullopt;
    }
    auto gpu = host->strides_gpu();
    if (!gpu) {
        return std::nullopt;
    }

    constexpr usize ONCE = SIZE_MAX - 0x5e;
    if (!list.first_use(ONCE)) {
        return gpu;
    }

    if (ID3D12Resource* b = host->strides_buffer()) {
        list.keep(ComPtr<IUnknown>(b));
    }


    static std::atomic<std::uint64_t> FRAME{UINT64_MAX};
    std::uint64_t frame = frame_current();
    if (FRAME.load(std::memory_order_relaxed) == frame || FRAME.exchange(frame, std::memory_order_relaxed) == frame) {
        return gpu;
    }
    auto started = std::chrono::steady_clock::now();
    std::unique_lock kept(CACHES_LOCK);
    auto& slot = CACHES[host];
    if (!slot) {
        slot = std::make_unique<HeapBufferCache>();
    }
    HeapBufferCache& cache = *slot;
    std::vector<std::uint32_t> changed = host->buffer_changes();
    std::uint64_t dirty = 0;


    std::vector<std::pair<std::uint32_t, bool>> slots;
    if (cache.passes == 0) {
        for (std::size_t b = 0; b < 64; ++b) {
            cache.buckets[b].clear();
            cache.spans[b].clear();
        }
        dirty = UINT64_MAX;
        for (std::uint32_t i : host->buffer_slots()) {
            slots.emplace_back(i, true);
        }
    } else {
        std::size_t bucket = cache.passes & 63;
        slots.reserve(changed.size() + 2048);
        std::unordered_set<std::uint32_t> seen;
        seen.reserve(changed.size() * 2 + 16);
        for (std::uint32_t i : changed) {
            if (seen.insert(i).second) {
                slots.emplace_back(i, true);
            }
        }
        std::vector<std::uint32_t> rolling = host->buffer_slots_bucket(static_cast<std::uint32_t>(bucket));
        std::unordered_set<std::uint32_t> live(rolling.begin(), rolling.end());
        auto& map = cache.buckets[bucket];
        std::size_t before = map.size();
        std::erase_if(map, [&](const auto& kv) { return !live.contains(kv.first); });
        if (map.size() != before) {
            dirty |= std::uint64_t{1} << bucket;
        }
        for (std::uint32_t i : rolling) {
            if (seen.insert(i).second) {
                slots.emplace_back(i, false);
            }
        }
    }
    ++cache.passes;


    static std::atomic<bool> GONE{false};
    bool alive = !GONE.load(std::memory_order_relaxed) && !device_removed();
    for (std::size_t n = 0; n < slots.size(); ++n) {
        auto [index, force] = slots[n];
        std::size_t b = index & 63;
        auto& map = cache.buckets[b];
        auto handle = heap_handle(*heap, index);
        if (!handle) {
            if (map.erase(index)) {
                dirty |= std::uint64_t{1} << b;
            }
            continue;
        }
        std::uint64_t cached_key = 0;
        if (!force) {
            if (auto it = map.find(index); it != map.end() && !it->second.shadow->placed()) {
                cached_key = it->second.key;
            }
        }
        HostHeap::BufferMirror m = host->mirror_buffer(index, *handle, cached_key);
        if (m.kind == HostHeap::BufferMirror::Unchanged) {
            continue;
        }
        if (m.kind == HostHeap::BufferMirror::Empty || !m.view) {
            if (map.erase(index)) {
                dirty |= std::uint64_t{1} << b;
            }
            continue;
        }

        if (alive && n % 64 == 63) {
            if (auto why = device_removed()) {
                alive = false;
                GONE.store(true, std::memory_order_relaxed);
                std::string last = "[";
                for (std::size_t k = n - 63; k <= n; ++k) {
                    std::uint32_t i = slots[k].first;
                    auto h = heap_handle(*heap, i);
                    auto v = h ? view(*h) : std::nullopt;
                    if (v) {
                        std::string text = std::to_string(i) + ": " + view_debug(*v);
                        last += "\n    " + log::debug(text.substr(0, 200)) + ",";
                    }
                }
                last += last.size() > 1 ? "\n]" : "]";
                log::error(TARGET, "the host device went while buffer views of the heap were made; one of: " + last,
                           {{"why", log::debug(*why)}, {"slot", log::debug(index)}});
                log_removal_context();
            }
        }


        auto copy = buffer_copy(*m.view);
        if (!copy) {
            if (map.erase(index)) {
                dirty |= std::uint64_t{1} << b;
            }
            continue;
        }
        map[index] = {m.key, std::move(copy->shadow), copy->address, copy->address + copy->len};
        dirty |= std::uint64_t{1} << b;
    }
    for (std::uint64_t mask = dirty; mask != 0; mask &= mask - 1) {
        cache.rebuild(static_cast<std::size_t>(_tzcnt_u64(mask)));
    }


    std::unordered_map<const shadow::Shadow*, HeapBufferCache::Span> spans;
    for (const auto& list_spans : cache.spans) {
        for (const auto& s : list_spans) {
            auto [it, fresh] = spans.try_emplace(s.shadow.get(), s);
            if (!fresh) {
                it->second.from = std::min(it->second.from, s.from);
                it->second.to = std::max(it->second.to, s.to);
            }
        }
    }
    std::uint64_t views = 0;
    for (const auto& bucket : cache.buckets) {
        views += bucket.size();
    }
    kept.unlock();


    static std::mutex WHOLE_LOCK;
    static std::unordered_map<std::uint64_t, std::uint64_t> WHOLE;
    static const std::uint64_t EVERY = env_u64(L"VICTORY_UNWATCHED_FRAMES", 1);
    {
        std::lock_guard whole(WHOLE_LOCK);
        for (auto& [_, s] : spans) {
            if (s.shadow->unwatched()) {
                auto [it, fresh] = WHOLE.try_emplace(s.shadow->id(), frame);
                if (!fresh) {
                    if (frame < it->second + EVERY) {
                        continue;
                    }
                    it->second = frame;
                }
            }
            shadow_refresh(list, s.shadow, s.from, s.to - s.from);
        }
    }
    BUFFER_PASSES.fetch_add(1, std::memory_order_relaxed);
    BUFFER_MICROS.fetch_add(micros_since(started), std::memory_order_relaxed);
    heap_textures(list, *heap, *host, std::nullopt);

    static std::atomic<std::uint64_t> LOGGED{0};
    std::uint64_t seen = LOGGED.load(std::memory_order_relaxed);
    if (views > seen + seen / 2) {
        LOGGED.store(views, std::memory_order_relaxed);
        std::uint64_t bytes = 0;
        for (const auto& [_, s] : spans) {
            bytes += s.to - s.from;
        }
        log::info(TARGET, "buffer views of the heap made ready for shaders that pick them by index",
                  {{"slots", log::debug(static_cast<std::uint64_t>(slots.size()))},
                   {"views", log::debug(views)},
                   {"copies", log::debug(static_cast<std::uint64_t>(spans.size()))},
                   {"mb", log::debug(bytes >> 20)},
                   {"ms", log::debug(micros_since(started) / 1000)}});
    }
    return gpu;
}

void ListState::early_textures(HostList& list, const HostPipeline& p, bool compute) {
    auto [early_ms, bake_ms] = early_budgets();
    if (compute || (early_ms == 0 && bake_ms == 0)) {
        return;
    }
    bool bake = bake_ms > 0 && p.bake;
    static std::atomic<std::uint64_t> FRAME{UINT64_MAX};

    static std::atomic<std::uint64_t> BAKED_MS{0};
    std::uint64_t frame = frame_current();
    bool first = FRAME.load(std::memory_order_relaxed) != frame && FRAME.exchange(frame, std::memory_order_relaxed) != frame;
    if (first) {
        BAKED_MS.store(0, std::memory_order_relaxed);
    }
    if (!bake && !(first && early_ms > 0)) {
        return;   
    }
    std::uint64_t baked = BAKED_MS.load(std::memory_order_relaxed);
    std::uint64_t left = bake_ms > baked ? bake_ms - baked : 0;
    std::pair<std::uint64_t, std::uint64_t> budget;
    if (bake && left > 0) {

        budget = {left, std::uint64_t{2} << 30};
    } else if (first && early_ms > 0) {
        budget = {early_ms, std::uint64_t{64} << 20};
    } else {
        return;
    }
    DescHeap* heap = view_heap();
    if (!heap) {
        return;
    }
    HostHeap* host = heap_host(*heap);
    if (!host) {
        return;
    }
    bake = bake && left > 0;
    std::uint64_t n = EARLY_PASSES[bake ? 1 : 0].fetch_add(1, std::memory_order_relaxed) + 1;
    if (bake && std::has_single_bit(n)) {
        log::info(TARGET, "a draw of the title's decoration bake: textures picked by index are uploaded first (VICTORY_BAKE_TEXTURES_MS=0: not)",
                  {{"frame", log::debug(frame)},
                   {"bakes", log::debug(n)},
                   {"queued_at_the_last_pass", log::debug(TEXTURES_QUEUE[0].load(std::memory_order_relaxed))},
                   {"due_at_the_last_pass", log::debug(TEXTURES_QUEUE[1].load(std::memory_order_relaxed))},
                   {"uploaded_so_far", log::debug(TEXTURES.load(std::memory_order_relaxed))}});
    }
    auto began = std::chrono::steady_clock::now();
    heap_textures(list, *heap, *host, budget);
    if (bake) {
        BAKED_MS.fetch_add(micros_since(began) / 1000, std::memory_order_relaxed);
    }
}

void ListState::heap_textures(HostList& list, DescHeap& heap, HostHeap& host, std::optional<std::pair<std::uint64_t, std::uint64_t>> pass_budget) {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_HEAP_TEXTURES", L"0");
    if (!on) {
        return;
    }

    static const std::uint64_t PER_FRAME = env_u64(L"VICTORY_TEXTURE_STAGE_MB", 192) << 20;
    static std::atomic<std::uint64_t> FRAME{UINT64_MAX};
    static std::atomic<std::uint64_t> SPENT{0};
    std::uint64_t frame = frame_current();
    if (FRAME.exchange(frame, std::memory_order_relaxed) != frame) {
        SPENT.store(0, std::memory_order_relaxed);
    }


    bool has_room = room();
    if (!has_room) {
        TEXTURES_FULL.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<bool> said{false};
        if (!said.load(std::memory_order_relaxed) && !said.exchange(true, std::memory_order_relaxed)) {
            log::warn(TARGET, "textures picked by index: the card has no room for more; they wait");
        }
    }
    int refused = 0;


    host.texture_slots_remapped(frame);
    host.texture_sweep(heap, frame);
    if (heap_recheck()) {
        auto [same, changed, waiting] = host.texture_slots_rechecked(frame);
        TEXTURES_RECHECKED[0].fetch_add(same, std::memory_order_relaxed);
        TEXTURES_RECHECKED[1].fetch_add(changed, std::memory_order_relaxed);
        TEXTURES_RECHECKED[2].store(waiting, std::memory_order_relaxed);
    }
    auto [due, census] = host.texture_slots_due(frame, 4096);
    TEXTURES_WAITING.store(census[0], std::memory_order_relaxed);
    for (int i = 0; i < 5; ++i) {
        TEXTURES_QUEUE[i].store(census[i], std::memory_order_relaxed);
    }


    static std::atomic<std::uint64_t> PASSED{0};
    std::uint64_t now = now_ms();
    std::uint64_t last = PASSED.exchange(now, std::memory_order_relaxed);
    std::uint64_t took = std::max<std::uint64_t>(now > last ? now - last : 0, 1);
    std::uint64_t most = std::clamp<std::uint64_t>(4000 / took, 16, 256);

    static const std::uint64_t SPAN_MS = env_u64(L"VICTORY_TEXTURE_STAGE_MS", 24);

    constexpr std::uint64_t PER_FRAME_CATCHING_UP = 128u << 20, SPAN_CATCHING_UP_MS = 80, LONG = 2000;
    static const bool catchup = danzi::env::not_set_or_not(L"VICTORY_TEXTURE_CATCHUP", L"0");
    std::uint64_t plain_ms = SPAN_MS;
    std::uint64_t per_frame = PER_FRAME, span_ms = SPAN_MS;
    if (catchup && census[0] > LONG && catching_up(0)) {
        per_frame = std::max(per_frame, PER_FRAME_CATCHING_UP);
        span_ms = std::max(span_ms, SPAN_CATCHING_UP_MS);
    }


    std::uint64_t bytes_at_most = per_frame;
    if (pass_budget) {
        span_ms = pass_budget->first;
        std::uint64_t spent = SPENT.load(std::memory_order_relaxed);
        bytes_at_most = spent + pass_budget->second < spent ? UINT64_MAX : spent + pass_budget->second;
    }
    auto began = std::chrono::steady_clock::now();
    auto span = std::chrono::milliseconds(span_ms);
    for (const auto& [index, write] : due) {
        if (SPENT.load(std::memory_order_relaxed) >= bytes_at_most || std::chrono::steady_clock::now() - began > span) {
            break;
        }
        auto handle = heap_handle(heap, index);
        std::optional<View> recorded = handle ? view(*handle) : std::nullopt;


        std::optional<std::uint64_t> partly;
        if (recorded && recorded->kind == View::Placed && !recorded->flag) {
            const std::uint32_t* desc = recorded->words.data() + 14;
            std::uint64_t address = std::uint64_t{desc[10]} | std::uint64_t{desc[11]} << 32;
            std::uint64_t len = placed_bytes_from(recorded->words.data(), first_mip(desc, 16)).value_or(1);
            std::uint64_t mapped = mapped_bytes(reinterpret_cast<const void*>(address), static_cast<usize>(len));
            if (mapped == 0) {
                TEXTURES_AWAY.fetch_add(1, std::memory_order_relaxed);

                host.texture_slot_gone(index);
                host.texture_slot_later(index, write, frame, most);
                continue;
            }
            if (mapped < len) {
                if (mapped <= host.texture_slot_mapped(index)) {
                    TEXTURES_PARTLY[0].fetch_add(1, std::memory_order_relaxed);
                    host.texture_slot_later(index, write, frame, most);
                    continue;
                }
                if (!committed(reinterpret_cast<const void*>(address), 1)) {
                    TEXTURES_PARTLY[2].fetch_add(1, std::memory_order_relaxed);
                }
                partly = mapped;
            }
        }


        if (recorded && recorded->kind == View::Shader) {
            This object = reinterpret_cast<This>(recorded->a);
            std::uint8_t* data = nullptr;
            std::uint64_t size = 0;
            if (!buffer_bytes(object, data, size) && !title_fills(object)) {
                host.texture_slot_done(index, write, std::nullopt);
                continue;
            }
        }


        if (!has_room) {
            bool has_its_texture = reupload_when_full() && handle && host.texture_slot_made_from(index, *handle);
            if (!has_its_texture) {
                if (++refused > 256) {
                    break;
                }
                continue;
            }
            std::uint64_t n = REUPLOADED_FULL.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || n % 500 == 0) {
                log::info(TARGET, "a texture already on the card is brought up to date although the card has no room for new ones",
                          {{"so_far", log::debug(n)}, {"slot", log::debug(index)}});
            }
        }
        std::optional<View> v = handle ? host.mirror(index, *handle, true) : std::nullopt;
        std::optional<View> placed = v ? placed_texture(*v) : std::nullopt;
        const View* shown = placed ? &*placed : v ? &*v : nullptr;
        if (!shown || shown->kind != View::Shader) {
            host.texture_slot_done(index, write, std::nullopt);
            continue;
        }
        This resource = reinterpret_cast<This>(shown->a);
        ID3D12Resource* t = host_resource(resource);
        if (!t || !title_written(t)) {

            host.texture_slot_done(index, write, std::nullopt);
            continue;
        }


        bool tabled = is_staged(resource);
        if (tabled) {
            upload_again(resource);
        }
        if (v) {
            readable(list, SRV, *v);
        }

        static std::atomic<bool> GONE{false};
        if (!GONE.load(std::memory_order_relaxed)) {
            if (auto why = device_removed()) {
                GONE.store(true, std::memory_order_relaxed);
                std::string text = v ? "Some(" + view_debug(*v) + ")" : std::string("None");
                log::error(TARGET, "the host device went while textures picked by index were made ready; last: " + text.substr(0, 300),
                           {{"why", log::debug(*why)}, {"slot", log::debug(index)}});
                log_removal_context();
                return;
            }
        }
        auto [bytes, whole] = staged(resource).value_or(std::pair<std::uint64_t, bool>{0, true});


        std::optional<std::pair<std::uint64_t, std::uint64_t>> again;
        if (partly) {
            again = std::pair{frame + 1, *partly};
        } else if (!whole) {
            again = std::pair{frame + 120, std::uint64_t{0}};
        }
        if (again) {
            TEXTURES_PARTLY[1].fetch_add(1, std::memory_order_relaxed);
        }
        if (tabled) {


            TEXTURES_TABLED.fetch_add(1, std::memory_order_relaxed);
            host.texture_slot_done(index, write, again);
            continue;
        }

        std::optional<std::pair<std::uint64_t, std::uint64_t>> memory;
        if (!again && heap_recheck()) {
            memory = staged_memory(resource);
        }
        unstage(resource);
        SPENT.fetch_add(bytes, std::memory_order_relaxed);
        TEXTURE_BYTES.fetch_add(bytes, std::memory_order_relaxed);
        TEXTURES.fetch_add(1, std::memory_order_relaxed);
        host.texture_slot_done(index, write, again);

        if (memory) {
            host.texture_slot_recheck(index, frame, *memory, fingerprint(memory->first, memory->second));
        }
    }

    std::uint64_t took_us = micros_since(began);
    catching_up(took_us > plain_ms * 1000 ? (took_us - plain_ms * 1000) / 1000 : 0);
}

}   
