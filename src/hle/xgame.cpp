// SPDX-License-Identifier: GPL-3.0-or-later
// The title's own id, which runtime areas exist, game invites and events
// (`victory-hle/src/modules/xgame.rs`; DanziLoader / victory-perf, see
// NOTICE.md). Offline no invite arrives and events go nowhere.
#include "areas.hpp"

#include <shellapi.h>

#include <algorithm>
#include "modules.hpp"
#include "runtime.hpp"

#pragma comment(lib, "shell32.lib")

namespace danzi::hle::xgame {

namespace {

const Guid TITLE_PROVIDER{0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};
const Guid TITLE_IID{0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}};
const Guid FEATURES{0x8836fe87, 0xedb9, 0x4fe3, {0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40}};
const Guid INVITES{0x0651aae2, 0x4012, 0x4077, {0xbf, 0x84, 0x8b, 0x90, 0x97, 0x09, 0x0e, 0x2c}};
const Guid EVENTS{0xbbfbdcc7, 0xbfe7, 0x409b, {0xa5, 0xca, 0xed, 0xf0, 0x54, 0x96, 0x0b, 0x4d}};

using This = const Object*;

HRESULT_ __stdcall title_id(This, std::uint32_t* out) { return xgr::put(out, runtime::title().id); }


void __stdcall launch_new_game(This, const char* exe, const char* args, const void*) {
    std::string e = xgr::text(exe).value_or(""), a = xgr::text(args).value_or("");
    log::info("victory_hle::modules::xgame", "XLaunchNewGame", {{"exe", log::debug(e)}, {"args", log::debug(a)}});
    std::wstring current;
    if (auto m = runtime::main_image()) {
        if (auto p = runtime::image_path(*m)) {
            current = p->wstring();
        }
    }
    std::vector<std::wstring> ours;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            ours.emplace_back(argv[i]);
        }
        LocalFree(argv);
    }
    auto command = relaunch_args(std::move(ours), current, e, a);
    lifecycle::end_title("XLaunchNewGame", [command] { lifecycle::relaunch(command); });
    for (;;) {
        Sleep(INFINITE);
    }
}


constexpr std::uint32_t AVAILABLE[20] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20};

bool __stdcall is_feature_available(This, std::uint32_t feature) { return std::find(std::begin(AVAILABLE), std::end(AVAILABLE), feature) != std::end(AVAILABLE); }


HRESULT_ __stdcall reg(This, const void*, void*, const void*, std::uint64_t* out) { return xgr::put(out, xgr::token()); }
bool __stdcall unreg(This, std::uint64_t, bool) { return true; }

HRESULT_ __stdcall write_event(This, const void*, const char*, const char*, const char*, const char*, const char*) { return S_OK_; }

#define F(x) reinterpret_cast<const void*>(&x)
using xgr::add_ref;
using xgr::missing;
const void* const TITLE_VTABLE[5] = {F(missing<0>), F(add_ref), F(add_ref), F(title_id), F(launch_new_game)};
const void* const FEATURES_VTABLE[4] = {F(missing<0>), F(add_ref), F(add_ref), F(is_feature_available)};
const void* const INVITES_VTABLE[5] = {F(missing<0>), F(add_ref), F(add_ref), F(reg), F(unreg)};
const void* const EVENTS_VTABLE[4] = {F(missing<0>), F(add_ref), F(add_ref), F(write_event)};
#undef F
const Object TITLE_OBJECT{TITLE_VTABLE, "title"};
const Object FEATURES_OBJECT{FEATURES_VTABLE, "features"};
const Object INVITES_OBJECT{INVITES_VTABLE, "invites"};
const Object EVENTS_OBJECT{EVENTS_VTABLE, "events"};

}   


std::vector<std::wstring> relaunch_args(std::vector<std::wstring> ours, const std::wstring& current, const std::string& exe, const std::string& args) {
    std::vector<std::wstring> out;
    for (auto& a : ours) {
        if (a == L"--") {
            break;
        }
        out.push_back(std::move(a));
    }
    auto widen = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    };
    if (!exe.empty()) {
        for (auto& a : out) {
            if (a == current) {
                a = widen(exe);
                break;
            }
        }
    }
    bool blank = std::all_of(args.begin(), args.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; });
    if (!blank) {
        out.emplace_back(L"--");
        std::size_t i = 0;
        while (i < args.size()) {
            while (i < args.size() && std::isspace(static_cast<unsigned char>(args[i]))) {
                ++i;
            }
            std::size_t j = i;
            while (j < args.size() && !std::isspace(static_cast<unsigned char>(args[j]))) {
                ++j;
            }
            if (j > i) {
                out.push_back(widen(args.substr(i, j - i)));
            }
            i = j;
        }
    }
    return out;
}

std::vector<Interface> interfaces() {
    return {{TITLE_PROVIDER, TITLE_IID, &TITLE_OBJECT}, {FEATURES, FEATURES, &FEATURES_OBJECT}, {INVITES, INVITES, &INVITES_OBJECT}, {EVENTS, EVENTS, &EVENTS_OBJECT}};
}

}   
