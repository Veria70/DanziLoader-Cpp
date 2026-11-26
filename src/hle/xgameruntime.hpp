// SPDX-License-Identifier: GPL-3.0-or-later
// `xgameruntime.dll`: the console game runtime (`victory-hle/src/modules/
// xgameruntime.rs`; DanziLoader / victory-perf, see NOTICE.md). A stub
// linked into the title loads the DLL (a pseudo handle, see `kernel`), looks
// up the `*ApiImpl` functions and `XErrorReport`, and initialises. Each
// runtime API call then asks `QueryApiImpl` for a COM-style object of its
// area (provider and interface GUIDs), calls one method and releases it.
// Until an area is implemented its query fails. Log lines: milestone 7.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.hpp"

namespace danzi::hle {

using HRESULT_ = std::int32_t;
constexpr HRESULT_ S_OK_ = 0;

constexpr HRESULT_ NOT_SUPPORTED = static_cast<HRESULT_>(0x80070032u);
constexpr HRESULT_ E_PENDING_ = static_cast<HRESULT_>(0x8000000Au);
constexpr HRESULT_ E_ABORT_ = static_cast<HRESULT_>(0x80004004u);
constexpr HRESULT_ E_NOTIMPL_ = static_cast<HRESULT_>(0x80004001u);
constexpr HRESULT_ E_INVALIDARG_ = static_cast<HRESULT_>(0x80070057u);
constexpr HRESULT_ E_NOT_SUFFICIENT_BUFFER_ = static_cast<HRESULT_>(0x8007007Au);
constexpr HRESULT_ E_ILLEGAL_METHOD_CALL_ = static_cast<HRESULT_>(0x8000000Eu);

struct Guid {
    std::uint32_t data1;
    std::uint16_t data2, data3;
    std::uint8_t data4[8];
    bool operator==(const Guid& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};


struct Object {
    const void* const* vtable;

    const char* area;
};


struct Interface {
    Guid provider, iid;
    const Object* object;
};

namespace xgr {

std::uint32_t __stdcall add_ref(const Object*);

template <std::size_t SLOT>
HRESULT_ __stdcall missing(const Object* self) {
    log::warn("victory_hle::modules::xgameruntime", "runtime method not implemented", {{"area", log::debug(self ? self->area : "")}, {"slot", log::hex(SLOT * 8)}});
    return E_NOTIMPL_;
}

template <class T>
HRESULT_ put(T* out, T value) {
    if (!out) {
        return E_INVALIDARG_;
    }
    *out = value;
    return S_OK_;
}


HRESULT_ put_string(const std::string& text, std::size_t size, char* buffer, std::size_t* used);

std::optional<std::string> text(const char* s);

std::string utf16_lossy(std::wstring_view w);

std::wstring utf16(std::string_view s);

std::uint64_t token();

HRESULT_ __stdcall XGameRuntimeInitialize();
void __stdcall XGameRuntimeUninitialize();
HRESULT_ __stdcall InitializeApiImpl(std::uint64_t version, std::uint64_t build);
HRESULT_ __stdcall InitializeApiImplEx(std::uint64_t version, std::uint64_t build, std::uint64_t flags);
HRESULT_ __stdcall InitializeApiImplEx2(std::uint64_t version, std::uint64_t build, std::uint64_t flags, const void* options);
void __stdcall UninitializeApiImpl();
HRESULT_ __stdcall QueryApiImpl(const Guid* provider, const Guid* iid, void** out);
void __stdcall XErrorReport(HRESULT_ hr, const char* message);


const std::vector<Interface>& areas();
}   

}   
