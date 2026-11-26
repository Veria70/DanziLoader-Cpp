// SPDX-License-Identifier: GPL-3.0-or-later
// `victory translate`: the offline batch: every shader found goes through
// the translator, and what it cannot handle yet is tallied (decision 0003)
// (`apps/victory/src/translate.rs`; DanziLoader / victory-perf, see
// NOTICE.md).
//
// Port's choice: the reference translates the sources of one file at a
// time on its workers; the port's workers take the sources of every file,
// a window ahead, so a folder of one-container files is translated in
// parallel too. What depends on the order is kept in file order: the
// sources are read, unpacked, hashed and checked for duplicates in order
// on this thread, and each translation's tally is merged in order (the
// first example of each failure, the shaders `VICTORY_PAIRS` picks from),
// as the reference does with one worker or one source per file.
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

#include "core/config.hpp"
#include "core/log.hpp"
#include "shader/dxbc.hpp"
#include "shader/dxil.hpp"
#include "shader/emit.hpp"
#include "shader/error.hpp"
#include "shader/host.hpp"
#include "shader/io.hpp"
#include "shader/ir.hpp"
#include "shader/lower.hpp"
#include "shader/native.hpp"
#include "shader/resource.hpp"
#include "shader/spill.hpp"
#include "shader/structure.hpp"
#include "shader/validator.hpp"
#include "shaders.hpp"

namespace danzi::victory {

using namespace danzi::shader;
using emit::HostShader;
using emit::validator::Validator;

namespace {


std::optional<std::string> env_var(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) {
        return std::nullopt;
    }
    std::wstring w(n, L'\0');
    n = GetEnvironmentVariableW(name, w.data(), n);
    w.resize(n);
    return config::display(fs::path(w));
}

fs::path utf8_path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

void write_file(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream f(path, std::ios::binary);
    if (f) {
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

void write_text(const fs::path& path, const std::string& text) {
    write_file(path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string trim(std::string_view s) {

    auto ws = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    while (!s.empty() && ws(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && ws(s.back())) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

std::vector<std::string_view> split(std::string_view s, char by) {
    std::vector<std::string_view> out;
    for (;;) {
        auto at = s.find(by);
        out.push_back(s.substr(0, at));
        if (at == std::string_view::npos) {
            return out;
        }
        s.remove_prefix(at + 1);
    }
}

bool alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }


template <class F>
void each_char(const std::string& s, F f) {
    for (std::size_t i = 0; i < s.size();) {
        auto b = static_cast<std::uint8_t>(s[i]);
        std::size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        n = std::min(n, s.size() - i);
        if (!f(std::string_view(s).substr(i, n))) {
            return;
        }
        i += n;
    }
}

const char* stage_name(HwStage s) {
    static const char* const N[] = {"Ls", "Hs", "Es", "Gs", "Vs", "Ps", "Ms", "Cs"};
    return N[static_cast<int>(s)];
}

std::string root_name(std::uint32_t r) {
    switch (r) {
        case 0: return "Constants";
        case 1: return "Descriptor";
        case 3: return "Table";
        case 4: return "ImmediateSampler";
        case 5: return "ResourceHeap";
        case 6: return "SamplerHeap";
        default: return std::format("Other({})", r);
    }
}

std::string resource_name(std::uint8_t k) {
    switch (k) {
        case 0: return "Srv";
        case 1: return "Sampler";
        case 2: return "Cbv";
        case 3: return "Uav";
        default: return std::format("Other({})", k);
    }
}

constexpr std::uint32_t ROOT_CONSTANTS = 0, ROOT_IMMEDIATE_SAMPLER = 4, ROOT_RESOURCE_HEAP = 5, ROOT_SAMPLER_HEAP = 6;


std::string debug_map(const std::map<std::string, std::size_t>& m) {
    std::string s = "{";
    bool first = true;
    for (const auto& [k, n] : m) {
        s += (first ? "" : ", ") + log::debug(k) + ": " + std::to_string(n);
        first = false;
    }
    return s + "}";
}

struct Tally {
    std::size_t containers = 0, duplicates = 0, programs = 0, translated = 0, instructions = 0, structured = 0, vector = 0, guarded = 0;

    std::size_t opaque = 0;
    std::size_t lane_branches = 0, uniform_branches = 0, tracked = 0, loaded = 0, built = 0, constant = 0;

    std::size_t dynamic = 0;
    std::size_t scalar = 0, used = 0;


    std::map<std::string, std::size_t> untraced_used;

    std::size_t unlisted = 0;

    std::size_t io_programs = 0, io_raised = 0, interpolated = 0, vertex_values = 0;

    std::map<std::string, std::size_t> exports;

    std::size_t compressed = 0;


    std::size_t emit_programs = 0, emitted = 0, validated = 0, pipelines = 0;
    std::size_t cs_programs = 0, cs_emitted = 0, cs_validated = 0, cs_pipelines = 0;
    std::size_t ms_programs = 0, ms_emitted = 0, ms_validated = 0, ms_pipelines = 0;

    std::map<std::string, std::pair<std::size_t, std::string>> failed;

    bool list = false;
    std::vector<std::string> lines;

    template <class F>
    void fail(std::string reason, F example) {
        if (list) {
            std::string e = example();
            lines.push_back(e + "  " + reason);
            failed.try_emplace(std::move(reason), 0, std::move(e)).first->second.first += 1;
            return;
        }
        auto it = failed.find(reason);
        if (it == failed.end()) {
            it = failed.emplace(std::move(reason), std::pair<std::size_t, std::string>(0, example())).first;
        }
        it->second.first += 1;
    }

    void merge(Tally&& o) {
        containers += o.containers;
        duplicates += o.duplicates;
        programs += o.programs;
        translated += o.translated;
        instructions += o.instructions;
        structured += o.structured;
        vector += o.vector;
        guarded += o.guarded;
        opaque += o.opaque;
        lane_branches += o.lane_branches;
        uniform_branches += o.uniform_branches;
        tracked += o.tracked;
        loaded += o.loaded;
        built += o.built;
        constant += o.constant;
        dynamic += o.dynamic;
        scalar += o.scalar;
        used += o.used;
        for (const auto& [k, n] : o.untraced_used) {
            untraced_used[k] += n;
        }
        unlisted += o.unlisted;
        io_programs += o.io_programs;
        io_raised += o.io_raised;
        interpolated += o.interpolated;
        vertex_values += o.vertex_values;
        compressed += o.compressed;
        emit_programs += o.emit_programs;
        emitted += o.emitted;
        validated += o.validated;
        pipelines += o.pipelines;
        cs_programs += o.cs_programs;
        cs_emitted += o.cs_emitted;
        cs_validated += o.cs_validated;
        cs_pipelines += o.cs_pipelines;
        ms_programs += o.ms_programs;
        ms_emitted += o.ms_emitted;
        ms_validated += o.ms_validated;
        ms_pipelines += o.ms_pipelines;
        for (const auto& [k, n] : o.exports) {
            exports[k] += n;
        }
        for (auto& l : o.lines) {
            lines.push_back(std::move(l));
        }
        for (auto& [reason, v] : o.failed) {
            failed.try_emplace(reason, 0, std::move(v.second)).first->second.first += v.first;
        }
    }
};


std::size_t& counter(Tally& t, HwStage stage, int k) {
    std::size_t* c[4];
    if (stage == HwStage::Cs) {
        c[0] = &t.cs_programs, c[1] = &t.cs_emitted, c[2] = &t.cs_validated, c[3] = &t.cs_pipelines;
    } else if (stage == HwStage::Ms) {
        c[0] = &t.ms_programs, c[1] = &t.ms_emitted, c[2] = &t.ms_validated, c[3] = &t.ms_pipelines;
    } else {
        c[0] = &t.emit_programs, c[1] = &t.emitted, c[2] = &t.validated, c[3] = &t.pipelines;
    }
    return *c[k];
}


struct Env {

    std::vector<std::string> only;

    std::optional<std::pair<std::string, std::string>> dump_match;

    std::optional<std::string> fail_dump;

    std::optional<std::string> emit_dump;
};


struct Checks {


    bool keep = false;
    std::optional<Validator> validator;
    std::optional<std::pair<emit::host::Host, HostShader>> host;
};


struct Done {
    Tally t;
    std::vector<std::pair<std::string, HostShader>> kept;
};

void tally_io(const io::Io& r, Tally& t) {
    ++t.io_raised;
    for (const auto& [site, i] : r.interps) {
        switch (i.kind) {
            case io::Interp::Interpolated: ++t.interpolated; break;
            case io::Interp::Vertex: ++t.vertex_values; break;
            case io::Interp::Weighted: ++t.interpolated; break;
        }
    }
    for (const auto& e : r.exports) {
        const char* kind = "";
        switch (e.target.kind) {
            case io::Target::Color: kind = "color"; break;
            case io::Target::Depth: kind = "depth"; break;
            case io::Target::Null: kind = "null"; break;
            case io::Target::Position: kind = "position"; break;
            case io::Target::Param: kind = "param"; break;
            case io::Target::Unread: kind = "unread param"; break;
            case io::Target::Primitive: kind = "primitive"; break;
            case io::Target::PrimitiveParam: kind = "primitive param"; break;
        }
        ++t.exports[kind];
        t.compressed += e.compressed ? 1 : 0;
    }
}

bool same_sampler(const emit::StaticSampler& a, const emit::StaticSampler& b) {
    return a.filter == b.filter && a.address == b.address && a.mip_lod_bias == b.mip_lod_bias && a.max_anisotropy == b.max_anisotropy &&
           a.comparison == b.comparison && a.border == b.border && a.min_lod == b.min_lod && a.max_lod == b.max_lod;
}

void tally_resources(const NativeProgram& p, const resource::Resources& r, Tally& t) {
    ++t.tracked;
    t.scalar += r.scalar.size();
    for (const auto& s : r.scalar) {
        t.dynamic += s.second.dynamic ? 1 : 0;
    }
    auto address = [&](const resource::Site& load) -> std::optional<resource::Address> {
        for (const auto& s : r.scalar) {
            if (s.first == load) {
                return s.second;
            }
        }
        return std::nullopt;
    };
    std::set<std::uint8_t> traced;
    for (const auto& d : r.descriptors) {
        for (const resource::Source* leaf : d.source.leaves()) {
            std::optional<resource::Address> a;
            switch (leaf->kind) {
                case resource::Source::Loaded:
                case resource::Source::Fetched:
                    ++t.loaded;
                    a = address(leaf->load);
                    break;
                case resource::Source::Built:
                    ++t.built;
                    t.dynamic += leaf->address.dynamic ? 1 : 0;
                    a = leaf->address;
                    break;
                case resource::Source::Constant: ++t.constant; break;
                default:

                    break;
            }


            if (a && a->param != resource::CODE && a->param < p.layout.size() && p.layout[a->param].root != ROOT_RESOURCE_HEAP &&
                p.layout[a->param].root != ROOT_SAMPLER_HEAP) {
                traced.insert(p.layout[a->param].user_data);
            }
        }
    }

    for (const auto& [v, param, dword] : r.params) {
        const RootParam& l = p.layout[param];
        if (l.root == ROOT_CONSTANTS) {
            traced.insert(l.user_data);
        }
    }
    std::set<std::uint8_t> listed;
    for (const auto& u : p.used) {
        listed.insert(u.user_data);
    }
    t.used += p.used.size();


    std::vector<emit::StaticSampler> values;
    for (const auto& d : r.descriptors) {
        if (d.role != resource::Role::Sampler) {
            continue;
        }
        for (const resource::Source* leaf : d.source.leaves()) {
            if (leaf->kind == resource::Source::Constant && leaf->consts.size() >= 4) {
                auto s = emit::StaticSampler::from_words({leaf->consts[0], leaf->consts[1], leaf->consts[2], leaf->consts[3]});
                if (std::none_of(values.begin(), values.end(), [&](const auto& v) { return same_sampler(v, s); })) {
                    values.push_back(s);
                }
            }
        }
    }


    std::size_t immediate = 0;
    for (const auto& u : p.used) {
        immediate += u.root == ROOT_IMMEDIATE_SAMPLER ? 1 : 0;
    }
    bool samplers_match = !values.empty() && values.size() <= immediate;
    for (const auto& u : p.used) {
        bool matched = u.root == ROOT_IMMEDIATE_SAMPLER ? samplers_match : traced.contains(u.user_data);
        if (!matched) {
            ++t.untraced_used[resource_name(u.kind) + " via " + root_name(u.root)];
        }
    }
    for (std::uint8_t x : traced) {
        t.unlisted += listed.contains(x) ? 0 : 1;
    }
}

void tally_structured(const ir::Program& program, const structure::Structured& s, Tally& t) {
    ++t.structured;
    std::size_t n = std::min(program.blocks.size(), s.guards.size());
    for (std::size_t b = 0; b < n; ++b) {
        for (const auto& i : program.blocks[b].insts) {
            t.vector += i.read(ir::Reg::s(ir::EXEC)) ? 1 : 0;
        }
        for (const auto& g : s.guards[b]) {
            if (!g) {
                continue;
            }
            ++t.guarded;
            auto support = s.bools.support(*g);
            t.opaque += std::any_of(support.begin(), support.end(), [](const structure::Atom& a) { return a.kind == structure::Atom::Opaque; }) ? 1 : 0;
        }
    }
    for (const auto& exit : s.exits) {
        if (exit.kind == structure::LaneExit::Branch) {
            auto support = s.bools.support(exit.when);
            if (std::all_of(support.begin(), support.end(), [](const structure::Atom& a) { return a.kind == structure::Atom::Uniform; })) {
                ++t.uniform_branches;
            } else {
                ++t.lane_branches;
            }
        }
    }
}


std::string first_error_line(const std::string& text) {
    std::vector<std::string_view> lines;
    std::string_view rest = text;
    while (!rest.empty()) {
        auto at = rest.find('\n');
        std::string_view l = rest.substr(0, at);
        if (!l.empty() && l.back() == '\r') {
            l.remove_suffix(1);
        }
        lines.push_back(l);
        if (at == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(at + 1);
    }
    for (auto l : lines) {
        if (l.find("error") != std::string_view::npos) {
            return std::string(l);
        }
    }
    return lines.empty() ? std::string() : std::string(lines.front());
}


template <class Label>
void check(HostShader shader, Tally& t, Checks& checks, const Env& env, Label label, Done& done) {
    if (checks.keep) {
        done.kept.emplace_back(label(), shader);
    }

    if (t.list && env.emit_dump) {
        std::string name;
        each_char(label(), [&](std::string_view c) {
            name += c.size() == 1 && alnum(c[0]) ? c[0] : '_';
            return true;
        });
        std::string tail = name.substr(name.size() > 100 ? name.size() - 100 : 0);
        write_file(utf8_path(*env.emit_dump + "/" + tail + ".dxil"), shader.dxil);
    }
    if (checks.host) {
        const auto& [h, stub_ps] = *checks.host;
        std::optional<std::string> made;
        switch (shader.stage) {
            case HwStage::Vs: made = emit::host::pipeline(h, shader, stub_ps); break;
            case HwStage::Cs: made = emit::host::compute_pipeline(h, shader); break;
            case HwStage::Ms: made = emit::host::mesh_pipeline(h, shader, stub_ps); break;
            default: {
                std::string error;
                auto vs = emit::host::stub_vs(shader, error);
                made = vs ? emit::host::pipeline(h, *vs, shader) : std::optional<std::string>(error);
                break;
            }
        }
        if (!made) {
            ++counter(t, shader.stage, 3);
            if (t.list) {
                t.lines.push_back(label() + "  pipeline created");
            }
        } else {
            t.fail("pipeline: " + *made, label);
        }
    }
    if (!checks.validator) {
        return;
    }
    auto rejected = checks.validator->validate(shader.dxil);
    if (!rejected) {
        ++counter(t, shader.stage, 2);
        if (t.list) {
            t.lines.push_back(label() + "  validated");
        }
        return;
    }
    const std::string& text = *rejected;
    std::string first = first_error_line(text);
    if (env.emit_dump) {
        auto f = t.failed.find("validator: " + first);
        std::size_t n = f == t.failed.end() ? 0 : f->second.first;
        if (n < 3) {
            std::string stem;
            for (char c : first) {
                if (alnum(c) && stem.size() < 40) {
                    stem += c;
                }
            }
            std::string name = std::format("{}/{}_{}", *env.emit_dump, stem, n);
            write_file(utf8_path(name + ".dxil"), shader.dxil);
            write_text(utf8_path(name + ".txt"), label() + "\n" + text);
        }
    }
    t.fail("validator: " + first, label);
}

void translate_programs(const Container& container, std::vector<NativeProgram>& programs, const std::function<std::string()>& example, Tally& t,
                        Checks& checks, const Env& env, Done& done) {
    for (const NativeProgram& p : programs) {
        ++t.programs;
        auto staged = [&] { return example() + " " + stage_name(p.stage); };
        auto at = [&](std::size_t offset) { return [&, offset] { return std::format("{} {} at {:#x}", example(), stage_name(p.stage), offset); }; };
        ir::Program program;
        try {
            program = spill_build(p);
        } catch (const ShaderError& e) {
            std::string reason = e.kind == ShaderError::Unsupported ? std::string("unsupported: ") + e.text : e.display();
            t.fail(std::move(reason), staged);
            continue;
        }
        ++t.translated;
        for (const auto& b : program.blocks) {
            t.instructions += b.insts.size();
        }


        std::optional<structure::Structured> structured;
        std::optional<ShaderError> structure_error;
        try {
            structured = structure::structure(program);
        } catch (const ShaderError& e) {
            structure_error = e;
        }
        std::optional<resource::Resources> res;
        try {
            res = resource::track(program, p.layout, p.stage, structured ? &*structured : nullptr);
            tally_resources(p, *res, t);
        } catch (const ShaderError& e) {
            res.reset();
            if (e.kind == ShaderError::Unsupported) {
                t.fail(std::string("resources: ") + e.text, at(e.offset));
            } else {
                t.fail(e.display(), staged);
            }
        }
        bool graphics = p.stage == HwStage::Vs || p.stage == HwStage::Ps;
        bool compute = p.stage == HwStage::Cs;
        std::optional<io::Io> raised;
        if (graphics || compute || p.stage == HwStage::Ms) {
            t.io_programs += graphics ? 1 : 0;
            try {
                raised = io::raise(program, p);
                if (graphics) {
                    tally_io(*raised, t);
                }
            } catch (const ShaderError& e) {
                raised.reset();
                if (e.kind == ShaderError::Unsupported) {
                    t.fail(std::string("I/O: ") + e.text, at(e.offset));
                } else {
                    t.fail(e.display(), staged);
                }
            }
        }
        if (structured) {
            tally_structured(program, *structured, t);
        } else {
            const ShaderError& e = *structure_error;
            std::string reason = e.kind == ShaderError::Unsupported || e.kind == ShaderError::Isa ? std::string("structure: ") + e.text : e.display();
            t.fail(std::move(reason), staged);
        }
        if (res && raised && structured) {
            ++counter(t, p.stage, 0);
            try {
                HostShader shader = emit::emit(container, p, program, *structured, *raised, *res);

                ++counter(t, p.stage, 1);
                check(std::move(shader), t, checks, env, staged, done);
            } catch (const ShaderError& e) {
                if (e.kind == ShaderError::Unsupported) {
                    t.fail(std::string("emit: ") + e.text, [&] { return std::format("{} at {:#x}", staged(), e.offset); });
                } else {
                    t.fail("emit: " + e.display(), staged);
                }
            }
        }
    }
}


struct Job {
    std::size_t index;


    std::shared_ptr<const std::vector<std::uint8_t>> file;
    std::vector<std::uint8_t> unpacked;
    std::span<const std::uint8_t> bytes;

    std::string example;
};

Done translate_job(const Job& job, bool list, Checks& checks, const Env& env) {
    Done done;
    Tally& t = done.t;
    t.list = list;
    Container container = Container::parse(job.bytes);
    std::span<const std::uint8_t> dxil = container.part(fourcc("DXIL"))->data;
    auto example = [&] { return job.example; };
    std::vector<NativeProgram> programs;
    try {
        programs = native_programs_checked(dxil);
    } catch (const ShaderError& e) {
        t.fail(e.display(), example);
        return done;
    }


    std::optional<std::map<std::string, std::size_t>> before;
    if (env.fail_dump) {
        before.emplace();
        for (const auto& [k, v] : t.failed) {
            (*before)[k] = v.first;
        }
    }
    translate_programs(container, programs, example, t, checks, env, done);
    if (env.fail_dump) {
        for (const auto& [reason, v] : t.failed) {
            auto b = before->find(reason);
            std::size_t was = b == before->end() ? 0 : b->second;
            if (v.first > was && was < 3) {
                std::string stem;
                each_char(reason, [&](std::string_view c) {
                    if (stem.size() >= 60) {
                        return false;
                    }
                    stem += c.size() == 1 && alnum(c[0]) ? c[0] : '_';
                    return true;
                });
                std::error_code ec;
                fs::create_directories(utf8_path(*env.fail_dump), ec);
                write_file(utf8_path(*env.fail_dump) / utf8_path(std::format("{}_{}.cdxil", stem, was)), job.bytes);
            }
        }
    }
    return done;
}


class Pool {
public:
    Pool(std::size_t threads, std::function<Done(const Job&, Checks&)> run, std::function<Checks()> make_checks) : run_(std::move(run)) {
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this, make_checks] {
                Checks checks = make_checks();
                for (;;) {
                    std::unique_ptr<Job> job;
                    {
                        std::unique_lock l(lock_);
                        work_.wait(l, [&] { return stop_ || !queue_.empty(); });
                        if (queue_.empty()) {
                            return;
                        }
                        job = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    Done d = run_(*job, checks);
                    {
                        std::lock_guard l(lock_);
                        done_[job->index] = std::move(d);
                    }
                    ready_.notify_all();
                }
            });
        }
    }
    ~Pool() {
        {
            std::lock_guard l(lock_);
            stop_ = true;
        }
        work_.notify_all();
        for (auto& w : workers_) {
            w.join();
        }
    }
    std::size_t submitted() const { return next_; }
    void submit(std::unique_ptr<Job> job) {
        job->index = next_++;
        {
            std::lock_guard l(lock_);
            queue_.push_back(std::move(job));
        }
        work_.notify_one();
    }


    std::optional<Done> take(bool wait) {
        if (taken_ == next_) {
            return std::nullopt;
        }
        std::unique_lock l(lock_);
        if (wait) {
            ready_.wait(l, [&] { return done_.contains(taken_); });
        }
        auto it = done_.find(taken_);
        if (it == done_.end()) {
            return std::nullopt;
        }
        Done d = std::move(it->second);
        done_.erase(it);
        ++taken_;
        return d;
    }
    std::size_t pending() const { return next_ - taken_; }

private:
    std::function<Done(const Job&, Checks&)> run_;
    std::vector<std::thread> workers_;
    std::mutex lock_;
    std::condition_variable work_, ready_;
    std::deque<std::unique_ptr<Job>> queue_;
    std::map<std::size_t, Done> done_;
    bool stop_ = false;
    std::size_t next_ = 0, taken_ = 0;
};

}   

void translate_env() {
    if (auto v = env_var(L"VICTORY_VIEW_LIMIT")) {
        if (auto n = parse_u32(*v)) {
            dxil::set_view_heap_limit(*n);
        }
    }
    if (auto v = env_var(L"VICTORY_HOST_WAVE")) {
        if (auto n = parse_u32(*v)) {
            lower::set_host_max_wave(*n);
        }
    }
}

void translate(std::vector<fs::path> paths_in, DefaultPaths default_paths, bool validate, bool pipelines, bool list, std::optional<std::size_t> jobs) {
    binary_output();
    auto paths = default_paths(std::move(paths_in));


    if (auto v = env_var(L"VICTORY_HOST_MAX_WAVE")) {
        if (auto n = parse_u32(*v)) {
            lower::set_host_max_wave(*n);
        }
    }
    std::optional<fs::path> dll = validate ? emit::validator::default_path() : std::nullopt;
    if (validate) {
        std::string error;
        if (!Validator::load(dll, error)) {
            throw config::Error({error});
        }
        say("validator    " + (dll ? config::display(*dll) : std::string("dxil.dll")));
    }
    if (pipelines) {
        std::string error;
        if (!emit::host::make_host(false, error)) {
            throw config::Error({error});
        }
    }
    std::vector<std::pair<std::string, std::string>> pairs;
    for (auto p : split(env_var(L"VICTORY_PAIRS").value_or(""), ',')) {
        auto plus = p.find('+');
        if (plus != std::string_view::npos) {
            pairs.emplace_back(trim(p.substr(0, plus)), trim(p.substr(plus + 1)));
        }
    }
    Env env;
    for (auto h : split(env_var(L"VICTORY_TRANSLATE_ONLY").value_or(""), ',')) {
        std::string o = trim(h);
        for (char& c : o) {
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        if (!o.empty()) {
            env.only.push_back(o);
        }
    }
    if (auto spec = env_var(L"VICTORY_DUMP_MATCH")) {
        auto at = spec->find(';');
        if (at != std::string::npos) {
            env.dump_match.emplace(spec->substr(0, at), spec->substr(at + 1));
        }
    }
    env.fail_dump = env_var(L"VICTORY_FAIL_DUMP");
    env.emit_dump = env_var(L"VICTORY_EMIT_DUMP");

    std::size_t threads = jobs.value_or(available_parallelism());
    Tally total;
    total.list = list;
    std::vector<std::pair<std::string, HostShader>> kept;
    auto merge = [&](Done&& d) {
        total.merge(std::move(d.t));
        for (auto& k : d.kept) {
            kept.push_back(std::move(k));
        }
    };
    {
        bool keep = !pairs.empty();
        Pool pool(
            threads, [&](const Job& job, Checks& checks) { return translate_job(job, list, checks, env); },
            [&] {
                Checks c;
                c.keep = keep;
                if (validate) {
                    std::string error;
                    c.validator = Validator::load(dll, error);
                }
                if (pipelines) {
                    std::string error;
                    if (auto h = emit::host::make_host(false, error)) {
                        c.host.emplace(std::move(*h), emit::host::stub_ps());
                    }
                }
                return c;
            });
        const std::size_t window = std::max<std::size_t>(64, threads * 16);
        std::unordered_set<std::string> seen;

        for (const auto& root : threads == 0 ? std::vector<fs::path>() : paths) {
            for (const auto& entry : walk_files(root)) {
                if (entry.len > MAX_FILE) {
                    continue;
                }
                auto read = read_whole(entry.path);
                if (!read) {
                    continue;
                }
                auto file = std::make_shared<const std::vector<std::uint8_t>>(std::move(*read));
                std::string shown = config::display(entry.path);
                for (const auto& source : sources(*file)) {
                    auto job = std::make_unique<Job>();
                    job->file = file;
                    Tally& t = total;
                    std::string label = source.label();
                    auto example0 = [&] { return shown + label; };
                    auto bytes = source.bytes(job->unpacked);
                    if (!bytes) {
                        t.fail("corrupt record", example0);
                        continue;
                    }
                    job->bytes = *bytes;


                    std::string hash = hex_prefix(blake3_of(job->bytes), 16);
                    if (!env.only.empty() && std::none_of(env.only.begin(), env.only.end(), [&](const std::string& o) { return hash.starts_with(o); })) {
                        continue;
                    }
                    auto example1 = [&] { return shown + label + " #" + hash; };
                    ++t.containers;
                    std::optional<Container> container;
                    try {
                        container = Container::parse(job->bytes);
                    } catch (const std::exception&) {
                    }
                    const ContainerPart* dxil = container ? container->part(fourcc("DXIL")) : nullptr;
                    if (!dxil) {
                        t.fail("no DXIL part (SM5 or older)", example1);
                        continue;
                    }


                    auto code = blake3_of(dxil->data);
                    std::string code8 = hex_prefix(code, 8);
                    job->example = example1() + " ~" + code8;


                    if (env.dump_match) {
                        const auto& [dir, texts] = *env.dump_match;
                        bool hit = false;
                        for (auto text : split(texts, '|')) {
                            hit = hit || (!text.empty() && job->example.find(text) != std::string::npos);
                        }
                        if (hit) {
                            std::string name;
                            std::size_t taken = 0;
                            bool skipping = true;
                            each_char(label, [&](std::string_view c) {
                                skipping = skipping && c != " ";
                                if (!skipping && taken < 70 && c.size() == 1 && (alnum(c[0]) || c[0] == '_')) {
                                    name += c[0];
                                    ++taken;
                                }
                                return true;
                            });
                            std::error_code ec;
                            fs::create_directories(utf8_path(dir), ec);
                            write_file(utf8_path(dir) / utf8_path(name + "." + hash + ".cdxil"), job->bytes);
                        }
                    }
                    if (!seen.insert(std::string(reinterpret_cast<const char*>(code.data()), code.size())).second) {
                        ++t.duplicates;
                        if (t.list) {
                            t.lines.push_back(job->example + "  duplicate");
                        }
                        continue;
                    }
                    pool.submit(std::move(job));
                    while (auto d = pool.take(pool.pending() >= window)) {
                        merge(std::move(*d));
                    }
                }
            }
        }
        while (auto d = pool.take(true)) {
            merge(std::move(*d));
        }
    }
    const Tally& t = total;
    say(std::format("containers   {} ({} duplicates skipped)", t.containers, t.duplicates));
    say(std::format("programs     {} translated of {} ({} IR instructions)", t.translated, t.programs, t.instructions));
    say(std::format("structured   {} of {} ({} vector instructions, {} guarded, {} on opaque masks; branches {} per lane, {} uniform)", t.structured,
                    t.translated, t.vector, t.guarded, t.opaque, t.lane_branches, t.uniform_branches));
    say(std::format("resources    {} of {} tracked (descriptors {} loaded, {} built, {} constant; {} scalar accesses; {} with a computed term)", t.tracked,
                    t.translated, t.loaded, t.built, t.constant, t.scalar, t.dynamic));
    std::size_t untraced = 0;
    for (const auto& [k, n] : t.untraced_used) {
        untraced += n;
    }
    say(std::format("  used-resource entries {}: {} not traced {}; {} traced parameters not listed", t.used, untraced, debug_map(t.untraced_used),
                    t.unlisted));
    say(std::format("I/O          {} of {} vertex and pixel programs raised ({} interpolations, {} vertex values; exports {}, {} still f16 pairs)",
                    t.io_raised, t.io_programs, t.interpolated, t.vertex_values, debug_map(t.exports), t.compressed));
    say(std::format("DXIL         {} emitted of {} vertex and pixel programs; {} validated, {} pipelines", t.emitted, t.emit_programs, t.validated,
                    t.pipelines));
    say(std::format("             {} emitted of {} compute programs; {} validated, {} pipelines", t.cs_emitted, t.cs_programs, t.cs_validated,
                    t.cs_pipelines));
    say(std::format("             {} emitted of {} mesh programs; {} validated, {} pipelines", t.ms_emitted, t.ms_programs, t.ms_validated,
                    t.ms_pipelines));
    if (!pairs.empty()) {
        std::string error;
        auto host = emit::host::make_host(false, error);
        if (!host) {
            throw config::Error({error});
        }
        auto find = [&](const std::string& name) -> const HostShader* {
            for (const auto& [l, s] : kept) {
                if (l.find(name) != std::string::npos) {
                    return &s;
                }
            }
            return nullptr;
        };
        for (const auto& [v, p] : pairs) {
            std::string made;
            const HostShader* vs = find(v);
            const HostShader* ps = find(p);
            if (vs && ps) {
                auto e = vs->stage == HwStage::Ms ? emit::host::mesh_pipeline(*host, *vs, *ps) : emit::host::pipeline(*host, *vs, *ps);
                made = e ? *e : "pipeline created";
            } else {
                made = "not emitted";
            }
            say("pair " + v + " + " + p + ": " + made);
        }
    }
    std::vector<std::string> lines = t.lines;
    std::sort(lines.begin(), lines.end());
    for (const auto& l : lines) {
        say(l);
    }
    std::vector<std::pair<std::string, std::pair<std::size_t, std::string>>> failed(t.failed.begin(), t.failed.end());
    std::stable_sort(failed.begin(), failed.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    for (const auto& [reason, v] : failed) {
        say(std::format("  {:>7}  {}  (e.g. {})", v.first, reason, v.second));
    }
}

}   
