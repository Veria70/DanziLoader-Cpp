// SPDX-License-Identifier: GPL-3.0-or-later
// One log for every binary (`victory-core/src/logging.rs`, with the line
// format of `tracing-subscriber` 0.3.23's default formatter; DanziLoader /
// victory-perf, see NOTICE.md). See log.hpp.
#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

#include "env.hpp"

namespace danzi::log {

namespace {


constexpr Level CAP = Level::Info;

struct Directive {
    std::string target;   
    std::optional<Level> level;   
};

struct State {
    std::once_flag once;
    std::vector<Directive> directives;
    bool ansi = true;
};

State& state() {
    static State s;
    return s;
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return o;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
        --b;
    }
    return std::string(s.substr(a, b - a));
}


bool parse_level(std::string_view text, std::optional<Level>& out) {
    std::string l = lower(text);
    if (l == "trace" || l == "5") {
        out = Level::Trace;
    } else if (l == "debug" || l == "4") {
        out = Level::Debug;
    } else if (l == "info" || l == "3") {
        out = Level::Info;
    } else if (l == "warn" || l == "2") {
        out = Level::Warn;
    } else if (l == "error" || l == "1") {
        out = Level::Error;
    } else if (l == "off" || l == "0") {
        out = std::nullopt;
    } else {
        return false;
    }
    return true;
}


std::optional<std::vector<Directive>> parse_filter(std::string_view text) {
    std::vector<Directive> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t comma = text.find(',', at);
        std::string part = trim(text.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
        at = comma == std::string_view::npos ? text.size() + 1 : comma + 1;
        if (part.empty()) {
            continue;
        }
        if (part.find('[') != std::string::npos) {

            return std::nullopt;
        }
        Directive d;
        if (auto eq = part.find('='); eq != std::string::npos) {
            d.target = trim(part.substr(0, eq));
            if (!parse_level(trim(part.substr(eq + 1)), d.level)) {
                return std::nullopt;
            }
        } else if (std::optional<Level> l; parse_level(part, l)) {
            d.level = l;
        } else {
            d.target = part;
            d.level = Level::Trace;
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void ensure_init() {
    if (state().directives.empty()) {
        init("info");
    }
}


std::string sanitize(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b) {
            out += "\\x1b";
        } else if (c == 0x07) {
            out += "\\x07";
        } else if (c == 0x08) {
            out += "\\x08";
        } else if (c == 0x0c) {
            out += "\\x0c";
        } else if (c == 0x7f) {
            out += "\\x7f";
        } else if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 && static_cast<unsigned char>(s[i + 1]) <= 0x9f) {
            char b[16];
            std::snprintf(b, sizeof b, "\\u{%x}", static_cast<unsigned char>(s[i + 1]));
            out += b;
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}


std::string now() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    std::uint64_t t = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    unsigned micros = static_cast<unsigned>((t / 10) % 1000000);
    char b[40];
    std::snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02u.%06uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, micros);
    return b;
}

constexpr const char* DIM = "\x1b[2m";
constexpr const char* ITALIC = "\x1b[3m";
constexpr const char* RESET = "\x1b[0m";

}   

void init(std::string_view default_level) {
    State& s = state();
    std::call_once(s.once, [&] {
        std::optional<std::vector<Directive>> parsed;
        auto v = env::get(L"VICTORY_LOG");
        if (!v) {
            v = env::get(L"RUST_LOG");
        }
        if (v) {
            parsed = parse_filter(utf8(*v));
        }
        if (!parsed) {
            parsed = parse_filter(default_level);
        }
        s.directives = parsed ? std::move(*parsed) : std::vector<Directive>{};
        if (s.directives.empty()) {

            s.directives.push_back({"", Level::Error});
        }
        auto nc = env::get(L"NO_COLOR");
        s.ansi = !nc || nc->empty();
    });
}

bool enabled(Level level, std::string_view target) {
    if (level < CAP) {
        return false;
    }

    DWORD error = GetLastError();
    ensure_init();
    SetLastError(error);
    const Directive* best = nullptr;
    for (const auto& d : state().directives) {
        if (target.starts_with(d.target) && (!best || d.target.size() >= best->target.size())) {
            best = &d;
        }
    }
    return best && best->level && level >= *best->level;
}

std::string debug(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\0': out += "\\0"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char b[12];
                std::snprintf(b, sizeof b, "\\u{%x}", c);
                out += b;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

namespace {


template <class F>
std::string float_debug(F v) {
    if (v != v) {
        return "NaN";
    }
    if (v == std::numeric_limits<F>::infinity()) {
        return "inf";
    }
    if (v == -std::numeric_limits<F>::infinity()) {
        return "-inf";
    }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    bool negative = sci[0] == '-';
    if (negative) {
        sci.erase(0, 1);
    }
    auto e = sci.find('e');
    std::string digits = sci.substr(0, e);
    int exp = std::stoi(sci.substr(e + 1));
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    std::string out = negative ? "-" : "";
    if (v == 0) {
        return out + "0.0";
    }
    if (exp < -4 || exp >= 16) {
        out += digits.substr(0, 1);
        if (digits.size() > 1) {
            out += "." + digits.substr(1);
        }
        return out + "e" + std::to_string(exp);
    }
    if (exp < 0) {
        return out + "0." + std::string(static_cast<std::size_t>(-exp - 1), '0') + digits;
    }
    auto whole = static_cast<std::size_t>(exp) + 1;
    if (digits.size() <= whole) {
        return out + digits + std::string(whole - digits.size(), '0') + ".0";
    }
    return out + digits.substr(0, whole) + "." + digits.substr(whole);
}

}   

std::string debug(float v) { return float_debug(v); }
std::string debug(double v) { return float_debug(v); }

std::string debug(std::uint64_t v) { return std::to_string(v); }
std::string debug(std::int64_t v) { return std::to_string(v); }
std::string debug(bool v) { return v ? "true" : "false"; }

std::string hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::string hresult(std::int32_t code) {


    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD id = static_cast<DWORD>(code);
    HMODULE module = nullptr;
    if ((id & 0x1000'0000u) != 0) {
        id ^= 0x1000'0000u;
        flags |= FORMAT_MESSAGE_FROM_HMODULE;
        module = LoadLibraryExA("ntdll.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    }
    wchar_t* text = nullptr;
    DWORD n = FormatMessageW(flags, module, id, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message;
    if (text) {
        message.assign(text, n);
        LocalFree(text);
    }
    while (!message.empty() && (message.back() == L' ' || (message.back() >= 9 && message.back() <= 13))) {
        message.pop_back();
    }
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(code));
    return message.empty() ? std::string(hex) : utf8(message) + " (" + hex + ")";
}

std::string format(Level level, std::string_view target, std::string_view message, std::initializer_list<Field> fields, bool ansi) {
    static const char* NAMES[5] = {"TRACE", "DEBUG", " INFO", " WARN", "ERROR"};
    static const char* COLORS[5] = {"\x1b[35m", "\x1b[34m", "\x1b[32m", "\x1b[33m", "\x1b[31m"};
    auto l = static_cast<int>(level);
    std::string out;
    if (ansi) {
        out += std::string(COLORS[l]) + NAMES[l] + RESET + " ";
        out += std::string(DIM) + std::string(target) + RESET + DIM + ":" + RESET + " ";
    } else {
        out += std::string(NAMES[l]) + " " + std::string(target) + ": ";
    }
    bool first = true;
    if (!message.empty()) {
        out += sanitize(message);
        first = false;
    }
    for (const auto& f : fields) {
        if (!first) {
            out += ' ';
        }
        first = false;
        if (f.name == "message") {

            out += sanitize(f.value);
        } else if (ansi) {
            out += std::string(ITALIC) + std::string(f.name) + RESET + DIM + "=" + RESET + f.value;
        } else {
            out += std::string(f.name) + "=" + f.value;
        }
    }
    return out;
}

void write(Level level, std::string_view target, std::string_view message, std::initializer_list<Field> fields) {
    if (!enabled(level, target)) {
        return;
    }
    bool ansi = state().ansi;
    std::string line = ansi ? std::string(DIM) + now() + RESET + " " : now() + " ";
    line += format(level, target, message, fields, ansi);
    line += '\n';
    static std::mutex lock;
    std::lock_guard g(lock);


    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        int n = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), w.data(), n);
        DWORD done = 0;
        WriteConsoleW(h, w.data(), static_cast<DWORD>(w.size()), &done, nullptr);
    } else {
        DWORD done = 0;
        WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &done, nullptr);
    }
}

}   
