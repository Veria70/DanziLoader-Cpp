// SPDX-License-Identifier: GPL-3.0-or-later
// The `kernel` module (`victory-hle/src/modules/kernel.rs`; DanziLoader /
// victory-perf, see NOTICE.md): module handles and paths that name the
// title's images (not our exe or cache copies), our console modules as
// pseudo handles, the title's debug output, its ways to end the process,
// and its unhandled-exception filter behind ours.
#pragma once

#include <windows.h>

namespace danzi::hle::kernel {

HMODULE WINAPI GetModuleHandleW(LPCWSTR name);
HMODULE WINAPI GetModuleHandleA(LPCSTR name);
DWORD WINAPI GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size);
DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR out, DWORD size);
HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);
HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags);
HMODULE WINAPI LoadLibraryA(LPCSTR name);
HMODULE WINAPI LoadLibraryW(LPCWSTR name);
FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name);
BOOL WINAPI FreeLibrary(HMODULE module);
BOOL WINAPI DisableThreadLibraryCalls(HMODULE module);
void WINAPI OutputDebugStringA(LPCSTR text);
void WINAPI OutputDebugStringW(LPCWSTR text);


[[noreturn]] void __cdecl exit(int code);
[[noreturn]] void WINAPI ExitProcess(UINT code);
[[noreturn]] void __cdecl abort();
[[noreturn]] void __cdecl terminate();
[[noreturn]] void __cdecl _invalid_parameter_noinfo_noreturn();
BOOL WINAPI TerminateProcess(HANDLE process, UINT code);
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);

}   
