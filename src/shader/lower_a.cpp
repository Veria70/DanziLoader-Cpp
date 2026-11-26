// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// the free functions, the state's construction, the constant table, global
// loads, and whole-wave sections. See lower_impl.hpp.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

const char* const LANE_MASK = "a lane mask read as data";
const char* const MASK_ELSEWHERE = "a lane mask holding lanes elsewhere read as data";

namespace {

std::atomic<std::uint32_t> g_host_max_wave{0};
thread_local std::vector<std::size_t> t_needs;
thread_local bool t_local_waves = false;
thread_local bool t_local_any = false;

bool env_is(const char* name, const char* value) {
    char* v = nullptr;
    std::size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || !v) {
        return false;
    }
    bool r = value ? std::strcmp(v, value) == 0 : true;
    std::free(v);
    return r;
}
bool env_set(const char* name) { return env_is(name, nullptr); }

std::optional<std::vector<std::pair<std::size_t, std::vector<bool>>>> lane_regions(const structure::Structured& st) {
    std::size_t n = st.exits.size();
    std::vector<std::vector<std::size_t>> succ(n);
    for (std::size_t b = 0; b < n; ++b) {
        const LaneExit& e = st.exits[b];
        switch (e.kind) {
            case LaneExit::Jump: succ[b].push_back(e.taken); break;
            case LaneExit::Branch:
                succ[b].push_back(e.taken);
                succ[b].push_back(e.not_taken);
                break;
            case LaneExit::End: break;
            case LaneExit::Indirect: return std::nullopt;
        }
    }
    std::size_t words = (n + 63) / 64;
    std::vector<std::uint64_t> full(words, UINT64_MAX);
    std::vector<std::vector<std::uint64_t>> pdom(n);
    for (std::size_t b = 0; b < n; ++b) {
        if (succ[b].empty()) {
            pdom[b].assign(words, 0);
            pdom[b][b / 64] |= std::uint64_t{1} << (b % 64);
        } else {
            pdom[b] = full;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t b = n; b-- > 0;) {
            if (succ[b].empty()) {
                continue;
            }
            std::vector<std::uint64_t> v = full;
            for (std::size_t t : succ[b]) {
                for (std::size_t w = 0; w < words; ++w) {
                    v[w] &= pdom[t][w];
                }
            }
            v[b / 64] |= std::uint64_t{1} << (b % 64);
            if (v != pdom[b]) {
                pdom[b] = std::move(v);
                changed = true;
            }
        }
    }
    std::vector<std::pair<std::size_t, std::vector<bool>>> regions;
    for (std::size_t d = 0; d < n; ++d) {
        const LaneExit& e = st.exits[d];
        if (e.kind != LaneExit::Branch) {
            continue;
        }
        if (st.wide.contains(d)) {
            continue;
        }
        auto sup = st.bools.support(e.when);
        if (std::all_of(sup.begin(), sup.end(), [](const Atom& a) { return a.kind == Atom::Uniform; })) {
            continue;
        }
        std::vector<bool> seen(n, false);
        std::vector<std::size_t> stack = succ[d];
        while (!stack.empty()) {
            std::size_t b = stack.back();
            stack.pop_back();
            if (seen[b] || (pdom[d][b / 64] >> (b % 64) & 1) == 1) {
                continue;
            }
            seen[b] = true;
            stack.insert(stack.end(), succ[b].begin(), succ[b].end());
        }
        regions.emplace_back(d, std::move(seen));
    }
    return regions;
}

std::unordered_set<Value, ValueHash> lane_uniform(const ir::Program& p, const structure::Structured& st, const std::vector<bool>& all) {
    auto is_mask = [&](Value v) { return st.masks.contains(v) || st.highs.contains(v); };
    std::unordered_set<Value, ValueHash> same;
    for (std::size_t b : ir::reverse_postorder(p.blocks)) {
        const ir::Block& block = p.blocks[b];
        bool whole = std::all_of(block.preds.begin(), block.preds.end(), [&](std::size_t q) { return q < all.size() && all[q]; });
        for (const ir::Phi& phi : block.phis) {
            if (phi.reg.kind == Reg::V && whole && std::all_of(phi.args.begin(), phi.args.end(), [&](Value a) { return same.contains(a); })) {
                same.insert(phi.value);
            }
        }
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const Inst& inst = block.insts[i];
            auto name = inst.name();
            auto* v = std::get_if<Valu>(&inst.fields);
            if (!(v && !v->dpp && !v->sdwa && !name.starts_with("v_mbcnt") && !name.starts_with("v_cmpx") &&
                  name.find("lane") == std::string_view::npos)) {
                continue;
            }
            if (st.guards[b][i]) {
                continue;
            }
            bool alike = true;
            for (const Operand& op : v->src) {
                if (op.kind == Operand::Vector) {
                    bool any = false, every = true;
                    for (const auto& [r, x] : inst.reads) {
                        if (r.kind == Reg::V && r.n >= op.a) {
                            any = true;
                            if (!same.contains(x)) {
                                every = false;
                            }
                        }
                    }
                    if (!(any && every)) {
                        alike = false;
                        break;
                    }
                } else if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
                    auto x = inst.read(Reg::s(op.a));
                    if (x && is_mask(*x) && !same.contains(*x)) {
                        alike = false;
                        break;
                    }
                }
            }
            if (alike) {
                for (const auto& w : inst.writes) {
                    if (w.first.kind != Reg::Scc) {
                        same.insert(w.second);
                    }
                }
            }
        }
    }
    return same;
}

std::optional<std::vector<std::pair<bool, bool>>> lds_meetings(const ir::Program& p, const structure::Structured& st, const std::vector<bool>& all) {
    std::size_t n = st.exits.size();
    auto succ = [&](std::size_t b) -> std::vector<std::size_t> {
        const LaneExit& e = st.exits[b];
        switch (e.kind) {
            case LaneExit::Jump: return {e.taken};
            case LaneExit::Branch: return {e.taken, e.not_taken};
            default: return {};
        }
    };
    std::vector<std::size_t> region(n);
    for (std::size_t b = 0; b < n; ++b) {
        region[b] = b;
    }
    auto find = [&](std::size_t b) {
        while (region[b] != b) {
            region[b] = region[region[b]];
            b = region[b];
        }
        return b;
    };
    for (std::size_t b = 0; b < n; ++b) {
        for (std::size_t t : succ(b)) {
            if (!all[b] && !all[t]) {
                std::size_t x0 = find(b);
                std::size_t y0 = find(t);
                region[x0] = y0;
            }
        }
    }
    std::vector<bool> reads(n, false), writes(n, false);
    for (std::size_t b = 0; b < p.blocks.size() && b < n; ++b) {
        if (all[b]) {
            continue;
        }
        std::size_t r = find(b);
        for (const Inst& i : p.blocks[b].insts) {
            auto name = i.name();
            if (!std::holds_alternative<DsFields>(i.fields) || name.starts_with("ds_swizzle") || name.find("permute") != std::string_view::npos) {
                continue;
            }
            if (name.starts_with("ds_read")) {
                reads[r] = true;
            } else {
                writes[r] = true;
            }
        }
    }
    for (std::size_t r = 0; r < n; ++r) {
        if (reads[r] && writes[r]) {
            return std::nullopt;
        }
    }
    std::vector<std::pair<bool, bool>> meet(n, {false, false});
    for (std::size_t b = 0; b < n; ++b) {
        for (std::size_t t : succ(b)) {
            if (all[b] && !all[t]) {
                std::size_t r = find(t);
                meet[b].second = meet[b].second || reads[r] || writes[r];
            } else if (!all[b] && all[t]) {
                std::size_t r = find(b);
                meet[t].first = meet[t].first || reads[r] || writes[r];
            }
        }
    }
    return meet;
}

bool lane_constant_masks(const structure::Structured& st) {
    std::vector<Bool> work;
    for (const auto& g : st.guards) {
        for (const auto& x : g) {
            if (x) {
                work.push_back(*x);
            }
        }
    }
    for (const LaneExit& e : st.exits) {
        if (e.kind == LaneExit::Branch) {
            work.push_back(e.when);
        }
    }
    std::unordered_set<std::uint32_t> seen;
    std::unordered_set<std::uint64_t> joins;
    while (!work.empty()) {
        Bool g = work.back();
        work.pop_back();
        if (!seen.insert(g.id).second) {
            continue;
        }
        for (const Atom& a : st.bools.support(g)) {
            if (a.kind == Atom::Opaque || a.kind == Atom::Constant) {
                return true;
            }
            if (a.kind == Atom::Phi && joins.insert(a.payload).second) {
                auto it = st.phi_masks.find(Value{static_cast<std::uint32_t>(a.payload)});
                if (it != st.phi_masks.end()) {
                    work.insert(work.end(), it->second.begin(), it->second.end());
                }
            }
        }
    }
    return false;
}

bool own_bits_on() {
    static const bool on = !env_is("VICTORY_WAVE_OWN_BITS", "0");
    return on;
}

using Packs = std::vector<std::pair<std::uint8_t, std::optional<std::uint8_t>>>;
using PackedLayout = std::vector<std::pair<std::uint8_t, std::vector<std::array<std::uint8_t, 4>>>>;

PackedLayout packed_layout(const std::vector<SigElement>& elements, const Packs& packs_in) {
    auto of_row = [&](std::uint8_t r) {
        std::vector<const SigElement*> of;
        for (const auto& e : elements) {
            if (e.row == std::optional<std::uint32_t>(r)) {
                of.push_back(&e);
            }
        }
        std::stable_sort(of.begin(), of.end(), [](const SigElement* a, const SigElement* b) { return a->col < b->col; });
        return of;
    };
    Packs packs = packs_in;
    std::sort(packs.begin(), packs.end());
    packs.erase(std::unique(packs.begin(), packs.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), packs.end());
    struct Param {
        std::uint8_t a, used;
        std::vector<std::array<std::uint8_t, 4>> of;
    };
    std::vector<Param> layout;
    std::vector<std::array<std::uint8_t, 3>> behind;
    for (const auto& [a, b] : packs) {
        std::vector<std::array<std::uint8_t, 4>> placed;
        std::uint8_t at = 0;
        if (!of_row(a).empty()) {
            std::vector<std::uint8_t> rows{a};
            if (b) {
                rows.push_back(*b);
            }
            for (std::uint8_t r : rows) {
                auto of = of_row(r);
                if (of.empty()) {
                    continue;
                }
                const SigElement* lead = of[0];
                placed.push_back({at, r, lead->col, lead->cols});
                at = static_cast<std::uint8_t>(at + (lead->cols + 1) / 2);
                for (std::size_t k = 1; k < of.size(); ++k) {
                    behind.push_back({r, of[k]->col, of[k]->cols});
                }
            }
        }
        layout.push_back(Param{a, at, std::move(placed)});
    }
    std::sort(behind.begin(), behind.end());
    for (const auto& [r, col, cols] : behind) {
        std::uint8_t need_ = static_cast<std::uint8_t>((cols + 1) / 2);
        for (Param& p : layout) {
            if (p.used + need_ <= 4) {
                p.of.push_back({p.used, r, col, cols});
                p.used = static_cast<std::uint8_t>(p.used + need_);
                break;
            }
        }
    }
    PackedLayout out;
    for (Param& p : layout) {
        out.emplace_back(p.a, std::move(p.of));
    }
    return out;
}

std::unordered_map<std::uint8_t, std::uint8_t> half_input_partners(const StageIo& io, const std::vector<SigElement>& inputs) {
    std::unordered_map<std::uint8_t, std::uint8_t> partners;
    if (io.kind != StageIo::Ps) {
        return partners;
    }
    const auto& entries = io.ps_inputs;
    std::vector<std::uint8_t> free;
    for (const auto& e : inputs) {
        if (!(e.half && e.kind == 0) || !e.row || *e.row > 0xFF) {
            continue;
        }
        auto r = static_cast<std::uint8_t>(*e.row);
        if (std::any_of(entries.begin(), entries.end(), [&](const PsInput& i) { return i.row == r; })) {
            continue;
        }
        if (std::any_of(entries.begin(), entries.end(), [&](const PsInput& i) { return i.fp16 && i.pack2 && i.row2 != i.row && i.row2 == r; })) {
            continue;
        }
        free.push_back(r);
    }
    std::sort(free.begin(), free.end());
    free.erase(std::unique(free.begin(), free.end()), free.end());
    std::vector<const PsInput*> missing;
    for (const auto& i : entries) {
        if (i.fp16 && i.pack2 && i.row2 == i.row) {
            missing.push_back(&i);
        }
    }
    std::stable_sort(missing.begin(), missing.end(), [](const PsInput* a, const PsInput* b) { return a->row < b->row; });
    for (const PsInput* i : missing) {
        auto it = std::find_if(free.begin(), free.end(), [&](std::uint8_t r) { return r > i->row; });
        if (it != free.end()) {
            partners.emplace(i->row, *it);
            free.erase(it);
        }
    }
    return partners;
}

std::unordered_map<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>> half_outputs_of(const StageIo& io, const std::vector<SigElement>& outputs) {
    std::unordered_map<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>> out;
    const std::vector<VsOutput>* entries;
    if (io.kind == StageIo::Vs) {
        entries = &io.vs_outputs;
    } else if (io.kind == StageIo::Ms) {
        entries = &io.ms.outputs;
    } else {
        return out;
    }
    std::vector<const VsOutput*> per_vertex;
    for (const auto& o : *entries) {
        if ((o.param & 0x80) == 0) {
            per_vertex.push_back(&o);
        }
    }
    std::vector<std::uint8_t> free;
    for (const auto& e : outputs) {
        if (!(e.half && e.kind == 0) || !e.row || *e.row > 0xFF) {
            continue;
        }
        auto r = static_cast<std::uint8_t>(*e.row);
        if (std::any_of(per_vertex.begin(), per_vertex.end(), [&](const VsOutput* o) { return o->row == r; })) {
            continue;
        }
        free.push_back(r);
    }
    std::sort(free.begin(), free.end());
    free.erase(std::unique(free.begin(), free.end()), free.end());
    std::vector<const VsOutput*> halves;
    for (const VsOutput* o : per_vertex) {
        if (o->fp16) {
            halves.push_back(o);
        }
    }
    std::stable_sort(halves.begin(), halves.end(), [](const VsOutput* a, const VsOutput* b) { return a->row < b->row; });
    for (const VsOutput* o : halves) {
        std::optional<std::uint8_t> partner;
        if (o->pack2) {
            auto it = std::find_if(free.begin(), free.end(), [&](std::uint8_t r) { return r > o->row; });
            if (it != free.end()) {
                partner = *it;
                free.erase(it);
            }
        }
        out[o->row] = {o->pack2, partner};
    }
    return out;
}

}   


std::optional<std::vector<bool>> all_lane_blocks(const structure::Structured& st) {
    std::size_t n = st.exits.size();
    std::vector<bool> all(n, true);
    auto regions = lane_regions(st);
    if (!regions) {
        return std::nullopt;
    }
    for (const auto& [_, region] : *regions) {
        for (std::size_t b = 0; b < n; ++b) {
            if (region[b]) {
                all[b] = false;
            }
        }
    }
    return all;
}

std::optional<std::vector<std::size_t>> branches_around(const structure::Structured& st, const std::vector<std::size_t>& needs) {
    auto regions = lane_regions(st);
    if (!regions) {
        return std::nullopt;
    }
    std::vector<std::size_t> out;
    for (const auto& [d, region] : *regions) {
        bool holds = std::any_of(needs.begin(), needs.end(), [&](std::size_t b) { return b < region.size() && region[b]; });
        if (holds) {
            if (d >= st.wave_any.size() || !st.wave_any[d]) {
                return std::nullopt;
            }
            out.push_back(d);
        }
    }
    return out;
}

bool needs_every_lane(std::string_view what) { return what.ends_with("where not every lane runs") || what == MASK_ELSEWHERE; }

std::vector<std::size_t> take_needs() { return std::exchange(t_needs, {}); }

void need(std::size_t block) {
    if (std::find(t_needs.begin(), t_needs.end(), block) == t_needs.end()) {
        t_needs.push_back(block);
    }
}

bool& local_waves_flag() { return t_local_waves; }
bool& local_any_flag() { return t_local_any; }

void set_host_max_wave(std::uint32_t lanes) { g_host_max_wave.store(lanes, std::memory_order_relaxed); }
std::uint32_t host_max_wave() { return g_host_max_wave.load(std::memory_order_relaxed); }

std::uint8_t group_sync() {
    static const std::uint8_t mode = [] {
        char* v = nullptr;
        std::size_t n = 0;
        if (_dupenv_s(&v, &n, "VICTORY_GROUP_SYNC") != 0 || !v) {
            return std::uint8_t{2};
        }
        std::string s(v);
        std::free(v);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.erase(s.begin());
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.pop_back();
        }
        if (s == "0") {
            return std::uint8_t{0};
        }
        if (s == "1") {
            return std::uint8_t{1};
        }
        return std::uint8_t{2};
    }();
    return mode;
}

std::uint32_t loop_limit() { return LOOP_LIMIT; }

std::uint32_t meeting_mode() { return group_sync() == 0 ? 8 : (1 | 8); }

bool f16_nearest_on() {
    static const bool on = env_is("VICTORY_F16_NEAREST", "1");
    return on;
}

bool reads_mask_as_data(const structure::Structured& st, const Inst& i) {
    auto is_mask = [&](Value v) { return st.masks.contains(v) || st.highs.contains(v); };
    const Operand* ops = nullptr;
    std::size_t n = 0;
    if (auto* f = std::get_if<SopFields>(&i.fields)) {
        for (const auto& [r, w] : i.writes) {
            if (r.kind == Reg::S && is_mask(w)) {
                return false;
            }
        }
        ops = f->src.data();
        n = f->src.size();
    } else if (auto* v = std::get_if<Valu>(&i.fields); v && i.name() != "v_cndmask_b32" && i.name().find("_co_ci_") == std::string_view::npos) {
        ops = v->src.data();
        n = v->src.size();
    } else {
        return false;
    }
    for (std::size_t k = 0; k < n; ++k) {
        const Operand& op = ops[k];
        if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
            auto r = i.read(Reg::s(op.a));
            if (r && is_mask(*r)) {
                return true;
            }
        }
    }
    return false;
}

std::optional<std::pair<std::uint8_t, std::uint8_t>> packed_half(const std::vector<SigElement>& elements, const Packs& packs, std::uint8_t a,
                                                                 std::uint8_t h) {
    auto layout = packed_layout(elements, packs);
    auto it = std::find_if(layout.begin(), layout.end(), [&](const auto& p) { return p.first == a; });
    if (it == layout.end()) {
        return std::nullopt;
    }
    for (const auto& [dword, row, col, cols] : it->second) {
        if (h < 2 * dword) {
            continue;
        }
        std::uint8_t k = static_cast<std::uint8_t>(h - 2 * dword);
        if (k < cols) {
            return std::pair{row, static_cast<std::uint8_t>(col + k)};
        }
    }
    return std::nullopt;
}

Packs input_packs(const StageIo& io, const std::vector<SigElement>& inputs) {
    Packs out;
    if (io.kind != StageIo::Ps) {
        return out;
    }
    auto partners = half_input_partners(io, inputs);
    for (const PsInput& i : io.ps_inputs) {
        if (!(i.fp16 && i.pack2)) {
            continue;
        }
        std::optional<std::uint8_t> b;
        if (i.row2 != i.row) {
            b = i.row2;
        } else if (auto it = partners.find(i.row); it != partners.end()) {
            b = it->second;
        }
        out.emplace_back(i.row, b);
    }
    return out;
}

std::uint32_t inline_f16(std::uint8_t c) {
    static const std::uint32_t table[] = {0x3800, 0xB800, 0x3C00, 0xBC00, 0x4000, 0xC000, 0x4400, 0xC400, 0x3118};
    return table[c - 240];
}

std::optional<std::pair<const std::vector<std::uint32_t>*, Numeric>> buffer_format(std::uint8_t format) {
    using N = Numeric;
    static const std::vector<N> NORM{N::Unorm, N::Snorm, N::Uscaled, N::Sscaled, N::Uint, N::Sint};
    static const std::vector<N> ALL{N::Unorm, N::Snorm, N::Uscaled, N::Sscaled, N::Uint, N::Sint, N::Float};
    static const std::vector<N> WIDE{N::Uint, N::Sint, N::Float};
    static const std::vector<std::pair<std::vector<std::uint32_t>, const std::vector<N>*>> FAMILIES{
        {{8}, &NORM},
        {{16}, &ALL},
        {{8, 8}, &NORM},
        {{32}, &WIDE},
        {{16, 16}, &ALL},
        {{11, 11, 10}, &ALL},
        {{10, 11, 11}, &ALL},
        {{2, 10, 10, 10}, &NORM},
        {{10, 10, 10, 2}, &NORM},
        {{8, 8, 8, 8}, &NORM},
        {{32, 32}, &WIDE},
        {{16, 16, 16, 16}, &ALL},
        {{32, 32, 32}, &WIDE},
        {{32, 32, 32, 32}, &WIDE},
    };
    if (format == 0) {
        return std::nullopt;
    }
    std::size_t n = format - 1u;
    for (const auto& [bits, numerics] : FAMILIES) {
        if (n < numerics->size()) {
            return std::pair{&bits, (*numerics)[n]};
        }
        n -= numerics->size();
    }
    return std::nullopt;
}

std::uint32_t half_shift(std::uint32_t width) { return width == 16 ? 0 : 15 - width; }

const char* root_type_name(std::uint32_t root, std::string& buf) {
    switch (root) {
        case 0: return "Constants";
        case 1: return "Descriptor";
        case 3: return "Table";
        case 4: return "ImmediateSampler";
        case 5: return "ResourceHeap";
        case 6: return "SamplerHeap";
        default: buf = "Other(" + std::to_string(root) + ")"; return buf.c_str();
    }
}


namespace {

struct WaveKind {
    enum Kind : std::uint8_t { Plain, First, Total, Prefix, Base, Mixed } kind = Plain;
    wave::Op op = wave::Op::AddU;
    bool operator==(const WaveKind& o) const { return kind == o.kind && (kind != Total || op == o.op); }
    static WaveKind of(Kind k) { return {k, wave::Op::AddU}; }
    static WaveKind total(wave::Op op) { return {Total, op}; }
};

}   

std::optional<std::pair<std::size_t, const char*>> wave_local(const ir::Program& p, const structure::Structured& st) {
    using wave::Op;
    using WK = WaveKind;
    static constexpr Why FIRST = "a wave's first-lane number used as data";
    static constexpr Why TOTAL = "a count or sum over the wave used as data";
    static constexpr Why PREFIX = "a count or sum over the lanes below used without a reserved base";
    static constexpr Why MIXED = "a value that depends on the wave on some paths only";
    static constexpr Why EFFECT = "a write by the wave's first lane other than its reservation";
    using KindMap = std::unordered_map<Value, WK, ValueHash>;
    using Firsts = std::unordered_set<Value, ValueHash>;
    auto of = [](const KindMap& kind, std::optional<Value> v) -> WK {
        if (!v) {
            return WK{};
        }
        auto it = kind.find(*v);
        return it != kind.end() ? it->second : WK{};
    };
    auto put = [](KindMap& kind, Value v, WK k) {
        if (k.kind == WK::Plain) {
            kind.erase(v);
        } else {
            kind[v] = k;
        }
    };
    auto wild = [](WK k) { return !(k.kind == WK::Plain || k.kind == WK::Base); };
    auto why = [](WK k) -> Why {
        switch (k.kind) {
            case WK::First: return FIRST;
            case WK::Total: return TOTAL;
            case WK::Prefix: return PREFIX;
            default: return MIXED;
        }
    };
    auto first_mask = [&](const Firsts& firsts, std::optional<Value> m) {
        if (!m) {
            return false;
        }
        auto it = st.masks.find(*m);
        if (it == st.masks.end()) {
            return false;
        }
        for (const Atom& a : st.bools.support(it->second)) {
            if (a.kind == Atom::Lane && firsts.contains(Value{static_cast<std::uint32_t>(a.payload)})) {
                return true;
            }
        }
        return false;
    };
    struct StepOut {
        WK k;
        bool reserved;
        Why err;
    };
    auto step = [&](const Inst& inst, const KindMap& kind, Firsts& firsts) -> StepOut {
        auto name = inst.name();
        auto val = [&](const Operand& op) -> std::optional<Value> {
            if (op.kind == Operand::Vector) {
                return inst.read(Reg::v(op.a));
            }
            if (op.kind == Operand::Scalar && op.a != NULL_CODE) {
                return inst.read(Reg::s(op.a));
            }
            return std::nullopt;
        };
        std::vector<Operand> ops;
        if (auto* v = std::get_if<Valu>(&inst.fields)) {
            ops.assign(v->src.begin(), v->src.end());
        } else if (auto* f = std::get_if<SopFields>(&inst.fields)) {
            ops.assign(f->src.begin(), f->src.end());
            if ((name == "s_addk_i32" || name == "s_mulk_i32") && f->dst) {
                ops.push_back(*f->dst);
            }
        } else if (auto* bf = std::get_if<BufferFields>(&inst.fields);
                   bf && (name.find("_atomic_") != std::string_view::npos || name.find("_store_") != std::string_view::npos)) {
            if (bf->data) {
                ops.push_back(*bf->data);
            }
        }
        std::vector<WK> kinds;
        std::vector<Value> named;
        for (const Operand& o : ops) {
            auto v = val(o);
            kinds.push_back(of(kind, v));
            if (v) {
                named.push_back(*v);
            }
        }
        for (const auto& [r, v] : inst.reads) {
            bool kept = r.kind == Reg::V && std::any_of(inst.writes.begin(), inst.writes.end(), [&](const auto& w) { return w.first == r; });
            if ((r.kind == Reg::S && (r.n == ir::EXEC || r.n == ir::EXEC + 1)) || kept || std::find(named.begin(), named.end(), v) != named.end()) {
                continue;
            }
            WK k = of(kind, v);
            if (wild(k)) {
                return {{}, false, why(k)};
            }
        }
        WK w = WK::of(WK::Mixed);
        for (WK k : kinds) {
            if (wild(k)) {
                w = k;
                break;
            }
        }
        auto sum = [](WK k) { return k == WK::total(Op::AddU); };
        auto is_mask_op = [&](const Operand& o) {
            auto v = val(o);
            return v && (st.masks.contains(*v) || st.highs.contains(*v));
        };
        bool any_wild = std::any_of(kinds.begin(), kinds.end(), wild);
        auto err = [&](Why y) { return StepOut{{}, false, y}; };
        auto ok = [](WK k) { return StepOut{k, false, nullptr}; };
        if ((name == "s_ff1_i32_b64" || name == "s_ff1_i32_b32") && !ops.empty() && is_mask_op(ops[0])) {
            return ok(WK::of(WK::First));
        }
        if ((name == "s_bcnt1_i32_b64" || name == "s_bcnt1_i32_b32") && !ops.empty() && is_mask_op(ops[0])) {
            return ok(WK::total(Op::AddU));
        }
        if ((name == "v_mbcnt_lo_u32_b32" || name == "v_mbcnt_hi_u32_b32") && kinds.size() == 2) {
            bool scalar0 = ops[0].kind == Operand::Scalar;
            WK b = kinds[1];
            if (b.kind == WK::Prefix || (scalar0 && (b.kind == WK::Plain || b.kind == WK::Base))) {
                return ok(WK::of(WK::Prefix));
            }
            if (!scalar0 && (b.kind == WK::Plain || b.kind == WK::Base)) {
                return ok(WK::of(WK::Plain));
            }
            return err(why(b));
        }
        if (!any_wild) {
            bool derives = std::holds_alternative<Valu>(inst.fields) || std::holds_alternative<SopFields>(inst.fields);
            bool has_base = std::any_of(kinds.begin(), kinds.end(), [](WK k) { return k.kind == WK::Base; });
            return ok(derives && has_base ? WK::of(WK::Base) : WK::of(WK::Plain));
        }
        if ((name == "v_mov_b32" || name == "s_mov_b32") && kinds.size() == 1) {
            return ok(kinds[0]);
        }
        if ((name == "v_cmp_eq_i32" || name == "v_cmp_eq_u32" || name == "v_cmpx_eq_i32" || name == "v_cmpx_eq_u32") && kinds.size() == 2) {
            WK a = kinds[0], b = kinds[1];
            const Operand* lane;
            if (a.kind == WK::First && !wild(b)) {
                lane = &ops[1];
            } else if (b.kind == WK::First && !wild(a)) {
                lane = &ops[0];
            } else {
                return err(why(w));
            }
            if (lane->kind != Operand::Vector) {
                return err(FIRST);
            }
            std::optional<Value> mask;
            if (name.starts_with("v_cmpx")) {
                mask = inst.write(Reg::s(ir::EXEC));
            } else if (auto* v = std::get_if<Valu>(&inst.fields); v && v->dst && v->dst->kind == Operand::Scalar) {
                mask = inst.write(Reg::s(v->dst->a));
            }
            if (mask) {
                firsts.insert(*mask);
            }
            return ok(WK::of(WK::Plain));
        }
        if ((name == "v_add_nc_u32" || name == "s_add_i32") && kinds.size() == 2) {
            WK a = kinds[0], b = kinds[1];
            if ((a.kind == WK::Prefix && b.kind == WK::Base) || (a.kind == WK::Base && b.kind == WK::Prefix)) {
                return ok(WK::of(WK::Base));
            }
            if ((a.kind == WK::Prefix && (b.kind == WK::Plain || b.kind == WK::Prefix)) || (a.kind == WK::Plain && b.kind == WK::Prefix)) {
                return ok(WK::of(WK::Prefix));
            }
            if (sum(a) && sum(b)) {
                return ok(a);
            }
            return err(why(w));
        }
        if ((name == "v_sub_nc_u32" || name == "s_sub_i32") && kinds.size() == 2) {
            WK a = kinds[0], b = kinds[1];
            if (a.kind == WK::Base && b.kind == WK::Prefix) {
                return ok(WK::of(WK::Base));
            }
            if (a.kind == WK::Prefix && b.kind == WK::Plain) {
                return ok(WK::of(WK::Prefix));
            }
            if (a.kind == WK::Plain && sum(b) && ops[0] == Operand::constant(128)) {
                return ok(b);
            }
            return err(why(w));
        }
        if ((name == "s_mul_i32" || name == "v_mul_lo_u32" || name == "v_mul_u32_u24") && kinds.size() == 2) {
            WK a = kinds[0], b = kinds[1];
            if (b.kind == WK::Plain && (sum(a) || a.kind == WK::Prefix)) {
                return ok(a);
            }
            if (a.kind == WK::Plain && (sum(b) || b.kind == WK::Prefix)) {
                return ok(b);
            }
            return err(why(w));
        }
        if (name == "s_lshl_b32" && kinds.size() == 2 && kinds[1].kind == WK::Plain && (sum(kinds[0]) || kinds[0].kind == WK::Prefix)) {
            return ok(kinds[0]);
        }
        if (name == "v_lshlrev_b32" && kinds.size() == 2 && kinds[0].kind == WK::Plain && (sum(kinds[1]) || kinds[1].kind == WK::Prefix)) {
            return ok(kinds[1]);
        }
        if (name == "v_lshl_add_u32" && kinds.size() == 3 && kinds[0].kind == WK::Prefix && kinds[1].kind == WK::Plain && !wild(kinds[2])) {
            return ok(kinds[2].kind == WK::Base ? WK::of(WK::Base) : WK::of(WK::Prefix));
        }
        if (kinds.size() == 1 && kinds[0].kind == WK::Total && name.find("_atomic_") != std::string_view::npos) {
            const char* want;
            switch (kinds[0].op) {
                case Op::AddU: want = "buffer_atomic_add"; break;
                case Op::MinU: want = "buffer_atomic_umin"; break;
                case Op::MaxU: want = "buffer_atomic_umax"; break;
                case Op::MinI: want = "buffer_atomic_smin"; break;
                case Op::MaxI: want = "buffer_atomic_smax"; break;
                case Op::Or: want = "buffer_atomic_or"; break;
                case Op::And: want = "buffer_atomic_and"; break;
                case Op::Xor: want = "buffer_atomic_xor"; break;
                default: want = ""; break;
            }
            if (name != want || !first_mask(firsts, inst.read(Reg::s(ir::EXEC)))) {
                return err(TOTAL);
            }
            return StepOut{WK::of(WK::Base), true, nullptr};
        }
        return err(why(w));
    };
    std::vector<std::unordered_map<std::size_t, wave::Section>> secs;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        secs.push_back(wave::sections(p, b));
    }
    auto order = ir::reverse_postorder(p.blocks);
    using Out = std::optional<std::pair<std::size_t, Why>>;
    auto pass = [&](KindMap& kind, Firsts& firsts, bool check) -> Out {
        for (std::size_t b : order) {
            const ir::Block& block = p.blocks[b];
            for (const ir::Phi& phi : block.phis) {
                WK k{};
                for (std::size_t n = 0; n < phi.args.size(); ++n) {
                    WK xk = of(kind, phi.args[n]);
                    if (n == 0) {
                        k = xk;
                    } else if (k == xk) {
                    } else if (k.kind == WK::Plain && (xk.kind == WK::Base || xk.kind == WK::Prefix)) {
                        k = xk;
                    } else if ((k.kind == WK::Base || k.kind == WK::Prefix) && xk.kind == WK::Plain) {
                    } else {
                        k = WK::of(WK::Mixed);
                    }
                }
                put(kind, phi.value, k);
            }
            for (std::size_t i = 0; i < block.insts.size(); ++i) {
                const Inst& inst = block.insts[i];
                const wave::Section* in_sec = nullptr;
                for (const auto& [s, sec] : secs[b]) {
                    if (i > s && i < sec.end) {
                        in_sec = &sec;
                        break;
                    }
                }
                if (in_sec) {
                    for (const auto& [_, w] : inst.writes) {
                        WK k;
                        auto it = in_sec->shapes.find(w);
                        if (it == in_sec->shapes.end()) {
                            k = WK::of(WK::Mixed);
                        } else {
                            const wave::Shape& sh = it->second;
                            switch (sh.kind) {
                                case wave::Shape::K: k = WK::of(WK::Plain); break;
                                case wave::Shape::Value: k = of(kind, sh.a); break;
                                case wave::Shape::Masked: k = of(kind, sh.a); break;
                                case wave::Shape::Active: {
                                    WK sk = of(kind, sh.a);
                                    if (sk.kind == WK::Plain || sk.kind == WK::Base) {
                                        k = WK::total(sh.op);
                                    } else if (sk.kind == WK::Total && sk.op == sh.op && first_mask(firsts, sh.b)) {
                                        k = WK::total(sh.op);
                                    } else {
                                        k = WK::of(WK::Mixed);
                                    }
                                    break;
                                }
                                case wave::Shape::Prefix:
                                    if (sh.op == Op::AddU) {
                                        WK sk = of(kind, sh.a);
                                        if (sk.kind == WK::Plain || sk.kind == WK::Base) {
                                            k = WK::of(WK::Prefix);
                                        } else if (sk == WK::total(Op::AddU) && first_mask(firsts, sh.b)) {
                                            k = WK::of(WK::Prefix);
                                        } else {
                                            k = WK::of(WK::Mixed);
                                        }
                                    } else {
                                        k = WK::of(WK::Mixed);
                                    }
                                    break;
                                default: k = WK::of(WK::Mixed); break;
                            }
                        }
                        put(kind, w, k);
                    }
                    continue;
                }
                auto name = inst.name();
                if (name.starts_with("s_cbranch")) {
                    continue;
                }
                StepOut so = step(inst, kind, firsts);
                WK k;
                bool reserved;
                if (so.err) {
                    if (check) {
                        return std::pair{inst.machine.offset, so.err};
                    }
                    k = WK::of(WK::Mixed);
                    reserved = false;
                } else {
                    k = so.k;
                    reserved = so.reserved;
                }
                bool ds = std::holds_alternative<DsFields>(inst.fields);
                bool effect = std::holds_alternative<ExpFields>(inst.fields) ||
                              (std::holds_alternative<FlatFields>(inst.fields) && !name.starts_with("global_load_")) ||
                              name.find("store") != std::string_view::npos || name.find("atomic") != std::string_view::npos ||
                              (ds && !(name.starts_with("ds_read") || name.starts_with("ds_swizzle") || name.find("permute") != std::string_view::npos));
                if (check && effect && !reserved && first_mask(firsts, inst.read(Reg::s(ir::EXEC)))) {
                    return std::pair{inst.machine.offset, EFFECT};
                }
                bool guarded = st.guards[b][i].has_value();
                for (const auto& [r, w] : inst.writes) {
                    WK kw = k;
                    if (r.kind == Reg::Scc && !name.starts_with("s_bcnt1")) {
                        kw = wild(k) ? WK::of(WK::Mixed) : WK::of(WK::Plain);
                    }
                    if (guarded && r.kind == Reg::V && kw.kind == WK::Plain) {
                        WK old = of(kind, inst.read(r));
                        if (old.kind == WK::First || old.kind == WK::Total || old.kind == WK::Prefix) {
                            kw = WK::of(WK::Mixed);
                        }
                    }
                    put(kind, w, kw);
                }
            }
            if (check && !block.insts.empty()) {
                const Inst& last = block.insts.back();
                if (last.name().starts_with("s_cbranch_scc")) {
                    auto scc = last.read(Reg::scc());
                    if (scc && wild(of(kind, *scc))) {
                        bool own = false;
                        if (st.exits[b].kind == LaneExit::Branch) {
                            own = true;
                            for (const Atom& a : st.bools.support(st.exits[b].when)) {
                                if (a.kind == Atom::Uniform && a.payload == scc->id) {
                                    own = false;
                                    break;
                                }
                            }
                        }
                        if (!own) {
                            return std::pair{last.machine.offset, TOTAL};
                        }
                    }
                }
            }
        }
        return std::nullopt;
    };
    KindMap kind;
    Firsts firsts;
    bool settled = false;
    for (int round = 0; round < 16; ++round) {
        KindMap before = kind;
        std::size_t before_n = firsts.size();
        pass(kind, firsts, false);
        if (before == kind && before_n == firsts.size()) {
            settled = true;
            break;
        }
    }
    if (!settled) {
        return std::pair{std::size_t{0}, MIXED};
    }
    return pass(kind, firsts, true);
}


Lower::Lower(const Parts& parts, const std::unordered_map<Value, Why, ValueHash>& bad_) : s(parts.native->stage == HwStage::Ps), x(parts), bad(bad_) {
    const ir::Program& prog = *x.p;
    const structure::Structured& stt = *x.st;
    for (std::size_t b = 0; b < prog.blocks.size(); ++b) {
        for (std::size_t i = 0; i < prog.blocks[b].phis.size(); ++i) {
            phi_at[prog.blocks[b].phis[i].value] = Site{b, i};
        }
    }
    s.inputs = x.inputs;
    s.outputs = x.outputs;
    auto any_inst = [&](auto f) {
        for (const auto& b : prog.blocks) {
            for (const auto& i : b.insts) {
                if (f(i)) {
                    return true;
                }
            }
        }
        return false;
    };
    writes_uavs = any_inst([](const Inst& i) {
        bool mem = std::holds_alternative<ImageFields>(i.fields) || std::holds_alternative<BufferFields>(i.fields);
        auto n = i.name();
        return mem && (n.find("store") != std::string_view::npos || n.find("atomic") != std::string_view::npos);
    });
    const NativeProgram& native = *x.native;
    if (native.io.kind == StageIo::Cs) {
        const ComputeIo& cs = native.io.cs;
        s.threads = std::array<std::uint32_t, 3>{cs.threads[0], cs.threads[1], cs.threads[2]};
        s.lds_dwords = cs.lds_bytes() / 4;
        console_lanes = any_inst([&](const Inst& i) {
                            auto n = i.name();
                            auto* v = std::get_if<Valu>(&i.fields);
                            return n.starts_with("ds_swizzle") || n.find("permute") != std::string_view::npos || n.starts_with("v_mbcnt") ||
                                   n == "v_readlane_b32" || n == "v_writelane_b32" || (v && v->dpp) || reads_mask_as_data(stt, i);
                        }) ||
                        lane_constant_masks(stt) || !stt.wide.empty();
        std::uint32_t wave = native.wave64 ? 64 : 32;
        std::uint32_t group = cs.threads[0] * cs.threads[1] * cs.threads[2];
        bool one_wave = group <= wave;
        if (wave == 64 && host_max_wave() == 32 && (console_lanes || (one_wave && s.lds_dwords > 0))) {
            bool crosses = any_inst([&](const Inst& i) {
                               auto n = i.name();
                               return n.starts_with("v_mbcnt") || n == "v_readlane_b32" || n == "v_writelane_b32" || n.starts_with("ds_permute") ||
                                      n.starts_with("ds_bpermute") || reads_mask_as_data(stt, i);
                           }) ||
                           lane_constant_masks(stt) || !stt.wide.empty();
            if (!crosses && (s.lds_dwords == 0 || group <= 32)) {
                wave = 32;
            } else {
                auto lds = [](const Inst& i) {
                    auto n = i.name();
                    return std::holds_alternative<DsFields>(i.fields) && !(n.starts_with("ds_swizzle") || n.find("permute") != std::string_view::npos);
                };
                if (auto all = all_lane_blocks(stt)) {
                    bool ok = true;
                    for (std::size_t b = 0; b < prog.blocks.size() && ok; ++b) {
                        for (const Inst& inst : prog.blocks[b].insts) {
                            if (lds(inst) && !(b < all->size() && (*all)[b])) {
                                ok = false;
                                break;
                            }
                        }
                    }
                    bool lds_ok = !(s.lds_dwords > 0 && group > 32) || ok;
                    if (!lds_ok) {
                        if (auto meet = lds_meetings(prog, stt, *all)) {
                            lds_meet = std::pair{*all, std::move(*meet)};
                            lds_ok = true;
                        }
                    }
                    if (!lds_ok) {
                        for (std::size_t b = 0; b < prog.blocks.size(); ++b) {
                            bool every = b < all->size() && (*all)[b];
                            if (!every && std::any_of(prog.blocks[b].insts.begin(), prog.blocks[b].insts.end(), lds)) {
                                need(b);
                            }
                        }
                    }
                    if (lds_ok) {
                        wave = 32;
                        split_lds = s.lds_dwords > 0 && group > 32;
                        if (crosses) {
                            exchange = Exchange{s.lds_dwords, group <= 32, *all};
                            s.lds_dwords += 2 * ((group + 63) / 64);
                        }
                    }
                }
            }
            if (wave == 64 && env_set("VICTORY_WAVE_WHY")) {
                std::vector<const char*> why;
                auto add = [&](const char* r) {
                    if (std::find_if(why.begin(), why.end(), [&](const char* x0) { return std::strcmp(x0, r) == 0; }) == why.end()) {
                        why.push_back(r);
                    }
                };
                for (const auto& blk : prog.blocks) {
                    for (const Inst& i : blk.insts) {
                        auto n = i.name();
                        if (n.starts_with("v_mbcnt")) {
                            add("v_mbcnt");
                        } else if (n == "v_readlane_b32") {
                            add("v_readlane");
                        } else if (n == "v_writelane_b32") {
                            add("v_writelane");
                        } else if (n.starts_with("ds_permute") || n.starts_with("ds_bpermute")) {
                            add("ds_(b)permute");
                        } else if (reads_mask_as_data(stt, i)) {
                            add("mask as data");
                        }
                    }
                }
                if (lane_constant_masks(stt)) {
                    why.push_back("lane-constant mask");
                }
                if (s.lds_dwords > 0 && group > 32) {
                    why.push_back("LDS in a group over 32");
                    auto all = all_lane_blocks(stt);
                    if (!all) {
                        why.push_back("an indirect exit");
                    } else if (!lds_meetings(prog, stt, *all)) {
                        why.push_back("an LDS read and write in one lane-branch region");
                    }
                }
                std::sort(why.begin(), why.end(), [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
                std::string joined;
                for (std::size_t k = 0; k < why.size(); ++k) {
                    joined += (k ? ", " : "");
                    joined += why[k];
                }
                std::fprintf(stderr, "WAVE64-WHY group=%u lds=%s [%s]\n", group, s.lds_dwords > 0 ? "true" : "false", joined.c_str());
            }
        }
        if (wave == 32 && native.wave64) {
            halves = s.lds_dwords;
            s.lds_dwords += 1;
        }
        if (console_lanes || (one_wave && s.lds_dwords > 0)) {
            s.wave_size = wave;
        }
    }
    if (any_inst([](const Inst& i) { return i.name() == "s_barrier"; })) {
        auto order = ir::reverse_postorder(prog.blocks);
        std::vector<std::size_t> place(prog.blocks.size(), SIZE_MAX);
        for (std::size_t n = 0; n < order.size(); ++n) {
            place[order[n]] = n;
        }
        auto at = [&](std::size_t b) { return b < place.size() ? place[b] : SIZE_MAX; };
        auto back = [&](std::size_t from, std::size_t to) { return at(from) != SIZE_MAX && at(to) <= at(from); };
        bool cut_short = false;
        for (std::size_t b = 0; b < stt.exits.size(); ++b) {
            if (stt.exits[b].kind == LaneExit::Jump && back(b, stt.exits[b].taken)) {
                cut_short = true;
                break;
            }
        }
        auto all = all_lane_blocks(stt);
        for (std::size_t b = 0; b < prog.blocks.size(); ++b) {
            if (!std::any_of(prog.blocks[b].insts.begin(), prog.blocks[b].insts.end(), [](const Inst& i) { return i.name() == "s_barrier"; })) {
                continue;
            }
            if (cut_short || !all || !(b < all->size() && (*all)[b])) {
                fence_only.insert(b);
            }
        }
    }
    if (native.io.kind == StageIo::Ms) {
        const MeshIo& ms = native.io.ms;
        s.prims = x.prims;
        s.threads = std::array<std::uint32_t, 3>{ms.threads, 1, 1};
        s.lds_dwords = (ms.lds_bytes + 3) / 4;
        s.mesh = container::Mesh{ms.max_vertices, ms.max_primitives, ms.topology};
        auto order = ir::reverse_postorder(prog.blocks);
        std::vector<std::size_t> index(prog.blocks.size(), SIZE_MAX);
        for (std::size_t n = 0; n < order.size(); ++n) {
            index[order[n]] = n;
        }
        auto idom = ir::idoms(prog.blocks, order, index);
        std::vector<std::size_t> alloc;
        for (std::size_t b = 0; b < prog.blocks.size(); ++b) {
            for (const Inst& i : prog.blocks[b].insts) {
                auto* f = std::get_if<SoppFields>(&i.fields);
                if (i.name() == "s_sendmsg" && f && (f->imm & 0xF) == GS_ALLOC_REQ) {
                    alloc.push_back(b);
                    break;
                }
            }
        }
        for (std::size_t b : order) {
            std::size_t d = b;
            bool dominated;
            for (;;) {
                if (std::find(alloc.begin(), alloc.end(), d) != alloc.end()) {
                    dominated = true;
                    break;
                }
                if (idom[d] == d || idom[d] == SIZE_MAX) {
                    dominated = false;
                    break;
                }
                d = idom[d];
            }
            if (ms.lds_bytes == 0 || dominated) {
                no_barrier.insert(b);
            }
        }
    }
    block.assign(prog.blocks.size(), std::nullopt);
    for (const auto& [v, pr, d] : x.res->params) {
        params[v] = {pr, d};
    }
    for (const auto& d : x.res->descriptors) {
        (d.role == resource::Role::Sampler ? descriptors_smp : descriptors_res)[d.site] = &d.source;
    }
    for (const auto& [site, a] : x.res->scalar) {
        scalar[site] = a;
    }
    for (const Export& e : x.io->exports) {
        exports[e.site] = &e;
    }
    for (const auto& [site, i] : x.io->interps) {
        interps[site] = &i;
    }
    if (!env_is("VICTORY_FIRST_HALF", "0")) {
        for (const auto& [_, i] : x.io->interps) {
            if (i.kind == Interp::Interpolated) {
                for (const Site& f : i.first) {
                    interps.try_emplace(f, &i);
                }
            }
        }
    }
    exported.assign(prog.blocks.size(), false);
    rpo.assign(prog.blocks.size(), SIZE_MAX);
    tail.assign(prog.blocks.size(), std::nullopt);
    local = exchange.has_value() && t_local_waves;
    local_any = t_local_any;
    if (exchange && !exchange->small && !t_local_waves) {
        uniform = lane_uniform(prog, stt, exchange->all);
        if (own_bits_on()) {
            own_bits = own_bits::find(prog, stt, env_set("VICTORY_WAVE_WHY"));
        }
    }
    half_outputs = half_outputs_of(native.io, x.outputs);
    switch (native.stage) {
        case HwStage::Cs: mask_data = MaskData::Console; break;
        case HwStage::Ps:
        case HwStage::Vs: mask_data = MaskData::Host; break;
        default: mask_data = MaskData::None; break;
    }
    for (std::size_t b = 0; b < prog.blocks.size(); ++b) {
        for (const Inst& i : prog.blocks[b].insts) {
            for (const auto& w : i.writes) {
                def_block[w.second] = b;
            }
        }
        for (const ir::Phi& phi : prog.blocks[b].phis) {
            def_block[phi.value] = b;
        }
    }
}

V Lower::code_dword(V byte) {
    if (!code_table) {
        std::optional<std::uint32_t> first;
        auto consider = [&](const Address& a) {
            if (a.param == resource::CODE) {
                std::uint32_t o = a.offset & ~3u;
                if (!first || o < *first) {
                    first = o;
                }
            }
        };
        for (const auto& sc : x.res->scalar) {
            consider(sc.second);
        }
        for (const auto& d : x.res->descriptors) {
            for (const Source* l : d.source.leaves()) {
                if (l->kind == Source::Built) {
                    consider(l->address);
                }
            }
        }
        if (!first) {
            fail("a code table");
        }
        const auto& code = x.native->code;
        if (*first > code.size()) {
            fail("a code table past the code");
        }
        std::vector<std::uint32_t> words;
        for (std::size_t at = *first; at < code.size(); at += 4) {
            std::uint8_t w[4] = {};
            std::memcpy(w, code.data() + at, std::min<std::size_t>(4, code.size() - at));
            std::uint32_t v;
            std::memcpy(&v, w, 4);
            words.push_back(v);
        }
        if (words.empty()) {
            fail("a code table past the code");
        }
        std::vector<V> elems;
        elems.reserve(words.size());
        for (std::uint32_t w : words) {
            elems.push_back(s.m.i32(w));
        }
        Ty arr = s.m.array(static_cast<std::uint32_t>(words.size()), s.t.i32);
        V init = s.m.aggregate(arr, elems);
        V g = s.m.constant_global("code", init);
        code_table = std::tuple{g, *first, static_cast<std::uint32_t>(words.size())};
    }
    auto [g, base, len] = *code_table;
    std::size_t c = cur;
    V k = s.m.i32(base);
    V rel = s.m.bin(c, Bin::Sub, byte, k);
    V two = s.m.i32(2);
    V index = s.m.bin(c, Bin::LShr, rel, two);
    V last = s.m.i32(len - 1);
    V over = s.m.cmp(c, pred::IUGT, index, last);
    index = s.m.select(c, over, last, index);
    V at = s.m.element(c, g, index);
    return s.m.load(c, at);
}

void Lower::find_sections() {
    const ir::Program& prog = p();
    std::unordered_map<Value, std::tuple<std::size_t, std::size_t, std::size_t>, ValueHash> inside;
    for (std::size_t b = 0; b < prog.blocks.size(); ++b) {
        auto found = wave::sections(prog, b);
        for (const auto& [start, sec] : found) {
            for (std::size_t i = start + 1; i < sec.end; ++i) {
                for (const auto& w : prog.blocks[b].insts[i].writes) {
                    inside[w.second] = {b, start, sec.end};
                }
            }
        }
        if (!found.empty()) {
            sections.emplace(b, std::move(found));
        }
    }
    if (inside.empty()) {
        return;
    }
    for (std::size_t b = 0; b < prog.blocks.size(); ++b) {
        const ir::Block& blk = prog.blocks[b];
        for (const ir::Phi& phi : blk.phis) {
            for (Value a : phi.args) {
                if (inside.contains(a)) {
                    section_out.insert(a);
                }
            }
        }
        for (std::size_t i = 0; i < blk.insts.size(); ++i) {
            for (const auto& [_, r] : blk.insts[i].reads) {
                auto it = inside.find(r);
                if (it != inside.end()) {
                    auto [sb, start, end] = it->second;
                    if (!(sb == b && i > start && i < end)) {
                        section_out.insert(r);
                    }
                }
            }
        }
    }
}

void Lower::section_inst(std::size_t b, std::size_t i, std::size_t start) {
    static constexpr Why UNKNOWN = "a whole-wave section step the simulation does not follow";
    const Inst& inst = p().blocks[b].insts[i];
    for (const auto& [_, w] : inst.writes) {
        Val val;
        if (section_out.contains(w)) {
            const auto& shapes = sections.at(b).at(start).shapes;
            auto it = shapes.find(w);
            bool own = own_bits.totals.contains(w);
            if (it == shapes.end()) {
                val = Val::missing(UNKNOWN);
            } else {
                try {
                    val = Val::i32(shape(it->second, own));
                } catch (const WhyError& e) {
                    val = Val::missing(e.why);
                }
            }
        } else {
            val = Val::missing(UNKNOWN);
        }
        vals[w] = val;
    }
}

}   
