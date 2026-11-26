// SPDX-License-Identifier: GPL-3.0-or-later
// I/O raising (`io.rs`; DanziLoader / victory-perf, see NOTICE.md).
#include "io.hpp"

#include <algorithm>
#include <unordered_map>

#include "error.hpp"

namespace danzi::shader::io {

using ir::Reg;
using ir::Value;

namespace {


struct PsVgpr {
    std::uint8_t count;
    std::optional<Barycentric> kind;
};
const PsVgpr PS_VGPRS[16] = {
    {2, Barycentric::PerspectiveSample},
    {2, Barycentric::PerspectiveCenter},
    {2, Barycentric::PerspectiveCentroid},
    {3, Barycentric::PerspectivePullModel},
    {2, Barycentric::LinearSample},
    {2, Barycentric::LinearCenter},
    {2, Barycentric::LinearCentroid},
    {1, std::nullopt},   
    {1, std::nullopt},   
    {1, std::nullopt},   
    {1, std::nullopt},   
    {1, std::nullopt},   
    {1, std::nullopt},   
    {1, std::nullopt},   
    {1, std::nullopt},   
    {1, std::nullopt},   
};

Input simple(Input::Kind k, std::uint8_t index = 0) {
    Input i;
    i.kind = k;
    i.index = index;
    return i;
}

Input ps_system(std::size_t bit) {
    switch (bit) {
        case 7: return simple(Input::LineStipple);
        case 8:
        case 9:
        case 10:
        case 11: return simple(Input::Position, static_cast<std::uint8_t>(bit - 8));
        case 12: return simple(Input::FrontFace);
        case 13: return simple(Input::Ancillary);
        case 14: return simple(Input::SampleCoverage);
        default: return simple(Input::FixedPosition);
    }
}

std::vector<std::pair<std::uint8_t, Input>> ps_vgprs(std::uint32_t addr) {
    std::vector<std::pair<std::uint8_t, Input>> out;
    std::uint8_t next = 0;
    for (std::size_t bit = 0; bit < 16; ++bit) {
        if ((addr >> bit & 1) == 0) {
            continue;
        }
        const PsVgpr& e = PS_VGPRS[bit];
        for (std::uint8_t index = 0; index < e.count; ++index) {
            Input input;
            if (e.kind) {
                input.kind = Input::Bary;
                input.bary = *e.kind;
                input.index = index;
            } else {
                input = ps_system(bit);
            }
            out.emplace_back(static_cast<std::uint8_t>(next + index), input);
        }
        next = static_cast<std::uint8_t>(next + e.count);
    }
    return out;
}


struct Pair {
    std::optional<Barycentric> barycentric;
    std::vector<Site> first;
    Interp with(const Attribute& attribute) const {
        Interp i;
        i.attribute = attribute;
        if (barycentric) {
            i.kind = Interp::Interpolated;
            i.barycentric = *barycentric;
            i.first = first;
        } else {
            i.kind = Interp::Weighted;
        }
        return i;
    }
};

void push_pair(Io& io, Site site, const Pair& pair, const Attribute& attribute) {
    if (!pair.barycentric) {
        for (const Site& f : pair.first) {
            if (std::none_of(io.interps.begin(), io.interps.end(), [&](const auto& e) { return e.first == f; })) {
                Interp w;
                w.kind = Interp::Weighted;
                w.attribute = attribute;
                io.interps.emplace_back(f, w);
            }
        }
    }
    io.interps.emplace_back(site, pair.with(attribute));
}

struct Halves {
    const ir::Program& p;
    const std::vector<std::optional<Site>>& defs;
    const std::vector<std::optional<std::pair<std::size_t, std::size_t>>>& phis;

    template <class B>
    const char* pair(const Operand& attr, std::optional<Value> acc, std::optional<Value> j, const B& barycentric, Pair& out) const {
        static constexpr const char* NO_FIRST = "an interpolation without its first half";
        std::vector<Site> first;
        if (!acc) {
            return NO_FIRST;
        }
        std::vector<Value> seen;
        if (const char* e = sites(*acc, seen, first)) {
            return e;
        }
        std::optional<Barycentric> kind;
        if (auto b = barycentric(j); b && b->second == 1) {
            kind = b->first;
        }
        for (const auto& [b, i] : first) {
            const ir::Inst& inst = p.blocks[b].insts[i];
            Operand a;
            std::optional<Value> iv;
            if (auto* f = std::get_if<VintrpFields>(&inst.fields); f && f->src.kind == Operand::Vector && inst.name() == "v_interp_p1_f32") {
                a = f->attr;
                iv = inst.read(Reg::v(f->src.a));
            } else if (auto* v = std::get_if<Valu>(&inst.fields); v && inst.name() == "v_interp_p1ll_f16") {
                if (v->src.size() > 1 && v->src[1].kind == Operand::Vector) {
                    a = v->src[0];
                    iv = inst.read(Reg::v(v->src[1].a));
                } else {
                    return NO_FIRST;
                }
            } else {
                return NO_FIRST;
            }
            if (!(a == attr)) {
                return "interpolation halves of different attributes";
            }
            if (kind) {
                auto bi = barycentric(iv);
                if (!(bi && bi->first == *kind && bi->second == 0)) {
                    kind.reset();
                }
            }
        }
        out.barycentric = kind;
        out.first = std::move(first);
        return nullptr;
    }

    const char* sites(Value v, std::vector<Value>& seen, std::vector<Site>& out) const {
        if (v.id < defs.size() && defs[v.id]) {
            Site site = *defs[v.id];
            if (std::find(out.begin(), out.end(), site) == out.end()) {
                out.push_back(site);
            }
            return nullptr;
        }
        if (v.id >= phis.size() || !phis[v.id]) {
            return "an interpolation without its first half";
        }
        auto [b, i] = *phis[v.id];
        if (std::find(seen.begin(), seen.end(), v) == seen.end()) {
            seen.push_back(v);
            for (Value a : p.blocks[b].phis[i].args) {
                if (const char* e = sites(a, seen, out)) {
                    return e;
                }
            }
        }
        return nullptr;
    }
};

}   

Io raise(const ir::Program& p, const NativeProgram& native) {
    std::vector<std::pair<Reg, Input>> loaded;
    const std::vector<PsInput>* ps_inputs = nullptr;
    const std::vector<VsOutput>* params = nullptr;
    static const std::vector<PsInput> no_ps;
    static const std::vector<VsOutput> no_params;
    ps_inputs = &no_ps;
    params = &no_params;
    const StageIo& sio = native.io;
    if (sio.kind == StageIo::Ps && native.stage == HwStage::Ps) {
        for (const auto& [r, i] : ps_vgprs(sio.input_addr)) {
            loaded.emplace_back(Reg::v(r), i);
        }
        ps_inputs = &sio.ps_inputs;
    } else if (sio.kind == StageIo::Vs && native.stage == HwStage::Vs) {
        loaded = {{Reg::v(0), simple(Input::VertexId)}, {Reg::v(2), simple(Input::PrimitiveId)}, {Reg::v(3), simple(Input::InstanceId)}};
        for (const VsInput& i : sio.vs_inputs) {
            for (std::uint8_t channel = 0; channel < 4; ++channel) {
                if ((i.mask >> channel & 1) == 0) {
                    continue;
                }
                Input input;
                input.kind = Input::Attribute;
                input.row = i.row;
                input.channel = channel;
                loaded.emplace_back(Reg::v(static_cast<std::uint8_t>(i.vgpr + channel)), input);
            }
        }
        params = &sio.vs_outputs;
    } else if (sio.kind == StageIo::Cs && native.stage == HwStage::Cs) {
        std::uint8_t ids[3], info;
        sio.cs.system_sgprs(ids, info);
        for (std::uint8_t c = 0; c < sio.cs.thread_id_vgprs(); ++c) {
            loaded.emplace_back(Reg::v(c), simple(Input::ThreadIdInGroup, c));
        }
        for (std::uint8_t c = 0; c < 3; ++c) {
            if (ids[c] != 0xFF) {
                loaded.emplace_back(Reg::s(ids[c]), simple(Input::GroupId, c));
            }
        }
        if (info != 0xFF) {
            loaded.emplace_back(Reg::s(info), simple(Input::GroupInfo));
        }
    } else if (sio.kind == StageIo::Ms && native.stage == HwStage::Ms) {

        std::vector<Value> group;
        for (const auto& [r, v] : p.inputs) {
            if (r == Reg::v(5)) {
                group.push_back(v);
            }
        }


        auto no_lanes = [&](std::size_t q, std::size_t b) {
            const ir::Block& k = p.blocks[q];
            if (k.insts.empty() || k.exit.kind != ir::Exit::Branch) {
                return false;
            }
            auto n = k.insts.back().name();
            if (n == "s_cbranch_execz") {
                return k.exit.a == b && k.exit.b != b;
            }
            if (n == "s_cbranch_execnz") {
                return k.exit.b == b && k.exit.a != b;
            }
            return false;
        };
        auto contains = [&](Value v) { return std::find(group.begin(), group.end(), v) != group.end(); };
        for (;;) {
            std::optional<Value> found;
            for (std::size_t b = 0; b < p.blocks.size() && !found; ++b) {
                for (const ir::Phi& f : p.blocks[b].phis) {
                    bool carried = false;
                    for (std::size_t k = 0; k < f.args.size() && k < p.blocks[b].preds.size(); ++k) {
                        if (contains(f.args[k]) && !no_lanes(p.blocks[b].preds[k], b)) {
                            carried = true;
                            break;
                        }
                    }
                    if (carried && !contains(f.value)) {
                        found = f.value;
                        break;
                    }
                }
            }
            if (!found) {
                break;
            }
            group.push_back(*found);
        }


        for (const ir::Block& b : p.blocks) {
            for (const ir::Inst& i : b.insts) {
                auto r = i.read(Reg::v(5));
                if (!r || !contains(*r) || i.name() == "v_readfirstlane_b32") {
                    continue;
                }
                bool per_lane = !i.write(Reg::v(5));
                if (!per_lane) {
                    auto reads = ir::operand_reads(p, i);
                    per_lane = std::find(reads.begin(), reads.end(), Reg::v(5)) != reads.end();
                }
                if (per_lane) {
                    unsupported(i.machine.offset, "the mesh group input read per lane");
                }
            }
        }
        loaded = {{Reg::v(5), simple(Input::MeshGroup)}, {Reg::s(3), simple(Input::MergedWaveInfo)}};
        params = &sio.ms.outputs;
    } else {
        unsupported(0, "I/O of this stage");
    }
    std::vector<std::pair<Value, Input>> inputs;
    for (const auto& [reg, v] : p.inputs) {
        for (const auto& l : loaded) {
            if (l.first == reg) {
                inputs.emplace_back(v, l.second);
                break;
            }
        }
    }
    auto input_of = [&](Value v) -> std::optional<Input> {
        for (const auto& i : inputs) {
            if (i.first == v) {
                return i.second;
            }
        }
        return std::nullopt;
    };
    auto attribute = [&](const ir::Inst& inst, const Operand& attr) -> Attribute {
        if (attr.kind != Operand::Attr) {
            unsupported(inst.machine.offset, "an interpolation operand");
        }
        for (const PsInput& i : *ps_inputs) {
            if (i.attr == attr.a) {
                return Attribute{i.usage, i.row, attr.b, attr.high};
            }
        }
        unsupported(inst.machine.offset, "an interpolated attribute the stage data does not list");
    };
    auto barycentric = [&](std::optional<Value> v) -> std::optional<std::pair<Barycentric, std::uint8_t>> {
        if (!v) {
            return std::nullopt;
        }
        auto i = input_of(*v);
        if (i && i->kind == Input::Bary) {
            return std::pair{i->bary, i->index};
        }
        return std::nullopt;
    };

    Io io;
    io.inputs = inputs;
    if (sio.kind == StageIo::Cs) {
        io.initialized_sgprs = sio.cs.initialized_sgprs();
    }
    std::uint32_t n = 0;
    for (const auto& b : p.blocks) {
        for (const auto& phi : b.phis) {
            n = std::max(n, phi.value.id + 1);
        }
        for (const auto& inst : b.insts) {
            for (const auto& w : inst.writes) {
                n = std::max(n, w.second.id + 1);
            }
        }
    }
    std::vector<std::optional<Site>> defs(n);
    std::vector<std::optional<std::pair<std::size_t, std::size_t>>> phis(n);
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        for (std::size_t i = 0; i < p.blocks[b].insts.size(); ++i) {
            for (const auto& w : p.blocks[b].insts[i].writes) {
                defs[w.second.id] = Site{b, i};
            }
        }
        for (std::size_t i = 0; i < p.blocks[b].phis.size(); ++i) {
            phis[p.blocks[b].phis[i].value.id] = std::pair{b, i};
        }
    }
    Halves halves{p, defs, phis};
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const ir::Block& block = p.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const ir::Inst& inst = block.insts[i];
            Site site{b, i};
            std::size_t at = inst.machine.offset;
            auto name = inst.name();
            if (auto* f = std::get_if<VintrpFields>(&inst.fields)) {
                Attribute attr = attribute(inst, f->attr);
                if (name == "v_interp_mov_f32" && f->src.kind == Operand::Const) {
                    Interp it;
                    it.kind = Interp::Vertex;
                    it.attribute = attr;
                    it.vertex = f->src.a;
                    io.interps.emplace_back(site, it);
                } else if (name == "v_interp_p1_f32") {
                } else if (name == "v_interp_p2_f32" && f->src.kind == Operand::Vector) {
                    if (f->dst.kind != Operand::Vector) {
                        unsupported(at, "an interpolation operand");
                    }
                    Pair pr;
                    if (const char* e = halves.pair(f->attr, inst.read(Reg::v(f->dst.a)), inst.read(Reg::v(f->src.a)), barycentric, pr)) {
                        unsupported(at, e);
                    }
                    push_pair(io, site, pr, attr);
                } else {
                    unsupported(at, "an interpolation form");
                }
            } else if (auto* v = std::get_if<Valu>(&inst.fields); v && name.starts_with("v_interp_")) {
                if (name == "v_interp_p1ll_f16") {
                } else if (name == "v_interp_p2_f16") {
                    Attribute attr = attribute(inst, v->src[0]);
                    auto read = [&](std::size_t k) -> std::optional<Value> {
                        if (k < v->src.size() && v->src[k].kind == Operand::Vector) {
                            return inst.read(Reg::v(v->src[k].a));
                        }
                        return std::nullopt;
                    };
                    Pair pr;
                    if (const char* e = halves.pair(v->src[0], read(2), read(1), barycentric, pr)) {
                        unsupported(at, e);
                    }
                    push_pair(io, site, pr, attr);
                } else {
                    unsupported(at, "an interpolation form");
                }
            } else if (auto* e = std::get_if<ExpFields>(&inst.fields)) {
                Target target;
                std::uint8_t t = e->target;
                if (t <= 7) {
                    target.kind = Target::Color;
                    target.index = t;
                } else if (t == 8) {
                    target.kind = Target::Depth;
                } else if (t == 9) {
                    target.kind = Target::Null;
                } else if (t >= 12 && t <= 16) {
                    target.kind = Target::Position;
                    target.index = static_cast<std::uint8_t>(t - 12);
                } else if (t == 20) {
                    target.kind = Target::Primitive;
                } else if (t == 32 && params->empty()) {
                    target.kind = Target::Unread;
                } else if (t >= 32 && t <= 63) {
                    const VsOutput* o = nullptr;
                    for (const VsOutput& x : *params) {
                        if ((x.param & 0x7F) == t - 32) {
                            o = &x;
                            break;
                        }
                    }
                    if (!o) {
                        unsupported(at, "a parameter export the stage data does not list");
                    }
                    target.kind = (o->param & 0x80) ? Target::PrimitiveParam : Target::Param;
                    target.usage = o->usage;
                    target.row = o->row;
                } else {
                    unsupported(at, "an export target");
                }
                Export x;
                x.site = site;
                x.target = target;
                for (int k = 0; k < 4; ++k) {
                    if (e->src[k] && e->src[k]->kind == Operand::Vector) {
                        x.values[k] = inst.read(Reg::v(e->src[k]->a));
                    }
                }
                x.compressed = e->compr;
                x.done = e->done;
                io.exports.push_back(x);
            }
        }
    }
    return io;
}

}   
