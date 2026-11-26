// SPDX-License-Identifier: GPL-3.0-or-later
// Milestone 6 fixture: a synthetic title (`fixture_title.exe`) run by the
// loader under test. No C runtime start-up (its own entry point). It uses
// static TLS the way an executable does (index 0 assumed, which the loader
// redirects), has TLS callbacks, calls the "console API" (`fixture_host`),
// a DLL it ships (`fixture_dll`, by name and by ordinal), an import nobody
// implements, and the host (a thread of its own); everything it sees goes
// to the harness's log through `Log`. Its entry point returns 42.
#include <windows.h>

extern "C" {
__declspec(dllimport) void __stdcall Log(const char*);
__declspec(dllimport) unsigned __stdcall Value(unsigned);
__declspec(dllimport) unsigned __stdcall Unimplemented(unsigned);
__declspec(dllimport) unsigned __stdcall Mode();
__declspec(dllimport) int __stdcall dll_add(int, int);
__declspec(dllimport) void __stdcall dll_watch(DWORD);

__declspec(dllimport) int __stdcall dll_by_ordinal(int);
}

namespace {


void log_num(const char* text, long long v) {
    char line[128];
    int n = 0;
    for (const char* p = text; *p && n < 100; ++p) {
        line[n++] = *p;
    }
    line[n++] = ' ';
    if (v < 0) {
        line[n++] = '-';
        v = -v;
    }
    char digits[24];
    int d = 0;
    do {
        digits[d++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (d > 0) {
        line[n++] = digits[--d];
    }
    line[n] = 0;
    Log(line);
}

__declspec(thread) int title_local = 5;
__declspec(thread) long long title_big[4] = {1, 2, 3, 4};

volatile DWORD watched = 0;

void NTAPI title_tls(PVOID, DWORD reason, PVOID) {
    if (reason <= 1 || (watched != 0 && GetCurrentThreadId() == watched)) {
        log_num("title tls callback, reason", reason);
    }
}

DWORD WINAPI worker(void*) {

    log_num("worker: title_local", title_local);
    log_num("worker: title_big[3]", title_big[3]);
    title_local = 99;
    log_num("worker: title_local now", title_local);
    return 7;
}

}   

extern "C" {
#pragma section(".CRT$XLB", long, read)
__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK fixture_title_tls = title_tls;
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:fixture_title_tls")

extern "C" DWORD __stdcall title_entry(void*) {
    Log("title: hello");
    log_num("title: title_local", title_local);
    title_local += 10;
    title_big[0] = 40;
    log_num("title: title_local now", title_local);
    log_num("title: title_big sum", title_big[0] + title_big[1] + title_big[2] + title_big[3]);
    log_num("title: Value(1234)", Value(1234));
    log_num("title: dll_add(2, 3)", dll_add(2, 3));
    log_num("title: dll_by_ordinal(14)", dll_by_ordinal(14));

    DWORD id = 0;
    HANDLE t = CreateThread(nullptr, 0, worker, nullptr, CREATE_SUSPENDED, &id);
    watched = id;
    dll_watch(id);
    ResumeThread(t);
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    watched = 0;
    dll_watch(0);
    log_num("title: worker returned", code);
    log_num("title: title_local after the worker", title_local);
    log_num("title: Unimplemented(5)", Unimplemented(5));
    unsigned mode = Mode();
    if (mode == 1) {
        Log("title: writing to 0x10");
        *reinterpret_cast<volatile int*>(0x10) = 1;
    } else if (mode == 2) {
        Log("title: breakpoint");
        __debugbreak();
    }
    Log("title: bye");
    return 42;
}
