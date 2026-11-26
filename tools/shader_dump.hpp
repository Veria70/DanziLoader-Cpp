// SPDX-License-Identifier: GPL-3.0-or-later
// The canonical texts of the translator's stages, as `oracle-m5` prints
// them (`irdump.rs`, `dump.rs`): compared line for line.
#pragma once

#include <cstdio>
#include <ostream>
#include <string>

#include <algorithm>
#include <set>
#include <vector>

#include "shader/ir.hpp"
#include "shader/structure.hpp"

namespace dump {

using namespace danzi::shader;

inline std::string hexx(std::size_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%zx", v);
    return b;
}

inline std::string op(const Operand& o) {
    char b[48];
    switch (o.kind) {
        case Operand::Scalar: std::snprintf(b, sizeof b, "S%ux%u", o.a, o.b); break;
        case Operand::Vector: std::snprintf(b, sizeof b, "V%ux%u", o.a, o.b); break;
        case Operand::Const: std::snprintf(b, sizeof b, "C%u", o.a); break;
        case Operand::Literal: std::snprintf(b, sizeof b, "L%x", o.literal); break;
        case Operand::Attr: std::snprintf(b, sizeof b, "A%u.%u%s", o.a, o.b, o.high ? "h" : ""); break;
    }
    return b;
}

inline std::string opt(const Opt& o) { return o ? op(*o) : "-"; }

inline std::string ops(const Ops& list) {
    std::string s = "[";
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i) {
            s += ",";
        }
        s += op(list[i]);
    }
    return s + "]";
}

inline int b(bool x) { return x ? 1 : 0; }

inline std::string cache(const Cache& c) { return std::to_string(b(c.glc)) + std::to_string(b(c.slc)) + std::to_string(b(c.dlc)); }


#pragma warning(push)
#pragma warning(disable : 4456)
inline std::string fields(const Fields& f) {
    std::string s;
    if (auto* x = std::get_if<SopFields>(&f)) {
        s = "sop " + opt(x->dst) + " " + ops(x->src) + " " + (x->imm ? std::to_string(*x->imm) : "-");
    } else if (auto* x = std::get_if<SoppFields>(&f)) {
        s = "sopp " + std::to_string(x->imm);
    } else if (auto* x = std::get_if<SmemFields>(&f)) {
        s = "smem " + opt(x->data) + " " + op(x->base) + " " + std::to_string(x->offset) + " " + op(x->soffset) + " " + cache(x->cache);
    } else if (auto* v = std::get_if<Valu>(&f)) {
        s = "valu " + opt(v->dst) + " " + opt(v->carry) + " " + ops(v->src) + " " + std::to_string(v->neg) + " " + std::to_string(v->abs) +
            " " + std::to_string(v->neg_hi) + " " + std::to_string(v->opsel) + " " + std::to_string(v->opsel_hi) + " " +
            std::to_string(b(v->clamp)) + " " + std::to_string(v->omod);
        if (v->sdwa) {
            const Sdwa& w = *v->sdwa;
            s += " sdwa " + std::to_string(w.dst_sel) + " " + std::to_string(w.dst_unused) + " " + std::to_string(w.src_sel[0]) + " " +
                 std::to_string(w.src_sel[1]) + " " + std::to_string(w.sext);
        }
        if (v->dpp) {
            const Dpp& d = *v->dpp;
            if (d.kind == Dpp::Row) {
                s += " dpp " + std::to_string(d.ctrl) + " " + std::to_string(d.row_mask) + " " + std::to_string(d.bank_mask) + " " +
                     std::to_string(b(d.bound_ctrl)) + " " + std::to_string(b(d.fi));
            } else {
                s += " dpp8 " + std::to_string(d.sel) + " " + std::to_string(b(d.fi));
            }
        }
    } else if (auto* x = std::get_if<VintrpFields>(&f)) {
        s = "vintrp " + op(x->dst) + " " + op(x->src) + " " + op(x->attr);
    } else if (auto* x = std::get_if<DsFields>(&f)) {
        s = "ds " + opt(x->dst) + " " + opt(x->addr) + " " + ops(x->data) + " " + std::to_string(x->offset0) + " " + std::to_string(x->offset1) +
            " " + std::to_string(b(x->gds));
    } else if (auto* x = std::get_if<BufferFields>(&f)) {
        s = "buf " + opt(x->data) + " " + op(x->addr) + " " + op(x->rsrc) + " " + op(x->soffset) + " " + std::to_string(x->offset) + " " +
            std::to_string(b(x->offen)) + std::to_string(b(x->idxen)) + " " + (x->format ? std::to_string(*x->format) : "-") + " " +
            std::to_string(b(x->lds)) + std::to_string(b(x->tfe)) + " " + cache(x->cache);
    } else if (auto* x = std::get_if<ImageFields>(&f)) {
        s = "img " + op(x->data) + " " + ops(x->addr) + " " + op(x->rsrc) + " " + opt(x->sampler) + " " + std::to_string(x->dmask) + " " +
            std::to_string(x->dim) + " " + std::to_string(b(x->unorm)) + std::to_string(b(x->r128)) + std::to_string(b(x->a16)) +
            std::to_string(b(x->d16)) + std::to_string(b(x->tfe)) + std::to_string(b(x->lwe)) + " " + cache(x->cache);
    } else if (auto* x = std::get_if<FlatFields>(&f)) {
        s = "flat " + opt(x->dst) + " " + opt(x->addr) + " " + opt(x->data) + " " + opt(x->saddr) + " " + std::to_string(x->offset) + " " +
            std::to_string(b(x->lds)) + " " + cache(x->cache);
    } else if (auto* x = std::get_if<ExpFields>(&f)) {
        s = "exp " + std::to_string(x->target) + " " + opt(x->src[0]) + " " + opt(x->src[1]) + " " + opt(x->src[2]) + " " + opt(x->src[3]) +
            " " + std::to_string(b(x->compr)) + std::to_string(b(x->done)) + std::to_string(b(x->vm));
    }
    return s;
}
#pragma warning(pop)

inline std::string reg(ir::Reg r) {
    switch (r.kind) {
        case ir::Reg::S: return "s" + std::to_string(r.n);
        case ir::Reg::Scc: return "scc";
        default: return "v" + std::to_string(r.n);
    }
}

inline std::string rv(const ir::RegValues& list) {
    std::string s;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i) {
            s += " ";
        }
        s += reg(list[i].first) + "=" + std::to_string(list[i].second.id);
    }
    return s;
}

inline void program(const ir::Program& p, std::string& out) {
    out += " wave64 " + std::to_string(b(p.wave64)) + " vgprs " + std::to_string(p.vgprs) + "\n";
    for (std::size_t i = 0; i < p.blocks.size(); ++i) {
        const ir::Block& block = p.blocks[i];
        std::string preds;
        for (std::size_t k = 0; k < block.preds.size(); ++k) {
            if (k) {
                preds += ",";
            }
            preds += std::to_string(block.preds[k]);
        }
        std::string exit;
        switch (block.exit.kind) {
            case ir::Exit::Jump: exit = "jump " + std::to_string(block.exit.a); break;
            case ir::Exit::Branch: exit = "branch " + std::to_string(block.exit.a) + " " + std::to_string(block.exit.b); break;
            case ir::Exit::End: exit = "end"; break;
            case ir::Exit::Indirect: exit = "indirect"; break;
        }
        out += " block " + std::to_string(i) + " " + hexx(block.offset) + " preds " + preds + " exit " + exit + "\n";
        for (const ir::Phi& phi : block.phis) {
            std::string args;
            for (std::size_t k = 0; k < phi.args.size(); ++k) {
                if (k) {
                    args += ",";
                }
                args += std::to_string(phi.args[k].id);
            }
            out += "  phi " + reg(phi.reg) + "=" + std::to_string(phi.value.id) + " " + args + "\n";
        }
        for (const ir::Inst& inst : block.insts) {
            std::string words;
            char w[16];
            for (auto x : inst.machine.words) {
                std::snprintf(w, sizeof w, "%08x", x);
                words += w;
            }
            out += "  " + hexx(inst.machine.offset) + " " + std::string(inst.name()) + " " + words + " | " + fields(inst.fields) + " | r " +
                   rv(inst.reads) + " | w " + rv(inst.writes) + "\n";
        }
    }
    out += " inputs " + rv(p.inputs) + "\n";
}

inline std::string atom(const structure::Atom& a) {
    char b[48];
    switch (a.kind) {
        case structure::Atom::Lane: std::snprintf(b, sizeof b, "lane%llu", static_cast<unsigned long long>(a.payload)); break;
        case structure::Atom::Uniform: std::snprintf(b, sizeof b, "uni%llu", static_cast<unsigned long long>(a.payload)); break;
        case structure::Atom::Phi: std::snprintf(b, sizeof b, "phi%llu", static_cast<unsigned long long>(a.payload)); break;
        case structure::Atom::Quad: std::snprintf(b, sizeof b, "quad%llu", static_cast<unsigned long long>(a.payload)); break;
        case structure::Atom::Opaque: std::snprintf(b, sizeof b, "opq%llu", static_cast<unsigned long long>(a.payload)); break;
        case structure::Atom::Constant: std::snprintf(b, sizeof b, "const%llx", static_cast<unsigned long long>(a.payload)); break;
    }
    return b;
}

inline void collect(const structure::Bools& bools, structure::Bool root, std::set<std::uint32_t>& seen) {
    std::vector<structure::Bool> stack{root};
    while (!stack.empty()) {
        structure::Bool x = stack.back();
        stack.pop_back();
        if (!seen.insert(x.id).second) {
            continue;
        }
        if (auto n = bools.node(x)) {
            stack.push_back(n->lo);
            stack.push_back(n->hi);
        }
    }
}

inline std::string list(const std::vector<std::uint64_t>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) {
            s += ", ";
        }
        s += std::to_string(v[i]);
    }
    return s + "]";
}

inline void structured(const structure::Structured& s, std::string& out) {
    std::set<std::uint32_t> named;
    for (std::size_t i = 0; i < s.exits.size(); ++i) {
        const auto& e = s.exits[i];
        switch (e.kind) {
            case structure::LaneExit::Jump: out += " exit " + std::to_string(i) + " jump " + std::to_string(e.taken) + "\n"; break;
            case structure::LaneExit::Branch:
                out += " exit " + std::to_string(i) + " branch " + std::to_string(e.when.id) + " " + std::to_string(e.taken) + " " +
                       std::to_string(e.not_taken) + "\n";
                collect(s.bools, e.when, named);
                break;
            case structure::LaneExit::End: out += " exit " + std::to_string(i) + " end\n"; break;
            case structure::LaneExit::Indirect: out += " exit " + std::to_string(i) + " indirect\n"; break;
        }
    }
    for (std::size_t b = 0; b < s.guards.size(); ++b) {
        for (std::size_t i = 0; i < s.guards[b].size(); ++i) {
            if (s.guards[b][i]) {
                out += " guard " + std::to_string(b) + " " + std::to_string(i) + " " + std::to_string(s.guards[b][i]->id) + "\n";
                collect(s.bools, *s.guards[b][i], named);
            }
        }
    }
    for (std::size_t b = 0; b < s.reach.size(); ++b) {
        out += " reach " + std::to_string(b) + " " + std::to_string(s.reach[b].id) + "\n";
        collect(s.bools, s.reach[b], named);
    }
    std::vector<std::pair<std::uint32_t, structure::Bool>> masks;
    for (const auto& [v, m] : s.masks) {
        masks.emplace_back(v.id, m);
    }
    std::sort(masks.begin(), masks.end());
    for (const auto& [v, m] : masks) {
        out += " mask " + std::to_string(v) + " " + std::to_string(m.id) + "\n";
        collect(s.bools, m, named);
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> highs;
    for (const auto& [h, l] : s.highs) {
        highs.emplace_back(h.id, l.id);
    }
    std::sort(highs.begin(), highs.end());
    for (const auto& [h, l] : highs) {
        out += " high " + std::to_string(h) + " " + std::to_string(l) + "\n";
    }
    std::vector<std::uint64_t> active;
    for (const auto& v : s.active) {
        active.push_back(v.id);
    }
    std::sort(active.begin(), active.end());
    out += " active " + list(active) + "\n";
    std::vector<std::pair<std::uint32_t, std::vector<structure::Bool>>> phis;
    for (const auto& [v, m] : s.phi_masks) {
        phis.emplace_back(v.id, m);
    }
    std::sort(phis.begin(), phis.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [v, m] : phis) {
        std::string ids;
        for (std::size_t i = 0; i < m.size(); ++i) {
            if (i) {
                ids += ",";
            }
            ids += std::to_string(m[i].id);
        }
        out += " phimask " + std::to_string(v) + " " + ids + "\n";
        for (auto b : m) {
            collect(s.bools, b, named);
        }
    }
    std::vector<std::uint64_t> wide(s.wide.begin(), s.wide.end());
    std::sort(wide.begin(), wide.end());
    out += " wide " + list(wide) + "\n";
    for (std::size_t b = 0; b < s.wave_any.size(); ++b) {
        if (s.wave_any[b]) {
            out += " wave_any " + std::to_string(b) + " " + std::to_string(*s.wave_any[b] ? 1 : 0) + "\n";
        }
    }
    std::vector<std::pair<std::uint32_t, structure::Bool>> scc;
    for (const auto& [v, m] : s.scc_some) {
        scc.emplace_back(v.id, m);
    }
    std::sort(scc.begin(), scc.end());
    for (const auto& [v, m] : scc) {
        out += " scc_some " + std::to_string(v) + " " + std::to_string(m.id) + "\n";
        collect(s.bools, m, named);
    }
    for (std::uint32_t n : named) {
        if (auto node = s.bools.node(structure::Bool{n})) {
            out += " node " + std::to_string(n) + " " + atom(node->atom) + " " + std::to_string(node->lo.id) + " " + std::to_string(node->hi.id) + "\n";
        } else {
            out += " node " + std::to_string(n) + " const\n";
        }
    }
}

}   
