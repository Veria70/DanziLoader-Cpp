// SPDX-License-Identifier: GPL-3.0-or-later
// Milestone 6 fixture: a DLL the synthetic title ships beside it
// (`fixture_dll.dll`), mapped and initialised by the loader under test. No
// C runtime start-up (its own entry point): it logs, through the imported
// `Log`, every process notice, the thread notices of the one thread the
// title marks, and its TLS callbacks; it exports by name and by ordinal.
#include <windows.h>

extern "C" __declspec(dllimport) void __stdcall Log(const char*);

namespace {

volatile DWORD watched = 0;

void log_reason(const char* who, DWORD reason) {

    static const char* const ATTACH[] = {"process detach", "process attach", "thread attach", "thread detach"};
    char line[96];
    int n = 0;
    for (const char* p = who; *p && n < 60; ++p) {
        line[n++] = *p;
    }
    line[n++] = ':';
    line[n++] = ' ';
    const char* r = reason < 4 ? ATTACH[reason] : "?";
    for (const char* p = r; *p && n < 94; ++p) {
        line[n++] = *p;
    }
    line[n] = 0;
    Log(line);
}

bool counts(DWORD reason) { return reason <= 1 || (watched != 0 && GetCurrentThreadId() == watched); }

void NTAPI dll_tls(PVOID, DWORD reason, PVOID) {
    if (counts(reason)) {
        log_reason("dll tls callback", reason);
    }
}

}   

extern "C" {
#pragma section(".CRT$XLB", long, read)
__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK fixture_dll_tls = dll_tls;
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:fixture_dll_tls")

__declspec(thread) int dll_local = 7;

extern "C" {

__declspec(dllexport) int __stdcall dll_add(int a, int b) { return a + b + dll_local - 7; }

int __stdcall dll_by_ordinal(int a) { return a * 3; }

__declspec(dllexport) void __stdcall dll_watch(DWORD thread) { watched = thread; }

BOOL WINAPI fixture_dll_main(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (counts(reason)) {
        log_reason(reason == DLL_PROCESS_DETACH && reserved ? "dll main (process ending)" : "dll main", reason);
    }
    return TRUE;
}
}
