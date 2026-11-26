// SPDX-License-Identifier: GPL-3.0-or-later
// What the runtime's modules offer one another (`victory-hle/src/modules/`;
// DanziLoader / victory-perf, see NOTICE.md).
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "registry.hpp"
#include "xgameruntime.hpp"

namespace danzi::hle {


namespace xuser {
std::vector<Interface> interfaces();

constexpr std::uint64_t XUID = 0x0009000000000001ull;
}   
namespace xgame {
std::vector<Interface> interfaces();

std::vector<std::wstring> relaunch_args(std::vector<std::wstring> ours, const std::wstring& current, const std::string& exe, const std::string& args);
}   
namespace storage {
std::vector<Interface> interfaces();

std::uint64_t disk_free(const std::filesystem::path& dir);

std::uint64_t used_bytes(const std::filesystem::path& dir);
}   
namespace xnetworking { std::vector<Interface> interfaces(); }
namespace xpackage {
std::vector<Interface> interfaces();

std::string identifier();
constexpr HRESULT_ NO_PACKAGE = static_cast<HRESULT_>(0x89245208u);
}   
namespace display { std::vector<Interface> interfaces(); }
namespace xgamesave { std::vector<Interface> interfaces(); }
namespace xstore { std::vector<Interface> interfaces(); }
namespace xgameui { std::vector<Interface> interfaces(); }
namespace speech { std::vector<Interface> interfaces(); }


void kernel_will_write(std::uintptr_t at, std::size_t len);


void title_memory_remapped(std::uintptr_t at, std::size_t len);

namespace xmem {
std::vector<HleExport> exports();

void* WINAPI XMemAlloc(std::size_t size, std::uint64_t attributes);
void WINAPI XMemFree(void* address, std::uint64_t attributes);

bool on_wine();


bool pages_known(std::span<const std::size_t> pages, std::size_t offset, std::size_t len);


bool write_physical(std::span<const std::size_t> pages, std::size_t offset, std::span<const std::uint8_t> data);

class PageWindow {
public:
    struct State;
    explicit PageWindow(std::unique_ptr<State> state);
    PageWindow(PageWindow&&) noexcept;
    ~PageWindow();
    std::span<std::uint8_t> bytes();

private:
    std::unique_ptr<State> state_;
};


std::optional<PageWindow> page_window(std::span<const std::size_t> pages, std::size_t offset, std::size_t len);


bool map_alias(std::uint64_t to, std::uint64_t from, std::uint64_t len);
}   

namespace files {
std::vector<HleExport> exports();


HANDLE WINAPI CreateFileW(const wchar_t* path, DWORD access, DWORD share, const void* security, DWORD disposition, DWORD flags, HANDLE tmpl);


std::optional<wchar_t> mount(const std::filesystem::path& root);


void mount_at(wchar_t letter, const std::filesystem::path& root);

std::optional<std::wstring> translate(std::wstring_view path);

std::wstring verbatim(std::wstring_view root, std::wstring_view tail);
}   


namespace net { std::vector<HleExport> exports(); }

namespace media { std::vector<HleExport> exports(); }


namespace audio {
std::vector<HleExport> exports();


namespace capture {
void start();
bool child();
}   
}   
namespace apu { std::vector<HleExport> exports(); }
namespace dsp { std::vector<HleExport> exports(); }
namespace xg { std::vector<HleExport> exports(); }


namespace d3d12x_module {
std::vector<HleExport> exports();


void use_shader_cache(const std::filesystem::path& dir);
}   

namespace dstorage {
std::vector<HleExport> exports();
}

namespace lifecycle {
std::vector<HleExport> exports();


void end_title(const char* why, std::function<void()> then);


void relaunch(std::vector<std::wstring> args);


void wait_for_previous();

std::pair<int, int> fit_into(const RECT& area, int width, int height);
}   

namespace gameinput::cursor {


bool captured();
void note_window(std::uintptr_t hwnd);
void click();
void focus_moved(std::uintptr_t to, const char* why);
bool ours(std::uintptr_t window);
void capture(const char* why);
void release(const char* why);
bool clip(std::uintptr_t hwnd);
std::uintptr_t target();


extern std::atomic<bool> ENABLED;
extern std::atomic<bool> FOCUSED;
extern std::atomic<std::uintptr_t> WINDOW;
}   

namespace gameinput {
std::vector<HleExport> exports();
}   

}   
