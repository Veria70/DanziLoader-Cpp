// SPDX-License-Identifier: GPL-3.0-or-later
// The mouse cursor while the title window has focus
// (`victory-hle/src/modules/gameinput.rs` `cursor`; DanziLoader /
// victory-perf, see NOTICE.md): captured (hidden, held inside the window,
// its motion turns the camera) or free. The host poll and the title window's
// procedure (`lifecycle`) keep it. Log lines: milestone 7.
#include <atomic>

#include "modules.hpp"

namespace danzi::hle::gameinput::cursor {

std::atomic<bool> ENABLED{false};
std::atomic<bool> CAPTURED{false};
std::atomic<bool> FOCUSED{false};


std::atomic<std::uintptr_t> WINDOW{0};

bool captured() { return CAPTURED.load(std::memory_order_relaxed); }

void capture(const char*) {
    if (ENABLED.load(std::memory_order_relaxed)) {
        CAPTURED.exchange(true, std::memory_order_relaxed);
    }
}

void release(const char*) { CAPTURED.exchange(false, std::memory_order_relaxed); }


void click() { capture("click"); }

bool ours(std::uintptr_t window) {
    DWORD process = 0;
    GetWindowThreadProcessId(reinterpret_cast<HWND>(window), &process);
    return process == GetCurrentProcessId();
}


void note_window(std::uintptr_t window) {
    if (window != 0 && !GetParent(reinterpret_cast<HWND>(window))) {
        WINDOW.store(window, std::memory_order_relaxed);
    }
}


void focus_moved(std::uintptr_t to, const char* event) {
    if (to != 0 && !ours(to)) {
        release(event);
    }
}


bool clip(std::uintptr_t window) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    RECT client{};
    if (!GetClientRect(hwnd, &client)) {
        return false;
    }
    POINT a{client.left, client.top}, b{client.right, client.bottom};
    ClientToScreen(hwnd, &a);
    ClientToScreen(hwnd, &b);
    if (b.x > a.x && b.y > a.y) {
        RECT rect{a.x, a.y, b.x, b.y};
        ClipCursor(&rect);
        return true;
    }
    return false;
}


std::uintptr_t target() {
    std::uintptr_t window = WINDOW.load(std::memory_order_relaxed);
    if (window != 0 && IsWindow(reinterpret_cast<HWND>(window))) {
        return window;
    }
    return reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
}

}   
