// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboard and mouse played the way PC games are (`victory-core/src/kbm.rs`;
// DanziLoader / victory-perf, see NOTICE.md). See kbm.hpp. The debug log
// line (`Look::take`) is not ported (a release build logs info and above).
#include "kbm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <system_error>

#include "env.hpp"
#include "log.hpp"
#include "rust_text.hpp"
#include "win.hpp"

namespace danzi::kbm {

namespace {

constexpr const char* TARGET = "victory_core::kbm";


constexpr double FULL_RATE = 2000.0;

constexpr std::uint64_t MIN_TAKE_US = 2'000;


constexpr std::uint64_t FIRST_TAKE_US = 16'667;
constexpr std::uint64_t MAX_TAKE_US = 1'000'000;

constexpr double CARRY_S = 0.25;


constexpr float PAD_TAKES_OVER = 0.2f;

constexpr std::size_t WHEEL_QUEUE = 8;


float fmax_rust(float a, float b) {
    if (std::isnan(a)) {
        return b;
    }
    if (std::isnan(b)) {
        return a;
    }
    return a > b ? a : b;
}

float hypot32(float x, float y) { return std::hypot(x, y); }

bool pushed(float x, float y) { return hypot32(x, y) > PAD_TAKES_OVER; }

bool ascii_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
char ascii_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }
char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool eq_ignore_ascii_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string to_ascii_lowercase(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = ascii_lower(c);
    }
    return out;
}

bool is_char_boundary(std::string_view s, std::size_t at) {
    return at == s.size() || (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) != 0x80);
}

using Kind = Output::Kind;

struct Named {
    const char* name;
    Output output;
};

constexpr Named OUTPUTS[27] = {
    {"A", Output::of_button(A)},
    {"B", Output::of_button(B)},
    {"X", Output::of_button(X)},
    {"Y", Output::of_button(Y)},
    {"LB", Output::of_button(LEFT_SHOULDER)},
    {"RB", Output::of_button(RIGHT_SHOULDER)},
    {"LT", Output::of(Kind::LeftTrigger)},
    {"RT", Output::of(Kind::RightTrigger)},
    {"LS", Output::of_button(LEFT_STICK)},
    {"RS", Output::of_button(RIGHT_STICK)},
    {"View", Output::of_button(VIEW)},
    {"Menu", Output::of_button(MENU)},
    {"DpadUp", Output::of_button(UP)},
    {"DpadDown", Output::of_button(DOWN)},
    {"DpadLeft", Output::of_button(LEFT)},
    {"DpadRight", Output::of_button(RIGHT)},
    {"LeftUp", Output::stick(false, false, true)},
    {"LeftDown", Output::stick(false, false, false)},
    {"LeftLeft", Output::stick(false, true, false)},
    {"LeftRight", Output::stick(false, true, true)},
    {"RightUp", Output::stick(true, false, true)},
    {"RightDown", Output::stick(true, false, false)},
    {"RightLeft", Output::stick(true, true, false)},
    {"RightRight", Output::stick(true, true, true)},
    {"Walk", Output::of(Kind::Walk)},
    {"ToggleVehicle", Output::of(Kind::ToggleVehicle)},
    {"ReleaseCursor", Output::of(Kind::ReleaseCursor)},
};

struct Key {
    const char* name;
    std::uint8_t vk;
};


constexpr Key KEYS[48] = {
    {"MouseLeft", 0x01},   {"MouseRight", 0x02}, {"MouseMiddle", 0x04}, {"Mouse4", 0x05},     {"Mouse5", 0x06},
    {"WheelUp", WHEEL_UP}, {"WheelDown", WHEEL_DOWN}, {"Backspace", 0x08}, {"Tab", 0x09},     {"Enter", 0x0D},
    {"Pause", 0x13},       {"CapsLock", 0x14},   {"Esc", 0x1B},         {"Space", 0x20},      {"PageUp", 0x21},
    {"PageDown", 0x22},    {"End", 0x23},        {"Home", 0x24},        {"Left", 0x25},       {"Up", 0x26},
    {"Right", 0x27},       {"Down", 0x28},       {"Insert", 0x2D},      {"Delete", 0x2E},     {"NumMultiply", 0x6A},
    {"NumAdd", 0x6B},      {"NumSubtract", 0x6D}, {"NumDecimal", 0x6E}, {"NumDivide", 0x6F},  {"LShift", 0xA0},
    {"RShift", 0xA1},      {"LCtrl", 0xA2},      {"RCtrl", 0xA3},       {"LAlt", 0xA4},       {"RAlt", 0xA5},
    {"Semicolon", 0xBA},   {"Equals", 0xBB},     {"Comma", 0xBC},       {"Minus", 0xBD},      {"Period", 0xBE},
    {"Slash", 0xBF},       {"Grave", 0xC0},      {"LBracket", 0xDB},    {"Backslash", 0xDC},  {"RBracket", 0xDD},
    {"Quote", 0xDE},       {"Shift", 0xA0},      {"Ctrl", 0xA2},
};

std::optional<Output> output(std::string_view name) {
    auto n = rust_text::trim(name);
    for (const auto& o : OUTPUTS) {
        if (eq_ignore_ascii_case(o.name, n)) {
            return o.output;
        }
    }
    return std::nullopt;
}

struct Default {
    const char* key;
    const char* output;
};


constexpr Default COMMON[] = {
    {"A", "LeftLeft"},   {"D", "LeftRight"},        {"LShift", "A"},           {"RShift", "A"},
    {"Space", "X"},      {"E", "Y"},                {"F", "Y"},                {"Q", "LB"},
    {"R", "RB"},         {"LCtrl", "LS"},           {"RCtrl", "LS"},           {"C", "RS"},
    {"Tab", "View"},     {"P", "Menu"},             {"Z", "Walk"},             {"G", "ToggleVehicle"},
    {"LAlt", "ReleaseCursor"}, {"RAlt", "ReleaseCursor"}, {"Up", "DpadUp"},  {"Down", "DpadDown"},
    {"Left", "DpadLeft"}, {"Right", "DpadRight"},   {"WheelUp", "DpadUp"},     {"WheelDown", "DpadDown"},
    {"MouseLeft", "RT"}, {"MouseRight", "LT"},      {"MouseMiddle", "RS"},     {"Enter", "A"},
    {"Backspace", "B"},  {"Esc", "B"},
};

constexpr Default ON_FOOT[] = {{"W", "LeftUp"}, {"S", "LeftDown"}};

constexpr Default VEHICLE[] = {{"W", "RT"}, {"S", "LT"}};

Layout default_layout(std::span<const Default> own) {
    Layout layout;
    for (const auto& d : COMMON) {
        layout.bind(d.key, {d.output});
    }
    for (const auto& d : own) {
        layout.bind(d.key, {d.output});
    }
    return layout;
}


struct DeError {
    std::string message;
    std::vector<std::string> keys;

    std::string display() const {
        std::string out = message + "\n";
        if (!keys.empty()) {
            out += "in `";
            for (std::size_t i = 0; i < keys.size(); ++i) {
                out += (i ? "." : "") + keys[i];
            }
            out += "`\n";
        }
        return out;
    }
};


std::string unexpected(const toml::Value& v) {
    if (auto b = v.as_bool()) {
        return std::string("boolean `") + (*b ? "true" : "false") + "`";
    }
    if (auto i = v.as_integer()) {
        return "integer `" + std::to_string(*i) + "`";
    }
    if (auto f = v.as_float()) {

        std::string s = rust_text::display(*f);
        if (std::isfinite(*f) && s.find('.') == std::string::npos) {
            s += ".0";
        }
        return "floating point `" + s + "`";
    }
    if (auto s = v.as_string()) {
        return "string " + rust_text::debug_str(*s);
    }
    if (auto d = std::get_if<toml::Datetime>(&v.v)) {
        return "string " + rust_text::debug_str(d->text);
    }
    if (v.is_array()) {
        return "sequence";
    }
    return "map";
}

DeError invalid_type(const toml::Value& v, const char* expected) {
    return {"invalid type: " + unexpected(v) + ", expected " + expected, {}};
}

std::optional<DeError> de_bool(const toml::Value& v, std::optional<bool>& into) {
    if (auto b = v.as_bool()) {
        into = *b;
        return std::nullopt;
    }
    return invalid_type(v, "a boolean");
}


std::optional<DeError> de_f32(const toml::Value& v, std::optional<float>& into) {
    if (auto i = v.as_integer()) {
        into = static_cast<float>(*i);
        return std::nullopt;
    }
    if (auto f = v.as_float()) {
        into = std::copysign(static_cast<float>(*f), std::signbit(*f) ? -1.0f : 1.0f);
        return std::nullopt;
    }
    return invalid_type(v, "f32");
}


const std::string* as_text(const toml::Value& v) {
    if (auto s = v.as_string()) {
        return s;
    }
    if (auto d = std::get_if<toml::Datetime>(&v.v)) {
        return &d->text;
    }
    return nullptr;
}

using Binds = std::vector<std::pair<std::string, std::vector<std::string>>>;


std::optional<DeError> de_binds(const toml::Value& v, Binds& into) {
    auto table = v.as_table();
    if (!table) {
        return invalid_type(v, "a map");
    }
    into.clear();
    for (const auto& [key, value] : table->entries) {
        std::vector<std::string> outs;
        bool ok = false;
        if (auto s = as_text(value)) {
            outs.push_back(*s);
            ok = true;
        } else if (auto a = value.as_array()) {
            ok = true;
            for (const auto& item : *a) {
                auto text = as_text(item);
                if (!text) {
                    ok = false;
                    break;
                }
                outs.push_back(*text);
            }
        }
        if (!ok) {
            return DeError{"data did not match any variant of untagged enum Binding", {key}};
        }
        into.emplace_back(key, std::move(outs));
    }
    return std::nullopt;
}

constexpr const char* FIELDS[] = {"enabled",        "mouse_sensitivity", "mouse_y_scale", "mouse_invert_y",
                                  "mouse_capture",  "mouse_carry",       "stick_deadzone", "stick_curve",
                                  "walk_speed",     "on_foot",           "vehicle"};

struct File {
    std::optional<bool> enabled;
    std::optional<float> mouse_sensitivity, mouse_y_scale;
    std::optional<bool> mouse_invert_y, mouse_capture, mouse_carry;
    std::optional<float> stick_deadzone, stick_curve, walk_speed;
    Binds on_foot, vehicle;
};

std::optional<DeError> de_file(const toml::Table& table, File& f) {
    for (const auto& [key, value] : table.entries) {
        std::optional<DeError> e;
        if (key == "enabled") {
            e = de_bool(value, f.enabled);
        } else if (key == "mouse_sensitivity") {
            e = de_f32(value, f.mouse_sensitivity);
        } else if (key == "mouse_y_scale") {
            e = de_f32(value, f.mouse_y_scale);
        } else if (key == "mouse_invert_y") {
            e = de_bool(value, f.mouse_invert_y);
        } else if (key == "mouse_capture") {
            e = de_bool(value, f.mouse_capture);
        } else if (key == "mouse_carry") {
            e = de_bool(value, f.mouse_carry);
        } else if (key == "stick_deadzone") {
            e = de_f32(value, f.stick_deadzone);
        } else if (key == "stick_curve") {
            e = de_f32(value, f.stick_curve);
        } else if (key == "walk_speed") {
            e = de_f32(value, f.walk_speed);
        } else if (key == "on_foot") {
            e = de_binds(value, f.on_foot);
        } else if (key == "vehicle") {
            e = de_binds(value, f.vehicle);
        } else {
            std::string m = "unknown field `" + key + "`, expected one of ";
            for (std::size_t i = 0; i < std::size(FIELDS); ++i) {
                m += (i ? ", `" : "`") + std::string(FIELDS[i]) + "`";
            }
            return DeError{m, {}};
        }
        if (e) {
            e->keys.insert(e->keys.begin(), key);
            return e;
        }
    }
    return std::nullopt;
}


constexpr std::string_view CAPTION_HINT = " - click to capture the mouse (Alt frees it)";

std::optional<bool> flag(std::string_view value) {
    std::string v = to_ascii_lowercase(rust_text::trim(value));
    if (v == "1" || v == "true" || v == "on" || v == "yes") {
        return true;
    }
    if (v == "0" || v == "false" || v == "off" || v == "no" || v.empty()) {
        return false;
    }
    return std::nullopt;
}


std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            out.push_back(text.substr(start));
            break;
        }
        std::string_view line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out.push_back(line);
        start = nl + 1;
    }
    return out;
}

std::vector<std::string_view> split(std::string_view s, char at) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t i = s.find(at, start);
        if (i == std::string_view::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, i - start));
        start = i + 1;
    }
}

std::uint64_t saturating_sub(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : 0; }

}   

PadState PadState::merged(PadState other) const {
    auto [lx, ly] = pushed(other.left_x, other.left_y) ? std::pair{other.left_x, other.left_y} : std::pair{left_x, left_y};
    auto [rx, ry] = pushed(other.right_x, other.right_y) ? std::pair{other.right_x, other.right_y} : std::pair{right_x, right_y};
    return {buttons | other.buttons, fmax_rust(left_trigger, other.left_trigger), fmax_rust(right_trigger, other.right_trigger), lx, ly, rx, ry};
}

std::optional<std::uint8_t> key_code(std::string_view name) {
    name = rust_text::trim(name);
    if (name.size() == 1 && ascii_alnum(name[0])) {
        return static_cast<std::uint8_t>(ascii_upper(name[0]));
    }
    for (const auto& k : KEYS) {
        if (eq_ignore_ascii_case(k.name, name)) {
            return k.vk;
        }
    }
    if (eq_ignore_ascii_case(name, "Alt")) {
        return std::uint8_t{0xA4};
    }
    auto numbered = [&](std::string_view prefix, std::uint8_t first, std::uint8_t count) -> std::optional<std::uint8_t> {
        if (prefix.size() > name.size() || !is_char_boundary(name, prefix.size())) {
            return std::nullopt;
        }
        if (!eq_ignore_ascii_case(name.substr(0, prefix.size()), prefix)) {
            return std::nullopt;
        }
        auto n = rust_text::parse_u8(name.substr(prefix.size()));
        if (!n || *n >= count) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(first + *n);
    };
    if (auto vk = numbered("Num", 0x60, 10)) {
        return vk;
    }
    auto f = numbered("F", 0x6F, 25);   
    if (f && *f > 0x6F) {
        return f;
    }
    return std::nullopt;
}

std::string key_name(std::uint8_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= 0x60 && vk <= 0x69) {
        return "Num" + std::to_string(vk - 0x60);
    }
    if (vk >= 0x70 && vk <= 0x87) {
        return "F" + std::to_string(vk - 0x6F);
    }
    for (const auto& k : KEYS) {
        if (k.vk == vk) {
            return k.name;
        }
    }
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02x", vk);
    return buf;
}

std::span<const Output> Layout::outputs(std::uint8_t vk) const {
    auto it = keys.find(vk);
    if (it == keys.end()) {
        return {};
    }
    return it->second;
}

std::optional<std::string> Layout::bind(std::string_view key, const std::vector<std::string_view>& outputs) {
    auto vk = key_code(key);
    if (!vk) {
        return "unknown key `" + std::string(key) + "`";
    }
    std::vector<Output> outs;
    for (auto o : outputs) {
        if (rust_text::trim(o).empty()) {
            continue;
        }
        auto out = output(o);
        if (!out) {
            return "unknown pad output `" + std::string(o) + "` for key `" + std::string(key) + "`";
        }
        outs.push_back(*out);
    }
    if (outs.empty()) {
        keys.erase(*vk);
    } else {
        keys[*vk] = std::move(outs);
    }
    return std::nullopt;
}

Settings::Settings() : on_foot(default_layout(ON_FOOT)), vehicle(default_layout(VEHICLE)) {}

std::variant<Settings, std::string> Settings::parse(const toml::Table& table) {
    File file;
    if (auto e = de_file(table, file)) {
        return "[controls]: " + e->display();
    }
    Settings s;
    auto range = [](const char* name, std::optional<float> v, float lo, float hi, float& into) -> std::optional<std::string> {
        if (!v) {
            return std::nullopt;
        }
        if (!(lo <= *v && *v <= hi)) {
            return std::string("[controls] ") + name + " = " + rust_text::display(*v) + " is outside " + rust_text::display(lo) +
                   " to " + rust_text::display(hi);
        }
        into = *v;
        return std::nullopt;
    };
    if (auto e = range("mouse_sensitivity", file.mouse_sensitivity, 0.01f, 100.0f, s.sensitivity)) return *e;
    if (auto e = range("mouse_y_scale", file.mouse_y_scale, 0.01f, 100.0f, s.y_scale)) return *e;
    if (auto e = range("stick_deadzone", file.stick_deadzone, 0.0f, 0.9f, s.deadzone)) return *e;
    if (auto e = range("stick_curve", file.stick_curve, 0.2f, 5.0f, s.curve)) return *e;
    if (auto e = range("walk_speed", file.walk_speed, 0.05f, 1.0f, s.walk_speed)) return *e;
    s.enabled = file.enabled.value_or(s.enabled);
    s.invert_y = file.mouse_invert_y.value_or(s.invert_y);
    s.capture = file.mouse_capture.value_or(s.capture);
    s.carry = file.mouse_carry.value_or(s.carry);
    std::pair<Layout*, const Binds*> layouts[] = {{&s.on_foot, &file.on_foot}, {&s.vehicle, &file.vehicle}};
    const char* names[] = {"on_foot", "vehicle"};
    for (int i = 0; i < 2; ++i) {
        for (const auto& [key, binding] : *layouts[i].second) {
            std::vector<std::string_view> outs(binding.begin(), binding.end());
            if (auto e = layouts[i].first->bind(key, outs)) {
                return std::string("[controls.") + names[i] + "]: " + *e;
            }
        }
    }
    return s;
}

std::string caption(std::string_view base, bool captured) {
    if (base.size() >= CAPTION_HINT.size() && base.substr(base.size() - CAPTION_HINT.size()) == CAPTION_HINT) {
        base.remove_suffix(CAPTION_HINT.size());
    }
    if (captured) {
        return std::string(base);
    }
    return std::string(base) + std::string(CAPTION_HINT);
}

std::vector<std::string> Settings::apply_keymap(std::string_view text) {
    std::vector<std::string> problems;
    bool foot = true, in_vehicle = true;
    auto all = lines(text);
    for (std::size_t k = 0; k < all.size(); ++k) {
        std::string_view line = all[k];
        line = rust_text::trim(line.substr(0, line.find('#')));
        if (line.empty()) {
            continue;
        }
        std::string n = std::to_string(k + 1);
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            std::string_view name = line.substr(1, line.size() - 2);
            std::string section = to_ascii_lowercase(rust_text::trim(name));
            if (section == "on_foot" || section == "onfoot" || section == "foot") {
                foot = true, in_vehicle = false;
            } else if (section == "vehicle") {
                foot = false, in_vehicle = true;
            } else if (section == "all" || section == "both") {
                foot = true, in_vehicle = true;
            } else {
                problems.push_back("line " + n + ": unknown section `" + std::string(name) + "`");
            }
            continue;
        }
        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            problems.push_back("line " + n + ": expected key=button");
            continue;
        }
        std::string_view key = line.substr(0, eq);
        auto outs = split(line.substr(eq + 1), ',');
        std::vector<Layout*> layouts;
        if (foot) {
            layouts.push_back(&on_foot);
        }
        if (in_vehicle) {
            layouts.push_back(&vehicle);
        }
        for (Layout* layout : layouts) {
            if (auto e = layout->bind(key, outs)) {
                problems.push_back("line " + n + ": " + *e);
                break;
            }
        }
    }
    return problems;
}

std::vector<std::string> Settings::apply_env(const Get& get, const Read& read) {
    std::vector<std::string> problems;
    if (auto v = get("VICTORY_KBM")) {
        if (auto on = flag(*v)) {
            enabled = *on;
        } else {
            problems.push_back("VICTORY_KBM=" + *v + ": expected 0 or 1");
        }
    }
    if (auto v = get("VICTORY_MOUSE_SENS")) {
        auto s = rust_text::parse_f32(rust_text::trim(*v));
        if (s && 0.01f <= *s && *s <= 100.0f) {
            sensitivity = *s;
        } else {
            problems.push_back("VICTORY_MOUSE_SENS=" + *v + ": expected 0.01 to 100");
        }
    }
    if (auto v = get("VICTORY_MOUSE_INVERT_Y")) {
        if (auto on = flag(*v)) {
            invert_y = *on;
        } else {
            problems.push_back("VICTORY_MOUSE_INVERT_Y=" + *v + ": expected 0 or 1");
        }
    }
    auto path = get("VICTORY_KEYMAP");
    if (path && !rust_text::trim(*path).empty()) {
        auto got = read(rust_text::trim(*path));
        if (auto text = std::get_if<std::string>(&got)) {
            for (auto& p : apply_keymap(*text)) {
                problems.push_back("VICTORY_KEYMAP " + *path + ": " + p);
            }
        } else {
            problems.push_back("VICTORY_KEYMAP " + *path + ": " + std::get<IoError>(got).message);
        }
    }
    return problems;
}

namespace {

std::mutex SETTINGS_LOCK;
std::optional<Settings> SETTINGS;


std::optional<std::string> env_var(std::string_view name) {
    std::wstring wide(name.begin(), name.end());
    auto v = env::get(wide.c_str());
    if (!v) {
        return std::nullopt;
    }
    return rust_text::utf8_strict(*v);
}

std::wstring widen(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

IoError os_error(DWORD code) {
    return {std::system_category().message(static_cast<int>(code)) + " (os error " + std::to_string(code) + ")"};
}


std::variant<std::string, IoError> read_to_string(std::string_view path) {
    HANDLE file = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return os_error(GetLastError());
    }
    std::string text;
    char buf[65536];
    while (true) {
        DWORD got = 0;
        if (!ReadFile(file, buf, sizeof buf, &got, nullptr)) {
            DWORD e = GetLastError();
            CloseHandle(file);
            return os_error(e);
        }
        if (got == 0) {
            break;
        }
        text.append(buf, got);
    }
    CloseHandle(file);
    if (!rust_text::valid_utf8(text)) {
        return IoError{"stream did not contain valid UTF-8"};
    }
    return text;
}

}   

std::optional<std::string> configure(const toml::Table* table) {
    Settings s;
    if (table) {
        auto parsed = Settings::parse(*table);
        if (auto e = std::get_if<std::string>(&parsed)) {
            return *e;
        }
        s = std::get<Settings>(std::move(parsed));
    }

    for (const auto& problem : s.apply_env(env_var, read_to_string)) {
        log::warn(TARGET, problem);
    }
    log::info(TARGET, "keyboard and mouse controls",
              {{"enabled", log::debug(s.enabled)},
               {"sensitivity", log::debug(static_cast<double>(s.sensitivity))},
               {"invert_y", log::debug(s.invert_y)},
               {"capture", log::debug(s.capture)},
               {"deadzone", log::debug(static_cast<double>(s.deadzone))},
               {"curve", log::debug(static_cast<double>(s.curve))}});
    std::lock_guard lock(SETTINGS_LOCK);
    if (!SETTINGS) {
        SETTINGS = std::move(s);
    }
    return std::nullopt;
}

Settings settings() {
    std::lock_guard lock(SETTINGS_LOCK);
    if (!SETTINGS) {
        SETTINGS.emplace();
    }
    return *SETTINGS;
}


std::pair<float, float> Controls::Look::take(std::uint64_t now, std::pair<std::int64_t, std::int64_t> d, const Settings& s) {
    auto [dx, dy] = d;
    pending.first += static_cast<double>(dx);
    pending.second += static_cast<double>(dy);
    if (last_take && saturating_sub(now, *last_take) < MIN_TAKE_US) {
        return value;
    }
    std::uint64_t span = last_take ? std::min(saturating_sub(now, *last_take), MAX_TAKE_US) : FIRST_TAKE_US;
    last_take = now;
    double dt = static_cast<double>(span) / 1e6;
    double full = FULL_RATE / static_cast<double>(s.sensitivity);
    double ux = pending.first / dt / full;
    double uy = -pending.second * static_cast<double>(s.y_scale) / dt / full;
    double m = std::hypot(ux, uy);
    moved = m > 0.0;
    if (m <= 1.0) {
        pending = {0.0, 0.0};
    } else {

        double keep = s.carry ? 1.0 - 1.0 / m : 0.0;
        pending.first *= keep;
        pending.second *= keep;
        double cap = CARRY_S * full;
        double left = std::hypot(pending.first, pending.second);
        if (left > cap) {
            pending.first *= cap / left;
            pending.second *= cap / left;
        }
        ux /= m;
        uy /= m;
        m = 1.0;
    }
    if (m > 0.0) {
        double dz = static_cast<double>(s.deadzone);
        double shaped = dz + (1.0 - dz) * std::pow(m, 1.0 / static_cast<double>(s.curve));
        ux *= shaped / m;
        uy *= shaped / m;
    }
    if (s.invert_y) {
        uy = -uy;
    }
    value = {static_cast<float>(ux), static_cast<float>(uy)};
    return value;
}

Controls::Controls(Settings settings) : s(std::move(settings)) {}

Controls::Derived Controls::derive(std::span<const std::uint8_t> keys) const {
    Derived d;
    std::pair<float, float> l{0.0f, 0.0f}, r{0.0f, 0.0f};
    bool walk = false;
    for (std::uint8_t vk : keys) {
        for (const Output& out : layout().outputs(vk)) {
            switch (out.kind) {
            case Kind::Button: d.buttons |= out.button; break;
            case Kind::LeftTrigger: d.left_trigger = true; break;
            case Kind::RightTrigger: d.right_trigger = true; break;
            case Kind::Stick: {
                auto& stick = out.right ? r : l;
                float& axis = out.x ? stick.first : stick.second;
                axis += out.positive ? 1.0f : -1.0f;
                (out.right ? d.right_moved : d.left_moved) = true;
                break;
            }
            case Kind::Walk: walk = true; break;
            case Kind::ToggleVehicle:
            case Kind::ReleaseCursor: break;
            }
        }
    }
    auto round = [](std::pair<float, float> v) -> std::pair<float, float> {
        float x = std::clamp(v.first, -1.0f, 1.0f), y = std::clamp(v.second, -1.0f, 1.0f);
        float m = hypot32(x, y);
        if (m > 1.0f) {
            return {x / m, y / m};
        }
        return {x, y};
    };
    float speed = walk ? s.walk_speed : 1.0f;
    auto [lx, ly] = round(l);
    d.left = {lx * speed, ly * speed};
    d.right = round(r);
    return d;
}

void Controls::feed(std::span<const std::uint8_t> held, Counts& counts) {
    std::int32_t steps = counts.drain_wheel();
    std::uint8_t key = steps > 0 ? WHEEL_UP : WHEEL_DOWN;
    std::uint32_t n = steps < 0 ? 0u - static_cast<std::uint32_t>(steps) : static_cast<std::uint32_t>(steps);

    for (std::uint32_t i = 0; i < n && wheel_.size() < WHEEL_QUEUE; ++i) {
        wheel_.push_back(key);
    }
    std::vector<std::uint8_t> pressed;
    for (std::uint8_t vk : held) {
        if (std::find(held_.begin(), held_.end(), vk) == held_.end()) {
            pressed.push_back(vk);
        }
    }
    for (std::uint8_t vk : pressed) {
        auto span = layout().outputs(vk);
        std::vector<Output> outs(span.begin(), span.end());
        if (std::find(outs.begin(), outs.end(), Output::of(Kind::ToggleVehicle)) != outs.end()) {
            vehicle_ = !vehicle_;
            log::info(TARGET, "controls layout", {{"vehicle", log::debug(vehicle_)}, {"key", key_name(vk)}});
        }
        if (std::find(outs.begin(), outs.end(), Output::of(Kind::ReleaseCursor)) != outs.end()) {
            release_cursor = true;
        }
    }
    Derived before = derived_;
    held_.assign(held.begin(), held.end());
    derived_ = derive(held_);
    latched_ |= derived_.buttons & ~before.buttons;
    latched_triggers_.first |= derived_.left_trigger && !before.left_trigger;
    latched_triggers_.second |= derived_.right_trigger && !before.right_trigger;
    if (pulse_.kind == Pulse::Idle && !wheel_.empty()) {
        pulse_ = {Pulse::Down, wheel_.front()};
        wheel_.pop_front();
    }
}

PadState Controls::kbm() const {
    Derived d = derived_;
    if (pulse_.kind == Pulse::Down) {
        Derived p = derive(std::span(&pulse_.key, 1));
        d.buttons |= p.buttons;
        d.left_trigger |= p.left_trigger;
        d.right_trigger |= p.right_trigger;
    }
    auto on = [](bool b) { return b ? 1.0f : 0.0f; };
    float rx = d.right.first + look_.value.first, ry = d.right.second + look_.value.second;
    float m = hypot32(rx, ry);
    if (m > 1.0f) {
        rx = rx / m;
        ry = ry / m;
    }
    PadState pad{d.buttons | latched_, on(d.left_trigger || latched_triggers_.first), on(d.right_trigger || latched_triggers_.second),
                 d.left.first, d.left.second, rx, ry};
    return pad.merged(script_);
}

void Controls::set_script(PadState pad) {
    latched_ |= pad.buttons & ~script_.buttons;
    latched_triggers_.first |= pad.left_trigger > 0.5f && script_.left_trigger <= 0.5f;
    latched_triggers_.second |= pad.right_trigger > 0.5f && script_.right_trigger <= 0.5f;
    script_ = pad;
}

PadState Controls::peek(std::optional<PadState> pad) { return merge(pad); }

PadState Controls::take(std::uint64_t now, std::optional<PadState> pad, Counts& counts) {
    auto before = look_.last_take;
    look_.take(now, counts.drain(), s);
    PadState state = merge(pad);
    if (look_.last_take != before) {
        latched_ = 0;
        latched_triggers_ = {false, false};
        pulse_ = pulse_.kind == Pulse::Down ? Pulse{Pulse::Gap, 0} : Pulse{Pulse::Idle, 0};
    }
    return state;
}

PadState Controls::merge(std::optional<PadState> pad) {
    PadState k = kbm();
    bool script_left = pushed(script_.left_x, script_.left_y);
    bool script_right = pushed(script_.right_x, script_.right_y);
    bool kbm_used = !held_.empty() || look_.moved || pulse_.kind != Pulse::Idle || !script_.is_idle();
    if (!pad) {
        used(kbm_used ? std::optional(Owner::Kbm) : std::nullopt);
        return k;
    }
    bool pad_left = pushed(pad->left_x, pad->left_y);
    bool pad_right = pushed(pad->right_x, pad->right_y);


    if (pad_left) {
        left_owner_ = Owner::Pad;
    } else if (derived_.left_moved || script_left) {
        left_owner_ = Owner::Kbm;
    }
    if (pad_right) {
        right_owner_ = Owner::Pad;
    } else if (derived_.right_moved || look_.moved || script_right) {
        right_owner_ = Owner::Kbm;
    }
    bool pad_used = pad->buttons != 0 || pad_left || pad_right || pad->left_trigger > 0.1f || pad->right_trigger > 0.1f;
    used(kbm_used ? std::optional(Owner::Kbm) : pad_used ? std::optional(Owner::Pad) : std::nullopt);
    auto [lx, ly] = left_owner_ == Owner::Kbm ? std::pair{k.left_x, k.left_y} : std::pair{pad->left_x, pad->left_y};
    auto [rx, ry] = right_owner_ == Owner::Kbm ? std::pair{k.right_x, k.right_y} : std::pair{pad->right_x, pad->right_y};
    return {pad->buttons | k.buttons, fmax_rust(pad->left_trigger, k.left_trigger), fmax_rust(pad->right_trigger, k.right_trigger),
            lx, ly, rx, ry};
}

void Controls::used(std::optional<Owner> now) {
    if (now && last_used_ != now) {
        last_used_ = now;
        log::info(TARGET, "input: now using", {{"device", log::debug(*now == Owner::Pad ? "gamepad" : "keyboard and mouse")}});
    }
}

}   
