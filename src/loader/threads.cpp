// SPDX-License-Identifier: GPL-3.0-or-later
// Thread notices for title images (`victory-loader/src/threads.rs`;
// DanziLoader / victory-perf, see NOTICE.md). The prepared copies have no
// entry point and no TLS callbacks, so the Windows loader tells them nothing
// when a thread starts or ends. Our own TLS callback, which Windows calls on
// every thread of the process whoever created it, passes each notice on to
// every attached image: its TLS callbacks, then a DLL's `DllMain`.
//
// Port note: the reference saves and restores its own image's static TLS
// around the notices on threads Rust's std started (its std aborts if that
// state was touched before its thread set-up). The C++ runtime has no such
// check (thread-locals initialise on first use), so there is nothing to
// save.
#include "loader.hpp"
#include "core/log.hpp"

#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>

namespace danzi::loader {

namespace {

constexpr DWORD DLL_THREAD_ATTACH_ = 2;
constexpr DWORD DLL_THREAD_DETACH_ = 3;

struct Mapped {
    std::shared_ptr<LoadedImage> image;

    bool attached = false;

    bool quiet = false;
};


std::mutex& images_lock() {
    static std::mutex m;
    return m;
}
std::vector<Mapped>& images() {
    static std::vector<Mapped>* v = new std::vector<Mapped>();   
    return *v;
}


std::atomic<bool> EXITING{false};


constexpr auto DETACH_DEADLINE = std::chrono::seconds(5);


void end_process() { TerminateProcess(GetCurrentProcess(), 1); }

using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);


bool starts_in(HANDLE thread, const char* module) {
    static auto query = reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
    if (!query) {
        return false;
    }
    constexpr int ThreadQuerySetWin32StartAddress = 9;
    void* start = nullptr;
    if (query(thread, ThreadQuerySetWin32StartAddress, &start, sizeof start, nullptr) != 0) {
        return false;
    }
    HMODULE found = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(start), &found)) {
        return false;
    }
    return found == GetModuleHandleA(module);
}


void end_other_threads(DWORD keep) {
    DWORD process = GetCurrentProcessId(), me = GetCurrentThreadId();
    std::set<DWORD> ended;
    for (;;) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            break;
        }
        std::vector<DWORD> others;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof entry;
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID == process && entry.th32ThreadID != me && entry.th32ThreadID != keep) {
                others.push_back(entry.th32ThreadID);
            }
        }
        CloseHandle(snapshot);
        std::vector<DWORD> fresh;
        for (DWORD id : others) {
            if (ended.insert(id).second) {
                fresh.push_back(id);
            }
        }
        if (fresh.empty()) {
            break;
        }
        for (DWORD id : fresh) {
            HANDLE thread = OpenThread(THREAD_TERMINATE | THREAD_QUERY_INFORMATION, FALSE, id);
            if (thread) {
                if (!starts_in(thread, "ntdll.dll")) {
                    TerminateThread(thread, 0);
                }
                CloseHandle(thread);
            }
        }
    }
}

std::atomic<DWORD> WATCHDOG{0};

DWORD WINAPI watchdog(void*) {
    WATCHDOG.store(GetCurrentThreadId(), std::memory_order_seq_cst);
    Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(DETACH_DEADLINE).count()));
    end_process();
    return 0;
}


void __cdecl detach_all() {
    std::vector<std::shared_ptr<LoadedImage>> attached;
    {
        std::lock_guard l(images_lock());
        for (auto& m : images()) {
            if (m.attached) {
                m.attached = false;
                attached.push_back(m.image);
            }
        }
    }
    std::vector<std::shared_ptr<LoadedImage>> dlls, exes;
    for (auto it = attached.rbegin(); it != attached.rend(); ++it) {
        ((*it)->info().is_dll() ? dlls : exes).push_back(*it);
    }
    log::info("victory_loader::threads", "process exit: ending other threads, detaching title images", {{"images", log::debug(static_cast<std::uint64_t>(dlls.size() + exes.size()))}});
    EXITING.store(true, std::memory_order_seq_cst);


    HANDLE w = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    if (w) {
        CloseHandle(w);
    }
    auto waiting = std::chrono::steady_clock::now();
    DWORD id;
    for (;;) {
        id = WATCHDOG.load(std::memory_order_seq_cst);
        if (id != 0) {
            break;
        }
        if (std::chrono::steady_clock::now() - waiting >= DETACH_DEADLINE) {
            end_process();
        }
        SwitchToThread();
    }
    end_other_threads(id);
    for (const auto* list : {&dlls, &exes}) {
        for (const auto& image : *list) {


            image->notify(DLL_PROCESS_DETACH_, true, reinterpret_cast<void*>(1));
        }
    }
}

void NTAPI on_thread(PVOID, DWORD reason, PVOID) {
    if (reason != DLL_THREAD_ATTACH_ && reason != DLL_THREAD_DETACH_) {
        return;
    }
    if (EXITING.load(std::memory_order_seq_cst)) {
        return;   
    }

    std::vector<std::pair<std::shared_ptr<LoadedImage>, bool>> notify;
    {
        std::lock_guard l(images_lock());
        for (const auto& m : images()) {
            if (m.attached) {
                notify.emplace_back(m.image, !m.quiet);
            }
        }
    }

    if (reason == DLL_THREAD_ATTACH_) {
        for (const auto& [image, dll_main] : notify) {
            image->notify(reason, dll_main, nullptr);
        }
    } else {
        for (auto it = notify.rbegin(); it != notify.rend(); ++it) {
            it->first->notify(reason, it->second, nullptr);
        }
    }
}

}   

}   


extern "C" {
#pragma section(".CRT$XLAV", long, read)
__declspec(allocate(".CRT$XLAV")) extern const PIMAGE_TLS_CALLBACK danzi_loader_on_thread = danzi::loader::on_thread;
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:danzi_loader_on_thread")

namespace danzi::loader {

namespace threads {

void mapped(const std::shared_ptr<LoadedImage>& image) {
    std::lock_guard l(images_lock());
    auto& v = images();
    std::erase_if(v, [&](const Mapped& m) { return m.image->base() == image->base(); });
    v.push_back(Mapped{image, false, false});
}

void attached(std::uintptr_t base) {
    {
        std::lock_guard l(images_lock());
        auto& v = images();
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i].image->base() == base) {
                Mapped m = std::move(v[i]);
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                m.attached = true;
                v.push_back(std::move(m));
                break;
            }
        }
    }


    static std::once_flag at_exit;
    std::call_once(at_exit, [] { _crt_atexit(detach_all); });
}

void detached(std::uintptr_t base) {
    std::lock_guard l(images_lock());
    for (auto& m : images()) {
        if (m.image->base() == base) {
            m.attached = false;
            break;
        }
    }
}

void unmapped(std::uintptr_t base) {
    std::lock_guard l(images_lock());
    std::erase_if(images(), [&](const Mapped& m) { return m.image->base() == base; });
}

}   

std::optional<bool> disable_thread_calls(std::uintptr_t base) {
    std::lock_guard l(images_lock());
    for (auto& m : images()) {
        if (m.image->base() == base) {
            if (m.image->info().tls) {
                return false;
            }
            m.quiet = true;
            return true;
        }
    }
    return std::nullopt;
}

}   
