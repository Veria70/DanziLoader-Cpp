// SPDX-License-Identifier: GPL-3.0-or-later
// `gameinput.dll`: input devices from the host (`victory-hle/src/modules/
// gameinput.rs`, with `controls.rs`'s re-exports of `victory_core::kbm`;
// DanziLoader / victory-perf, see NOTICE.md). Gamepads come from XInput
// (host slots 0-3). The keyboard and mouse play as a pad (`kbm`): on the
// first connected host pad, or on one stand-in gamepad while none is
// connected. With `[controls] enabled = false` the stand-in pad is the old
// fixed layout (`FALLBACK_BUTTONS`). The keyboard is a device of its own
// too. A poll thread turns host state into readings (a new one per change)
// and runs device callbacks on connect and disconnect; a title's pad read
// makes one too, carrying the mouse motion since its last read.
//
// The mouse cursor (`cursor`) is in gameinput_cursor.cpp. Debug and trace
// log lines are not ported (a release build logs info and above), nor the
// reference's `diagnostics`-feature debugging aids (`VICTORY_KEYS`,
// `VICTORY_KBM_SCRIPT`).
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "core/env.hpp"
#include "core/kbm.hpp"
#include "core/log.hpp"
#include "core/rust_text.hpp"
#include "modules.hpp"

#pragma comment(lib, "xinput.lib")

namespace danzi::hle::gameinput {

namespace {

using namespace std::chrono_literals;
using kbm::Controls;
using kbm::Counts;
using kbm::PadState;
using Clock = std::chrono::steady_clock;


constexpr HRESULT_ DEVICE_DISCONNECTED = static_cast<HRESULT_>(0x838A0001u);
constexpr HRESULT_ DEVICE_NOT_FOUND = static_cast<HRESULT_>(0x838A0002u);
constexpr HRESULT_ READING_NOT_FOUND = static_cast<HRESULT_>(0x838A0003u);
constexpr HRESULT_ REFERENCE_TOO_OLD = static_cast<HRESULT_>(0x838A0004u);
constexpr HRESULT_ E_NOINTERFACE_ = static_cast<HRESULT_>(0x80004002u);


constexpr std::uint32_t KEYBOARD_KIND = 0x10;
constexpr std::uint32_t GAMEPAD_KIND = 0x4'0000;

constexpr std::uint32_t STATUS_CONNECTED = 0x3;
constexpr std::uint32_t ASYNC_ENUMERATION = 1;


constexpr std::size_t PADS = 4;
constexpr std::size_t FALLBACK = PADS;
constexpr std::size_t KEYBOARD = PADS + 1;
constexpr std::size_t DEVICES = PADS + 2;


constexpr std::size_t HISTORY = 32;
constexpr auto POLL = 8ms;

constexpr const char* TARGET = "victory_hle::modules::gameinput";


struct KeyButton {
    std::uint8_t vk;
    std::uint32_t bit;
};
constexpr KeyButton FALLBACK_BUTTONS[14] = {
    {0x1B, kbm::MENU},   
    {0x09, kbm::VIEW},   
    {0x0D, kbm::A},      
    {0x08, kbm::B},      
    {'X', kbm::X},
    {'Y', kbm::Y},
    {0x26, kbm::UP},   
    {0x28, kbm::DOWN},
    {0x25, kbm::LEFT},
    {0x27, kbm::RIGHT},
    {'Q', kbm::LEFT_SHOULDER},
    {'E', kbm::RIGHT_SHOULDER},
    {'F', kbm::LEFT_STICK},
    {'H', kbm::RIGHT_STICK},
};


struct KeyState {
    std::uint32_t scan_code = 0;
    std::uint32_t code_point = 0;
    std::uint8_t virtual_key = 0;
    bool dead_key = false;
    bool operator==(const KeyState&) const = default;
};
static_assert(sizeof(KeyState) == 12);


class Source {
public:
    virtual ~Source() = default;

    virtual std::optional<PadState> pad(std::size_t slot) = 0;

    virtual std::vector<KeyState> keys() = 0;


    virtual std::vector<std::uint8_t> mouse_buttons() { return {}; }

    virtual void rumble(std::size_t slot, std::uint16_t low, std::uint16_t high) = 0;
};

PadState fallback(const std::vector<KeyState>& keys) {
    auto on = [&](std::uint8_t vk) {
        return std::any_of(keys.begin(), keys.end(), [&](const KeyState& k) { return k.virtual_key == vk; }) ? 1.0f : 0.0f;
    };
    PadState p;
    for (const auto& [vk, bit] : FALLBACK_BUTTONS) {
        if (on(vk) != 0.0f) {
            p.buttons |= bit;
        }
    }
    p.left_trigger = on('Z');
    p.right_trigger = on('C');
    p.left_x = on('D') - on('A');
    p.left_y = on('W') - on('S');
    p.right_x = on('L') - on('J');
    p.right_y = on('I') - on('K');
    return p;
}

struct Hub;


struct InputObject {
    const void* const* vtable;
    Hub* hub;
};


struct DeviceInfo {
    std::uint32_t size;
    std::uint16_t vendor, product, revision;
    std::uint8_t interface_, collection;
    std::uint16_t usage[2];
    std::uint16_t versions[8];
    std::uint8_t id[32];
    std::uint8_t root_id[32];
    std::int32_t family;
    std::uint32_t capabilities, supported_input, rumble_motors;

    std::uint64_t rest[26];
};
static_assert(offsetof(DeviceInfo, id) == 0x20);
static_assert(sizeof(DeviceInfo) == 0x140);

std::uint32_t kind(std::size_t index) { return index == KEYBOARD ? KEYBOARD_KIND : GAMEPAD_KIND; }

struct Device {
    const void* const* vtable;
    Hub* hub;
    std::size_t index;
    DeviceInfo info;
};

using Value = std::variant<PadState, std::vector<KeyState>>;

struct Reading {
    const void* const* vtable;
    std::atomic<std::uint32_t> refs;
    const Device* device;
    std::uint64_t sequence;
    std::uint64_t timestamp;
    Value value;

    std::uint32_t kind() const { return std::holds_alternative<PadState>(value) ? GAMEPAD_KIND : KEYBOARD_KIND; }
};

std::uint32_t __stdcall reading_add_ref(Reading* self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }
std::uint32_t __stdcall reading_release(Reading* self) {
    std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {

        delete self;
    }
    return left;
}


class Ref {
public:
    explicit Ref(Reading* r) : r_(r) {}
    Ref(Ref&& o) noexcept : r_(std::exchange(o.r_, nullptr)) {}
    Ref& operator=(Ref&& o) noexcept {
        if (this != &o) {
            reset();
            r_ = std::exchange(o.r_, nullptr);
        }
        return *this;
    }
    ~Ref() { reset(); }

    Reading* share() const {
        reading_add_ref(r_);
        return r_;
    }
    const Reading* operator->() const { return r_; }

private:
    void reset() {
        if (r_) {
            reading_release(std::exchange(r_, nullptr));
        }
    }
    Reading* r_;
};

using DeviceCallback = void(__stdcall*)(std::uint64_t, void*, const Device*, std::uint64_t, std::uint32_t, std::uint32_t);

struct Event;

struct Registration {
    std::uint64_t token;

    std::uintptr_t device;
    std::uint32_t kind;
    std::uint32_t status;
    std::uintptr_t context;
    DeviceCallback callback;

    bool matches(const std::array<Device, DEVICES>& devices, std::size_t index) const {
        return (gameinput::kind(index) & kind) != 0 && (device == 0 || device == reinterpret_cast<std::uintptr_t>(&devices[index]));
    }
    Event event(std::size_t device_index, std::uint64_t timestamp, std::uint32_t current, std::uint32_t previous) const;
};

struct Event {
    Registration registration;
    std::size_t device;
    std::uint64_t timestamp;
    std::uint32_t current, previous;
};

Event Registration::event(std::size_t device_index, std::uint64_t timestamp, std::uint32_t current, std::uint32_t previous) const {
    return {*this, device_index, timestamp, current, previous};
}

struct Slot {
    bool connected = false;
    std::uint64_t sequence = 0;
    std::deque<Ref> history;
};


std::uint64_t now() {
    static const Clock::time_point START = Clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - START).count());
}


std::optional<std::string> read_text(const std::string& path) {
    std::ifstream in(std::filesystem::path(xgr::utf16(path)), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad() || !rust_text::valid_utf8(text)) {
        return std::nullopt;
    }
    return text;
}


void live_press(std::vector<KeyState>& keys) {
    static const std::optional<std::string> PATH = [] () -> std::optional<std::string> {
        auto v = env::get(L"VICTORY_PRESS_FILE");
        return v ? rust_text::utf8_strict(*v) : std::nullopt;
    }();
    static std::mutex q_lock;
    static std::optional<Clock::time_point> last_read;
    static std::vector<std::uint8_t> queue;
    static std::optional<Clock::time_point> since;
    if (!PATH) {
        return;
    }
    std::lock_guard lock(q_lock);
    auto t = Clock::now();
    if (queue.empty() && (!last_read || t - *last_read >= 50ms)) {
        last_read = t;
        auto s = read_text(*PATH);
        if (s && !rust_text::trim(*s).empty()) {
            std::ofstream(std::filesystem::path(xgr::utf16(*PATH)), std::ios::binary | std::ios::trunc);
            queue.clear();
            auto words = rust_text::split_whitespace(*s);
            for (auto it = words.rbegin(); it != words.rend(); ++it) {
                std::string b(*it);
                for (char& c : b) {
                    c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
                }
                int vk = -1;
                if (b == "A") vk = 0x0D;
                else if (b == "B") vk = 0x08;
                else if (b == "UP") vk = 0x26;
                else if (b == "DOWN") vk = 0x28;
                else if (b == "LEFT") vk = 0x25;
                else if (b == "RIGHT") vk = 0x27;
                else if (b == "MENU" || b == "START") vk = 0x1B;
                else if (b == "VIEW") vk = 0x09;
                else if (b == "LS_UP") vk = 'W';
                else if (b == "LS_DOWN") vk = 'S';
                else if (b == "LS_LEFT") vk = 'A';
                else if (b == "LS_RIGHT") vk = 'D';
                else if (b == "RS_UP") vk = 'I';
                else if (b == "RS_DOWN") vk = 'K';
                else if (b == "RS_LEFT") vk = 'J';
                else if (b == "RS_RIGHT") vk = 'L';
                else if (b == "X") vk = 'X';
                else if (b == "Y") vk = 'Y';
                if (vk >= 0) {
                    queue.push_back(static_cast<std::uint8_t>(vk));
                }
            }
            since = t;
        }
    }
    if (queue.empty()) {
        return;
    }
    std::uint8_t vk = queue.back();
    auto held = since ? t - *since : Clock::duration::zero();

    auto hold = std::string_view("WASDIJKL").find(static_cast<char>(vk)) != std::string_view::npos ? 1500ms : 150ms;
    if (held < hold) {
        keys.push_back(KeyState{0, 0, vk, false});
    } else if (held >= hold + 150ms) {
        queue.pop_back();
        since = t;
    }
}


void auto_input_inject(std::vector<KeyState>& keys) {
    live_press(keys);
    static const bool AUTO = env::get(L"VICTORY_AUTO_INPUT").has_value();
    if (!AUTO) {
        return;
    }
    static const Clock::time_point START = Clock::now();
    auto elapsed = Clock::now() - START;
    if (elapsed > 120s) {
        return;
    }
    auto cycle = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() % 2000;
    if (cycle < 100) {
        keys.push_back(KeyState{0x1C, 0x0D, 0x0D, false});
    }
}

extern const void* const INPUT_VTABLE[22];
extern const void* const DEVICE_VTABLE[21];
extern const void* const READING_VTABLE[25];

struct State {
    std::array<Slot, DEVICES> slots;
    std::vector<Registration> callbacks;

    std::vector<Event> pending;

    std::vector<std::pair<std::uint64_t, std::thread::id>> running;

    std::optional<Controls> controls;

    std::array<std::optional<PadState>, PADS> host_pads;
    std::vector<KeyState> keys;

    std::optional<PadState> script;

    std::vector<Event> poll(const std::array<Device, DEVICES>& devices, const std::array<std::optional<PadState>, PADS>& pads,
                            std::vector<KeyState> host_keys, const std::vector<std::uint8_t>& mouse_buttons, Counts& counts);
    void publish(const std::array<Device, DEVICES>& devices, std::uint64_t t, Counts* take, std::vector<Event>& events);
    void title_read(const std::array<Device, DEVICES>& devices, Counts& counts);
    void update(const std::array<Device, DEVICES>& devices, std::size_t index, std::optional<Value> value, std::uint64_t t,
                std::vector<Event>& events);
};

struct Hub {
    InputObject object;
    std::array<Device, DEVICES> devices;


    std::mutex source_lock;
    std::unique_ptr<Source> source;

    std::shared_ptr<Counts> counts;
    std::mutex state_lock;
    State state;

    std::condition_variable returned;

    Hub(std::unique_ptr<Source> src, std::shared_ptr<Counts> c, const kbm::Settings& settings)
        : object{INPUT_VTABLE, this}, source(std::move(src)), counts(std::move(c)) {
        cursor::ENABLED.store(settings.enabled && settings.capture, std::memory_order_relaxed);
        for (std::size_t index = 0; index < DEVICES; ++index) {
            Device& d = devices[index];
            std::memset(&d.info, 0, sizeof d.info);
            d.vtable = DEVICE_VTABLE;
            d.hub = this;
            d.index = index;
            d.info.size = sizeof(DeviceInfo);
            std::memcpy(d.info.id, "victory", 7);
            d.info.id[31] = static_cast<std::uint8_t>(index + 1);
            std::memcpy(d.info.root_id, d.info.id, 32);
            d.info.supported_input = kind(index);

            d.info.rumble_motors = index < PADS ? 0x3 : 0;
        }
        if (settings.enabled) {
            state.controls.emplace(settings);
        }
    }


    static Hub* start(std::unique_ptr<Source> src, std::shared_ptr<Counts> c, const kbm::Settings& settings) {
        Hub* hub = new Hub(std::move(src), std::move(c), settings);
        hub->poll();
        std::thread([hub] {
            while (true) {
                std::this_thread::sleep_for(POLL);
                hub->poll();
            }
        }).detach();
        return hub;
    }


    void poll() {
        std::array<std::optional<PadState>, PADS> pads;
        std::vector<KeyState> keys;
        std::vector<std::uint8_t> buttons;
        {
            std::lock_guard lock(source_lock);
            for (std::size_t slot = 0; slot < PADS; ++slot) {
                pads[slot] = source->pad(slot);
            }
            keys = source->keys();
            buttons = source->mouse_buttons();
        }
        std::vector<Event> events;
        {
            std::lock_guard lock(state_lock);
            events = state.poll(devices, pads, std::move(keys), buttons, *counts);
            if (state.controls && std::exchange(state.controls->release_cursor, false)) {
                cursor::release("Alt key");
            }
        }
        deliver(std::move(events));
    }


    void deliver(std::vector<Event> events) {
        auto me = std::this_thread::get_id();
        for (const Event& e : events) {
            const Registration& r = e.registration;
            {
                std::lock_guard lock(state_lock);
                if (std::none_of(state.callbacks.begin(), state.callbacks.end(), [&](const Registration& c) { return c.token == r.token; })) {
                    continue;
                }
                state.running.emplace_back(r.token, me);
            }
            r.callback(r.token, reinterpret_cast<void*>(r.context), &devices[e.device], e.timestamp, e.current, e.previous);
            std::lock_guard lock(state_lock);
            auto& running = state.running;
            auto it = std::find(running.begin(), running.end(), std::pair{r.token, me});
            if (it != running.end()) {
                *it = running.back();
                running.pop_back();
            }
            returned.notify_all();
        }
    }

    std::optional<std::size_t> index(const Device* device) const {
        for (std::size_t i = 0; i < DEVICES; ++i) {
            if (&devices[i] == device) {
                return i;
            }
        }
        return std::nullopt;
    }
};

std::vector<Event> State::poll(const std::array<Device, DEVICES>& devices, const std::array<std::optional<PadState>, PADS>& pads,
                               std::vector<KeyState> host_keys, const std::vector<std::uint8_t>& mouse_buttons, Counts& counts) {
    std::uint64_t t = now();
    std::vector<Event> events = std::exchange(pending, {});


    std::vector<KeyState> scripted;
    auto_input_inject(scripted);
    std::optional<PadState> script_pad;
    if (!scripted.empty()) {
        script_pad = fallback(scripted);
    }
    if (controls) {
        std::vector<std::uint8_t> held;
        for (const auto& k : host_keys) {
            held.push_back(k.virtual_key);
        }
        held.insert(held.end(), mouse_buttons.begin(), mouse_buttons.end());
        controls->feed(held, counts);
        controls->set_script(script_pad.value_or(PadState{}));
    }
    script = script_pad;
    host_pads = pads;
    keys = host_keys;
    publish(devices, t, nullptr, events);
    host_keys.insert(host_keys.end(), scripted.begin(), scripted.end());
    update(devices, KEYBOARD, Value(std::move(host_keys)), t, events);
    return events;
}


void State::publish(const std::array<Device, DEVICES>& devices, std::uint64_t t, Counts* take, std::vector<Event>& events) {
    auto pads = host_pads;
    std::optional<std::size_t> first;
    for (std::size_t i = 0; i < PADS; ++i) {
        if (pads[i]) {
            first = i;
            break;
        }
    }
    std::array<std::optional<PadState>, PADS + 1> values;
    std::copy(pads.begin(), pads.end(), values.begin());
    std::size_t target = first.value_or(FALLBACK);
    if (controls) {
        std::optional<PadState> base = first ? pads[*first] : std::nullopt;
        values[target] = take ? controls->take(t, base, *take) : controls->peek(base);
    } else {


        std::optional<PadState> base = first ? pads[*first] : std::optional(fallback(keys));
        if (base && script) {
            values[target] = base->merged(*script);
        } else if (base) {
            values[target] = base;
        } else {
            values[target] = script;
        }
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        update(devices, index, values[index] ? std::optional<Value>(*values[index]) : std::nullopt, t, events);
    }
}


void State::title_read(const std::array<Device, DEVICES>& devices, Counts& counts) {
    if (!controls) {
        return;
    }
    std::vector<Event> events;
    publish(devices, now(), &counts, events);

    pending.insert(pending.end(), events.begin(), events.end());
}


void State::update(const std::array<Device, DEVICES>& devices, std::size_t index, std::optional<Value> value, std::uint64_t t,
                   std::vector<Event>& events) {
    Slot& slot = slots[index];
    bool was = slot.connected;
    slot.connected = value.has_value();
    if (value && (!was || slot.history.empty() || slot.history.back()->value != *value)) {
        slot.sequence += 1;
        if (slot.history.size() == HISTORY) {
            slot.history.pop_front();
        }
        slot.history.emplace_back(new Reading{READING_VTABLE, 1, &devices[index], slot.sequence, t, std::move(*value)});
    }
    if (was != slot.connected) {
        log::info(TARGET, "input device", {{"device", log::debug(static_cast<std::uint64_t>(index))}, {"connected", log::debug(!was)}});
        auto [current, previous] = was ? std::pair{0u, STATUS_CONNECTED} : std::pair{STATUS_CONNECTED, 0u};
        for (const Registration& r : callbacks) {
            if (r.matches(devices, index) && (r.status & STATUS_CONNECTED) != 0) {
                events.push_back(r.event(index, t, current, previous));
            }
        }
    }
}


constexpr auto NO_FOREGROUND = 1500ms;

constexpr auto EMPTY_SLOT_RECHECK = 1s;

struct PadButton {
    WORD host;
    std::uint32_t bit;
};
constexpr PadButton BUTTONS[14] = {
    {XINPUT_GAMEPAD_START, kbm::MENU},
    {XINPUT_GAMEPAD_BACK, kbm::VIEW},
    {XINPUT_GAMEPAD_A, kbm::A},
    {XINPUT_GAMEPAD_B, kbm::B},
    {XINPUT_GAMEPAD_X, kbm::X},
    {XINPUT_GAMEPAD_Y, kbm::Y},
    {XINPUT_GAMEPAD_DPAD_UP, kbm::UP},
    {XINPUT_GAMEPAD_DPAD_DOWN, kbm::DOWN},
    {XINPUT_GAMEPAD_DPAD_LEFT, kbm::LEFT},
    {XINPUT_GAMEPAD_DPAD_RIGHT, kbm::RIGHT},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, kbm::LEFT_SHOULDER},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, kbm::RIGHT_SHOULDER},
    {XINPUT_GAMEPAD_LEFT_THUMB, kbm::LEFT_STICK},
    {XINPUT_GAMEPAD_RIGHT_THUMB, kbm::RIGHT_STICK},
};


PadState convert(const XINPUT_GAMEPAD& pad) {

    auto stick = [](SHORT v) {
        float f = static_cast<float>(v) / 32767.0f;
        return f > -1.0f ? f : -1.0f;
    };
    PadState p;
    for (const auto& [host, bit] : BUTTONS) {
        if ((pad.wButtons & host) != 0) {
            p.buttons |= bit;
        }
    }
    p.left_trigger = static_cast<float>(pad.bLeftTrigger) / 255.0f;
    p.right_trigger = static_cast<float>(pad.bRightTrigger) / 255.0f;
    p.left_x = stick(pad.sThumbLX);
    p.left_y = stick(pad.sThumbLY);
    p.right_x = stick(pad.sThumbRX);
    p.right_y = stick(pad.sThumbRY);
    return p;
}


struct Caption {
    std::uintptr_t window = 0;


    const std::wstring* plain = nullptr;
    const std::wstring* hinted = nullptr;
    std::optional<bool> shown;
};

class Host final : public Source {
public:
    explicit Host(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) {}

    std::optional<PadState> pad(std::size_t slot) override {
        if (empty_until_[slot] && Clock::now() < *empty_until_[slot]) {
            return std::nullopt;
        }
        XINPUT_STATE state{};
        if (XInputGetState(static_cast<DWORD>(slot), &state) != 0) {
            empty_until_[slot] = Clock::now() + EMPTY_SLOT_RECHECK;
            return std::nullopt;
        }
        empty_until_[slot].reset();
        return convert(state.Gamepad);
    }

    std::vector<KeyState> keys() override {
        bool focus = this->focus();
        hold_cursor(focus);
        std::vector<std::uint8_t> down;
        if (focus) {


            std::vector<std::uint8_t> held;
            for (int vk = 0x08; vk <= 0xFE; ++vk) {
                if ((vk >= 0x10 && vk <= 0x12) || std::find(down.begin(), down.end(), vk) != down.end()) {
                    continue;
                }
                if (GetAsyncKeyState(vk) < 0) {
                    held.push_back(static_cast<std::uint8_t>(vk));
                }
            }
            down.insert(down.end(), held.begin(), held.end());
        }
        std::vector<KeyState> out;
        for (std::uint8_t vk : down) {


            out.push_back(KeyState{MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX), 0, vk, false});
        }
        return out;
    }

    std::vector<std::uint8_t> mouse_buttons() override {
        bool captured = cursor::FOCUSED.load(std::memory_order_relaxed) && cursor::captured();
        std::vector<std::uint8_t> down;
        if (captured) {
            std::vector<std::uint8_t> held;
            for (std::uint8_t vk : kbm::MOUSE_BUTTONS) {
                if (GetAsyncKeyState(vk) < 0) {
                    held.push_back(vk);
                }
            }
            if (!was_captured_) {
                swallowed_ = held;
            }
            std::erase_if(swallowed_, [&](std::uint8_t b) { return std::find(held.begin(), held.end(), b) == held.end(); });
            for (std::uint8_t b : held) {
                if (std::find(swallowed_.begin(), swallowed_.end(), b) == swallowed_.end()) {
                    down.push_back(b);
                }
            }
        }
        was_captured_ = captured;
        return down;
    }

    void rumble(std::size_t slot, std::uint16_t low, std::uint16_t high) override {
        XINPUT_VIBRATION speeds{low, high};

        XInputSetState(static_cast<DWORD>(slot), &speeds);
    }

private:


    bool focus() {
        auto front = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
        if (front == 0) {
            if (!no_foreground_) {
                no_foreground_ = Clock::now();
            }
            if (Clock::now() - *no_foreground_ < NO_FOREGROUND) {
                return was_focused_;
            }
            lost_ = "no foreground window for too long";
            return false;
        }
        no_foreground_.reset();
        bool ours = cursor::ours(front);
        if (!ours) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(front));
            lost_ = std::string("another program's window ") + buf + " is in front";
        }
        return ours;
    }


    void title_hint(bool focus) {
        if (!cursor::ENABLED.load(std::memory_order_relaxed)) {
            return;
        }
        Caption& caption = caption_;
        if (focus) {
            std::uintptr_t hwnd = cursor::target();
            if (hwnd != caption.window) {
                wchar_t text[256] = {};
                int n = GetWindowTextW(reinterpret_cast<HWND>(hwnd), text, 256);
                std::string base = xgr::utf16_lossy(std::wstring_view(text, static_cast<std::size_t>(std::max(n, 0))));
                base = kbm::caption(base, true);
                caption.hinted = new std::wstring(xgr::utf16(kbm::caption(base, false)));
                caption.plain = new std::wstring(xgr::utf16(base));
                caption.window = hwnd;
                caption.shown.reset();
            }
        }
        if (caption.window == 0) {
            return;
        }
        bool captured = focus && cursor::captured();
        if (caption.shown != captured) {
            const std::wstring* text = captured ? caption.plain : caption.hinted;
            DWORD_PTR done = 0;

            LRESULT ok = SendMessageTimeoutW(reinterpret_cast<HWND>(caption.window), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text->c_str()),
                                             SMTO_ABORTIFHUNG, 250, &done);

            caption.shown = ok != 0 ? std::optional(captured) : std::nullopt;
        }
    }


    void hold_cursor(bool focus) {
        cursor::FOCUSED.store(focus, std::memory_order_relaxed);


        if (!focus) {
            std::string why = std::exchange(lost_, std::string());
            cursor::release(why.c_str());
        }
        was_focused_ = focus;
        title_hint(focus);
        if (focus && cursor::captured()) {


            clipped_ = cursor::clip(cursor::target()) || clipped_;
        } else if (clipped_) {

            ClipCursor(nullptr);
            clipped_ = false;
        }
    }

    Caption caption_;


    std::optional<Clock::time_point> no_foreground_;

    std::string lost_;
    std::array<std::optional<Clock::time_point>, PADS> empty_until_;

    std::shared_ptr<Counts> counts_;
    bool was_focused_ = false;
    bool was_captured_ = false;
    bool clipped_ = false;


    std::vector<std::uint8_t> swallowed_;
};


void raw_input(HRAWINPUT handle, Counts& counts, std::int32_t& wheel) {
    RAWINPUT raw{};
    UINT size = sizeof raw;
    UINT got = GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
    if (got == static_cast<UINT>(-1) || raw.header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    if (!(cursor::FOCUSED.load(std::memory_order_relaxed) && cursor::captured())) {
        wheel = 0;
        return;
    }
    const RAWMOUSE& mouse = raw.data.mouse;


    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
        counts.add(mouse.lLastX, mouse.lLastY);
    }
    if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
        wheel += static_cast<std::int16_t>(mouse.usButtonData);
        std::int32_t steps = wheel / 120;
        wheel -= steps * 120;
        if (steps != 0) {
            counts.add_wheel(steps);
        }
    }
}


void raw_mouse(std::shared_ptr<Counts> counts) {
    try {
        std::thread([counts] {
            SetThreadDescription(GetCurrentThread(), L"victory-mouse");
            const wchar_t* cls = L"victory.mouse";
            HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = instance;
            wc.lpszClassName = cls;
            RegisterClassW(&wc);
            HWND hwnd = CreateWindowExW(0, cls, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
            RAWINPUTDEVICE device{1, 2, RIDEV_INPUTSINK, hwnd};   
            if (!hwnd || !RegisterRawInputDevices(&device, 1, sizeof device)) {
                log::warn("victory_hle::modules::gameinput::host", "raw mouse input unavailable: no mouse look");
                return;
            }
            log::info("victory_hle::modules::gameinput::host", "raw mouse input on");
            std::int32_t wheel = 0;
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (msg.message == WM_INPUT) {
                    raw_input(reinterpret_cast<HRAWINPUT>(msg.lParam), *counts, wheel);
                }
                DispatchMessageW(&msg);
            }
        }).detach();
    } catch (const std::system_error&) {
        log::warn("victory_hle::modules::gameinput::host", "no raw mouse thread: no mouse look");
    }
}

std::unique_ptr<Source> host(const std::shared_ptr<Counts>& counts, const kbm::Settings& settings) {
    if (settings.enabled) {
        raw_mouse(counts);
    }
    return std::make_unique<Host>(counts);
}

Hub& the_hub() {
    static Hub* hub = [] {
        auto settings = kbm::settings();
        auto counts = std::make_shared<Counts>();
        auto source = host(counts, settings);
        return Hub::start(std::move(source), counts, settings);
    }();
    return *hub;
}


using This = const InputObject*;
using Out = const void**;


HRESULT_ fail(Out out, HRESULT_ hr) {
    if (out) {
        *out = nullptr;
    }
    return hr;
}


HRESULT_ hand(Out out, const Ref* reading) {
    if (!out) {
        return E_INVALIDARG_;
    }
    if (reading) {
        *out = reading->share();
        return S_OK_;
    }
    return fail(out, READING_NOT_FOUND);
}

HRESULT_ __stdcall no_interface(const void*, const void*, Out out) {
    log::warn(TARGET, "input QueryInterface not implemented");
    return fail(out, E_NOINTERFACE_);
}

std::uint32_t __stdcall add_ref(const void*) { return 1; }

std::uint64_t __stdcall timestamp(This) { return now(); }

HRESULT_ __stdcall current_reading(This self, std::uint32_t kind, const Device* device, Out out) {
    Hub& hub = *self->hub;
    std::lock_guard lock(hub.state_lock);
    State& state = hub.state;
    if ((kind & GAMEPAD_KIND) != 0) {
        state.title_read(hub.devices, *hub.counts);
    }
    auto newest = [&](std::size_t i) -> const Ref* {
        const auto& history = state.slots[i].history;
        if (history.empty() || (history.back()->kind() & kind) == 0) {
            return nullptr;
        }
        return &history.back();
    };
    const Ref* found = nullptr;
    if (!device) {

        for (std::size_t i = 0; i < DEVICES; ++i) {
            if (!state.slots[i].connected) {
                continue;
            }
            if (const Ref* r = newest(i); r && (!found || (*r)->timestamp >= (*found)->timestamp)) {
                found = r;
            }
        }
    } else {
        auto i = hub.index(device);
        if (!i) {
            return fail(out, E_INVALIDARG_);
        }
        if (!state.slots[*i].connected) {
            return fail(out, DEVICE_DISCONNECTED);
        }
        found = newest(*i);
    }
    return hand(out, found);
}


HRESULT_ relative(This self, const Reading* reference, std::uint32_t kind, const Device* device, Out out, bool forward) {
    if (!reference) {
        return fail(out, E_INVALIDARG_);
    }
    Hub& hub = *self->hub;


    auto index = hub.index(reference->device);
    if (!index) {
        return fail(out, E_INVALIDARG_);
    }
    if (device && device != reference->device) {
        return fail(out, E_INVALIDARG_);
    }
    std::lock_guard lock(hub.state_lock);
    const auto& history = hub.state.slots[*index].history;
    if (history.empty() || reference->sequence < history.front()->sequence) {
        return fail(out, REFERENCE_TOO_OLD);
    }
    const Ref* found = nullptr;
    if (forward) {
        for (const Ref& r : history) {
            if (r->sequence > reference->sequence && (r->kind() & kind) != 0) {
                found = &r;
                break;
            }
        }
    } else {
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if ((*it)->sequence < reference->sequence && ((*it)->kind() & kind) != 0) {
                found = &*it;
                break;
            }
        }
    }
    return hand(out, found);
}
HRESULT_ __stdcall next_reading(This self, const Reading* reference, std::uint32_t kind, const Device* device, Out out) {
    return relative(self, reference, kind, device, out, true);
}
HRESULT_ __stdcall previous_reading(This self, const Reading* reference, std::uint32_t kind, const Device* device, Out out) {
    return relative(self, reference, kind, device, out, false);
}


HRESULT_ __stdcall temporal_reading(This self, std::uint64_t time, const Device* device, Out out) {
    Hub& hub = *self->hub;
    auto index = hub.index(device);
    if (!index) {
        return fail(out, E_INVALIDARG_);
    }
    std::lock_guard lock(hub.state_lock);
    const auto& history = hub.state.slots[*index].history;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if ((*it)->timestamp <= time) {
            return hand(out, &*it);
        }
    }
    return hand(out, nullptr);
}

std::atomic<std::uint64_t> NEXT_TOKEN{1};

HRESULT_ token(std::uint64_t* out) {
    if (out) {
        *out = NEXT_TOKEN.fetch_add(1, std::memory_order_relaxed);
    }
    return S_OK_;
}


HRESULT_ __stdcall register_reading(This, const void*, std::uint32_t, float, void*, const void*, std::uint64_t* out) {
    log::warn(TARGET, "input reading callbacks are never called");
    return token(out);
}


HRESULT_ __stdcall register_device(This self, const Device* device, std::uint32_t kind, std::uint32_t status, std::uint32_t enumeration,
                                   void* context, DeviceCallback callback, std::uint64_t* out) {
    if (!callback) {
        return E_INVALIDARG_;
    }
    Hub& hub = *self->hub;
    Registration registration{NEXT_TOKEN.fetch_add(1, std::memory_order_relaxed), reinterpret_cast<std::uintptr_t>(device), kind, status,
                              reinterpret_cast<std::uintptr_t>(context), callback};
    std::vector<Event> found;
    {
        std::lock_guard lock(hub.state_lock);
        State& state = hub.state;
        if (enumeration != 0 && (status & STATUS_CONNECTED) != 0) {
            std::uint64_t t = now();
            for (std::size_t i = 0; i < DEVICES; ++i) {
                if (state.slots[i].connected && registration.matches(hub.devices, i)) {
                    found.push_back(registration.event(i, t, STATUS_CONNECTED, 0));
                }
            }
        }
        log::info(TARGET, "input device callback",
                  {{"kind", log::hex(kind)}, {"enumeration", log::debug(enumeration)}, {"devices", log::debug(static_cast<std::uint64_t>(found.size()))}});
        state.callbacks.push_back(registration);
        if (enumeration == ASYNC_ENUMERATION) {
            state.pending.insert(state.pending.end(), found.begin(), found.end());
            found.clear();
        }
    }
    if (out) {
        *out = registration.token;
    }
    hub.deliver(std::move(found));
    return S_OK_;
}

HRESULT_ __stdcall register_other(This, const void*, void*, const void*, std::uint64_t* out) { return token(out); }

void forget(This self, std::uint64_t t) {
    Hub& hub = *self->hub;
    std::lock_guard lock(hub.state_lock);
    std::erase_if(hub.state.callbacks, [&](const Registration& r) { return r.token == t; });
    std::erase_if(hub.state.pending, [&](const Event& e) { return e.registration.token == t; });
}
void __stdcall stop_callback(This self, std::uint64_t t) { forget(self, t); }


bool __stdcall unregister_callback(This self, std::uint64_t t, std::uint64_t timeout) {
    forget(self, t);
    Hub& hub = *self->hub;
    auto me = std::this_thread::get_id();

    std::optional<Clock::time_point> deadline;
    if (timeout <= 1'000'000'000'000'000ull) {
        deadline = Clock::now() + std::chrono::microseconds(timeout);
    }
    std::unique_lock lock(hub.state_lock);
    while (true) {
        std::size_t running = 0;
        for (const auto& [token, thread] : hub.state.running) {
            if (token == t) {
                if (thread == me) {
                    return false;
                }
                ++running;
            }
        }
        if (running == 0) {
            return true;
        }
        if (!deadline) {
            hub.returned.wait(lock);
        } else {
            if (Clock::now() > *deadline) {
                return false;
            }
            hub.returned.wait_until(lock, *deadline);
        }
    }
}


HRESULT_ __stdcall create_dispatcher(This, Out out) {
    log::warn(TARGET, "input dispatcher not implemented");
    return fail(out, E_NOTIMPL_);
}
HRESULT_ __stdcall create_aggregate(This, std::uint32_t, Out out) {
    log::warn(TARGET, "aggregate input device not implemented");
    return fail(out, E_NOTIMPL_);
}
HRESULT_ __stdcall find_by_id(This self, const std::uint8_t* id, Out out) {
    if (!id || !out) {
        return fail(out, E_INVALIDARG_);
    }
    for (const Device& device : self->hub->devices) {
        if (std::memcmp(device.info.id, id, 32) == 0) {
            *out = &device;
            return S_OK_;
        }
    }
    return fail(out, DEVICE_NOT_FOUND);
}


HRESULT_ __stdcall find_device(This, const void*, Out out) { return fail(out, DEVICE_NOT_FOUND); }
HRESULT_ __stdcall enable_oem_device(This, std::uint16_t, std::uint16_t, std::uint8_t, std::uint8_t) { return S_OK_; }
void __stdcall set_focus_policy(This, std::uint32_t) {}

#define F(x) reinterpret_cast<const void*>(&x)
const void* const INPUT_VTABLE[22] = {
    F(no_interface),          
    F(add_ref),               
    F(add_ref),               
    F(timestamp),             
    F(current_reading),       
    F(next_reading),          
    F(previous_reading),      
    F(temporal_reading),      
    F(register_reading),      
    F(register_device),       
    F(register_other),        
    F(register_other),        
    F(stop_callback),         
    F(unregister_callback),   
    F(create_dispatcher),     
    F(create_aggregate),      
    F(find_by_id),            
    F(find_device),           
    F(find_device),           
    F(find_device),           
    F(enable_oem_device),     
    F(set_focus_policy),      
};


const DeviceInfo* __stdcall device_info(const Device* self) { return &self->info; }
std::uint32_t __stdcall device_status(const Device* self) {
    std::lock_guard lock(self->hub->state_lock);
    return self->hub->state.slots[self->index].connected ? STATUS_CONNECTED : 0;
}


void __stdcall set_rumble(const Device* self, const float* params) {
    if (self->index >= PADS) {
        return;
    }
    float levels[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (params) {
        std::memcpy(levels, params, sizeof levels);
    }

    auto speed = [](float level) -> std::uint16_t {
        if (level < 0.0f) {
            level = 0.0f;
        }
        if (level > 1.0f) {
            level = 1.0f;
        }
        float v = level * 65535.0f;
        if (!(v > 0.0f)) {
            return 0;   
        }
        return v >= 65535.0f ? std::uint16_t{65535} : static_cast<std::uint16_t>(v);
    };
    std::lock_guard lock(self->hub->source_lock);
    self->hub->source->rumble(self->index, speed(levels[0]), speed(levels[1]));
}
template <std::size_t SLOT>
HRESULT_ __stdcall unsupported(const void*) {
    log::warn(TARGET, "input device method not implemented", {{"slot", log::hex(SLOT)}});
    return E_NOTIMPL_;
}

const void* const DEVICE_VTABLE[21] = {
    F(no_interface),           
    F(add_ref),                
    F(add_ref),                
    F(device_info),            
    F(device_status),          
    F(unsupported<0x28>),      
    F(unsupported<0x30>),      
    F(unsupported<0x38>),      
    F(unsupported<0x40>),      
    F(unsupported<0x48>),      
    F(set_rumble),             
    F(unsupported<0x58>),      
    F(unsupported<0x60>),      
    F(unsupported<0x68>),      
    F(unsupported<0x70>),      
    F(unsupported<0x78>),      
    F(unsupported<0x80>),      
    F(unsupported<0x88>),      
    F(unsupported<0x90>),      
    F(unsupported<0x98>),      
    F(unsupported<0xa0>),      
};


std::uint32_t __stdcall input_kind(const Reading* self) { return self->kind(); }
std::uint64_t __stdcall sequence_number(const Reading* self, std::uint32_t kind) {
    return (self->kind() & kind) != 0 ? self->sequence : 0;
}
std::uint64_t __stdcall reading_timestamp(const Reading* self) { return self->timestamp; }
void __stdcall reading_device(const Reading* self, const Device** out) {
    if (out) {

        *out = self->device;
    }
}
std::uint32_t __stdcall key_count(const Reading* self) {
    auto keys = std::get_if<std::vector<KeyState>>(&self->value);
    return keys ? static_cast<std::uint32_t>(keys->size()) : 0;
}
std::uint32_t __stdcall key_state(const Reading* self, std::uint32_t count, KeyState* out) {
    auto keys = std::get_if<std::vector<KeyState>>(&self->value);
    if (!keys || !out) {
        return 0;
    }
    std::size_t n = std::min<std::size_t>(keys->size(), count);
    std::memcpy(out, keys->data(), n * sizeof(KeyState));
    return static_cast<std::uint32_t>(n);
}
bool __stdcall gamepad_state(const Reading* self, PadState* out) {
    auto state = std::get_if<PadState>(&self->value);
    if (!state || !out) {
        return false;
    }
    *out = *state;
    return true;
}

std::size_t __stdcall nothing(const void*) { return 0; }

const void* const READING_VTABLE[25] = {
    F(no_interface),        
    F(reading_add_ref),     
    F(reading_release),     
    F(input_kind),          
    F(sequence_number),     
    F(reading_timestamp),   
    F(reading_device),      
    F(nothing),             
    F(nothing),             
    F(nothing),             
    F(nothing),             
    F(nothing),             
    F(nothing),             
    F(nothing),             
    F(key_count),           
    F(key_state),           
    F(nothing),             
    F(nothing),             
    F(nothing),             
    F(nothing),             
    F(nothing),             
    F(nothing),             
    F(gamepad_state),       
    F(nothing),             
    F(nothing),             
};
#undef F


HRESULT_ __stdcall GameInputCreate(This* out) {
    log::info(TARGET, "GameInputCreate");
    if (!out) {
        return E_INVALIDARG_;
    }
    *out = &the_hub().object;
    return S_OK_;
}

}   

std::vector<HleExport> exports() {

    return {HleExport{"#1", reinterpret_cast<std::uintptr_t>(&GameInputCreate)}};
}

}   
