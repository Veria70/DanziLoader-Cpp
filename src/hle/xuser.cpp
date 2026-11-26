// SPDX-License-Identifier: GPL-3.0-or-later
// XUser (`victory-hle/src/modules/xuser.rs`; DanziLoader / victory-perf, see
// NOTICE.md): one offline local user, signed in, never a guest, with every
// privilege. Adding a user completes through `xasync` like any other async
// call. No input device is associated with the user.
#include "areas.hpp"
#include "modules.hpp"
#include "xasync.hpp"

namespace danzi::hle::xuser {

namespace {

using xasync::AsyncBlock;
using xasync::ProviderData;

const Guid PROVIDER{0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
const Guid IID{0xeb9bf948, 0x18dc, 0x4d82, {0xbb, 0xcc, 0x40, 0xe0, 0xa8, 0x09, 0xc4, 0xc0}};
const Guid GAMERTAG_IID{0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};
const Guid DEVICE_PROVIDER{0x7d824997, 0x10dc, 0x45ab, {0x86, 0xb7, 0x27, 0x37, 0x76, 0x7c, 0x0b, 0xf1}};
const Guid DEVICE_IID{0x0cc6a956, 0xe7e1, 0x4fdf, {0x93, 0x41, 0x9d, 0x5d, 0xa6, 0x49, 0xeb, 0xc8}};


constexpr HRESULT_ USER_NOT_FOUND = static_cast<HRESULT_>(0x89245104u);

constexpr HRESULT_ FAILED_TO_GET_TOKEN = static_cast<HRESULT_>(0x89245111u);
constexpr std::uint64_t LOCAL_ID = 1;
constexpr const char* GAMERTAG = "Player";


std::uint8_t USER = 0;
using Handle = const void*;
Handle user() { return &USER; }

HRESULT_ found(bool f, Handle* out) { return f ? xgr::put(out, user()) : USER_NOT_FOUND; }

using This = const Object*;
HRESULT_ __stdcall duplicate(This, Handle, Handle* out) { return xgr::put(out, user()); }
void __stdcall close(This, Handle) {}
std::int32_t __stdcall compare(This, Handle a, Handle b) { return a != b ? 1 : 0; }


HRESULT_ __stdcall add_provider(std::uint32_t op, const ProviderData* data) {
    if (op == 0) {
        xasync::XAsyncComplete(data->block, S_OK_, sizeof(Handle));
    } else if (op == 2) {
        return xgr::put(static_cast<Handle*>(data->buffer), user());
    }
    return S_OK_;
}


std::uint8_t ADD = 0;
std::uint8_t RESOLVE = 0;
std::uint8_t TOKEN = 0;


HRESULT_ __stdcall add_async(This, std::uint32_t options, AsyncBlock* block) {
    log::info("victory_hle::modules::xuser", "XUserAddAsync: the offline local user", {{"options", log::debug(options)}});
    return xasync::XAsyncBegin(block, nullptr, &ADD, nullptr, add_provider); }
HRESULT_ __stdcall add_result(This, AsyncBlock* block, Handle* out) { return xasync::XAsyncGetResult(block, &ADD, sizeof(Handle), out, nullptr); }
HRESULT_ __stdcall get_local_id(This, Handle, std::uint64_t* out) { return xgr::put(out, LOCAL_ID); }
HRESULT_ __stdcall find_by_local_id(This, std::uint64_t id, Handle* out) { return found(id == LOCAL_ID, out); }
HRESULT_ __stdcall get_id(This, Handle, std::uint64_t* out) { return xgr::put(out, XUID); }
HRESULT_ __stdcall find_by_id(This, std::uint64_t id, Handle* out) { return found(id == XUID, out); }
HRESULT_ __stdcall get_is_guest(This, Handle, bool* out) { return xgr::put(out, false); }

HRESULT_ __stdcall get_state(This, Handle, std::uint32_t* out) { return xgr::put(out, 0u); }


HRESULT_ __stdcall get_gamertag(This, Handle, std::uint32_t component, std::size_t size, char* buffer, std::size_t* used) {
    return xgr::put_string(component == 2 ? "" : GAMERTAG, size, buffer, used);
}

HRESULT_ __stdcall check_privilege(This, Handle, std::uint32_t, std::uint32_t, bool* has, std::uint32_t* reason) {
    if (reason) {
        *reason = 0;
    }
    return xgr::put(has, true);
}

HRESULT_ __stdcall get_age_group(This, Handle, std::uint32_t* out) { return xgr::put(out, 3u); }

HRESULT_ __stdcall resolve_privilege_async(This, Handle, std::uint32_t, std::uint32_t, AsyncBlock* block) { return xasync::finished(block, &RESOLVE, S_OK_); }
HRESULT_ __stdcall resolve_privilege_result(This, AsyncBlock* block) { return xasync::XAsyncGetResult(block, &RESOLVE, 0, nullptr, nullptr); }
HRESULT_ __stdcall token_async(This, Handle, std::uint32_t, const char*, const char* url, std::size_t, const void*, std::size_t, const void*, AsyncBlock* block) {
    log::info("victory_hle::modules::xuser", "token and signature: offline", {{"url", log::debug(xgr::text(url).value_or(""))}});
    return xasync::finished(block, &TOKEN, FAILED_TO_GET_TOKEN);
}
HRESULT_ __stdcall token_size(This, AsyncBlock* block, std::size_t* out) { return xasync::XAsyncGetResultSize(block, out); }
HRESULT_ __stdcall token_result(This, AsyncBlock* block, std::size_t size, void* buffer, const void**, std::size_t* used) {
    return xasync::XAsyncGetResult(block, &TOKEN, size, buffer, used);
}


HRESULT_ __stdcall reg(This, const void*, void*, const void*, std::uint64_t* out) { return xgr::put(out, xgr::token()); }
bool __stdcall unreg(This, std::uint64_t, bool) { return true; }

HRESULT_ __stdcall find_for_device(This, const void*, Handle*) { return USER_NOT_FOUND; }

#define F(x) reinterpret_cast<const void*>(&x)
using xgr::add_ref;
using xgr::missing;
const void* const VTABLE[35] = {
    F(missing<0>),           F(add_ref),        F(add_ref),          F(duplicate),       F(close),          F(compare),     F(missing<6>),
    F(add_async),            F(add_result),     F(get_local_id),     F(find_by_local_id), F(get_id),        F(find_by_id),  F(get_is_guest),
    F(get_state),            F(get_gamertag),   F(missing<16>),      F(missing<17>),     F(missing<18>),    F(get_age_group), F(check_privilege),
    F(resolve_privilege_async), F(resolve_privilege_result), F(token_async), F(token_size), F(token_result), F(missing<26>), F(missing<27>),
    F(missing<28>),          F(missing<29>),    F(missing<30>),      F(missing<31>),     F(missing<32>),    F(reg),         F(unreg),
};
const void* const GAMERTAG_VTABLE[4] = {F(missing<0>), F(add_ref), F(add_ref), F(get_gamertag)};
const void* const DEVICE_VTABLE[6] = {F(missing<0>), F(add_ref), F(add_ref), F(find_for_device), F(reg), F(unreg)};
#undef F

const Object OBJECT{VTABLE, "user"};
const Object GAMERTAG_OBJECT{GAMERTAG_VTABLE, "user gamertag"};
const Object DEVICE_OBJECT{DEVICE_VTABLE, "user devices"};

}   

std::vector<Interface> interfaces() {
    return {{PROVIDER, IID, &OBJECT}, {PROVIDER, PROVIDER, &OBJECT}, {PROVIDER, GAMERTAG_IID, &GAMERTAG_OBJECT}, {DEVICE_PROVIDER, DEVICE_IID, &DEVICE_OBJECT}};
}

}   
