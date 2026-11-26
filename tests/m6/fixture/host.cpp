// SPDX-License-Identifier: GPL-3.0-or-later
// Milestone 6 fixture: the "console API" the synthetic title imports
// (`fixture_host.dll`). Only its import library is used: the loaders under
// test bind these names to their harness's own functions, never to this
// DLL. `Unimplemented` is the import the harness leaves unresolved.
#include <windows.h>

extern "C" {
__declspec(dllexport) void __stdcall Log(const char*) {}
__declspec(dllexport) unsigned __stdcall Value(unsigned x) { return x; }
__declspec(dllexport) unsigned __stdcall Unimplemented(unsigned) { return 1; }


__declspec(dllexport) unsigned __stdcall Mode() { return 0; }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
