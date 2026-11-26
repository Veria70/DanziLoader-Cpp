// SPDX-License-Identifier: GPL-3.0-or-later
// The reference's `kbm` unit tests (`victory-core/src/kbm.rs` `mod tests`;
// DanziLoader / victory-perf, see NOTICE.md). Tables the reference parses
// from TOML text are built by hand (no parser yet).
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/kbm.hpp"
#include "core/rust_text.hpp"

using namespace danzi::kbm;
namespace toml = danzi::toml;

namespace {

std::uint8_t vk(const char* name) { return key_code(name).value(); }

Controls controls() { return Controls(Settings()); }

PadState held(Controls& c, std::initializer_list<const char*> keys) {
    std::vector<std::uint8_t> codes;
    for (auto k : keys) {
        codes.push_back(vk(k));
    }
    Counts counts;
    c.feed(codes, counts);
    return c.peek(std::nullopt);
}

std::vector<Output> outs(const Layout& l, std::uint8_t key) {
    auto s = l.outputs(key);
    return {s.begin(), s.end()};
}

toml::Value str(const char* s) { return toml::Value{std::string(s)}; }
toml::Value num(double f) { return toml::Value{f}; }
toml::Value flag(bool b) { return toml::Value{b}; }
toml::Value table(std::initializer_list<std::pair<const char*, toml::Value>> entries) {
    toml::Table t;
    for (const auto& [k, v] : entries) {
        t.set(k, v);
    }
    return toml::Value{t};
}

const Settings* ok(const std::variant<Settings, std::string>& r) { return std::get_if<Settings>(&r); }

const float R = 0.70710677f;   

Settings::Get env(std::vector<std::pair<std::string, std::string>> pairs) {
    return [pairs](std::string_view k) -> std::optional<std::string> {
        for (const auto& [n, v] : pairs) {
            if (n == k) {
                return v;
            }
        }
        return std::nullopt;
    };
}

std::variant<std::string, IoError> no_files(std::string_view) { return IoError{"entity not found"}; }

PadState script(std::uint32_t buttons) {
    PadState p;
    p.buttons = buttons;
    return p;
}

}   

TEST(kbm_key_names_round_trip) {
    std::pair<const char*, int> names[] = {{"w", 'W'},          {"7", '7'},       {"Space", 0x20},     {"lshift", 0xA0},
                                                    {"F1", 0x70},        {"F24", 0x87},    {"Num5", 0x65},      {"MouseLeft", 0x01},
                                                    {"WheelDown", WHEEL_DOWN}, {"Alt", 0xA4}};
    for (auto [name, code] : names) {
        CHECK_EQ(key_code(name), std::optional(static_cast<std::uint8_t>(code)));
    }
    CHECK(!key_code("F0"));
    CHECK(!key_code("F25"));
    CHECK(!key_code("Num10"));
    CHECK(!key_code("Banana"));
    CHECK_EQ(key_name(0x70), "F1");
    CHECK_EQ(key_name(0xA0), "LShift");
    CHECK_EQ(key_name('Q'), "Q");
}

TEST(kbm_wasd_moves_the_left_stick_round_and_walk_slows_it) {
    auto c = controls();
    auto p = held(c, {"W"});
    CHECK(p.left_x == 0.0f && p.left_y == 1.0f);
    p = held(c, {"W", "D"});
    CHECK(std::fabs(p.left_x - R) < 1e-6f && std::fabs(p.left_y - R) < 1e-6f);
    p = held(c, {"A", "D"});
    CHECK(p.left_x == 0.0f && p.left_y == 0.0f);
    p = held(c, {"S", "Z"});
    CHECK(p.left_x == 0.0f && p.left_y == -0.5f);
}

TEST(kbm_keys_and_mouse_buttons_press_pad_buttons_and_triggers) {
    auto c = controls();
    auto p = held(c, {"Space", "LShift", "MouseLeft", "MouseRight", "Tab"});
    CHECK_EQ(p.buttons, X | A | VIEW);
    CHECK(p.left_trigger == 1.0f && p.right_trigger == 1.0f);
    c = controls();
    p = held(c, {"Esc"});
    CHECK_EQ(p.buttons, B);   
    CHECK(!c.release_cursor);
    auto c2 = controls();
    p = held(c2, {"Backspace", "P"});
    CHECK_EQ(p.buttons, B | MENU);
    held(c, {"LAlt"});
    CHECK(c.release_cursor);   
}

TEST(kbm_g_switches_to_the_vehicle_layout_and_back) {
    auto c = controls();
    held(c, {"G"});
    CHECK(c.in_vehicle());
    held(c, {});
    auto p = held(c, {"W", "A"});
    CHECK_EQ(p.right_trigger, 1.0f);   
    CHECK(p.left_x == -1.0f && p.left_y == 0.0f);   

    held(c, {"G"});
    held(c, {"G"});
    CHECK(!c.in_vehicle());
}

TEST(kbm_a_tap_between_two_reads_is_read_once) {
    auto c = controls();
    Counts counts;
    c.take(0, std::nullopt, counts);
    std::uint8_t space[] = {vk("Space")};
    c.feed(space, counts);
    c.feed({}, counts);
    CHECK_EQ(c.peek(std::nullopt).buttons, X);   
    CHECK_EQ(c.take(10'000, std::nullopt, counts).buttons, X);
    CHECK_EQ(c.take(20'000, std::nullopt, counts).buttons, 0u);

    std::uint8_t click[] = {vk("MouseLeft")};
    c.feed(click, counts);
    c.feed({}, counts);
    CHECK_EQ(c.take(30'000, std::nullopt, counts).right_trigger, 1.0f);
    CHECK_EQ(c.take(40'000, std::nullopt, counts).right_trigger, 0.0f);
}

TEST(kbm_wheel_steps_are_each_a_press_the_title_reads) {
    auto c = controls();
    Counts counts;
    counts.add_wheel(2);
    std::vector<std::uint32_t> seen;
    for (std::uint64_t i = 0; i < 6; ++i) {
        c.feed({}, counts);
        seen.push_back(c.take(i * 10'000, std::nullopt, counts).buttons);
    }
    CHECK_EQ(seen, (std::vector<std::uint32_t>{UP, 0, UP, 0, 0, 0}));
    counts.add_wheel(-1);
    c.feed({}, counts);
    CHECK_EQ(c.take(100'000, std::nullopt, counts).buttons, DOWN);
}

TEST(kbm_mouse_counts_become_a_rate_at_each_read) {
    auto c = controls();
    Counts counts;
    c.take(0, std::nullopt, counts);

    counts.add(10, 0);
    auto p = c.take(10'000, std::nullopt, counts);
    CHECK(std::fabs(p.right_x - 0.5f) < 1e-6f);


    counts.add(10, 0);
    p = c.take(50'000, std::nullopt, counts);
    CHECK(std::fabs(p.right_x - 0.125f) < 1e-6f);

    counts.add(0, -5);
    p = c.take(60'000, std::nullopt, counts);
    CHECK(std::fabs(p.right_y - 0.25f) < 1e-6f && p.right_x == 0.0f);
    CHECK_EQ(c.take(70'000, std::nullopt, counts).right_y, 0.0f);
}

TEST(kbm_slow_frames_get_the_rate_over_their_whole_span) {
    auto c = controls();
    Counts counts;
    c.take(0, std::nullopt, counts);

    counts.add(1000, 0);
    auto p = c.take(800'000, std::nullopt, counts);
    CHECK(std::fabs(p.right_x - 0.625f) < 1e-6f);

    counts.add(1000, 0);
    p = c.take(3'800'000, std::nullopt, counts);
    CHECK(std::fabs(p.right_x - 0.5f) < 1e-6f);
}

TEST(kbm_reads_close_together_see_the_same_mouse_stick) {
    auto c = controls();
    Counts counts;
    c.take(0, std::nullopt, counts);
    counts.add(10, 0);
    auto a = c.take(10'000, std::nullopt, counts);
    auto b = c.take(10'500, std::nullopt, counts);
    CHECK(a == b);
    counts.add(4, 0);
    auto p = c.take(11'000, std::nullopt, counts);
    CHECK(p == a);   
    p = c.take(20'000, std::nullopt, counts);
    CHECK(std::fabs(p.right_x - 0.2f) < 1e-6f);
}

TEST(kbm_dead_zone_curve_and_inversion_shape_the_mouse_stick) {
    Settings s;
    s.deadzone = 0.2f;
    s.curve = 2.0f;
    s.invert_y = true;
    Controls c(s);
    Counts counts;
    c.take(0, std::nullopt, counts);
    counts.add(0, -5);   
    auto p = c.take(10'000, std::nullopt, counts);

    CHECK(std::fabs(p.right_y + 0.6f) < 1e-6f);

    counts.add(1, 0);
    p = c.take(20'000, std::nullopt, counts);
    CHECK(p.right_x > 0.2f);
}

TEST(kbm_a_fast_flick_is_turned_in_full_later) {
    auto c = controls();
    Counts counts;
    c.take(0, std::nullopt, counts);
    counts.add(50, 0);   
    std::vector<float> xs;
    for (std::uint64_t i = 1; i <= 4; ++i) {
        xs.push_back(c.take(i * 10'000, std::nullopt, counts).right_x);
    }
    CHECK(xs[0] == 1.0f && xs[1] == 1.0f);
    CHECK(std::fabs(xs[2] - 0.5f) < 1e-6f);
    CHECK_EQ(xs[3], 0.0f);
    Settings s;
    s.carry = false;
    Controls c2(s);
    c2.take(0, std::nullopt, counts);
    counts.add(50, 0);
    CHECK_EQ(c2.take(10'000, std::nullopt, counts).right_x, 1.0f);
    CHECK_EQ(c2.take(20'000, std::nullopt, counts).right_x, 0.0f);
}

TEST(kbm_a_resting_pad_never_overrides_the_mouse_and_a_pushed_one_does) {
    auto c = controls();
    Counts counts;
    PadState drift;
    drift.right_x = 0.1f;
    drift.left_y = -0.05f;
    drift.buttons = B;
    drift.right_trigger = 0.4f;
    c.take(0, drift, counts);
    counts.add(10, 0);
    std::uint8_t keys[] = {vk("W"), vk("MouseLeft")};
    c.feed(keys, counts);
    auto p = c.take(10'000, drift, counts);
    CHECK(std::fabs(p.right_x - 0.5f) < 1e-6f);
    CHECK(p.left_y == 1.0f && p.buttons == B && p.right_trigger == 1.0f);
    c.feed({}, counts);
    CHECK_EQ(c.take(20'000, drift, counts).right_x, 0.0f);   
    PadState pushed;
    pushed.right_x = 0.7f;
    pushed.left_x = -0.9f;
    p = c.take(30'000, pushed, counts);
    CHECK(p.right_x == 0.7f && p.left_x == -0.9f);
    CHECK(c.last_used() == Owner::Pad);
}

TEST(kbm_config_rebinds_unbinds_and_refuses_typos) {


    toml::Table t = table({{"mouse_sensitivity", num(2.5)},
                           {"mouse_invert_y", flag(true)},
                           {"stick_deadzone", num(0.24)},
                           {"on_foot", table({{"Space", str("A")},
                                              {"LShift", str("")},
                                              {"Mouse4", toml::Value{toml::Array{str("LB"), str("RB")}}}})},
                           {"vehicle", table({{"W", str("RightTrigger_typo")}})}})
                        .as_table()[0];
    auto r = Settings::parse(t);
    CHECK(!ok(r));
    CHECK(std::get<std::string>(r).find("RightTrigger_typo") != std::string::npos);
    std::erase_if(t.entries, [](const auto& e) { return e.first == "vehicle"; });
    r = Settings::parse(t);
    const Settings* s = ok(r);
    CHECK(s);
    CHECK(s->sensitivity == 2.5f && s->invert_y && s->deadzone == 0.24f);
    CHECK_EQ(outs(s->on_foot, 0x20).size(), 1u);
    CHECK(outs(s->on_foot, 0x20)[0] == Output::of_button(A));
    CHECK(outs(s->on_foot, 0xA0).empty());
    CHECK(outs(s->on_foot, 0x05) == (std::vector<Output>{Output::of_button(LEFT_SHOULDER), Output::of_button(RIGHT_SHOULDER)}));
    CHECK(outs(s->on_foot, 'W') == outs(Settings().on_foot, 'W'));
    toml::Value bad[] = {table({{"mouse_sensitivty", num(1.0)}}), table({{"stick_deadzone", num(1.5)}}),
                         table({{"on_foot", table({{"Banana", str("A")}})}})};
    for (const auto& b : bad) {
        CHECK(!ok(Settings::parse(*b.as_table())));
    }
    auto off = Settings::parse(*table({{"enabled", flag(false)}}).as_table());
    CHECK(ok(off) && !ok(off)->enabled);
}

TEST(kbm_keymap_lines_rebind_unbind_and_report_what_they_cannot_read) {
    Settings s;
    auto problems = s.apply_keymap(
        "# my keys\n"
        "Space = A\n"
        "MouseLeft = RT, A   # fire and confirm\n"
        "LShift =\n"
        "Banana = X\n"
        "Tab = Nonsense\n"
        "no equals here\n"
        "[vehicle]\n"
        "W = RT, LB\n"
        "[oops]\n");
    CHECK_EQ(problems.size(), 4u);
    CHECK(problems[0].starts_with("line 5") && problems[0].find("Banana") != std::string::npos);
    CHECK(problems[1].starts_with("line 6") && problems[1].find("Nonsense") != std::string::npos);
    CHECK(problems[2].starts_with("line 7"));
    CHECK(problems[3].starts_with("line 10") && problems[3].find("oops") != std::string::npos);

    for (const Layout* layout : {&s.on_foot, &s.vehicle}) {
        CHECK(outs(*layout, 0x20) == std::vector<Output>{Output::of_button(A)});
        CHECK(outs(*layout, 0x01) == (std::vector<Output>{Output::of(Output::Kind::RightTrigger), Output::of_button(A)}));
        CHECK(outs(*layout, 0xA0).empty());
    }

    CHECK(outs(s.vehicle, 'W') == (std::vector<Output>{Output::of(Output::Kind::RightTrigger), Output::of_button(LEFT_SHOULDER)}));
    CHECK(outs(s.on_foot, 'W') == std::vector<Output>{Output::stick(false, false, true)});
    CHECK(outs(s.on_foot, 0x09) == outs(Settings().on_foot, 0x09));
}

TEST(kbm_keymap_sections_name_one_layout) {
    Settings s;
    auto p = s.apply_keymap("[on_foot]\nH = Y\n[vehicle]\nJ = B\n[all]\nK = X\n");
    CHECK(p.empty());
    CHECK(outs(s.on_foot, 'H') == std::vector<Output>{Output::of_button(Y)});
    CHECK(outs(s.vehicle, 'H').empty());
    CHECK(outs(s.vehicle, 'J') == std::vector<Output>{Output::of_button(B)});
    CHECK(outs(s.on_foot, 'J').empty());
    CHECK(outs(s.on_foot, 'K') == outs(s.vehicle, 'K'));
}

TEST(kbm_environment_switches_kbm_off_and_sets_the_mouse) {
    Settings s;
    CHECK(s.enabled);   
    auto p = s.apply_env(env({}), no_files);
    CHECK(p.empty() && s.enabled);
    p = s.apply_env(env({{"VICTORY_MOUSE_SENS", "2.5"}, {"VICTORY_MOUSE_INVERT_Y", "1"}}), no_files);
    CHECK(p.empty());
    CHECK(s.sensitivity == 2.5f && s.invert_y && s.enabled);
    p = s.apply_env(env({{"VICTORY_KBM", "0"}}), no_files);
    CHECK(p.empty() && !s.enabled);
    s.apply_env(env({{"VICTORY_KBM", "1"}}), no_files);
    CHECK(s.enabled);

    auto parsed = Settings::parse(*table({{"mouse_sensitivity", num(3.0)}}).as_table());
    Settings s2 = std::get<Settings>(parsed);
    s2.apply_env(env({{"VICTORY_MOUSE_SENS", "0.5"}}), no_files);
    CHECK_EQ(s2.sensitivity, 0.5f);
}

TEST(kbm_bad_environment_values_are_reported_and_ignored) {
    Settings s;
    auto p = s.apply_env(env({{"VICTORY_KBM", "maybe"},
                              {"VICTORY_MOUSE_SENS", "fast"},
                              {"VICTORY_MOUSE_INVERT_Y", "2"},
                              {"VICTORY_KEYMAP", "missing.txt"}}),
                         no_files);
    CHECK_EQ(p.size(), 4u);
    CHECK(s == Settings());
    p = s.apply_env(env({{"VICTORY_MOUSE_SENS", "0"}}), no_files);
    CHECK_EQ(p.size(), 1u);
}

TEST(kbm_the_keymap_file_is_read_through_the_environment) {
    Settings s;
    std::string asked;
    auto read = [&](std::string_view p) -> std::variant<std::string, IoError> {
        asked = p;
        return std::string("Space=B\nBogus=A\n");
    };
    auto p = s.apply_env(env({{"VICTORY_KEYMAP", "keys.txt"}}), read);
    CHECK_EQ(asked, "keys.txt");
    CHECK_EQ(p.size(), 1u);
    CHECK(p[0].find("keys.txt") != std::string::npos && p[0].find("Bogus") != std::string::npos);
    CHECK(outs(s.on_foot, 0x20) == std::vector<Output>{Output::of_button(B)});
}


TEST(kbm_the_default_layout_is_the_documented_one) {
    std::pair<const char*, std::uint32_t> table[] = {
        {"LShift", A}, {"Space", X},    {"E", Y},         {"F", Y},      {"Q", LEFT_SHOULDER}, {"R", RIGHT_SHOULDER},
        {"LCtrl", LEFT_STICK}, {"MouseMiddle", RIGHT_STICK}, {"Enter", A}, {"Esc", B}, {"Backspace", B}, {"Up", UP},
        {"Down", DOWN}, {"Left", LEFT}, {"Right", RIGHT}, {"Tab", VIEW}, {"P", MENU}};
    for (auto [key, buttons] : table) {
        auto c = controls();
        CHECK_EQ(held(c, {key}).buttons, buttons);
    }
    auto c = controls();
    auto p = held(c, {"MouseLeft"});
    CHECK(p.left_trigger == 0.0f && p.right_trigger == 1.0f);
    c = controls();
    p = held(c, {"MouseRight"});
    CHECK(p.left_trigger == 1.0f && p.right_trigger == 0.0f);
    c = controls();
    p = held(c, {"S", "A"});
    CHECK(std::fabs(p.left_x + R) < 1e-6f && std::fabs(p.left_y + R) < 1e-6f);
}

TEST(kbm_alt_frees_the_mouse_and_does_not_press_anything) {
    for (const char* alt : {"LAlt", "RAlt"}) {
        auto c = controls();
        auto p = held(c, {alt});
        CHECK(c.release_cursor);
        CHECK_EQ(p.buttons, 0u);
    }
}

TEST(kbm_mouse_counts_over_a_slow_frame_turn_the_same_angle) {


    auto c = controls();
    Counts counts;
    c.take(0, std::nullopt, counts);
    for (int i = 0; i < 30; ++i) {
        counts.add(20, 0);   
    }
    auto p = c.take(300'000, std::nullopt, counts);
    CHECK(std::fabs(p.right_x - 1.0f) < 1e-6f);

    for (int i = 0; i < 30; ++i) {
        counts.add(10, 0);
    }
    p = c.take(600'000, std::nullopt, counts);
    CHECK(std::fabs(p.right_x - 0.5f) < 1e-6f);

    p = c.take(900'000, std::nullopt, counts);
    CHECK(p.right_x == 0.0f && p.right_y == 0.0f);
}

TEST(kbm_a_key_tap_and_a_click_inside_one_slow_frame_are_not_lost) {
    auto c = controls();
    Counts counts;
    c.take(0, std::nullopt, counts);

    std::uint8_t both[] = {vk("Enter"), vk("MouseRight")}, enter[] = {vk("Enter")}, space[] = {vk("Space")};
    c.feed(both, counts);
    c.feed(enter, counts);
    c.feed({}, counts);
    auto p = c.take(300'000, std::nullopt, counts);
    CHECK_EQ(p.buttons, A);
    CHECK_EQ(p.left_trigger, 1.0f);

    p = c.take(600'000, std::nullopt, counts);
    CHECK(p.buttons == 0 && p.left_trigger == 0.0f);

    c.feed(space, counts);
    CHECK_EQ(c.take(900'000, std::nullopt, counts).buttons, X);
    CHECK_EQ(c.take(1'200'000, std::nullopt, counts).buttons, X);
}

TEST(kbm_scripted_input_joins_keys_and_pad_and_none_of_them_is_lost) {
    auto c = controls();
    Counts counts;
    PadState pad;
    pad.buttons = B;
    pad.right_trigger = 0.5f;
    c.take(0, pad, counts);
    std::uint8_t space[] = {vk("Space")};
    c.feed(space, counts);
    c.set_script(script(A));
    auto p = c.take(10'000, pad, counts);
    CHECK_EQ(p.buttons, B | X | A);
    CHECK_EQ(p.right_trigger, 0.5f);

    auto c2 = controls();
    c2.set_script(script(MENU));
    CHECK_EQ(c2.peek(std::nullopt).buttons, MENU);
}

TEST(kbm_a_scripted_press_shorter_than_a_frame_is_latched_until_read) {
    auto c = controls();
    Counts counts;
    c.take(0, std::nullopt, counts);
    c.set_script(script(A));
    c.set_script(PadState{});   
    CHECK_EQ(c.take(300'000, std::nullopt, counts).buttons, A);
    CHECK_EQ(c.take(600'000, std::nullopt, counts).buttons, 0u);

    PadState stick;
    stick.left_y = 1.0f;
    c.set_script(stick);
    PadState keys;
    keys.left_y = -0.7f;
    auto p = c.take(610'000, keys, counts);
    CHECK_EQ(p.left_y, -0.7f);   
    p = c.take(620'000, PadState{}, counts);
    CHECK_EQ(p.left_y, 1.0f);   
}

TEST(kbm_a_physical_pad_has_priority_for_a_stick_it_pushes) {
    auto c = controls();
    Counts counts;
    std::uint8_t w[] = {vk("W")};
    c.feed(w, counts);
    c.take(0, std::nullopt, counts);
    PadState pad;
    pad.left_y = -0.8f;

    CHECK_EQ(c.take(10'000, pad, counts).left_y, -0.8f);

    auto p = c.take(20'000, PadState{}, counts);
    CHECK_EQ(p.left_y, 1.0f);

    c.feed({}, counts);
    p = c.take(30'000, PadState{}, counts);
    CHECK_EQ(p.left_y, 0.0f);
}

TEST(kbm_merged_pads_or_buttons_max_triggers_and_prefer_the_pushed_stick) {
    PadState a;
    a.buttons = A;
    a.left_trigger = 0.3f;
    a.left_x = 0.1f;
    a.right_y = 0.9f;
    PadState b;
    b.buttons = B;
    b.left_trigger = 0.8f;
    b.left_x = -1.0f;
    b.right_y = 0.05f;
    auto m = a.merged(b);
    CHECK_EQ(m.buttons, A | B);
    CHECK_EQ(m.left_trigger, 0.8f);
    CHECK_EQ(m.left_x, -1.0f);   
    CHECK_EQ(m.right_y, 0.9f);   
    CHECK(PadState{}.is_idle() && !a.is_idle());
}

TEST(kbm_the_caption_hints_while_free_and_is_the_title_s_own_when_captured) {
    auto free = caption("Game", false);
    CHECK(free.starts_with("Game") && free.find("click") != std::string::npos);
    CHECK_EQ(caption(free, true), "Game");
    CHECK_EQ(caption(free, false), free);   
    CHECK_EQ(caption("Game", true), "Game");
}

TEST(kbm_mouse_sensitivity_and_inversion_scale_the_stick) {
    Counts counts;
    auto run = [&](Settings s) {
        Controls c(s);
        c.take(0, std::nullopt, counts);
        counts.add(5, 5);
        return c.take(10'000, std::nullopt, counts);
    };
    auto base = run(Settings());
    Settings fast_s;
    fast_s.sensitivity = 2.0f;
    auto fast = run(fast_s);
    CHECK(std::fabs(fast.right_x - 2.0f * base.right_x) < 1e-6f);
    Settings inv_s;
    inv_s.invert_y = true;
    auto inverted = run(inv_s);
    CHECK_EQ(inverted.right_y, -base.right_y);
    CHECK_EQ(inverted.right_x, base.right_x);
    CHECK(base.right_y < 0.0f);   
}


TEST(kbm_rust_text_formats_and_parses_as_rust_does) {
    using namespace danzi::rust_text;
    CHECK_EQ(display(0.5f), "0.5");
    CHECK_EQ(display(100.0f), "100");
    CHECK_EQ(display(1e-7f), "0.0000001");
    CHECK_EQ(display(0.01f), "0.01");
    CHECK_EQ(display(-0.0f), "-0");
    CHECK_EQ(display(1e20f), "100000000000000000000");
    CHECK_EQ(display(std::nanf("")), "NaN");
    CHECK_EQ(display(-INFINITY), "-inf");
    CHECK_EQ(debug_str("a\"b\\\n\x01\x7f\xc2\xa0\xcc\x81z"), "\"a\\\"b\\\\\\n\\u{1}\\u{7f}\\u{a0}\\u{301}z\"");
    CHECK_EQ(trim("\xe3\x80\x80 x \t\xc2\x85"), "x");
    CHECK_EQ(parse_f32("1."), std::optional(1.0f));
    CHECK_EQ(parse_f32(".5"), std::optional(0.5f));
    CHECK(!parse_f32("."));
    CHECK(!parse_f32("1e"));
    CHECK(!parse_f32("0x1"));
    CHECK_EQ(parse_f32("+2.5E1"), std::optional(25.0f));
    CHECK_EQ(parse_f32("1e50"), std::optional(INFINITY));
    CHECK_EQ(parse_f32("InFiNiTy"), std::optional(INFINITY));
    CHECK_EQ(parse_u8("+7"), std::optional(std::uint8_t{7}));
    CHECK(!parse_u8("-0"));
    CHECK(!parse_u8("256"));
}
