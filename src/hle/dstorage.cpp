// SPDX-License-Identifier: GPL-3.0-or-later
// The console's DirectStorage (`XCRDAPI.dll`; `victory-hle/src/modules/
// dstorage.rs`; DanziLoader / victory-perf, see NOTICE.md): a factory that
// opens files and creates request queues and status arrays the title polls.
// Its interfaces have grown since 2020 (new interface ids, new slots); the
// slots here are the ones the title calls, read from its call sites.
//
// Submitted requests of every queue run on one pool of workers, as the
// console's hardware runs them: in parallel, started in queue order, queues
// served by weighted round robin over their priorities (about 10x per
// level, counted in bytes), with at most the staging buffer's worth of
// compressed source in flight. Each request reads its source (a file or
// memory), inflates zlib data and/or unpacks BCPack data, and writes the
// destination (an address, or physical pages). Completion is what stays in
// order: a status entry is written once every request enqueued before it on
// its queue has finished, with the first failure (in queue order) since the
// previous status entry, or success.
//
// Log lines, the walls trace (`VICTORY_WALLS_TRACE`) and the debug summary
// of throughput: milestone 7.
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/env.hpp"
#include "core/log.hpp"
#include "formats/bcpack.hpp"
#include "modules.hpp"
#include "shader/native.hpp"
#include "xasync.hpp"

namespace danzi::hle::dstorage {

namespace {


const Guid FACTORY{0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr HRESULT_ E_NOINTERFACE_ = static_cast<HRESULT_>(0x80004002u);
constexpr HRESULT_ E_OUTOFMEMORY_ = static_cast<HRESULT_>(0x8007000Eu);

constexpr HRESULT_ END_OF_FILE = static_cast<HRESULT_>(0x80070026u);

constexpr HRESULT_ INVALID_DATA = static_cast<HRESULT_>(0x8007000Du);

using xgr::add_ref;
using xgr::missing;
#define F(x) reinterpret_cast<const void*>(&x)


template <class T>
struct Instance {
    Object base;
    std::atomic<std::uint32_t> refs;
    T state;
};

template <class T>
void* create(Object base, T state) {
    return new Instance<T>{base, 1, std::move(state)};
}

template <class T>
std::uint32_t __stdcall add_ref_instance(Instance<T>* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <class T>
std::uint32_t __stdcall release(Instance<T>* self) {
    std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        delete self;
    }
    return left;
}

std::string guid_text(const Guid& g) {
    char b[40];
    std::snprintf(b, sizeof b, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.data1, g.data2, g.data3, g.data4[0], g.data4[1], g.data4[2], g.data4[3], g.data4[4],
                  g.data4[5], g.data4[6], g.data4[7]);
    return b;
}
std::string iid_text(const Guid* iid) { return iid ? guid_text(*iid) : "null"; }
std::string os_error(DWORD e) { return std::system_category().message(static_cast<int>(e)) + " (os error " + std::to_string(e) + ")"; }


HRESULT_ from_win32(DWORD error) { return static_cast<HRESULT_>(0x80070000u | (error & 0xffff)); }


struct Request {
    std::uint64_t options;

    void* destination;


    std::uint64_t page_offset;
    std::uint32_t destination_size;

    const void* source;
    std::uint64_t offset;
    std::uint32_t source_size;

    std::uint32_t intermediate_size;

    std::uint64_t tag;
    const char* name;
};
static_assert(sizeof(Request) == 0x48);


constexpr std::uint64_t ZLIB = 1;

constexpr std::uint64_t BCPACK = 0xf << 1;
constexpr std::uint64_t PAGE_DESTINATION = 1 << 21;
constexpr std::uint64_t MEMORY_SOURCE = 1 << 22;


constexpr std::size_t PAGE = 64 * 1024;


struct File {
    HANDLE handle;
    explicit File(HANDLE h) : handle(h) {}
    File(File&& o) noexcept : handle(std::exchange(o.handle, INVALID_HANDLE_VALUE)) {}
    ~File() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

struct StatusArray {
    std::unique_ptr<std::atomic<HRESULT_>[]> entries;
    std::uint32_t count;
};
using Array = Instance<StatusArray>;


HRESULT_ __stdcall status_result(Array* self, std::uint32_t index) {
    if (index >= self->state.count) {
        return E_INVALIDARG_;
    }
    return self->state.entries[index].load(std::memory_order_acquire);
}


bool __stdcall is_complete(Array* self, std::uint32_t index) { return status_result(self, index) != E_PENDING_; }

const void* const FILE_VTABLE[8] = {F(missing<0>), F(add_ref_instance<File>), F(release<File>), F(missing<3>), F(missing<4>), F(missing<5>), F(missing<6>), F(missing<7>)};
const void* const STATUS_VTABLE[5] = {F(missing<0>), F(add_ref_instance<StatusArray>), F(release<StatusArray>), F(is_complete), F(status_result)};


struct Status {
    Array* array = nullptr;
    std::uint32_t index = 0;
    Status(Array* a, std::uint32_t i) : array(a), index(i) {}
    Status(Status&& o) noexcept : array(std::exchange(o.array, nullptr)), index(o.index) {}
    Status& operator=(Status&& o) noexcept {
        if (this != &o) {
            drop();
            array = std::exchange(o.array, nullptr);
            index = o.index;
        }
        return *this;
    }
    ~Status() { drop(); }
    void drop() {
        if (array) {
            release(array);
            array = nullptr;
        }
    }
    std::atomic<HRESULT_>& entry() const { return array->state.entries[index]; }
};

using Command = std::variant<Request, Status>;


constexpr std::uint64_t COST[3] = {100, 10, 1};

constexpr std::uint64_t STAGING = 32 << 20;


struct Lane {
    std::size_t klass;
    std::size_t capacity;

    std::uint64_t clock = 0;

    std::vector<Command> unsubmitted;


    std::deque<std::pair<std::uint64_t, Request>> reads;
    std::deque<std::pair<std::uint64_t, Status>> statuses;
    std::set<std::uint64_t> running;
    std::map<std::uint64_t, HRESULT_> failures;
    bool closed = false;

    Lane(std::int8_t priority, std::uint16_t cap)
        : klass(static_cast<std::size_t>(static_cast<std::int8_t>(std::clamp<std::int8_t>(priority, -1, 1) + 1))), capacity(cap) {}


    std::size_t entries() const { return unsubmitted.size() + reads.size() + statuses.size() + running.size(); }


    void flush() {
        while (!statuses.empty()) {
            std::uint64_t seq = statuses.front().first;
            if ((!reads.empty() && reads.front().first < seq) || (!running.empty() && *running.begin() < seq)) {
                return;
            }
            Status status = std::move(statuses.front().second);
            statuses.pop_front();

            std::optional<HRESULT_> first;
            auto later = failures.lower_bound(seq);
            if (failures.begin() != later) {
                first = failures.begin()->second;
            }
            failures.erase(failures.begin(), later);
            status.entry().store(first.value_or(S_OK_), std::memory_order_release);
        }
    }
};


struct Job {
    std::uint64_t lane, seq;
    Request request;
    std::uint64_t staging;
};


std::uint64_t staging_bytes(const Request& r) {
    bool buffered = (r.options & (ZLIB | BCPACK | PAGE_DESTINATION)) != 0;
    return buffered && !(r.options & MEMORY_SOURCE) ? r.source_size : 0;
}

struct Lanes {
    std::unordered_map<std::uint64_t, Lane> lanes;
    std::uint64_t next_id = 0;

    std::uint64_t next_seq = 0;

    std::uint64_t staged = 0, staging = STAGING;

    bool starved = false;


    std::uint64_t clock[3] = {0, 0, 0};
    std::uint64_t now = 0;
    std::uint64_t lane_now[3] = {0, 0, 0};


    std::optional<Job> start() {
        std::optional<std::pair<std::uint64_t, std::size_t>> best_class;
        for (auto& [id, l] : lanes) {
            if (!l.reads.empty()) {
                std::pair<std::uint64_t, std::size_t> key{clock[l.klass], l.klass};
                if (!best_class || key < *best_class) {
                    best_class = key;
                }
            }
        }
        if (!best_class) {
            return std::nullopt;
        }
        std::size_t klass = best_class->second;
        std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> best;
        for (auto& [id, l] : lanes) {
            if (!l.reads.empty() && l.klass == klass) {
                std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> key{l.clock, l.reads.front().first, id};
                if (!best || key < *best) {
                    best = key;
                }
            }
        }
        if (!best) {
            return std::nullopt;
        }
        std::uint64_t id = std::get<2>(*best);
        Lane& lane = lanes.at(id);
        auto [seq, request] = lane.reads.front();
        std::uint64_t staging_ = staging_bytes(request);
        if (staged > 0 && staged + staging_ > staging) {
            starved = true;
            return std::nullopt;
        }
        lane.reads.pop_front();
        lane.running.insert(seq);
        staged += staging_;
        std::uint64_t size = std::max<std::uint64_t>(std::max(request.destination_size, request.source_size), 1);
        now = clock[klass];
        clock[klass] += size * COST[klass];
        lane_now[klass] = lane.clock;
        lane.clock += size;
        return Job{id, seq, request, staging_};
    }

    void finish(const Job& job, std::optional<HRESULT_> failure) {
        staged -= job.staging;
        if (auto it = lanes.find(job.lane); it != lanes.end()) {
            it->second.running.erase(job.seq);
            if (failure) {
                it->second.failures.insert_or_assign(job.seq, *failure);
            }
            it->second.flush();
        }
    }


    void submit(std::uint64_t id) {
        auto it = lanes.find(id);
        if (it == lanes.end()) {
            return;
        }
        std::size_t klass = it->second.klass;


        bool waiting = false;
        for (auto& [i, l] : lanes) {
            waiting = waiting || (l.klass == klass && !l.reads.empty());
        }
        if (!waiting) {
            clock[klass] = std::max(clock[klass], now);
        }
        Lane& lane = it->second;
        if (lane.reads.empty()) {
            lane.clock = std::max(lane.clock, lane_now[klass]);
        }
        auto commands = std::move(lane.unsubmitted);
        lane.unsubmitted.clear();
        for (auto& c : commands) {
            std::uint64_t seq = next_seq++;
            if (auto* r = std::get_if<Request>(&c)) {
                lane.reads.emplace_back(seq, *r);
            } else {
                lane.statuses.emplace_back(seq, std::move(std::get<Status>(c)));
            }
        }
        lane.flush();
    }
};


struct Scheduler {
    std::mutex lock;
    Lanes lanes;

    std::condition_variable work;

    std::condition_variable finished;
};

std::optional<HRESULT_> execute(const Request& request);


[[noreturn]] void worker(Scheduler* s) {
    std::unique_lock l(s->lock);
    for (;;) {
        auto job = s->lanes.start();
        if (!job) {
            s->work.wait(l);
            continue;
        }
        l.unlock();
        auto failure = execute(job->request);
        l.lock();
        s->lanes.finish(*job, failure);
        if (std::exchange(s->lanes.starved, false)) {
            s->work.notify_all();
        }
        s->finished.notify_all();
    }
}


Scheduler* scheduler() {
    static Scheduler* s = [] {
        auto* sch = new Scheduler;


        DWORD_PTR process = 0, system = 0;
        unsigned workers = 4;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
            workers = static_cast<unsigned>(std::popcount(static_cast<std::uint64_t>(process)));
        }
        unsigned started = 0;
        for (unsigned i = 0; i < workers; ++i) {
            try {
                std::thread(worker, sch).detach();
                ++started;
            } catch (...) {
            }
        }
        log::info("victory_hle::modules::dstorage", "dstorage: workers", {{"workers", log::debug(started)}});
        return sch;
    }();
    return s;
}


void close_lane(std::uint64_t id) {
    Scheduler* s = scheduler();
    if (!s) {
        return;
    }
    std::unique_lock l(s->lock);
    auto it = s->lanes.lanes.find(id);
    if (it == s->lanes.lanes.end()) {
        return;
    }
    it->second.closed = true;
    it->second.unsubmitted.clear();
    it->second.reads.clear();
    it->second.statuses.clear();   
    for (;;) {
        auto found = s->lanes.lanes.find(id);
        if (found == s->lanes.lanes.end() || found->second.running.empty()) {
            break;
        }
        s->finished.wait(l);
    }
    s->lanes.lanes.erase(id);
    s->finished.notify_all();
}


struct Queue {
    std::uint64_t id;
    bool owner = true;
    explicit Queue(std::uint64_t i) : id(i) {}
    Queue(Queue&& o) noexcept : id(o.id), owner(std::exchange(o.owner, false)) {}
    ~Queue() {
        if (owner) {
            close_lane(id);
        }
    }
};
using QueueRef = Instance<Queue>;


void queue(QueueRef* self, Command command) {
    std::uint64_t id = self->state.id;
    Scheduler* s = scheduler();
    if (!s) {
        return;
    }
    std::unique_lock l(s->lock);
    for (;;) {
        auto it = s->lanes.lanes.find(id);
        if (it == s->lanes.lanes.end() || it->second.closed) {
            return;   
        }

        if (it->second.entries() + 1 < std::max<std::size_t>(it->second.capacity, 2)) {
            break;
        }
        s->finished.wait(l);
    }
    Lane& lane = s->lanes.lanes.at(id);
    lane.unsubmitted.push_back(std::move(command));
    if (lane.unsubmitted.size() >= std::max<std::size_t>(lane.capacity / 2, 1)) {
        s->lanes.submit(id);
        s->work.notify_all();
    }
}

void __stdcall enqueue_request(QueueRef* self, const Request* request) {
    if (!request) {
        return;
    }
    queue(self, *request);
}

void __stdcall enqueue_status(QueueRef* self, Array* array, std::uint32_t index) {
    if (!array) {
        return;
    }
    if (index >= array->state.count) {
        log::warn("victory_hle::modules::dstorage", "dstorage: status entry out of range", {{"index", log::debug(index)}});
        return;
    }
    array->state.entries[index].store(E_PENDING_, std::memory_order_release);
    add_ref_instance(array);
    queue(self, Status{array, index});
}

void __stdcall submit(QueueRef* self) {
    std::uint64_t id = self->state.id;
    Scheduler* s = scheduler();
    if (!s) {
        return;
    }
    std::lock_guard l(s->lock);
    s->lanes.submit(id);
    s->work.notify_all();
}


void __stdcall close(QueueRef* self) { close_lane(self->state.id); }


const void* const QUEUE_VTABLE[12] = {F(missing<0>),      F(add_ref_instance<Queue>), F(release<Queue>), F(enqueue_request), F(enqueue_status), F(missing<5>),
                                      F(submit),          F(missing<7>),              F(close),          F(missing<9>),      F(missing<10>),    F(missing<11>)};


std::optional<HRESULT_> read_into(const Request& request, std::span<std::uint8_t> out) {
    if (request.options & MEMORY_SOURCE) {
        std::memcpy(out.data(), request.source, out.size());
        return std::nullopt;
    }
    HANDLE file = static_cast<const Instance<File>*>(request.source)->state.handle;
    std::size_t done = 0;
    while (done < out.size()) {


        OVERLAPPED o{};
        std::uint64_t at = request.offset + done;
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 0xFFFFFFFFu)), n = 0;
        if (!ReadFile(file, out.data() + done, want, &n, &o)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF || e == ERROR_BROKEN_PIPE) {
                n = 0;
            } else {
                return from_win32(e);
            }
        }
        if (n == 0) {
            return END_OF_FILE;
        }
        done += n;
    }
    return std::nullopt;
}


std::vector<std::size_t> filled(const std::size_t* array, std::size_t n) {
    auto read = [&] {
        std::vector<std::size_t> v(n);
        for (std::size_t i = 0; i < n; ++i) {
            v[i] = *static_cast<const volatile std::size_t*>(array + i);
        }
        return v;
    };
    auto pages = read();
    auto has_zero = [&] { return std::find(pages.begin(), pages.end(), 0) != pages.end(); };
    if (!has_zero()) {
        return pages;
    }
    ULONGLONG began = GetTickCount64();
    while (has_zero() && GetTickCount64() - began < 100) {
        Sleep(2);
        pages = read();
    }
    return pages;
}

using Fill = std::function<std::variant<std::size_t, HRESULT_>(std::span<std::uint8_t>)>;


std::optional<HRESULT_> into_destination(const Request& request, std::size_t len, const Fill& fill) {
    if (!(request.options & PAGE_DESTINATION)) {


        kernel_will_write(reinterpret_cast<std::uintptr_t>(request.destination), len);
        auto r = fill({static_cast<std::uint8_t*>(request.destination), len});
        if (auto* hr = std::get_if<HRESULT_>(&r)) {
            return *hr;
        }
        return std::nullopt;
    }
    std::size_t offset = static_cast<std::size_t>(request.page_offset & 0xffff);
    auto pages = filled(static_cast<const std::size_t*>(request.destination), (offset + len + PAGE - 1) / PAGE);


    if (auto window = xmem::page_window(pages, offset, len)) {
        auto r = fill(window->bytes());
        if (auto* hr = std::get_if<HRESULT_>(&r)) {
            return *hr;
        }
        return std::nullopt;
    }
    std::vector<std::uint8_t> data(len);
    auto r = fill(data);
    if (auto* hr = std::get_if<HRESULT_>(&r)) {
        return *hr;
    }
    std::size_t n = std::get<std::size_t>(r);


    static const std::uint32_t retries = [] {
        auto v = env::get(L"VICTORY_DSTORAGE_RETRIES");
        if (v && !v->empty() && v->size() <= 10 && std::all_of(v->begin(), v->end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
            std::uint64_t x = std::stoull(*v);
            if (x <= 0xFFFFFFFFu) {
                return static_cast<std::uint32_t>(x);
            }
        }
        return 12u;
    }();
    std::span<const std::uint8_t> bytes(data.data(), n);
    bool written = xmem::write_physical(pages, offset, bytes);
    std::uint32_t tries = 0;
    while (!written && tries < retries) {
        Sleep(static_cast<DWORD>(std::min<std::uint64_t>(std::uint64_t{2} << std::min<std::uint32_t>(tries, 6), 128)));
        ++tries;
        written = xmem::write_physical(pages, offset, bytes);
    }
    constexpr const char* T = "victory_hle::modules::dstorage";
    if (written) {
        if (tries > 0) {
            log::info(T, "dstorage: a destination's physical pages written after a retry",
                      {{"offset", log::debug(static_cast<std::uint64_t>(offset))}, {"len", log::debug(static_cast<std::uint64_t>(len))}, {"tries", log::debug(tries)}});
        }
        return std::nullopt;
    }
    DWORD last = GetLastError();
    bool known = xmem::pages_known(pages, offset, len);
    log::warn(T, "dstorage: retries used up", {{"known", log::debug(known)}, {"last_error", "Some(" + std::to_string(static_cast<int>(last)) + ")"}});


    char o[24], first[24];
    std::snprintf(o, sizeof o, "0x%llx", static_cast<unsigned long long>(request.options));
    std::snprintf(first, sizeof first, "0x%llx", static_cast<unsigned long long>(pages.empty() ? 0 : pages[0]));
    log::warn(T, "dstorage: a destination's physical pages could not be written (unknown page or a host failure); its request fails",
              {{"offset", log::debug(static_cast<std::uint64_t>(offset))}, {"len", log::debug(static_cast<std::uint64_t>(len))},
               {"pages", log::debug(static_cast<std::uint64_t>(pages.size()))}, {"first", first}, {"options", o}});
    return E_INVALIDARG_;
}


const formats::bcpack::Tables* bcpack_tables() {
    static const formats::bcpack::Tables* tables = [] {
        std::string error;
        auto t = formats::bcpack::builtin(&error);
        if (!t) {
            log::warn("victory_hle::modules::dstorage", "dstorage: BCPack requests fail: the built-in BCPack tables: " + error + " (more at debug)");
        }
        return t;
    }();
    return tables;
}


std::optional<HRESULT_> execute(const Request& request) {
    std::uint64_t options = request.options;
    auto fail = [&](HRESULT_ hr, const std::string& why) -> HRESULT_ {
        char o[24], h[16];
        std::snprintf(o, sizeof o, "0x%llx", static_cast<unsigned long long>(options));
        std::snprintf(h, sizeof h, "0x%08x", static_cast<unsigned>(hr));
        log::warn("victory_hle::modules::dstorage", "dstorage: request failed: " + why,
                  {{"options", o}, {"offset", log::debug(request.offset)}, {"source_size", log::debug(request.source_size)},
                   {"destination_size", log::debug(request.destination_size)}, {"intermediate_size", log::debug(request.intermediate_size)},
                   {"tag", log::debug(request.tag)}, {"name", log::debug(request.name ? xgr::text(request.name).value_or("") : std::string())}, {"hr", h}});
        return hr;
    };
    if (options & ~(ZLIB | BCPACK | PAGE_DESTINATION | MEMORY_SOURCE)) {
        return fail(E_NOTIMPL_, "unsupported options");
    }
    if (!request.source || !request.destination) {
        return fail(E_INVALIDARG_, "no source or destination");
    }
    auto bc = static_cast<std::uint8_t>((options & BCPACK) >> 1);
    std::size_t size = request.destination_size;
    if (!(options & (ZLIB | BCPACK))) {
        if (request.source_size > request.destination_size) {
            return fail(E_INVALIDARG_, "source larger than destination");
        }
        std::size_t n = request.source_size;


        bool bounce = !(options & (PAGE_DESTINATION | MEMORY_SOURCE));
        auto copied = into_destination(request, n, [&](std::span<std::uint8_t> out) -> std::variant<std::size_t, HRESULT_> {
            if (!bounce) {
                if (auto hr = read_into(request, out)) {
                    return *hr;
                }
                return n;
            }
            std::vector<std::uint8_t> data(n);
            if (auto hr = read_into(request, data)) {
                return *hr;
            }
            std::memcpy(out.data(), data.data(), n);
            return n;
        });
        if (copied) {
            return fail(*copied, "copying");
        }
        return std::nullopt;
    }
    const formats::bcpack::Tables* tables = nullptr;
    if (bc != 0) {
        tables = bcpack_tables();
        if (!tables) {
            return E_NOTIMPL_;
        }
    }

    std::vector<std::uint8_t> owned;
    std::span<const std::uint8_t> source;
    if (options & MEMORY_SOURCE) {
        source = {static_cast<const std::uint8_t*>(request.source), request.source_size};
    } else {
        owned.resize(request.source_size);
        if (auto hr = read_into(request, owned)) {
            return fail(*hr, "reading the source");
        }
        source = owned;
    }

    std::vector<std::uint8_t> packed;
    if (bc != 0 && (options & ZLIB)) {
        packed.resize(request.intermediate_size);
        auto n = shader::inflate_zlib_into(source, packed);
        if (!n) {
            return fail(INVALID_DATA, "zlib");
        }
        packed.resize(*n);
        source = packed;
    }
    return into_destination(request, size, [&](std::span<std::uint8_t> out) -> std::variant<std::size_t, HRESULT_> {
        if (tables) {
            if (auto why = formats::bcpack::decode(bc, source, out, *tables)) {
                return fail(INVALID_DATA, *why);
            }
            return out.size();
        }
        auto n = shader::inflate_zlib_into(source, out);
        if (!n) {
            return fail(INVALID_DATA, "zlib");
        }
        return *n;
    });
}


using Factory = const Object*;
using Out = void**;


HRESULT_ __stdcall open_file(Factory, const wchar_t* path, const Guid* iid, Out out) {
    constexpr DWORD GENERIC_READ_ = 0x80000000, FILE_SHARE_READ_ = 1, OPEN_EXISTING_ = 3;
    if (!path) {
        return E_INVALIDARG_;
    }

    HANDLE handle = files::CreateFileW(path, GENERIC_READ_, FILE_SHARE_READ_, nullptr, OPEN_EXISTING_, 0, nullptr);
    std::string name = xgr::utf16_lossy(path);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        log::warn("victory_hle::modules::dstorage", "dstorage: opening a file failed", {{"path", log::debug(name)}, {"error", os_error(e)}});
        return from_win32(e);
    }
    log::info("victory_hle::modules::dstorage", "dstorage: file opened", {{"path", log::debug(name)}, {"iid", log::debug(iid_text(iid))}});
    void* file = create(Object{FILE_VTABLE, "dstorage file"}, File{handle});
    return xgr::put(out, file);
}


HRESULT_ __stdcall create_status_array(Factory, std::uint32_t capacity, const char* name, const Guid* iid, Out out) {
    log::info("victory_hle::modules::dstorage", "dstorage: status array",
              {{"capacity", log::debug(capacity)}, {"name", log::debug(name ? xgr::text(name).value_or("") : std::string())}, {"iid", log::debug(iid_text(iid))}});
    StatusArray state{std::make_unique<std::atomic<HRESULT_>[]>(capacity), capacity};
    for (std::uint32_t i = 0; i < capacity; ++i) {
        state.entries[i].store(S_OK_, std::memory_order_relaxed);
    }
    return xgr::put(out, create(Object{STATUS_VTABLE, "dstorage status array"}, std::move(state)));
}


struct QueueDesc {
    std::uint64_t source_type;
    std::uint16_t capacity;
    std::int8_t priority;
    const char* name;
    std::uint64_t unknown;
};

HRESULT_ __stdcall create_queue(Factory, const QueueDesc* desc, const Guid* iid, Out out) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    log::info("victory_hle::modules::dstorage", "dstorage: queue",
              {{"source", log::debug(desc->source_type)}, {"capacity", log::debug(static_cast<std::uint32_t>(desc->capacity))}, {"priority", log::debug(static_cast<int>(desc->priority))},
               {"name", log::debug(desc->name ? xgr::text(desc->name).value_or("") : std::string())}, {"unknown", log::debug(desc->unknown)}, {"iid", log::debug(iid_text(iid))}});
    Scheduler* s = scheduler();
    if (!s) {
        return E_OUTOFMEMORY_;
    }
    std::uint64_t id;
    {
        std::lock_guard l(s->lock);
        id = s->lanes.next_id++;
        s->lanes.lanes.emplace(id, Lane(desc->priority, desc->capacity));
    }
    return xgr::put(out, create(Object{QUEUE_VTABLE, "dstorage queue"}, Queue{id}));
}


HRESULT_ __stdcall set_staging_buffer_size(Factory, std::uint32_t size) {
    log::info("victory_hle::modules::dstorage", "dstorage: staging buffer size", {{"size", log::debug(size)}});
    if (Scheduler* s = scheduler()) {
        std::lock_guard l(s->lock);
        s->lanes.staging = size;
    }
    return S_OK_;
}

const void* const FACTORY_VTABLE[16] = {
    F(missing<0>),  F(add_ref),     F(add_ref),
    F(missing<3>),                
    F(open_file),                 
    F(create_status_array),       
    F(missing<6>),                
    F(missing<7>),                
    F(set_staging_buffer_size),   
    F(missing<9>),
    F(create_queue),              
    F(missing<11>), F(missing<12>), F(missing<13>), F(missing<14>), F(missing<15>),
};
const Object FACTORY_OBJECT{FACTORY_VTABLE, "dstorage factory"};

HRESULT_ WINAPI DStorageGetFactory(const Guid* iid, Out out) {
    if (iid && *iid == FACTORY) {
        return xgr::put(out, const_cast<void*>(static_cast<const void*>(&FACTORY_OBJECT)));
    }
    log::warn("victory_hle::modules::dstorage", "DStorageGetFactory: unknown interface", {{"iid", iid ? "Some(" + log::debug(guid_text(*iid)) + ")" : std::string("None")}});
    return E_NOINTERFACE_;
}

#undef F

}   

std::vector<HleExport> exports() { return {{"DStorageGetFactory", reinterpret_cast<std::uintptr_t>(&DStorageGetFactory)}}; }

}   
