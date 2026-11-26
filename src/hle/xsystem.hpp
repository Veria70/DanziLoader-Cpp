// SPDX-License-Identifier: GPL-3.0-or-later
// XSystem and the device type (`victory-hle/src/modules/xsystem.rs`).
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "xgameruntime.hpp"

namespace danzi::hle {

namespace xsystem {
std::vector<Interface> interfaces();
}

namespace device_type {
std::uint32_t console_mode(bool series_s);
LONG WINAPI RegOpenKeyW(HKEY root, LPCWSTR sub, HKEY* out);
LONG WINAPI RegOpenKeyExW(HKEY root, LPCWSTR sub, DWORD options, REGSAM access, HKEY* out);
LONG WINAPI RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD kind, LPBYTE data, LPDWORD size);
LONG WINAPI RegCloseKey(HKEY key);
}   

}   
