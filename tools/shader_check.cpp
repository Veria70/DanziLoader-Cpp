// SPDX-License-Identifier: GPL-3.0-or-later
// Milestone 5: the port's translator, checked against the reference's
// (`oracle-m5`, `shader-oracle`): the same canonical dumps of each stage and
// the same translations over a corpus of shader containers.
//
//   shader-check dump <stage> <corpus dir> <out.txt> [wave] [view heap limit]
//   shader-check translate|time <corpus dir> <out.txt> [wave] [view heap limit]
//
// Stages: native.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sampler.hpp"
#include "shader/dxbc.hpp"
#include "shader/dxil.hpp"
#include "shader/emit.hpp"
#include "shader/error.hpp"
#include "shader/io.hpp"
#include "shader/ir.hpp"
#include "shader/native.hpp"
#include "shader/resource.hpp"
#include "shader/spill.hpp"
#include "shader_dump.hpp"

namespace fs = std::filesystem;
using namespace danzi::shader;

namespace {

std::vector<fs::path> corpus_files(const fs::path& dir) {
    std::vector<fs::path> out;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".dxbc") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::uint8_t> read(const fs::path& p) {

    std::ifstream f(p, std::ios::binary | std::ios::ate);
    std::vector<std::uint8_t> out;
    if (!f) {
        return out;
    }
    auto size = static_cast<std::size_t>(f.tellg());
    out.resize(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return out;
}

const char* stage_name(HwStage s) {
    static const char* const n[] = {"Ls", "Hs", "Es", "Gs", "Vs", "Ps", "Ms", "Cs"};
    return n[static_cast<int>(s)];
}

std::string root_name(std::uint32_t r) {
    switch (r) {
        case 0: return "Constants";
        case 1: return "Descriptor";
        case 3: return "Table";
        case 4: return "ImmediateSampler";
        case 5: return "ResourceHeap";
        case 6: return "SamplerHeap";
        default: return "Other(" + std::to_string(r) + ")";
    }
}

std::string resource_name(std::uint8_t k) {
    switch (k) {
        case 0: return "Srv";
        case 1: return "Sampler";
        case 2: return "Cbv";
        case 3: return "Uav";
        default: return "Other(" + std::to_string(k) + ")";
    }
}

std::string hash16(std::span<const std::uint8_t> bytes);

void dump_native(std::span<const std::uint8_t> data, std::ostream& out) {
    Container c;
    try {
        c = Container::parse(data);
    } catch (const ShaderError& e) {
        out << " container err " << e.display() << "\n";
        return;
    }
    for (const auto& p : c.parts) {
        out << " part " << fourcc_text(p.fourcc) << " " << p.data.size() << "\n";
    }
    const ContainerPart* dxil = c.part(fourcc("DXIL"));
    if (!dxil) {
        return;
    }
    std::vector<NativeProgram> programs;
    try {
        programs = native_programs_checked(dxil->data);
    } catch (const ShaderError& e) {
        out << " native err " << e.display() << "\n";
        return;
    }
    for (const auto& p : programs) {
        out << " program " << stage_name(p.stage) << " wave64 " << (p.wave64 ? "true" : "false") << " vgprs " << p.vgprs << " code "
            << p.code.size() << " " << hash16(p.code) << "\n";
        for (const auto& l : p.layout) {
            out << "  layout " << root_name(l.root) << " " << l.table_offset_dwords << " " << l.sgprs << " " << l.base_register << " "
                << int{l.user_data} << " " << int{l.slot} << "\n";
        }
        for (const auto& u : p.used) {
            out << "  used " << resource_name(u.kind) << " " << root_name(u.root) << " " << u.table_offset_dwords << " " << u.lower_bound
                << " " << u.stride << " " << int{u.dwords} << " " << int{u.user_data} << " " << int{u.slot} << "\n";
        }
    }
}


std::optional<NativeProgram> picked(std::span<const std::uint8_t> data, std::string& out) {
    try {
        Container c = Container::parse(data);
        const ContainerPart* dxil = c.part(fourcc("DXIL"));
        if (!dxil) {
            out += " err no DXIL part\n";
            return std::nullopt;
        }
        for (auto& p : native_programs_checked(dxil->data)) {
            if (p.stage == HwStage::Vs || p.stage == HwStage::Ps || p.stage == HwStage::Cs || p.stage == HwStage::Ms) {
                return std::move(p);
            }
        }
        out += " err no program\n";
    } catch (const ShaderError& e) {
        out += " err " + e.display() + "\n";
    }
    return std::nullopt;
}

void dump_ir(std::span<const std::uint8_t> data, std::string& out) {
    auto p = picked(data, out);
    if (!p) {
        return;
    }
    try {
        auto code = p->patched_code();
        auto program = ir::build(code, p->wave64, p->vgprs);
        dump::program(program, out);
    } catch (const ShaderError& e) {
        out += " err " + e.display() + "\n";
    }
}

void dump_spill(std::span<const std::uint8_t> data, std::string& out) {
    auto p = picked(data, out);
    if (!p) {
        return;
    }
    try {
        dump::program(spill_build(*p), out);
    } catch (const ShaderError& e) {
        out += " err " + e.display() + "\n";
    }
}

void dump_structure(std::span<const std::uint8_t> data, std::string& out) {
    auto p = picked(data, out);
    if (!p) {
        return;
    }
    ir::Program program;
    try {
        program = spill_build(*p);
    } catch (const ShaderError& e) {
        out += " err " + e.display() + "\n";
        return;
    }
    try {
        dump::structured(structure::structure(program), out);
    } catch (const ShaderError& e) {
        out += " st err " + e.display() + "\n";
    }
}

std::string hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}

std::string site(const resource::Site& s) { return std::to_string(s.first) + "." + std::to_string(s.second); }

std::string address(const resource::Address& a) {
    std::string param = a.param == resource::CODE ? "code" : a.param == resource::MEMORY ? "memory" : std::to_string(a.param);
    std::string dynamic = a.dynamic ? "v" + std::to_string(a.dynamic->id) : "-";
    return param + "+" + hex(a.offset) + "+" + dynamic;
}

void source(const resource::Source& s, std::string& out) {
    using resource::Source;
    auto list = [&](const std::vector<Source>& args) {
        out += '[';
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                out += ',';
            }
            source(args[i], out);
        }
        out += ']';
    };
    switch (s.kind) {
        case Source::Loaded: out += "L" + site(s.load) + "+" + std::to_string(s.first); break;
        case Source::Fetched: out += "F" + site(s.load) + "+" + std::to_string(s.first); break;
        case Source::Built: out += "B(" + address(s.address) + " " + hex(s.bits) + " " + hex(s.words[0]) + " " + hex(s.words[1]) + ")"; break;
        case Source::Constant:
            out += "K[";
            for (std::size_t i = 0; i < s.consts.size(); ++i) {
                out += (i > 0 ? "," : "") + hex(s.consts[i]);
            }
            out += ']';
            break;
        case Source::Phi:
            out += "P" + std::to_string(s.block);
            list(s.args);
            break;
        case Source::Select:
            out += "S" + std::to_string(s.value.id);
            list(s.args);
            break;
        case Source::Lanes:
            out += "M" + std::to_string(s.value.id);
            list(s.args);
            break;
        case Source::LanePhi:
            out += "Q" + std::to_string(s.block) + "[";
            for (std::size_t i = 0; i < s.args.size(); ++i) {
                if (i > 0) {
                    out += ',';
                }
                if (s.present[i]) {
                    source(s.args[i], out);
                } else {
                    out += '-';
                }
            }
            out += ']';
            break;
    }
}

void dump_resource(std::span<const std::uint8_t> data, std::string& out) {
    auto p = picked(data, out);
    if (!p) {
        return;
    }
    ir::Program program;
    try {
        program = spill_build(*p);
    } catch (const ShaderError& e) {
        out += " err " + e.display() + "\n";
        return;
    }
    std::optional<structure::Structured> st;
    try {
        st = structure::structure(program);
    } catch (const ShaderError& e) {
        out += " st err " + e.display() + "\n";
    }
    resource::Resources r;
    try {
        r = resource::track(program, p->layout, p->stage, st ? &*st : nullptr);
    } catch (const ShaderError& e) {
        out += " err " + e.display() + "\n";
        return;
    }
    for (const auto& [s, a] : r.scalar) {
        out += " scalar " + site(s) + " " + address(a) + "\n";
    }
    for (const auto& d : r.descriptors) {
        out += " desc " + site(d.site) + (d.role == resource::Role::Resource ? " r " : " s ");
        source(d.source, out);
        out += '\n';
    }
    for (const auto& v : r.root_indexed_views) {
        out += " view " + std::to_string(v.table_slot) + " " + std::to_string(v.constant_slot) + " " + std::to_string(v.constant_dword) + " " +
               hex(v.byte_offset) + " " + std::to_string(v.shift) + "\n";
    }
    for (const auto& [v, param, dword] : r.params) {
        out += " param v" + std::to_string(v.id) + " " + std::to_string(param) + " " + std::to_string(dword) + "\n";
    }
    for (const auto& s : r.lane_copied) {
        out += " copied " + site(s) + "\n";
    }
    for (const auto& s : r.waterfall.compares) {
        out += " wf compare " + site(s) + "\n";
    }
    for (const auto& v : r.waterfall.reads) {
        out += " wf read v" + std::to_string(v.id) + "\n";
    }
    for (const auto& s : r.waterfall.loads) {
        out += " wf load " + site(s) + "\n";
    }
    for (const auto& [s, b] : r.bvh) {
        if (b.kind == resource::BvhNodes::Resource) {
            out += " bvh " + site(s) + " resource\n";
        } else {
            out += " bvh " + site(s) + " memory " + hex(b.word1) + " " + hex(b.word3) + "\n";
        }
    }
}

std::string usage(Usage u) {
    if (u.raw == 6) {
        return "g";
    }
    if (u.raw == 7) {
        return "c";
    }
    return "o" + std::to_string(u.raw);
}

const char* bary_name(io::Barycentric b) {
    static const char* const n[] = {"PerspectiveSample", "PerspectiveCenter", "PerspectiveCentroid", "PerspectivePullModel",
                                    "LinearSample",      "LinearCenter",      "LinearCentroid"};
    return n[static_cast<int>(b)];
}

std::string input_text(const io::Input& i) {
    using io::Input;
    auto idx = [&](const char* n) { return std::string(n) + "(" + std::to_string(i.index) + ")"; };
    switch (i.kind) {
        case Input::VertexId: return "VertexId";
        case Input::PrimitiveId: return "PrimitiveId";
        case Input::InstanceId: return "InstanceId";
        case Input::Attribute: return "attr " + std::to_string(i.row) + " " + std::to_string(i.channel);
        case Input::Bary: return std::string("bary ") + bary_name(i.bary) + " " + std::to_string(i.index);
        case Input::Position: return idx("Position");
        case Input::FrontFace: return "FrontFace";
        case Input::Ancillary: return "Ancillary";
        case Input::SampleCoverage: return "SampleCoverage";
        case Input::FixedPosition: return "FixedPosition";
        case Input::LineStipple: return "LineStipple";
        case Input::GroupId: return idx("GroupId");
        case Input::ThreadIdInGroup: return idx("ThreadIdInGroup");
        case Input::GroupInfo: return "GroupInfo";
        case Input::MeshGroup: return "MeshGroup";
        case Input::MergedWaveInfo: return "MergedWaveInfo";
    }
    return "?";
}

void dump_io(std::span<const std::uint8_t> data, std::string& out) {
    auto p = picked(data, out);
    if (!p) {
        return;
    }
    io::Io r;
    try {
        ir::Program program = spill_build(*p);
        r = io::raise(program, *p);
    } catch (const ShaderError& e) {
        out += " err " + e.display() + "\n";
        return;
    }
    for (const auto& [v, i] : r.inputs) {
        out += " input v" + std::to_string(v.id) + " " + input_text(i) + "\n";
    }
    auto attr = [](const io::Attribute& a) {
        return usage(a.usage) + " " + std::to_string(a.row) + " " + std::to_string(a.channel) + (a.high ? " true" : " false");
    };
    for (const auto& [s, i] : r.interps) {
        out += " interp " + site(s) + " ";
        switch (i.kind) {
            case io::Interp::Interpolated: {
                out += "interp " + attr(i.attribute) + " " + bary_name(i.barycentric) + " [";
                for (std::size_t k = 0; k < i.first.size(); ++k) {
                    out += (k ? "," : "") + site(i.first[k]);
                }
                out += "]";
                break;
            }
            case io::Interp::Vertex: out += "vertex " + attr(i.attribute) + " " + std::to_string(i.vertex); break;
            case io::Interp::Weighted: out += "weighted " + attr(i.attribute); break;
        }
        out += "\n";
    }
    for (const auto& e : r.exports) {
        std::string target;
        switch (e.target.kind) {
            case io::Target::Color: target = "color" + std::to_string(e.target.index); break;
            case io::Target::Depth: target = "depth"; break;
            case io::Target::Null: target = "null"; break;
            case io::Target::Position: target = "pos" + std::to_string(e.target.index); break;
            case io::Target::Param: target = "param " + usage(e.target.usage) + " " + std::to_string(e.target.row); break;
            case io::Target::Unread: target = "unread"; break;
            case io::Target::Primitive: target = "prim"; break;
            case io::Target::PrimitiveParam: target = "primparam " + usage(e.target.usage) + " " + std::to_string(e.target.row); break;
        }
        std::string values;
        for (int k = 0; k < 4; ++k) {
            values += (k ? "," : "") + (e.values[k] ? "v" + std::to_string(e.values[k]->id) : std::string("-"));
        }
        out += " export " + site(e.site) + " " + target + " " + values + (e.compressed ? " true" : " false") + (e.done ? " true" : " false") + "\n";
    }
    if (r.initialized_sgprs) {
        out += " sgprs " + std::to_string(*r.initialized_sgprs) + "\n";
    }
}


std::string hash32(std::span<const std::uint8_t> bytes);

std::string hexbytes(const std::vector<std::uint8_t>& b) {
    std::string s;
    char t[3];
    for (std::uint8_t x : b) {
        std::snprintf(t, sizeof t, "%02x", x);
        s += t;
    }
    return s;
}


std::string result_line(const std::string& name, std::span<const std::uint8_t> data) {
    emit::HostShader h;
    try {
        h = emit::translate(data);
    } catch (const ShaderError& e) {
        return name + " err " + e.display();
    }
    std::string l = name + " ok " + stage_name(h.stage) + " dxil " + hash32(h.dxil) + " " + std::to_string(h.dxil.size());
    l += " ud";
    std::string buf;
    for (const auto& u : h.user_data) {
        l += " " + std::to_string(u.index) + ":" + std::to_string(u.slot) + ":" + lower::root_type_name(u.root, buf) + ":" + std::to_string(u.dword);
    }
    l += " s";
    char w[64];
    for (const auto& s : h.samplers) {
        std::snprintf(w, sizeof w, " %08x%08x%08x%08x", s[0], s[1], s[2], s[3]);
        l += w;
    }
    l += " ss";
    for (const auto& x : h.static_samplers) {
        std::snprintf(w, sizeof w, " %u:%u:%u:%u:%08x:%u:%u:%u:", x.filter, x.address[0], x.address[1], x.address[2],
                      std::bit_cast<std::uint32_t>(x.mip_lod_bias), x.max_anisotropy, x.comparison, x.border);
        l += w;
        std::snprintf(w, sizeof w, "%08x:%08x", std::bit_cast<std::uint32_t>(x.min_lod), std::bit_cast<std::uint32_t>(x.max_lod));
        l += w;
    }
    l += " cbv " + hexbytes(h.root_cbvs) + " srv " + hexbytes(h.root_srvs) + " uav " + hexbytes(h.root_uavs);
    l += " riv";
    for (const auto& v : h.root_indexed_views) {
        l += " " + std::to_string(v.table_slot) + ":" + std::to_string(v.constant_slot) + ":" + std::to_string(v.constant_dword) + ":" +
             std::to_string(v.byte_offset) + ":" + std::to_string(v.shift);
    }
    return l;
}

}   

extern "C" {
#include "blake3.h"
}

namespace {
std::string hash16(std::span<const std::uint8_t> bytes) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, bytes.data(), bytes.size());
    std::uint8_t out[8];
    blake3_hasher_finalize(&h, out, 8);
    char text[17];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(text + 2 * i, 3, "%02x", out[i]);
    }
    return std::string(text, 16);
}
std::string hash32(std::span<const std::uint8_t> bytes) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, bytes.data(), bytes.size());
    std::uint8_t out[16];
    blake3_hasher_finalize(&h, out, 16);
    char text[33];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(text + 2 * i, 3, "%02x", out[i]);
    }
    return std::string(text, 32);
}
}   

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);

    static Sampler sampler;
    const char* profile = std::getenv("SHADER_PROFILE");
    if (profile) {
        sampler.start(std::string_view(profile) == "incl");
    }
    struct Report {
        const char* mode;
        ~Report() {
            if (mode) {
                sampler.report(mode);
            }
        }
    } report{profile};
    if (args.size() >= 4 && args[1] == "translate") {

        lower::set_host_max_wave(args.size() > 4 ? std::stoul(args[4]) : 32);
        dxil::set_view_heap_limit(args.size() > 5 ? std::stoul(args[5]) : 999999);
        std::optional<std::set<std::string>> only;
        if (args.size() > 6) {
            only.emplace();
            std::ifstream f(args[6]);
            for (std::string line; std::getline(f, line);) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                    line.pop_back();
                }
                only->insert(line);
            }
        }
        std::ofstream file(args[3], std::ios::binary);
        for (const auto& f : corpus_files(args[2])) {
            std::string name = f.stem().string();
            if (only && !only->contains(name)) {
                continue;
            }
            auto data = read(f);
            file << result_line(name, data) << "\n";
        }
        return 0;
    }
    if (args.size() >= 4 && args[1] == "rect") {

        lower::set_host_max_wave(32);
        dxil::set_view_heap_limit(999999);
        auto line = [](std::span<const std::uint8_t> vs, std::optional<std::span<const std::uint8_t>> ps) {
            try {
                auto gs = emit::rect_list_gs(vs, ps);
                return "ok " + hash32(gs) + " " + std::to_string(gs.size());
            } catch (const ShaderError& e) {
                return "err " + e.display();
            }
        };
        std::ofstream file(args[3], std::ios::binary);
        std::optional<std::vector<std::uint8_t>> first_vs;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> pss;
        for (const auto& f : corpus_files(args[2])) {
            std::string name = f.stem().string();
            auto data = read(f);
            emit::HostShader h;
            try {
                h = emit::translate(data);
            } catch (const ShaderError&) {
                continue;
            }
            if (h.stage == HwStage::Vs) {
                file << name << " vs " << line(h.dxil, std::nullopt) << "\n";
                if (!first_vs) {
                    first_vs = h.dxil;
                }
            } else if (h.stage == HwStage::Ps) {
                pss.emplace_back(name, std::move(h.dxil));
            }
        }
        if (first_vs) {
            for (const auto& [name, ps] : pss) {
                file << name << " ps " << line(*first_vs, std::span<const std::uint8_t>(ps)) << "\n";
            }
        }
        return 0;
    }
    if (args.size() >= 4 && args[1] == "dxil") {

        lower::set_host_max_wave(32);
        dxil::set_view_heap_limit(999999);
        auto data = read(args[2]);
        try {
            auto h = emit::translate(data);
            std::ofstream(args[3], std::ios::binary).write(reinterpret_cast<const char*>(h.dxil.data()), static_cast<std::streamsize>(h.dxil.size()));
        } catch (const ShaderError& e) {
            std::fprintf(stderr, "%s\n", e.display().c_str());
            return 1;
        }
        return 0;
    }
    if (args.size() >= 5 && (args[1] == "dump" || args[1] == "hash")) {

        bool full = args[1] == "dump";
        std::optional<std::set<std::string>> only;
        if (args.size() >= 6) {
            only.emplace();
            std::ifstream f(args[5]);
            for (std::string line; std::getline(f, line);) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                    line.pop_back();
                }
                only->insert(line);
            }
        }
        std::ofstream file(args[4], std::ios::binary);
        for (const auto& f : corpus_files(args[3])) {
            std::string name = f.stem().string();
            if (only && !only->contains(name)) {
                continue;
            }
            auto data = read(f);
            std::string text;
            if (args[2] == "native") {
                std::ostringstream o;
                dump_native(data, o);
                text = o.str();
            } else if (args[2] == "ir") {
                dump_ir(data, text);
            } else if (args[2] == "spill") {
                dump_spill(data, text);
            } else if (args[2] == "structure") {
                dump_structure(data, text);
            } else if (args[2] == "resource") {
                dump_resource(data, text);
            } else if (args[2] == "io") {
                dump_io(data, text);
            } else {
                std::fprintf(stderr, "no stage %s\n", args[2].c_str());
                return 2;
            }
            if (full) {
                file << name << "\n" << text;
            } else {
                file << name << " " << hash16(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())) << "\n";
            }
        }
        return 0;
    }
    std::fprintf(stderr, "usage: shader-check dump|hash <stage> <corpus dir> <out.txt> [only.txt] | translate|time <corpus dir> <out.txt>\n");
    return 2;
}
