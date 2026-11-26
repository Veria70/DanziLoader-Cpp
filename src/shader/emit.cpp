// SPDX-License-Identifier: GPL-3.0-or-later
// DXIL emission (`emit/mod.rs`, `emit/sampler.rs`, `emit/threading.rs`;
// DanziLoader / victory-perf, see NOTICE.md). See emit.hpp.
#include "emit.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "dxbc.hpp"
#include "dxil.hpp"
#include "error.hpp"
#include "io.hpp"
#include "ir.hpp"
#include "spill.hpp"
#include "structure.hpp"

namespace danzi::shader::emit {

using container::SigElement;
using ir::Value;
using structure::Atom;
using structure::Bool;
using structure::LaneExit;

namespace {

bool env_set(const char* name) {
    char* v = nullptr;
    std::size_t n = 0;
    bool on = _dupenv_s(&v, &n, name) == 0 && v;
    std::free(v);
    return on;
}


std::optional<std::string> env_value(const char* name) {
    char* v = nullptr;
    std::size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || !v) {
        return std::nullopt;
    }
    std::string s(v);
    std::free(v);
    return s;
}

std::uint32_t u32_at(std::span<const std::uint8_t> data, std::size_t at) {
    if (at + 4 > data.size() || at + 4 < at) {
        malformed("truncated");
    }
    std::uint32_t v;
    std::memcpy(&v, data.data() + at, 4);
    return v;
}


bool plain(const Valu& v) { return v.abs == 0 && v.neg == 0 && v.opsel == 0 && !v.clamp && v.omod == 0 && !v.sdwa && !v.dpp; }

std::optional<std::uint32_t> immediate(const Operand& op) {
    if (op.kind == Operand::Const) {
        return resource::inline_constant(op.a);
    }
    if (op.kind == Operand::Literal) {
        return op.literal;
    }
    return std::nullopt;
}

std::vector<std::array<std::size_t, 3>> constant_edges(const ir::Program& p, const structure::Structured& st) {
    std::unordered_map<Value, std::uint32_t, structure::ValueHash> constants;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const auto& block = p.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const ir::Inst& inst = block.insts[i];
            auto* v = std::get_if<Valu>(&inst.fields);
            if (inst.name() == "v_mov_b32" && !st.guards[b][i] && v && plain(*v) && v->src.size() == 1 && inst.writes.size() == 1) {
                if (auto word = immediate(v->src[0])) {
                    constants[inst.writes[0].second] = *word;
                }
            }
        }
    }
    std::vector<std::array<std::size_t, 3>> edges;
    for (std::size_t join = 0; join < p.blocks.size(); ++join) {
        const auto& block = p.blocks[join];
        const LaneExit& ex = st.exits[join];
        if (ex.kind != LaneExit::Branch) {
            continue;
        }
        if (ex.when == structure::TRUE_ || ex.when == structure::FALSE_) {
            continue;
        }
        bool simple = std::all_of(block.insts.begin(), block.insts.end(), [](const ir::Inst& i) {
            if (std::holds_alternative<SopFields>(i.fields)) {
                return true;
            }
            if (std::holds_alternative<SoppFields>(i.fields)) {
                return i.name().starts_with("s_cbranch_");
            }
            if (std::holds_alternative<Valu>(i.fields)) {
                return i.name().starts_with("v_cmp_");
            }
            return false;
        });
        if (!simple || block.preds.size() != 2) {
            continue;
        }
        std::size_t a = block.preds[0], b2 = block.preds[1];
        auto arm_from = [&](std::size_t arm) -> std::optional<std::size_t> {
            const auto& ab = p.blocks[arm];
            if (ab.preds.size() == 1 && ab.exit == ir::Exit::jump(join)) {
                return ab.preds[0];
            }
            return std::nullopt;
        };
        auto fa = arm_from(a), fbm = arm_from(b2);
        std::size_t fork;
        if (fa && fbm && *fa == *fbm) {
            fork = *fa;
        } else if (fbm && *fbm == a) {
            fork = *fbm;
        } else if (fa && *fa == b2) {
            fork = *fa;
        } else {
            continue;
        }
        if (fork == join || a == b2) {
            continue;
        }
        if (st.exits[fork].kind != LaneExit::Branch) {
            continue;
        }
        auto sup = st.bools.support(st.exits[fork].when);
        if (!std::all_of(sup.begin(), sup.end(), [](const Atom& at) { return at.kind == Atom::Uniform; })) {
            continue;
        }
        for (std::size_t edge = 0; edge < block.preds.size(); ++edge) {
            std::size_t pred = block.preds[edge];
            auto value = [&](Value v) -> std::optional<std::uint32_t> {
                Value input = v;
                for (const ir::Phi& ph : block.phis) {
                    if (ph.value == v) {
                        input = ph.args[edge];
                        break;
                    }
                }
                if (auto it = constants.find(input); it != constants.end()) {
                    return it->second;
                }
                return std::nullopt;
            };
            Bool cond = ex.when;
            for (;;) {
                auto node = st.bools.node(cond);
                if (!node || node->atom.kind != Atom::Lane) {
                    break;
                }
                Value id{static_cast<std::uint32_t>(node->atom.payload)};
                const ir::Inst* inst = nullptr;
                for (const ir::Inst& i : block.insts) {
                    if (std::any_of(i.writes.begin(), i.writes.end(), [&](const auto& w) { return w.second == id; })) {
                        inst = &i;
                        break;
                    }
                }
                if (!inst) {
                    break;
                }
                auto* v = std::get_if<Valu>(&inst->fields);
                if (!v || v->src.size() != 2) {
                    break;
                }
                auto operand = [&](const Operand& op) -> std::optional<std::uint32_t> {
                    if (op.kind == Operand::Vector && op.b == 1) {
                        auto r = inst->read(ir::Reg::v(op.a));
                        if (!r) {
                            return std::nullopt;
                        }
                        return value(*r);
                    }
                    return immediate(op);
                };
                auto xv = operand(v->src[0]);
                auto yv = operand(v->src[1]);
                if (!xv || !yv) {
                    break;
                }
                if (!plain(*v)) {
                    break;
                }
                bool yes;
                auto nm = inst->name();
                if (nm == "v_cmp_eq_u32" || nm == "v_cmp_eq_i32") {
                    yes = *xv == *yv;
                } else if (nm == "v_cmp_ne_u32" || nm == "v_cmp_ne_i32") {
                    yes = *xv != *yv;
                } else {
                    break;
                }
                cond = yes ? node->hi : node->lo;
            }
            if (cond == structure::TRUE_ || cond == structure::FALSE_) {
                edges.push_back({pred, join, cond == structure::TRUE_ ? ex.taken : ex.not_taken});
                break;
            }
        }
    }
    return edges;
}


std::vector<SigElement> signature(std::span<const std::uint8_t> part) {
    std::uint32_t count = u32_at(part, 0);
    std::uint32_t base = u32_at(part, 4);
    std::vector<SigElement> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::size_t at = std::size_t{base} + 32 * std::size_t{i};
        std::uint32_t name_at = u32_at(part, at + 4);
        if (name_at > part.size()) {
            malformed("signature name out of bounds");
        }
        std::string name;
        for (std::size_t k = name_at; k < part.size() && part[k] != 0; ++k) {
            name.push_back(static_cast<char>(part[k]));
        }
        if (at + 24 >= part.size()) {
            malformed("truncated");
        }
        std::uint8_t mask = part[at + 24];
        if (u32_at(part, at + 28) != 0) {
            unsupported(0, "a minimum-precision signature element");
        }
        if (u32_at(part, at) != 0) {
            unsupported(0, "a signature element of another stream");
        }
        std::uint8_t kind;
        switch (u32_at(part, at + 12)) {
            case 0: kind = 0; break;
            case 1: kind = 3; break;
            case 2: kind = 6; break;
            case 3: kind = 7; break;
            case 4: kind = 4; break;
            case 5: kind = 5; break;
            case 6: kind = 1; break;
            case 7: kind = 10; break;
            case 8: kind = 2; break;
            case 9: kind = 13; break;
            case 10: kind = 12; break;
            case 64: kind = 16; break;
            case 65: kind = 17; break;
            case 66: kind = 14; break;
            case 67: kind = 19; break;
            case 68: kind = 18; break;
            case 69: kind = 20; break;
            case 70: kind = 15; break;
            case 23: kind = 28; break;
            case 25: kind = 30; break;
            default: unsupported(0, "a system value in the signature");
        }
        std::uint32_t raw = u32_at(part, at + 16);
        std::uint8_t comp;
        if (kind == 13) {
            comp = 5;
        } else if (raw == 1 || raw == 4) {
            comp = 5;
        } else if (raw == 2 || raw == 5) {
            comp = 4;
        } else if (raw == 3 || raw == 6) {
            comp = 9;
        } else {
            unsupported(0, raw == 0 ? "a signature element of unknown type" : "a 64-bit signature element");
        }
        std::uint32_t reg = u32_at(part, at + 20);
        SigElement e;
        e.name = std::move(name);
        e.index = u32_at(part, at + 8);
        e.kind = kind;
        e.comp = comp;
        e.interp = 0;
        if (reg != UINT32_MAX) {
            e.row = reg;
        }
        e.col = static_cast<std::uint8_t>(std::min(mask ? std::countr_zero(static_cast<unsigned>(mask)) : 32, 3));
        e.cols = static_cast<std::uint8_t>(std::max(std::popcount(static_cast<unsigned>(mask)), 1));
        e.usage = 0;
        e.half = raw >= 4 && raw <= 6;
        out.push_back(std::move(e));
    }
    return out;
}

struct Emitted {
    HostShader shader;
    std::optional<std::uint32_t> wave;
};

Emitted emit_program(const Container& c, const NativeProgram& native, const ir::Program& p, const structure::Structured& st, const io::Io& io,
                     const resource::Resources& res) {
    auto sig = [&](std::uint32_t code) -> std::vector<SigElement> {
        if (const ContainerPart* part = c.part(code)) {
            return signature(part->data);
        }
        return {};
    };
    bool pixel = native.stage == HwStage::Ps;
    std::vector<SigElement> inputs = sig(fourcc("ISG1"));
    std::vector<SigElement> outputs = sig(fourcc("OSG1"));
    auto& interpolated = pixel ? inputs : outputs;
    for (SigElement& e : interpolated) {
        if (e.kind == 3) {
            e.interp = 4;
        } else if (e.kind == 13) {
            e.interp = 1;
        } else if (e.kind == 28) {
            e.interp = 2;
        } else if ((e.kind == 0 || e.kind == 6 || e.kind == 7) && e.comp == 9) {
            e.interp = 2;
        } else if (e.kind == 0) {
            e.interp = 1;
        } else {
            e.interp = 0;
        }
    }
    if (pixel && std::any_of(outputs.begin(), outputs.end(), [](const SigElement& e) { return e.kind == 18 || e.kind == 19; })) {
        for (SigElement& e : inputs) {
            if (e.kind == 3) {
                e.interp = 5;
            }
        }
    }
    std::vector<SigElement> prims = sig(fourcc("PSG1"));
    std::erase_if(prims, [](const SigElement& e) { return e.kind == 10 || e.kind == 30; });
    for (SigElement& e : prims) {
        e.interp = 1;
    }
    std::uint32_t mesh_stride = 0;
    if (native.io.kind == StageIo::Ms) {
        const MeshIo& ms = native.io.ms;
        std::optional<std::uint32_t> threads;
        if (const ContainerPart* psv = c.part(fourcc("PSV0"))) {
            auto d = psv->data;
            if (d.size() >= 4) {
                std::uint32_t n;
                std::memcpy(&n, d.data(), 4);
                if (n >= 48 && d.size() >= 4 + 36 + 4) {
                    std::uint32_t t;
                    std::memcpy(&t, d.data() + 4 + 36, 4);
                    threads = t;
                }
            }
        }
        mesh_stride = threads ? std::max({*threads, ms.max_vertices, ms.max_primitives}) : ms.threads;
    }
    lower::Parts parts{&native, &p, &st, &io, &res, std::move(inputs), std::move(outputs), std::move(prims), mesh_stride};
    lower::Lowered l = lower::lower(parts);
    Emitted out;
    out.wave = l.wave_size;
    HostShader& h = out.shader;
    h.stage = native.stage;
    h.dxil = std::move(l.dxil);
    h.user_data = std::move(l.user_data);
    for (std::size_t n = 0; n < l.samplers.size(); ++n) {
        StaticSampler ss = StaticSampler::from_words(l.samplers[n]);
        if (l.cmp_samplers >> n & 1) {
            ss.filter |= 0x80;
        }
        h.static_samplers.push_back(ss);
    }
    h.samplers = std::move(l.samplers);
    h.root_cbvs = std::move(l.root_cbvs);
    h.root_srvs = std::move(l.root_srvs);
    h.root_uavs = std::move(l.root_uavs);
    h.root_indexed_views = res.root_indexed_views;
    return out;
}


struct Attempt {
    std::optional<Emitted> ok;
    std::optional<ShaderError> err;
    bool done64() const { return ok && ok->wave != std::optional<std::uint32_t>(64); }
};

template <class F>
Attempt attempt(F f) {
    Attempt a;
    try {
        a.ok = f();
    } catch (const ShaderError& e) {
        a.err = e;
    }
    return a;
}

bool unsupported_what(const Attempt& a, std::string_view what) {
    return a.err && a.err->kind == ShaderError::Unsupported && std::string_view(a.err->text) == what;
}

HostShader finish(Attempt&& a) {
    if (a.err) {
        throw *a.err;
    }
    return std::move(a.ok->shader);
}


struct Form {
    ir::Program p;
    structure::Structured st;
    io::Io io;
    resource::Resources res;
};

std::optional<Form> make_form(std::optional<ir::Program> p, const NativeProgram& native) {
    if (!p) {
        return std::nullopt;
    }
    try {
        Form f;
        f.st = structure::structure(*p);
        f.io = io::raise(*p, native);
        f.res = resource::track(*p, native.layout, native.stage, &f.st);
        f.p = std::move(*p);
        return f;
    } catch (const ShaderError&) {
        return std::nullopt;
    }
}

Attempt widen(const Container& c, const NativeProgram& native, const ir::Program& p, bool why) {
    static const ShaderError NOT(ShaderError::Unsupported, "wave-wide branches in a thread group of several console waves", 0);
    if (native.io.kind != StageIo::Cs) {
        return Attempt{std::nullopt, NOT};
    }
    const ComputeIo& cs = native.io.cs;
    bool host_waves = !(native.wave64 && lower::host_max_wave() == 32);
    if (!host_waves && cs.threads[0] * cs.threads[1] * cs.threads[2] != 64) {
        if (why) {
            std::fprintf(stderr, "WAVE64-WIDE not tried: thread groups of [%u, %u, %u]\n", cs.threads[0], cs.threads[1], cs.threads[2]);
        }
        return Attempt{std::nullopt, NOT};
    }
    std::unordered_set<std::size_t> wide;
    for (;;) {
        Attempt r;
        std::vector<std::size_t> needs;
        try {
            structure::Structured st = structure::structure_wide(p, wide);
            io::Io io = io::raise(p, native);
            resource::Resources res = resource::track(p, native.layout, native.stage, &st);
            lower::take_needs();
            r = attempt([&] { return emit_program(c, native, p, st, io, res); });
            needs = lower::take_needs();
            bool stuck = (r.err && r.err->kind == ShaderError::Unsupported &&
                          (lower::needs_every_lane(r.err->text) || (std::string_view(r.err->text) == lower::LANE_MASK && !needs.empty()))) ||
                         (r.ok && r.ok->wave == std::optional<std::uint32_t>(64) && !needs.empty());
            if (!stuck) {
                if (why) {
                    std::fprintf(stderr, "WAVE64-WIDE %zu branches taken wave-wide\n", wide.size());
                }
                return r;
            }
            auto around = lower::branches_around(st, needs);
            std::vector<std::size_t> more;
            if (around) {
                for (std::size_t b : *around) {
                    if (!wide.contains(b)) {
                        more.push_back(b);
                    }
                }
            }
            if (more.empty()) {
                if (why) {
                    std::fprintf(stderr, "WAVE64-WIDE stopped with %zu branches wave-wide: a branch around it cannot be\n", wide.size());
                }
                return r;
            }
            wide.insert(more.begin(), more.end());
        } catch (const ShaderError& e) {
            return Attempt{std::nullopt, e};
        }
    }
}

HostShader emit_all(const Container& c, const NativeProgram& native, const ir::Program& p, const structure::Structured& st, const io::Io& io,
                    const resource::Resources& res) {
    lower::take_needs();
    Attempt original = attempt([&] { return emit_program(c, native, p, st, io, res); });
    bool stuck = original.ok ? original.ok->wave == std::optional<std::uint32_t>(64)
                             : (original.err->kind == ShaderError::Unsupported && lower::needs_every_lane(original.err->text));
    bool exact_only = unsupported_what(original, lower::MASK_ELSEWHERE);
    if (stuck && native.stage == HwStage::Cs && native.wave64 && lower::host_max_wave() == 32) {
        bool why = env_set("VICTORY_WAVE_WHY");
        auto report = [&](const std::string& what, const Attempt& r) {
            if (!why) {
                return;
            }
            if (r.ok) {
                if (r.ok->wave) {
                    std::fprintf(stderr, "WAVE64-RETRY %s: wave Some(%u)\n", what.c_str(), *r.ok->wave);
                } else {
                    std::fprintf(stderr, "WAVE64-RETRY %s: wave None\n", what.c_str());
                }
            } else {
                std::fprintf(stderr, "WAVE64-RETRY %s: %s\n", what.c_str(), r.err->display().c_str());
            }
        };
        auto local_env = env_value("VICTORY_WAVE_LOCAL");
        bool local_on = !local_env || *local_env != "0";
        bool strict = local_env && *local_env == "idiom";
        auto idiom = [&](const ir::Program& fp, const structure::Structured& fst) {
            auto r = lower::wave_local(fp, fst);
            if (!r) {
                return true;
            }
            if (why) {
                std::fprintf(stderr, "WAVE64-RETRY local waves: not the allocation idiom at %#zx: %s\n", r->first, r->second);
            }
            return false;
        };
        std::pair<const char*, std::optional<Form>> forms[2] = {
            {"without early-outs", make_form(ir::without_early_outs(p), native)},
            {"without skips", make_form(ir::without_skips(p, false), native)},
        };
        for (auto& [name, f] : forms) {
            if (f) {
                Attempt again = attempt([&] { return emit_program(c, native, f->p, f->st, f->io, f->res); });
                report(name, again);
                if (again.done64()) {
                    return finish(std::move(again));
                }
            }
        }
        auto wide_env = env_value("VICTORY_WAVE_WIDE");
        bool wide_on = wide_env && *wide_env == "1";
        for (bool exact : {true, false}) {
            if (!exact) {
                auto deep_env = env_value("VICTORY_WAVE_DEEP_SKIPS");
                bool deep_on = !deep_env || *deep_env != "0";
                std::optional<Form> deep;
                if (deep_on) {
                    deep = make_form(ir::without_skips(p, true), native);
                }
                if (deep) {
                    Attempt again = attempt([&] { return emit_program(c, native, deep->p, deep->st, deep->io, deep->res); });
                    report("without skips, scalars followed", again);
                    if (again.done64()) {
                        return finish(std::move(again));
                    }
                }
                if (wide_on) {
                    const ir::Program* bases[4] = {deep ? &deep->p : nullptr, forms[1].second ? &forms[1].second->p : nullptr,
                                                   forms[0].second ? &forms[0].second->p : nullptr, &p};
                    const ir::Program* base = nullptr;
                    for (const ir::Program* bp : bases) {
                        if (bp) {
                            base = bp;
                            break;
                        }
                    }
                    if (base) {
                        Attempt again = widen(c, native, *base, why);
                        report("wave-wide branches", again);
                        if (again.done64()) {
                            return finish(std::move(again));
                        }
                    }
                }
                if (strict) {
                    break;
                }
            }
            if (!local_on || exact_only) {
                continue;
            }
            for (auto& [name, f] : forms) {
                if (f && idiom(f->p, f->st) == exact) {
                    Attempt local =
                        lower::with_local_waves([&] { return attempt([&] { return emit_program(c, native, f->p, f->st, f->io, f->res); }); });
                    report(std::string("local waves (") + (exact ? "idiom" : "APPROXIMATE") + "), " + name, local);
                    if (local.done64()) {
                        return finish(std::move(local));
                    }
                }
            }
            if (idiom(p, st) == exact) {
                Attempt local = lower::with_local_waves([&] { return attempt([&] { return emit_program(c, native, p, st, io, res); }); });
                report(exact ? "local waves (idiom)" : "local waves (APPROXIMATE)", local);
                if (local.done64()) {
                    return finish(std::move(local));
                }
            }
        }
        if (local_on && !exact_only && !strict) {
            for (auto& [name, f] : forms) {
                if (f) {
                    Attempt local =
                        lower::with_local_waves_any([&] { return attempt([&] { return emit_program(c, native, f->p, f->st, f->io, f->res); }); });
                    report(std::string("local waves (APPROXIMATE, any mask), ") + name, local);
                    if (local.done64()) {
                        return finish(std::move(local));
                    }
                }
            }
            Attempt local = lower::with_local_waves_any([&] { return attempt([&] { return emit_program(c, native, p, st, io, res); }); });
            report("local waves (APPROXIMATE, any mask)", local);
            if (local.done64()) {
                return finish(std::move(local));
            }
        }
    }
    auto wide_env = env_value("VICTORY_WAVE_WIDE");
    if (native.stage == HwStage::Cs && unsupported_what(original, lower::LANE_MASK) && (!wide_env || *wide_env != "0")) {
        Attempt again = widen(c, native, p, env_set("VICTORY_WAVE_WHY"));
        if (again.ok) {
            original = std::move(again);
        } else {
            original = attempt([&] { return emit_program(c, native, p, st, io, res); });
        }
    }
    if (!(original.err && original.err->kind == ShaderError::Unsupported)) {
        return finish(std::move(original));
    }
    auto edges = constant_edges(p, st);
    if (edges.empty()) {
        return finish(std::move(original));
    }
    ir::Program tp = ir::thread_edges(p, edges);
    structure::Structured tst = structure::structure(tp);
    io::Io tio = io::raise(tp, native);
    resource::Resources tres = resource::track(tp, native.layout, native.stage, &tst);
    return emit_program(c, native, tp, tst, tio, tres).shader;
}

}   

StaticSampler StaticSampler::from_words(const std::array<std::uint32_t, 4>& w) {
    auto bits = [](std::uint32_t word, std::uint32_t at, std::uint32_t n) { return word >> at & ((1u << n) - 1); };
    static const std::uint32_t ADDRESS[8] = {1, 2, 3, 5, 3, 5, 4, 5};
    std::uint32_t mag = bits(w[2], 20, 2), min = bits(w[2], 22, 2), mip = bits(w[2], 26, 2);
    std::uint32_t reduction;
    switch (bits(w[0], 29, 2)) {
        case 1: reduction = 2; break;
        case 2: reduction = 3; break;
        default: reduction = 0; break;
    }
    StaticSampler s;
    if (mag >= 2 || min >= 2) {
        s.filter = 0x55 | reduction << 7;
    } else {
        s.filter = (min & 1) << 4 | (mag & 1) << 2 | (mip == 2 ? 1u : 0u) | reduction << 7;
    }
    auto lod = [](std::uint32_t v) { return static_cast<float>(v) / 256.0f; };
    if (mip != 0) {
        s.min_lod = lod(bits(w[1], 0, 12));
        s.max_lod = lod(bits(w[1], 12, 12));
    }
    for (int k = 0; k < 3; ++k) {
        s.address[k] = ADDRESS[bits(w[0], 3 * k, 3)];
    }
    s.mip_lod_bias = static_cast<float>(static_cast<std::int32_t>(bits(w[2], 0, 14) << 18) >> 18) / 256.0f;
    s.max_anisotropy = 1u << std::min(bits(w[0], 9, 3), 4u);
    s.comparison = bits(w[0], 12, 3) + 1;
    s.border = bits(w[3], 30, 2) % 3;
    return s;
}

std::uint32_t HostShader::user_data_dwords() const {
    std::uint32_t n = 0;
    for (const auto& u : user_data) {
        n = std::max(n, u.index + 1);
    }
    return (n + 3) / 4 * 4;
}

HostShader translate(std::span<const std::uint8_t> container) { return translate_code(container, std::nullopt); }

HostShader emit(const Container& c, const NativeProgram& native, const ir::Program& p, const structure::Structured& st, const io::Io& io,
                const resource::Resources& res) {
    return emit_all(c, native, p, st, io, res);
}

std::vector<SigElement> part_signature(std::span<const std::uint8_t> part) { return signature(part); }

HostShader translate_code(std::span<const std::uint8_t> bytes, std::optional<std::vector<std::uint8_t>> code) {
    Container c = Container::parse(bytes);
    const ContainerPart* dxil = c.part(fourcc("DXIL"));
    if (!dxil) {
        malformed("no DXIL part");
    }
    std::vector<NativeProgram> programs = native_programs_checked(dxil->data);
    NativeProgram* picked = nullptr;
    for (NativeProgram& np : programs) {
        if (np.stage == HwStage::Vs || np.stage == HwStage::Ps || np.stage == HwStage::Cs || np.stage == HwStage::Ms) {
            picked = &np;
            break;
        }
    }
    if (!picked) {
        unsupported(0, "a container without a vertex, pixel, compute or mesh program");
    }
    NativeProgram& native = *picked;
    if (code) {
        native.code = std::move(*code);
    }
    ir::Program p = spill_build(native);
    std::optional<structure::Structured> st;
    std::optional<ShaderError> st_err;
    try {
        st = structure::structure(p);
    } catch (const ShaderError& e) {
        st_err = e;
    }
    resource::Resources res = resource::track(p, native.layout, native.stage, st ? &*st : nullptr);
    io::Io io = io::raise(p, native);
    if (st_err) {
        throw *st_err;
    }
    return emit_all(c, native, p, *st, io, res);
}


namespace {

constexpr std::uint32_t RECT_VERTICES = 6;
constexpr std::uint32_t RECT_MAX_SCALARS = 1024;

using bitcode::Attr;
using bitcode::Bin;
using bitcode::Term;
using bitcode::V;
namespace pred = bitcode::pred;

V rect_load(dxil::Shader& s, std::size_t id, std::uint8_t col, std::uint32_t vertex) {
    bool f = s.inputs[id].comp == 9;
    const char* name = f ? "loadInput.f32" : "loadInput.i32";
    bitcode::Ty ty = f ? s.t.f32 : s.t.i32;
    V a0 = s.m.i32(static_cast<std::uint32_t>(id));
    V a1 = s.m.i32(0);
    V a2 = s.m.i8(col);
    V a3 = s.m.i32(vertex);
    return s.op(s.entry, name, Attr::ReadNone, ty, 4, {a0, a1, a2, a3});
}
V rect_same(dxil::Shader& s, V a, V b) { return s.m.cmp(s.entry, pred::OEQ, a, b); }
V rect_both(dxil::Shader& s, V a, V b) { return s.m.bin(s.entry, Bin::And, a, b); }
V rect_either(dxil::Shader& s, V a, V b) { return s.m.bin(s.entry, Bin::Or, a, b); }
V rect_not(dxil::Shader& s, V a) {
    V on = s.m.boolean(true);
    return s.m.bin(s.entry, Bin::Xor, a, on);
}

std::vector<std::uint8_t> rect_build(std::vector<SigElement> outputs, const std::vector<SigElement>& ps_inputs) {
    if (std::any_of(outputs.begin(), outputs.end(), [](const SigElement& e) { return !e.row; })) {
        unsupported(0, "a rectangle list output without a register");
    }
    std::optional<std::size_t> position;
    for (std::size_t k = 0; k < outputs.size(); ++k) {
        if (outputs[k].kind == 3 && outputs[k].comp == 9 && outputs[k].cols == 4) {
            position = k;
            break;
        }
    }
    if (!position) {
        unsupported(0, "a rectangle list without a four-component position");
    }
    for (SigElement& e : outputs) {
        if (e.kind == 3) {
            e.interp = 4;
        } else if ((e.kind == 0 || e.kind == 6 || e.kind == 7) && e.comp == 9) {
            e.interp = 2;
        } else if (e.kind == 0) {
            e.interp = 1;
        } else {
            e.interp = 0;
        }
        e.usage = static_cast<std::uint8_t>((1u << e.cols) - 1);
    }
    dxil::Shader s(false);
    s.geometry = dxil::Geometry{RECT_VERTICES};
    s.inputs = outputs;
    const SigElement* wanted = nullptr;
    for (const SigElement& e : ps_inputs) {
        if (e.kind == 10 && e.row) {
            wanted = &e;
            break;
        }
    }
    if (wanted && std::any_of(outputs.begin(), outputs.end(), [](const SigElement& o) { return o.kind == 10; })) {
        wanted = nullptr;
    }
    std::optional<std::size_t> primitive;
    if (wanted) {
        const SigElement& e = *wanted;
        bool clash = std::any_of(outputs.begin(), outputs.end(), [&](const SigElement& o) {
            bool overlap = o.col < e.col + e.cols && e.col < o.col + o.cols;
            return o.row == e.row && (o.interp != 1 || overlap);
        });
        if (clash) {
            unsupported(0, "a rectangle list whose primitive id shares a register with a vertex output");
        }
        SigElement pe = e;
        pe.interp = 1;
        pe.usage = static_cast<std::uint8_t>((1u << e.cols) - 1);
        outputs.push_back(std::move(pe));
        primitive = outputs.size() - 1;
    }
    std::uint32_t scalars = 0;
    for (const auto& e : outputs) {
        scalars += e.cols;
    }
    if (scalars * RECT_VERTICES > RECT_MAX_SCALARS) {
        unsupported(0, "a rectangle list with more output components than a geometry shader emits");
    }
    s.outputs = outputs;
    std::size_t b = s.entry;
    std::vector<std::vector<std::array<V, 3>>> corners;
    for (std::size_t id = 0; id < s.inputs.size(); ++id) {
        std::vector<std::array<V, 3>> columns;
        for (std::uint8_t col = 0; col < s.inputs[id].cols; ++col) {
            std::array<V, 3> v;
            for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
                v[vertex] = rect_load(s, id, col, vertex);
            }
            columns.push_back(v);
        }
        corners.push_back(std::move(columns));
    }
    auto xs = corners[*position][0], ys = corners[*position][1], ws = corners[*position][3];
    V zero = s.m.f32(0);
    V plain = s.m.cmp(b, pred::OGT, ws[0], zero);
    for (std::size_t k = 1; k < 3; ++k) {
        V one_w = rect_same(s, ws[k], ws[0]);
        plain = rect_both(s, plain, one_w);
    }
    for (const auto& [i, j] : {std::pair{0, 1}, std::pair{0, 2}, std::pair{1, 2}}) {
        V same_x = rect_same(s, xs[i], xs[j]);
        V same_y = rect_same(s, ys[i], ys[j]);
        V together = rect_both(s, same_x, same_y);
        V apart = rect_not(s, together);
        plain = rect_both(s, plain, apart);
    }
    std::vector<V> corner;
    for (std::size_t i = 0; i < 3; ++i) {
        std::size_t a = (i + 1) % 3, c = (i + 2) % 3;
        V xa = rect_same(s, xs[i], xs[a]);
        V yc = rect_same(s, ys[i], ys[c]);
        V xc = rect_same(s, xs[i], xs[c]);
        V ya = rect_same(s, ys[i], ys[a]);
        V one = rect_both(s, xa, yc);
        V other = rect_both(s, xc, ya);
        V shares = rect_either(s, one, other);
        V here = rect_both(s, shares, plain);
        for (V found : corner) {
            V earlier = rect_not(s, found);
            here = rect_both(s, here, earlier);
        }
        corner.push_back(here);
    }
    std::vector<std::vector<std::array<V, 6>>> emitted;
    for (std::size_t id = 0; id < corners.size(); ++id) {
        bool is_float = s.inputs[id].comp == 9;
        std::vector<std::array<V, 6>> out;
        for (const auto& [p0, p1, p2] : corners[id]) {
            V fourth = p0;
            if (is_float) {
                const std::array<V, 3> triples[3] = {{p1, p2, p0}, {p2, p0, p1}, {p0, p1, p2}};
                for (std::size_t k = 0; k < 3; ++k) {
                    V sum = s.m.bin(b, Bin::Add, triples[k][0], triples[k][1]);
                    V opposite = s.m.bin(b, Bin::Sub, sum, triples[k][2]);
                    fourth = s.m.select(b, corner[k], opposite, fourth);
                }
            }
            V third = s.m.select(b, corner[0], fourth, p2);
            V fifth = s.m.select(b, corner[1], p2, fourth);
            V sixth = s.m.select(b, corner[0], p2, p1);
            sixth = s.m.select(b, corner[1], fourth, sixth);
            out.push_back({p0, p1, third, p0, fifth, sixth});
        }
        emitted.push_back(std::move(out));
    }
    std::optional<std::pair<std::size_t, V>> number;
    if (primitive) {
        V n = s.op(b, "primitiveID.i32", Attr::ReadNone, s.t.i32, 108, {});
        number = std::pair{*primitive, n};
    }
    V stream = s.m.i8(0);
    for (std::size_t vertex = 0; vertex < RECT_VERTICES; ++vertex) {
        for (std::size_t id = 0; id < emitted.size(); ++id) {
            const char* name = s.outputs[id].comp == 9 ? "storeOutput.f32" : "storeOutput.i32";
            for (std::size_t col = 0; col < emitted[id].size(); ++col) {
                V a0 = s.m.i32(static_cast<std::uint32_t>(id));
                V a1 = s.m.i32(0);
                V a2 = s.m.i8(static_cast<std::uint8_t>(col));
                s.op(b, name, Attr::NoUnwind, s.t.void_, 5, {a0, a1, a2, emitted[id][col][vertex]});
            }
        }
        if (number) {
            V a0 = s.m.i32(static_cast<std::uint32_t>(number->first));
            V a1 = s.m.i32(0);
            V a2 = s.m.i8(0);
            s.op(b, "storeOutput.i32", Attr::NoUnwind, s.t.void_, 5, {a0, a1, a2, number->second});
        }
        s.op(b, "emitStream", Attr::NoUnwind, s.t.void_, 97, {stream});
        if (vertex == 2) {
            s.op(b, "cutStream", Attr::NoUnwind, s.t.void_, 98, {stream});
        }
    }
    s.m.terminate(b, Term::ret());
    return s.finish();
}

}   

std::vector<std::uint8_t> rect_list_gs(std::span<const std::uint8_t> vs, std::optional<std::span<const std::uint8_t>> ps) {
    auto sig = [](std::span<const std::uint8_t> dxil_bytes, std::uint32_t code) -> std::vector<SigElement> {
        Container c = Container::parse(dxil_bytes);
        if (const ContainerPart* part = c.part(code)) {
            return signature(part->data);
        }
        return {};
    };
    std::vector<SigElement> outputs = sig(vs, fourcc("OSG1"));
    std::vector<SigElement> wanted;
    if (ps) {
        wanted = sig(*ps, fourcc("ISG1"));
    }
    return rect_build(std::move(outputs), wanted);
}

}   
