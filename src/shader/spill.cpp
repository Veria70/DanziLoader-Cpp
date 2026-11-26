// SPDX-License-Identifier: GPL-3.0-or-later
// The spilled user-data table of a compute program (`spill.rs`) and the
// geometry half of a merged ES-GS stage (`merged.rs`; DanziLoader /
// victory-perf, see NOTICE.md).
#include "spill.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace danzi::shader {

namespace {

using ir::Reg;
using ir::Value;


constexpr std::uint32_t FIRST = 16;


std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>> spilled(const NativeProgram& native) {
    if (native.io.kind != StageIo::Cs || native.stage != HwStage::Cs) {
        return std::nullopt;
    }
    std::uint32_t registers = native.io.cs.user_sgprs();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    for (const RootParam& e : native.layout) {
        if (e.sgprs > 0 && std::uint32_t{e.user_data} >= registers) {
            out.emplace_back(e.user_data, e.sgprs);
        }
    }
    std::optional<std::uint32_t> least;
    for (const auto& [at, n] : out) {
        least = least ? std::min(*least, at) : at;
    }
    if (least == FIRST && registers >= 2) {
        return out;
    }
    return std::nullopt;
}

std::optional<Value> entry(const ir::Program& p, Reg reg) {
    for (const auto& i : p.inputs) {
        if (i.first == reg) {
            return i.second;
        }
    }
    return std::nullopt;
}

bool has(const std::vector<Value>& v, Value x) { return std::find(v.begin(), v.end(), x) != v.end(); }

std::uint32_t next_value(const ir::Program& p) {
    std::uint32_t next = 0;
    for (const auto& i : p.inputs) {
        next = std::max(next, i.second.id + 1);
    }
    for (const auto& b : p.blocks) {
        for (const auto& phi : b.phis) {
            next = std::max(next, phi.value.id + 1);
        }
        for (const auto& inst : b.insts) {
            for (const auto& w : inst.writes) {
                next = std::max(next, w.second.id + 1);
            }
        }
    }
    return next;
}


void trace_pointer(const ir::Program& p, std::vector<Value>& lows, std::vector<Value>& highs, std::vector<Value>& raws,
                   std::vector<std::pair<std::size_t, std::size_t>>* forming) {
    for (;;) {
        bool changed = false;
        for (std::size_t b = 0; b < p.blocks.size(); ++b) {
            const auto& block = p.blocks[b];
            for (std::size_t i = 0; i < block.insts.size(); ++i) {
                const ir::Inst& inst = block.insts[i];
                auto* sop = std::get_if<SopFields>(&inst.fields);
                if (!sop || !sop->dst || sop->dst->kind != Operand::Scalar) {
                    continue;
                }
                auto out = inst.write(Reg::s(sop->dst->a));
                if (!out) {
                    continue;
                }
                auto read = [&](const Operand& op) -> std::optional<Value> {
                    if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
                        return inst.read(Reg::s(op.a));
                    }
                    return std::nullopt;
                };
                std::vector<Value>* set = nullptr;
                auto name = inst.name();
                if (name == "s_mov_b32" && sop->src.size() == 1) {
                    auto v = read(sop->src[0]);
                    if (v && has(lows, *v)) {
                        set = &lows;
                    } else if (v && has(highs, *v)) {
                        set = &highs;
                    } else if (v && has(raws, *v)) {
                        set = &raws;
                    }
                } else if (name == "s_and_b32" && sop->src.size() == 2) {
                    auto masked = [&](const Operand& a, const Operand& m) {
                        if (m.kind != Operand::Literal || m.literal != 0xff) {
                            return false;
                        }
                        auto v = read(a);
                        return v && has(raws, *v);
                    };
                    if (masked(sop->src[0], sop->src[1]) || masked(sop->src[1], sop->src[0])) {
                        set = &highs;
                    }
                }
                if (set && !has(*set, *out)) {
                    set->push_back(*out);
                    if (forming) {
                        forming->emplace_back(b, i);
                    }
                    changed = true;
                }
            }
        }
        if (!changed) {
            break;
        }
    }
}

std::unordered_set<std::uint32_t> values_read(const ir::Program& p) {
    std::unordered_set<std::uint32_t> out;
    for (const auto& block : p.blocks) {
        for (const auto& phi : block.phis) {
            for (Value a : phi.args) {
                out.insert(a.id);
            }
        }
        for (const auto& inst : block.insts) {
            for (const auto& r : inst.reads) {
                out.insert(r.second.id);
            }
        }
    }
    return out;
}

bool contains_site(const std::vector<std::pair<std::size_t, std::size_t>>& v, std::size_t b, std::size_t i) {
    return std::find(v.begin(), v.end(), std::pair{b, i}) != v.end();
}

void remove_sites(ir::Program& p, const std::vector<std::pair<std::size_t, std::size_t>>& sites) {
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        auto& insts = p.blocks[b].insts;
        std::vector<ir::Inst> kept;
        kept.reserve(insts.size());
        for (std::size_t i = 0; i < insts.size(); ++i) {
            if (!contains_site(sites, b, i)) {
                kept.push_back(std::move(insts[i]));
            }
        }
        insts = std::move(kept);
    }
}

}   

std::size_t spill_user_data(ir::Program& p, const NativeProgram& native) {
    auto spill = spilled(native);
    if (!spill) {
        return 0;
    }
    auto low = entry(p, Reg::s(0)), high = entry(p, Reg::s(1));
    if (!low || !high) {
        return 0;
    }
    std::vector<Value> lows{*low}, highs, raws{*high};
    std::vector<std::pair<std::size_t, std::size_t>> forming;
    trace_pointer(p, lows, highs, raws, &forming);
    if (highs.empty()) {
        return 0;
    }
    auto read_somewhere = values_read(p);
    std::uint32_t next = next_value(p);
    std::unordered_map<std::uint32_t, Value> replaced;
    std::vector<std::pair<std::size_t, std::size_t>> gone;
    ir::RegValues inputs = p.inputs;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const auto& block = p.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const ir::Inst& inst = block.insts[i];
            auto* sm = std::get_if<SmemFields>(&inst.fields);
            if (!sm || !sm->data || sm->data->kind != Operand::Scalar || sm->base.kind != Operand::Scalar) {
                continue;
            }
            std::uint8_t first = sm->data->a, base = sm->base.a;
            bool soffset_none = (sm->soffset.kind == Operand::Scalar && sm->soffset.a == NULL_CODE) ||
                                (sm->soffset.kind == Operand::Const && sm->soffset.a == 128);
            auto rb = inst.read(Reg::s(base));
            auto rb1 = inst.read(Reg::s(static_cast<std::uint8_t>(base + 1)));
            if (!inst.name().starts_with("s_load_dword") || !soffset_none || sm->offset < 0 || sm->offset % 4 != 0 || !rb || !has(lows, *rb) ||
                !rb1 || !has(highs, *rb1)) {
                continue;
            }
            std::vector<std::pair<Reg, Value>> dwords;
            bool skip = false;
            for (const auto& [reg, v] : inst.writes) {
                if (reg.kind != Reg::S || reg.n < first) {
                    skip = true;
                    break;
                }
                std::uint32_t k = reg.n - first;
                std::uint32_t index = FIRST + static_cast<std::uint32_t>(sm->offset) / 4 + k;
                bool declared = std::any_of(spill->begin(), spill->end(), [&](const auto& e) { return index >= e.first && index < e.first + e.second; });
                if (!declared) {
                    if (read_somewhere.contains(v.id)) {
                        skip = true;
                        break;
                    }
                    continue;
                }

                if (index > 255 || index > 105) {
                    skip = true;
                    break;
                }
                dwords.emplace_back(Reg::s(static_cast<std::uint8_t>(index)), v);
            }
            if (skip) {
                continue;
            }
            for (const auto& [reg, v] : dwords) {
                Value value;
                auto it = std::find_if(inputs.begin(), inputs.end(), [&](const auto& e) { return e.first == reg; });
                if (it != inputs.end()) {
                    value = it->second;
                } else {
                    value = Value{next++};
                    inputs.emplace_back(reg, value);
                }
                replaced[v.id] = value;
            }
            gone.emplace_back(b, i);
        }
    }
    if (gone.empty()) {
        return 0;
    }
    p.inputs = std::move(inputs);
    auto renamed = [&](Value& v) {
        if (auto it = replaced.find(v.id); it != replaced.end()) {
            v = it->second;
        }
    };
    for (auto& block : p.blocks) {
        for (auto& phi : block.phis) {
            for (auto& a : phi.args) {
                renamed(a);
            }
        }
        for (auto& inst : block.insts) {
            for (auto& r : inst.reads) {
                renamed(r.second);
            }
        }
    }

    std::vector<std::pair<std::size_t, std::size_t>> drop = gone;
    for (;;) {
        std::unordered_set<std::uint32_t> used;
        for (std::size_t b = 0; b < p.blocks.size(); ++b) {
            const auto& block = p.blocks[b];
            for (const auto& phi : block.phis) {
                for (Value a : phi.args) {
                    used.insert(a.id);
                }
            }
            for (std::size_t i = 0; i < block.insts.size(); ++i) {
                if (!contains_site(drop, b, i)) {
                    for (const auto& r : block.insts[i].reads) {
                        used.insert(r.second.id);
                    }
                }
            }
        }
        std::size_t before = drop.size();
        for (const auto& site : forming) {
            const ir::Inst& inst = p.blocks[site.first].insts[site.second];
            bool unread = std::all_of(inst.writes.begin(), inst.writes.end(), [&](const auto& w) { return !used.contains(w.second.id); });
            if (!contains_site(drop, site.first, site.second) && unread) {
                drop.push_back(site);
            }
        }
        if (drop.size() == before) {
            break;
        }
    }
    remove_sites(p, drop);

    auto read = values_read(p);
    std::erase_if(p.inputs, [&](const auto& e) {
        bool pointer = e.first.kind == Reg::S && (e.first.n == 0 || e.first.n == 1);
        return pointer && !read.contains(e.second.id);
    });
    return gone.size();
}

std::size_t merged_prepare(ir::Program& p, const NativeProgram& native) {


    constexpr std::uint8_t DRIVER_TABLE = 240;
    if (native.stage != HwStage::Gs) {
        return 0;
    }
    std::uint32_t next = next_value(p);
    auto fresh = [&] { return Value{next++}; };
    std::unordered_map<std::uint32_t, Value> renamed;
    std::vector<std::pair<std::size_t, std::size_t>> gone;
    std::vector<std::uint8_t> halves = p.wave64 ? std::vector<std::uint8_t>{ir::EXEC, ir::EXEC + 1} : std::vector<std::uint8_t>{ir::EXEC};
    std::vector<std::pair<std::uint8_t, Value>> entry_exec;
    for (std::uint8_t r : halves) {
        Value v;
        auto e = entry(p, Reg::s(r));
        if (e) {
            v = *e;
        } else {
            v = fresh();
            p.inputs.emplace_back(Reg::s(r), v);
        }
        entry_exec.emplace_back(r, v);
    }
    if (!p.blocks.empty()) {
        const auto& block = p.blocks[0];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const ir::Inst& inst = block.insts[i];
            auto* sop = std::get_if<SopFields>(&inst.fields);
            bool to_exec = sop && sop->dst && sop->dst->kind == Operand::Scalar && sop->dst->a == ir::EXEC;
            bool ok = false;
            auto name = inst.name();
            if (name == "s_bfm_b64") {
                ok = true;
            } else if (name == "s_cselect_b64" && sop) {
                ok = sop->src.size() == 2 && sop->src[0].kind == Operand::Const && sop->src[0].a == 193 && sop->src[1].kind == Operand::Scalar &&
                     sop->src[1].a == ir::EXEC;
            }
            if (!(to_exec && ok)) {
                continue;
            }
            for (const auto& [r, v] : inst.writes) {
                for (const auto& [code, e] : entry_exec) {
                    if (Reg::s(code) == r) {
                        renamed[v.id] = e;
                        break;
                    }
                }
            }
            gone.emplace_back(0, i);
        }
    }
    auto low = entry(p, Reg::s(8)), high = entry(p, Reg::s(9));
    if (low && high) {
        std::vector<Value> lows{*low}, highs, raws{*high};
        trace_pointer(p, lows, highs, raws, nullptr);
        std::uint8_t k = 0;
        for (std::size_t b = 0; b < p.blocks.size(); ++b) {
            const auto& block = p.blocks[b];
            for (std::size_t i = 0; i < block.insts.size(); ++i) {
                const ir::Inst& inst = block.insts[i];
                auto* sm = std::get_if<SmemFields>(&inst.fields);
                if (!sm || sm->base.kind != Operand::Scalar) {
                    continue;
                }
                std::uint8_t base = sm->base.a;
                auto rb = inst.read(Reg::s(base));
                auto rb1 = inst.read(Reg::s(static_cast<std::uint8_t>(base + 1)));
                if (!inst.name().starts_with("s_load_dword") || !rb || !has(lows, *rb) || !rb1 || !has(highs, *rb1)) {
                    continue;
                }
                for (const auto& [reg, v] : inst.writes) {
                    std::uint8_t code = static_cast<std::uint8_t>(std::min(255, int{DRIVER_TABLE} + k));
                    k = static_cast<std::uint8_t>(std::min(255, k + 1));
                    Value value = fresh();
                    p.inputs.emplace_back(Reg::s(code), value);
                    renamed[v.id] = value;
                }
                gone.emplace_back(b, i);
            }
        }
    }
    if (gone.empty()) {
        return 0;
    }
    auto target = [&](Value v) {
        for (auto it = renamed.find(v.id); it != renamed.end(); it = renamed.find(v.id)) {
            v = it->second;
        }
        return v;
    };
    for (auto& block : p.blocks) {
        for (auto& phi : block.phis) {
            for (auto& a : phi.args) {
                a = target(a);
            }
        }
        for (auto& inst : block.insts) {
            for (auto& r : inst.reads) {
                r.second = target(r.second);
            }
        }
    }
    remove_sites(p, gone);
    return gone.size();
}

ir::Program spill_build(const NativeProgram& native) {
    auto code = native.patched_code();
    ir::Program p = ir::build(code, native.wave64, native.vgprs);
    spill_user_data(p, native);
    merged_prepare(p, native);
    return p;
}

}   
