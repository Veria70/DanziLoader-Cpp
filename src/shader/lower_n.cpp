// SPDX-License-Identifier: GPL-3.0-or-later
// The console's ray-tracing node test as inline DXIL (`emit/intersect.rs`;
// DanziLoader / victory-perf, see NOTICE.md): `image_bvh_intersect_ray` and
// `image_bvh64_intersect_ray`, the same float operations in the same order
// as the reference's CPU model (`bvh.rs`). Nodes are read from the host's
// copies of title memory, or from a raw buffer of the host.
#include <algorithm>
#include <bit>
#include <limits>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

namespace {

constexpr std::uint32_t BOX_SORT = 1u << 31;
constexpr std::uint32_t RETURN_BARYCENTRICS = 1u << 24;
constexpr std::uint32_t NONE = 0xFFFFFFFFu;
const std::uint32_t MISS_BITS[4] = {0x7F800000u, 0x3F800000u, 0, 0};

std::uint32_t vertex_map(std::uint32_t k) {
    static const std::uint32_t TRIANGLE_VERTICES[4][3] = {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {2, 4, 0}};
    const auto& v = TRIANGLE_VERTICES[k & 3];
    return v[0] | v[1] << 4 | v[2] << 8;
}

}   

void Lower::bvh(const Inst& inst, std::size_t b, std::size_t i) {
    const auto& f = std::get<ImageFields>(inst.fields);
    if (f.a16 || f.d16 || f.tfe || f.lwe || f.dmask != 0xf) {
        fail("a ray-tracing node test with 16-bit operands or a partial result");
    }
    bool wide = inst.name() == "image_bvh64_intersect_ray";
    std::vector<std::uint8_t> regs;
    for (const Operand& a : f.addr) {
        if (a.kind == Operand::Vector) {
            for (std::uint8_t k = 0; k < a.b; ++k) {
                regs.push_back(static_cast<std::uint8_t>(a.a + k));
            }
        }
    }
    if (regs.size() != (wide ? 12u : 11u)) {
        fail("a ray-tracing node test's operands");
    }
    if (f.data.kind != Operand::Vector) {
        fail("a ray-tracing node test's result");
    }
    std::uint8_t dst = f.data.a;
    Site site{b, i};
    const resource::BvhNodes* nodes = nullptr;
    for (const auto& n : x.res->bvh) {
        if (n.first == site) {
            nodes = &n.second;
            break;
        }
    }
    if (!nodes) {
        fail("an untracked ray-tracing structure");
    }
    bool sort, barycentrics;
    if (nodes->kind == resource::BvhNodes::Resource) {
        sort = true;
        barycentrics = true;
    } else {
        sort = (nodes->word1 & BOX_SORT) != 0;
        barycentrics = (nodes->word3 & RETURN_BARYCENTRICS) != 0;
    }
    if (!barycentrics) {
        fail("a ray-tracing structure that returns triangle ids");
    }
    std::optional<V> g = guard(b, i);
    std::vector<V> xs;
    for (std::uint8_t r : regs) {
        auto v = inst.read(Reg::v(r));
        if (!v) {
            fail("an unread operand");
        }
        xs.push_back(i32_of(*v));
    }
    V pointer = xs[0];
    std::optional<V> pointer_high;
    if (wide) {
        pointer_high = xs[1];
    }
    std::vector<V> fl;
    for (std::size_t k = 1 + (wide ? 1 : 0); k < xs.size(); ++k) {
        fl.push_back(as_float(xs[k]));
    }
    Ray ray{fl[0], {fl[1], fl[2], fl[3]}, {fl[4], fl[5], fl[6]}, {fl[7], fl[8], fl[9]}};
    std::size_t c = cur;
    NodeAt at;
    if (nodes->kind == resource::BvhNodes::Resource) {
        auto dit = descriptors_res.find(site);
        if (dit == descriptors_res.end()) {
            fail("an untracked ray-tracing structure");
        }
        V hnd = handle(site, *dit->second, false, {11, 0});
        V kind_bits = s.m.i32(~7u);
        V k3 = s.m.i32(3);
        V byte = s.m.bin(c, Bin::And, pointer, kind_bits);
        byte = s.m.bin(c, Bin::Shl, byte, k3);
        at = NodeAt{false, hnd, byte, {}, {}};
    } else {
        if (f.rsrc.kind != Operand::Scalar) {
            fail("a ray-tracing structure descriptor not in SGPRs");
        }
        V words[2] = {pointer, pointer};
        for (std::uint8_t k = 0; k < 2; ++k) {
            auto v = inst.read(Reg::s(static_cast<std::uint8_t>(f.rsrc.a + k)));
            if (!v) {
                fail("an unread operand");
            }
            words[k] = i32_of(*v);
        }
        auto [low, high] = bvh_address(words[0], words[1], pointer, pointer_high);
        auto [hnd, byte] = memory(low, high, g);
        at = NodeAt{true, hnd, byte, low, high};
    }
    V zero = s.m.i32(0);
    auto gate = begin_guarded(g);
    std::size_t head = cur;
    V k4 = s.m.i32(4);
    V k6 = s.m.i32(6);
    V k7 = s.m.i32(7);
    V kind = s.m.bin(head, Bin::And, pointer, k7);
    V pairv = s.m.bin(head, Bin::And, pointer, k6);
    V is_box = s.m.cmp(head, pred::EQ, pairv, k4);
    V is_triangle = s.m.cmp(head, pred::IULT, kind, k4);
    std::size_t box_block = s.m.new_block();
    std::size_t leaf_block = s.m.new_block();
    std::size_t triangle_block = s.m.new_block();
    std::size_t join = s.m.new_block();
    s.m.terminate(head, Term::cond_br(is_box, box_block, leaf_block));
    s.m.terminate(leaf_block, Term::cond_br(is_triangle, triangle_block, join));
    cur = box_block;
    auto boxes = bvh_boxes(at, kind, ray, sort);
    std::size_t box_end = cur;
    s.m.terminate(box_end, Term::br(join));
    cur = triangle_block;
    auto triangle = bvh_triangle(at, kind, ray);
    std::size_t triangle_end = cur;
    s.m.terminate(triangle_end, Term::br(join));
    cur = join;
    V other[4];
    for (int k = 0; k < 4; ++k) {
        other[k] = s.m.i32(MISS_BITS[k]);
    }
    Ty i32t = s.t.i32;
    std::vector<std::pair<V, V>> results;
    for (int k = 0; k < 4; ++k) {
        V phi = s.m.phi(join, i32t);
        s.m.set_phi(phi, {{boxes[k], box_end}, {triangle[k], triangle_end}, {other[k], leaf_block}});
        results.emplace_back(phi, zero);
    }
    auto rs = end_guarded(gate, results);
    for (std::size_t k = 0; k < rs.size(); ++k) {
        write(inst, Reg::v(static_cast<std::uint8_t>(dst + k)), rs[k], g);
    }
}

std::vector<V> Lower::bvh_node_words(const NodeAt& at, std::optional<V> base, std::uint32_t offset, std::uint32_t count) {
    std::size_t c = cur;
    V undef = s.m.undef(s.t.i32);
    V align = s.m.i32(4);
    std::optional<std::pair<V, V>> second;
    std::vector<V> out;
    for (std::uint32_t start = 0; start < count; start += 4) {
        std::uint32_t n = std::min(count - start, 4u);
        std::uint32_t node_byte = offset + 4 * start;
        V hnd, byte;
        if (!at.memory || node_byte < 64) {
            hnd = at.handle;
            byte = at.byte;
        } else {
            if (!second) {
                auto [l2, h2] = address_plus(at.low, at.high, 64);
                auto [hh, bb] = memory(l2, h2, std::nullopt);
                V k64 = s.m.i32(64);
                second = std::pair{hh, s.m.bin(c, Bin::Sub, bb, k64)};
            }
            hnd = second->first;
            byte = second->second;
        }
        if (base) {
            byte = s.m.bin(c, Bin::Add, byte, *base);
        }
        V k = s.m.i32(node_byte);
        byte = s.m.bin(c, Bin::Add, byte, k);
        V mask = s.m.i8(static_cast<std::uint8_t>((1u << n) - 1));
        V r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {hnd, byte, undef, mask, align});
        for (std::uint32_t q = 0; q < n; ++q) {
            V fv = s.m.extract(c, r, q);
            out.push_back(as_int(fv));
        }
    }
    return out;
}

std::pair<V, V> Lower::bvh_address(V word0, V word1, V pointer, std::optional<V> pointer_high) {
    std::size_t c = cur;
    Ty i64t = s.m.int_(64);
    V zero = s.m.i32(0);
    V byte = s.m.i32(0xff);
    V base_high = s.m.bin(c, Bin::And, word1, byte);
    V base = join64(word0, base_high);
    V k8 = s.m.int_const(i64t, 8);
    base = s.m.bin(c, Bin::Shl, base, k8);
    V pv = join64(pointer, pointer_high ? *pointer_high : zero);
    V k3 = s.m.int_const(i64t, 3);
    V k6 = s.m.int_const(i64t, 6);
    pv = s.m.bin(c, Bin::LShr, pv, k3);
    pv = s.m.bin(c, Bin::Shl, pv, k6);
    V a = s.m.bin(c, Bin::Add, base, pv);
    s.flags |= dxil::flags::INT64;
    return split64(a);
}

std::array<V, 4> Lower::bvh_boxes(const NodeAt& at, V kind, const Ray& ray, bool sort) {
    std::size_t head = cur;
    std::vector<V> children = bvh_node_words(at, std::nullopt, 0, 4);
    V k5 = s.m.i32(5);
    V wide = s.m.cmp(head, pred::EQ, kind, k5);
    std::size_t wide_block = s.m.new_block();
    std::size_t half_block = s.m.new_block();
    std::size_t join = s.m.new_block();
    s.m.terminate(head, Term::cond_br(wide, wide_block, half_block));
    cur = wide_block;
    V k16 = s.m.i32(16);
    std::vector<V> words = bvh_node_words(at, std::nullopt, 16, 24);
    std::vector<V> floats_v;
    for (V w : words) {
        floats_v.push_back(as_float(w));
    }
    s.m.terminate(wide_block, Term::br(join));
    cur = half_block;
    std::vector<V> hwords = bvh_node_words(at, std::nullopt, 16, 12);
    Ty f32t = s.t.f32;
    std::vector<V> halves_v;
    for (std::size_t n = 0; n < 24; ++n) {
        V h = hwords[n / 2];
        if (n % 2 == 1) {
            h = s.m.bin(half_block, Bin::LShr, h, k16);
        }
        halves_v.push_back(s.op(half_block, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {h}));
    }
    s.m.terminate(half_block, Term::br(join));
    cur = join;
    std::vector<V> coords;
    for (std::size_t n = 0; n < 24; ++n) {
        V phi = s.m.phi(join, f32t);
        s.m.set_phi(phi, {{floats_v[n], wide_block}, {halves_v[n], half_block}});
        coords.push_back(phi);
    }
    V most = s.m.f32(std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max()));
    V least = s.m.f32(std::bit_cast<std::uint32_t>(-std::numeric_limits<float>::max()));
    V inverse[3];
    for (int a = 0; a < 3; ++a) {
        V t = binary(35, ray.inverse[a], least);
        inverse[a] = binary(36, t, most);
    }
    std::size_t c = join;
    V none = s.m.i32(NONE);
    V infinity = s.m.f32(0x7F800000u);
    V fzero = s.m.f32(0);
    std::array<V, 4> ids{none, none, none, none};
    std::array<V, 4> distances{infinity, infinity, infinity, infinity};
    for (std::size_t n = 0; n < 4; ++n) {
        const V* bx = &coords[6 * n];
        V near_v[3] = {fzero, fzero, fzero}, far_v[3] = {fzero, fzero, fzero};
        for (int a = 0; a < 3; ++a) {
            V low = s.m.bin(c, Bin::Sub, bx[a], ray.origin[a]);
            low = s.m.bin(c, Bin::Mul, low, inverse[a]);
            V high = s.m.bin(c, Bin::Sub, bx[3 + a], ray.origin[a]);
            high = s.m.bin(c, Bin::Mul, high, inverse[a]);
            near_v[a] = binary(36, low, high);
            far_v[a] = binary(35, low, high);
        }
        V enter = binary(35, near_v[0], near_v[1]);
        enter = binary(35, enter, near_v[2]);
        V leave = binary(36, far_v[0], far_v[1]);
        leave = binary(36, leave, far_v[2]);
        V from = binary(35, enter, fzero);
        V through = s.m.cmp(c, pred::OGE, leave, from);
        V before = s.m.cmp(c, pred::OLT, enter, ray.extent);
        V off = is_nan_f32(bx[0]);
        V on = s.m.boolean(true);
        on = s.m.bin(c, Bin::Xor, off, on);
        V hit = s.m.bin(c, Bin::And, through, before);
        hit = s.m.bin(c, Bin::And, hit, on);
        ids[n] = s.m.select(c, hit, children[n], none);
        distances[n] = s.m.select(c, hit, enter, infinity);
    }
    if (sort) {
        const std::pair<int, int> net[5] = {{0, 1}, {2, 3}, {0, 2}, {1, 3}, {1, 2}};
        for (const auto& [a, bq] : net) {
            V swap = s.m.cmp(c, pred::OLT, distances[bq], distances[a]);
            V da = distances[a], db = distances[bq];
            distances[a] = s.m.select(c, swap, db, da);
            distances[bq] = s.m.select(c, swap, da, db);
            V ia = ids[a], ib = ids[bq];
            ids[a] = s.m.select(c, swap, ib, ia);
            ids[bq] = s.m.select(c, swap, ia, ib);
        }
    }
    return ids;
}

std::array<V, 4> Lower::bvh_triangle(const NodeAt& at, V kind, const Ray& ray) {
    std::size_t c = cur;
    V id = bvh_node_words(at, std::nullopt, 60, 1)[0];
    V k2 = s.m.i32(2);
    V k3 = s.m.i32(3);
    V k12 = s.m.i32(12);
    V k15 = s.m.i32(15);
    V shift = s.m.bin(c, Bin::Shl, kind, k3);
    id = s.m.bin(c, Bin::LShr, id, shift);
    V iv = s.m.bin(c, Bin::And, id, k3);
    V jv = s.m.bin(c, Bin::LShr, id, k2);
    jv = s.m.bin(c, Bin::And, jv, k3);
    V first = s.m.bin(c, Bin::Sub, k3, iv);
    first = s.m.bin(c, Bin::Sub, first, jv);
    first = s.m.bin(c, Bin::And, first, k3);
    V k0 = s.m.i32(0);
    V k1 = s.m.i32(1);
    V maps[4];
    for (std::uint32_t n = 0; n < 4; ++n) {
        maps[n] = s.m.i32(vertex_map(n));
    }
    V is0 = s.m.cmp(c, pred::EQ, kind, k0);
    V is1 = s.m.cmp(c, pred::EQ, kind, k1);
    V is2 = s.m.cmp(c, pred::EQ, kind, k2);
    V map = s.m.select(c, is2, maps[2], maps[3]);
    map = s.m.select(c, is1, maps[1], map);
    map = s.m.select(c, is0, maps[0], map);
    std::vector<std::array<V, 3>> pts;
    for (V n : {first, iv, jv}) {
        V nibble = s.m.bin(c, Bin::Shl, n, k2);
        V vertex = s.m.bin(c, Bin::LShr, map, nibble);
        vertex = s.m.bin(c, Bin::And, vertex, k15);
        V byte = s.m.bin(c, Bin::Mul, vertex, k12);
        std::vector<V> words = bvh_node_words(at, byte, 0, 3);
        std::array<V, 3> r;
        for (int a = 0; a < 3; ++a) {
            V fv = as_float(words[a]);
            r[a] = s.m.bin(c, Bin::Sub, fv, ray.origin[a]);
        }
        pts.push_back(r);
    }
    V fzero = s.m.f32(0);
    V size[3];
    for (int a = 0; a < 3; ++a) {
        size[a] = unary(6, ray.direction[a]);
    }
    V xy = s.m.cmp(c, pred::OGE, size[0], size[1]);
    V xz = s.m.cmp(c, pred::OGE, size[0], size[2]);
    V yz = s.m.cmp(c, pred::OGE, size[1], size[2]);
    V z0 = s.m.bin(c, Bin::And, xy, xz);
    V on = s.m.boolean(true);
    V not_xy = s.m.bin(c, Bin::Xor, xy, on);
    V z1 = s.m.bin(c, Bin::And, not_xy, yz);
    auto pick = [&](const std::array<V, 3>& q, std::size_t n) {
        V rest = s.m.select(c, z1, q[(1 + n) % 3], q[(2 + n) % 3]);
        return s.m.select(c, z0, q[n % 3], rest);
    };
    std::array<V, 3> dir{ray.direction[0], ray.direction[1], ray.direction[2]};
    V dz = pick(dir, 0);
    V backwards = s.m.cmp(c, pred::OLT, dz, fzero);
    auto axes = [&](const std::array<V, 3>& q) {
        V px = pick(q, 1);
        V py = pick(q, 2);
        V a0 = s.m.select(c, backwards, py, px);
        V a1 = s.m.select(c, backwards, px, py);
        V a2 = pick(q, 0);
        return std::array<V, 3>{a0, a1, a2};
    };
    auto d = axes(dir);
    std::array<V, 3> inv{ray.inverse[0], ray.inverse[1], ray.inverse[2]};
    V sz = pick(inv, 0);
    V sx = s.m.bin(c, Bin::Mul, d[0], sz);
    V sy = s.m.bin(c, Bin::Mul, d[1], sz);
    std::vector<std::array<V, 3>> sheared;
    for (const auto& r0 : pts) {
        auto r = axes(r0);
        V xm = s.m.bin(c, Bin::Mul, sx, r[2]);
        V xx = s.m.bin(c, Bin::Sub, r[0], xm);
        V ym = s.m.bin(c, Bin::Mul, sy, r[2]);
        V yy = s.m.bin(c, Bin::Sub, r[1], ym);
        V zz = s.m.bin(c, Bin::Mul, sz, r[2]);
        sheared.push_back({xx, yy, zz});
    }
    V ax = sheared[0][0], ay = sheared[0][1], az = sheared[0][2];
    V bx = sheared[1][0], by = sheared[1][1], bz = sheared[1][2];
    V cx = sheared[2][0], cy = sheared[2][1], cz = sheared[2][2];
    auto cross = [&](V pp, V q, V r, V ss) {
        V a = s.m.bin(c, Bin::Mul, pp, q);
        V bb = s.m.bin(c, Bin::Mul, r, ss);
        return s.m.bin(c, Bin::Sub, a, bb);
    };
    V u = cross(cx, by, cy, bx);
    V v = cross(ax, cy, ay, cx);
    V w = cross(bx, ay, by, ax);
    auto any = [&](std::uint8_t pr) {
        V a = s.m.cmp(c, pr, u, fzero);
        V bb = s.m.cmp(c, pr, v, fzero);
        V dd = s.m.cmp(c, pr, w, fzero);
        a = s.m.bin(c, Bin::Or, a, bb);
        return s.m.bin(c, Bin::Or, a, dd);
    };
    V behind = any(pred::OLT);
    V before = any(pred::OGT);
    V outside = s.m.bin(c, Bin::And, behind, before);
    V inside = s.m.bin(c, Bin::Xor, outside, on);
    V det = s.m.bin(c, Bin::Add, v, w);
    det = s.m.bin(c, Bin::Add, u, det);
    V t = s.m.bin(c, Bin::Mul, u, az);
    V t2 = s.m.bin(c, Bin::Mul, v, bz);
    t = s.m.bin(c, Bin::Add, t, t2);
    t2 = s.m.bin(c, Bin::Mul, w, cz);
    t = s.m.bin(c, Bin::Add, t, t2);
    V front = s.m.cmp(c, pred::OGT, det, fzero);
    V front_ahead = s.m.cmp(c, pred::OGE, t, fzero);
    front = s.m.bin(c, Bin::And, front, front_ahead);
    V back = s.m.cmp(c, pred::OLT, det, fzero);
    V back_ahead = s.m.cmp(c, pred::OLE, t, fzero);
    back = s.m.bin(c, Bin::And, back, back_ahead);
    V ahead = s.m.bin(c, Bin::Or, front, back);
    V hit = s.m.bin(c, Bin::And, inside, ahead);
    V miss[4];
    for (int k = 0; k < 4; ++k) {
        miss[k] = s.m.f32(MISS_BITS[k]);
    }
    V out_v[4] = {t, det, v, w};
    std::array<V, 4> out;
    for (int n = 0; n < 4; ++n) {
        V fv = s.m.select(c, hit, out_v[n], miss[n]);
        out[n] = as_int(fv);
    }
    return out;
}

}   
