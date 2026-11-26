// SPDX-License-Identifier: GPL-3.0-or-later
// The title's life cycle (`victory-hle/src/modules/lifecycle.rs`;
// DanziLoader / victory-perf, see NOTICE.md). A console title is never told
// it is closing: quitting it or launching a new game first suspends it (its
// suspend routines run, and it has a second to save) and then ends the
// process. Closing the title's window does the same here; a new launch then
// starts the launcher again, which waits for this process to be gone.
// Log lines: milestone 7.
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/log.hpp"
#include "d3d12x/pipeline.hpp"
#include "modules.hpp"

namespace danzi::hle::lifecycle {

namespace {

using Routine = void(WINAPI*)(std::uint8_t quiesced, void* context);
struct Registration {
    std::size_t id;
    Routine routine;
    void* context;
};
std::mutex ROUTINES_LOCK;
std::vector<Registration> ROUTINES;


constexpr DWORD SUSPEND_DEADLINE_MS = 1000;
constexpr DWORD ERROR_INVALID_PARAMETER_ = 87;


constexpr const wchar_t* TITLE_PROC = L"victory.wndproc";

DWORD WINAPI RegisterAppStateChangeNotification(Routine routine, void* context, std::size_t* registration) {
    if (!routine || !registration) {
        return ERROR_INVALID_PARAMETER_;
    }
    static std::atomic<std::size_t> next{1};
    std::size_t id = next.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard l(ROUTINES_LOCK);
        ROUTINES.push_back({id, routine, context});
    }
    *registration = id;
    return 0;
}

void WINAPI UnregisterAppStateChangeNotification(std::size_t registration) {
    std::lock_guard l(ROUTINES_LOCK);
    std::erase_if(ROUTINES, [&](const Registration& r) { return r.id == registration; });
}


void fit(HWND parent, int& x, int& y, int& width, int& height) {
    RECT area{};
    BOOL known = SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    auto [w, h] = fit_into(area, width, height);
    if (parent || !known || (w == width && h == height)) {
        return;
    }
    log::info("victory_hle::modules::lifecycle", "title window fitted to the screen",
              {{"width", log::debug(width)}, {"height", log::debug(height)}, {"to", "(" + std::to_string(w) + ", " + std::to_string(h) + ")"}});
    x = area.left + (area.right - area.left - w) / 2;
    y = area.top + (area.bottom - area.top - h) / 2;
    width = w;
    height = h;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);


void hook(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    LONG_PTR title = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    SetPropW(hwnd, TITLE_PROC, reinterpret_cast<HANDLE>(title));
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc));
}

HWND WINAPI CreateWindowExW_(DWORD ex_style, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance, void* param) {
    fit(parent, x, y, width, height);
    HWND hwnd = ::CreateWindowExW(ex_style, cls, name, style, x, y, width, height, parent, menu, instance, param);
    hook(hwnd);
    return hwnd;
}

HWND WINAPI CreateWindowExA_(DWORD ex_style, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance, void* param) {
    fit(parent, x, y, width, height);
    HWND hwnd = ::CreateWindowExA(ex_style, cls, name, style, x, y, width, height, parent, menu, instance, param);
    hook(hwnd);
    return hwnd;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    namespace cursor = gameinput::cursor;


    if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_CLOSE) {
        end_title("window closed", [] {});
        return 0;
    }


    if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_KEYMENU) {
        return 0;
    }


    auto self = reinterpret_cast<std::uintptr_t>(hwnd);
    switch (message) {
    case WM_SETCURSOR:
        if (cursor::captured() && (static_cast<std::uint32_t>(lparam) & 0xFFFF) == HTCLIENT) {
            SetCursor(nullptr);
            return 1;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        cursor::note_window(self);
        cursor::click();
        break;
    case WM_SETFOCUS:
        cursor::note_window(self);
        break;
    case WM_KILLFOCUS:
        cursor::focus_moved(wparam, "WM_KILLFOCUS");
        break;
    case WM_ACTIVATE:
        if ((wparam & 0xFFFF) == WA_INACTIVE) {
            cursor::focus_moved(static_cast<std::uintptr_t>(lparam), "WM_ACTIVATE");
        } else {
            cursor::note_window(self);
        }
        if (cursor::captured()) {
            cursor::clip(cursor::target());
        }
        break;
    case WM_MOVE:
    case WM_SIZE:
    case WM_WINDOWPOSCHANGED:
        if (cursor::captured()) {
            cursor::clip(cursor::target());
        }
        break;
    default:
        break;
    }
    HANDLE title = GetPropW(hwnd, TITLE_PROC);
    if (message == WM_NCDESTROY) {
        RemovePropW(hwnd, TITLE_PROC);
    }
    return CallWindowProcW(reinterpret_cast<WNDPROC>(title), hwnd, message, wparam, lparam);
}

void end() {

    d3d12x::save_pipeline_cache();

    TerminateProcess(GetCurrentProcess(), 0);
}


constexpr const wchar_t* RELAUNCHED_FROM = L"VICTORY_RELAUNCHED_FROM";


void append_arg(std::wstring& line, const std::wstring& arg) {
    if (!line.empty()) {
        line += L' ';
    }
    bool quote = arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!quote) {
        line += arg;
        return;
    }
    line += L'"';
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else {
            if (c == L'"') {
                line.append(backslashes + 1, L'\\');
            }
            backslashes = 0;
        }
        line += c;
    }
    line.append(backslashes, L'\\');
    line += L'"';
}

}   

std::pair<int, int> fit_into(const RECT& area, int width, int height) {
    int aw = area.right - area.left, ah = area.bottom - area.top;
    if (width <= 0 || height <= 0 || aw <= 0 || ah <= 0 || (width <= aw && height <= ah)) {
        return {width, height};
    }
    double scale = std::min(static_cast<double>(aw) / width, static_cast<double>(ah) / height);
    return {static_cast<int>(width * scale), static_cast<int>(height * scale)};
}

void end_title(const char* why, std::function<void()> then) {
    static std::atomic<bool> ending{false};
    if (ending.exchange(true)) {
        return;
    }
    std::vector<Registration> suspend;
    {
        std::lock_guard l(ROUTINES_LOCK);
        suspend = ROUTINES;
    }
    log::info("victory_hle::modules::lifecycle", std::string(why) + ": suspending the title", {{"routines", log::debug(static_cast<std::uint64_t>(suspend.size()))}});


    struct Shared {
        std::mutex lock;
        std::function<void()> then;
    };
    auto shared = std::make_shared<Shared>();
    shared->then = std::move(then);
    auto finish = [shared](const char* how) {
        std::lock_guard l(shared->lock);
        log::info("victory_hle::modules::lifecycle", std::string(how) + "; ending the title");
        if (auto t = std::exchange(shared->then, nullptr)) {
            t();
        }
        end();
    };


    std::thread([suspend, finish] {
        for (auto& r : suspend) {
            r.routine(1, r.context);
        }
        finish("title suspended");
    }).detach();
    std::thread([finish] {
        Sleep(SUSPEND_DEADLINE_MS);
        finish("title did not finish suspending in time");
    }).detach();
}

void relaunch(std::vector<std::wstring> args) {
    HANDLE me = nullptr;
    bool duplicated = DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &me, 0, TRUE, DUPLICATE_SAME_ACCESS) != 0;
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n == 0 || n >= std::size(exe)) {
        return;
    }
    std::wstring line;
    append_arg(line, exe);
    for (auto& a : args) {
        append_arg(line, a);
    }
    if (duplicated) {
        SetEnvironmentVariableW(RELAUNCHED_FROM, std::to_wstring(reinterpret_cast<std::uintptr_t>(me)).c_str());
    }
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};


    BOOL spawned = CreateProcessW(exe, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    DWORD spawn_error = spawned ? 0 : GetLastError();
    SetEnvironmentVariableW(RELAUNCHED_FROM, nullptr);
    if (spawned) {
        std::string list = "[";
        for (std::size_t i = 0; i < args.size(); ++i) {
            list += (i ? ", " : "") + log::debug(xgr::utf16_lossy(args[i]));
        }
        log::info("victory_hle::modules::lifecycle", "relaunched", {{"pid", log::debug(static_cast<std::uint32_t>(pi.dwProcessId))}, {"args", list + "]"}});
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        DWORD e = spawn_error;
        log::warn("victory_hle::modules::lifecycle", "relaunch failed; the title just ends", {{"e", std::system_category().message(static_cast<int>(e)) + " (os error " + std::to_string(e) + ")"}});
    }
}

void wait_for_previous() {
    wchar_t text[32];
    DWORD n = GetEnvironmentVariableW(RELAUNCHED_FROM, text, 32);
    if (n == 0 || n >= 32) {
        return;
    }
    wchar_t* endp = nullptr;
    unsigned long long h = wcstoull(text, &endp, 10);

    SetEnvironmentVariableW(RELAUNCHED_FROM, nullptr);
    if (endp == text || *endp != 0) {
        return;
    }
    HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(h));
    WaitForSingleObject(handle, 30000);
    CloseHandle(handle);
}

std::vector<HleExport> exports() {
    return {{"RegisterAppStateChangeNotification", reinterpret_cast<std::uintptr_t>(&RegisterAppStateChangeNotification)},
            {"UnregisterAppStateChangeNotification", reinterpret_cast<std::uintptr_t>(&UnregisterAppStateChangeNotification)},
            {"CreateWindowExW", reinterpret_cast<std::uintptr_t>(&CreateWindowExW_)},
            {"CreateWindowExA", reinterpret_cast<std::uintptr_t>(&CreateWindowExA_)}};
}

}   
