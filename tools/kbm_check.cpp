// SPDX-License-Identifier: GPL-3.0-or-later
// `kbm-check <script> <out>`: the port's `kbm` driven by a script, printing
// what the reference's is printed by build/kbm-oracle (the same commands;
// tools/check_kbm.py makes scripts and compares).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/kbm.hpp"

using namespace danzi::kbm;
namespace toml = danzi::toml;

namespace {

std::string unhex(const std::string& t) {
    std::string out;
    for (std::size_t i = 1; i + 1 < t.size(); i += 2) {
        out += static_cast<char>(std::stoi(t.substr(i, 2), nullptr, 16));
    }
    return out;
}

std::string esc(const std::string& s) {
    std::string out;
    for (unsigned char b : s) {
        if (b >= 0x20 && b <= 0x7e && b != '\\') {
            out += static_cast<char>(b);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", b);
            out += buf;
        }
    }
    return out;
}

float f32_of(const std::string& t) {
    std::uint32_t bits = static_cast<std::uint32_t>(std::stoul(t, nullptr, 16));
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::uint32_t bits_of(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

std::vector<std::string> split(const std::string& s, char at) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t i = s.find(at, start);
        if (i == std::string::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, i - start));
        start = i + 1;
    }
}

std::optional<PadState> pad_of(const std::string& t) {
    if (t == "-") {
        return std::nullopt;
    }
    auto v = split(t, ',');
    return PadState{static_cast<std::uint32_t>(std::stoul(v[0], nullptr, 16)), f32_of(v[1]), f32_of(v[2]), f32_of(v[3]), f32_of(v[4]),
                    f32_of(v[5]), f32_of(v[6])};
}

std::string show_pad(const PadState& p) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%x %08x %08x %08x %08x %08x %08x", p.buttons, bits_of(p.left_trigger), bits_of(p.right_trigger),
                  bits_of(p.left_x), bits_of(p.left_y), bits_of(p.right_x), bits_of(p.right_y));
    return buf;
}


std::string debug(const Layout& l) {
    std::string out = "Layout({";
    bool first = true;
    for (const auto& [vk, outs] : l.keys) {
        out += (first ? "" : ", ") + std::to_string(vk) + ": [";
        first = false;
        for (std::size_t i = 0; i < outs.size(); ++i) {
            const Output& o = outs[i];
            out += i ? ", " : "";
            switch (o.kind) {
            case Output::Kind::Button: out += "Button(" + std::to_string(o.button) + ")"; break;
            case Output::Kind::LeftTrigger: out += "LeftTrigger"; break;
            case Output::Kind::RightTrigger: out += "RightTrigger"; break;
            case Output::Kind::Stick:
                out += std::string("Stick { right: ") + (o.right ? "true" : "false") + ", x: " + (o.x ? "true" : "false") +
                       ", positive: " + (o.positive ? "true" : "false") + " }";
                break;
            case Output::Kind::Walk: out += "Walk"; break;
            case Output::Kind::ToggleVehicle: out += "ToggleVehicle"; break;
            case Output::Kind::ReleaseCursor: out += "ReleaseCursor"; break;
            }
        }
        out += "]";
    }
    return out + "})";
}

void dump(std::string& out, const Settings& s) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "  S en=%d sens=%08x ys=%08x inv=%d cap=%d carry=%d dz=%08x curve=%08x walk=%08x\n", s.enabled,
                  bits_of(s.sensitivity), bits_of(s.y_scale), s.invert_y, s.capture, s.carry, bits_of(s.deadzone), bits_of(s.curve),
                  bits_of(s.walk_speed));
    out += buf;
    out += "  foot " + debug(s.on_foot) + "\n";
    out += "  veh " + debug(s.vehicle) + "\n";
}

toml::Value leaf(const std::string& t) {
    std::string kind = t.substr(0, t.find(':')), rest = t.substr(t.find(':') + 1);
    if (kind == "s") {
        return toml::Value{unhex(rest)};
    }
    if (kind == "i") {
        return toml::Value{static_cast<std::int64_t>(std::stoll(rest))};
    }
    if (kind == "f") {
        std::uint64_t bits = std::stoull(rest, nullptr, 16);
        double d;
        std::memcpy(&d, &bits, 8);
        return toml::Value{d};
    }
    if (kind == "b") {
        return toml::Value{rest == "1"};
    }
    return toml::Value{toml::Datetime{unhex(rest)}};
}

toml::Table table(const std::vector<std::string>& t, std::size_t& i);

std::optional<toml::Value> value_or_end(const std::vector<std::string>& t, std::size_t& i, const char* end) {
    const std::string& tok = t[i++];
    if (tok == end) {
        return std::nullopt;
    }
    if (tok == "[") {
        toml::Array a;
        while (auto v = value_or_end(t, i, "]")) {
            a.push_back(std::move(*v));
        }
        return toml::Value{std::move(a)};
    }
    if (tok == "{") {
        return toml::Value{table(t, i)};
    }
    return leaf(tok);
}


toml::Table table(const std::vector<std::string>& t, std::size_t& i) {
    toml::Table out;
    while (true) {
        const std::string& k = t[i++];
        if (k == "}") {
            return out;
        }
        std::string key = unhex(k.substr(2));
        auto v = value_or_end(t, i, "}");
        out.set(std::move(key), std::move(*v));
    }
}

}   

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: kbm-check <script> <out>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::string out;
    Settings s;
    Controls c(s);
    Counts counts;
    std::string line;
    for (std::size_t n = 1; std::getline(in, line); ++n) {
        std::istringstream words(line);
        std::vector<std::string> t;
        for (std::string w; words >> w;) {
            t.push_back(w);
        }
        if (t.empty() || t[0][0] == '#') {
            continue;
        }
        out += "#" + std::to_string(n) + " " + t[0] + "\n";
        const std::string& cmd = t[0];
        if (cmd == "settings") {
            s = Settings();
        } else if (cmd == "sset") {
            const std::string& f = t[1];
            if (f == "enabled") s.enabled = t[2] == "1";
            else if (f == "invert_y") s.invert_y = t[2] == "1";
            else if (f == "capture") s.capture = t[2] == "1";
            else if (f == "carry") s.carry = t[2] == "1";
            else if (f == "sensitivity") s.sensitivity = f32_of(t[2]);
            else if (f == "y_scale") s.y_scale = f32_of(t[2]);
            else if (f == "deadzone") s.deadzone = f32_of(t[2]);
            else if (f == "curve") s.curve = f32_of(t[2]);
            else if (f == "walk_speed") s.walk_speed = f32_of(t[2]);
        } else if (cmd == "new") {
            c = Controls(s);
        } else if (cmd == "cs") {
            c.s = s;
        } else if (cmd == "feed") {
            std::vector<std::uint8_t> keys;
            if (t[1] != "-") {
                for (const auto& k : split(t[1], ',')) {
                    keys.push_back(static_cast<std::uint8_t>(std::stoul(k, nullptr, 16)));
                }
            }
            c.feed(keys, counts);
        } else if (cmd == "add") {
            counts.add(std::stoll(t[1]), std::stoll(t[2]));
        } else if (cmd == "wheel") {
            counts.add_wheel(std::stoi(t[1]));
        } else if (cmd == "peek") {
            out += "  " + show_pad(c.peek(pad_of(t[1]))) + "\n";
        } else if (cmd == "take") {
            out += "  " + show_pad(c.take(std::stoull(t[1]), pad_of(t[2]), counts)) + "\n";
        } else if (cmd == "script") {
            c.set_script(*pad_of(t[1]));
        } else if (cmd == "merged") {
            auto a = *pad_of(t[1]), b = *pad_of(t[2]);
            out += "  " + show_pad(a.merged(b)) + " " + (a.is_idle() ? "1" : "0") + "\n";
        } else if (cmd == "clear") {
            c.release_cursor = false;
        } else if (cmd == "state") {
            auto used = c.last_used();
            out += std::string("  release=") + (c.release_cursor ? "1" : "0") +
                   " used=" + (!used ? "none" : *used == Owner::Pad ? "pad" : "kbm") + "\n";
        } else if (cmd == "keycode") {
            auto r = key_code(unhex(t[1]));
            char buf[8];
            std::snprintf(buf, sizeof buf, "%02x", r ? *r : 0);
            out += "  " + (r ? std::string(buf) : std::string("none")) + "\n";
        } else if (cmd == "keyname") {
            out += "  " + esc(key_name(static_cast<std::uint8_t>(std::stoul(t[1], nullptr, 16)))) + "\n";
        } else if (cmd == "caption") {
            out += "  " + esc(caption(unhex(t[1]), t[2] == "1")) + "\n";
        } else if (cmd == "keymap") {
            for (const auto& p : s.apply_keymap(unhex(t[1]))) {
                out += "  ! " + esc(p) + "\n";
            }
            dump(out, s);
        } else if (cmd == "env") {
            std::vector<std::pair<std::string, std::string>> vars;
            std::vector<std::pair<std::string, std::variant<std::string, IoError>>> files;
            for (std::size_t k = 1; k < t.size(); ++k) {
                std::string kind = t[k].substr(0, t[k].find(':')), rest = t[k].substr(t[k].find(':') + 1);
                std::string a = rest.substr(0, rest.find('=')), b = rest.substr(rest.find('=') + 1);
                if (kind == "get") {
                    vars.emplace_back(unhex(a), unhex(b));
                } else if (kind == "file") {
                    files.emplace_back(unhex(a), unhex(b));
                } else {
                    files.emplace_back(unhex(a), IoError{unhex(b)});
                }
            }
            auto get = [&](std::string_view k) -> std::optional<std::string> {
                for (const auto& [name, v] : vars) {
                    if (name == k) {
                        return v;
                    }
                }
                return std::nullopt;
            };
            auto read = [&](std::string_view p) -> std::variant<std::string, IoError> {
                for (const auto& [name, r] : files) {
                    if (name == p) {
                        return r;
                    }
                }
                return IoError{"no such file here"};
            };
            for (const auto& p : s.apply_env(get, read)) {
                out += "  ! " + esc(p) + "\n";
            }
            dump(out, s);
        } else if (cmd == "parse") {
            std::size_t i = 2;   
            auto tbl = table(t, i);
            auto r = Settings::parse(tbl);
            if (auto ok = std::get_if<Settings>(&r)) {
                s = *ok;
                dump(out, s);
            } else {
                out += "  err " + esc(std::get<std::string>(r)) + "\n";
            }
        } else if (cmd == "dump") {
            dump(out, s);
        } else {
            std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
            return 2;
        }
    }
    std::ofstream(argv[2], std::ios::binary) << out;
    return 0;
}
