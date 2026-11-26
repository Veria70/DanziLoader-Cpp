// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/host.rs, the submission core (see
// host.hpp).
#include "host.hpp"

#include <windows.h>
#include <psapi.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <immintrin.h>
#include <chrono>
#include <deque>
#include <set>
#include <tuple>
#include <condition_variable>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "core/env.hpp"
#include "core/inplace.hpp"
#include "core/log.hpp"
#include "core/memcopy.hpp"
#include "shader/lower.hpp"
#include "core/title_copy.hpp"
#include "core/watch.hpp"
#include "core/wide.hpp"
#include "list.hpp"
#include "pipeline.hpp"
#include "descriptor.hpp"
#include "resource.hpp"
#include "texture.hpp"

namespace danzi::d3d12x {

namespace {


std::mutex RETIRED_LOCK;
std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>> RETIRED;
std::atomic<std::uint64_t> STARTED{0}, FINISHED{0};


std::atomic<std::uint64_t> MESH_GROUPS[2]{};


std::atomic<std::uint64_t> RETIREMENTS[2]{};


std::mutex TRAIL_LOCK;
std::deque<std::tuple<std::uint64_t, std::uint32_t, std::string>> TRAIL;
constexpr usize TRAIL_MOST = 96;


std::atomic<std::uint64_t> SUBMITTING[3]{};

void submitting(std::uint64_t phase, usize lists) {
    SUBMITTING[1].store(lists, std::memory_order_relaxed);
    SUBMITTING[2].store(now_us(), std::memory_order_relaxed);
    SUBMITTING[0].store(phase, std::memory_order_relaxed);
}


struct Disposer {
    std::mutex lock;
    std::condition_variable wake, idle;
    std::vector<std::shared_ptr<void>> queue;
    bool busy = false;
    std::atomic<std::uint32_t> hurry{0};

    static std::uint64_t gap_times() {
        static const std::uint64_t g = [] {
            auto v = danzi::env::get(L"VICTORY_RETIRE_GAP");
            return v ? std::wcstoull(v->c_str(), nullptr, 10) : 3;
        }();
        return g;
    }

    void run() {
        constexpr std::uint64_t LONG_NS = 50'000, MOST_GAP_NS = 2'000'000;
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        std::uint64_t times = gap_times();
        std::vector<std::shared_ptr<void>> batch;
        for (;;) {
            {
                std::unique_lock l(lock);
                busy = false;
                if (queue.empty()) {
                    idle.notify_all();
                }
                wake.wait(l, [&] { return !queue.empty(); });
                batch.swap(queue);
                busy = true;
            }
            while (!batch.empty()) {
                auto start = std::chrono::steady_clock::now();
                batch.back().reset();
                batch.pop_back();
                auto took = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
                if (times == 0 || took < LONG_NS || batch.empty() || hurry.load(std::memory_order_relaxed) != 0) {
                    continue;
                }
                std::uint64_t gap = std::min(took * times, MOST_GAP_NS);
                if (timer) {
                    LARGE_INTEGER due;
                    due.QuadPart = -static_cast<LONGLONG>(gap / 100);   
                    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
                        WaitForSingleObject(timer, INFINITE);
                    }
                } else {
                    Sleep(1);
                }
            }
        }
    }
};

Disposer* disposer() {
    static Disposer* const made = []() -> Disposer* {
        if (!danzi::env::not_set_or_not(L"VICTORY_RETIRE_THREAD", L"0")) {
            return nullptr;
        }

        auto* d = new Disposer;
        try {
            std::thread([d] {
                SetThreadDescription(GetCurrentThread(), L"victory-retire");
                d->run();
            }).detach();
        } catch (...) {
            delete d;
            return nullptr;
        }
        return d;
    }();
    return made;
}

void dispose_one(std::shared_ptr<void>&& object) {
    Disposer* d = disposer();
    if (!d) {
        object.reset();
        return;
    }
    bool was_empty;
    {
        std::lock_guard l(d->lock);
        was_empty = d->queue.empty();
        d->queue.push_back(std::move(object));
    }
    if (was_empty) {
        d->wake.notify_one();
    }
}

void bury(std::uint64_t finished) {
    std::vector<std::shared_ptr<void>> done;
    {
        std::lock_guard lock(RETIRED_LOCK);
        if (RETIRED.empty()) {
            return;
        }
        auto keep = std::partition(RETIRED.begin(), RETIRED.end(), [&](const auto& r) { return r.first > finished; });
        for (auto it = keep; it != RETIRED.end(); ++it) {
            done.push_back(std::move(it->second));
        }
        RETIRED.erase(keep, RETIRED.end());
    }
    dispose(std::move(done));
}


std::uint64_t us_since(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
}


bool join_copies() {
    static const bool on = danzi::env::is(L"VICTORY_JOIN_COPIES", L"1");
    return on;
}


bool continues(const Step& last, usize dst, usize src, usize len) {
    usize d = last.dst, s = last.src, n = last.len;
    if (n > SIZE_MAX - len) {
        return false;
    }
    usize all = n + len;
    bool apart = (d <= SIZE_MAX - all && d + all <= s) || (s <= SIZE_MAX - all && s + all <= d);
    return d <= SIZE_MAX - n && d + n == dst && s <= SIZE_MAX - n && s + n == src && apart;
}


bool known_writes() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_KNOWN_WRITES", L"0");
    return on;
}


bool wide_refresh() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_WIDE_REFRESH", L"0");
    return on;
}


bool texture_overlap() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_TEXTURE_OVERLAP", L"0");
    return on;
}


class Group {
public:
    void spawn(std::function<void()> job) {
        {
            std::lock_guard lock(left_lock_);
            ++left_;
        }
        pool().push([this, job = std::move(job)] {
            job();
            std::lock_guard lock(left_lock_);
            if (--left_ == 0) {
                done_.notify_all();
            }
        });
    }
    bool busy() {
        std::lock_guard lock(left_lock_);
        return left_ > 0;
    }
    void join() {
        std::unique_lock lock(left_lock_);
        done_.wait(lock, [&] { return left_ == 0; });
    }
    ~Group() { join(); }

private:
    struct Pool {
        std::mutex lock;
        std::condition_variable more;
        std::deque<std::function<void()>> jobs;
        void push(std::function<void()> job) {
            {
                std::lock_guard l(lock);
                jobs.push_back(std::move(job));
            }
            more.notify_one();
        }
    };
    static Pool& pool() {
        static Pool* const p = [] {
            auto* p = new Pool;
            unsigned n = std::clamp(std::thread::hardware_concurrency() / 6, 1u, 3u);
            for (unsigned i = 0; i < n; ++i) {
                std::thread([p] {
                    for (;;) {
                        std::function<void()> job;
                        {
                            std::unique_lock l(p->lock);
                            p->more.wait(l, [&] { return !p->jobs.empty(); });
                            job = std::move(p->jobs.front());
                            p->jobs.pop_front();
                        }
                        job();
                    }
                }).detach();
            }
            return p;
        }();
        return *p;
    }
    std::mutex left_lock_;
    std::condition_variable done_;
    usize left_ = 0;
};


struct Batch {
    struct Refresh {
        std::shared_ptr<shadow::Shadow> shadow;
        usize from, to;
    };
    std::vector<Refresh> refreshes;
    std::unordered_map<const shadow::Shadow*, usize> index;


    std::unordered_set<const Staging*> textures;
    Group textures_beside;


    bool texture_due(const Staging* staging) { return textures.insert(staging).second; }

    void join_textures() {
        if (textures_beside.busy()) {
            auto started = std::chrono::steady_clock::now();
            textures_beside.join();
            memcopy::add(memcopy::M::OverlapWaitUs, us_since(started));
        }
    }

    void add(const std::shared_ptr<shadow::Shadow>& shadow, usize address, usize len) {
        usize end = address + len;
        auto [it, fresh] = index.try_emplace(shadow.get(), refreshes.size());
        if (fresh) {
            refreshes.push_back({shadow, address, end});
        } else {
            Refresh& r = refreshes[it->second];
            r.from = std::min(r.from, address);
            r.to = std::max(r.to, end);
        }
    }


    void flush_for_write(const std::shared_ptr<shadow::Shadow>& shadow) {
        auto it = index.find(shadow.get());
        if (it == index.end()) {
            return;
        }
        if (wide_refresh() && refreshes.size() >= 4) {
            flush_refreshes();
            return;
        }
        usize at = it->second;
        index.erase(it);
        Refresh r = std::move(refreshes[at]);
        refreshes[at] = std::move(refreshes.back());
        refreshes.pop_back();
        if (at < refreshes.size()) {
            index[refreshes[at].shadow.get()] = at;
        }
        r.shadow->update(r.from, r.to - r.from);
    }


    void title_written() {
        flush();
        textures.clear();
    }

    void flush_refreshes() {
        index.clear();
        if (refreshes.empty()) {
            return;
        }

        bool ahead = refreshes.size() > 1;
        if (ahead) {
            std::vector<watch::Range> ranges;
            for (const Refresh& r : refreshes) {
                if (!r.shadow->unwatched() && !r.shadow->is_view_guarded()) {
                    ranges.push_back({r.from, r.to});
                }
            }
            watch::ask_ahead(ranges);
        }
        if (wide_refresh() && refreshes.size() >= 4) {

            std::vector<wide::Job> jobs;
            jobs.reserve(refreshes.size());
            for (Refresh& r : refreshes) {
                jobs.emplace_back([s = r.shadow, from = r.from, to = r.to] { s->update(from, to - from); });
            }
            refreshes.clear();
            wide::run(jobs);
        } else {
            for (Refresh& r : refreshes) {
                r.shadow->update(r.from, r.to - r.from);
            }
            refreshes.clear();
        }
        if (ahead) {
            watch::ask_ahead_done();
        }
    }

    void flush() {
        flush_refreshes();
        join_textures();
    }
};


void run_deferred(Deferred& d, Batch& batch) {
    for (Step& step : d.steps) {
        switch (step.kind) {
            case Step::Copy:
                batch.title_written();
                copy_mapped(reinterpret_cast<std::uint8_t*>(step.dst), reinterpret_cast<const std::uint8_t*>(step.src), step.len, true);

                watch::wrote(step.dst, step.len);
                break;
            case Step::Fill:
                copy_mapped(reinterpret_cast<std::uint8_t*>(step.dst), reinterpret_cast<const std::uint8_t*>(step.src), step.len, false);
                break;
            case Step::Refresh: batch.add(step.shadow, step.dst, step.len); break;
            case Step::Run:
            case Step::Host:
                if (step.kind == Step::Run) {
                    batch.title_written();
                }
                d.works[step.work]();
                if (step.kind == Step::Run) {
                    if (step.known && known_writes()) {
                        watch::wrote(step.dst, step.len);
                        memcopy::add(memcopy::M::RunsKept, 1);
                    } else {
                        watch::ask_once_end();
                        memcopy::add(memcopy::M::RunsEnded, 1);
                    }
                }
                break;
            case Step::GpuWrite:

                batch.flush_for_write(step.shadow);
                step.shadow->writing(step.dst, step.len);
                break;
            case Step::Texture:
                if (batch.texture_due(step.staging.get())) {
                    if (texture_overlap()) {
                        batch.textures_beside.spawn([s = step.staging] {
                            auto started = std::chrono::steady_clock::now();
                            refresh(*s);
                            memcopy::add(memcopy::M::Overlapped, 1);
                            memcopy::add(memcopy::M::OverlapUs, us_since(started));
                        });
                    } else {
                        refresh(*step.staging);
                    }
                }
                break;
        }
    }
}


std::vector<shadow::Declared> declared_writes(std::span<HostList* const> runs, std::vector<std::shared_ptr<shadow::Shadow>>& held) {
    std::vector<shadow::Declared> back;
    std::unordered_map<const shadow::Shadow*, usize> index;
    std::vector<std::unordered_set<std::uint64_t>> seen;
    for (HostList* list : runs) {
        list->with_deferred([&](Deferred& d) {
            for (const Step& s : d.steps) {
                if (s.kind != Step::GpuWrite) {
                    continue;
                }
                shadow::Range range{s.dst, s.dst + s.len};
                auto [it, fresh] = index.try_emplace(s.shadow.get(), back.size());
                if (fresh) {
                    back.emplace_back(s.shadow.get(), std::vector<shadow::Range>{});
                    seen.emplace_back();
                    held.push_back(s.shadow);
                }
                std::uint64_t key = static_cast<std::uint64_t>(range.first) * 0x9e3779b97f4a7c15ull ^ range.second;
                if (seen[it->second].insert(key).second) {
                    back[it->second].second.push_back(range);
                }
            }
            return 0;
        });
    }
    return back;
}


bool walls_trace() {
    static const bool on = danzi::env::is(L"VICTORY_WALLS_TRACE", L"1") || danzi::env::is(L"VICTORY_WALLS_TRACE", L"2");
    return on;
}

void read_backs(Deferred& d) {
    for (const Deferred::Back& b : d.backs) {

        if (const std::uint8_t* from = b.from) {
            usize stride = (b.row + 255) & ~usize{255};
            if (stride == b.pitch) {
                std::memcpy(reinterpret_cast<std::uint8_t*>(b.at), from, stride * (b.rows - 1) + b.row);
            } else {
                for (usize line = 0; line < b.rows; ++line) {
                    std::memcpy(reinterpret_cast<std::uint8_t*>(b.at) + line * b.pitch, from + line * stride, b.row);
                }
            }


            if (walls_trace() && b.rows == 1 && b.row <= 96) {
                float values[24] = {};
                std::memcpy(values, from, b.row & ~usize{3});
                std::string list = "[";
                for (usize i = 0; i < b.row / 4; ++i) {
                    list += (i ? ", " : "") + log::debug(values[i]);
                }
                log::info("victory_gpu::host", "walls trace: a read-back lands in title memory",
                          {{"frame", log::debug(frame_current())}, {"to", log::hex(b.at)}, {"bytes", log::debug(static_cast<std::uint64_t>(b.row))},
                           {"values", list + "]"}});
            }
        }
    }
}


std::pair<usize, usize> readable(usize at, usize len, bool all) {
    constexpr usize PAGE = 4096;
    usize first = at / PAGE, last = (at + (len - 1)) / PAGE;
    usize of = last - first + 1, there = 0;
    PSAPI_WORKING_SET_EX_INFORMATION info[256];
    for (usize page = first; page <= last;) {
        usize n = std::min<usize>(last - page + 1, 256);
        for (usize i = 0; i < n; ++i) {
            info[i] = {};
            info[i].VirtualAddress = reinterpret_cast<void*>((page + i) * PAGE);
        }
        bool asked = QueryWorkingSetEx(GetCurrentProcess(), info, static_cast<DWORD>(n * sizeof info[0])) != 0;
        for (usize i = 0; i < n; ++i) {
            bool valid = asked && (info[i].VirtualAttributes.Flags & 1) != 0;
            std::uint8_t byte;
            if (valid || danzi::host::read_into(reinterpret_cast<const std::uint8_t*>((page + i) * PAGE), &byte, 1)) {
                ++there;
            } else if (all) {
                return {there, of};
            }
        }
        page += n;
    }
    return {there, of};
}

bool committed_by_query(const void* at, usize len) {
    auto p = reinterpret_cast<usize>(at);
    usize end = p + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(p), &info, sizeof info) == 0 || info.State != MEM_COMMIT) {
            return false;
        }
        p = reinterpret_cast<usize>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

}   


HostList::HostList(ComPtr<ID3D12CommandAllocator> allocator, ComPtr<ID3D12GraphicsCommandList> list)
    : allocator_(std::move(allocator)), list_(std::move(list)) {
    bump_epoch();
}

HostList::~HostList() = default;

void HostList::close() {
    twins_to_depth();
    if (FAILED(list_->Close())) {
        fail("a host command list did not close: a command recorded into it was invalid");
    }
}

void HostList::reset() {
    uav_pending_.store(true, std::memory_order_relaxed);
    with_deferred([&](Deferred& d) {
        d = Deferred{};
        bump_epoch();
        return 0;
    });
    {
        std::lock_guard lock(states_lock_);
        states_.clear();
    }
    {
        std::lock_guard lock(twins_lock_);
        twins_.clear();
    }
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) {
        fail("a host command list did not reset");
    }
}

bool HostList::untouched() {
    bool none = with_deferred([](Deferred& d) {
        return d.steps.empty() && d.uploads.empty() && d.kept.empty() && d.textures.empty() && d.writes.empty() && d.backs.empty();
    });
    if (!none) {
        return false;
    }
    {
        std::lock_guard lock(states_lock_);
        if (!states_.empty()) {
            return false;
        }
    }
    std::lock_guard lock(twins_lock_);
    return twins_.empty();
}

void HostList::defer_copy(std::uint8_t* dst, const std::uint8_t* src, usize len) {
    with_deferred([&](Deferred& d) {


        if (!d.steps.empty() && d.steps.back().kind == Step::Copy &&
            continues(d.steps.back(), reinterpret_cast<usize>(dst), reinterpret_cast<usize>(src), len) && join_copies()) {
            d.steps.back().len += len;
            return 0;
        }
        Step s{Step::Copy};
        s.dst = reinterpret_cast<usize>(dst);
        s.src = reinterpret_cast<usize>(src);
        s.len = len;
        push_doubling(d.steps, std::move(s));


        if (!d.refreshed.empty() || !d.gpu_written.empty()) {
            if (!d.refreshed.empty()) {
                d.refreshed.clear();
            }
            if (!d.gpu_written.empty()) {
                d.gpu_written.clear();
            }
            bump_epoch();
        }
        return 0;
    });
}

void HostList::defer_fill(std::uint8_t* dst, const std::uint8_t* src, usize len) {
    with_deferred([&](Deferred& d) {
        Step s{Step::Fill};
        s.dst = reinterpret_cast<usize>(dst);
        s.src = reinterpret_cast<usize>(src);
        s.len = len;
        push_doubling(d.steps, std::move(s));
        return 0;
    });
}

std::atomic<std::uint64_t> RANGE_EPOCHS{1};


std::atomic<std::uint64_t> WINDOWS_LET_GO[2]{};


void sweep() {

    danzi::inplace::tick(now_ms());
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_SWEEP", L"0");
    if (!on) {
        return;
    }
    static std::atomic<std::uint64_t> LAST{0};
    std::uint64_t now = now_ms(), last = LAST.load(std::memory_order_relaxed);
    if (now < last + 1000 || !LAST.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    trim_library();
    if (Host* h = host()) {


        constexpr std::uint64_t IDLE_FRAMES = 4;
        std::uint64_t idle_ms = ram_idle_ms();
        auto gone = h->registry().let_go_idle([&](std::uint64_t used) { return long_ago(used, idle_ms, IDLE_FRAMES); },
                                              [](ID3D12Resource* r) {
                                                  r->AddRef();
                                                  return r->Release();
                                              });
        if (!gone.empty()) {
            trail([&] {
                std::uint64_t bytes = 0;
                for (const auto& s : gone) {
                    bytes += s->len();
                }
                return "sweep: " + std::to_string(gone.size()) + " host copies of windows of title memory let go (" + std::to_string(bytes >> 10) +
                       " KiB), held by no list and no slot";
            });
        }
        for (auto& s : gone) {
            WINDOWS_LET_GO[0].fetch_add(1, std::memory_order_relaxed);
            WINDOWS_LET_GO[1].fetch_add(s->len(), std::memory_order_relaxed);
            retire(std::move(s));
        }
    }
    textures_let_go_idle();
    resources_let_go_idle();
}

std::array<std::uint64_t, 2> windows_let_go() { return {WINDOWS_LET_GO[0].load(), WINDOWS_LET_GO[1].load()}; }

bool barrier_merge() {
    static const bool on = [] {
        bool merge = danzi::env::not_set_or_not(L"VICTORY_BARRIER_MERGE", L"0");
        if (!merge) {
            log::info("victory_gpu::host", "global UAV barrier deduplication off (VICTORY_BARRIER_MERGE=0)");
        }
        return merge;
    }();
    return on;
}

std::optional<std::uint64_t> HostList::budget() {
    if (!budget_) {
        Host* h = host();
        auto b = h ? h->device().shared_buffer(256) : std::nullopt;
        if (!b) {
            return std::nullopt;
        }
        budget_ = b->first;
        budget_data_ = b->second;
        std::memset(budget_data_, 0, 256);
    }
    bool zero = with_deferred([](Deferred& d) { return !std::exchange(d.budget_zeroed, true); });
    if (zero) {


        auto* words = reinterpret_cast<volatile std::uint32_t*>(budget_data_);
        defer_host([words] {
            std::uint64_t largest = words[1], all = words[2];
            for (std::uint64_t was = MESH_GROUPS[0].load(std::memory_order_relaxed);
                 largest > was && !MESH_GROUPS[0].compare_exchange_weak(was, largest, std::memory_order_relaxed);) {
            }
            MESH_GROUPS[1].fetch_add(all, std::memory_order_relaxed);
            for (int k = 0; k < 3; ++k) {
                words[k] = 0;
            }
        });
    }
    return budget_->GetGPUVirtualAddress();
}

ID3D12Resource* HostList::indirect_scratch(std::uint64_t size) {
    std::uint64_t needed = std::max<std::uint64_t>(size, 64u << 10);
    std::lock_guard lock(indirect_lock_);
    if (indirect_args_ && indirect_cap_ >= needed) {
        return indirect_args_.Get();
    }
    std::uint64_t alloc = std::max(indirect_cap_ * 2, needed);
    alloc = (alloc + (64u << 10) - 1) & ~std::uint64_t((64u << 10) - 1);
    Host* h = host();
    auto buffer = h ? h->device().uav_buffer(alloc) : nullptr;
    if (!buffer) {
        return nullptr;
    }
    if (indirect_args_) {

        keep(ComPtr<IUnknown>(indirect_args_.Get()));
    }
    indirect_args_ = buffer;
    indirect_cap_ = alloc;
    return indirect_args_.Get();
}


void HostList::bump_epoch() { epoch_.store(RANGE_EPOCHS.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

void HostList::defer_refresh(const std::shared_ptr<shadow::Shadow>& shadow, usize address, usize len) {
    with_deferred([&](Deferred& d) {
        auto copy = reinterpret_cast<usize>(shadow.get());
        if (d.refreshed.insert({copy, address, len}).second) {
            std::erase_if(d.gpu_written, [&](const Deferred::Key& k) { return k.copy == copy; });
            bump_epoch();
            Step s{Step::Refresh};
            s.shadow = shadow;
            s.dst = address;
            s.len = len;
            push_doubling(d.steps, std::move(s));
        }
        return 0;
    });
}

void HostList::defer_gpu_write(const std::shared_ptr<shadow::Shadow>& shadow, usize address, usize len) {
    with_deferred([&](Deferred& d) {
        if (d.gpu_written.insert({reinterpret_cast<usize>(shadow.get()), address, len}).second) {
            Step s{Step::GpuWrite};
            s.shadow = shadow;
            s.dst = address;
            s.len = len;
            push_doubling(d.steps, std::move(s));
        }
        return 0;
    });
}

void HostList::defer_read_back(const std::uint8_t* from, std::uint8_t* dst, usize row, usize rows, usize pitch) {
    with_deferred([&](Deferred& d) {
        d.backs.push_back({from, reinterpret_cast<usize>(dst), row, rows, pitch});
        return 0;
    });
}

void HostList::defer_write(std::uint8_t* dst, Written::Kind kind, std::uint64_t value, usize bytes) {
    with_deferred([&](Deferred& d) {


        if (d.writes.size() == d.writes.capacity()) {
            d.writes.reserve(std::max<std::size_t>(16, d.writes.capacity() * 2));
        }
        Deferred::Write& w = d.writes.emplace_back();
        w.at = reinterpret_cast<usize>(dst);
        w.what.kind = kind;
        w.what.value = value;
        w.bytes = bytes;
        return 0;
    });
}

void HostList::defer(std::function<void()> work) {
    with_deferred([&](Deferred& d) {
        Step s{Step::Run};
        s.work = static_cast<std::uint32_t>(d.works.size());
        d.works.push_back(std::move(work));
        push_doubling(d.steps, std::move(s));


        if (!d.refreshed.empty() || !d.gpu_written.empty()) {
            if (!d.refreshed.empty()) {
                d.refreshed.clear();
            }
            if (!d.gpu_written.empty()) {
                d.gpu_written.clear();
            }
            bump_epoch();
        }
        return 0;
    });
}

void HostList::defer_known(usize dst, usize src, usize len, std::function<void()> work) {
    with_deferred([&](Deferred& d) {
        Step s{Step::Run};
        s.work = static_cast<std::uint32_t>(d.works.size());
        d.works.push_back(std::move(work));
        s.known = true;
        s.dst = dst;
        s.src = src;
        s.len = len;
        push_doubling(d.steps, std::move(s));


        if (!d.refreshed.empty() || !d.gpu_written.empty()) {
            if (!d.refreshed.empty()) {
                d.refreshed.clear();
            }
            if (!d.gpu_written.empty()) {
                d.gpu_written.clear();
            }
            bump_epoch();
        }
        return 0;
    });
}

void HostList::defer_host(std::function<void()> work) {
    with_deferred([&](Deferred& d) {
        Step s{Step::Host};
        s.work = static_cast<std::uint32_t>(d.works.size());
        d.works.push_back(std::move(work));
        push_doubling(d.steps, std::move(s));
        return 0;
    });
}

void HostList::keep(ComPtr<IUnknown> object) {
    with_deferred([&](Deferred& d) {
        d.kept.push_back(std::move(object));
        return 0;
    });
}

bool HostList::first_use(usize key) {
    return with_deferred([&](Deferred& d) { return d.textures.insert(key).second; });
}


struct Host::Sink : BufferSink {
    gpu::Registry& registry;
    explicit Sink(gpu::Registry& r) : registry(r) {}
    void made(std::uintptr_t resource, std::uint64_t start, std::uint64_t len) override { registry.register_buffer(resource, start, len); }
    void released(std::uintptr_t resource, std::uint64_t start) override { registry.unregister(resource, start); }
};

Host::Host(std::unique_ptr<gpu::Device> device) : device_(std::move(device)) {


    if (device_->features().wave_lanes_max != 0) {
        log::info("victory_gpu::host", "host wave lanes",
                  {{"min", log::debug(device_->features().wave_lanes_min)}, {"max", log::debug(device_->features().wave_lanes_max)}});
        shader::lower::set_host_max_wave(device_->features().wave_lanes_max);
    }


    D3D12_DESCRIPTOR_HEAP_DESC rd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    device_->device()->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtvs_));
    device_->device()->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&dsvs_));
    rtv_size_ = device_->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsv_size_ = device_->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (int depth = 0; depth < 2; ++depth) {
        D3D12_DESCRIPTOR_HEAP_DESC kd{depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4096, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (SUCCEEDED(device_->device()->CreateDescriptorHeap(&kd, IID_PPV_ARGS(&kept_[depth])))) {
            std::uint64_t base = kept_[depth]->GetCPUDescriptorHandleForHeapStart().ptr;
            UINT step = depth ? dsv_size_ : rtv_size_;
            for (std::uint64_t i = 4096; i-- > 0;) {
                kept_free_[depth].push_back(base + i * step);
            }
        }
    }
    card_ = std::make_unique<gpu::CardHost>(*device_);
    registry_ = std::make_unique<gpu::Registry>(*card_);
    registry_->set_clock(&moment);
    gpu::Device::set_frame_clock(&frame_current);
    sink_ = std::make_unique<Sink>(*registry_);
    adopt_buffers(sink_.get());
    log::info("victory_gpu::host", "host D3D12 device ready");
}

Host::~Host() { adopt_buffers(nullptr); }

std::unique_ptr<HostList> Host::new_list(HRESULT* hr) {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    HRESULT r;
    if (FAILED(r = device_->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(r = device_->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        if (hr) {
            *hr = r;
        }
        return nullptr;
    }
    return std::make_unique<HostList>(std::move(allocator), std::move(list));
}


namespace {


constexpr int WHY_SYNC = 2, WHY_CARD_DONE = 3, WHY_VALUE = 4, WHY_STALE = 5, WHY_READ = 6, WHY_RUN = 7, WHY_MOST = 8, WHY_WRITES = 9,
              WHY_FENCE = 10;


bool late_exact() { return gpu::late_stage() > 1; }


bool relay_exact() {
    static const bool on = gpu::relay() && [] {
        auto v = danzi::env::get(L"VICTORY_RELAY");
        return !v || *v == L"2";
    }();
    return on;
}


usize late_most() {
    static const usize most = [] {
        auto v = danzi::env::get(L"VICTORY_LATE_WAIT_LISTS");
        try {
            return std::max<usize>(v ? std::stoull(*v) : 256, 1);
        } catch (...) {
            return usize{256};
        }
    }();
    return most;
}


bool late_writes() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_LATE_WAIT_WRITES", L"0");
    return on;
}

}   


struct Sent {
    std::uint64_t number = 0;
    std::uint64_t fence = 0;
    std::optional<gpu::CardPlan> card;
    std::vector<shadow::Declared> back;
    std::vector<std::shared_ptr<shadow::Shadow>> held;
    std::vector<Deferred::Write> writes;
};


struct Flight {
    std::uint64_t number = 0;

    std::uint64_t fence = 0;
    Taken lists;
    std::optional<gpu::CardPlan> card;
    std::vector<shadow::Declared> back;
    std::vector<std::shared_ptr<shadow::Shadow>> held;
};


struct LateItem {
    enum Kind : std::uint8_t { Run, Write, Signal } kind;
    std::unique_ptr<Flight> flight;
    Deferred::Write write{};
    This fence = nullptr;
    std::uint64_t value = 0;
};

namespace {

struct Line {
    std::vector<LateItem> line;

    std::vector<shadow::Range> written;

    usize lists = 0;

    std::uint64_t target = 0;

    bool settling = false;


    bool poke = false;


    std::vector<Taken> landed;

    std::vector<Deferred::Write> in_hand;
};
std::mutex LINE_LOCK;
Line LINE;


std::condition_variable LINE_MOVED, RELAY_MOVED, LINE_LANDED;


bool touches(const std::vector<shadow::Range>& ranges, usize from, usize to) {
    if (from >= to) {
        return false;
    }
    auto at = std::partition_point(ranges.begin(), ranges.end(), [&](const shadow::Range& r) { return r.second <= from; });
    return at != ranges.end() && at->first < to;
}


void join_ranges(std::vector<shadow::Range>& ranges, const std::vector<shadow::Range>& more) {
    for (const auto& r : more) {
        if (r.first < r.second) {
            ranges.push_back(r);
        }
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<shadow::Range> out;
    out.reserve(ranges.size());
    for (const auto& r : ranges) {
        if (!out.empty() && r.first <= out.back().second) {
            out.back().second = std::max(out.back().second, r.second);
        } else {
            out.push_back(r);
        }
    }
    ranges = std::move(out);
}

std::vector<shadow::Range> ranges_of(const std::vector<shadow::Declared>& back) {
    std::vector<shadow::Range> all;
    for (const auto& [_, ranges] : back) {
        all.insert(all.end(), ranges.begin(), ranges.end());
    }
    return all;
}


bool flying() {
    if (!late()) {
        return false;
    }
    std::lock_guard lock(LINE_LOCK);
    return LINE.settling || std::any_of(LINE.line.begin(), LINE.line.end(), [](const LateItem& i) { return i.kind == LateItem::Run; });
}


std::optional<int> blocked_by(const std::vector<shadow::Range>& written, std::span<HostList* const> runs, bool count) {
    if (written.empty()) {
        return std::nullopt;
    }
    for (HostList* list : runs) {
        auto why = list->with_deferred([&](Deferred& d) -> std::optional<int> {
            for (const Step& s : d.steps) {
                switch (s.kind) {
                    case Step::Copy:
                        if (touches(written, s.src, s.src + s.len) || touches(written, s.dst, s.dst + s.len)) {
                            return WHY_READ;
                        }
                        break;
                    case Step::Fill:
                        if (touches(written, s.src, s.src + s.len)) {
                            return WHY_READ;
                        }
                        break;
                    case Step::Texture: {
                        auto [at, len] = staging_memory(*s.staging);
                        if (touches(written, at, at + len)) {
                            return WHY_READ;
                        }
                        break;
                    }
                    case Step::Run:
                        if (s.known && known_writes()) {
                            if (touches(written, s.src, s.src + s.len) || touches(written, s.dst, s.dst + s.len)) {
                                return WHY_READ;
                            }
                            if (count) {
                                memcopy::add(memcopy::M::RunsNotBlocking, 1);
                            }
                        } else {
                            return WHY_RUN;
                        }
                        break;
                    default: break;
                }
            }
            return std::nullopt;
        });
        if (why) {
            return why;
        }
    }
    return std::nullopt;
}

std::optional<int> late_blocked(std::span<HostList* const> runs) {
    std::vector<shadow::Range> written;
    {
        std::lock_guard lock(LINE_LOCK);
        if (LINE.written.empty()) {
            return std::nullopt;
        }
        written = LINE.written;
    }
    return blocked_by(written, runs, true);
}


bool stale_under_flights(std::span<HostList* const> runs) {
    struct Span {
        std::shared_ptr<shadow::Shadow> shadow;
        usize from, to;
    };
    std::vector<Span> spans;
    for (HostList* list : runs) {
        list->with_deferred([&](Deferred& d) {
            for (const Step& s : d.steps) {
                if (s.kind != Step::Refresh || !s.shadow->writes_pending()) {
                    continue;
                }
                auto it = std::find_if(spans.begin(), spans.end(), [&](const Span& x) { return x.shadow == s.shadow; });
                if (it == spans.end()) {
                    spans.push_back({s.shadow, s.dst, s.dst + s.len});
                } else {
                    it->from = std::min(it->from, s.dst);
                    it->to = std::max(it->to, s.dst + s.len);
                }
            }
            return 0;
        });
    }
    if (late_exact()) {
        usize runs_left = 512;
        return std::any_of(spans.begin(), spans.end(), [&](const Span& s) { return s.shadow->merge_under_writes(s.from, s.to - s.from, runs_left); });
    }
    return std::any_of(spans.begin(), spans.end(), [](const Span& s) { return s.shadow->stale_under_writes(s.from, s.to - s.from); });
}


std::uint64_t relay_spin_us() {
    static const std::uint64_t us = [] {
        auto v = danzi::env::get(L"VICTORY_RELAY_SPIN_US");
        return v ? std::wcstoull(v->c_str(), nullptr, 10) : 200;
    }();
    return us;
}


std::atomic<std::uint64_t> LANDINGS{0};


template <class Done>
void relay_wait(Done&& done) {
    std::unique_lock lock(LINE_LOCK);
    if (done(LINE)) {
        return;
    }

    if (std::uint64_t spin = relay_spin_us()) {
        auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spin);
        while (!done(LINE)) {
            std::uint64_t seen = LANDINGS.load(std::memory_order_acquire);
            lock.unlock();
            while (LANDINGS.load(std::memory_order_acquire) == seen && std::chrono::steady_clock::now() < until) {
                for (int k = 0; k < 32; ++k) {
                    _mm_pause();
                }
            }
            lock.lock();
            if (std::chrono::steady_clock::now() >= until) {
                break;
            }
        }
    }
    LINE_LANDED.wait(lock, [&] { return done(LINE); });
}


void land() {
    std::vector<Taken> landed;
    {
        std::lock_guard lock(LINE_LOCK);
        landed = std::move(LINE.landed);
        LINE.landed.clear();
    }
    for (Taken& t : landed) {
        recycle(std::move(t.lists), t.order);
    }
}


void late_helper() {
    Host* h = host();
    if (!h) {
        return;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        log::warn("victory_gpu::host", "no event for the thread that settles submissions sent ahead");
        return;
    }
    for (;;) {
        std::optional<std::uint64_t> target;
        {
            std::unique_lock lock(LINE_LOCK);
            for (;;) {
                if (LINE.poke) {
                    LINE.poke = false;
                    break;
                }

                if (!gpu::relay() && !LINE.line.empty() && !LINE.settling) {
                    target = LINE.target;
                    break;
                }
                LINE_MOVED.wait(lock);
            }
        }
        if (!target) {
            land();
            release_held(false);
            continue;
        }

        if (event && SUCCEEDED(h->fence()->SetEventOnCompletion(*target, event))) {
            WaitForSingleObject(event, INFINITE);
        }
        {
            std::lock_guard one(h->submission_lock());
            {
                std::lock_guard lock(LINE_LOCK);
                if (LINE.target != *target) {
                    continue;   
                }
            }
            h->settle(WHY_CARD_DONE);
        }
        land();
        release_held(false);
    }
}


void relay_worker() {
    Host* h = host();
    if (!h) {
        return;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        fail("no event for the thread that lands submissions sent ahead");
    }
    for (;;) {
        std::vector<LateItem> items;
        std::uint64_t target = 0;
        {
            std::unique_lock lock(LINE_LOCK);
            RELAY_MOVED.wait(lock, [] { return !LINE.line.empty(); });
            std::uint64_t completed = h->fence()->GetCompletedValue();

            usize n = 0;
            while (n < LINE.line.size() && !(LINE.line[n].kind == LateItem::Run && LINE.line[n].flight->fence > completed)) {
                ++n;
            }
            if (n == 0) {
                target = LINE.line[0].flight->fence;
            } else {
                LINE.settling = true;
                items.reserve(n);
                for (usize i = 0; i < n; ++i) {
                    items.push_back(std::move(LINE.line[i]));
                }
                LINE.line.erase(LINE.line.begin(), LINE.line.begin() + static_cast<std::ptrdiff_t>(n));
                LINE.in_hand.clear();
                for (const LateItem& i : items) {
                    if (i.kind == LateItem::Write) {
                        LINE.in_hand.push_back(i.write);
                    }
                }
            }
        }
        if (!items.empty()) {
            h->relay_land(std::move(items));
            continue;
        }

        if (std::uint64_t spin = relay_spin_us()) {
            auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spin);
            bool reached = false;
            while (!(reached = h->fence()->GetCompletedValue() >= target) && std::chrono::steady_clock::now() < until) {
                for (int k = 0; k < 64; ++k) {
                    _mm_pause();
                }
            }
            if (reached) {
                continue;
            }
        }
        if (FAILED(h->fence()->SetEventOnCompletion(target, event)) || WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) {
            Sleep(1);
        }
    }
}


void late_threads() {
    static std::once_flag once;
    std::call_once(once, [] {
        try {
            std::thread(late_helper).detach();
        } catch (const std::system_error& e) {
            log::warn("victory_gpu::host", "no thread to settle submissions sent ahead: they are settled by the calls that follow", {{"error", e.what()}});
        }
        log::info("victory_gpu::host", "submissions are sent to the card without a wait for each (VICTORY_LATE_WAIT=1)",
                  {{"most", log::debug(static_cast<std::uint64_t>(late_most()))}, {"writes", log::debug(late_writes())}});
        if (gpu::relay()) {
            std::thread(relay_worker).detach();
            log::info("victory_gpu::host", "what shaders wrote is copied back by a thread of its own, beside the submitting thread (VICTORY_RELAY=1)",
                      {{"exact", log::debug(relay_exact())}});
        }
    });
}

}   

bool late() { return gpu::late_stage() > 0; }

bool late_free_present() { return gpu::late_stage() > 2; }

bool late_signal(This fence, std::uint64_t value) {
    if (!late()) {
        return false;
    }
    {
        std::lock_guard lock(LINE_LOCK);
        if (LINE.line.empty() && !LINE.settling) {
            return false;
        }
        LateItem s{LateItem::Signal};
        s.fence = fence;
        s.value = value;
        LINE.line.push_back(std::move(s));
    }
    LINE_MOVED.notify_one();
    RELAY_MOVED.notify_one();
    return true;
}

namespace {

void settle_now(int why) {
    if (!late()) {
        return;
    }
    Host* h = host();
    if (!h) {
        return;
    }
    std::lock_guard one(h->submission_lock());
    h->settle(why);
}

}   

bool late_met(const ValueWait& wait) {
    usize address = static_cast<usize>(wait.address), bytes = wait.wide ? 8 : 4;
    {
        std::lock_guard lock(LINE_LOCK);
        if (LINE.line.empty() && LINE.in_hand.empty()) {
            return false;
        }
        bool unsure = touches(LINE.written, address, address + bytes);
        auto look = [&](const Deferred::Write& w) -> int {
            if (w.at >= address + bytes || address >= w.at + w.bytes) {
                return 0;
            }
            if (w.what.kind == Written::Value && w.at == address && w.bytes >= bytes) {
                return wait.holds_for(w.what.value) ? 1 : 0;
            }
            unsure = true;
            return 0;
        };

        for (const Deferred::Write& w : LINE.in_hand) {
            if (look(w) == 1) {
                return true;
            }
        }
        for (const LateItem& i : LINE.line) {
            if (i.kind == LateItem::Write && look(i.write) == 1) {
                return true;
            }
        }
        if (!unsure) {
            return false;
        }
    }
    settle_now(WHY_VALUE);
    return wait.met();
}

void settle_for_present() { settle_now(WHY_SYNC); }
void settle_for_fence() { settle_now(WHY_FENCE); }

ID3D12Fence* Host::fence() { return device_->fence(); }

void Host::execute(std::span<HostList* const> runs) {


    std::lock_guard one(submitting_);
    submit(runs, false);
}

std::uint64_t Host::finished() { return FINISHED.load(std::memory_order_acquire); }

std::uint64_t Host::execute_own(HostList& list) {
    late_threads();
    std::unique_lock one(submitting_);
    HostList* runs[1] = {&list};
    auto sent = submit(runs, true);
    if (!sent) {
        return 0;
    }
    std::uint64_t number = sent->number;
    {
        std::lock_guard lock(LINE_LOCK);
        join_ranges(LINE.written, ranges_of(sent->back));
        LINE.lists += 1;
        LINE.target = sent->fence;
        auto flight = std::make_unique<Flight>();
        flight->number = sent->number;
        flight->fence = sent->fence;
        flight->card = std::move(sent->card);
        flight->back = std::move(sent->back);
        flight->held = std::move(sent->held);
        LateItem run{LateItem::Run};
        run.flight = std::move(flight);
        LINE.line.push_back(std::move(run));
        for (const Deferred::Write& w : sent->writes) {
            LateItem write{LateItem::Write};
            write.write = w;
            LINE.line.push_back(std::move(write));
        }
    }
    LINE_MOVED.notify_one();
    RELAY_MOVED.notify_one();
    return number;
}

void Host::execute_late(Taken taken) {
    if (taken.order.empty()) {
        recycle(std::move(taken.lists), taken.order);
        return;
    }
    late_threads();
    std::unique_lock one(submitting_);
    std::vector<HostList*> runs;
    runs.reserve(taken.order.size());
    for (usize i : taken.order) {
        runs.push_back(taken.lists[i].host.get());
    }
    auto sent = submit(runs, true);
    bool told = !sent->writes.empty();
    bool most;
    {
        std::lock_guard lock(LINE_LOCK);
        join_ranges(LINE.written, ranges_of(sent->back));
        LINE.lists += taken.order.size();
        LINE.target = sent->fence;
        auto flight = std::make_unique<Flight>();
        flight->number = sent->number;
        flight->fence = sent->fence;
        flight->lists = std::move(taken);
        flight->card = std::move(sent->card);
        flight->back = std::move(sent->back);
        flight->held = std::move(sent->held);
        LateItem run{LateItem::Run};
        run.flight = std::move(flight);
        LINE.line.push_back(std::move(run));
        for (const Deferred::Write& w : sent->writes) {
            LateItem write{LateItem::Write};
            write.write = w;
            LINE.line.push_back(std::move(write));
        }
        most = LINE.lists >= late_most();
    }
    RELAY_MOVED.notify_one();
    if (told && !late_writes()) {
        settle(WHY_WRITES);
    } else if (most && relay_exact()) {

        relay_wait([](const Line& l) { return l.lists < late_most(); });
    } else if (most) {
        settle(WHY_MOST);
    } else {
        LINE_MOVED.notify_one();
    }
    one.unlock();
    land();
}

void Host::settle(int why) {
    (void)why;
    if (!late()) {
        return;
    }
    if (gpu::relay()) {


        relay_wait([](const Line& l) { return l.line.empty() && !l.settling; });
        return;
    }
    bool counted = false;
    for (;;) {
        std::vector<LateItem> items;
        std::uint64_t target;
        {
            std::lock_guard lock(LINE_LOCK);
            if (LINE.line.empty()) {
                LINE.settling = false;
                break;
            }
            target = LINE.target;
            LINE.settling = true;
            LINE.written.clear();
            LINE.lists = 0;
            items = std::move(LINE.line);
            LINE.line.clear();
        }
        counted = true;
        auto flights = static_cast<usize>(std::count_if(items.begin(), items.end(), [](const LateItem& i) { return i.kind == LateItem::Run; }));
        if (flights > 0) {
            submitting(2, flights);
            if (HRESULT hr = device_->wait_for(target); FAILED(hr)) {
                log::warn("victory_gpu::host", "submissions sent ahead not waited for", {{"error", log::hresult(hr)}});
            }
            if (FAILED(device_->device()->GetDeviceRemovedReason())) {
                fail("host GPU device removed after submissions sent ahead");
            }
            submitting(3, flights);
            gpu::CardHost::Seen seen;
            std::vector<shadow::Declared> back;
            std::unordered_map<const shadow::Shadow*, usize> index;
            std::vector<std::unordered_set<std::uint64_t>> seen_ranges;
            for (const LateItem& i : items) {
                if (i.kind != LateItem::Run) {
                    continue;
                }
                if (i.flight->card) {
                    card_->card_back(*i.flight->card, &seen);
                }
                for (const auto& [s, ranges] : i.flight->back) {
                    auto [it, fresh] = index.try_emplace(s, back.size());
                    if (fresh) {
                        back.emplace_back(s, std::vector<shadow::Range>{});
                        seen_ranges.emplace_back();
                    }
                    for (const auto& r : ranges) {
                        if (seen_ranges[it->second].insert(static_cast<std::uint64_t>(r.first) * 0x9e3779b97f4a7c15ull ^ r.second).second) {
                            back[it->second].second.push_back(r);
                        }
                    }
                }
            }
            shadow::hand_back(back);
        }

        std::uint64_t last = 0;
        std::vector<Taken> landed;
        for (LateItem& i : items) {
            switch (i.kind) {
                case LateItem::Run:
                    last = i.flight->number;
                    for (usize k : i.flight->lists.order) {
                        i.flight->lists.lists[k].host->with_deferred([](Deferred& d) {
                            read_backs(d);
                            return 0;
                        });
                    }
                    if (i.flight->card) {
                        card_->card_done(std::move(*i.flight->card));
                    }
                    landed.push_back(std::move(i.flight->lists));
                    break;
                case LateItem::Write: i.write.what.write(reinterpret_cast<std::uint8_t*>(i.write.at), i.write.bytes); break;
                case LateItem::Signal: fence_set(i.fence, i.value); break;
            }
        }
        if (last > 0) {
            FINISHED.store(last, std::memory_order_release);
            bury(last);
        }
        std::lock_guard lock(LINE_LOCK);
        for (Taken& t : landed) {
            LINE.landed.push_back(std::move(t));
        }
    }
    if (counted) {
        submitting(0, 0);
    }
    if (counted && why != WHY_CARD_DONE) {

        {
            std::lock_guard lock(LINE_LOCK);
            LINE.poke = true;
        }
        LINE_MOVED.notify_one();
    }
}

void Host::relay_land(std::vector<LateItem> items) {
    std::vector<const Flight*> flights;
    for (const LateItem& i : items) {
        if (i.kind == LateItem::Run) {
            flights.push_back(i.flight.get());
        }
    }
    if (!flights.empty()) {
        if (FAILED(device_->device()->GetDeviceRemovedReason())) {
            fail("host GPU device removed after submissions sent ahead (the relay)");
        }
        std::vector<const gpu::CardPlan*> plans;
        for (const Flight* f : flights) {
            if (f->card) {
                plans.push_back(&*f->card);
            }
        }
        card_->relay_card_back(plans);
        std::vector<shadow::Declared> back;
        std::unordered_map<const shadow::Shadow*, usize> index;
        std::vector<std::unordered_set<std::uint64_t>> seen;
        for (const Flight* f : flights) {
            for (const auto& [s, ranges] : f->back) {
                auto [it, fresh] = index.try_emplace(s, back.size());
                if (fresh) {
                    back.emplace_back(s, std::vector<shadow::Range>{});
                    seen.emplace_back();
                }
                for (const auto& r : ranges) {
                    if (seen[it->second].insert(static_cast<std::uint64_t>(r.first) * 0x9e3779b97f4a7c15ull ^ r.second).second) {
                        back[it->second].second.push_back(r);
                    }
                }
            }
        }
        shadow::relay_hand_back(back);


        for (const Flight* f : flights) {
            for (const auto& [s, ranges] : f->back) {
                s->copy_back_done(ranges);
            }
        }
    }

    std::uint64_t last = 0;
    std::vector<Taken> landed;
    for (LateItem& i : items) {
        switch (i.kind) {
            case LateItem::Run:
                last = i.flight->number;
                for (usize k : i.flight->lists.order) {
                    i.flight->lists.lists[k].host->with_deferred([](Deferred& d) {
                        read_backs(d);
                        return 0;
                    });
                }
                if (i.flight->card) {
                    card_->card_done(std::move(*i.flight->card));
                }
                landed.push_back(std::move(i.flight->lists));
                break;
            case LateItem::Write: i.write.what.write(reinterpret_cast<std::uint8_t*>(i.write.at), i.write.bytes); break;
            case LateItem::Signal: fence_set(i.fence, i.value); break;
        }
    }
    if (last > 0) {
        FINISHED.store(last, std::memory_order_release);
        bury(last);
    }
    {
        std::lock_guard lock(LINE_LOCK);
        for (Taken& t : landed) {
            LINE.landed.push_back(std::move(t));
        }
        LINE.in_hand.clear();

        LINE.written.clear();
        LINE.lists = 0;
        std::vector<shadow::Range> ranges;
        for (const LateItem& i : LINE.line) {
            if (i.kind == LateItem::Run) {
                LINE.lists += std::max<usize>(i.flight->lists.order.size(), 1);
                auto r = ranges_of(i.flight->back);
                ranges.insert(ranges.end(), r.begin(), r.end());
            }
        }
        join_ranges(LINE.written, ranges);
        LINE.settling = false;


        LINE.poke = true;
    }
    LINE_MOVED.notify_all();
    LANDINGS.fetch_add(1, std::memory_order_release);
    LINE_LANDED.notify_all();
}

namespace {


constexpr GUID PIPELINE_NAME = {0x6b0f2c1d, 0x93a4, 0x4e57, {0x8c, 0x21, 0x5d, 0x7e, 0x40, 0x19, 0xa3, 0x6f}};


std::string pipelines_of(std::span<HostList* const> runs) {
    std::set<std::string> names;
    for (HostList* list : runs) {
        list->with_deferred([&](Deferred& d) {
            for (const ComPtr<IUnknown>& k : d.kept) {
                ComPtr<ID3D12PipelineState> pso;
                UINT size = 0;
                if (k && SUCCEEDED(k.As(&pso)) && SUCCEEDED(pso->GetPrivateData(PIPELINE_NAME, &size, nullptr)) && size > 0) {
                    std::string name(size, '\0');
                    if (SUCCEEDED(pso->GetPrivateData(PIPELINE_NAME, &size, name.data()))) {
                        names.insert(std::move(name));
                    }
                }
            }
            return 0;
        });
    }
    std::string out = "{";
    for (const std::string& n : names) {
        out += (out.size() > 1 ? ", " : "") + log::debug(n);
    }
    return out + "}";
}

}   

void name_pipeline(ID3D12PipelineState* pso, const std::string& name) {
    if (pso && !name.empty()) {
        pso->SetPrivateData(PIPELINE_NAME, static_cast<UINT>(name.size()), name.data());
    }
}

std::optional<Sent> Host::submit(std::span<HostList* const> runs, bool late_submit) {
    submitting(1, runs.size());


    if (!late_submit) {
        settle(WHY_SYNC);
    } else if (auto why = late_blocked(runs)) {
        if (relay_exact() && *why == WHY_READ) {


            relay_wait([&](const Line& l) { return !blocked_by(l.written, runs, false); });
        } else {
            settle(*why);
        }
    }
    bool behind = late_submit && flying();


    static std::atomic<std::uint64_t> SWEPT{0};
    if (!behind) {
        sweep();
        SWEPT.store(now_ms(), std::memory_order_relaxed);
    } else if (late_free_present() && now_ms() > SWEPT.load(std::memory_order_relaxed) + 1500) {
        settle(WHY_SYNC);
        sweep();
        SWEPT.store(now_ms(), std::memory_order_relaxed);
    }


    make_waiting();
    std::uint64_t this_submission = STARTED.fetch_add(1, std::memory_order_acq_rel) + 1;
    shadow::preparing(this_submission);
    std::vector<ID3D12CommandList*> lists;
    lists.reserve(runs.size() + 2);
    Batch batch;


    if (watch::once()) {
        std::unordered_map<const shadow::Shadow*, watch::Range> spans;
        for (HostList* list : runs) {
            list->with_deferred([&](Deferred& d) {
                for (const Step& s : d.steps) {
                    if (s.kind == Step::Refresh && !s.shadow->unwatched() && !s.shadow->is_view_guarded()) {
                        auto [it, fresh] = spans.try_emplace(s.shadow.get(), watch::Range{s.dst, s.dst + s.len});
                        if (!fresh) {
                            it->second = {std::min(it->second.first, s.dst), std::max(it->second.second, s.dst + s.len)};
                        }
                    }
                }
                return 0;
            });
        }
        std::vector<watch::Range> ranges;
        ranges.reserve(spans.size());
        for (const auto& [_, r] : spans) {
            ranges.push_back(r);
        }
        watch::ask_once(ranges);


        if (behind && stale_under_flights(runs)) {
            settle(WHY_STALE);
            watch::ask_once(ranges);
        }
    } else if (behind && stale_under_flights(runs)) {
        settle(WHY_STALE);
    }
    for (HostList* list : runs) {
        list->with_deferred([&](Deferred& d) {
            run_deferred(d, batch);
            return 0;
        });
        if (ID3D12CommandList* prelude = list->prelude()) {
            lists.push_back(prelude);
        }
        lists.push_back(list->list());
    }
    batch.flush();
    watch::ask_once_end();
    shadow::take_snapshots();

    if (late_submit && gpu::relay() && card_->card_results_low()) {
        settle(WHY_MOST);
    }
    std::vector<gpu::GpuWrite> writes;
    for (HostList* list : runs) {
        list->with_deferred([&](Deferred& d) {
            for (const Step& s : d.steps) {
                if (s.kind == Step::GpuWrite) {
                    writes.push_back({s.shadow.get(), s.dst, s.len});
                }
            }
            return 0;
        });
    }
    auto plan = card_->card_before(writes, late_submit);
    if (plan && plan->check) {
        lists.push_back(plan->check.Get());
    }
    if (plan && plan->download) {
        lists.push_back(plan->download.Get());
    }
    auto prepared = std::chrono::steady_clock::now();
    if (late_submit) {
        device_->queue()->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        Sent sent;
        sent.number = this_submission;
        sent.fence = device_->signal();
        sent.card = std::move(plan);
        sent.back = declared_writes(runs, sent.held);
        for (HostList* list : runs) {
            list->with_deferred([&](Deferred& d) {
                sent.writes.insert(sent.writes.end(), d.writes.begin(), d.writes.end());
                return 0;
            });
        }
        submitting(0, 0);
        return sent;
    }
    submitting(2, runs.size());
    device_->queue()->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
    if (HRESULT hr = device_->wait_for(device_->signal()); FAILED(hr)) {
        log::warn("victory_gpu::host", "host submission not waited for", {{"error", log::hresult(hr)}});
    }
    FINISHED.store(this_submission, std::memory_order_release);
    bury(this_submission);

    if (auto gpu = std::chrono::steady_clock::now() - prepared; gpu > std::chrono::milliseconds(250)) {
        log::warn("victory_gpu::host", "slow host submission",
                  {{"ms", log::debug(static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(gpu).count()))},
                   {"lists", log::debug(static_cast<std::uint64_t>(runs.size()))},
                   {"pipelines", pipelines_of(runs)}});
    }
    if (FAILED(device_->device()->GetDeviceRemovedReason())) {
        fail("host GPU device removed after a submission");
    }
    submitting(3, runs.size());


    if (plan) {
        card_->card_back(*plan);
    }
    std::vector<std::shared_ptr<shadow::Shadow>> held;
    auto back = declared_writes(runs, held);
    shadow::hand_back(back);
    if (plan) {
        card_->card_done(std::move(*plan));
    }
    for (HostList* list : runs) {
        list->with_deferred([&](Deferred& d) {


            read_backs(d);
            for (const Deferred::Write& w : d.writes) {
                w.what.write(reinterpret_cast<std::uint8_t*>(w.at), w.bytes);
            }
            return 0;
        });
    }

    if (gpu::relay()) {
        shadow::relay_fetched(gpu::queue_order());
    }
    submitting(0, 0);
    return std::nullopt;
}


std::uint64_t now_ms() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}


const std::uint64_t START_TICK = GetTickCount64();
std::uint64_t moment() { return (frame_current() << 32) | (((GetTickCount64() - START_TICK) / 100) & 0xffffffffu); }

bool long_ago(std::uint64_t m, std::uint64_t ms, std::uint64_t frames) {
    std::uint64_t now = moment();
    std::uint64_t a = now & 0xffffffffu, b = m & 0xffffffffu;
    std::uint64_t tenths = a > b ? a - b : 0;
    std::uint64_t fa = now >> 32, fb = m >> 32;
    return (fa > fb ? fa - fb : 0) >= frames && tenths * 100 >= ms;
}

std::uint64_t ram_idle_ms() {
    constexpr std::uint64_t GIB = std::uint64_t{1} << 30;
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) {
        return 10'000;
    }
    return m.ullAvailPhys < 2 * GIB ? 2'000 : m.ullAvailPhys < 4 * GIB ? 5'000 : 10'000;
}

D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource, std::uint32_t before, std::uint32_t after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
    b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(after);
    return b;
}

namespace {


std::vector<std::uint8_t> compile(const char* source, const char* entry, const char* target, HRESULT* error) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(source, std::strlen(source), "victory", nullptr, nullptr, entry, target, 0, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        if (FAILED(hr) && errors) {
            log::warn("victory_gpu::host", "a shader of ours: " +
                                               std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
        }
        *error = FAILED(hr) ? hr : S_OK;
        return {};
    }
    auto* p = static_cast<const std::uint8_t*>(code->GetBufferPointer());
    return {p, p + code->GetBufferSize()};
}


const char* PLANE_CORNERS = R"HLSL(
float4 corner(uint i : SV_VertexID) : SV_Position {
    return float4(float(i & 1) * 4.0 - 1.0, float(i >> 1) * 4.0 - 1.0, 0.0, 1.0);
}
)HLSL";
const char* PLANE_STEPS = R"HLSL(
#define RS "DescriptorTable(SRV(t0), visibility = SHADER_VISIBILITY_PIXEL)"
Texture2DArray<uint> twin : register(t0);
[RootSignature(RS)]
float plane(float4 p : SV_Position) : SV_Depth {
    return float(twin.Load(int4(int2(p.xy), 0, 0))) * asfloat(0x37800080);
}
)HLSL";
const char* PLANE_FLOATS = R"HLSL(
#define RS "DescriptorTable(SRV(t0), visibility = SHADER_VISIBILITY_PIXEL)"
Texture2DArray<float> twin : register(t0);
[RootSignature(RS)]
float plane(float4 p : SV_Position) : SV_Depth {
    return twin.Load(int4(int2(p.xy), 0, 0));
}
)HLSL";
constexpr std::uint32_t PLANE_VIEWS = 256;

struct PlanePass {
    ComPtr<ID3D12DescriptorHeap> views;
    std::uint64_t cpu = 0, gpu = 0;
    UINT step = 0;
    std::atomic<std::uint32_t> next{0};
    struct Pipeline {
        bool steps;
        DXGI_FORMAT format;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pipeline;
    };
    std::mutex made_lock;
    std::vector<Pipeline> made;

    const Pipeline* pipeline(bool steps, DXGI_FORMAT format) {
        std::lock_guard lock(made_lock);
        for (const Pipeline& p : made) {
            if (p.steps == steps && p.format == format) {
                return p.pipeline ? &p : nullptr;
            }
        }
        Pipeline p{steps, format, nullptr, nullptr};
        HRESULT error = E_FAIL;
        auto vs = compile(PLANE_CORNERS, "corner", "vs_5_1", &error);
        auto ps = vs.empty() ? std::vector<std::uint8_t>{} : compile(steps ? PLANE_STEPS : PLANE_FLOATS, "plane", "ps_5_1", &error);
        Host* h = host();
        if (h && !vs.empty() && !ps.empty() &&
            SUCCEEDED(error = h->device().device()->CreateRootSignature(0, ps.data(), ps.size(), IID_PPV_ARGS(&p.root)))) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.pRootSignature = p.root.Get();
            d.VS = {vs.data(), vs.size()};
            d.PS = {ps.data(), ps.size()};
            d.SampleMask = UINT_MAX;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            d.RasterizerState.DepthClipEnable = TRUE;
            d.DepthStencilState.DepthEnable = TRUE;
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.DSVFormat = format;
            d.SampleDesc.Count = 1;
            if (FAILED(error = h->device().device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.pipeline)))) {
                p.pipeline = nullptr;
            }
        }
        if (h && !p.pipeline) {
            log::warn("victory_gpu::host", "no pipeline to draw a depth plane from its twin: such planes stay as they are",
                      {{"error", log::hresult(error)}, {"steps", log::debug(steps)}, {"format", log::debug(static_cast<int>(format))}});
        }
        made.push_back(p);
        return made.back().pipeline ? &made.back() : nullptr;
    }
};

PlanePass* plane_pass() {
    static PlanePass* const pass = []() -> PlanePass* {
        Host* h = host();
        if (!h) {
            return nullptr;
        }
        auto* p = new PlanePass;
        D3D12_DESCRIPTOR_HEAP_DESC d{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, PLANE_VIEWS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(h->device().device()->CreateDescriptorHeap(&d, IID_PPV_ARGS(&p->views)))) {
            delete p;
            return nullptr;
        }
        p->cpu = p->views->GetCPUDescriptorHandleForHeapStart().ptr;
        p->gpu = p->views->GetGPUDescriptorHandleForHeapStart().ptr;
        p->step = h->device().device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return p;
    }();
    return pass;
}


void record_plane_pass(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* plane) {
    Host* h = host();
    PlanePass* pass = plane_pass();
    if (!h || !pass) {
        return;
    }
    D3D12_RESOURCE_DESC of = depth->GetDesc(), twin = plane->GetDesc();
    bool steps = twin.Format == 53;
    auto format = static_cast<DXGI_FORMAT>(of.Format == 44 ? 45 : of.Format == 19 ? 20 : of.Format);
    const PlanePass::Pipeline* p = pass->pipeline(steps, format);
    if (!p) {
        return;
    }
    for (UINT slice = 0; slice < of.DepthOrArraySize; ++slice) {
        for (UINT mip = 0; mip < of.MipLevels; ++mip) {
            std::uint32_t slot = pass->next.fetch_add(1, std::memory_order_relaxed) % PLANE_VIEWS;
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = static_cast<DXGI_FORMAT>(steps ? 57 : 41);
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2DArray = {mip, 1, slice, 1, 0, 0.0f};
            D3D12_DEPTH_STENCIL_VIEW_DESC target{};
            target.Format = format;
            target.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            target.Texture2DArray = {mip, slice, 1};
            auto dsv = h->dsv(depth, &target);
            UINT width = static_cast<UINT>(std::max<UINT64>(of.Width >> mip, 1)), height = std::max<UINT>(of.Height >> mip, 1);
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{static_cast<SIZE_T>(pass->cpu + std::uint64_t{slot} * pass->step)};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{pass->gpu + std::uint64_t{slot} * pass->step};
            h->device().device()->CreateShaderResourceView(plane, &view, cpu);
            ID3D12DescriptorHeap* heaps[] = {pass->views.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetGraphicsRootSignature(p->root.Get());
            list->SetPipelineState(p->pipeline.Get());
            list->SetGraphicsRootDescriptorTable(0, gpu);
            list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            D3D12_VIEWPORT vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
            list->RSSetViewports(1, &vp);
            D3D12_RECT sc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            list->RSSetScissorRects(1, &sc);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        }
    }
}


void record_twin_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, const Twin& twin, bool colour) {
    ID3D12Resource* from = colour ? depth : twin.colour->texture.Get();
    ID3D12Resource* to = colour ? twin.colour->texture.Get() : depth;
    if (!twin.through) {
        list->CopyResource(to, from);
        return;
    }
    Host* h = host();
    if (!h) {
        return;
    }
    D3D12_RESOURCE_DESC of = depth->GetDesc();
    UINT planes = UINT{of.MipLevels} * of.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> rows(planes);
    h->device().device()->GetCopyableFootprints(&of, planes, planes, 0, rows.data(), nullptr, nullptr, nullptr);
    UINT first_from = colour ? planes : 0, first_to = colour ? 0 : planes;
    ID3D12Resource* through = twin.through.Get();
    auto b1 = transition_barrier(through, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &b1);
    for (UINT i = 0; i < planes; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dst{through, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = rows[i];
        D3D12_TEXTURE_COPY_LOCATION src{from, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        src.SubresourceIndex = first_from + i;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    auto b2 = transition_barrier(through, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b2);
    for (UINT i = 0; i < planes; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dst{to, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = first_to + i;
        D3D12_TEXTURE_COPY_LOCATION src{through, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = rows[i];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    auto b3 = transition_barrier(through, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &b3);
}

}   


struct HostList::TwinUse {
    std::shared_ptr<HostTexture> depth;
    std::shared_ptr<Twin> twin;

    bool needs[2] = {false, false};


    std::optional<bool> behind[2];

    bool writing = false;


    bool late = false;
};

void HostList::defer_texture(std::shared_ptr<Staging> staging) {
    with_deferred([&](Deferred& d) {
        Step s{Step::Texture};
        s.staging = std::move(staging);
        push_doubling(d.steps, std::move(s));
        return 0;
    });
}

std::optional<HostList::Slice> HostList::room(std::uint64_t size, bool back, std::uint64_t ALIGN) {
    constexpr std::uint64_t CHUNK = 1 << 20, SMALL = CHUNK / 8;
    Host* h = host();
    if (!h) {
        return std::nullopt;
    }
    std::uint64_t aligned = (std::max<std::uint64_t>(size, 1) + ALIGN - 1) & ~(ALIGN - 1);
    if (size <= SMALL) {
        auto shared = with_deferred([&](Deferred& d) -> std::optional<Slice> {
            Deferred::Chunk& c = back ? d.back_chunk : d.up_chunk;
            std::uint64_t at = (c.used + ALIGN - 1) & ~(ALIGN - 1);
            if (c.buffer && at + aligned <= c.size) {
                Slice s{c.buffer, c.data + at, at};
                c.used = at + aligned;
                d.upload_bytes += size;
                return s;
            }
            return std::nullopt;
        });
        if (shared) {
            return shared;
        }
    }
    auto made = back ? h->device().pooled_readback(size <= SMALL ? CHUNK : size) : h->device().pooled_upload(size <= SMALL ? CHUNK : size);
    if (!made) {
        return std::nullopt;
    }
    Slice out{made->get(), made->data(), 0};
    with_deferred([&](Deferred& d) {
        if (size <= SMALL) {
            Deferred::Chunk& c = back ? d.back_chunk : d.up_chunk;
            c = {made->get(), made->data(), aligned, CHUNK};
        }
        d.uploads.push_back(std::move(*made));
        d.upload_bytes += size;
        return 0;
    });
    return out;
}

std::optional<std::pair<ID3D12Resource*, std::uint32_t>> HostList::transition(This resource, std::uint32_t wanted) {
    auto copy = host_copy(resource);
    if (!copy) {
        return std::nullopt;
    }
    return transition(copy, wanted);
}

std::optional<std::pair<ID3D12Resource*, std::uint32_t>> HostList::transition_fast(This resource, std::uint32_t wanted) {
    HostTexture* raw = host_copy_raw(resource);
    if (!raw) {
        return std::nullopt;
    }
    {
        std::lock_guard lock(states_lock_);
        auto it = states_.find(raw);
        if (it != states_.end()) {
            wanted = legal_for(raw->flags, wanted);
            std::uint32_t before = std::exchange(it->second.last, wanted);
            if (before == wanted) {
                return std::nullopt;
            }
            return std::pair{raw->texture.Get(), before};
        }
    }

    return transition(resource, wanted);
}

std::optional<std::pair<ID3D12Resource*, std::uint32_t>> HostList::transition(const std::shared_ptr<HostTexture>& copy, std::uint32_t wanted) {
    ID3D12Resource* texture = copy->texture.Get();
    wanted = legal_for(copy->flags, wanted);
    std::lock_guard lock(states_lock_);
    auto it = states_.find(copy.get());
    if (it == states_.end()) {
        states_.emplace(copy.get(), State{wanted, wanted, copy});
        return std::nullopt;
    }
    std::uint32_t before = std::exchange(it->second.last, wanted);
    if (before == wanted) {
        return std::nullopt;
    }
    return std::pair{texture, before};
}

void HostList::settle(const std::shared_ptr<HostTexture>& copy, std::uint32_t wanted) {
    wanted = legal_for(copy->flags, wanted);
    std::optional<std::uint32_t> before;
    {
        std::lock_guard lock(states_lock_);
        auto [it, fresh] = states_.try_emplace(copy.get(), State{wanted, wanted, copy});
        if (!fresh) {
            std::uint32_t b = std::exchange(it->second.last, wanted);
            if (b != wanted) {
                before = b;
            }
        }
    }
    if (before) {
        auto barrier = transition_barrier(copy->texture.Get(), *before, wanted);
        list_->ResourceBarrier(1, &barrier);
    }
}

bool HostList::twin_use(This resource, bool colour, bool write) {
    constexpr std::uint32_t UNORDERED_ACCESS = 0x8;
    auto t = twin(resource);
    if (!t) {
        return false;
    }
    auto depth = host_copy(resource);
    if (!depth) {
        return false;
    }
    usize side = colour ? 1 : 0;
    bool copy;
    {
        std::lock_guard lock(twins_lock_);
        auto& slot = twins_[t.get()];
        if (!slot) {
            slot = std::make_unique<TwinUse>();
            slot->depth = depth;
            slot->twin = t;
        }
        TwinUse& u = *slot;
        if (u.behind[side]) {
            copy = *u.behind[side];
        } else {
            u.needs[side] = true;
            copy = false;
        }
        u.behind[side] = false;
        if (write && colour) {
            u.writing = true;
        } else if (write) {
            u.behind[1] = true;
        }
        if (copy && !colour && t->plane) {
            u.late = true;
        }
    }
    if (copy) {
        twin_copy(depth, *t, colour);
    } else if (colour) {
        settle(t->colour, UNORDERED_ACCESS);
    }
    if (colour && t->plane) {
        settle(t->plane, UNORDERED_ACCESS);
    }
    return true;
}

void HostList::pass_plane(const std::shared_ptr<HostTexture>& depth, const Twin& twin) {
    constexpr std::uint32_t UNORDERED_ACCESS = 0x8, DEPTH_WRITE = 0x10, PIXEL_SHADER_RESOURCE = 0x80;
    if (!twin.plane) {
        return;
    }
    settle(depth, DEPTH_WRITE);
    settle(twin.plane, PIXEL_SHADER_RESOURCE);
    record_plane_pass(list_.Get(), depth->texture.Get(), twin.plane->texture.Get());
    settle(twin.plane, UNORDERED_ACCESS);
}

void HostList::twin_copy(const std::shared_ptr<HostTexture>& depth, const Twin& twin, bool colour) {
    constexpr std::uint32_t UNORDERED_ACCESS = 0x8, COPY_DEST = 0x400, COPY_SOURCE = 0x800;
    const auto& from = colour ? depth : twin.colour;
    const auto& to = colour ? twin.colour : depth;
    settle(from, COPY_SOURCE);
    settle(to, COPY_DEST);
    record_twin_copy(list_.Get(), depth->texture.Get(), twin, colour);
    settle(twin.colour, UNORDERED_ACCESS);
}

void HostList::twins_to_depth() {
    constexpr std::uint32_t SHADER_RESOURCE = 0x40 | 0x80;
    struct Written {
        std::shared_ptr<HostTexture> depth;
        std::shared_ptr<Twin> twin;
        bool copy;
    };
    std::vector<Written> written;
    {
        std::lock_guard lock(twins_lock_);
        for (auto& [_, u] : twins_) {
            if (u->behind[0] == std::optional<bool>{true} || u->late) {
                bool copy = u->behind[0] == std::optional<bool>{true};
                u->behind[0] = false;
                u->late = false;
                written.push_back({u->depth, u->twin, copy});
            }
        }
    }
    for (const Written& w : written) {
        if (w.copy) {
            twin_copy(w.depth, *w.twin, false);
        }
        pass_plane(w.depth, *w.twin);
        settle(w.depth, SHADER_RESOURCE);
    }
}

void HostList::twins_written() {
    std::lock_guard lock(twins_lock_);
    for (auto& [_, u] : twins_) {
        if (u->writing) {
            u->writing = false;
            u->behind[0] = true;
        }
    }
}

ID3D12CommandList* HostList::prelude() {
    constexpr std::uint32_t COPY_DEST = 0x400, COPY_SOURCE = 0x800;
    std::lock_guard states_lock(states_lock_);


    struct Sync {
        std::shared_ptr<HostTexture> depth;
        std::shared_ptr<Twin> twin;
        bool colour;
    };
    std::vector<Sync> syncs;
    {
        std::lock_guard lock(twins_lock_);
        for (auto& [_, u] : twins_) {
            std::uint32_t real = u->twin->behind.load(std::memory_order_relaxed);
            bool behind[2] = {(real & 1) != 0, (real & 2) != 0};
            for (int side = 0; side < 2; ++side) {
                if (u->needs[side] && behind[side]) {
                    syncs.push_back({u->depth, u->twin, side == 1});
                    behind[0] = behind[1] = false;
                }
            }
            bool left0 = u->behind[0].value_or(behind[0]), left1 = u->behind[1].value_or(behind[1]);
            u->twin->behind.store((left0 ? 1u : 0u) | (left1 ? 2u : 0u), std::memory_order_relaxed);
        }
    }

    std::vector<D3D12_RESOURCE_BARRIER> to_copy, barriers;
    std::unordered_map<const HostTexture*, std::uint32_t> copying;
    for (const Sync& sy : syncs) {
        const auto& from = sy.colour ? sy.depth : sy.twin->colour;
        const auto& to = sy.colour ? sy.twin->colour : sy.depth;
        for (auto [texture, state] : {std::pair{from.get(), COPY_SOURCE}, std::pair{to.get(), COPY_DEST}}) {
            std::uint32_t real = texture->state.load(std::memory_order_relaxed);
            copying[texture] = state;
            if (real != state) {
                to_copy.push_back(transition_barrier(texture->texture.Get(), real, state));
                if (!states_.count(texture)) {
                    barriers.push_back(transition_barrier(texture->texture.Get(), state, real));
                }
            }
        }
    }
    for (auto& [key, st] : states_) {
        std::uint32_t real = st.copy->state.exchange(st.last, std::memory_order_relaxed);
        if (auto it = copying.find(key); it != copying.end()) {
            real = it->second;
        }
        if (real != st.first) {
            barriers.push_back(transition_barrier(st.copy->texture.Get(), real, st.first));
        }
    }
    if (barriers.empty() && syncs.empty()) {
        return nullptr;
    }
    if (!prelude_made_) {
        prelude_made_ = true;
        if (Host* h = host()) {
            prelude_ = h->device().make_prelude();
        }
    }
    if (!prelude_) {
        fail("no host list for a state prelude");
    }
    auto* list = prelude_->list.Get();
    if (FAILED(prelude_->allocator->Reset()) || FAILED(list->Reset(prelude_->allocator.Get(), nullptr))) {
        fail("a host state prelude did not record");
    }
    if (!to_copy.empty()) {
        list->ResourceBarrier(static_cast<UINT>(to_copy.size()), to_copy.data());
    }
    for (const Sync& sy : syncs) {
        record_twin_copy(list, sy.depth->texture.Get(), *sy.twin, sy.colour);
    }
    if (!barriers.empty()) {
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }
    if (FAILED(list->Close())) {
        fail("a host state prelude did not record");
    }
    return list;
}

D3D12_CPU_DESCRIPTOR_HANDLE Host::rtv(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc) {
    std::uint32_t slot = next_rtv_.fetch_add(1, std::memory_order_relaxed) % 1024;
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvs_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * rtv_size_;
    device_->device()->CreateRenderTargetView(resource, desc, h);
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Host::dsv(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc) {
    std::uint32_t slot = next_dsv_.fetch_add(1, std::memory_order_relaxed) % 1024;
    D3D12_CPU_DESCRIPTOR_HANDLE h = dsvs_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * dsv_size_;
    device_->device()->CreateDepthStencilView(resource, desc, h);
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Host::view_of(HostTexture& t, bool depth, const std::array<std::uint32_t, 6>* desc) {
    auto matches = [&](const HostTexture::View& v) { return desc ? !v.none && v.desc == *desc : v.none; };
    auto& count = t.view_count[depth];
    std::uint8_t n = count.load(std::memory_order_acquire);
    for (std::uint8_t i = 0; i < n; ++i) {
        if (matches(t.views[depth][i])) {
            return {static_cast<SIZE_T>(t.views[depth][i].handle)};
        }
    }
    auto make = [&](D3D12_CPU_DESCRIPTOR_HANDLE h) {
        if (depth) {
            device_->device()->CreateDepthStencilView(t.texture.Get(), desc ? reinterpret_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(desc->data()) : nullptr, h);
        } else {
            device_->device()->CreateRenderTargetView(t.texture.Get(), desc ? reinterpret_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(desc->data()) : nullptr, h);
        }
    };
    {
        std::lock_guard lock(t.views_lock);
        n = count.load(std::memory_order_relaxed);
        for (std::uint8_t i = 0; i < n; ++i) {
            if (matches(t.views[depth][i])) {
                return {static_cast<SIZE_T>(t.views[depth][i].handle)};
            }
        }
        if (n < HostTexture::VIEWS) {
            std::uint64_t slot = 0;
            {
                std::lock_guard kl(kept_lock_);
                if (!kept_free_[depth].empty()) {
                    slot = kept_free_[depth].back();
                    kept_free_[depth].pop_back();
                }
            }
            if (slot) {
                make({static_cast<SIZE_T>(slot)});
                HostTexture::View& v = t.views[depth][n];
                v.none = !desc;
                v.desc = desc ? *desc : std::array<std::uint32_t, 6>{};
                v.handle = slot;
                count.store(static_cast<std::uint8_t>(n + 1), std::memory_order_release);
                return {static_cast<SIZE_T>(slot)};
            }
        }
    }

    std::uint32_t slot = (depth ? next_dsv_ : next_rtv_).fetch_add(1, std::memory_order_relaxed) % 1024;
    D3D12_CPU_DESCRIPTOR_HANDLE h = (depth ? dsvs_ : rtvs_)->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * (depth ? dsv_size_ : rtv_size_);
    make(h);
    return h;
}

void Host::free_kept_view(bool depth, std::uint64_t handle) {
    std::lock_guard lock(kept_lock_);
    kept_free_[depth].push_back(handle);
}

Host* host() {
    static Host* const made = []() -> Host* {
        std::unique_ptr<gpu::Device> device;
        try {
            device = gpu::Device::create(gpu::Device::Kind::Hardware, danzi::env::is(L"VICTORY_D3D12_DEBUG", L"1"));
        } catch (const std::exception& e) {
            log::warn("victory_gpu::host", "no host D3D12 device; command lists will not run", {{"error", e.what()}});
            return nullptr;
        }
        return device ? new Host(std::move(device)) : nullptr;
    }();
    return made;
}

void copy_mapped(std::uint8_t* dst, const std::uint8_t* src, usize len, bool title_dst) {
    if (len == 0) {
        return;
    }

    static const bool ask = danzi::env::is(L"VICTORY_COPY_ASK", L"1");
    bool ask_first = title_dst && ask;
    if (ask_first && !committed(dst, len)) {
        return;
    }
    if (title_dst) {

        watch::writing(reinterpret_cast<usize>(dst), len);
    }
    if (danzi::host::read_into(src, dst, len)) {
        return;
    }


    if (title_dst && !ask_first && !committed(dst, len)) {
        return;
    }

    danzi::host::copy_into_host(dst, src, len);
}

std::uint64_t mapped_bytes(const void* at, usize len) {
    if (len == 0) {
        return 0;
    }
    usize start = reinterpret_cast<usize>(at);
    usize end = start + len < start ? SIZE_MAX : start + len;
    if (!committed(at, 1) && !committed(reinterpret_cast<const void*>(end - 1), 1)) {
        return 0;
    }
    static const bool by_query = danzi::env::is(L"VICTORY_COMMITTED_QUERY", L"1");
    if (!by_query) {
        static const bool by_page = danzi::env::is(L"VICTORY_MAPPED_BY_PAGE", L"1");
        if (by_page) {
            auto [there, of] = readable(start, len, false);
            return std::min<std::uint64_t>(std::uint64_t{there} * 4096, len);
        }
        constexpr usize BLOCK = 64 << 10;
        std::uint64_t there = 0;
        for (usize block = start & ~(BLOCK - 1); block < end;) {
            usize lo = std::max(block, start), hi = std::min(block + BLOCK < block ? SIZE_MAX : block + BLOCK, end);
            if (readable(lo, 1, true).first == 1) {
                there += hi - lo;
            }
            if (block + BLOCK < block) {
                break;
            }
            block += BLOCK;
        }
        return there;
    }
    usize p = start;
    std::uint64_t there = 0;
    while (p < end) {
        MEMORY_BASIC_INFORMATION info{};
        SIZE_T n = VirtualQuery(reinterpret_cast<const void*>(p), &info, sizeof info);
        usize next = std::min(reinterpret_cast<usize>(info.BaseAddress) + info.RegionSize, end);
        if (n == 0 || next <= p) {
            break;
        }
        if (info.State == MEM_COMMIT) {
            there += next - p;
        }
        p = next;
    }
    return there;
}

bool committed(const void* at, usize len) {
    static const bool by_query = danzi::env::is(L"VICTORY_COMMITTED_QUERY", L"1");
    if (by_query) {
        return committed_by_query(at, len);
    }
    if (len == 0) {
        return true;
    }
    auto [there, of] = readable(reinterpret_cast<usize>(at), len, true);
    return there == of;
}

void dispose(std::vector<std::shared_ptr<void>> batch) {
    if (batch.empty()) {
        return;
    }
    Disposer* d = disposer();
    if (!d) {
        return;   
    }
    bool was_empty;
    {
        std::lock_guard l(d->lock);
        was_empty = d->queue.empty();
        if (was_empty) {
            d->queue.swap(batch);
        } else {
            for (auto& o : batch) {
                d->queue.push_back(std::move(o));
            }
        }
    }
    if (was_empty) {
        d->wake.notify_one();
    }
}

void settle_disposals() {
    Disposer* d = disposer();
    if (!d) {
        return;
    }
    d->hurry.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock l(d->lock);
        d->idle.wait(l, [&] { return d->queue.empty() && !d->busy; });
    }
    d->hurry.fetch_sub(1, std::memory_order_relaxed);
}

void retire(std::shared_ptr<void> object) {
    std::uint64_t started = STARTED.load(std::memory_order_acquire);
    if (started <= FINISHED.load(std::memory_order_acquire)) {
        RETIREMENTS[0].fetch_add(1, std::memory_order_relaxed);
        dispose_one(std::move(object));
        return;
    }
    RETIREMENTS[1].fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(RETIRED_LOCK);
    RETIRED.emplace_back(started, std::move(object));
}

namespace {
std::atomic<FailureReport> ON_FAILURE{nullptr};
}   

void on_failure(FailureReport report) {
    FailureReport none = nullptr;
    ON_FAILURE.compare_exchange_strong(none, report);
}

void memcopy_line(double frames) { memcopy::log(frames, known_writes(), texture_overlap(), texture_hash_every()); }

std::array<std::uint64_t, 2> take_mesh_groups() {
    return {MESH_GROUPS[0].exchange(0, std::memory_order_relaxed), MESH_GROUPS[1].exchange(0, std::memory_order_relaxed)};
}

bool trail_on() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_TRAIL", L"0");
    return on;
}

void trail_note(std::string what) {
    auto entry = std::make_tuple(now_us(), thread_id(), std::move(what));
    std::lock_guard lock(TRAIL_LOCK);
    if (TRAIL.size() >= TRAIL_MOST) {
        TRAIL.pop_front();
    }
    TRAIL.push_back(std::move(entry));
}

std::uint64_t now_us() {
    static const auto epoch = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - epoch).count());
}

std::uint32_t thread_id() { return GetCurrentThreadId(); }

std::string removal_context() {
    std::uint64_t now = now_us();
    auto ms = [&](std::uint64_t at) { return static_cast<double>(now > at ? now - at : 0) / 1000.0; };
    const char* phase;
    switch (SUBMITTING[0].load(std::memory_order_relaxed)) {
        case 0: phase = "between submissions (nothing of ours was running on the card)"; break;
        case 1: phase = "preparing a submission (nothing of ours was running on the card)"; break;
        case 2: phase = "waiting for the card to run a submission"; break;
        default: phase = "copying back what a submission wrote (nothing of ours was running on the card)"; break;
    }
    char b[160];
    std::snprintf(b, sizeof b, "%llu lists, for %.1f ms; ", static_cast<unsigned long long>(SUBMITTING[1].load(std::memory_order_relaxed)),
                  ms(SUBMITTING[2].load(std::memory_order_relaxed)));
    std::string text = std::string("the submitting thread was ") + phase + ", of " + b + library_removal_note();
    std::snprintf(b, sizeof b, "; objects let go at once %llu and after a running submission %llu; submissions started %llu finished %llu",
                  static_cast<unsigned long long>(RETIREMENTS[0].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(RETIREMENTS[1].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(STARTED.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(FINISHED.load(std::memory_order_relaxed)));
    text += b;
    std::lock_guard lock(TRAIL_LOCK);
    text += "; the last " + std::to_string(TRAIL.size()) + " things done, newest last:";
    for (const auto& [at, thread, what] : TRAIL) {
        std::snprintf(b, sizeof b, "\n  %.1f ms before, thread %u: ", ms(at), static_cast<unsigned>(thread));
        text += b + what;
    }
    return text;
}

void log_removal_context() {
    static std::atomic<bool> said{false};
    if (!trail_on() || said.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    log::error("victory_gpu::host", "when the host device went: " + removal_context());
}

[[noreturn]] void fail(const char* what) {
    std::string text = what;


    if (Host* h = host()) {
        HRESULT removed = h->device().device()->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            text += "; device removed: " + log::hresult(removed);
            log_removal_context();
        }
        if (danzi::env::is(L"VICTORY_D3D12_DEBUG", L"1") || danzi::env::is(L"VICTORY_D3D12_DRED", L"1")) {
            Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT output{};
            if (SUCCEEDED(h->device().device()->QueryInterface(IID_PPV_ARGS(&dred))) &&
                SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&output))) {
                for (auto* node = output.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
                    if (!node->pLastBreadcrumbValue || !node->pCommandHistory) continue;
                    const UINT done = *node->pLastBreadcrumbValue;
                    if (done < node->BreadcrumbCount) {
                        std::fprintf(stderr, "DRED list=%p done=%u/%u next_op=%u\n",
                                     static_cast<void*>(node->pCommandList), done, node->BreadcrumbCount,
                                     static_cast<unsigned>(node->pCommandHistory[done]));
                    }
                }
                std::fflush(stderr);
            }
        }
    }
    log::error("victory_gpu::host", text);
    if (FailureReport report = ON_FAILURE.load()) {
        report(text.c_str());
    }
    std::abort();
}

}   
