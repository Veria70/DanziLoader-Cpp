// SPDX-License-Identifier: GPL-3.0-or-later
// The crash log (`victory-loader/src/crash.rs`; DanziLoader / victory-perf,
// see NOTICE.md): reports of what ended the process, appended to a file so
// they can be read after the console is gone, with the title's last debug
// output (which often states the reason).
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <vector>
#include <string>
#include <string_view>

namespace danzi::loader::crash {


void log_crashes_to(const std::filesystem::path& file);


void log_crash(std::string_view text);

void note_title_output(std::string_view line);


std::string title_output();


void report_crashes();


void report_unhandled(const EXCEPTION_POINTERS* info);

std::string locate(std::uintptr_t address);


void unwind_into(const CONTEXT& context, std::uintptr_t low, std::uintptr_t high, std::vector<std::uint64_t>& frames);

}   
