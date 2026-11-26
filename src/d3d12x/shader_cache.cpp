// SPDX-License-Identifier: GPL-3.0-or-later
// Translated shaders kept on disk (`shader_cache.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See shader_cache.hpp.
#include "shader_cache.hpp"

#include <blake3.h>
#include <windows.h>

#include <bit>
#include <cctype>
#include <string>

#include "core/env.hpp"
#include "core/log.hpp"
#include "host_heap.hpp"
#include "shader/dxil.hpp"
#include "shader/emit.hpp"
#include "shader/error.hpp"
#include "shader/lower.hpp"
#include "shader/stand_in.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace danzi::d3d12x {

namespace {

std::mutex CACHE_LOCK;
std::optional<std::filesystem::path> CACHE_DIR;
std::optional<std::array<std::uint8_t, 32>> BUILD;
std::atomic<std::uint64_t> COUNTS[2]{};

constexpr std::uint8_t MAGIC[4] = {'V', 'S', 'H', '1'};

struct Reader {
    std::span<const std::uint8_t> rest;
    bool take(std::size_t n, std::span<const std::uint8_t>& out) {
        if (rest.size() < n) {
            return false;
        }
        out = rest.first(n);
        rest = rest.subspan(n);
        return true;
    }
    bool word(std::uint32_t& out) {
        std::span<const std::uint8_t> b;
        if (!take(4, b)) {
            return false;
        }
        std::memcpy(&out, b.data(), 4);
        return true;
    }
    bool list(std::vector<std::uint8_t>& out) {
        std::uint32_t n;
        std::span<const std::uint8_t> b;
        if (!word(n) || !take(n, b)) {
            return false;
        }
        out.assign(b.begin(), b.end());
        return true;
    }
};

std::pair<RootKind, std::uint32_t> root_kind(std::uint32_t code) {
    switch (code) {
        case 0: return {RootKind::Constants, 0};
        case 1: return {RootKind::Descriptor, 0};
        case 2: return {RootKind::Table, 0};
        case 3: return {RootKind::ImmediateSampler, 0};
        case 4: return {RootKind::ResourceHeap, 0};
        case 5: return {RootKind::SamplerHeap, 0};
        default: return {RootKind::Other, code - 6};
    }
}

std::optional<HostShader> decode(std::span<const std::uint8_t> bytes) {
    Reader r{bytes};
    std::span<const std::uint8_t> magic;
    if (!r.take(4, magic) || std::memcmp(magic.data(), MAGIC, 4) != 0) {
        return std::nullopt;
    }
    HostShader s;
    std::uint32_t n;
    if (!r.word(s.stage) || s.stage >= 8 || !r.list(s.dxil) || !r.word(n)) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t index, slot, root, dword;
        if (!r.word(index) || !r.word(slot) || !r.word(root) || !r.word(dword) || slot > 255) {
            return std::nullopt;
        }
        auto [kind, other] = root_kind(root);
        s.user_data.push_back({index, static_cast<std::uint8_t>(slot), kind, other, dword});
    }
    if (!r.word(n)) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::array<std::uint32_t, 4> w;
        for (auto& x : w) {
            if (!r.word(x)) {
                return std::nullopt;
            }
        }
        s.samplers.push_back(w);
    }
    if (!r.word(n)) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t w[10];
        for (auto& x : w) {
            if (!r.word(x)) {
                return std::nullopt;
            }
        }
        StaticSampler t;
        t.filter = w[0];
        t.address = {w[1], w[2], w[3]};
        std::memcpy(&t.mip_lod_bias, &w[4], 4);
        t.max_anisotropy = w[5];
        t.comparison = w[6];
        t.border = w[7];
        std::memcpy(&t.min_lod, &w[8], 4);
        std::memcpy(&t.max_lod, &w[9], 4);
        s.static_samplers.push_back(t);
    }
    if (!r.list(s.root_cbvs) || !r.list(s.root_srvs) || !r.list(s.root_uavs) || !r.word(n)) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t table, constant, dword, offset, shift;
        if (!r.word(table) || !r.word(constant) || !r.word(dword) || !r.word(offset) || !r.word(shift) || table > 255 || constant > 255 ||
            shift >= 32) {
            return std::nullopt;
        }
        s.root_indexed_views.push_back({static_cast<std::uint8_t>(table), static_cast<std::uint8_t>(constant), dword, offset, static_cast<std::uint8_t>(shift)});
    }
    if (!r.rest.empty()) {
        return std::nullopt;
    }
    return s;
}

}   

std::uint32_t HostShader::user_data_dwords() const {
    std::uint32_t n = 0;
    for (const UserData& u : user_data) {
        n = std::max(n, u.index + 1);
    }

    return (n + 3) / 4 * 4;
}

void use_shader_cache(const wchar_t* dir) {
    if (!dir) {
        return;
    }

    std::filesystem::path path(dir);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        log::warn("victory_gpu::shader_cache", "no shader cache", {{"error", "creating " + std::string(reinterpret_cast<const char*>(path.u8string().c_str()))}});
        return;
    }
    std::lock_guard lock(CACHE_LOCK);
    if (!CACHE_DIR) {
        CACHE_DIR = std::move(path);
    }
}

void use_shader_build(const std::uint8_t* build) {
    if (!build) {
        return;
    }
    std::lock_guard lock(CACHE_LOCK);
    std::array<std::uint8_t, 32> b;
    std::copy(build, build + 32, b.begin());
    BUILD = b;
}

namespace {


std::uint32_t root_code(std::uint32_t raw) {
    switch (raw) {
        case 0: return 0;
        case 1: return 1;
        case 3: return 2;
        case 4: return 3;
        case 5: return 4;
        case 6: return 5;
        default: return 6 + raw;
    }
}


std::vector<std::uint8_t> encode(const shader::emit::HostShader& t) {
    std::vector<std::uint8_t> out(MAGIC, MAGIC + 4);
    auto word = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    };
    auto bytes = [&](const std::vector<std::uint8_t>& b) {
        word(static_cast<std::uint32_t>(b.size()));
        out.insert(out.end(), b.begin(), b.end());
    };
    word(static_cast<std::uint32_t>(t.stage));
    bytes(t.dxil);
    word(static_cast<std::uint32_t>(t.user_data.size()));
    for (const auto& u : t.user_data) {
        word(u.index);
        word(u.slot);
        word(root_code(u.root));
        word(u.dword);
    }
    word(static_cast<std::uint32_t>(t.samplers.size()));
    for (const auto& w : t.samplers) {
        for (std::uint32_t v : w) {
            word(v);
        }
    }
    word(static_cast<std::uint32_t>(t.static_samplers.size()));
    for (const auto& x : t.static_samplers) {
        for (std::uint32_t v : {x.filter, x.address[0], x.address[1], x.address[2], std::bit_cast<std::uint32_t>(x.mip_lod_bias), x.max_anisotropy,
                                x.comparison, x.border, std::bit_cast<std::uint32_t>(x.min_lod), std::bit_cast<std::uint32_t>(x.max_lod)}) {
            word(v);
        }
    }
    bytes(t.root_cbvs);
    bytes(t.root_srvs);
    bytes(t.root_uavs);
    word(static_cast<std::uint32_t>(t.root_indexed_views.size()));
    for (const auto& v : t.root_indexed_views) {
        word(v.table_slot);
        word(v.constant_slot);
        word(v.constant_dword);
        word(v.byte_offset);
        word(v.shift);
    }
    return out;
}

std::optional<std::string> env_text(const wchar_t* name) {
    auto v = danzi::env::get(name);
    if (!v) {
        return std::nullopt;
    }
    std::string out;
    for (wchar_t c : *v) {
        out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front()))) {
        out.erase(out.begin());
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    return out;
}

std::string container_hash16(std::span<const std::uint8_t> container) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, container.data(), container.size());
    std::uint8_t d[8];
    blake3_hasher_finalize(&h, d, sizeof d);
    static constexpr char HEX[] = "0123456789abcdef";
    std::string hex;
    for (std::uint8_t b : d) {
        hex.push_back(HEX[b >> 4]);
        hex.push_back(HEX[b & 15]);
    }
    return hex;
}

bool listed(const std::optional<std::string>& override_list, const std::string& hash, std::initializer_list<const char*> defaults) {
    auto lower = [](std::string x) {
        for (char& c : x) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return x;
    };
    if (override_list) {
        std::string list = *override_list;
        std::size_t at = 0;
        while (at <= list.size()) {
            std::size_t end = at;
            while (end < list.size() && list[end] != ',' && !std::isspace(static_cast<unsigned char>(list[end]))) {
                ++end;
            }
            if (lower(list.substr(at, end - at)) == hash) {
                return true;
            }
            at = end + 1;
        }
        return false;
    }
    for (const char* d : defaults) {
        if (hash == d) {
            return true;
        }
    }
    return false;
}

float value_or(const std::optional<std::string>& v, float fallback) {
    if (!v) {
        return fallback;
    }
    try {
        std::size_t used = 0;
        float f = std::stof(*v, &used);
        return used == v->size() ? f : fallback;
    } catch (...) {
        return fallback;
    }
}


std::optional<shader::emit::HostShader> stand_in(std::span<const std::uint8_t> container) {
    namespace si = shader::stand_in;
    std::string hash = container_hash16(container);
    struct Kind {
        const wchar_t* name;
        const wchar_t* hashes_var;
        const wchar_t* value_var;
        const wchar_t* slots_var;
        std::initializer_list<const char*> hashes;
        float value;
        const char* defaults;
    };
    const char* ambient_defaults = (hash == "3b4f6eb5edb79848" || hash == "cf76630573822d04")
                                       ? "144=mask,160=rgb9e5,168=rgb9e5,176=rgb9e5,184=rgb9e5,192=rgb9e5,208=rgb9e5,200=zero,216=mask"
                                       : "136=mask,152=rgb9e5,160=rgb9e5,168=rgb9e5,176=rgb9e5,184=rgb9e5,200=rgb9e5,192=zero,208=mask";
    const Kind kinds[] = {
        {L"VICTORY_AMBIENT_STUB", L"VICTORY_AMBIENT_STUB_HASHES", L"VICTORY_AMBIENT_STUB_VALUE", L"VICTORY_AMBIENT_STUB_SLOTS",
         {"2bed88a03a2f5558", "34fb51b8a0312e82", "5053352ac4370ca1", "2fc5d11ece80c884", "3b4f6eb5edb79848", "cf76630573822d04"}, 0.1f, ambient_defaults},
        {L"VICTORY_SHADOWMASK_STUB", L"VICTORY_SHADOWMASK_STUB_HASHES", L"VICTORY_SHADOWMASK_STUB_VALUE", L"VICTORY_SHADOWMASK_STUB_SLOTS",
         {"a0544eac81df1fc8", "a1b20bc47e3c9597", "ff6a098566877642", "e8c962358d862e3f", "d4c32d1ea3b676be", "17829ac71aab4e8d", "ba263b6ab4b6d619",
          "afeea48bbb9913ad"},
         1.0f, ""},
    };
    for (const Kind& k : kinds) {
        if (env_text(k.name) != std::optional<std::string>("1")) {
            continue;
        }
        if (!listed(env_text(k.hashes_var), hash, k.hashes)) {
            continue;
        }
        float value = value_or(env_text(k.value_var), k.value);
        std::string slots = env_text(k.slots_var).value_or("");
        si::store_plan() = si::Plan(value).with_overrides(k.defaults).with_overrides(slots);
        std::string name;
        for (const wchar_t* c = k.name; *c; ++c) {
            name += static_cast<char>(*c);
        }
        try {
            auto shader = shader::emit::translate(container);
            std::string outputs = "[";
            for (const auto& seen : si::store_plan()->seen) {
                outputs += (outputs.size() > 1 ? ", " : "") + (seen ? "Some(" + std::to_string(*seen) + ")" : std::string("None"));
            }
            si::store_plan().reset();
            log::warn("victory_gpu::shader_cache", "pass replaced by a stand-in writing each output in its own encoding",
                      {{"hash", hash}, {"stand_in", log::debug(name)}, {"value", log::debug(value)}, {"outputs", outputs + "]"}});
            return shader;
        } catch (const shader::ShaderError& e) {
            si::store_plan().reset();
            log::warn("victory_gpu::shader_cache", "stand-in does not translate", {{"hash", hash}, {"stand_in", log::debug(name)}, {"error", e.display()}});
        }
    }
    return std::nullopt;
}


std::optional<shader::emit::HostShader> exposure_stub(std::span<const std::uint8_t> container) {
    if (env_text(L"VICTORY_EXPOSURE_STUB") != std::optional<std::string>("1")) {
        return std::nullopt;
    }
    std::string hash = container_hash16(container);
    if (!listed(env_text(L"VICTORY_EXPOSURE_STUB_HASHES"), hash, {"e622b718b6d9b057"})) {
        return std::nullopt;
    }
    float value = value_or(env_text(L"VICTORY_EXPOSURE_STUB_VALUE"), 0.18f);
    try {
        auto shader = shader::emit::translate_code(container, shader::stand_in::exposure_code(value));
        log::warn("victory_gpu::shader_cache", "exposure shader replaced by a constant stand-in", {{"hash", hash}, {"value", log::debug(value)}});
        return shader;
    } catch (const shader::ShaderError& e) {
        log::warn("victory_gpu::shader_cache", "exposure stand-in does not translate either", {{"hash", hash}, {"error", e.display()}});
        return std::nullopt;
    }
}


constexpr char PORT_TRANSLATOR[] = "danzi-cpp shader.dxil m5.10 1";

std::array<std::uint8_t, 32> own_build() {
    probe_view_limit();
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, PORT_TRANSLATOR, sizeof PORT_TRANSLATOR - 1);
    for (std::uint32_t n : {shader::lower::loop_limit(), shader::dxil::view_heap_limit(), shader::lower::host_max_wave(),
                            std::uint32_t{shader::lower::group_sync()}, shader::dxil::shader_opt()}) {
        blake3_hasher_update(&h, &n, 4);
    }
    static constexpr std::pair<const wchar_t*, std::string_view> SWITCHES[] = {{L"VICTORY_WAVE_WIDE", "VICTORY_WAVE_WIDE"},
                                                                             {L"VICTORY_WAVE_DEEP_SKIPS", "VICTORY_WAVE_DEEP_SKIPS"},
                                                                             {L"VICTORY_WAVE_OWN_BITS", "VICTORY_WAVE_OWN_BITS"},
                                                                             {L"VICTORY_F16_NEAREST", "VICTORY_F16_NEAREST"}};
    for (const auto& [name, n] : SWITCHES) {
        blake3_hasher_update(&h, n.data(), n.size());
        blake3_hasher_update(&h, "=", 1);
        if (auto v = env_text(name)) {
            blake3_hasher_update(&h, v->data(), v->size());
        }
        std::uint8_t zero = 0;
        blake3_hasher_update(&h, &zero, 1);
    }
    std::array<std::uint8_t, 32> out;
    blake3_hasher_finalize(&h, out.data(), out.size());
    return out;
}

}   

Translation host_shader(std::span<const std::uint8_t> container) {
    if (auto s = stand_in(container)) {
        return {decode(encode(*s)), {}};
    }
    std::optional<std::filesystem::path> dir;
    std::optional<std::array<std::uint8_t, 32>> build;
    {
        std::lock_guard lock(CACHE_LOCK);
        dir = CACHE_DIR;
        build = BUILD;
    }
    auto fallback = [&](std::string error) -> Translation {
        if (auto s = exposure_stub(container)) {
            return {decode(encode(*s)), {}};
        }
        return {std::nullopt, std::move(error)};
    };


    if (dir && !build) {
        static const std::array<std::uint8_t, 32> OWN = own_build();
        build = OWN;
    }
    if (!dir) {
        try {
            auto s = shader::emit::translate(container);
            return {decode(encode(s)), {}};
        } catch (const shader::ShaderError& e) {
            return fallback(e.display());
        }
    }


    blake3_hasher h;
    blake3_hasher_init(&h);
    static constexpr char ID[] = "shader.dxil";
    blake3_hasher_update(&h, ID, sizeof ID - 1);
    std::uint8_t zero = 0;
    blake3_hasher_update(&h, &zero, 1);
    std::uint32_t version = 1;
    blake3_hasher_update(&h, &version, 4);
    blake3_hasher_update(&h, build->data(), 32);
    blake3_hasher_update(&h, container.data(), container.size());
    std::uint8_t key[32];
    blake3_hasher_finalize(&h, key, sizeof key);
    static constexpr char HEX[] = "0123456789abcdef";
    std::string hex;
    for (std::uint8_t b : key) {
        hex.push_back(HEX[b >> 4]);
        hex.push_back(HEX[b & 15]);
    }
    std::filesystem::path path = *dir / hex.substr(0, 2) / hex.substr(2);


    bool ran = false;
    for (int round = 0; round < 2; ++round) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            ran = true;
            std::vector<std::uint8_t> output;
            try {
                output = encode(shader::emit::translate(container));
            } catch (const shader::ShaderError& e) {
                return fallback("converter shader.dxil v1: " + e.display());
            }
            std::filesystem::create_directories(path.parent_path(), ec);
            static std::atomic<std::uint64_t> counter{0};
            std::filesystem::path tmp =
                path.parent_path() / (".tmp-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter.fetch_add(1)));
            bool written = false;
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (out) {
                    out.write(reinterpret_cast<const char*>(output.data()), static_cast<std::streamsize>(output.size()));
                    written = static_cast<bool>(out);
                }
            }
            if (!written) {
                std::filesystem::remove(tmp, ec);
                return fallback("converter shader.dxil v1: the cache entry could not be written");
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
                std::error_code ec2;
                std::filesystem::remove(tmp, ec2);
                return fallback("converter shader.dxil v1: the cache entry could not be published");
            }
        }

        std::ifstream in(path, std::ios::binary | std::ios::ate);
        std::vector<std::uint8_t> bytes;
        if (in) {
            bytes.resize(static_cast<std::size_t>(in.tellg()));
            in.seekg(0);
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<std::size_t>(in.gcount()));
        }
        if (auto shader = decode(bytes)) {
            COUNTS[ran ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
            return {std::move(shader), {}, !ran};
        }
        in.close();
        std::filesystem::remove(path, ec);
    }
    COUNTS[1].fetch_add(1, std::memory_order_relaxed);
    try {
        auto s = shader::emit::translate(container);
        return {decode(encode(s)), {}};
    } catch (const shader::ShaderError& e) {
        return fallback(e.display());
    }
}

std::array<std::uint64_t, 2> shader_cache_counts() { return {COUNTS[0].load(), COUNTS[1].load()}; }

}   
