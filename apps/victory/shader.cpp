// SPDX-License-Identifier: GPL-3.0-or-later
// `victory shader-info`: survey shader containers: bitcode, console
// additions and native code (`apps/victory/src/shader.rs`; DanziLoader /
// victory-perf, see NOTICE.md). Also what the shader commands share.
#include <windows.h>

#include <blake3.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <string_view>

#include "core/config.hpp"
#include "shader/dxbc.hpp"
#include "shader/error.hpp"
#include "shader/gfx10.hpp"
#include "shader/native.hpp"
#include "shader/sp3.hpp"
#include "shaders.hpp"

namespace danzi::victory {

using namespace danzi::shader;


std::optional<std::span<const std::uint8_t>> Source::bytes(std::vector<std::uint8_t>& storage) const {
    if (!packed) {
        return data;
    }
    auto unpacked = record.unpack();
    if (!unpacked) {
        return std::nullopt;
    }
    storage = std::move(*unpacked);
    return std::span<const std::uint8_t>(storage);
}

std::string Source::label() const {
    if (!packed) {
        return std::format("@{:#x}", offset);
    }
    return std::format("@{:#x} {}", record.offset, record.name ? *record.name : std::string_view("?"));
}

std::vector<Source> sources(std::span<const std::uint8_t> data) {
    std::vector<Source> out;
    if (sgd5::is_sgd5(data)) {
        for (const auto& r : sgd5::records(data)) {
            Source s;
            s.packed = true;
            s.record = r;
            out.push_back(s);
        }
    }
    static constexpr std::uint8_t DXBC[4] = {'D', 'X', 'B', 'C'};
    sgd5::find_iter(data, DXBC, [&](std::size_t offset) {
        auto rest = data.subspan(offset);
        try {
            Container::parse(rest);
        } catch (const std::exception&) {
            return;
        }


        std::size_t total = std::size_t{rest[0x18]} | std::size_t{rest[0x19]} << 8 | std::size_t{rest[0x1A]} << 16 | std::size_t{rest[0x1B]} << 24;
        Source s;
        s.offset = offset;
        s.data = rest.first(total);
        out.push_back(s);
    });
    return out;
}

namespace {

void walk(const fs::path& path, std::vector<WalkEntry>& out) {
    std::error_code ec;
    fs::directory_iterator it(path, ec);
    if (ec) {
        return;
    }
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            return;
        }
        const fs::directory_entry& e = *it;
        std::error_code tec;
        auto st = e.symlink_status(tec);
        if (tec) {
            continue;
        }
        if (fs::is_regular_file(st)) {
            std::error_code sec;
            auto len = e.file_size(sec);
            out.push_back({e.path(), sec ? 0 : static_cast<std::uint64_t>(len)});
        } else if (fs::is_directory(st)) {
            walk(e.path(), out);
        }
    }
}

}   

std::vector<WalkEntry> walk_files(const fs::path& root) {
    std::vector<WalkEntry> out;
    std::error_code ec;

    auto st = fs::status(root, ec);
    if (ec) {
        return out;
    }
    if (fs::is_regular_file(st)) {
        auto len = fs::file_size(root, ec);
        out.push_back({root, ec ? 0 : static_cast<std::uint64_t>(len)});
    } else if (fs::is_directory(st)) {
        walk(root, out);
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> read_whole(const fs::path& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size)) {
        data.reserve(static_cast<std::size_t>(size.QuadPart));
    }
    bool ok = true;
    std::size_t at = 0;
    for (;;) {
        if (data.size() - at < 65536) {
            data.resize(std::max<std::size_t>(at + 65536, data.capacity()));
        }
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - at, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(f, data.data() + at, want, &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        at += got;
    }
    CloseHandle(f);
    if (!ok) {
        return std::nullopt;
    }
    data.resize(at);
    return data;
}

std::size_t available_parallelism() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors == 0 ? 4 : info.dwNumberOfProcessors;
}

namespace {

template <class T>
std::optional<T> parse_unsigned(std::string_view s, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<T> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    if (s.empty()) {
        return fail("cannot parse integer from empty string");
    }
    if (s.front() == '+') {
        s.remove_prefix(1);
        if (s.empty()) {
            return fail("invalid digit found in string");
        }
    }
    T v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return fail("invalid digit found in string");
        }
        T d = static_cast<T>(c - '0');
        if (v > (std::numeric_limits<T>::max() - d) / 10) {
            return fail("number too large to fit in target type");
        }
        v = static_cast<T>(v * 10 + d);
    }
    return v;
}

}   

std::optional<std::uint32_t> parse_u32(std::string_view s) { return parse_unsigned<std::uint32_t>(s, nullptr); }
std::optional<std::size_t> parse_usize(std::string_view s, std::string* error) { return parse_unsigned<std::size_t>(s, error); }

void binary_output() {


    std::fflush(stdout);
    std::fflush(stderr);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

void say(const std::string& line) {
    std::string l = line + "\n";
    std::fwrite(l.data(), 1, l.size(), stdout);
}

std::array<std::uint8_t, 32> blake3_of(std::span<const std::uint8_t> data) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data.data(), data.size());
    std::array<std::uint8_t, 32> out;
    blake3_hasher_finalize(&h, out.data(), out.size());
    return out;
}

std::string hex_prefix(const std::array<std::uint8_t, 32>& hash, std::size_t n) {
    static const char* DIGITS = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t b = hash[i / 2];
        s += DIGITS[i % 2 == 0 ? b >> 4 : b & 15];
    }
    return s;
}


namespace {


struct DxilProgram {
    std::uint16_t kind;
    std::uint8_t major, minor;
    std::span<const std::uint8_t> bitcode, prefix, trailer;

    static std::optional<DxilProgram> parse(std::span<const std::uint8_t> part) {
        auto u32_at = [&](std::size_t o) -> std::optional<std::uint32_t> {
            if (o + 4 > part.size()) {
                return std::nullopt;
            }
            return std::uint32_t{part[o]} | std::uint32_t{part[o + 1]} << 8 | std::uint32_t{part[o + 2]} << 16 | std::uint32_t{part[o + 3]} << 24;
        };
        auto version = u32_at(0);
        auto dwords = u32_at(4);
        if (!version || !dwords) {
            return std::nullopt;
        }
        std::size_t size = std::size_t{*dwords} * 4;
        if (size > part.size() || size < 24) {
            return std::nullopt;
        }
        part = part.first(size);
        if (std::memcmp(part.data() + 8, "DXIL", 4) != 0) {
            return std::nullopt;
        }
        auto offset = u32_at(16);
        auto len = u32_at(20);
        if (!u32_at(12) || !offset || !len) {
            return std::nullopt;
        }
        std::size_t start = 8 + std::size_t{*offset};
        if (start > part.size() || part.size() - start < *len) {
            return std::nullopt;
        }
        auto bitcode = part.subspan(start, *len);
        static constexpr std::uint8_t MAGIC[4] = {0x42, 0x43, 0xC0, 0xDE};
        if (bitcode.size() < 4 || std::memcmp(bitcode.data(), MAGIC, 4) != 0) {
            return std::nullopt;
        }
        if (start < 24) {
            return std::nullopt;
        }
        DxilProgram p;
        p.kind = static_cast<std::uint16_t>(*version >> 16);
        p.major = static_cast<std::uint8_t>(*version >> 4 & 0xF);
        p.minor = static_cast<std::uint8_t>(*version & 0xF);
        p.bitcode = bitcode;
        p.prefix = part.subspan(24, start - 24);
        p.trailer = part.subspan(start + *len);
        return p;
    }

    bool is_standard_layout() const { return prefix.empty() && trailer.empty(); }
    bool xdat() const { return prefix.size() >= 4 && std::memcmp(prefix.data(), "XDAT", 4) == 0; }
    bool xbop() const {
        std::string_view t(reinterpret_cast<const char*>(trailer.data()), trailer.size());
        return t.find("xbop") != std::string_view::npos;
    }

    std::string kind_name() const {
        static const char* const NAMES[] = {"Pixel",        "Vertex",   "Geometry",   "Hull", "Domain", "Compute", "Library", "RayGeneration",
                                            "Intersection", "AnyHit",   "ClosestHit", "Miss", "Callable", "Mesh",  "Amplification", "Node"};
        return kind < 16 ? NAMES[kind] : std::format("Unknown({})", kind);
    }
};

const char* stage_name(HwStage s) {
    static const char* const N[] = {"Ls", "Hs", "Es", "Gs", "Vs", "Ps", "Ms", "Cs"};
    return N[static_cast<int>(s)];
}

const char* encoding_name(Encoding e) {
    static const char* const N[] = {"Sop2", "Sopk", "Sop1", "Sopc", "Sopp", "Smem", "Vop2",  "Vop1", "Vopc", "Vop3",
                                    "Vop3p", "Vintrp", "Ds", "Mubuf", "Mtbuf", "Mimg", "Exp", "Flat", "Global", "Scratch"};
    return N[static_cast<int>(e)];
}

struct Totals {
    std::size_t containers = 0, with_dxil_part = 0, with_bitcode = 0, standard_layout = 0, xdat = 0, xbop = 0;
    std::map<std::string, std::size_t> kinds;
    std::size_t with_native = 0;
    std::map<HwStage, std::size_t> native_stages;
    std::map<std::string, std::size_t> native_errors;
    std::size_t decoded = 0, instructions = 0;
    std::map<std::string, std::size_t> unnamed;
    std::map<std::string, std::size_t> decode_errors;
    std::size_t files_with_shaders = 0;
};

std::string format_instruction(const Instruction& i, bool wave64) {
    std::string words;
    for (std::size_t k = 0; k < i.words.size(); ++k) {
        words += std::format("{}{:08X}", k ? " " : "", i.words[k]);
    }
    std::string text = i.name().empty() ? std::format("<{} {:#x}>", encoding_name(i.encoding), i.opcode) : instruction_text(i, wave64);
    return std::format("    {:06x}: {:<44} {}", i.offset, words, text);
}

const char* yes(bool b) { return b ? "true" : "false"; }

}   

void shader_info(std::vector<fs::path> paths_in, DefaultPaths default_paths, bool verbose, bool disasm) {
    binary_output();
    auto paths = default_paths(std::move(paths_in));
    Totals t;
    for (const auto& root : paths) {
        for (const auto& entry : walk_files(root)) {
            const fs::path& path = entry.path;
            if (entry.len > MAX_FILE) {
                std::fprintf(stderr, "skipping large file %s\n", config::display(path).c_str());
                continue;
            }
            auto data = read_whole(path);
            if (!data) {
                continue;
            }
            std::string shown = config::display(path);
            std::size_t found_here = 0;
            for (const auto& source : sources(*data)) {
                std::vector<std::uint8_t> storage;
                auto bytes = source.bytes(storage);
                if (!bytes) {
                    std::fflush(stdout);
                    std::fprintf(stderr, "corrupt record %s%s\n", shown.c_str(), source.label().c_str());
                    continue;
                }
                Container container;
                try {
                    container = Container::parse(*bytes);
                } catch (const std::exception&) {
                    continue;
                }
                ++found_here;
                ++t.containers;
                const ContainerPart* dxil = container.part(fourcc("DXIL"));
                t.with_dxil_part += dxil ? 1 : 0;
                std::optional<DxilProgram> program = dxil ? DxilProgram::parse(dxil->data) : std::nullopt;
                std::vector<std::string> listing;

                std::string native;
                if (dxil) {
                    std::optional<std::vector<NativeProgram>> programs;
                    std::optional<std::string> error;
                    try {
                        programs = native_programs_checked(dxil->data);
                    } catch (const ShaderError& e) {
                        error = e.display();
                    }
                    if (programs && !programs->empty()) {
                        ++t.with_native;
                        std::string list;
                        auto add = [&](const std::string& s) { list += (list.empty() ? "" : " ") + s; };
                        for (const auto& n : *programs) {
                            ++t.native_stages[n.stage];
                            add(std::format("{} {} B", stage_name(n.stage), n.code.size()));
                            try {
                                auto insts = decode_checked(n.code);
                                ++t.decoded;
                                t.instructions += insts.size();
                                for (const auto& i : insts) {
                                    if (i.name().empty()) {
                                        ++t.unnamed[std::format("{} {:#x}", encoding_name(i.encoding), i.opcode)];
                                    }
                                }
                                if (disasm) {
                                    for (const auto& p : n.layout) {
                                        listing.push_back("    " + debug_text(p));
                                    }
                                    for (const auto& u : n.used) {
                                        listing.push_back("    " + debug_text(u));
                                    }
                                    for (const auto& i : insts) {
                                        listing.push_back(format_instruction(i, n.wave64));
                                    }
                                }
                            } catch (const ShaderError& e) {
                                std::string reason = e.kind == ShaderError::Isa ? e.text : "other";
                                ++t.decode_errors[reason];
                                add("(" + e.display() + ")");
                            }
                        }
                        native = ", native " + list;
                    } else if (error) {
                        ++t.native_errors[*error];
                        native = ", native: " + *error;
                    }
                }
                std::string line;
                if (program) {
                    ++t.with_bitcode;
                    t.standard_layout += program->is_standard_layout() ? 1 : 0;
                    bool xdat = program->xdat(), xbop = program->xbop();
                    t.xdat += xdat ? 1 : 0;
                    t.xbop += xbop ? 1 : 0;
                    ++t.kinds[program->kind_name()];
                    line = std::format("{} sm{}.{} bitcode {} B, xdat {}, xbop {}", program->kind_name(), program->major, program->minor,
                                       program->bitcode.size(), yes(xdat), yes(xbop));
                } else {
                    line = "no parsable DXIL program";
                }
                line += native;
                if (verbose) {
                    say(shown + source.label() + ": " + line);
                    for (const auto& l : listing) {
                        say(l);
                    }
                }
            }
            t.files_with_shaders += found_here > 0 ? 1 : 0;
        }
    }
    say(std::format("containers found       {} (in {} files)", t.containers, t.files_with_shaders));
    say(std::format("  with DXIL part       {}", t.with_dxil_part));
    say(std::format("  with LLVM bitcode    {}", t.with_bitcode));
    say(std::format("  PC-standard layout   {}", t.standard_layout));
    say(std::format("  XDAT prefix          {}", t.xdat));
    say(std::format("  xbop trailer         {}", t.xbop));
    for (const auto& [k, n] : t.kinds) {
        say(std::format("  {:<20} {}", k, n));
    }
    say(std::format("  with native code     {}", t.with_native));
    for (const auto& [stage, n] : t.native_stages) {
        say(std::format("    {:<18} {}", stage_name(stage), n));
    }
    for (const auto& [e, n] : t.native_errors) {
        say(std::format("  native unreadable    {}: {}", n, e));
    }
    if (t.with_native > 0) {
        say(std::format("  decoded programs     {} ({} instructions)", t.decoded, t.instructions));
        for (const auto& [e, n] : t.decode_errors) {
            say(std::format("  decode failed        {}: {}", n, e));
        }
        std::vector<std::pair<std::string, std::size_t>> unnamed(t.unnamed.begin(), t.unnamed.end());
        std::stable_sort(unnamed.begin(), unnamed.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < unnamed.size() && i < 10; ++i) {
            say(std::format("  opcode not in table  {} x{}", unnamed[i].first, unnamed[i].second));
        }
    }
    if (t.containers == 0) {
        say("(none found: shaders may sit in compressed or encrypted archives)");
    }
}

}   
