// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboard and mouse played the way PC games are (`victory-core/src/kbm.rs`;
// DanziLoader / victory-perf, see NOTICE.md): keys to pad buttons and
// sticks, the mouse to the right stick, mouse buttons to the triggers.
//
// The title reads gamepads and keyboards only (it registers no mouse), and
// turns its right stick into a turn rate. Mouse counts are a distance, so
// they become a rate when the title reads the pad: the counts since its last
// read over the time since then. The camera then turns the same angle for
// the same hand movement at any frame rate.
//
// All of this is plain data: the host side of `gameinput` feeds it and the
// title's reads drain it. `[controls]` in victory.toml sets and rebinds it.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "toml.hpp"

namespace danzi::kbm {


constexpr std::uint32_t MENU = 0x1;
constexpr std::uint32_t VIEW = 0x2;
constexpr std::uint32_t A = 0x4;
constexpr std::uint32_t B = 0x8;
constexpr std::uint32_t X = 0x10;
constexpr std::uint32_t Y = 0x20;
constexpr std::uint32_t UP = 0x40;
constexpr std::uint32_t DOWN = 0x80;
constexpr std::uint32_t LEFT = 0x100;
constexpr std::uint32_t RIGHT = 0x200;
constexpr std::uint32_t LEFT_SHOULDER = 0x400;
constexpr std::uint32_t RIGHT_SHOULDER = 0x800;
constexpr std::uint32_t LEFT_STICK = 0x1000;
constexpr std::uint32_t RIGHT_STICK = 0x2000;


struct PadState {
    std::uint32_t buttons = 0;
    float left_trigger = 0, right_trigger = 0;
    float left_x = 0, left_y = 0;
    float right_x = 0, right_y = 0;

    bool operator==(const PadState&) const = default;

    bool is_idle() const { return *this == PadState{}; }


    PadState merged(PadState other) const;
};
static_assert(sizeof(PadState) == 28);


constexpr std::uint8_t WHEEL_UP = 0x0E;
constexpr std::uint8_t WHEEL_DOWN = 0x0F;

constexpr std::array<std::uint8_t, 5> MOUSE_BUTTONS = {0x01, 0x02, 0x04, 0x05, 0x06};


struct Output {
    enum class Kind : std::uint8_t {
        Button,
        LeftTrigger,
        RightTrigger,

        Stick,

        Walk,

        ToggleVehicle,

        ReleaseCursor,
    };
    Kind kind = Kind::Button;
    std::uint32_t button = 0;   
    bool right = false, x = false, positive = false;   

    bool operator==(const Output&) const = default;
    static constexpr Output of_button(std::uint32_t b) { return {Kind::Button, b, false, false, false}; }
    static constexpr Output of(Kind k) { return {k, 0, false, false, false}; }
    static constexpr Output stick(bool right, bool x, bool positive) { return {Kind::Stick, 0, right, x, positive}; }
};


std::optional<std::uint8_t> key_code(std::string_view name);

std::string key_name(std::uint8_t vk);


struct Layout {
    std::map<std::uint8_t, std::vector<Output>> keys;

    bool operator==(const Layout&) const = default;
    std::span<const Output> outputs(std::uint8_t vk) const;


    std::optional<std::string> bind(std::string_view key, const std::vector<std::string_view>& outputs);
};


struct IoError {
    std::string message;
};

struct Settings {

    bool enabled = true;
    float sensitivity = 1.0f;

    float y_scale = 1.0f;
    bool invert_y = false;

    bool capture = true;

    bool carry = true;


    float deadzone = 0.0f;
    float curve = 1.0f;
    float walk_speed = 0.5f;
    Layout on_foot, vehicle;


    Settings();
    bool operator==(const Settings&) const = default;


    static std::variant<Settings, std::string> parse(const toml::Table& table);


    std::vector<std::string> apply_keymap(std::string_view text);


    using Get = std::function<std::optional<std::string>(std::string_view)>;
    using Read = std::function<std::variant<std::string, IoError>(std::string_view)>;
    std::vector<std::string> apply_env(const Get& get, const Read& read);
};


std::string caption(std::string_view base, bool captured);


std::optional<std::string> configure(const toml::Table* table);

Settings settings();


class Counts {
public:
    void add(std::int64_t dx, std::int64_t dy) {
        x_.fetch_add(dx, std::memory_order_relaxed);
        y_.fetch_add(dy, std::memory_order_relaxed);
    }

    void add_wheel(std::int32_t steps) { wheel_.fetch_add(steps, std::memory_order_relaxed); }

private:
    friend class Controls;
    std::pair<std::int64_t, std::int64_t> drain() {
        return {x_.exchange(0, std::memory_order_relaxed), y_.exchange(0, std::memory_order_relaxed)};
    }
    std::int32_t drain_wheel() { return wheel_.exchange(0, std::memory_order_relaxed); }
    std::atomic<std::int64_t> x_{0}, y_{0};
    std::atomic<std::int32_t> wheel_{0};
};


enum class Owner { Pad, Kbm };


class Controls {
public:
    explicit Controls(Settings s);

    Settings s;

    bool release_cursor = false;


    void feed(std::span<const std::uint8_t> held, Counts& counts);


    void set_script(PadState pad);


    PadState peek(std::optional<PadState> pad);


    PadState take(std::uint64_t now, std::optional<PadState> pad, Counts& counts);

    std::optional<Owner> last_used() const { return last_used_; }

    bool in_vehicle() const { return vehicle_; }

private:

    struct Derived {
        std::uint32_t buttons = 0;
        bool left_trigger = false, right_trigger = false;
        std::pair<float, float> left{0.0f, 0.0f}, right{0.0f, 0.0f};
        bool left_moved = false, right_moved = false;
    };


    struct Pulse {
        enum Kind { Idle, Down, Gap } kind = Idle;
        std::uint8_t key = 0;
        bool operator==(const Pulse&) const = default;
    };

    struct Look {

        std::pair<double, double> pending{0.0, 0.0};
        std::optional<std::uint64_t> last_take;
        std::pair<float, float> value{0.0f, 0.0f};
        bool moved = false;
        std::pair<float, float> take(std::uint64_t now, std::pair<std::int64_t, std::int64_t> d, const Settings& s);
    };

    const Layout& layout() const { return vehicle_ ? s.vehicle : s.on_foot; }
    Derived derive(std::span<const std::uint8_t> keys) const;
    PadState kbm() const;
    PadState merge(std::optional<PadState> pad);
    void used(std::optional<Owner> now);

    bool vehicle_ = false;
    std::vector<std::uint8_t> held_;
    Derived derived_;


    std::uint32_t latched_ = 0;
    std::pair<bool, bool> latched_triggers_{false, false};
    std::deque<std::uint8_t> wheel_;
    Pulse pulse_;
    Look look_;
    Owner left_owner_ = Owner::Kbm, right_owner_ = Owner::Kbm;
    std::optional<Owner> last_used_;


    PadState script_;
};

}   
