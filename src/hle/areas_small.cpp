// SPDX-License-Identifier: GPL-3.0-or-later
// Small runtime API areas (`victory-hle/src/modules/`: `storage.rs`, `xnetworking.rs`,
// `xpackage.rs`, `display.rs`, `speech.rs`; DanziLoader / victory-perf, see
// NOTICE.md). Log lines: milestone 7.
#include <filesystem>
#include <mutex>
#include <optional>

#include "areas.hpp"
#include "core/log.hpp"
#include "modules.hpp"
#include "runtime.hpp"
#include "xasync.hpp"

namespace danzi::hle {

using xasync::AsyncBlock;
using xgr::add_ref;
using xgr::missing;
using This = const Object*;
#define F(x) reinterpret_cast<const void*>(&x)

namespace {
HRESULT_ __stdcall reg_token(This, const void*, void*, const void*, std::uint64_t* out) { return xgr::put(out, xgr::token()); }
bool __stdcall unreg_true(This, std::uint64_t, bool) { return true; }

}   


namespace storage {

std::uint64_t used_bytes(const std::filesystem::path& dir) {
    std::error_code ec;
    std::uint64_t total = 0;
    std::filesystem::directory_iterator it(dir, ec), end;
    if (ec) {
        return 0;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }

        std::error_code e2;
        auto st = it->symlink_status(e2);
        if (e2) {
            continue;
        }
        if (std::filesystem::is_directory(st)) {
            total += used_bytes(it->path());
        } else {
            auto size = it->file_size(e2);
            total += e2 ? 0 : size;
        }
    }
    return total;
}

std::uint64_t disk_free(const std::filesystem::path& dir) {
    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(dir.c_str(), &free, nullptr, nullptr)) {
        return free.QuadPart;
    }
    return UINT64_MAX;
}

namespace {
const Guid PROVIDER{0xf4faf4d4, 0x2d04, 0x4fce, {0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e, 0x84}};
const Guid IID{0xd29411df, 0x0794, 0x4553, {0x8b, 0x27, 0x95, 0xfc, 0x02, 0xd0, 0xf7, 0x5d}};

const Guid IID_2026{0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}};
constexpr HRESULT_ E_FAIL_ = static_cast<HRESULT_>(0x80004005u);


std::optional<std::string> folder() {
    auto storage = runtime::title().storage;
    if (storage.empty()) {
        return std::nullopt;
    }
    if (auto letter = files::mount(storage)) {
        return std::string{static_cast<char>(*letter), ':', '\\'};
    }
    return std::nullopt;
}

HRESULT_ __stdcall path_size(This, std::size_t* out) {
    auto p = folder();
    return p ? xgr::put(out, p->size() + 1) : NOT_SUPPORTED;
}


HRESULT_ __stdcall path(This, std::size_t size, char* buffer, std::size_t* used) {
    auto p = folder();
    if (!p) {
        return NOT_SUPPORTED;
    }
    if (size <= p->size()) {
        return E_FAIL_;
    }
    return xgr::put_string(*p, size, buffer, used);
}

struct SpaceInfo {
    std::uint64_t available_free, total_free, used, total;
};
SpaceInfo space() {
    auto title = runtime::title();
    std::uint64_t used = used_bytes(title.storage);
    std::uint64_t free = title.storage_bytes > used ? title.storage_bytes - used : 0;

    return {std::min(free, disk_free(title.storage)), free, used, title.storage_bytes};
}
HRESULT_ __stdcall space_info(This, SpaceInfo* out) { return xgr::put(out, space()); }

std::uint8_t PROMPT = 0;

HRESULT_ __stdcall prompt_for_space(This, std::uint64_t bytes, AsyncBlock* block) {
    return xasync::finished(block, &PROMPT, bytes <= space().total_free ? S_OK_ : E_ABORT_);
}
HRESULT_ __stdcall prompt_result(This, AsyncBlock* block) { return xasync::XAsyncGetResult(block, &PROMPT, 0, nullptr, nullptr); }

const void* const VTABLE[8] = {F(missing<0>), F(add_ref), F(add_ref), F(path_size), F(path), F(space_info), F(prompt_for_space), F(prompt_result)};
const Object OBJECT{VTABLE, "persistent local storage"};
}   
std::vector<Interface> interfaces() { return {{PROVIDER, IID, &OBJECT}, {PROVIDER, IID_2026, &OBJECT}}; }
}   


namespace xnetworking {
namespace {
const Guid PROVIDER{0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}};
const Guid IID_2026{0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}};

constexpr HRESULT_ UNREACHABLE = static_cast<HRESULT_>(0x800704CFu);
constexpr std::uint16_t UDP_PORT = 3074;

struct ConnectivityHint {
    std::uint32_t level, cost, interface_type;
    bool initialized, approaching_data_limit, over_data_limit, roaming;
};

constexpr ConnectivityHint OFFLINE{1, 0, 0, true, false, false, false};

std::uint8_t PORT = 0;
std::uint8_t SECURITY = 0;

HRESULT_ __stdcall udp_port(This, std::uint16_t* out) { return xgr::put(out, UDP_PORT); }
HRESULT_ __stdcall udp_port_async(This, AsyncBlock* block) { return xasync::finished_with(block, &PORT, xasync::bytes(UDP_PORT)); }
HRESULT_ __stdcall udp_port_result(This, AsyncBlock* block, std::uint16_t* out) { return xasync::result(block, &PORT, out); }
HRESULT_ __stdcall security_info_async(This, const char*, AsyncBlock* block) { return xasync::finished(block, &SECURITY, UNREACHABLE); }
HRESULT_ __stdcall security_info_size(This, AsyncBlock* block, std::size_t* out) { return xasync::XAsyncGetResultSize(block, out); }
HRESULT_ __stdcall security_info_result(This, AsyncBlock* block, std::size_t size, std::size_t* used, std::uint8_t* buffer, const void**) {
    return xasync::XAsyncGetResult(block, &SECURITY, size, buffer, used);
}

HRESULT_ __stdcall verify_certificate(This, void*, const void*) { return UNREACHABLE; }
HRESULT_ __stdcall connectivity_hint(This, ConnectivityHint* out) { return xgr::put(out, OFFLINE); }

const void* const VTABLE[18] = {F(missing<0>),          F(add_ref),         F(add_ref),         F(udp_port),       F(udp_port_async), F(udp_port_result),
                                F(missing<6>),          F(missing<7>),      F(security_info_async), F(security_info_size), F(security_info_result), F(missing<11>),
                                F(missing<12>),         F(missing<13>),     F(verify_certificate), F(connectivity_hint), F(reg_token), F(unreg_true)};
const Object OBJECT{VTABLE, "networking"};
}   
std::vector<Interface> interfaces() { return {{PROVIDER, PROVIDER, &OBJECT}, {PROVIDER, IID_2026, &OBJECT}}; }
}   


namespace xpackage {
std::string identifier() {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", runtime::title().id);
    return b;
}
namespace {
const Guid PROVIDER{0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}};
const Guid IID_2026{0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};
bool is_ours(const char* id) {
    auto t = xgr::text(id);
    return t && *t == identifier();
}
std::uint64_t package_bytes() {
    static std::once_flag once;
    static std::uint64_t bytes = 0;
    std::call_once(once, [] {
        if (auto m = runtime::main_image()) {
            if (auto p = runtime::image_path(*m)) {
                if (p->has_parent_path()) {
                    bytes = storage::used_bytes(p->parent_path());
                }
            }
        }
    });
    return bytes;
}

std::uint8_t MONITOR = 0, INSTALL = 0, MOUNT = 0;
const void* monitor() { return &MONITOR; }

HRESULT_ __stdcall current_identifier(This, std::size_t size, char* buffer) { return xgr::put_string(identifier(), size, buffer, nullptr); }
bool __stdcall is_packaged(This) { return true; }
HRESULT_ __stdcall create_monitor(This, const char* id, std::uint32_t, const void*, std::uint32_t, const void*, const void** out) {
    if (!is_ours(id)) {
        return NO_PACKAGE;
    }
    return xgr::put(out, monitor());
}
void __stdcall close_monitor(This, const void*) {}
struct Progress {
    std::uint64_t total_bytes, installed_bytes, launch_bytes;
    bool launchable, completed;
};
void __stdcall progress(This, const void*, Progress* out) {
    std::uint64_t b = package_bytes();
    xgr::put(out, Progress{b, b, b, true, true});
}
HRESULT_ __stdcall find_availability(This, const char* id, std::uint32_t, const void*, std::uint32_t* out) {
    if (!is_ours(id)) {
        return NO_PACKAGE;
    }
    return xgr::put(out, 0u);
}
HRESULT_ __stdcall change_order(This, const char* id, std::uint32_t, const void*) { return is_ours(id) ? S_OK_ : NO_PACKAGE; }
HRESULT_ __stdcall install_chunks_async(This, const char* id, std::uint32_t, const void*, std::uint32_t, bool, AsyncBlock* block) {
    xasync::Result result = is_ours(id) ? xasync::Result(xasync::bytes(monitor())) : xasync::Result(NO_PACKAGE);
    return xasync::finished_with(block, &INSTALL, result);
}
HRESULT_ __stdcall install_chunks_result(This, AsyncBlock* block, const void** out) { return xasync::result(block, &INSTALL, out); }
HRESULT_ __stdcall estimate_download(This, const char* id, std::uint32_t, const void*, std::uint64_t* bytes, bool* confirm) {
    if (!is_ours(id)) {
        return NO_PACKAGE;
    }
    if (confirm) {
        *confirm = false;
    }
    return xgr::put(bytes, std::uint64_t{0});
}
HRESULT_ __stdcall uninstall_chunks(This, const char* id, std::uint32_t, const void*) { return is_ours(id) ? S_OK_ : NO_PACKAGE; }
HRESULT_ __stdcall mount_path(This, const void*, std::size_t, std::uint8_t*) { return E_INVALIDARG_; }
void __stdcall close_mount(This, const void*) {}
bool __stdcall uninstall_package(This, const char* id) {
    log::info("victory_hle::modules::xpackage", "package uninstall refused", {{"id", log::debug(xgr::text(id).value_or(""))}});
    return false;
}
HRESULT_ __stdcall mount_async(This, const char* id, AsyncBlock* block) {
    log::info("victory_hle::modules::xpackage", "package mount: not installed", {{"id", log::debug(xgr::text(id).value_or(""))}});
    return xasync::finished(block, &MOUNT, NO_PACKAGE);
}
HRESULT_ __stdcall mount_result(This, AsyncBlock* block, const void** out) { return xasync::result(block, &MOUNT, out); }
HRESULT_ __stdcall enumerate(This, std::uint32_t, std::uint32_t, void*, const void*) { return S_OK_; }

const void* const VTABLE[40] = {
    F(missing<0>),        F(add_ref),          F(add_ref),        F(current_identifier), F(is_packaged),          F(create_monitor),      F(close_monitor),
    F(progress),          F(missing<8>),       F(reg_token),      F(missing<10>),        F(missing<11>),          F(find_availability),   F(missing<13>),
    F(change_order),      F(missing<15>),      F(install_chunks_async), F(install_chunks_result), F(estimate_download), F(uninstall_chunks), F(missing<20>),
    F(missing<21>),       F(missing<22>),      F(missing<23>),    F(missing<24>),        F(mount_path),           F(close_mount),         F(missing<27>),
    F(missing<28>),       F(missing<29>),      F(missing<30>),    F(missing<31>),        F(missing<32>),          F(missing<33>),         F(uninstall_package),
    F(missing<35>),       F(missing<36>),      F(mount_async),    F(mount_result),       F(enumerate),
};
const Object OBJECT{VTABLE, "package"};
}   
std::vector<Interface> interfaces() { return {{PROVIDER, IID_2026, &OBJECT}}; }
}   


namespace display {
namespace {
const Guid HDR_PROVIDER{0x03f0fe74, 0xfdd9, 0x4e5c, {0xb6, 0x30, 0xf9, 0x33, 0x9c, 0x47, 0xac, 0xc5}};
const Guid HDR_IID{0x35f07670, 0x706e, 0x4bfb, {0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3}};
const Guid SHELL{0x1b339674, 0x328d, 0x4283, {0xa2, 0x00, 0x31, 0x71, 0xf1, 0x8d, 0x36, 0x39}};
const Guid CAPTURE_PROVIDER{0xa4f1aee2, 0x4bf1, 0x4485, {0xb0, 0x08, 0xa7, 0xc2, 0x6d, 0x52, 0xac, 0x27}};
const Guid CAPTURE_IID_2026{0x22e672d7, 0xb4e3, 0x406c, {0xbd, 0x50, 0x8f, 0x0d, 0x25, 0x23, 0x6f, 0x9e}};

std::uint32_t __stdcall try_enable_hdr(This, std::uint32_t preference, void*) {
    log::info("victory_hle::modules::display", "HDR mode: disabled (SDR presentation)", {{"preference", log::debug(preference)}});
    return 2;
}
HRESULT_ __stdcall launch_uri(This, const void*, const char* uri) {
    log::info("victory_hle::modules::display", "URI not launched", {{"uri", log::debug(xgr::text(uri).value_or(""))}});
    return NOT_SUPPORTED;
}
HRESULT_ last_error() { return static_cast<HRESULT_>(0x80070000u | (GetLastError() & 0xffffu)); }
HRESULT_ __stdcall acquire_deferral(This, void** out) {
    if (!out) {
        return E_INVALIDARG_;
    }
    wchar_t reason[] = L"Title display deferral";
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = reason;
    HANDLE request = PowerCreateRequest(&context);
    if (request == INVALID_HANDLE_VALUE) {
        return last_error();
    }
    if (!PowerSetRequest(request, PowerRequestDisplayRequired)) {
        HRESULT_ hr = last_error();
        CloseHandle(request);
        return hr;
    }
    return xgr::put(out, static_cast<void*>(request));
}
void __stdcall close_deferral(This, void* handle) {
    if (handle) {
        PowerClearRequest(handle, PowerRequestDisplayRequired);
        CloseHandle(handle);
    }
}
HRESULT_ __stdcall take_screenshot(This, const void*, void*) {
    log::info("victory_hle::modules::display", "screenshot not taken");
    return NOT_SUPPORTED;
}

const void* const HDR_VTABLE[5] = {F(missing<0>), F(add_ref), F(add_ref), F(missing<3>), F(try_enable_hdr)};
const void* const SHELL_VTABLE[6] = {F(missing<0>), F(add_ref), F(add_ref), F(launch_uri), F(acquire_deferral), F(close_deferral)};
const void* const CAPTURE_VTABLE[6] = {F(missing<0>), F(add_ref), F(add_ref), F(missing<3>), F(missing<4>), F(take_screenshot)};
const Object HDR_OBJECT{HDR_VTABLE, "hdr"};
const Object SHELL_OBJECT{SHELL_VTABLE, "shell"};
const Object CAPTURE_OBJECT{CAPTURE_VTABLE, "capture"};
}   
std::vector<Interface> interfaces() {
    return {{HDR_PROVIDER, HDR_IID, &HDR_OBJECT}, {SHELL, SHELL, &SHELL_OBJECT}, {CAPTURE_PROVIDER, CAPTURE_IID_2026, &CAPTURE_OBJECT}};
}
}   


namespace speech {
namespace {
const Guid PROVIDER{0x3e241977, 0x6237, 0x41e9, {0x85, 0x59, 0x10, 0x2c, 0x2d, 0x99, 0x83, 0xf1}};
const Guid IID{0xd722b373, 0x8c4d, 0x4692, {0x9e, 0x51, 0xd6, 0xad, 0x9b, 0x37, 0xaa, 0x7d}};
constexpr std::uint8_t SILENCE[44] = {'R', 'I', 'F', 'F', 0x24, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 0x10, 0, 0, 0, 1, 0,
                                      1,   0,   0x80, 0x3e, 0,   0,   0,   0x7d, 0, 0, 2, 0, 0x10, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
std::uint8_t SYNTHESIZER = 0, STREAM = 0;

HRESULT_ __stdcall enumerate_voices(This, void*, const void*) { return S_OK_; }
HRESULT_ __stdcall create(This, const std::uint8_t** out) { return xgr::put(out, static_cast<const std::uint8_t*>(&SYNTHESIZER)); }
HRESULT_ __stdcall close(This, const std::uint8_t*) { return S_OK_; }
HRESULT_ __stdcall set_default_voice(This, const std::uint8_t*) { return S_OK_; }
HRESULT_ __stdcall set_custom_voice(This, const std::uint8_t*, const char*) { return S_OK_; }
HRESULT_ __stdcall create_stream(This, const std::uint8_t*, const char*, const std::uint8_t** out) { return xgr::put(out, static_cast<const std::uint8_t*>(&STREAM)); }
HRESULT_ __stdcall stream_size(This, const std::uint8_t*, std::size_t* out) { return xgr::put(out, sizeof SILENCE); }
HRESULT_ __stdcall stream_data(This, const std::uint8_t*, std::size_t size, std::uint8_t* buffer, std::size_t* used) {
    if (!buffer || size < sizeof SILENCE) {
        return E_NOT_SUFFICIENT_BUFFER_;
    }
    std::memcpy(buffer, SILENCE, sizeof SILENCE);
    if (used) {
        *used = sizeof SILENCE;
    }
    return S_OK_;
}
const void* const VTABLE[22] = {F(missing<0>),  F(add_ref),     F(add_ref),     F(missing<3>),         F(missing<4>),     F(missing<5>),
                                F(missing<6>),  F(missing<7>),  F(enumerate_voices), F(create),        F(close),          F(set_default_voice),
                                F(set_custom_voice), F(create_stream), F(close), F(stream_size),       F(stream_data),    F(missing<17>),
                                F(missing<18>), F(missing<19>), F(missing<20>), F(create_stream)};
const Object OBJECT{VTABLE, "accessibility"};
}   
std::vector<Interface> interfaces() { return {{PROVIDER, IID, &OBJECT}}; }
}   

#undef F

}   
