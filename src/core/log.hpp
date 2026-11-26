// SPDX-License-Identifier: GPL-3.0-or-later
// One log for every binary (`victory-core/src/logging.rs`; DanziLoader /
// victory-perf, see NOTICE.md): the lines `tracing-subscriber`'s default
// formatter writes to stderr (0.3.23, `fmt::layer().with_target(true)`):
//
//   <time> <LEVEL> <target>: <message> <field>=<value> ...
//
// with ANSI styles unless `NO_COLOR` is set to something. Filter with
// `VICTORY_LOG` (falls back to `RUST_LOG`), e.g.
// `VICTORY_LOG=victory_loader=trace,info`: comma-separated directives, each
// `target=level`, `level` or `target`; the longest matching target wins. A
// plain release build (the reference without its `diagnostics` feature)
// logs info and above whatever the filter asks for.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace danzi::log {

enum class Level { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };


void init(std::string_view default_level);

bool enabled(Level level, std::string_view target);


struct Field {
    std::string_view name;
    std::string value;
};


void write(Level level, std::string_view target, std::string_view message, std::initializer_list<Field> fields = {});


std::string debug(std::string_view s);
inline std::string debug(const char* s) { return debug(std::string_view(s)); }
inline std::string debug(const std::string& s) { return debug(std::string_view(s)); }

std::string debug(float v);
std::string debug(double v);

std::string debug(std::uint64_t v);
std::string debug(std::int64_t v);
inline std::string debug(std::uint32_t v) { return debug(static_cast<std::uint64_t>(v)); }
inline std::string debug(int v) { return debug(static_cast<std::int64_t>(v)); }
std::string debug(bool v);

std::string hex(std::uint64_t v);


std::string hresult(std::int32_t code);


std::string format(Level level, std::string_view target, std::string_view message, std::initializer_list<Field> fields, bool ansi);

inline void info(std::string_view target, std::string_view message, std::initializer_list<Field> fields = {}) { write(Level::Info, target, message, fields); }
inline void warn(std::string_view target, std::string_view message, std::initializer_list<Field> fields = {}) { write(Level::Warn, target, message, fields); }
inline void error(std::string_view target, std::string_view message, std::initializer_list<Field> fields = {}) { write(Level::Error, target, message, fields); }

}   
