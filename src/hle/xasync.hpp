// SPDX-License-Identifier: GPL-3.0-or-later
// XTaskQueue and XAsync: the runtime's callback queues and async-call
// blocks, served as the task-queue provider's object from `QueryApiImpl`
// (`victory-hle/src/modules/xasync.rs`; DanziLoader / victory-perf, see
// NOTICE.md). A queue has a work port and a completion port, each
// dispatched manually, on a thread pool, serialised on one thread, or
// immediately on the submitting thread. A null queue handle means the
// process queue. An async block's system area holds its status and result
// size, so both stay readable after our state for the call is gone.
#pragma once

#include <functional>
#include <span>
#include <variant>
#include <vector>

#include "xgameruntime.hpp"

namespace danzi::hle::xasync {


extern const Guid PROVIDER;

struct Queue;
using Callback = void(__stdcall*)(void* context, bool canceled);
using MonitorCallback = void(__stdcall*)(void* context, const Queue* queue, std::uint32_t port);
using TerminatedCallback = void(__stdcall*)(void* context);


struct AsyncBlock;
using CompletionRoutine = void(__stdcall*)(AsyncBlock*);
struct AsyncBlock {
    const Queue* queue;
    void* context;
    CompletionRoutine callback;
    std::uintptr_t internal[4];
};

struct ProviderData {
    AsyncBlock* block;
    std::size_t buffer_size;
    void* buffer;
    void* context;
};
using Provider = HRESULT_(__stdcall*)(std::uint32_t op, const ProviderData* data);
using Work = HRESULT_(__stdcall*)(AsyncBlock*);

HRESULT_ XTaskQueueCreate(std::uint32_t work, std::uint32_t completion, const Queue** out);
HRESULT_ XTaskQueueGetPort(const Queue* handle, std::uint32_t port, const void** out);
HRESULT_ XTaskQueueCreateComposite(const void* work, const void* completion, const Queue** out);
HRESULT_ XTaskQueueDuplicateHandle(const Queue* handle, const Queue** out);
void XTaskQueueCloseHandle(const Queue* handle);
bool XTaskQueueDispatch(const Queue* handle, std::uint32_t port, std::uint32_t timeout_ms);
HRESULT_ XTaskQueueSubmitDelayedCallback(const Queue* handle, std::uint32_t port, std::uint32_t delay_ms, void* context, Callback callback);
HRESULT_ XTaskQueueTerminate(const Queue* handle, bool wait, void* context, TerminatedCallback callback);
HRESULT_ XTaskQueueRegisterMonitor(const Queue* handle, void* context, MonitorCallback callback, std::uint64_t* token);
void XTaskQueueUnregisterMonitor(const Queue* handle, std::uint64_t token);
bool XTaskQueueGetCurrentProcessTaskQueue(const Queue** out);
void XTaskQueueSetCurrentProcessTaskQueue(const Queue* handle);

HRESULT_ XAsyncBegin(AsyncBlock* block, void* context, const void* identity, const char* identity_name, Provider provider);
HRESULT_ XAsyncSchedule(AsyncBlock* block, std::uint32_t delay_ms);
void XAsyncComplete(AsyncBlock* block, HRESULT_ result, std::size_t required_size);
HRESULT_ XAsyncGetStatus(AsyncBlock* block, bool wait);
HRESULT_ XAsyncGetResultSize(AsyncBlock* block, std::size_t* out);
void XAsyncCancel(AsyncBlock* block);
HRESULT_ XAsyncGetResult(AsyncBlock* block, const void* identity, std::size_t buffer_size, void* buffer, std::size_t* used);
HRESULT_ XAsyncRun(AsyncBlock* block, Work work);


HRESULT_ finished(AsyncBlock* block, const void* identity, HRESULT_ hr);


using Result = std::variant<std::vector<std::uint8_t>, HRESULT_>;
HRESULT_ finished_with(AsyncBlock* block, const void* identity, Result result);


HRESULT_ pending_with(AsyncBlock* block, const void* identity, std::function<Result()> work);

template <class T>
HRESULT_ result(AsyncBlock* block, const void* identity, T* out) {
    return XAsyncGetResult(block, identity, sizeof(T), out, nullptr);
}

template <class T>
std::vector<std::uint8_t> bytes(const T& value) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    return {p, p + sizeof(T)};
}

extern const Object OBJECT;
std::vector<Interface> interfaces();

}   
