// SPDX-License-Identifier: GPL-3.0-or-later
// System dialogs a title opens (XGameUi; `victory-hle/src/modules/
// xgameui.rs`; DanziLoader / victory-perf, see NOTICE.md). A profile card
// has nothing to show and closes at once. Text entry opens a small host
// dialog (title, description, default text, the length limit) on a thread
// of its own and completes the call with what the user typed, or as
// cancelled (`E_ABORT`) when the dialog is dismissed or cannot open.
// Log lines: milestone 7.
#include <array>
#include <cstring>
#include <optional>
#include <string>

#include "core/log.hpp"
#include "modules.hpp"
#include "xasync.hpp"

namespace danzi::hle::xgameui {

namespace {

using xasync::AsyncBlock;
using This = const Object*;

const Guid PROVIDER{0xdfcd4649, 0x4ff8, 0x4043, {0xba, 0x07, 0x35, 0xd6, 0x07, 0xdf, 0x98, 0xb0}};


const Guid IID_2026{0xeaf669df, 0x5542, 0x4590, {0x99, 0xa3, 0x8d, 0xc0, 0x61, 0xf8, 0x37, 0xcc}};

std::uint8_t PROFILE = 0;
std::uint8_t TEXT = 0;


std::string limit(const std::string& typed, std::uint32_t max) {
    if (max == 0) {
        return typed;
    }
    std::size_t i = 0, chars = 0;
    while (i < typed.size() && chars < max) {
        auto b = static_cast<unsigned char>(typed[i]);
        i += b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        ++chars;
    }
    return typed.substr(0, std::min(i, typed.size()));
}

namespace dialog {

constexpr int EDIT = 100;
constexpr UINT EM_SETSEL_ = 0xb1;
constexpr UINT EM_LIMITTEXT_ = 0xc5;


struct Entry {
    std::uint32_t max;
    std::optional<std::string> typed;
};

INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    HWND edit = GetDlgItem(dialog, EDIT);
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lparam);
        auto* entry = reinterpret_cast<Entry*>(lparam);
        SendMessageW(edit, EM_LIMITTEXT_, entry->max, 0);
        SendMessageW(edit, EM_SETSEL_, 0, -1);
        SetFocus(edit);
        return 0;   
    }
    case WM_COMMAND: {
        auto* entry = reinterpret_cast<Entry*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        switch (static_cast<int>(wparam & 0xffff)) {
        case IDOK: {
            std::wstring buffer(static_cast<std::size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
            int n = GetWindowTextW(edit, buffer.data(), static_cast<int>(buffer.size()));
            buffer.resize(static_cast<std::size_t>(std::max(n, 0)));
            entry->typed = xgr::utf16_lossy(buffer);
            EndDialog(dialog, 1);
            break;
        }
        case IDCANCEL:
            EndDialog(dialog, 0);
            break;
        default:
            return 0;
        }
        return 1;
    }
    default:
        return 0;
    }
}


std::optional<std::string> ask(const std::string& title, const std::string& description, const std::string& def, std::uint32_t max) {
    std::vector<std::uint16_t> t;
    auto dword = [&](std::uint32_t v) {
        t.push_back(static_cast<std::uint16_t>(v));
        t.push_back(static_cast<std::uint16_t>(v >> 16));
    };
    auto text = [&](const std::string& s) {
        for (wchar_t c : xgr::utf16(s)) {
            if (c != 0) {
                t.push_back(c);
            }
        }
        t.push_back(0);
    };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD flags = DS_MODALFRAME | DS_CENTER | DS_SETFOREGROUND;
    dword(style | flags);
    dword(WS_EX_TOPMOST);
    for (std::uint16_t v : {4, 0, 0, 240, 62, 0, 0}) {   
        t.push_back(v);
    }
    text(title);
    auto item = [&](DWORD st, std::array<std::uint16_t, 4> rect, std::uint16_t id, std::uint16_t cls, const std::string& caption) {
        if (t.size() % 2 == 1) {
            t.push_back(0);   
        }
        dword(WS_CHILD | WS_VISIBLE | st);
        dword(0);
        t.insert(t.end(), rect.begin(), rect.end());
        t.push_back(id);
        t.push_back(0xffff);
        t.push_back(cls);
        text(caption);
        t.push_back(0);   
    };
    const std::uint16_t button = 0x80, edit = 0x81, label = 0x82;
    item(0, {7, 7, 226, 10}, 0xffff, label, description);
    item(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, {7, 20, 226, 12}, EDIT, edit, def);
    item(BS_DEFPUSHBUTTON | WS_TABSTOP, {129, 40, 50, 14}, IDOK, button, "OK");
    item(BS_PUSHBUTTON | WS_TABSTOP, {183, 40, 50, 14}, IDCANCEL, button, "Cancel");

    std::vector<std::uint32_t> tmpl((t.size() + 1) / 2);
    std::memcpy(tmpl.data(), t.data(), t.size() * 2);
    Entry entry{max, std::nullopt};
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<const DLGTEMPLATE*>(tmpl.data()), nullptr, &procedure, reinterpret_cast<LPARAM>(&entry));
    return entry.typed;
}

}   

HRESULT_ __stdcall profile_card_async(This, AsyncBlock* block, const void*, std::uint64_t xuid) {
    log::info("victory_hle::modules::xgameui", "profile card: nothing to show", {{"xuid", log::debug(xuid)}});
    return xasync::finished(block, &PROFILE, S_OK_);
}
HRESULT_ __stdcall profile_card_result(This, AsyncBlock* block) { return xasync::XAsyncGetResult(block, &PROFILE, 0, nullptr, nullptr); }


HRESULT_ __stdcall text_entry_async(This, AsyncBlock* block, const char* title, const char* description, const char* def, std::uint32_t, std::uint32_t max_length) {
    std::string t = xgr::text(title).value_or(""), d = xgr::text(description).value_or(""), f = xgr::text(def).value_or("");
    log::info("victory_hle::modules::xgameui", "text entry", {{"title", log::debug(t)}, {"description", log::debug(d)}, {"max_length", log::debug(max_length)}});
    return xasync::pending_with(block, &TEXT, [t, d, f, max_length]() -> xasync::Result {
        auto typed = dialog::ask(t, d, f, max_length);
        if (!typed) {
            log::info("victory_hle::modules::xgameui", "text entry: cancelled");
            return E_ABORT_;
        }
        {
            std::uint64_t chars = 0;
            for (unsigned char c : *typed) {
                chars += (c & 0xC0) != 0x80;
            }
            log::info("victory_hle::modules::xgameui", "text entry: done", {{"length", log::debug(chars)}});
        }
        std::string s = limit(*typed, max_length);
        std::vector<std::uint8_t> bytes(s.begin(), s.end());
        bytes.push_back(0);
        return bytes;
    });
}

HRESULT_ __stdcall text_entry_size(This, AsyncBlock* block, std::uint32_t* out) {
    std::size_t size = 0;
    HRESULT_ hr = xasync::XAsyncGetResultSize(block, &size);
    if (hr < 0) {
        return hr;
    }
    return xgr::put(out, static_cast<std::uint32_t>(size));
}
HRESULT_ __stdcall text_entry_result(This, AsyncBlock* block, std::uint32_t size, char* buffer, std::uint32_t* used) {
    std::size_t written = 0;
    HRESULT_ hr = xasync::XAsyncGetResult(block, &TEXT, size, buffer, &written);
    if (hr < 0 || !used) {
        return hr;
    }
    return xgr::put(used, static_cast<std::uint32_t>(written));
}

#define F(x) reinterpret_cast<const void*>(&x)
using xgr::add_ref;
using xgr::missing;
const void* const VTABLE[20] = {
    F(missing<0>),  F(add_ref),       F(add_ref),         F(missing<3>),        F(missing<4>),
    F(missing<5>),  F(missing<6>),    F(profile_card_async), F(profile_card_result), F(missing<9>),
    F(missing<10>), F(missing<11>),   F(missing<12>),     F(missing<13>),       F(missing<14>),
    F(missing<15>), F(missing<16>),   F(text_entry_async), F(text_entry_size),   F(text_entry_result),
};
#undef F
const Object OBJECT{VTABLE, "game ui"};

}   

std::vector<Interface> interfaces() { return {{PROVIDER, IID_2026, &OBJECT}}; }

}   
