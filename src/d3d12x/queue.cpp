// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/queue.rs (see sync.hpp): command queues and
// the line of what a queue holds after a wait for a fence not reached yet.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>

#include "core/env.hpp"
#include "core/log.hpp"
#include "core/win.hpp"
#include "device.hpp"
#include "draw.hpp"
#include "host.hpp"
#include "present.hpp"
#include "resource.hpp"
#include "list.hpp"
#include "resource.hpp"
#include "resource_iids.hpp"
#include "shader_cache.hpp"
#include "sync.hpp"

namespace danzi::d3d12x {

namespace {

const std::array<Iid, 4> QUEUE_IIDS = {
    guid(0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};


struct Held {
    enum What : std::uint32_t {

        Wait,

        Signal,

        Value,


        Run,
    } what;
    This fence = nullptr;
    std::uint64_t value = 0;
    ValueWait wait{};
    std::unique_ptr<Taken> run;
};


struct Line {
    std::vector<Held> items;
    std::size_t head = 0;

    bool empty() const { return head == items.size(); }
    Held& front() { return items[head]; }
    void pop_front() {
        if (++head == items.size()) {
            items.clear();
            head = 0;
        }
    }
    void push_back(Held&& h) { items.push_back(std::move(h)); }
};


struct Queue {
    Object object;
    std::uint32_t kind;
    QueueDesc desc;
    danzi::FastLock lock;
    Line held;
};
static_assert(offsetof(Queue, kind) == 0x10);

Queue& queue(This self) { return *reinterpret_cast<Queue*>(self); }


std::string wait_debug(const ValueWait& w) {
    return "ValueWait { address: " + std::to_string(w.address) + ", comparison: " + std::to_string(w.comparison) +
           ", reference: " + std::to_string(w.reference) + ", wide: " + (w.wide ? "true" : "false") + " }";
}
std::string waits_debug(std::span<const ValueWait> waits) {
    std::string out = "[";
    for (std::size_t i = 0; i < waits.size(); ++i) {
        out += (i ? ", " : "") + wait_debug(waits[i]);
    }
    return out + "]";
}


danzi::FastLock WAITING_LOCK;
std::vector<Queue*> WAITING;
std::atomic<std::size_t> WAITING_COUNT{0};

void add_waiting(Queue* q) {
    std::lock_guard lock(WAITING_LOCK);
    if (std::find(WAITING.begin(), WAITING.end(), q) == WAITING.end()) {
        WAITING.push_back(q);
        WAITING_COUNT.store(WAITING.size(), std::memory_order_release);
    }
}

void remove_waiting(Queue* q) {
    std::lock_guard lock(WAITING_LOCK);
    std::erase(WAITING, q);
    WAITING_COUNT.store(WAITING.size(), std::memory_order_release);
}

}   


bool waits() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_QUEUE_WAIT", L"0");
    return on;
}


bool split() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_SPLIT_WAITS", L"0");
    return on;
}


bool cuts() {
    static const bool on = waits() && split() && danzi::env::not_set_or_not(L"VICTORY_CUT_WAITS", L"0");
    return on;
}

namespace {


bool forces() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_WAIT_FORCE", L"0");
    return on;
}


danzi::FastLock RELEASING;

void release_held_now(bool all) {
    if (WAITING_COUNT.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::lock_guard one(RELEASING);


    static std::vector<Queue*> queues;
    for (;;) {
        {
            std::lock_guard lock(WAITING_LOCK);
            queues = WAITING;
        }
        bool moved = false;
        for (Queue* q : queues) {
            bool empty;
            {
                std::lock_guard lock(q->lock);
                while (!q->held.empty()) {
                    Held& head = q->held.front();
                    if (!all && head.what == Held::Wait && fence_completed(head.fence) < head.value) {
                        break;
                    }


                    if (!all && head.what == Held::Value && !(head.wait.met() || (late() && late_met(head.wait)))) {
                        break;
                    }
                    Held h = std::move(head);
                    q->held.pop_front();


                    if (h.what == Held::Value && all && !(h.wait.met() || (late() && late_met(h.wait)))) {
                        static std::atomic<std::uint64_t> seen{0};
                        if (seen.fetch_add(1, std::memory_order_relaxed) < 16) {
                            log::warn("victory_gpu::queue", "a wait for a memory value was not met at a present: the queue runs on",
                                      {{"queue", log::hex(reinterpret_cast<std::uintptr_t>(q))}, {"wait", wait_debug(h.wait)}});
                        }
                    }
                    if (h.what == Held::Run) {


                        if (Host* host = d3d12x::host(); host && late()) {


                            host->execute_late(std::move(*h.run));
                        } else if (host) {
                            std::vector<HostList*> runs;
                            runs.reserve(h.run->order.size());
                            for (std::size_t i : h.run->order) {
                                runs.push_back(h.run->lists[i].host.get());
                            }
                            host->execute(runs);

                            recycle(std::move(h.run->lists), h.run->order);
                        }
                        if (all) {
                            static std::atomic<bool> once{false};
                            if (!once.exchange(true, std::memory_order_relaxed)) {
                                log::warn("victory_gpu::queue", "a queue still held a submission at a present: its fence was not reached");
                            }
                        }
                    } else if (h.what == Held::Signal) {


                        if (!late_signal(h.fence, h.value)) {
                            fence_set(h.fence, h.value);
                        }
                    }
                    moved = true;
                }
                empty = q->held.empty();
            }
            if (empty) {
                remove_waiting(q);
            }
        }
        if (!moved) {
            break;
        }
    }
}


template <class Make>
bool hold(This self, Make&& make) {
    if (!self) {
        return false;
    }
    Queue& q = queue(self);
    std::lock_guard lock(q.lock);
    if (q.held.empty()) {
        return false;
    }
    q.held.push_back(make());
    return true;
}


std::vector<HostList*> host_lists(std::span<const This> lists) {
    std::vector<HostList*> out;
    out.reserve(lists.size());
    for (This l : lists) {
        if (HostList* h = host_list(l)) {
            out.push_back(h);
        }
    }
    return out;
}


void submit_cut(This self, std::span<const This> lists, Host& host) {
    Queue& q = queue(self);
    std::size_t first = 0;
    {
        std::lock_guard lock(q.lock);
        if (q.held.empty()) {
            first = static_cast<std::size_t>(std::find_if(lists.begin(), lists.end(), [](This l) { return cut_list(l); }) - lists.begin());
        }
    }
    if (first > 0 && late()) {


        host.execute_late(detach(lists.first(first)));
    } else if (first > 0) {
        auto now = host_lists(lists.first(first));
        host.execute(now);

        tables_done(lists.first(first));
    }
    if (first < lists.size()) {
        auto parts = detach_parts(lists.subspan(first));
        static std::atomic<std::uint64_t> seen{0};
        if (seen.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::uint64_t waits = 0;
            for (const Part& p : parts) {
                waits += p.waits.size();
            }
            log::info("victory_gpu::queue", "a submission is given to its queue part by part (its lists wait for memory values)",
                      {{"kind", log::debug(q.kind)},
                       {"lists", log::debug(static_cast<std::uint64_t>(lists.size()))},
                       {"run_at_once", log::debug(static_cast<std::uint64_t>(first))},
                       {"parts", log::debug(static_cast<std::uint64_t>(parts.size()))},
                       {"waits", log::debug(waits)}});
        }
        {
            std::lock_guard lock(q.lock);
            for (Part& p : parts) {
                for (const ValueWait& w : p.waits) {
                    Held h{Held::Value};
                    h.wait = w;
                    q.held.push_back(std::move(h));
                }
                Held run{Held::Run};
                run.run = std::make_unique<Taken>(std::move(p.taken));
                q.held.push_back(std::move(run));
            }
        }
        add_waiting(&q);
    }


    release_held(false);
}


void submit_split(This self, std::span<const This> lists, Host& host) {
    Queue& q = queue(self);
    auto by = waits_by_list(lists);
    std::size_t cut = 0;
    {
        std::lock_guard lock(q.lock);
        if (q.held.empty()) {
            cut = lists.size();
            for (std::size_t i = 0; i < lists.size(); ++i) {
                if (std::any_of(by[i].begin(), by[i].end(), [](const ValueWait& w) { return !w.met(); })) {
                    cut = i;
                    break;
                }
            }
        }
    }
    if (cut > 0) {
        auto now = host_lists(lists.first(cut));
        host.execute(now);
    }
    if (cut < lists.size()) {
        static std::atomic<std::uint64_t> seen{0};
        if (seen.fetch_add(1, std::memory_order_relaxed) < 8) {
            log::info("victory_gpu::queue", "lists of a submission are held for a memory value",
                      {{"kind", log::debug(q.kind)},
                       {"lists", log::debug(static_cast<std::uint64_t>(lists.size()))},
                       {"run_at_once", log::debug(static_cast<std::uint64_t>(cut))},
                       {"waits", waits_debug(by[cut])}});
        }
        auto rest = lists.subspan(cut);
        bool twice = false;
        for (std::size_t i = 0; i < rest.size() && !twice; ++i) {
            twice = std::find(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(i), rest[i]) != rest.begin() + static_cast<std::ptrdiff_t>(i);
        }
        std::vector<Held> pieces;
        for (std::size_t from = cut; from < lists.size();) {
            std::size_t to = lists.size();
            if (!twice) {
                for (std::size_t i = from + 1; i < lists.size(); ++i) {
                    if (!by[i].empty()) {
                        to = i;
                        break;
                    }
                }
            }
            for (std::size_t i = from; i < to; ++i) {
                for (const ValueWait& w : by[i]) {
                    Held h{Held::Value};
                    h.wait = w;
                    pieces.push_back(std::move(h));
                }
            }
            Held run{Held::Run};
            run.run = std::make_unique<Taken>(detach(lists.subspan(from, to - from)));
            pieces.push_back(std::move(run));
            from = to;
        }
        {
            std::lock_guard lock(q.lock);
            for (Held& h : pieces) {
                q.held.push_back(std::move(h));
            }
        }
        add_waiting(&q);
    }
    release_held(false);
}


void __stdcall execute(This self, std::uint32_t count, const This* lists) {
    if (!lists) {
        return;
    }
    Host* host = d3d12x::host();
    if (!host) {

        static std::atomic<bool> once{false};
        if (!once.exchange(true, std::memory_order_relaxed)) {
            log::warn("victory_gpu::queue", "command lists submitted; no host GPU runs them", {{"count", log::debug(count)}});
        }
        return;
    }
    std::span<const This> all(lists, count);
    if (cuts() && self) {
        submit_cut(self, all, *host);
        return;
    }
    if (waits() && split() && self) {
        submit_split(self, all, *host);
        return;
    }


    if (waits() && self) {
        auto unmet = unmet_waits(all);
        if (!unmet.empty()) {
            Queue& q = queue(self);
            static std::atomic<std::uint64_t> seen{0};
            if (seen.fetch_add(1, std::memory_order_relaxed) < 8) {
                log::info("victory_gpu::queue", "a submission is held for a memory value", {{"kind", log::debug(q.kind)}, {"unmet", waits_debug(unmet)}});
            }
            {
                std::lock_guard lock(q.lock);
                for (const ValueWait& w : unmet) {
                    Held h{Held::Value};
                    h.wait = w;
                    q.held.push_back(std::move(h));
                }
            }
            add_waiting(&q);
        }
    }
    bool kept = hold(self, [&] {
        Held run{Held::Run};
        run.run = std::make_unique<Taken>(detach(all));
        return run;
    });
    if (kept) {
        return;
    }
    host->execute(host_lists(all));
    tables_done(all);


    release_held(false);
}


void __stdcall execute_x(This self, std::uint32_t count, const This* lists, std::uint64_t, std::uint32_t) { execute(self, count, lists); }

HRESULT __stdcall query(This self, const Iid* iid, This* out) { return answer(QUEUE, iid, is(iid, QUEUE_IIDS) ? self : nullptr, out); }

std::uint32_t __stdcall release(This self) {
    std::uint32_t left = release_boxed<Queue>(self);
    if (left == 0) {
        remove_waiting(reinterpret_cast<Queue*>(self));
    }
    return left;
}


void __stdcall marker(This) { DANZI_EMPTY_BODY; }

HRESULT __stdcall signal(This self, This fence, std::uint64_t value) {
    if (!fence) {
        return E_INVALIDARG_;
    }

    if (hold(self, [&] { return Held{Held::Signal, fence, value}; })) {
        return S_OK_;
    }


    if (late_signal(fence, value)) {
        return S_OK_;
    }
    return fence_signal(fence, value);
}


HRESULT __stdcall signal2_x(This self, This fence, std::uint64_t value, const std::uint8_t*, const std::uint8_t*) { return signal(self, fence, value); }


HRESULT __stdcall wait(This self, This fence, std::uint64_t value) {
    if (!fence) {
        return E_INVALIDARG_;
    }
    if (self) {
        std::uint64_t now = fence_completed(fence);
        Queue& q = queue(self);

        static std::atomic<std::uint64_t> calls{0};
        if (calls.fetch_add(1, std::memory_order_relaxed) < 8) {
            log::info("victory_gpu::queue", "a queue is told to wait for a fence", {{"kind", log::debug(q.kind)}, {"now", log::debug(now)}, {"value", log::debug(value)}});
        }
        bool behind;
        {
            std::lock_guard lock(q.lock);
            behind = !q.held.empty() || now < value;
            if (behind && waits()) {
                q.held.push_back(Held{Held::Wait, fence, value});
            }
        }
        if (behind) {
            static std::atomic<std::uint64_t> seen{0};
            if (seen.fetch_add(1, std::memory_order_relaxed) < 8) {
                log::info("victory_gpu::queue", "a queue waits for a fence not reached yet",
                          {{"kind", log::debug(q.kind)}, {"now", log::debug(now)}, {"value", log::debug(value)}, {"held", log::debug(waits())}});
            }
        }
        if (behind && waits()) {
            add_waiting(&q);

            release_held(false);
        }
    }
    return S_OK_;
}


constexpr std::uint64_t TIMESTAMP_FREQUENCY = 100'000'000;

HRESULT __stdcall timestamp_frequency(This, std::uint64_t* out) {
    if (!out) {
        return E_INVALIDARG_;
    }
    *out = TIMESTAMP_FREQUENCY;
    return S_OK_;
}


std::atomic<std::uint64_t> FRAMES{0};


struct Plane {
    std::uint64_t token;
    std::uint32_t count;
    const This* resources;
};
constexpr std::size_t PLANE_STRIDE = 0x48;


HRESULT __stdcall present(This, std::uint32_t planes, const void* plane_parameters, const std::uint8_t*) {


    if (late() && !late_free_present()) {
        settle_for_present();
        release_held_now(false);
        settle_for_present();
    } else if (late()) {

        release_held_now(false);
    }
    release_held(forces());
    std::uint64_t frame = FRAMES.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((frame & (frame - 1)) == 0) {
        auto [draws, dispatches] = take_counts();
        static std::atomic<std::uint64_t> last_drawn{0}, last_dispatched{0};
        std::uint64_t drawn_now = drawn(), dispatched_now = dispatched();
        std::uint64_t drawn_since = drawn_now - last_drawn.exchange(drawn_now, std::memory_order_relaxed);
        std::uint64_t dispatched_since = dispatched_now - last_dispatched.exchange(dispatched_now, std::memory_order_relaxed);
        log::info("victory_gpu::queue", "frame presented (since the last line: draws and dispatches, and those run on the host)",
                  {{"frame", log::debug(frame)},
                   {"draws", log::debug(draws)},
                   {"drawn", log::debug(drawn_since)},
                   {"dispatches", log::debug(dispatches)},
                   {"dispatched", log::debug(dispatched_since)}});
        if (frame >= 256) {
            auto [hits, misses] = shader_cache_counts();
            log::info("victory_gpu::queue", "shaders from the cache and translated so far", {{"hits", log::debug(hits)}, {"misses", log::debug(misses)}});
        }
    }
    if (planes == 0 || !plane_parameters) {
        return S_OK_;
    }
    const auto* plane = static_cast<const Plane*>(plane_parameters);
    if (plane->count == 0 || !plane->resources) {
        return S_OK_;
    }
    This resource = plane->resources[0];
    ID3D12Resource* texture = host_resource(resource);
    if (!texture || !host()) {
        return S_OK_;
    }
    constexpr std::uint32_t COPY_SOURCE = 0x800, READABLE = 0x80;
    auto moved = host_transition(resource, COPY_SOURCE);
    std::uint32_t state = moved ? moved->second : COPY_SOURCE;


    std::optional<std::pair<This, std::pair<ID3D12Resource*, std::uint32_t>>> second;
    if (planes >= 2) {
        const auto* p2 = reinterpret_cast<const Plane*>(static_cast<const std::uint8_t*>(plane_parameters) + PLANE_STRIDE);
        if (p2->count > 0 && p2->resources && p2->resources[0] && p2->resources[0] != resource) {
            This r = p2->resources[0];
            if (ID3D12Resource* t = host_resource(r)) {
                auto was = host_transition(r, READABLE);
                second = std::pair{r, std::pair{t, was ? was->second : READABLE}};
            }
        }
    }
    std::optional<std::pair<ID3D12Resource*, std::uint32_t>> over;
    if (second) {
        over = second->second;
    }
    present_texture(texture, state, frame, over);
    if (second) {
        host_transition(second->first, second->second.second);
    }
    host_transition(resource, state);
    return S_OK_;
}

void __stdcall desc(This self, QueueDesc* out) {
    if (out) {
        *out = queue(self).desc;
    }
}


struct Method {
    std::size_t slot;
    const void* method;
};
const Method METHODS[] = {
    {0, fn(&query)},
    {2, fn(&release)},
    {7, fn(&parent_device)},
    {10, fn(&execute)},
    {33, fn(&execute_x)},
    {11, fn(&marker)},
    {12, fn(&marker)},
    {13, fn(&marker)},
    {14, fn(&signal)},
    {15, fn(&wait)},
    {16, fn(&timestamp_frequency)},
    {26, fn(&desc)},
    {29, fn(&copy_page_mappings)},
    {30, fn(&present)},
    {32, fn(&signal2_x)},
};


constexpr std::size_t newer_slot(std::size_t slot) { return slot >= 14 ? slot + 2 : slot; }

Table<36> queue_table(bool newer) {
    auto f = d3d12_object(missing_table<QUEUE, 36>());
    for (const Method& m : METHODS) {
        f[newer ? newer_slot(m.slot) : m.slot] = m.method;
    }
    return f;
}

const void* const* functions(bool newer) {
    static const Table<36> older = queue_table(false);
    static const Table<36> newer_ = queue_table(true);
    return newer ? newer_.data() : older.data();
}
const bool MADE = (functions(false), functions(true), true);

}   

void Written::write(std::uint8_t* at, std::size_t bytes) const {
    std::uint64_t v = value;
    if (kind == GpuTimestamp) {

        static const LARGE_INTEGER start = [] {
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            return c;
        }();
        static const std::uint64_t freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<std::uint64_t>(f.QuadPart);
        }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        std::uint64_t t = static_cast<std::uint64_t>(c.QuadPart - start.QuadPart);
        std::uint64_t ns = t / freq * 1'000'000'000 + t % freq * 1'000'000'000 / freq;
        v = ns / (1'000'000'000 / TIMESTAMP_FREQUENCY);
    } else if (kind == SystemTimestamp) {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        v = static_cast<std::uint64_t>(c.QuadPart);
    }
    if (bytes == 4) {
        auto low = static_cast<std::uint32_t>(v);
        std::memcpy(at, &low, 4);
    } else {
        std::memcpy(at, &v, 8);
    }
}

void release_held(bool all) {


    release_held_now(all);
}

HRESULT __stdcall create_queue(This, const QueueDesc* desc, const Iid* iid, This* out) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    auto* q = new Queue{Object{functions(newer())}, desc->kind, *desc};
    return hand_out(QUEUE, q, iid, QUEUE_IIDS, out);
}


HRESULT __stdcall create_queue_x(This self, const QueueDescX* x, const Iid* iid, This* out) {
    if (!x) {
        return E_INVALIDARG_;
    }
    QueueDesc d{x->kind, 0, x->flags, 1};
    return create_queue(self, &d, iid, out);
}

}   
