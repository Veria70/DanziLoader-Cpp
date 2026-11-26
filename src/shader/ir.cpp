// SPDX-License-Identifier: GPL-3.0-or-later
// One-to-one SSA form of a native program (`ir.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See ir.hpp.
#include "ir.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "error.hpp"

namespace danzi::shader::ir {

namespace {


constexpr Value NONE{UINT32_MAX};

constexpr std::size_t REGS = 256 + 1 + 256;
constexpr std::size_t WORDS = (REGS + 63) / 64;
using RegSet = std::array<std::uint64_t, WORDS>;

std::size_t index(Reg reg) {
    switch (reg.kind) {
        case Reg::S: return reg.n;
        case Reg::Scc: return 256;
        default: return 257 + std::size_t{reg.n};
    }
}

Reg reg_at(std::size_t i) {
    if (i < 256) {
        return Reg::s(static_cast<std::uint8_t>(i));
    }
    if (i == 256) {
        return Reg::scc();
    }
    return Reg::v(static_cast<std::uint8_t>(i - 257));
}

void insert(RegSet& set, Reg reg) {
    std::size_t i = index(reg);
    set[i / 64] |= std::uint64_t{1} << (i % 64);
}

bool contains(const RegSet& set, Reg reg) {
    std::size_t i = index(reg);
    return (set[i / 64] >> (i % 64) & 1) != 0;
}


template <class F>
void for_members(const RegSet& set, F f) {
    for (std::size_t w = 0; w < WORDS; ++w) {
        std::uint64_t bits = set[w];
        while (bits) {
            int b = std::countr_zero(bits);
            bits &= bits - 1;
            f(reg_at(w * 64 + static_cast<std::size_t>(b)));
        }
    }
}


Value lookup(const RegValues& outs, Reg reg) {
    auto it = std::lower_bound(outs.begin(), outs.end(), reg, [](const auto& e, Reg r) { return e.first < r; });
    return it != outs.end() && it->first == reg ? it->second : NONE;
}


void link(std::vector<Block>& blocks) {
    for (auto& b : blocks) {
        b.preds.clear();
    }
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        std::size_t s[2];
        std::size_t n = blocks[b].exit.successors(s);
        for (std::size_t k = 0; k < n; ++k) {
            blocks[s[k]].preds.push_back(b);
        }
    }
}


std::pair<std::vector<RegSet>, std::vector<RegSet>> liveness(const std::vector<Block>& blocks, const std::vector<std::size_t>& order) {
    RegSet empty{};
    std::vector<RegSet> uses(blocks.size(), empty), defs(blocks.size(), empty);
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        for (const Inst& inst : blocks[b].insts) {
            for (const auto& [reg, v] : inst.reads) {
                if (!contains(defs[b], reg)) {
                    insert(uses[b], reg);
                }
            }
            for (const auto& [reg, v] : inst.writes) {
                insert(defs[b], reg);
            }
        }
    }
    std::vector<RegSet> live_in(blocks.size(), empty), live_out(blocks.size(), empty);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            std::size_t b = *it;
            RegSet out = empty;
            std::size_t s[2];
            std::size_t n = blocks[b].exit.successors(s);
            for (std::size_t k = 0; k < n; ++k) {
                for (std::size_t w = 0; w < WORDS; ++w) {
                    out[w] |= live_in[s[k]][w];
                }
            }
            RegSet inn;
            for (std::size_t w = 0; w < WORDS; ++w) {
                inn[w] = uses[b][w] | (out[w] & ~defs[b][w]);
            }
            if (inn != live_in[b] || out != live_out[b]) {
                live_in[b] = inn;
                live_out[b] = out;
                changed = true;
            }
        }
    }
    return {std::move(live_in), std::move(live_out)};
}


void remove_trivial_phis(std::vector<Block>& blocks, std::uint32_t values) {
    std::vector<Value> replaced(values);
    for (std::uint32_t i = 0; i < values; ++i) {
        replaced[i] = Value{i};
    }
    auto find = [&](Value v) {
        while (replaced[v.id] != v) {
            v = replaced[v.id];
        }
        return v;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (Block& block : blocks) {
            std::vector<Phi> kept;
            kept.reserve(block.phis.size());
            for (Phi& phi : block.phis) {
                std::optional<Value> only;
                bool keep = false;
                for (Value a0 : phi.args) {
                    Value a = find(a0);
                    if (a == phi.value || (only && *only == a)) {
                        continue;
                    }
                    if (only) {
                        keep = true;
                        break;
                    }
                    only = a;
                }
                if (keep || !only) {
                    kept.push_back(std::move(phi));
                    continue;
                }
                replaced[phi.value.id] = *only;
                changed = true;
            }
            block.phis = std::move(kept);
        }
    }
    for (Block& block : blocks) {
        for (Phi& phi : block.phis) {
            for (Value& a : phi.args) {
                a = find(a);
            }
        }
        for (Inst& inst : block.insts) {
            for (auto& [r, v] : inst.reads) {
                v = find(v);
            }
        }
    }
}


Program rename(std::vector<Block> blocks, bool wave64, std::uint16_t vgprs) {
    auto order = reverse_postorder(blocks);
    auto [live_in, live_out] = liveness(blocks, order);
    std::uint32_t values = 0;
    auto new_value = [&] { return Value{values++}; };
    RegValues inputs;
    std::vector<RegValues> outs(blocks.size());
    std::vector<Value> current(REGS, NONE);
    for (std::size_t b : order) {
        std::fill(current.begin(), current.end(), NONE);
        Block& block = blocks[b];
        for_members(live_in[b], [&](Reg reg) {
            Value v;
            if (block.preds.empty()) {
                v = new_value();
                inputs.emplace_back(reg, v);
            } else if (block.preds.size() == 1) {

                v = lookup(outs[block.preds[0]], reg);
            } else {
                v = new_value();
                block.phis.push_back(Phi{reg, v, {}});
            }
            current[index(reg)] = v;
        });
        for (Inst& inst : block.insts) {
            for (auto& [reg, v] : inst.reads) {
                v = current[index(reg)];
            }
            for (auto& [reg, v] : inst.writes) {
                v = new_value();
                current[index(reg)] = v;
            }
        }
        RegValues out;
        std::size_t live = 0;
        for (std::uint64_t w : live_out[b]) {
            live += static_cast<std::size_t>(std::popcount(w));
        }
        out.reserve(live);
        for_members(live_out[b], [&](Reg reg) { out.emplace_back(reg, current[index(reg)]); });
        outs[b] = std::move(out);
    }
    for (Block& block : blocks) {
        for (Phi& phi : block.phis) {
            phi.args.clear();
            phi.args.reserve(block.preds.size());
            for (std::size_t p : block.preds) {
                phi.args.push_back(lookup(outs[p], phi.reg));
            }
        }
    }
    remove_trivial_phis(blocks, values);
    Program prog;
    prog.wave64 = wave64;
    prog.vgprs = vgprs;
    prog.blocks = std::move(blocks);
    prog.inputs = std::move(inputs);
    return prog;
}


std::optional<std::pair<std::vector<bool>, std::vector<std::size_t>>> multi_entry_loop(const std::vector<Block>& blocks,
                                                                                       const std::vector<bool>& inside) {
    std::size_t n = blocks.size();
    auto reach = [&](std::size_t from, bool forward) {
        std::vector<bool> seen(n, false);
        std::vector<std::size_t> stack{from};
        while (!stack.empty()) {
            std::size_t b = stack.back();
            stack.pop_back();
            std::vector<std::size_t> next = forward ? blocks[b].exit.successor_list() : blocks[b].preds;
            for (std::size_t x : next) {
                if (inside[x] && !seen[x]) {
                    seen[x] = true;
                    stack.push_back(x);
                }
            }
        }
        return seen;
    };
    std::vector<bool> done(n, false);
    for (std::size_t v = 0; v < n; ++v) {
        if (!inside[v] || done[v]) {
            continue;
        }
        auto ahead = reach(v, true), behind = reach(v, false);
        std::vector<bool> region(n);
        for (std::size_t i = 0; i < n; ++i) {
            region[i] = ahead[i] && behind[i];
        }
        if (!region[v]) {
            continue;
        }
        for (std::size_t b = 0; b < n; ++b) {
            if (region[b]) {
                done[b] = true;
            }
        }
        std::vector<std::size_t> entries;
        for (std::size_t b = 0; b < n; ++b) {
            if (region[b] && std::any_of(blocks[b].preds.begin(), blocks[b].preds.end(), [&](std::size_t p) { return !region[p]; })) {
                entries.push_back(b);
            }
        }
        if (entries.empty()) {
            continue;
        }
        if (entries.size() == 1) {
            std::vector<bool> nested = region;
            nested[entries[0]] = false;
            if (auto found = multi_entry_loop(blocks, nested)) {
                return found;
            }
            continue;
        }
        return std::pair{std::move(region), std::move(entries)};
    }
    return std::nullopt;
}

void retarget(Exit& exit, const std::function<std::size_t(std::size_t)>& to);


void split_irreducible(std::vector<Block>& blocks) {
    std::size_t limit = 4 * blocks.size() + 64;
    for (;;) {
        auto order = reverse_postorder(blocks);
        std::vector<std::size_t> idx(blocks.size(), SIZE_MAX);
        for (std::size_t i = 0; i < order.size(); ++i) {
            idx[order[i]] = i;
        }
        auto edge = irreducible_edge(blocks, order, idx);
        if (!edge) {
            return;
        }
        std::size_t from = edge->first;
        auto found = multi_entry_loop(blocks, std::vector<bool>(blocks.size(), true));
        if (!found || blocks.size() >= limit) {
            unsupported(blocks[from].offset, "irreducible control flow");
        }
        auto& [region, entries] = *found;
        std::sort(entries.begin(), entries.end(), [&](std::size_t a, std::size_t b) { return idx[a] < idx[b]; });
        std::size_t header = entries[0], entry = entries[1];
        std::vector<std::size_t> copy(blocks.size(), SIZE_MAX);
        std::vector<std::size_t> copied;
        std::vector<std::size_t> stack{entry};
        while (!stack.empty()) {
            std::size_t b = stack.back();
            stack.pop_back();
            if (copy[b] != SIZE_MAX) {
                continue;
            }
            copy[b] = blocks.size() + copied.size();
            copied.push_back(b);
            std::size_t s[2];
            std::size_t n = blocks[b].exit.successors(s);
            for (std::size_t k = 0; k < n; ++k) {
                if (region[s[k]] && s[k] != header) {
                    stack.push_back(s[k]);
                }
            }
        }
        for (std::size_t b : copied) {
            Block block = blocks[b];
            retarget(block.exit, [&](std::size_t s) { return copy[s] == SIZE_MAX ? s : copy[s]; });
            blocks.push_back(std::move(block));
        }
        std::vector<std::size_t> outside;
        for (std::size_t p : blocks[entry].preds) {
            if (!region[p]) {
                outside.push_back(p);
            }
        }
        for (std::size_t p : outside) {
            retarget(blocks[p].exit, [&](std::size_t s) { return s == entry ? copy[entry] : s; });
        }
        link(blocks);
    }
}

void retarget(Exit& exit, const std::function<std::size_t(std::size_t)>& to) {
    if (exit.kind == Exit::Jump) {
        exit.a = to(exit.a);
    } else if (exit.kind == Exit::Branch) {
        exit.a = to(exit.a);
        exit.b = to(exit.b);
    }
}


constexpr std::string_view DROPPED[] = {"s_nop",
                                        "s_waitcnt",
                                        "s_waitcnt_vscnt",
                                        "s_waitcnt_vmcnt",
                                        "s_waitcnt_expcnt",
                                        "s_waitcnt_lgkmcnt",
                                        "s_waitcnt_depctr",
                                        "s_wait_idle",
                                        "s_inst_prefetch",
                                        "s_clause",
                                        "s_setprio",
                                        "s_sleep",
                                        "s_ttracedata",
                                        "s_ttracedata_imm",
                                        "s_incperflevel",
                                        "s_decperflevel",
                                        "v_nop",
                                        "v_pipeflush",
                                        "v_clrexcp"};

bool has(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }


bool add_regs(const Operand& op, std::uint8_t lanes, std::vector<Reg>& out) {
    switch (op.kind) {
        case Operand::Scalar: {
            if (op.a == NULL_CODE) {
                return true;
            }
            std::uint16_t end = std::uint16_t{op.a} + op.b;
            std::uint16_t limit = op.a < PARKED ? 128 : 256;
            if (end > limit) {
                return false;
            }
            for (std::uint16_t c = op.a; c < end; ++c) {
                out.push_back(Reg::s(static_cast<std::uint8_t>(c)));
            }
            return true;
        }
        case Operand::Vector:
            if (std::uint16_t{op.a} + op.b > 256) {
                return false;
            }
            for (std::uint8_t i = 0; i < op.b; ++i) {
                out.push_back(Reg::v(static_cast<std::uint8_t>(op.a + i)));
            }
            return true;
        case Operand::Const:
            if (op.a == 251) {
                for (std::uint8_t i = 0; i < lanes; ++i) {
                    out.push_back(Reg::s(static_cast<std::uint8_t>(VCC + i)));
                }
            } else if (op.a == 252) {
                for (std::uint8_t i = 0; i < lanes; ++i) {
                    out.push_back(Reg::s(static_cast<std::uint8_t>(EXEC + i)));
                }
            } else if (op.a == 253) {
                out.push_back(Reg::scc());
            } else if (op.a == 254) {
                out.push_back(Reg::s(M0));
            }
            return true;
        default: return true;
    }
}


Operand atomic_result(const Operand& data, std::string_view name) {
    if (data.kind == Operand::Vector && has(name, "cmpswap")) {
        return Operand::vector(data.a, static_cast<std::uint8_t>(data.b / 2));
    }
    return data;
}


bool writes_scc(Encoding encoding, std::string_view name) {
    if (encoding == Encoding::Sopc) {
        return true;
    }
    if (encoding == Encoding::Sopk) {
        return name.starts_with("s_cmpk_") || name == "s_addk_i32";
    }

    std::string_view stem;
    auto a = name.find('_');
    if (a != std::string_view::npos) {
        auto rest = name.substr(a + 1);
        auto b = rest.find('_');
        stem = b == std::string_view::npos ? rest : rest.substr(0, b);
    }
    static constexpr std::string_view STEMS[] = {"add",   "sub",  "addc",  "subb",  "min",   "max",   "and",   "or",   "xor",  "andn2",
                                                 "orn2",  "nand", "nor",   "xnor",  "lshl",  "lshr",  "ashr",  "bfe",  "absdiff", "lshl1",
                                                 "lshl2", "lshl3", "lshl4", "not",   "wqm",   "bcnt0", "bcnt1", "quadmask", "abs"};
    if (has(name, "saveexec") || has(name, "wrexec")) {
        return true;
    }
    for (auto s : STEMS) {
        if (s == stem) {
            return true;
        }
    }
    return false;
}

using Effects = std::pair<std::vector<Reg>, std::vector<Reg>>;


#pragma warning(push)
#pragma warning(disable : 4456)

bool effects_fill(const Instruction& inst, const Fields& fields, std::string_view name, std::uint8_t lanes, std::uint16_t vgprs, Effects& out) {
    for (auto d : DROPPED) {
        if (d == name) {
            return false;
        }
    }
    auto unsupported_here = [&]() -> void {

        throw ShaderError(ShaderError::Unsupported, name.data(), inst.offset);
    };
    auto mask = [&](std::uint8_t code, std::vector<Reg>& out) {
        for (std::uint8_t i = 0; i < lanes; ++i) {
            out.push_back(Reg::s(static_cast<std::uint8_t>(code + i)));
        }
    };

    thread_local std::vector<Reg> r;
    r.clear();
    std::vector<Reg>& w = out.second;
    w.clear();
    auto regs = [&](const Operand& op, std::vector<Reg>& out) {
        if (!add_regs(op, lanes, out)) {
            isa(inst.offset, "register range out of bounds");
        }
    };
    bool vector = !(std::holds_alternative<SopFields>(fields) || std::holds_alternative<SoppFields>(fields) ||
                    std::holds_alternative<SmemFields>(fields));
    if (auto* f = std::get_if<SopFields>(&fields)) {
        if (name.starts_with("s_movrel") || name.starts_with("s_subvector_loop") || name == "s_call_b64" || name == "s_rfe_b64") {
            unsupported_here();
        }
        for (const Operand& s : f->src) {
            regs(s, r);
        }
        if (f->dst) {
            regs(*f->dst, w);
            if (name.starts_with("s_bitset") || name.starts_with("s_cmov") || name == "s_addk_i32" || name == "s_mulk_i32") {
                regs(*f->dst, r);
            }
        }
        if (name == "s_addc_u32" || name == "s_subb_u32" || name == "s_cmovk_i32" || name.starts_with("s_cselect_") || name.starts_with("s_cmov_")) {
            r.push_back(Reg::scc());
        }
        if (has(name, "saveexec") || has(name, "wrexec")) {
            mask(EXEC, r);
            mask(EXEC, w);
        }
        if (writes_scc(inst.encoding, name)) {
            w.push_back(Reg::scc());
        }
    } else if (std::holds_alternative<SoppFields>(fields)) {
        if (name == "s_cbranch_scc0" || name == "s_cbranch_scc1") {
            r.push_back(Reg::scc());
        } else if (name == "s_cbranch_vccz" || name == "s_cbranch_vccnz") {
            mask(VCC, r);
        } else if (name == "s_cbranch_execz" || name == "s_cbranch_execnz") {
            mask(EXEC, r);
        } else if (name == "s_sendmsg" || name == "s_sendmsghalt") {
            r.push_back(Reg::s(M0));
        } else if (name == "s_endpgm" || name == "s_endpgm_saved" || name == "s_endpgm_ordered_ps_done" || name == "s_code_end" ||
                   name == "s_branch" || name == "s_barrier" || name == "s_icache_inv" || name == "s_wakeup" || name == "s_round_mode" ||
                   name == "s_denorm_mode") {
        } else {
            unsupported_here();
        }
    } else if (auto* f = std::get_if<SmemFields>(&fields)) {
        if (name == "s_memtime" || name == "s_memrealtime" || name == "s_get_waveid_in_workgroup") {
            if (!f->data) {
                unsupported_here();
            }
            regs(*f->data, w);
        } else if (name == "s_gl1_inv" || name == "s_dcache_inv" || name == "s_dcache_wb") {
        } else if (has(name, "_load_") || has(name, "_store_")) {
            regs(f->base, r);
            regs(f->soffset, r);
            if (!f->data) {
                unsupported_here();
            }
            regs(*f->data, has(name, "_load_") ? w : r);
        } else {
            unsupported_here();
        }
    } else if (auto* v = std::get_if<Valu>(&fields)) {
        bool src_indexed = false, dst_indexed = false;
        if (name == "v_movrels_b32") {
            src_indexed = true;
        } else if (name == "v_movreld_b32") {
            dst_indexed = true;
        } else if (name == "v_movrelsd_b32" || name == "v_movrelsd_2_b32" || name == "v_swaprel_b32") {
            src_indexed = dst_indexed = true;
        }

        auto indexed = [&](const Operand& op, std::vector<Reg>& out) {
            if (op.kind == Operand::Vector && std::uint16_t{op.a} < vgprs) {
                for (std::uint16_t x = op.a; x < vgprs; ++x) {
                    out.push_back(Reg::v(static_cast<std::uint8_t>(x)));
                }
                return;
            }
            isa(inst.offset, "relative VGPR base outside the program's VGPRs");
        };
        for (std::size_t i = 0; i < v->src.size(); ++i) {
            if (i == 0 && src_indexed) {
                indexed(v->src[i], r);
            } else {
                regs(v->src[i], r);
            }
        }

        if (name.starts_with("v_cmpx")) {
            mask(EXEC, w);
        } else if (v->dst) {
            if (dst_indexed) {
                indexed(*v->dst, w);
            } else {
                regs(*v->dst, w);
            }
        }
        if (v->carry) {
            regs(*v->carry, w);
        }
        if (name == "v_swap_b32") {
            regs(v->src[0], w);
            if (v->dst) {
                regs(*v->dst, r);
            }
        } else if (name == "v_swaprel_b32") {
            indexed(v->src[0], w);
            if (v->dst) {
                indexed(*v->dst, r);
            }
        } else if (name == "v_div_fmas_f32" || name == "v_div_fmas_f64") {
            mask(VCC, r);
        }
        if (name.starts_with("v_interp_") || src_indexed || dst_indexed) {
            r.push_back(Reg::s(M0));
        }
    } else if (auto* f = std::get_if<VintrpFields>(&fields)) {
        regs(f->src, r);
        r.push_back(Reg::s(M0));
        regs(f->dst, w);
    } else if (auto* f = std::get_if<DsFields>(&fields)) {
        if (f->addr) {
            regs(*f->addr, r);
        }
        for (const Operand& o : f->data) {
            regs(o, r);
        }
        if (f->dst) {
            regs(*f->dst, w);
        }
        if (f->gds || name.starts_with("ds_gws") || has(name, "addtid") || name == "ds_ordered_count" || name == "ds_append" ||
            name == "ds_consume") {
            r.push_back(Reg::s(M0));
        }
    } else if (auto* f = std::get_if<BufferFields>(&fields)) {
        if (f->offen || f->idxen) {
            regs(f->addr, r);
        }
        regs(f->rsrc, r);
        regs(f->soffset, r);
        if (f->lds) {
            r.push_back(Reg::s(M0));
        }
        if (f->data) {
            if (has(name, "_load_")) {
                if (!f->lds) {
                    regs(*f->data, w);
                }
            } else if (has(name, "_store_")) {
                regs(*f->data, r);
            } else if (has(name, "_atomic_")) {
                regs(*f->data, r);
                if (f->cache.glc) {
                    regs(atomic_result(*f->data, name), w);
                }
            } else {
                unsupported_here();
            }
        }
    } else if (auto* f = std::get_if<ImageFields>(&fields)) {
        for (const Operand& a : f->addr) {
            regs(a, r);
        }
        regs(f->rsrc, r);
        if (f->sampler) {
            regs(*f->sampler, r);
        }
        if (has(name, "_store")) {
            regs(f->data, r);
        } else if (has(name, "_atomic_")) {
            regs(f->data, r);
            if (f->cache.glc) {
                regs(atomic_result(f->data, name), w);
            }
        } else {
            regs(f->data, w);
        }
    } else if (auto* f = std::get_if<FlatFields>(&fields)) {
        for (const Opt* o : {&f->addr, &f->data, &f->saddr}) {
            if (*o) {
                regs(**o, r);
            }
        }
        if (f->dst) {
            regs(*f->dst, w);
        }
    } else if (auto* f = std::get_if<ExpFields>(&fields)) {
        for (const Opt& s : f->src) {
            if (s) {
                regs(*s, r);
            }
        }
    }
    if (vector) {
        mask(EXEC, r);
    }

    std::vector<Reg>& unique = out.first;
    unique.clear();
    for (Reg reg : r) {
        if (std::find(unique.begin(), unique.end(), reg) == unique.end()) {
            unique.push_back(reg);
        }
    }
    return true;
}

#pragma warning(pop)

std::optional<Effects> effects(const Instruction& inst, const Fields& fields, std::string_view name, std::uint8_t lanes, std::uint16_t vgprs) {
    Effects e;
    if (!effects_fill(inst, fields, name, lanes, vgprs, e)) {
        return std::nullopt;
    }
    return e;
}


struct Park {
    bool save;
    std::uint8_t reg, code;
    std::uint32_t lane;
    bool operator==(const Park&) const = default;
};

std::optional<Park> parked(const Instruction& inst, const Fields& fields) {
    auto* v = std::get_if<Valu>(&fields);
    if (!v) {
        return std::nullopt;
    }
    if (v->abs != 0 || v->neg != 0 || v->opsel != 0 || v->clamp || v->omod != 0 || v->sdwa || v->dpp) {
        return std::nullopt;
    }
    auto lane = [](const Operand& op) -> std::optional<std::uint32_t> {
        if (op.kind == Operand::Const && op.a >= 128 && op.a <= 191) {
            return std::uint32_t{op.a} - 128;
        }
        return std::nullopt;
    };
    auto name = inst.name();
    if (name.empty() || !v->dst || v->src.size() != 2) {
        return std::nullopt;
    }
    const Operand& d = *v->dst;
    const Operand& s0 = v->src[0];
    if (name == "v_writelane_b32" && d.kind == Operand::Vector && s0.kind == Operand::Scalar && s0.a < 105) {
        auto k = lane(v->src[1]);
        if (!k) {
            return std::nullopt;
        }
        return Park{true, d.a, s0.a, *k};
    }
    if (name == "v_readlane_b32" && d.kind == Operand::Scalar && s0.kind == Operand::Vector && d.a < 105) {
        auto k = lane(v->src[1]);
        if (!k) {
            return std::nullopt;
        }
        return Park{false, s0.a, d.a, *k};
    }
    return std::nullopt;
}


std::vector<std::array<std::size_t, 3>> unspill(std::vector<Instruction>& list, const std::vector<std::size_t>& leaders, bool wave64,
                                                std::uint8_t lanes, std::uint16_t vgprs) {
    bool stores[256] = {}, other[256] = {}, used[128] = {};
    std::vector<std::optional<Park>> parks;
    parks.reserve(list.size());
    std::vector<std::vector<std::uint8_t>> written, read;
    written.reserve(list.size());
    read.reserve(list.size());
    for (const Instruction& inst : list) {
        auto name = inst.name();
        if (name.empty()) {
            return {};
        }
        Fields f = fields(inst, wave64);
        auto park = parked(inst, f);
        thread_local Effects scratch;
        Effects* e = nullptr;
        try {
            if (effects_fill(inst, f, name, lanes, vgprs, scratch)) {
                e = &scratch;
            }
        } catch (const ShaderError&) {
            return {};
        }
        std::vector<std::uint8_t> codes, from;
        if (e) {
            std::size_t ns = 0, nr = 0;
            for (Reg reg : e->second) {
                ns += reg.kind == Reg::S ? 1 : 0;
            }
            for (Reg reg : e->first) {
                nr += reg.kind == Reg::S ? 1 : 0;
            }
            codes.reserve(ns);
            from.reserve(nr);
            auto& [reads, writes] = *e;
            for (Reg reg : writes) {
                if (reg.kind == Reg::S) {
                    codes.push_back(reg.n);
                }
            }
            for (Reg reg : reads) {
                if (reg.kind == Reg::S) {
                    from.push_back(reg.n);
                }
            }
            bool relative = name == "v_movrels_b32";
            std::size_t n_reads = reads.size();
            std::size_t k = 0;
            auto note = [&](Reg reg) {
                if (relative && k < n_reads && reg.kind == Reg::V) {
                    ++k;
                    return;
                }
                if (reg.kind == Reg::S) {
                    used[reg.n] = true;
                } else if (reg.kind == Reg::V) {
                    if (park && park->reg == reg.n) {
                        stores[reg.n] = true;
                    } else {
                        other[reg.n] = true;
                    }
                }
                ++k;
            };
            for (Reg reg : reads) {
                note(reg);
            }
            for (Reg reg : writes) {
                note(reg);
            }
        }
        parks.push_back(park);
        written.push_back(std::move(codes));
        read.push_back(std::move(from));
    }
    for (auto& park : parks) {
        if (park && other[park->reg]) {
            park.reset();
        }
    }
    if (std::all_of(parks.begin(), parks.end(), [](const auto& p) { return !p; })) {
        return {};
    }
    auto leader = [&](std::size_t i) { return std::binary_search(leaders.begin(), leaders.end(), list[i].offset); };
    auto has_code = [](const std::vector<std::uint8_t>& v, std::uint32_t c) { return std::find(v.begin(), v.end(), c) != v.end(); };
    std::vector<std::pair<std::uint8_t, std::uint32_t>> slots;
    for (const auto& p : parks) {
        if (p) {
            slots.emplace_back(p->reg, p->lane);
        }
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());

    auto partner = [&](std::size_t i) -> std::optional<std::size_t> {
        if (!parks[i]) {
            return std::nullopt;
        }
        auto [save, reg, code, lane] = *parks[i];
        std::size_t j = i + 1;
        if (save) {
            while (j < list.size() && !leader(j)) {
                if (parks[j] == std::optional(Park{true, reg, static_cast<std::uint8_t>(code + 1), lane + 1})) {
                    return j;
                }
                if ((parks[j] && parks[j]->reg == reg && (parks[j]->lane == lane || parks[j]->lane == lane + 1)) || has_code(written[j], code) ||
                    has_code(written[j], std::uint32_t{code} + 1)) {
                    return std::nullopt;
                }
                ++j;
            }
            return std::nullopt;
        }
        while (j < list.size() && !leader(j)) {
            if (parks[j] == std::optional(Park{false, reg, static_cast<std::uint8_t>(code + 1), lane + 1})) {
                return j;
            }
            if ((parks[j] && parks[j]->save && parks[j]->reg == reg && (parks[j]->lane == lane || parks[j]->lane == lane + 1)) ||
                has_code(written[j], code) || has_code(written[j], std::uint32_t{code} + 1) || has_code(read[j], std::uint32_t{code} + 1)) {
                return std::nullopt;
            }
            ++j;
        }
        return std::nullopt;
    };
    std::vector<std::pair<std::uint8_t, std::uint32_t>> pairs;
    auto in_slots = [&](std::uint8_t reg, std::uint32_t lane) {
        return std::find(slots.begin(), slots.end(), std::pair{reg, lane}) != slots.end();
    };
    for (const auto& [reg, lane] : slots) {
        bool after_pair = std::any_of(pairs.begin(), pairs.end(), [&](const auto& p) { return p.first == reg && p.second + 1 == lane; });
        if (!wave64 || after_pair || !in_slots(reg, lane + 1)) {
            continue;
        }
        std::vector<std::size_t> lower;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (parks[i] && parks[i]->reg == reg && parks[i]->lane == lane) {
                lower.push_back(i);
            }
        }
        std::size_t upper = 0;
        for (const auto& p : parks) {
            if (p && p->reg == reg && p->lane == lane + 1) {
                ++upper;
            }
        }
        std::vector<std::optional<std::size_t>> partners;
        for (std::size_t i : lower) {
            partners.push_back(partner(i));
        }
        std::vector<std::size_t> distinct;
        bool all = true;
        for (const auto& p : partners) {
            if (p) {
                distinct.push_back(*p);
            } else {
                all = false;
            }
        }
        std::sort(distinct.begin(), distinct.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        if (all && distinct.size() == lower.size() && upper == lower.size()) {
            pairs.emplace_back(reg, lane);
        }
    }

    constexpr std::size_t past = PARKED;
    bool free_code[256] = {};
    for (std::size_t c = 0; c < 105; ++c) {
        free_code[c] = !used[c];
    }
    for (std::size_t c = 108; c <= 123; ++c) {
        free_code[c] = !used[c];
    }
    for (std::size_t c = past; c < 255; ++c) {
        free_code[c] = true;
    }
    std::vector<std::pair<std::pair<std::uint8_t, std::uint32_t>, std::uint8_t>> codes;
    for (const auto& [reg, lane] : pairs) {
        std::optional<std::size_t> at;

        for (std::size_t c = 108; c < 123 && !at; ++c) {
            if (free_code[c] && free_code[c + 1]) {
                at = c;
            }
        }
        for (std::size_t c = 104; c-- > 0 && !at;) {
            if (free_code[c] && free_code[c + 1]) {
                at = c;
            }
        }
        for (std::size_t c = past; c < 254 && !at; ++c) {
            if (free_code[c] && free_code[c + 1]) {
                at = c;
            }
        }
        if (!at) {
            return {};
        }
        free_code[*at] = free_code[*at + 1] = false;
        codes.push_back({{reg, lane}, static_cast<std::uint8_t>(*at)});
        codes.push_back({{reg, lane + 1}, static_cast<std::uint8_t>(*at + 1)});
    }
    for (const auto& slot : slots) {
        if (std::any_of(codes.begin(), codes.end(), [&](const auto& c) { return c.first == slot; })) {
            continue;
        }
        std::optional<std::size_t> at;
        for (std::size_t c = 108; c <= 123 && !at; ++c) {
            if (free_code[c]) {
                at = c;
            }
        }
        for (std::size_t c = 105; c-- > 0 && !at;) {
            if (free_code[c]) {
                at = c;
            }
        }
        for (std::size_t c = past; c < 255 && !at; ++c) {
            if (free_code[c]) {
                at = c;
            }
        }
        if (!at) {
            return {};
        }
        free_code[*at] = false;
        codes.push_back({slot, static_cast<std::uint8_t>(*at)});
    }
    auto code_of = [&](std::uint8_t reg, std::uint32_t lane) -> std::optional<std::uint8_t> {
        for (const auto& c : codes) {
            if (c.first == std::pair{reg, lane}) {
                return c.second;
            }
        }
        return std::nullopt;
    };
    auto put = [&](std::size_t i, Encoding encoding, std::uint16_t opcode, std::uint32_t word) {
        Instruction x;
        x.offset = list[i].offset;
        x.encoding = encoding;
        x.opcode = opcode;
        x.words.push_back(word);
        list[i] = x;
    };
    auto field = [](std::uint8_t code) -> std::uint32_t { return code < PARKED ? code : 0; };
    auto mov = [&](bool wide, std::uint8_t dst, std::uint8_t src) {
        return 0xBE800000u | field(dst) << 16 | (3 + (wide ? 1u : 0u)) << 8 | field(src);
    };
    constexpr std::uint32_t NOP = 0xBF800000u;
    struct Edit {
        std::size_t at;
        bool keep;
        bool wide;
        std::uint8_t dst, src;
    };
    std::vector<Edit> edits;
    auto is_pair = [&](std::uint8_t reg, std::uint32_t lane) {
        return std::find(pairs.begin(), pairs.end(), std::pair{reg, lane}) != pairs.end();
    };
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (!parks[i]) {
            continue;
        }
        auto [save, reg, code, lane] = *parks[i];
        auto slot = code_of(reg, lane);
        if (!slot) {
            return {};
        }
        if (is_pair(reg, lane)) {
            auto j = partner(i);
            if (!j) {
                return {};
            }
            std::size_t at = save ? *j : i, gone = save ? i : *j;
            std::uint8_t dst = save ? *slot : code, src = save ? code : *slot;
            edits.push_back({at, true, true, dst, src});
            edits.push_back({gone, false, false, 0, 0});
        } else if (lane > 0 && is_pair(reg, lane - 1)) {

        } else {
            std::uint8_t dst = save ? *slot : code, src = save ? code : *slot;
            edits.push_back({i, true, false, dst, src});
        }
    }
    std::vector<std::array<std::size_t, 3>> moves;
    for (const Edit& e : edits) {
        if (e.keep) {
            put(e.at, Encoding::Sop1, static_cast<std::uint16_t>(3 + (e.wide ? 1 : 0)), mov(e.wide, e.dst, e.src));
            moves.push_back({e.at, e.dst, e.src});
        } else {
            put(e.at, Encoding::Sopp, 0, NOP);
        }
    }
    std::sort(moves.begin(), moves.end());
    return moves;
}

}   

std::vector<std::size_t> reverse_postorder(const std::vector<Block>& blocks) {
    std::vector<bool> seen(blocks.size(), false);
    std::vector<std::size_t> post;
    post.reserve(blocks.size());
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, 0}};
    seen[0] = true;
    while (!stack.empty()) {
        auto& top = stack.back();
        auto [b, i] = top;
        top.second += 1;
        std::size_t s[2];
        std::size_t n = blocks[b].exit.successors(s);
        if (i < n) {
            if (!seen[s[i]]) {
                seen[s[i]] = true;
                stack.emplace_back(s[i], 0);
            }
        } else {
            post.push_back(b);
            stack.pop_back();
        }
    }
    std::reverse(post.begin(), post.end());
    return post;
}

std::optional<std::pair<std::size_t, std::size_t>> irreducible_edge(const std::vector<Block>& blocks, const std::vector<std::size_t>& order,
                                                                    const std::vector<std::size_t>& index) {
    if (order.empty()) {
        return std::nullopt;
    }
    std::size_t entry = order[0];
    auto idom = idoms(blocks, order, index);
    for (std::size_t b : order) {
        std::size_t s[2];
        std::size_t n = blocks[b].exit.successors(s);
        for (std::size_t k = 0; k < n; ++k) {
            if (index[s[k]] > index[b]) {
                continue;
            }
            std::size_t d = b;
            while (d != s[k] && d != entry) {
                d = idom[d];
            }
            if (d != s[k]) {
                return std::pair{b, s[k]};
            }
        }
    }
    return std::nullopt;
}

std::vector<std::size_t> idoms(const std::vector<Block>& blocks, const std::vector<std::size_t>& order, const std::vector<std::size_t>& index) {
    std::vector<std::size_t> idom(blocks.size(), SIZE_MAX);
    if (order.empty()) {
        return idom;
    }
    std::size_t entry = order[0];
    idom[entry] = entry;
    auto intersect = [&](std::size_t a, std::size_t b) {
        while (a != b) {
            while (index[a] > index[b]) {
                a = idom[a];
            }
            while (index[b] > index[a]) {
                b = idom[b];
            }
        }
        return a;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t k = 1; k < order.size(); ++k) {
            std::size_t b = order[k];
            std::size_t now = SIZE_MAX;
            for (std::size_t q : blocks[b].preds) {
                if (idom[q] == SIZE_MAX) {
                    continue;
                }
                now = now == SIZE_MAX ? q : intersect(q, now);
            }
            if (idom[b] != now) {
                idom[b] = now;
                changed = true;
            }
        }
    }
    return idom;
}

Program build(std::span<const std::uint8_t> code, bool wave64, std::uint16_t vgprs) {
    std::vector<Instruction> list = decode_checked(code);
    std::uint8_t lanes = static_cast<std::uint8_t>(1 + (wave64 ? 1 : 0));
    std::vector<Flow> flows;
    flows.reserve(list.size());
    std::vector<std::size_t> leaders{0};
    for (std::size_t i = 0; i < list.size(); ++i) {
        Flow flow = control(list[i], code.size(), [&](std::size_t at) -> const Instruction* {
            auto it = std::partition_point(list.begin(), list.end(), [&](const Instruction& x) { return x.offset < at; });
            return it == list.begin() ? nullptr : &*std::prev(it);
        });
        if (flow.kind == Flow::Jump || flow.kind == Flow::Branch) {
            leaders.push_back(flow.target);
        }
        if (flow.kind != Flow::Next && i + 1 < list.size()) {
            leaders.push_back(list[i + 1].offset);
        }
        flows.push_back(flow);
    }
    std::sort(leaders.begin(), leaders.end());
    leaders.erase(std::unique(leaders.begin(), leaders.end()), leaders.end());
    auto parked_moves = unspill(list, leaders, wave64, lanes, vgprs);
    auto block_at = [&](std::size_t offset) -> std::size_t {
        auto it = std::lower_bound(leaders.begin(), leaders.end(), offset);
        if (it == leaders.end() || *it != offset) {
            isa(offset, "jump into a block");
        }
        return static_cast<std::size_t>(it - leaders.begin());
    };
    std::vector<Block> blocks(leaders.size());
    for (std::size_t i = 0; i < leaders.size(); ++i) {
        blocks[i].offset = leaders[i];
        blocks[i].exit = Exit{Exit::End};
    }


    {
        std::size_t b = 0;
        std::size_t count = 0;
        for (const Instruction& m : list) {
            while (b + 1 < leaders.size() && leaders[b + 1] <= m.offset) {
                blocks[b].insts.reserve(count);
                count = 0;
                ++b;
            }
            ++count;
        }
        blocks[b].insts.reserve(count);
    }
    for (std::size_t n = 0; n < list.size(); ++n) {
        const Instruction& machine = list[n];
        Flow flow = flows[n];
        std::optional<std::size_t> next = n + 1 < list.size() ? std::optional(list[n + 1].offset) : std::nullopt;
        std::size_t b = static_cast<std::size_t>(std::upper_bound(leaders.begin(), leaders.end(), machine.offset) - leaders.begin()) - 1;
        auto name = machine.name();
        if (name.empty()) {
            unsupported(machine.offset, "unknown opcode");
        }
        Fields f = fields(machine, wave64);


        auto pm = std::lower_bound(parked_moves.begin(), parked_moves.end(), n, [](const auto& m, std::size_t x) { return m[0] < x; });
        if (pm != parked_moves.end() && (*pm)[0] == n) {
            if (auto* sop = std::get_if<SopFields>(&f); sop && sop->dst && sop->dst->kind == Operand::Scalar && !sop->src.empty() &&
                                                         sop->src[0].kind == Operand::Scalar) {
                sop->dst->a = static_cast<std::uint8_t>((*pm)[1]);
                sop->src[0].a = static_cast<std::uint8_t>((*pm)[2]);
            }
        }
        auto fallthrough = [&]() -> std::size_t {
            if (!next) {
                isa(machine.offset, "falls off the code");
            }
            return block_at(*next);
        };
        bool ends = flow.kind != Flow::Next || !next || std::binary_search(leaders.begin(), leaders.end(), *next);
        if (ends) {
            switch (flow.kind) {
                case Flow::Next: blocks[b].exit = Exit::jump(fallthrough()); break;
                case Flow::Jump: blocks[b].exit = Exit::jump(block_at(flow.target)); break;
                case Flow::Branch: {
                    std::size_t taken = block_at(flow.target);
                    std::size_t not_taken = fallthrough();
                    blocks[b].exit = Exit::branch(taken, not_taken);
                    break;
                }
                case Flow::Stop: blocks[b].exit = Exit{name == "s_setpc_b64" ? Exit::Indirect : Exit::End}; break;
            }
        }
        thread_local Effects scratch;
        if (effects_fill(machine, f, name, lanes, vgprs, scratch)) {
            auto& [reads, writes] = scratch;

            for (Reg w : writes) {
                if (w.kind == Reg::V && std::find(reads.begin(), reads.end(), w) == reads.end()) {
                    reads.push_back(w);
                }
            }
            Inst inst{machine, std::move(f), {}, {}};
            inst.reads.reserve(reads.size());
            for (Reg r : reads) {
                inst.reads.emplace_back(r, NONE);
            }
            inst.writes.reserve(writes.size());
            for (Reg w : writes) {
                inst.writes.emplace_back(w, NONE);
            }
            blocks[b].insts.push_back(std::move(inst));
        }
    }
    link(blocks);
    if (!blocks[0].preds.empty()) {
        unsupported(0, "a branch to the program entry");
    }
    split_irreducible(blocks);
    return rename(std::move(blocks), wave64, vgprs);
}

Program thread_edges(const Program& p, std::span<const std::array<std::size_t, 3>> edges) {
    std::vector<Block> blocks = p.blocks;
    for (Block& b : blocks) {
        b.phis.clear();
    }
    for (const auto& [pred, join, dest] : edges) {
        Block copy = blocks[join];
        copy.exit = Exit::jump(dest);
        std::size_t target = blocks.size();
        Exit& e = blocks[pred].exit;
        if (e.kind == Exit::Jump) {
            e.a = target;
        } else if (e.kind == Exit::Branch) {
            if (e.a == join) {
                e.a = target;
            }
            if (e.b == join) {
                e.b = target;
            }
        }
        blocks.push_back(std::move(copy));
    }
    link(blocks);
    split_irreducible(blocks);
    return rename(std::move(blocks), p.wave64, p.vgprs);
}

namespace {

bool ends_only(const Program& p, std::size_t b) {
    if (p.blocks[b].exit.kind != Exit::End) {
        return false;
    }
    for (const Inst& i : p.blocks[b].insts) {
        if (!std::holds_alternative<SoppFields>(i.fields)) {
            return false;
        }
    }
    return true;
}

std::optional<Program> renamed(std::vector<Block> blocks, const Program& p) {
    for (Block& b : blocks) {
        b.phis.clear();
    }
    link(blocks);
    try {
        split_irreducible(blocks);
    } catch (const ShaderError&) {
        return std::nullopt;
    }
    return rename(std::move(blocks), p.wave64, p.vgprs);
}

}   

std::optional<Program> without_early_outs(const Program& p) {
    std::vector<Block> blocks = p.blocks;
    bool any = false;
    for (Block& block : blocks) {
        if (block.exit.kind != Exit::Branch) {
            continue;
        }
        std::size_t taken = block.exit.a, not_taken = block.exit.b;
        if (ends_only(p, taken) && !block.insts.empty() && block.insts.back().name() == "s_cbranch_execz") {
            block.insts.pop_back();
            block.exit = Exit::jump(not_taken);
            any = true;
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return renamed(std::move(blocks), p);
}

std::optional<Program> without_skips(const Program& p, bool deep) {
    std::size_t n = p.blocks.size();
    std::unordered_map<std::uint32_t, Value> copy;
    std::unordered_set<std::uint32_t> live;
    auto idle = [&](const Inst& inst) {
        auto name = inst.name();
        if (!std::holds_alternative<SopFields>(inst.fields)) {
            return false;
        }
        if (name.starts_with("s_setpc") || name.starts_with("s_swappc") || name.starts_with("s_call") || name.starts_with("s_rfe") ||
            name.starts_with("s_movrel") || name.starts_with("s_subvector")) {
            return false;
        }
        for (const auto& w : inst.writes) {
            if (live.contains(w.second.id)) {
                return false;
            }
        }
        return true;
    };
    for (const Block& block : p.blocks) {
        for (const Inst& inst : block.insts) {
            auto name = inst.name();
            if ((name == "s_mov_b32" || name == "s_mov_b64") && inst.reads.size() == inst.writes.size()) {
                bool all_s = true;
                for (const auto& r : inst.reads) {
                    all_s = all_s && r.first.kind == Reg::S;
                }
                for (const auto& w : inst.writes) {
                    all_s = all_s && w.first.kind == Reg::S;
                }
                if (all_s) {
                    for (std::size_t k = 0; k < inst.writes.size(); ++k) {
                        copy[inst.writes[k].second.id] = inst.reads[k].second;
                    }
                }
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Block& block : p.blocks) {
            for (const Inst& inst : block.insts) {
                if (!idle(inst)) {
                    for (const auto& r : inst.reads) {
                        changed |= live.insert(r.second.id).second;
                    }
                }
            }
            for (const Phi& phi : block.phis) {
                if (live.contains(phi.value.id)) {
                    for (Value a : phi.args) {
                        changed |= live.insert(a.id).second;
                    }
                }
            }
        }
    }
    auto root = [&](Value v) {
        while (deep) {
            auto it = copy.find(v.id);
            if (it == copy.end()) {
                break;
            }
            v = it->second;
        }
        return v;
    };
    auto skipped = [&](std::size_t b, std::size_t target, std::size_t first) -> bool {
        std::vector<bool> inside(n, false);
        std::vector<std::size_t> stack{first};
        while (!stack.empty()) {
            std::size_t x = stack.back();
            stack.pop_back();
            if (x == target || inside[x]) {
                continue;
            }
            if (x == b) {
                return false;   
            }
            inside[x] = true;
            const Exit& e = p.blocks[x].exit;
            if (e.kind == Exit::End || e.kind == Exit::Indirect) {
                return false;
            }
            std::size_t s[2];
            std::size_t k = e.successors(s);
            for (std::size_t i = 0; i < k; ++i) {
                stack.push_back(s[i]);
            }
        }

        for (std::size_t x = 0; x < n; ++x) {
            if (!inside[x]) {
                continue;
            }
            for (std::size_t q : p.blocks[x].preds) {
                if (q != b && !inside[q]) {
                    return false;
                }
            }
        }

        const Block& join = p.blocks[target];
        for (const Phi& phi : join.phis) {
            bool scalar = phi.reg.kind == Reg::S ? (phi.reg.n != 126 && phi.reg.n != 127) : phi.reg.kind == Reg::Scc;
            if (!scalar || (deep && !live.contains(phi.value.id))) {
                continue;
            }
            std::vector<Value> before;
            for (std::size_t k = 0; k < join.preds.size(); ++k) {
                if (join.preds[k] == b) {
                    before.push_back(root(phi.args[k]));
                }
            }
            for (std::size_t k = 0; k < join.preds.size(); ++k) {
                if (inside[join.preds[k]]) {
                    Value a = root(phi.args[k]);
                    if (std::find(before.begin(), before.end(), a) == before.end()) {
                        return false;
                    }
                }
            }
        }
        if (!deep) {
            for (std::size_t q : join.preds) {
                if (q != b && !inside[q]) {
                    return false;
                }
            }
        }
        return true;
    };
    std::vector<Block> blocks = p.blocks;
    bool any = false;
    for (std::size_t b = 0; b < n; ++b) {
        const Exit& e = p.blocks[b].exit;
        if (e.kind != Exit::Branch) {
            continue;
        }
        std::size_t taken = e.a, not_taken = e.b;
        if (p.blocks[b].insts.empty() || p.blocks[b].insts.back().name() != "s_cbranch_execz") {
            continue;
        }
        if (ends_only(p, taken) || skipped(b, taken, not_taken)) {
            blocks[b].insts.pop_back();
            blocks[b].exit = Exit::jump(not_taken);
            any = true;
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return renamed(std::move(blocks), p);
}

std::vector<Reg> operand_reads(const Program& p, const Inst& inst) {
    std::uint8_t lanes = static_cast<std::uint8_t>(1 + (p.wave64 ? 1 : 0));
    try {
        auto e = effects(inst.machine, inst.fields, inst.name(), lanes, p.vgprs);
        return e ? e->first : std::vector<Reg>{};
    } catch (const ShaderError&) {
        return {};
    }
}

}   
