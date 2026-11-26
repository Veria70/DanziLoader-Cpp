// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// exports, mesh indices, block exits, loop counters, the joins, and the
// driver that retries with unreadable joins missing. See lower_impl.hpp.
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <set>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

void Lower::export_(std::size_t b, std::size_t i) {
    auto eit = exports.find(Site{b, i});
    if (eit == exports.end()) {
        fail("an export I/O raising did not see");
    }
    const Export& e = *eit->second;
    if (e.compressed) {
        fail("a compressed export");
    }
    std::optional<V> g = guard(b, i);
    bool mesh = s.mesh.has_value();
    std::size_t c = cur;
    if (g && !mesh) {
        if (!(e.done && s.pixel)) {
            fail("a conditional export");
        }
        V t = s.m.boolean(true);
        V kill = s.m.bin(c, Bin::Xor, *g, t);
        s.op(c, "discard", Attr::NoUnwind, s.t.void_, 82, {kill});
    }
    if (mesh && e.target.kind == Target::Primitive) {
        emit_indices(e, g);
        return;
    }
    auto param_row = [&](Usage usage, std::uint8_t row) -> std::optional<std::uint8_t> {
        if (usage.raw != 6 && usage.raw != 7 && usage.raw == 0 && mesh) {
            return unnamed_row();
        }
        return row;
    };
    struct Store {
        std::size_t id;
        std::uint8_t col;
        Value value;
        std::optional<bool> half;
    };
    std::vector<Store> stores;
    bool primitive = e.target.kind == Target::PrimitiveParam;
    auto& outputs = primitive ? s.prims : s.outputs;
    auto by_kind = [&](std::uint8_t kind) -> std::optional<std::size_t> {
        for (std::size_t id = 0; id < outputs.size(); ++id) {
            if (outputs[id].kind == kind) {
                return id;
            }
        }
        return std::nullopt;
    };
    std::optional<std::pair<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>>> halves_info;
    if (e.target.kind == Target::Param) {
        if (auto r = param_row(e.target.usage, e.target.row)) {
            if (auto it = half_outputs.find(*r); it != half_outputs.end()) {
                halves_info = std::pair{*r, it->second};
            }
        }
    }
    if (halves_info) {
        auto [row, hinfo] = *halves_info;
        bool pack2 = hinfo.first;
        std::vector<std::pair<std::uint8_t, std::optional<std::uint8_t>>> packs;
        for (const auto& [r, h] : half_outputs) {
            if (h.first) {
                packs.emplace_back(r, h.second);
            }
        }
        for (std::uint8_t ch = 0; ch < 4; ++ch) {
            if (!e.values[ch]) {
                continue;
            }
            Value value = *e.values[ch];
            for (bool high : {false, true}) {
                auto h = static_cast<std::uint8_t>(2 * ch + (high ? 1 : 0));
                std::optional<std::pair<std::uint8_t, std::uint8_t>> srcp;
                if (pack2) {
                    srcp = packed_half(outputs, packs, row, h);
                } else if (!high) {
                    srcp = std::pair{row, ch};
                }
                if (!srcp) {
                    continue;
                }
                auto [r, cc] = *srcp;
                for (std::size_t id = 0; id < outputs.size(); ++id) {
                    const SigElement& o = outputs[id];
                    if (o.row == std::optional<std::uint32_t>(r) && cc >= o.col && cc < o.col + o.cols) {
                        stores.push_back(Store{id, static_cast<std::uint8_t>(cc - o.col), value, high});
                        break;
                    }
                }
            }
        }
    }
    for (std::uint8_t ch = 0; ch < 4; ++ch) {
        if (!e.values[ch]) {
            continue;
        }
        Value value = *e.values[ch];
        if (halves_info) {
            break;
        }
        std::optional<std::pair<std::size_t, std::uint8_t>> found;
        switch (e.target.kind) {
            case Target::Color:
                for (std::size_t id = 0; id < outputs.size(); ++id) {
                    if (outputs[id].kind == 16 && outputs[id].row == std::optional<std::uint32_t>(e.target.index)) {
                        found = std::pair{id, ch};
                        break;
                    }
                }
                break;
            case Target::Position:
                if (e.target.index == 0) {
                    if (auto id = by_kind(3)) {
                        found = std::pair{*id, ch};
                    }
                } else if (e.target.index == 1 || e.target.index == 2) {
                    for (const auto& o : outputs) {
                        if (o.kind == 4 || o.kind == 5) {
                            fail("render-target or viewport index exports");
                        }
                    }
                    std::vector<std::size_t> dists;
                    for (std::size_t k = 0; k < outputs.size(); ++k) {
                        if (outputs[k].kind == 6 || outputs[k].kind == 7) {
                            dists.push_back(k);
                        }
                    }
                    std::stable_sort(dists.begin(), dists.end(), [&](std::size_t a, std::size_t bb) {
                        return std::pair{outputs[a].kind, outputs[a].index} < std::pair{outputs[bb].kind, outputs[bb].index};
                    });
                    std::vector<std::pair<std::size_t, std::uint8_t>> flat_list;
                    for (std::size_t k : dists) {
                        for (std::uint8_t cc = 0; cc < outputs[k].cols; ++cc) {
                            flat_list.emplace_back(k, cc);
                        }
                    }
                    std::size_t k = static_cast<std::size_t>(e.target.index - 1) * 4 + ch;
                    if (k < flat_list.size()) {
                        found = flat_list[k];
                    }
                } else {
                    fail("clip, cull or point-size exports");
                }
                break;
            case Target::Depth:
                if (ch == 0) {
                    auto id = by_kind(17);
                    if (!id) {
                        id = by_kind(18);
                    }
                    if (!id) {
                        id = by_kind(19);
                    }
                    if (id) {
                        found = std::pair{*id, std::uint8_t{0}};
                    }
                } else if (ch == 1) {
                    if (auto id = by_kind(20)) {
                        found = std::pair{*id, std::uint8_t{0}};
                    }
                } else if (ch == 2) {
                    if (auto id = by_kind(14)) {
                        found = std::pair{*id, std::uint8_t{0}};
                    }
                }
                break;
            case Target::Param:
            case Target::PrimitiveParam: {
                auto row = param_row(e.target.usage, e.target.row);
                for (std::size_t id = 0; id < outputs.size(); ++id) {
                    const SigElement& o = outputs[id];
                    if (row && o.row == std::optional<std::uint32_t>(*row) && ch >= o.col && ch < o.col + o.cols) {
                        found = std::pair{id, static_cast<std::uint8_t>(ch - o.col)};
                        break;
                    }
                }
                break;
            }
            case Target::Null:
            case Target::Unread: break;
            case Target::Primitive: fail("a primitive export");
        }
        if (found) {
            stores.push_back(Store{found->first, found->second, value, std::nullopt});
        }
    }
    for (const Store& st_ : stores) {
        if (!st_.half && outputs[st_.id].half) {
            fail("a 16-bit output the stage data does not mark");
        }
    }
    std::optional<std::pair<std::size_t, std::size_t>> at;
    if (mesh) {
        V flat = mesh_thread();
        std::uint32_t limit = 0;
        if (s.mesh) {
            limit = primitive ? s.mesh->max_primitives : s.mesh->max_vertices;
        }
        std::uint32_t threads = s.threads ? (*s.threads)[0] : UINT32_MAX;
        std::optional<V> mesh_guard = g;
        if (limit > 0 && threads > limit) {
            V lim = s.m.i32(limit);
            V in_bounds = s.m.cmp(cur, pred::IULT, flat, lim);
            mesh_guard = g ? s.m.bin(cur, Bin::And, *g, in_bounds) : in_bounds;
        }
        at = begin_guarded(mesh_guard);
    }
    const char* nm;
    std::uint32_t opcode;
    if (!mesh) {
        nm = "storeOutput";
        opcode = 5;
    } else if (!primitive) {
        nm = "storeVertexOutput";
        opcode = 171;
    } else {
        nm = "storePrimitiveOutput";
        opcode = 172;
    }
    for (const Store& st_ : stores) {
        V xv = i32_of(st_.value);
        if (st_.half) {
            std::size_t cc = cur;
            if (*st_.half) {
                V k16 = s.m.i32(16);
                xv = s.m.bin(cc, Bin::LShr, xv, k16);
            }
            V fv = s.op(cc, "legacyF16ToF32", Attr::ReadNone, s.t.f32, 131, {xv});
            xv = as_int(fv);
        }
        SigElement& el = (primitive ? s.prims : s.outputs)[st_.id];
        el.usage = static_cast<std::uint8_t>(el.usage | (1u << st_.col));
        const char* suffix;
        if (el.comp == 9 && st_.half) {
            suffix = ".f32";
            xv = as_float(xv);
        } else if (el.comp == 9) {
            suffix = ".f32";
            if (auto it = floats.find(st_.value); it != floats.end()) {
                xv = it->second;
            } else {
                xv = as_float(xv);
            }
        } else {
            suffix = ".i32";
        }
        V a0 = s.m.i32(static_cast<std::uint32_t>(st_.id));
        V a1 = s.m.i32(0);
        V a2 = s.m.i8(st_.col);
        std::vector<V> args{a0, a1, a2, xv};
        if (mesh) {
            args.push_back(mesh_thread());
        }
        s.op(cur, std::string(nm) + suffix, Attr::NoUnwind, s.t.void_, opcode, args);
    }
    end_guarded(at, {});
}

void Lower::emit_indices(const Export& e, std::optional<V> g) {
    if (!e.values[0]) {
        fail("a primitive export without its indices");
    }
    V xv = i32_of(*e.values[0]);
    V flat = mesh_thread();
    if (s.mesh) {
        std::uint32_t threads = s.threads ? (*s.threads)[0] : UINT32_MAX;
        if (threads > s.mesh->max_primitives) {
            V pc = s.m.i32(s.mesh->max_primitives);
            V in_bounds = s.m.cmp(cur, pred::IULT, flat, pc);
            g = g ? s.m.bin(cur, Bin::And, *g, in_bounds) : in_bounds;
        }
    }
    auto at = begin_guarded(g);
    std::size_t c = cur;
    V z = s.m.i32(0);
    V culled = s.m.cmp(c, pred::SLT, xv, z);
    std::optional<std::uint32_t> written;
    if (s.mesh) {
        written = std::min(s.mesh->max_vertices, s.threads ? (*s.threads)[0] : UINT32_MAX);
    }
    V indices[3] = {z, z, z};
    for (std::uint32_t k = 0; k < 3; ++k) {
        V shift = s.m.i32(10 * k);
        V mask = s.m.i32(0x1FF);
        V iv = s.m.bin(c, Bin::LShr, xv, shift);
        iv = s.m.bin(c, Bin::And, iv, mask);
        if (written) {
            V n = s.m.i32(*written);
            V past = s.m.cmp(c, pred::IUGE, iv, n);
            culled = s.m.bin(c, Bin::Or, culled, past);
        }
        indices[k] = iv;
    }
    std::vector<V> args{flat};
    for (V iv : indices) {
        args.push_back(s.m.select(c, culled, z, iv));
    }
    s.op(c, "emitIndices", Attr::NoUnwind, s.t.void_, 169, args);
    end_guarded(at, {});
}

std::optional<std::uint8_t> Lower::unnamed_row() const {
    if (x.native->io.kind != StageIo::Ms) {
        return std::nullopt;
    }
    const auto& outs = x.native->io.ms.outputs;
    auto named = [&](std::uint32_t r) {
        return std::any_of(outs.begin(), outs.end(), [&](const VsOutput& o) { return (o.param & 0x80) == 0 && o.usage.raw != 0 && o.row == r; });
    };
    std::set<std::uint32_t> free;
    for (const auto& o : s.outputs) {
        if (o.kind == 0 && o.row && !named(*o.row)) {
            free.insert(*o.row);
        }
    }
    if (free.size() == 1 && *free.begin() <= 0xFF) {
        return static_cast<std::uint8_t>(*free.begin());
    }
    return std::nullopt;
}

void Lower::exit(std::size_t b) {
    const ir::Block& blk = p().blocks[b];
    std::size_t offset = blk.insts.empty() ? blk.offset : blk.insts.back().machine.offset;
    const LaneExit& ex = st().exits[b];
    Term term;
    auto guarded = [&](auto f) {
        try {
            return f();
        } catch (const WhyError& e) {
            stop_unsupported(offset, e.why);
        }
    };
    switch (ex.kind) {
        case LaneExit::Jump: {
            std::size_t t = ex.taken;
            if (counters.contains(t) && rpo[t] <= rpo[b]) {
                V ok = count_iteration(t, pred::IULT);
                std::size_t abort_b = abort_block();
                term = Term::cond_br(ok, *block[t], abort_b);
            } else {
                enter_loop(b, t, cur);
                term = Term::br(*block[t]);
            }
            break;
        }
        case LaneExit::Branch: {
            V cond;
            std::size_t taken = ex.taken, not_taken = ex.not_taken;
            auto node = st().bools.node(ex.when);
            if (st().wide.contains(b)) {
                cond = guarded([&] { return wave_wide(b, ex.when); });
            } else if (node && node->lo == structure::TRUE_ && node->hi == structure::FALSE_) {
                cond = guarded([&] { return atom(node->atom, cur); });
                std::swap(taken, not_taken);
            } else {
                cond = guarded([&] { return boolean(ex.when, cur); });
            }
            auto back = [&](std::size_t to) { return counters.contains(to) && rpo[to] <= rpo[b]; };
            if (back(taken)) {
                V ok = count_iteration(taken, pred::IULT);
                cond = s.m.bin(cur, Bin::And, cond, ok);
            } else if (back(not_taken)) {
                V stop = count_iteration(not_taken, pred::IUGE);
                cond = s.m.bin(cur, Bin::Or, cond, stop);
            }
            std::size_t t = edge(b, taken);
            std::size_t f = edge(b, not_taken);
            for (const auto& [to, via] : {std::pair{taken, t}, std::pair{not_taken, f}}) {
                if (!back(to)) {
                    std::size_t from = block[to] == std::optional<std::size_t>(via) ? cur : via;
                    enter_loop(b, to, from);
                }
            }
            term = Term::cond_br(cond, t, f);
            break;
        }
        case LaneExit::End:
            if (s.pixel && !exported[b] && !has_done_export(b)) {
                V t = s.m.boolean(true);
                s.op(cur, "discard", Attr::NoUnwind, s.t.void_, 82, {t});
            }
            term = Term::ret();
            break;
        case LaneExit::Indirect: stop_unsupported(offset, "an indirect jump");
    }
    s.m.terminate(cur, term);
}

void Lower::enter_loop(std::size_t, std::size_t to, std::size_t via) {
    if (auto it = counters.find(to); it != counters.end()) {
        V z = s.m.i32(0);
        it->second.second.emplace_back(z, via);
    }
}

V Lower::count_iteration(std::size_t to, std::uint8_t pr) {
    std::size_t c = cur;
    V phi = counters.at(to).first;
    V one = s.m.i32(1);
    V next = s.m.bin(c, Bin::Add, phi, one);
    s.m.tag(phi, Tag::Counter);
    s.m.tag(next, Tag::Counter);
    std::uint32_t named = loop_limit();
    std::uint32_t limit;
    if (named == LOOP_LIMIT) {
        if (s.pixel) {
            limit = 1u << 7;
        } else if (s.mesh || !s.threads) {
            limit = 1u << 8;
        } else {
            limit = 1u << 12;
        }
    } else {
        limit = named;
    }
    V lim = s.m.i32(limit);
    counters.at(to).second.emplace_back(next, c);
    V over = s.m.cmp(c, pr, next, lim);
    s.m.tag(over, Tag::Counter);
    return over;
}

std::size_t Lower::abort_block() {
    if (abort) {
        return *abort;
    }
    std::size_t a = s.m.new_block();
    if (s.pixel) {
        V t = s.m.boolean(true);
        s.op(a, "discard", Attr::NoUnwind, s.t.void_, 82, {t});
    }
    s.m.terminate(a, Term::ret());
    abort = a;
    return a;
}

std::size_t Lower::edge(std::size_t from, std::size_t to) {
    std::size_t target = *block[to];
    std::size_t preds = 0;
    for (std::size_t q : p().blocks[to].preds) {
        if (block[q]) {
            ++preds;
        }
    }
    if (preds < 2 || rpo[to] <= rpo[from]) {
        return target;
    }
    std::size_t e = s.m.new_block();
    s.m.terminate(e, Term::br(target));
    edges[{from, to}] = e;
    return e;
}

void Lower::fill_phis() {
    const ir::Program& prog = p();
    const structure::Structured& stt = st();
    while (!pending_masks.empty()) {
        auto [v, b, i] = pending_masks.back();
        pending_masks.pop_back();
        const ir::Phi& phi = prog.blocks[b].phis[i];
        std::size_t offset = prog.blocks[b].offset;
        auto it = stt.phi_masks.find(phi.value);
        if (it == stt.phi_masks.end()) {
            stop_unsupported(offset, "a lane-mask join of an untracked mask");
        }
        const std::vector<Bool>& args = it->second;
        std::vector<std::pair<V, std::size_t>> incoming;
        const auto& preds = prog.blocks[b].preds;
        for (std::size_t n = 0; n < preds.size(); ++n) {
            std::size_t q = preds[n];
            if (!tail[q]) {
                continue;
            }
            std::size_t qb = *tail[q];
            if (auto e = edges.find({q, b}); e != edges.end()) {
                qb = e->second;
            }
            V xv;
            try {
                xv = boolean(args[n], qb);
            } catch (const WhyError& e) {
                stop_unsupported(offset, e.why);
            }
            incoming.emplace_back(xv, qb);
        }
        s.m.set_phi(v, std::move(incoming));
    }
    static const bool strict = [] {
        char* e = nullptr;
        std::size_t n = 0;
        bool on = _dupenv_s(&e, &n, "VICTORY_STRICT_JOINS") == 0 && e;
        std::free(e);
        return on;
    }();
    std::vector<Value> work;
    if (strict) {
        work.assign(read_phis.begin(), read_phis.end());
    }
    while (!work.empty()) {
        Value v = work.back();
        work.pop_back();
        auto it = phi_at.find(v);
        if (it == phi_at.end()) {
            continue;
        }
        auto [b, i] = it->second;
        for (Value a : prog.blocks[b].phis[i].args) {
            if (phi_at.contains(a) && read_phis.insert(a).second) {
                work.push_back(a);
            }
        }
    }
    auto dphis = std::move(data_phis);
    data_phis.clear();
    for (const auto& [v, b, i] : dphis) {
        const ir::Phi& phi = prog.blocks[b].phis[i];
        bool bit = phi.reg.kind == Reg::Scc;
        bool numbers = !bit && !stt.masks.contains(phi.value) && !stt.highs.contains(phi.value) &&
                       std::any_of(phi.args.begin(), phi.args.end(), [&](Value a) {
                           auto vi = vals.find(a);
                           return vi != vals.end() && vi->second.kind == Val::I32;
                       });
        bool read = read_phis.contains(phi.value);
        std::vector<std::pair<V, std::size_t>> incoming;
        const auto& preds = prog.blocks[b].preds;
        for (std::size_t n = 0; n < preds.size(); ++n) {
            std::size_t q = preds[n];
            if (!tail[q]) {
                continue;
            }
            std::size_t qb = *tail[q];
            if (auto e = edges.find({q, b}); e != edges.end()) {
                qb = e->second;
            }
            Value arg = phi.args[n];
            if (read && !vals.contains(arg)) {
                try {
                    lazy_barycentric(arg);
                } catch (const WhyError&) {
                }
                if (!vals.contains(arg)) {
                    if (auto pit = params.find(arg); pit != params.end()) {
                        Val val = user(pit->second.first, pit->second.second);
                        vals[arg] = val;
                    }
                }
            }
            std::optional<Val> got;
            if (auto vi = vals.find(arg); vi != vals.end()) {
                got = vi->second;
            }
            bool missing_or_none = !got || got->kind == Val::Missing;
            V xv;
            std::vector<Value> seen;
            if (got && ((got->kind == Val::I32 && !bit) || (got->kind == Val::Bit && bit))) {
                xv = got->v;
            } else if (missing_or_none && !bit && numbers && dead(arg, seen)) {
                xv = s.m.i32(0);
            } else if (missing_or_none && !bit && read && mask_data == MaskData::Console && (stt.masks.contains(arg) || stt.highs.contains(arg))) {
                try {
                    xv = mask_dword(arg);
                } catch (const WhyError& e) {
                    throw StopRetry{phi.value, e.why};
                }
            } else if (missing_or_none && bit && read && mask_data == MaskData::Console && stt.scc_some.contains(arg)) {
                bool per_lane = false;
                if (!stt.wide.contains(q) && stt.exits[q].kind == LaneExit::Branch) {
                    auto sup = stt.bools.support(stt.exits[q].when);
                    per_lane = !std::all_of(sup.begin(), sup.end(), [](const Atom& a) { return a.kind == Atom::Uniform; });
                }
                if (per_lane) {
                    for (std::size_t other : preds) {
                        if (other != q) {
                            need(other);
                        }
                    }
                    throw StopRetry{phi.value, LANE_MASK};
                }
                try {
                    xv = scc_some(arg);
                } catch (const WhyError& e) {
                    throw StopRetry{phi.value, e.why};
                }
            } else if (got && got->kind == Val::Bit && !bit && read) {
                throw StopRetry{phi.value, "an SCC joined with SGPR values"};
            } else if (got && got->kind == Val::Missing && read) {
                throw StopRetry{phi.value, got->why};
            } else if (read) {
                throw StopRetry{phi.value, "a join of mismatched values"};
            } else if (bit) {
                xv = s.m.boolean(false);
            } else {
                xv = s.m.i32(0);
            }
            incoming.emplace_back(xv, qb);
        }
        s.m.set_phi(v, std::move(incoming));
    }
    for (auto& [_, pr] : counters) {
        s.m.set_phi(pr.first, std::move(pr.second));
    }
    counters.clear();
}

Lowered lower(const Parts& parts) {
    std::unordered_map<Value, Why, ValueHash> bad;
    for (;;) {
        try {
            Lower l(parts, bad);
            return l.run();
        } catch (const StopRetry& r) {
            if (!bad.emplace(r.v, r.why).second) {
                unsupported(0, "a phi that does not settle");
            }
        }
    }
}

}   
