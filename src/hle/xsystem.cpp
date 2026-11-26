// SPDX-License-Identifier: GPL-3.0-or-later
// XSystem (`victory-hle/src/modules/xsystem.rs`; DanziLoader /
// victory-perf, see NOTICE.md): the console's identity and sandbox, and how
// analytics describe the device. Fixed values: one offline retail console,
// the model the user chose. And the device type, which code linked into the
// title reads from the registry (the `device type` module).
#include "xsystem.hpp"

#include <cstring>

#include "areas.hpp"
#include "d3d12x/device.hpp"
#include "registry.hpp"

namespace danzi::hle {

namespace xsystem {

namespace {

const Guid PROVIDER{0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};

const Guid IID{0x6fd71f09, 0x7513, 0x49f0, {0x89, 0xbc, 0xbf, 0xaf, 0x5d, 0xf6, 0xf8, 0x52}};

const Guid ANALYTICS{0xb884675d, 0xb738, 0x4a9c, {0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b}};

const Guid IID_2026{0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}};


constexpr const char* CONSOLE_ID = "00000000000000000000000000000000000001";
constexpr const char* SANDBOX = "RETAIL";

constexpr const char* DEVICE_ID = "VkljdG9yeSBvZmZsaW5lIGRldmljZSBpZGVudGl0eSE=";

using This = const Object*;
HRESULT_ __stdcall console_id(This, std::size_t size, char* buffer, std::size_t* used) { return xgr::put_string(CONSOLE_ID, size, buffer, used); }
HRESULT_ __stdcall sandbox_id(This, std::size_t size, char* buffer, std::size_t* used) { return xgr::put_string(SANDBOX, size, buffer, used); }
HRESULT_ __stdcall device_id(This, std::size_t size, char* buffer, std::size_t* used) { return xgr::put_string(DEVICE_ID, size, buffer, used); }

struct Version {
    std::uint16_t major, minor, build, revision;
};
struct AnalyticsInfo {
    Version os, hosting_os;
    char family[64];
    char form[64];
};
constexpr Version OS{10, 0, 22000, 0};


AnalyticsInfo* __stdcall analytics_info(This, AnalyticsInfo* out) {
    AnalyticsInfo info{};
    info.os = OS;
    info.hosting_os = OS;
    std::strcpy(info.family, "Xbox");
    std::strcpy(info.form, d3d12x::current_console() == d3d12x::Console::SeriesS ? "Xbox Series S" : "Xbox Series X");
    *out = info;
    return out;
}

#define F(x) reinterpret_cast<const void*>(&x)
using xgr::add_ref;
using xgr::missing;
const void* const VTABLE[6] = {F(missing<0>), F(add_ref), F(add_ref), F(console_id), F(sandbox_id), F(device_id)};
const void* const ANALYTICS_VTABLE[4] = {F(missing<0>), F(add_ref), F(add_ref), F(analytics_info)};
#undef F
const Object OBJECT{VTABLE, "system"};
const Object ANALYTICS_OBJECT{ANALYTICS_VTABLE, "analytics"};

}   

std::vector<Interface> interfaces() { return {{PROVIDER, IID, &OBJECT}, {PROVIDER, IID_2026, &OBJECT}, {ANALYTICS, ANALYTICS, &ANALYTICS_OBJECT}}; }

}   

namespace device_type {

namespace {

using Key = HKEY;
const Key LOCAL_MACHINE = reinterpret_cast<Key>(static_cast<std::uintptr_t>(0xffffffff80000002ull));
constexpr const char* KEY = R"(Software\Microsoft\Windows NT\CurrentVersion\OEM)";
constexpr const char* VALUE = "ConsoleMode";
constexpr LONG SUCCESS = 0, FILE_NOT_FOUND = 2, INVALID_PARAMETER = 87, MORE_DATA = 234;
constexpr DWORD DWORD_ = 4;


std::uint8_t OEM = 0;
Key oem() { return reinterpret_cast<Key>(&OEM); }

bool is(const wchar_t* text, const char* expected) {
    if (!text) {
        return false;
    }
    std::size_t n = std::wcslen(text), m = std::strlen(expected);
    if (n != m) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        wchar_t a = text[i];
        char b = expected[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<wchar_t>(a + 32);
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b + 32);
        }
        if (a != static_cast<unsigned char>(b)) {
            return false;
        }
    }
    return true;
}


std::optional<LONG> open(Key root, const wchar_t* sub, Key* out) {
    if (root == oem()) {
        return FILE_NOT_FOUND;
    }
    if (root == LOCAL_MACHINE && is(sub, KEY) && out) {
        *out = oem();
        return SUCCESS;
    }
    return std::nullopt;
}

}   


std::uint32_t console_mode(bool series_s) { return series_s ? 0x60 : 0x64; }

LONG WINAPI RegOpenKeyW(Key root, LPCWSTR sub, Key* out) {
    if (auto r = open(root, sub, out)) {
        return *r;
    }
    return HOST("advapi32.dll", "RegOpenKeyW", LONG(WINAPI*)(Key, LPCWSTR, Key*))(root, sub, out);
}

LONG WINAPI RegOpenKeyExW(Key root, LPCWSTR sub, DWORD options, REGSAM access, Key* out) {
    if (auto r = open(root, sub, out)) {
        return *r;
    }
    return HOST("advapi32.dll", "RegOpenKeyExW", LONG(WINAPI*)(Key, LPCWSTR, DWORD, REGSAM, Key*))(root, sub, options, access, out);
}

LONG WINAPI RegQueryValueExW(Key key, LPCWSTR name, LPDWORD reserved, LPDWORD kind, LPBYTE data, LPDWORD size) {
    if (key != oem()) {
        return HOST("advapi32.dll", "RegQueryValueExW", LONG(WINAPI*)(Key, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD))(key, name, reserved, kind, data, size);
    }
    if (!is(name, VALUE)) {
        return FILE_NOT_FOUND;
    }
    if (data && !size) {
        return INVALID_PARAMETER;
    }
    if (kind) {
        *kind = DWORD_;
    }
    if (!size) {
        return SUCCESS;
    }
    DWORD room = *size;
    *size = 4;
    if (!data) {
        return SUCCESS;
    }
    if (room < 4) {
        return MORE_DATA;
    }
    std::uint32_t v = console_mode(d3d12x::current_console() == d3d12x::Console::SeriesS);
    std::memcpy(data, &v, 4);
    return SUCCESS;
}

LONG WINAPI RegCloseKey(Key key) {
    if (key == oem()) {
        return SUCCESS;
    }
    return HOST("advapi32.dll", "RegCloseKey", LONG(WINAPI*)(Key))(key);
}

}   

}   
