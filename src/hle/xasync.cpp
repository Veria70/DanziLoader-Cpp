// SPDX-License-Identifier: GPL-3.0-or-later
// XTaskQueue and XAsync (`victory-hle/src/modules/xasync.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See xasync.hpp.
#include "xasync.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace danzi::hle::xasync {

const Guid PROVIDER{0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

namespace {

constexpr std::uint32_t WORK = 0;
constexpr std::uint32_t COMPLETION = 1;
constexpr std::uint32_t INFINITE_ = 0xFFFFFFFFu;

enum class Mode { Manual, ThreadPool, Serialized, Immediate };

std::optional<Mode> mode_from_raw(std::uint32_t m) {
    switch (m) {
        case 0: return Mode::Manual;
        case 1: return Mode::ThreadPool;
        case 2: return Mode::Serialized;
        case 3: return Mode::Immediate;
        default: return std::nullopt;
    }
}


struct Task {
    void* context;
    Callback callback;
    void run(bool canceled) const { callback(context, canceled); }
};


struct Pool {
    std::mutex lock;
    std::deque<Task> tasks;
    std::size_t idle = 0;
    std::condition_variable ready;

    void submit(Task task) {
        std::unique_lock l(lock);
        tasks.push_back(task);
        if (tasks.size() > idle) {
            std::thread([this] { work(); }).detach();
        } else {
            ready.notify_one();
        }
    }
    void work() {
        std::unique_lock l(lock);
        for (;;) {
            while (tasks.empty()) {
                ++idle;
                ready.wait(l);
                --idle;
            }
            Task t = tasks.front();
            tasks.pop_front();
            l.unlock();
            t.run(false);
            l.lock();
        }
    }
};

Pool& pool() {
    static Pool* p = new Pool();   
    return *p;
}


struct Port {
    std::atomic<long> refs{1};
    Mode mode;
    std::mutex lock;
    std::deque<Task> tasks;
    std::condition_variable ready;

    explicit Port(Mode m) : mode(m) {}

    void retain() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    static Port* create(Mode m) {
        auto* port = new Port(m);
        if (m == Mode::Serialized) {


            port->retain();
            std::thread([port] {
                for (;;) {
                    if (auto t = port->take(1000, [] { return false; })) {
                        t->run(false);
                    } else if (port->refs.load(std::memory_order_acquire) == 1) {
                        break;
                    }
                }
                port->release();
            }).detach();
        }
        return port;
    }

    void submit(Task task) {
        switch (mode) {
            case Mode::Immediate: task.run(false); break;
            case Mode::ThreadPool: pool().submit(task); break;
            default: {
                {
                    std::lock_guard l(lock);
                    tasks.push_back(task);
                }
                ready.notify_one();
            }
        }
    }


    template <class Ended>
    std::optional<Task> take(std::uint32_t timeout_ms, Ended ended) {
        std::unique_lock l(lock);
        auto waiting = [&] { return !(tasks.empty() && !ended()); };
        if (timeout_ms == INFINITE_) {
            ready.wait(l, waiting);
        } else {
            ready.wait_for(l, std::chrono::milliseconds(timeout_ms), waiting);
        }
        if (tasks.empty()) {
            return std::nullopt;
        }
        Task t = tasks.front();
        tasks.pop_front();
        return t;
    }
};

}   

struct Queue {
    std::atomic<long> refs{1};
    Port* ports[2];
    std::mutex monitors_lock;
    struct Monitor {
        std::uint64_t id;
        void* context;
        MonitorCallback callback;
    };
    std::vector<Monitor> monitors;
    std::atomic<bool> terminated{false};

    Queue(Port* work, Port* completion) : ports{work, completion} {}
    ~Queue() {
        ports[0]->release();
        ports[1]->release();
    }
    void retain() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
    HRESULT_ submit(std::uint32_t port, std::uint32_t delay_ms, Task task);
    void deliver(std::uint32_t port, Task task);
};

namespace {


class Ref {
public:
    Ref() = default;
    explicit Ref(Queue* q) : q_(q) {}
    Ref(const Ref& o) : q_(o.q_) {
        if (q_) {
            q_->retain();
        }
    }
    Ref& operator=(Ref o) {
        std::swap(q_, o.q_);
        return *this;
    }
    ~Ref() {
        if (q_) {
            q_->release();
        }
    }
    Queue* get() const { return q_; }
    Queue* operator->() const { return q_; }
    explicit operator bool() const { return q_ != nullptr; }

    Queue* into_raw() {
        return std::exchange(q_, nullptr);
    }

private:
    Queue* q_ = nullptr;
};

}   

HRESULT_ Queue::submit(std::uint32_t port, std::uint32_t delay_ms, Task task) {
    if (port > COMPLETION) {
        return E_INVALIDARG_;
    }
    if (terminated.load(std::memory_order_acquire)) {
        return E_ABORT_;
    }
    if (delay_ms == 0) {
        deliver(port, task);
    } else {
        retain();
        std::thread([this, port, delay_ms, task] {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            deliver(port, task);
            release();
        }).detach();
    }
    return S_OK_;
}

void Queue::deliver(std::uint32_t port, Task task) {
    ports[port]->submit(task);

    std::vector<Monitor> copy;
    {
        std::lock_guard l(monitors_lock);
        copy = monitors;
    }
    for (const auto& m : copy) {
        m.callback(m.context, this, port);
    }
}

namespace {


std::mutex PROCESS_LOCK;
bool PROCESS_SET = false;
Queue* PROCESS_QUEUE = nullptr;   

Ref process_queue() {
    std::lock_guard l(PROCESS_LOCK);
    if (!PROCESS_SET) {
        PROCESS_SET = true;
        PROCESS_QUEUE = new Queue(Port::create(Mode::ThreadPool), Port::create(Mode::ThreadPool));
    }
    if (!PROCESS_QUEUE) {
        return Ref();
    }
    PROCESS_QUEUE->retain();
    return Ref(PROCESS_QUEUE);
}


Ref queue(const Queue* handle) {
    if (!handle) {
        return process_queue();
    }
    auto* q = const_cast<Queue*>(handle);
    q->retain();
    return Ref(q);
}

}   

HRESULT_ XTaskQueueCreate(std::uint32_t work, std::uint32_t completion, const Queue** out) {
    auto w = mode_from_raw(work), c = mode_from_raw(completion);
    if (!w || !c) {
        return E_INVALIDARG_;
    }
    if (!out) {
        return E_INVALIDARG_;
    }
    *out = new Queue(Port::create(*w), Port::create(*c));
    return S_OK_;
}


HRESULT_ XTaskQueueGetPort(const Queue* handle, std::uint32_t port, const void** out) {
    Ref q = queue(handle);
    if (!q) {
        return E_INVALIDARG_;
    }
    if (port > COMPLETION || !out) {
        return E_INVALIDARG_;
    }
    *out = q->ports[port];
    return S_OK_;
}

HRESULT_ XTaskQueueCreateComposite(const void* work, const void* completion, const Queue** out) {
    if (!work || !completion || !out) {
        return E_INVALIDARG_;
    }
    auto* w = const_cast<Port*>(static_cast<const Port*>(work));
    auto* c = const_cast<Port*>(static_cast<const Port*>(completion));
    w->retain();
    c->retain();
    *out = new Queue(w, c);
    return S_OK_;
}

HRESULT_ XTaskQueueDuplicateHandle(const Queue* handle, const Queue** out) {
    Ref q = queue(handle);
    if (q && out) {
        *out = q.into_raw();
        return S_OK_;
    }
    return E_INVALIDARG_;
}

void XTaskQueueCloseHandle(const Queue* handle) {
    if (handle) {
        const_cast<Queue*>(handle)->release();
    }
}

bool XTaskQueueDispatch(const Queue* handle, std::uint32_t port, std::uint32_t timeout_ms) {
    Ref q = queue(handle);
    if (!q || port > COMPLETION) {
        return false;
    }
    Port* p = q->ports[port];
    if (p->mode != Mode::Manual) {
        return false;
    }
    Queue* raw = q.get();
    if (auto t = p->take(timeout_ms, [raw] { return raw->terminated.load(std::memory_order_acquire); })) {
        t->run(false);
        return true;
    }
    return false;
}

HRESULT_ XTaskQueueSubmitDelayedCallback(const Queue* handle, std::uint32_t port, std::uint32_t delay_ms, void* context, Callback callback) {
    Ref q = queue(handle);
    if (!q || !callback) {
        return E_INVALIDARG_;
    }
    return q->submit(port, delay_ms, Task{context, callback});
}


HRESULT_ XTaskQueueTerminate(const Queue* handle, bool, void* context, TerminatedCallback callback) {
    Ref q = queue(handle);
    if (!q) {
        return E_INVALIDARG_;
    }
    q->terminated.store(true, std::memory_order_release);
    for (Port* port : q->ports) {
        std::deque<Task> pending;
        {
            std::lock_guard l(port->lock);
            pending.swap(port->tasks);
        }

        port->ready.notify_all();
        for (const Task& t : pending) {
            t.run(true);
        }
    }
    if (callback) {
        callback(context);
    }
    return S_OK_;
}

namespace {
std::atomic<std::uint64_t> NEXT_TOKEN{1};
}

HRESULT_ XTaskQueueRegisterMonitor(const Queue* handle, void* context, MonitorCallback callback, std::uint64_t* token) {
    Ref q = queue(handle);
    if (!q || !callback) {
        return E_INVALIDARG_;
    }
    if (!token) {
        return E_INVALIDARG_;
    }
    std::uint64_t id = NEXT_TOKEN.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard l(q->monitors_lock);
        q->monitors.push_back({id, context, callback});
    }
    *token = id;
    return S_OK_;
}

void XTaskQueueUnregisterMonitor(const Queue* handle, std::uint64_t token) {
    if (Ref q = queue(handle)) {
        std::lock_guard l(q->monitors_lock);
        std::erase_if(q->monitors, [&](const auto& m) { return m.id == token; });
    }
}

bool XTaskQueueGetCurrentProcessTaskQueue(const Queue** out) {
    Ref found = process_queue();
    if (out) {
        Ref copy = found;
        *out = copy.into_raw();
    }
    return static_cast<bool>(found);
}

void XTaskQueueSetCurrentProcessTaskQueue(const Queue* handle) {
    Ref fresh = handle ? queue(handle) : Ref();
    Queue* old;
    {
        std::lock_guard l(PROCESS_LOCK);
        old = PROCESS_SET ? PROCESS_QUEUE : nullptr;
        PROCESS_SET = true;
        PROCESS_QUEUE = fresh.into_raw();
    }
    if (old) {
        old->release();
    }
}


namespace {

constexpr std::uint32_t OP_BEGIN = 0, OP_DO_WORK = 1, OP_GET_RESULT = 2, OP_CANCEL = 3, OP_CLEANUP = 4;


struct Call {
    Provider provider;
    void* context;
    const void* identity;
    std::mutex lock;
    bool done = false;
    std::condition_variable finished;

    HRESULT_ run(std::uint32_t op, AsyncBlock* block, std::size_t size, void* buffer) {
        ProviderData data{block, size, buffer, context};
        return provider(op, &data);
    }
};

std::mutex CALLS_LOCK;
std::unordered_map<std::uintptr_t, std::shared_ptr<Call>>& calls() {
    static auto* m = new std::unordered_map<std::uintptr_t, std::shared_ptr<Call>>();
    return *m;
}

std::shared_ptr<Call> call(AsyncBlock* block) {
    std::lock_guard l(CALLS_LOCK);
    auto it = calls().find(reinterpret_cast<std::uintptr_t>(block));
    return it == calls().end() ? nullptr : it->second;
}


std::atomic<std::uintptr_t>& status_word(AsyncBlock* block) { return *reinterpret_cast<std::atomic<std::uintptr_t>*>(&block->internal[1]); }
std::atomic<std::uintptr_t>& size_word(AsyncBlock* block) { return *reinterpret_cast<std::atomic<std::uintptr_t>*>(&block->internal[2]); }

HRESULT_ status(AsyncBlock* block) { return static_cast<HRESULT_>(static_cast<std::uint32_t>(status_word(block).load(std::memory_order_acquire))); }

void release(AsyncBlock* block) {

    std::shared_ptr<Call> removed;
    {
        std::lock_guard l(CALLS_LOCK);
        auto it = calls().find(reinterpret_cast<std::uintptr_t>(block));
        if (it != calls().end()) {
            removed = it->second;
            calls().erase(it);
        }
    }
    if (removed) {
        removed->run(OP_CLEANUP, block, 0, nullptr);
    }
}

}   

HRESULT_ XAsyncBegin(AsyncBlock* block, void* context, const void* identity, const char*, Provider provider) {
    if (!provider) {
        return E_INVALIDARG_;
    }
    if (!block || call(block)) {
        return E_INVALIDARG_;
    }
    status_word(block).store(static_cast<std::uint32_t>(E_PENDING_), std::memory_order_release);
    size_word(block).store(0, std::memory_order_release);
    auto c = std::make_shared<Call>();
    c->provider = provider;
    c->context = context;
    c->identity = identity;
    {
        std::lock_guard l(CALLS_LOCK);
        calls()[reinterpret_cast<std::uintptr_t>(block)] = c;
    }

    HRESULT_ hr = c->run(OP_BEGIN, block, 0, nullptr);
    if (hr < 0) {
        XAsyncComplete(block, hr, 0);
    }
    return S_OK_;
}

namespace {
void __stdcall work_task(void* p, bool canceled) {
    auto* block = static_cast<AsyncBlock*>(p);
    auto c = call(block);
    if (!c) {
        return;
    }
    HRESULT_ hr = canceled ? E_ABORT_ : c->run(OP_DO_WORK, block, 0, nullptr);
    if (hr != E_PENDING_) {
        XAsyncComplete(block, hr, 0);
    }
}

void __stdcall complete_task(void* p, bool) {
    auto* block = static_cast<AsyncBlock*>(p);

    if (block->callback) {
        block->callback(block);
    }
}
}   


HRESULT_ XAsyncSchedule(AsyncBlock* block, std::uint32_t delay_ms) {
    if (!block || !call(block)) {
        return E_INVALIDARG_;
    }
    Ref q = queue(block->queue);
    if (!q) {
        return E_INVALIDARG_;
    }
    return q->submit(WORK, delay_ms, Task{block, work_task});
}


void XAsyncComplete(AsyncBlock* block, HRESULT_ result, std::size_t required_size) {
    auto c = call(block);
    if (!c) {
        return;
    }
    if (result == E_PENDING_) {
        return;
    }
    std::unique_lock done(c->lock);
    if (c->done) {
        return;
    }


    const Queue* handle = block->queue;
    CompletionRoutine routine = block->callback;
    if (result < 0 || required_size == 0) {
        release(block);
    }
    size_word(block).store(required_size, std::memory_order_release);
    status_word(block).store(static_cast<std::uint32_t>(result), std::memory_order_release);
    c->done = true;
    c->finished.notify_all();
    done.unlock();
    if (routine) {
        Task task{block, complete_task};
        Ref q = queue(handle);
        if (!q || q->submit(COMPLETION, 0, task) != S_OK_) {
            task.run(true);
        }
    }
}

HRESULT_ XAsyncGetStatus(AsyncBlock* block, bool wait) {
    if (!block) {
        return E_INVALIDARG_;
    }
    if (wait) {
        if (auto c = call(block)) {
            std::unique_lock l(c->lock);
            c->finished.wait(l, [&] { return c->done; });
        }
    }
    return status(block);
}

HRESULT_ XAsyncGetResultSize(AsyncBlock* block, std::size_t* out) {
    if (!block || !out) {
        return E_INVALIDARG_;
    }
    HRESULT_ hr = status(block);

    if (hr >= 0) {
        *out = size_word(block).load(std::memory_order_acquire);
    }
    return hr;
}


void XAsyncCancel(AsyncBlock* block) {
    if (auto c = call(block)) {
        c->run(OP_CANCEL, block, 0, nullptr);
        XAsyncComplete(block, E_ABORT_, 0);
    }
}


HRESULT_ XAsyncGetResult(AsyncBlock* block, const void* identity, std::size_t buffer_size, void* buffer, std::size_t* used) {
    if (!block) {
        return E_INVALIDARG_;
    }
    HRESULT_ hr = status(block);
    std::size_t size = size_word(block).load(std::memory_order_acquire);
    if (hr < 0 || size == 0) {
        if (hr >= 0 && used) {
            *used = 0;
        }
        return hr;
    }
    auto c = call(block);
    if (!c) {
        return E_ILLEGAL_METHOD_CALL_;
    }
    if (c->identity != identity) {
        return E_INVALIDARG_;
    }
    if (buffer_size < size || !buffer) {
        return E_NOT_SUFFICIENT_BUFFER_;
    }
    HRESULT_ r = c->run(OP_GET_RESULT, block, buffer_size, buffer);
    if (used) {
        *used = size;
    }
    release(block);
    return r;
}

HRESULT_ finished(AsyncBlock* block, const void* identity, HRESULT_ hr) {
    return finished_with(block, identity, hr < 0 ? Result(hr) : Result(std::vector<std::uint8_t>{}));
}

namespace {
HRESULT_ __stdcall finished_provider(std::uint32_t op, const ProviderData* data) {
    auto* result = static_cast<Result*>(data->context);
    switch (op) {
        case OP_CLEANUP: delete result; break;
        case OP_BEGIN: {

            HRESULT_ hr = S_OK_;
            std::size_t len = 0;
            if (auto* b = std::get_if<std::vector<std::uint8_t>>(result)) {
                len = b->size();
            } else {
                hr = std::get<HRESULT_>(*result);
            }
            XAsyncComplete(data->block, hr, len);
            break;
        }
        case OP_GET_RESULT:
            if (auto* b = std::get_if<std::vector<std::uint8_t>>(result)) {
                std::memcpy(data->buffer, b->data(), b->size());
            }
            break;
        default: break;
    }
    return S_OK_;
}

struct Pending {
    std::mutex lock;
    std::function<Result()> work;
    std::vector<std::uint8_t> bytes;
};

HRESULT_ __stdcall pending_provider(std::uint32_t op, const ProviderData* data) {
    auto* state = static_cast<Pending*>(data->context);
    switch (op) {
        case OP_CLEANUP: delete state; break;
        case OP_BEGIN: {
            std::function<Result()> work;
            {
                std::lock_guard l(state->lock);
                work = std::move(state->work);
                state->work = nullptr;
            }
            AsyncBlock* block = data->block;
            std::thread([block, state, work = std::move(work)] {
                Result result = work ? work() : Result(E_ABORT_);
                HRESULT_ hr = S_OK_;
                std::size_t len = 0;
                if (auto* b = std::get_if<std::vector<std::uint8_t>>(&result)) {
                    len = b->size();
                    std::lock_guard l(state->lock);
                    state->bytes = std::move(*b);
                } else {
                    hr = std::get<HRESULT_>(result);
                }
                XAsyncComplete(block, hr, len);
            }).detach();
            break;
        }
        case OP_GET_RESULT: {
            std::lock_guard l(state->lock);
            std::memcpy(data->buffer, state->bytes.data(), state->bytes.size());
            break;
        }
        default: break;
    }
    return S_OK_;
}

HRESULT_ __stdcall run_provider(std::uint32_t op, const ProviderData* data) {
    switch (op) {
        case OP_BEGIN: return XAsyncSchedule(data->block, 0);
        case OP_DO_WORK: return reinterpret_cast<Work>(data->context)(data->block);
        default: return S_OK_;
    }
}
}   

HRESULT_ finished_with(AsyncBlock* block, const void* identity, Result result) {
    auto* context = new Result(std::move(result));
    HRESULT_ hr = XAsyncBegin(block, context, identity, nullptr, finished_provider);
    if (hr < 0) {

        delete context;
    }
    return hr;
}

HRESULT_ pending_with(AsyncBlock* block, const void* identity, std::function<Result()> work) {
    auto* state = new Pending();
    state->work = std::move(work);
    HRESULT_ hr = XAsyncBegin(block, state, identity, nullptr, pending_provider);
    if (hr < 0) {
        delete state;
    }
    return hr;
}

HRESULT_ XAsyncRun(AsyncBlock* block, Work work) {
    if (!work) {
        return E_INVALIDARG_;
    }
    const void* identity = reinterpret_cast<const void*>(&XAsyncRun);
    return XAsyncBegin(block, reinterpret_cast<void*>(work), identity, nullptr, run_provider);
}


namespace {
using This = const Object*;
HRESULT_ __stdcall m_get_status(This, AsyncBlock* b, std::uint8_t wait) { return XAsyncGetStatus(b, wait != 0); }
HRESULT_ __stdcall m_get_result_size(This, AsyncBlock* b, std::size_t* out) { return XAsyncGetResultSize(b, out); }
void __stdcall m_cancel(This, AsyncBlock* b) { XAsyncCancel(b); }
HRESULT_ __stdcall m_run(This, AsyncBlock* b, Work w) { return XAsyncRun(b, w); }
HRESULT_ __stdcall m_begin(This, AsyncBlock* b, void* c, const void* id, const char* name, Provider p) { return XAsyncBegin(b, c, id, name, p); }
HRESULT_ __stdcall m_schedule(This, AsyncBlock* b, std::uint32_t delay) { return XAsyncSchedule(b, delay); }
void __stdcall m_complete(This, AsyncBlock* b, HRESULT_ hr, std::size_t size) { XAsyncComplete(b, hr, size); }
HRESULT_ __stdcall m_get_result(This, AsyncBlock* b, const void* id, std::size_t size, void* buffer, std::size_t* used) {
    return XAsyncGetResult(b, id, size, buffer, used);
}
HRESULT_ __stdcall m_create(This, std::uint32_t w, std::uint32_t c, const Queue** out) { return XTaskQueueCreate(w, c, out); }
HRESULT_ __stdcall m_create_composite(This, const void* w, const void* c, const Queue** out) { return XTaskQueueCreateComposite(w, c, out); }
HRESULT_ __stdcall m_get_port(This, const Queue* q, std::uint32_t port, const void** out) { return XTaskQueueGetPort(q, port, out); }
HRESULT_ __stdcall m_duplicate(This, const Queue* q, const Queue** out) { return XTaskQueueDuplicateHandle(q, out); }
bool __stdcall m_dispatch(This, const Queue* q, std::uint32_t port, std::uint32_t timeout) { return XTaskQueueDispatch(q, port, timeout); }
void __stdcall m_close(This, const Queue* q) { XTaskQueueCloseHandle(q); }
HRESULT_ __stdcall m_submit(This, const Queue* q, std::uint32_t port, void* c, Callback cb) { return XTaskQueueSubmitDelayedCallback(q, port, 0, c, cb); }
HRESULT_ __stdcall m_submit_delayed(This, const Queue* q, std::uint32_t port, std::uint32_t delay, void* c, Callback cb) {
    return XTaskQueueSubmitDelayedCallback(q, port, delay, c, cb);
}
HRESULT_ __stdcall m_terminate(This, const Queue* q, std::uint8_t wait, void* c, TerminatedCallback cb) { return XTaskQueueTerminate(q, wait != 0, c, cb); }
HRESULT_ __stdcall m_register_monitor(This, const Queue* q, void* c, MonitorCallback cb, std::uint64_t* token) {
    return XTaskQueueRegisterMonitor(q, c, cb, token);
}
void __stdcall m_unregister_monitor(This, const Queue* q, std::uint64_t token) { XTaskQueueUnregisterMonitor(q, token); }
bool __stdcall m_get_process_queue(This, const Queue** out) { return XTaskQueueGetCurrentProcessTaskQueue(out); }
void __stdcall m_set_process_queue(This, const Queue* q) { XTaskQueueSetCurrentProcessTaskQueue(q); }

void __stdcall m_check_thread(This) {}

#define F(x) reinterpret_cast<const void*>(&x)
const void* const VTABLE[31] = {
    F(xgr::missing<0>), F(xgr::add_ref), F(xgr::add_ref), F(m_get_status), F(m_get_result_size), F(m_cancel), F(m_run), F(m_begin),
    F(xgr::missing<8>), F(m_schedule), F(m_complete), F(m_get_result), F(m_create), F(m_create_composite), F(m_get_port), F(m_duplicate),
    F(m_dispatch), F(m_close), F(m_submit), F(m_submit_delayed), F(xgr::missing<20>), F(xgr::missing<21>), F(m_terminate), F(m_register_monitor),
    F(m_unregister_monitor), F(m_get_process_queue), F(m_set_process_queue), F(xgr::missing<27>), F(xgr::missing<28>), F(m_check_thread),
    F(xgr::missing<30>),
};
#undef F
}   

const Object OBJECT{VTABLE, "task queue"};

std::vector<Interface> interfaces() { return {{PROVIDER, PROVIDER, &OBJECT}}; }

}   
