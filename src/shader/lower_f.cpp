// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// scalar memory through descriptors, descriptors read as data, the code's
// constant table, typed buffer formats, handles and heap indices. See
// lower_impl.hpp.
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "lower_impl.hpp"

namespace danzi::shader::lower {

namespace {

std::optional<std::uint32_t> load_count(std::string_view name, std::string_view prefix) {
    if (!name.starts_with(prefix)) {
        return std::nullopt;
    }
    auto rest = name.substr(prefix.size());
    if (rest.empty()) {
        return 1;
    }
    if (rest == "x2") {
        return 2;
    }
    if (rest == "x4") {
        return 4;
    }
    if (rest == "x8") {
        return 8;
    }
    if (rest == "x16") {
        return 16;
    }
    return std::nullopt;
}


struct CurGuard {
    std::size_t& cur;
    std::size_t was;
    ~CurGuard() { cur = was; }
};

}   

Why Lower::smem(const Inst& inst, Site site) {
    const auto& f = std::get<SmemFields>(inst.fields);
    auto name = inst.name();
    if (auto it = scalar.find(site); it != scalar.end() && it->second.param == resource::CODE) {
        code_smem(inst, it->second);
        return nullptr;
    }
    if (auto it = scalar.find(site); it != scalar.end() && it->second.param == resource::MEMORY) {
        memory_smem(inst);
        return nullptr;
    }
    if (!name.starts_with("s_buffer_load_dword")) {
        return descriptor_data(inst, site);
    }
    auto cnt = load_count(name, "s_buffer_load_dword");
    if (!cnt) {
        fail("a scalar buffer load");
    }
    std::uint32_t count = *cnt;
    if (!f.data || f.data->kind != Operand::Scalar) {
        fail("a scalar buffer load");
    }
    std::uint8_t code = f.data->a;
    auto dit = descriptors_res.find(site);
    if (dit == descriptors_res.end()) {
        fail("an untracked descriptor");
    }
    const Source& src = *dit->second;
    const UsedResource* kind = used_kind(src);
    std::size_t c = cur;
    auto [base, extra] = built_offset(src);
    std::int32_t offset = static_cast<std::int32_t>(static_cast<std::uint32_t>(f.offset) + base);
    V addr = s.m.i32(static_cast<std::uint32_t>(offset));
    if (extra) {
        addr = s.m.bin(c, Bin::Add, addr, *extra);
    }
    bool null_soffset = f.soffset.kind == Operand::Scalar && f.soffset.a == NULL_CODE;
    if (!null_soffset) {
        V so = operand(inst, f.soffset);
        addr = s.m.bin(c, Bin::Add, addr, so);
    }
    if (src.kind == Source::Built && src.address.param == resource::CODE) {
        for (std::uint32_t k = 0; k < count; ++k) {
            V off = s.m.i32(4 * k);
            V a = s.m.bin(c, Bin::Add, addr, off);
            V xv = code_dword(a);
            write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), xv, std::nullopt);
        }
        return nullptr;
    }
    std::optional<V> picked;
    if (!kind) {
        picked = slot_stride(src);
    }
    if (picked) {
        V stride = *picked;
        V zero = s.m.i32(0);
        V is_buffer = s.m.cmp(cur, pred::NE, stride, zero);
        auto at = begin_guarded(is_buffer);
        V h = handle(site, src, false, {11, 0});
        std::vector<std::pair<V, V>> raw;
        for (std::uint32_t k = 0; k < count; ++k) {
            std::size_t b = cur;
            V off = s.m.i32(4 * k);
            V a = s.m.bin(b, Bin::Add, addr, off);
            V mask = s.m.i8(1);
            V u = s.m.undef(s.t.i32);
            V al = s.m.i32(4);
            V r = s.op(b, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {h, a, u, mask, al});
            V xv = s.m.extract(b, r, 0);
            xv = as_int(xv);
            raw.emplace_back(xv, zero);
        }
        std::vector<V> rawv = end_guarded(at, raw);
        V other = s.m.cmp(cur, pred::EQ, stride, zero);
        auto at2 = begin_guarded(other);
        V h2 = handle(site, src, false, {13, 0x10000});
        std::vector<std::pair<V, V>> both;
        for (std::size_t k = 0; k < rawv.size(); ++k) {
            std::size_t b = cur;
            V off = s.m.i32(static_cast<std::uint32_t>(4 * k));
            V a = s.m.bin(b, Bin::Add, addr, off);
            V xv = cbuffer_dword(h2, a);
            both.emplace_back(xv, rawv[k]);
        }
        std::vector<V> bothv = end_guarded(at2, both);
        for (std::size_t k = 0; k < bothv.size(); ++k) {
            write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), bothv[k], std::nullopt);
        }
        return nullptr;
    }
    if (kind && kind->kind == 0) {
        V h = handle(site, src, false, {11, 0});
        for (std::uint32_t k = 0; k < count; ++k) {
            V off = s.m.i32(4 * k);
            V a = s.m.bin(c, Bin::Add, addr, off);
            V mask = s.m.i8(1);
            V u = s.m.undef(s.t.i32);
            V al = s.m.i32(4);
            V r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {h, a, u, mask, al});
            V xv = s.m.extract(c, r, 0);
            xv = as_int(xv);
            write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), xv, std::nullopt);
        }
        return nullptr;
    }
    V h = handle(site, src, false, {13, 0x10000});
    std::optional<std::uint32_t> constant;
    if (addr.is(V::Const) && null_soffset && !extra) {
        constant = static_cast<std::uint32_t>(offset);
    }
    std::unordered_map<std::uint32_t, V> rows;
    for (std::uint32_t k = 0; k < count; ++k) {
        V xv;
        if (constant) {
            std::uint32_t byte = *constant + 4 * k;
            V row;
            if (auto it = rows.find(byte >> 4); it != rows.end()) {
                row = it->second;
            } else {
                V ri = s.m.i32(byte >> 4);
                row = s.op(c, "cbufferLoadLegacy.i32", Attr::ReadOnly, s.t.cbuf_ret, 59, {h, ri});
                rows[byte >> 4] = row;
            }
            xv = s.m.extract(c, row, (byte >> 2) & 3);
        } else {
            V off = s.m.i32(4 * k);
            V a = s.m.bin(c, Bin::Add, addr, off);
            xv = cbuffer_dword(h, a);
        }
        write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), xv, std::nullopt);
    }
    return nullptr;
}

const Lower::LoadUses& Lower::load_uses() {
    if (!load_uses_) {
        LoadUses& lu = load_uses_.emplace();
        for (const auto& d : x.res->descriptors) {
            if (d.role == resource::Role::Resource) {

                std::vector<Site> seen;
                for (const Source* l : d.source.leaves()) {
                    if (l->kind == Source::Loaded && std::find(seen.begin(), seen.end(), l->load) == seen.end()) {
                        seen.push_back(l->load);
                        lu.resource[l->load].emplace_back(d.site, l->first);
                    }
                }
            } else if (d.role == resource::Role::Sampler) {
                for (const Source* l : d.source.leaves()) {
                    if (l->kind == Source::Loaded) {
                        lu.sampler.insert(l->load);
                    }
                }
            }
        }
    }
    return *load_uses_;
}

Why Lower::descriptor_data(const Inst& inst, Site site) {
    static constexpr Why OTHER = "a descriptor's address or format read as data";
    const auto& f = std::get<SmemFields>(inst.fields);
    if (!f.data || f.data->kind != Operand::Scalar) {
        fail("a scalar memory read as data");
    }
    std::uint8_t code = f.data->a;
    auto cnt = load_count(inst.name(), "s_load_dword");
    if (!cnt) {
        fail("a scalar memory read as data");
    }
    std::uint32_t count = *cnt;
    const ir::Program& prog = p();
    static const std::vector<std::pair<Site, std::uint8_t>> NONE;
    const LoadUses& lu = load_uses();
    auto uses = [&](Site of) -> const std::vector<std::pair<Site, std::uint8_t>>& {
        auto it = lu.resource.find(of);
        return it != lu.resource.end() ? it->second : NONE;
    };
    auto by_scalar_load = [&](const std::pair<Site, std::uint8_t>& u) {
        return std::holds_alternative<SmemFields>(prog.blocks[u.first.first].insts[u.first.second].fields);
    };
    auto use_of = [&](Site of) -> std::optional<std::pair<Site, std::uint8_t>> {
        const auto& us = uses(of);
        for (const auto& u : us) {
            if (!by_scalar_load(u)) {
                return u;
            }
        }
        if (!us.empty()) {
            return us[0];
        }
        return std::nullopt;
    };
    std::optional<Address> address;
    if (auto it = scalar.find(site); it != scalar.end()) {
        address = it->second;
    }
    auto twin = [&]() -> std::optional<std::pair<Site, std::uint8_t>> {
        if (!address) {
            return std::nullopt;
        }
        for (const auto& [sx, bx] : x.res->scalar) {
            if (sx != site && bx == *address && prog.blocks[sx.first].insts[sx.second].name() == inst.name()) {
                if (auto u = use_of(sx)) {
                    return u;
                }
            }
        }
        return std::nullopt;
    };
    auto use_at = use_of(site);
    if (!use_at) {
        use_at = twin();
    }
    if (!use_at) {
        const auto& lc = x.res->lane_copied;
        if (std::find(lc.begin(), lc.end(), site) != lc.end()) {
            if (auto it = scalar.find(site); it != scalar.end()) {
                Address a = it->second;
                for (std::uint32_t k = 0; k < count; ++k) {
                    V xv = s.m.i32(a.offset / 4 + k);
                    write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), xv, std::nullopt);
                }
                return nullptr;
            }
        }
        bool sampler = lu.sampler.contains(site);
        if (sampler) {
            for (const auto& w : inst.writes) {
                stale.insert(w.second);
            }
            return "a sampler's descriptor read as data";
        }
        if (count <= 4) {
            if (auto va = view_address(site)) {
                for (std::uint32_t k = 0; k < count; ++k) {
                    Reg reg = Reg::s(static_cast<std::uint8_t>(code + k));
                    if (k == 0) {
                        write(inst, reg, va->first, std::nullopt);
                    } else if (k == 1) {
                        write(inst, reg, va->second, std::nullopt);
                    } else if (auto v = inst.write(reg)) {
                        vals[*v] = Val::missing(OTHER);
                        stale.insert(*v);
                    }
                }
                return nullptr;
            }
        }
        char* env = nullptr;
        std::size_t n = 0;
        if (_dupenv_s(&env, &n, "VICTORY_DESC_WHY") == 0 && env) {
            std::fprintf(stderr, "DESC-WHY %#zx %.*s lane_copied=%s\n", inst.machine.offset, static_cast<int>(inst.name().size()), inst.name().data(),
                         std::find(lc.begin(), lc.end(), site) != lc.end() ? "true" : "false");
        }
        std::free(env);
        return "a descriptor read only as data";
    }
    auto [at, first] = *use_at;
    Source src;
    src.kind = Source::Loaded;
    src.load = site;
    src.first = first;
    const UsedResource* used = used_kind(src);
    const Inst& ui = prog.blocks[at.first].insts[at.second];
    auto un = ui.name();
    bool uav = un.find("store") != std::string_view::npos || un.find("atomic") != std::string_view::npos || (used && used->kind == 3);
    std::uint32_t u = uav ? 0x1000 : 0;
    std::pair<std::uint32_t, std::uint32_t> props;
    std::optional<std::uint32_t> image;
    if (auto* imf = std::get_if<ImageFields>(&ui.fields)) {
        if (un.starts_with("image_bvh")) {
            props = {11, 0};
        } else {
            std::uint32_t kind;
            switch (imf->dim) {
                case 0: kind = 1; break;
                case 1: kind = 2; break;
                case 2: kind = 4; break;
                case 4: kind = 6; break;
                case 5: kind = 7; break;
                default: return "an image dimension (cube or MSAA)";
            }
            props = {kind | u, 0x409};
            image = kind;
        }
    } else if (auto* bf = std::get_if<BufferFields>(&ui.fields)) {
        std::uint32_t stride = used ? used->stride : 0;
        if (un.starts_with("buffer_load_format") || un.starts_with("buffer_store_format")) {
            props = {10 | u, 0x409};
        } else if (bf->idxen && !bf->format && stride >= 4 && stride % 4 == 0) {
            props = {12 | u, stride};
        } else {
            props = {11 | u, 0};
        }
        if (used && used->kind == 2) {
            for (const auto& w : inst.writes) {
                stale.insert(w.second);
            }
            return "a constant buffer's descriptor read as data";
        }
    } else if (std::holds_alternative<SmemFields>(ui.fields) && used && used->kind == 0) {
        props = {11, 0};
    } else if (std::holds_alternative<SmemFields>(ui.fields)) {
        for (const auto& w : inst.writes) {
            stale.insert(w.second);
        }
        return "a constant buffer's descriptor read as data";
    } else {
        fail("a scalar memory read as data");
    }
    V h = handle(site, src, false, props);
    std::size_t c = cur;
    Ty i32t = s.t.i32;
    Ty dims = s.m.named_struct("dx.types.Dimensions", {i32t, i32t, i32t, i32t});
    V mip = image && !uav ? s.m.i32(0) : s.m.undef(i32t);
    V d = s.op(c, "getDimensions", Attr::ReadOnly, dims, 72, {h, mip});
    auto field = [&](std::uint32_t k) { return s.m.extract(c, d, k); };
    std::vector<std::pair<std::uint32_t, V>> words;
    if (image) {
        std::uint32_t kind = *image;
        V one = s.m.i32(1);
        V w = field(0);
        V height, depth;
        switch (kind) {
            case 1:
                height = one;
                depth = one;
                break;
            case 2:
                height = field(1);
                depth = one;
                break;
            case 6:
                height = one;
                depth = field(1);
                break;
            default:
                height = field(1);
                depth = field(2);
                break;
        }
        V levels = uav ? one : field(3);
        auto less = [&](V v) { return s.m.bin(c, Bin::Sub, v, one); };
        w = less(w);
        height = less(height);
        depth = less(depth);
        levels = less(levels);
        V k30 = s.m.i32(30);
        V k2 = s.m.i32(2);
        V k14 = s.m.i32(14);
        V k16 = s.m.i32(16);
        V mask = s.m.i32(0x3FFF);
        V d1 = s.m.bin(c, Bin::Shl, w, k30);
        V hi = s.m.bin(c, Bin::LShr, w, k2);
        hi = s.m.bin(c, Bin::And, hi, mask);
        V ht = s.m.bin(c, Bin::Shl, height, k14);
        V d2 = s.m.bin(c, Bin::Or, hi, ht);
        static const std::uint32_t TY[] = {0, 8, 9, 0, 10, 0, 12, 13};
        V base = s.m.i32(0xFAC | TY[kind] << 28);
        V last = s.m.bin(c, Bin::Shl, levels, k16);
        V d3 = s.m.bin(c, Bin::Or, base, last);
        words = {{1, d1}, {2, d2}, {3, d3}, {4, depth}};
    } else {
        V n = field(0);
        std::uint32_t stride = used ? used->stride : 0;
        if ((props.first & 0xFF) == 11 && stride > 1) {
            V k = s.m.i32(stride);
            n = s.m.bin(c, Bin::UDiv, n, k);
        }
        words = {{2, n}};
        bool computed = false;
        for (std::uint8_t k = 0; k < 2 && !computed; ++k) {
            auto v = inst.write(Reg::s(static_cast<std::uint8_t>(code + first + k)));
            computed = v && computed_with(*v, 6);
        }
        if (first == 0 && computed) {
            if (auto va = view_address(site)) {
                words.emplace_back(0, va->first);
                words.emplace_back(1, va->second);
            }
        }
    }
    for (std::uint32_t k = 0; k < count; ++k) {
        std::uint32_t rel = k - first;
        auto it = std::find_if(words.begin(), words.end(), [&](const auto& w) { return w.first == rel; });
        Reg reg = Reg::s(static_cast<std::uint8_t>(code + k));
        if (it != words.end()) {
            write(inst, reg, it->second, std::nullopt);
        } else if (auto v = inst.write(reg)) {
            vals[*v] = Val::missing(OTHER);
            stale.insert(*v);
        }
    }
    return nullptr;
}

void Lower::code_smem(const Inst& inst, Address a) {
    const auto& f = std::get<SmemFields>(inst.fields);
    if (!f.data || f.data->kind != Operand::Scalar) {
        fail("a scalar load");
    }
    std::uint8_t code = f.data->a;
    auto cnt = load_count(inst.name(), "s_load_dword");
    if (!cnt) {
        fail("a scalar load from the code");
    }
    std::size_t c = cur;
    V addr = s.m.i32(a.offset);
    if (a.dynamic) {
        Value value = *a.dynamic;
        V d = i32_of(value);
        bool per_lane = false;
        if (auto it = def_block.find(value); it != def_block.end()) {
            for (const Inst& i : p().blocks[it->second].insts) {
                for (const auto& w : i.writes) {
                    if (w.second == value && w.first.kind == Reg::V) {
                        per_lane = true;
                    }
                }
            }
        }
        if (per_lane) {
            s.flags |= dxil::flags::WAVE_OPS;
            d = s.op(c, "waveReadLaneFirst.i32", Attr::NoUnwind, s.t.i32, 118, {d});
        }
        addr = s.m.bin(c, Bin::Add, addr, d);
    }
    for (std::uint32_t k = 0; k < *cnt; ++k) {
        V off = s.m.i32(4 * k);
        V at = s.m.bin(c, Bin::Add, addr, off);
        V xv = code_dword(at);
        write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), xv, std::nullopt);
    }
}

V Lower::cbuffer_dword(V h, V a) {
    std::size_t c = cur;
    V four = s.m.i32(4);
    V ri = s.m.bin(c, Bin::LShr, a, four);
    V r = s.op(c, "cbufferLoadLegacy.i32", Attr::ReadOnly, s.t.cbuf_ret, 59, {h, ri});
    V two = s.m.i32(2);
    V three = s.m.i32(3);
    V comp = s.m.bin(c, Bin::LShr, a, two);
    comp = s.m.bin(c, Bin::And, comp, three);
    V xv = s.m.extract(c, r, 0);
    for (std::uint32_t j = 1; j < 4; ++j) {
        V e = s.m.extract(c, r, j);
        V jc = s.m.i32(j);
        V is = s.m.cmp(c, pred::EQ, comp, jc);
        xv = s.m.select(c, is, e, xv);
    }
    return xv;
}

std::vector<V> Lower::format_load(V h, V off, std::uint8_t format, std::uint32_t channels) {
    auto bf = buffer_format(format);
    if (!bf) {
        fail("a buffer format");
    }
    const std::vector<std::uint32_t>& bits = *bf->first;
    Numeric numeric = bf->second;
    std::size_t c = cur;
    std::uint32_t total = 0;
    for (std::uint32_t b : bits) {
        total += b;
    }
    V u = s.m.undef(s.t.i32);
    V align = s.m.i32(4);
    auto load = [&](V at, std::uint32_t n) {
        V mask = s.m.i8(static_cast<std::uint8_t>((1u << n) - 1));
        V r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {h, at, u, mask, align});
        std::vector<V> out;
        for (std::uint32_t k = 0; k < n; ++k) {
            V xv = s.m.extract(c, r, k);
            out.push_back(as_int(xv));
        }
        return out;
    };
    std::vector<V> words;
    if (total < 32) {
        V not3 = s.m.i32(~3u);
        V three = s.m.i32(3);
        V at = s.m.bin(c, Bin::And, off, not3);
        V w = load(at, 1)[0];
        V byte = s.m.bin(c, Bin::And, off, three);
        V sh = s.m.bin(c, Bin::Shl, byte, three);
        words.push_back(s.m.bin(c, Bin::LShr, w, sh));
    } else {
        words = load(off, total / 32);
    }
    std::vector<V> out;
    std::uint32_t at = 0;
    for (std::uint32_t w : bits) {
        V word = words[at / 32];
        V field;
        if (w == 32) {
            field = word;
        } else {
            V sh = s.m.i32(at % 32);
            V t = s.m.bin(c, Bin::LShr, word, sh);
            V m = s.m.i32((1u << w) - 1);
            field = s.m.bin(c, Bin::And, t, m);
        }
        at += w;
        auto sgn = [&]() -> V {
            if (w == 32) {
                return field;
            }
            V k = s.m.i32(32 - w);
            V t = s.m.bin(c, Bin::Shl, field, k);
            return s.m.bin(c, Bin::AShr, t, k);
        };
        Ty f32t = s.t.f32;
        V xv;
        switch (numeric) {
            case Numeric::Unorm: {
                V f = s.m.cast(c, Cast::UiToFp, field, f32t);
                V max = s.m.f32(std::bit_cast<std::uint32_t>(static_cast<float>((std::uint64_t{1} << w) - 1)));
                V q = s.m.bin(c, Bin::SDiv, f, max);
                xv = as_int(q);
                break;
            }
            case Numeric::Snorm: {
                V sv = sgn();
                V f = s.m.cast(c, Cast::SiToFp, sv, f32t);
                V max = s.m.f32(std::bit_cast<std::uint32_t>(static_cast<float>((std::uint64_t{1} << (w - 1)) - 1)));
                V q = s.m.bin(c, Bin::SDiv, f, max);
                V lo = s.m.f32(std::bit_cast<std::uint32_t>(-1.0f));
                q = binary(35, q, lo);
                xv = as_int(q);
                break;
            }
            case Numeric::Uscaled: {
                V f = s.m.cast(c, Cast::UiToFp, field, f32t);
                xv = as_int(f);
                break;
            }
            case Numeric::Sscaled: {
                V sv = sgn();
                V f = s.m.cast(c, Cast::SiToFp, sv, f32t);
                xv = as_int(f);
                break;
            }
            case Numeric::Uint: xv = field; break;
            case Numeric::Sint: xv = sgn(); break;
            case Numeric::Float: {
                if (w == 32) {
                    xv = field;
                    break;
                }
                V hv = field;
                if (std::uint32_t k = half_shift(w); k != 0) {
                    V sh = s.m.i32(k);
                    hv = s.m.bin(c, Bin::Shl, field, sh);
                }
                V f = s.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {hv});
                xv = as_int(f);
                break;
            }
        }
        out.push_back(xv);
    }
    std::uint32_t one = (numeric == Numeric::Uint || numeric == Numeric::Sint) ? 1 : std::bit_cast<std::uint32_t>(1.0f);
    for (auto k = static_cast<std::uint32_t>(out.size()); k < channels; ++k) {
        out.push_back(s.m.i32(k == 3 ? one : 0));
    }
    out.resize(std::min<std::size_t>(out.size(), channels));
    return out;
}

std::pair<std::uint32_t, std::optional<V>> Lower::built_offset(const Source& src) {
    if (src.kind != Source::Built) {
        return {0, std::nullopt};
    }
    std::optional<V> xv;
    if (src.address.dynamic) {
        xv = i32_of(*src.address.dynamic);
    }
    if (auto bias = root_bias(src.address.param)) {
        xv = xv ? s.m.bin(cur, Bin::Add, *xv, *bias) : *bias;
    }
    return {src.address.offset, xv};
}

std::optional<V> Lower::root_bias(std::size_t param) {
    if (param >= x.native->layout.size()) {
        return std::nullopt;
    }
    const RootParam& rp = x.native->layout[param];
    if (rp.root != 1) {
        return std::nullopt;
    }
    std::optional<std::uint8_t> kind;
    for (const UsedResource& u : x.native->used) {
        if (u.user_data == rp.user_data) {
            kind = u.kind;
            break;
        }
    }
    bool biased = kind == std::optional<std::uint8_t>(std::uint8_t{3}) || (kind == std::optional<std::uint8_t>(std::uint8_t{0}) && is_spilled(*x.native));
    if (!biased) {
        return std::nullopt;
    }
    return host_word(dxil::USER_VIEW_BIAS, rp.slot, 0);
}

const UsedResource* Lower::used_kind(const Source& src) const {
    if (src.kind == Source::Phi || src.kind == Source::Select || src.kind == Source::Lanes) {
        if (src.args.empty()) {
            return nullptr;
        }
        return used_kind(src.args[0]);
    }
    if (src.kind == Source::LanePhi) {
        for (std::size_t i = 0; i < src.args.size(); ++i) {
            if (src.present[i]) {
                if (const UsedResource* u = used_kind(src.args[i])) {
                    return u;
                }
            }
        }
        return nullptr;
    }
    if (src.kind == Source::Built) {
        if (src.address.param >= x.native->layout.size()) {
            return nullptr;
        }
        const RootParam& rp = x.native->layout[src.address.param];
        for (const UsedResource& u : x.native->used) {
            if (u.root == 1 && u.user_data == rp.user_data) {
                return &u;
            }
        }
        return nullptr;
    }
    if (src.kind != Source::Loaded && src.kind != Source::Fetched) {
        return nullptr;
    }
    auto it = scalar.find(src.load);
    if (it == scalar.end()) {
        return nullptr;
    }
    const Address& a = it->second;
    if (a.param >= x.native->layout.size()) {
        return nullptr;
    }
    const RootParam& rp = x.native->layout[a.param];
    if (rp.root != 3) {
        return nullptr;
    }
    std::uint32_t dword = a.offset / 4 + src.first;
    const UsedResource* best = nullptr;
    for (const UsedResource& u : x.native->used) {
        if (u.root == 3 && u.user_data == rp.user_data && u.table_offset_dwords <= dword) {
            if (!best || u.table_offset_dwords >= best->table_offset_dwords) {
                best = &u;
            }
        }
    }
    return best;
}

V Lower::handle(Site site, const Source& src, bool sampler, std::pair<std::uint32_t, std::uint32_t> props) {
    (void)site;
    switch (src.kind) {
        case Source::Loaded: {
            HandleKey key{cur, src.load, src.first, sampler, props};
            if (auto it = handles.find(key); it != handles.end()) {
                return it->second;
            }
            V index = heap_index(src.load, src.first, sampler);
            const auto& loads = x.res->waterfall.loads;
            bool per_lane = std::find(loads.begin(), loads.end(), src.load) != loads.end();
            V h = s.heap_handle(cur, index, sampler, per_lane, props);
            handles[key] = h;
            return h;
        }
        case Source::Constant: {
            if (!sampler) {
                fail("a constant resource descriptor");
            }
            if (src.consts.size() < 4) {
                fail("a sampler constant");
            }
            std::array<std::uint32_t, 4> words{src.consts[0], src.consts[1], src.consts[2], src.consts[3]};
            std::size_t n;
            auto it = std::find(samplers.begin(), samplers.end(), words);
            if (it != samplers.end()) {
                n = static_cast<std::size_t>(it - samplers.begin());
            } else {
                samplers.push_back(words);
                n = samplers.size() - 1;
            }
            if (props.first & 0x8000) {
                s.cmp_samplers |= std::uint64_t{1} << n;
            }
            return s.sampler_handle(cur, static_cast<std::uint32_t>(n));
        }
        case Source::Built: {
            if (src.address.param >= x.native->layout.size()) {
                fail("a descriptor built around a table in the code");
            }
            const RootParam& rp = x.native->layout[src.address.param];
            std::optional<std::uint8_t> kind;
            for (const UsedResource& u : x.native->used) {
                if (u.user_data == rp.user_data) {
                    kind = u.kind;
                    break;
                }
            }
            if (rp.root == 1) {
                if ((kind == std::optional<std::uint8_t>(std::uint8_t{2}) || !kind) && props.first == 13) {
                    return s.root_cbv_handle(rp.slot);
                }
                if (kind == std::optional<std::uint8_t>(std::uint8_t{0}) && props.first == 11) {
                    return s.root_srv_handle(rp.slot);
                }
                if (kind == std::optional<std::uint8_t>(std::uint8_t{3}) && (props.first == 11 || props.first == 0x100b)) {
                    return s.root_uav_handle(rp.slot);
                }
                if (kind == std::optional<std::uint8_t>(std::uint8_t{0}) && props.first == 0x100b) {
                    fail("a write through a root SRV");
                }
            }
            fail("a typed root view");
        }
        case Source::Phi: {
            V phi = descriptor_phi(src.block, src.args, sampler, 8);
            return s.heap_handle(cur, phi, sampler, false, props);
        }
        case Source::Select: {
            if (src.args.size() != 2 || src.args[0].kind != Source::Loaded || src.args[1].kind != Source::Loaded) {
                fail("a descriptor chosen by a condition");
            }
            Val g = get(src.value);
            if (g.kind != Val::Bit) {
                fail("SCC without a value");
            }
            V set = heap_index(src.args[0].load, src.args[0].first, sampler);
            V clear = heap_index(src.args[1].load, src.args[1].first, sampler);
            V index = s.m.select(cur, g.v, set, clear);
            return s.heap_handle(cur, index, sampler, false, props);
        }
        default: {
            V index = lane_heap_index(src, sampler);
            return s.heap_handle(cur, index, sampler, true, props);
        }
    }
}

V Lower::descriptor_phi(std::size_t blk, const std::vector<Source>& args, bool sampler, std::uint32_t depth) {
    static constexpr Why WHY = "a descriptor chosen by control flow";
    const ir::Program& prog = p();
    std::vector<std::pair<V, std::size_t>> incoming;
    const auto& preds = prog.blocks[blk].preds;
    for (std::size_t k = 0; k < preds.size() && k < args.size(); ++k) {
        std::size_t q = preds[k];
        const Source& arg = args[k];
        if (!block[q]) {
            continue;
        }
        if (!tail[q]) {
            fail(WHY);
        }
        std::size_t at = *tail[q];
        if (auto it = edges.find({q, blk}); it != edges.end()) {
            at = it->second;
        }
        V index;
        if (arg.kind == Source::Loaded) {
            CurGuard cg{cur, cur};
            cur = at;
            index = heap_index(arg.load, arg.first, sampler);
        } else if (arg.kind == Source::Phi && depth > 0) {
            index = descriptor_phi(arg.block, arg.args, sampler, depth - 1);
        } else {
            fail(WHY);
        }
        incoming.emplace_back(index, at);
    }
    if (!block[blk]) {
        fail(WHY);
    }
    V phi = s.m.phi(*block[blk], s.t.i32);
    s.m.set_phi(phi, std::move(incoming));
    return phi;
}

std::optional<V> Lower::slot_stride(const Source& src) {
    if (src.kind != Source::Loaded) {
        return std::nullopt;
    }
    auto it = scalar.find(src.load);
    bool heap = it != scalar.end() && it->second.param < x.native->layout.size() && x.native->layout[it->second.param].root == 5;
    if (!heap) {
        return std::nullopt;
    }
    StrideKey key{cur, src.load, src.first};
    if (auto sit = strides.find(key); sit != strides.end()) {
        return sit->second;
    }
    std::size_t c = cur;
    V slot = heap_index(src.load, src.first, false);
    V last = s.m.i32(dxil::SLOT_STRIDES_COUNT - 1);
    slot = s.op(c, "binary.i32", Attr::ReadNone, s.t.i32, 40, {slot, last});
    s.m.tag(slot, Tag::Clamp);
    V two = s.m.i32(2);
    V byte = s.m.bin(c, Bin::Shl, slot, two);
    V h = s.root_srv_handle(dxil::SLOT_STRIDES);
    V iu = s.m.undef(s.t.i32);
    V mask = s.m.i8(1);
    V align = s.m.i32(4);
    V r = s.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s.t.res_ret, 139, {h, byte, iu, mask, align});
    V f = s.m.extract(c, r, 0);
    V v = as_int(f);
    strides[key] = v;
    return v;
}

V Lower::lane_heap_index(const Source& src, bool sampler) {
    switch (src.kind) {
        case Source::Loaded: return heap_index(src.load, src.first, sampler);
        case Source::Fetched: return fetched_index(src.load, src.first, sampler);
        case Source::Lanes:
        case Source::Select: {
            if (src.args.size() != 2) {
                fail("a descriptor chosen per lane");
            }
            V cond;
            if (src.kind == Source::Lanes) {
                auto it = st().masks.find(src.value);
                if (it == st().masks.end()) {
                    fail("a descriptor chosen per lane by an untracked mask");
                }
                cond = boolean(it->second, cur);
            } else {
                Val g = get(src.value);
                if (g.kind != Val::Bit) {
                    fail("SCC without a value");
                }
                cond = g.v;
            }
            V set = lane_heap_index(src.args[0], sampler);
            V clear = lane_heap_index(src.args[1], sampler);
            return s.m.select(cur, cond, set, clear);
        }
        case Source::LanePhi: {
            if (!block[src.block]) {
                fail("a per-lane descriptor chosen by control flow");
            }
            std::size_t join = *block[src.block];
            std::vector<std::pair<V, std::size_t>> incoming;
            const auto& preds = p().blocks[src.block].preds;
            for (std::size_t k = 0; k < preds.size() && k < src.args.size(); ++k) {
                std::size_t q = preds[k];
                if (!block[q]) {
                    continue;
                }
                if (!tail[q]) {
                    fail("a per-lane descriptor joined from a later block");
                }
                std::size_t at = *tail[q];
                if (auto it = edges.find({q, src.block}); it != edges.end()) {
                    at = it->second;
                }
                V index;
                if (src.present[k]) {
                    CurGuard cg{cur, cur};
                    cur = at;
                    index = lane_heap_index(src.args[k], sampler);
                } else {
                    index = s.m.i32(0);
                }
                incoming.emplace_back(index, at);
            }
            V phi = s.m.phi(join, s.t.i32);
            s.m.set_phi(phi, std::move(incoming));
            return phi;
        }
        default: fail("a per-lane descriptor from neither a heap load nor a choice of them");
    }
}

V Lower::fetched_index(Site load, std::uint8_t first, bool sampler) {
    static constexpr Why WHY = "a per-lane descriptor load";
    auto it = scalar.find(load);
    if (it == scalar.end()) {
        fail("an untracked descriptor load");
    }
    Address a = it->second;
    if (a.param >= x.native->layout.size()) {
        fail("a descriptor outside the heaps");
    }
    std::uint32_t root = x.native->layout[a.param].root;
    if (!(root == 3 || root == 5 || root == 6)) {
        fail("a descriptor outside the heaps");
    }
    const Inst& li = p().blocks[load.first].insts[load.second];
    auto* f = std::get_if<FlatFields>(&li.fields);
    if (!f || !f->addr || f->addr->kind != Operand::Vector) {
        fail(WHY);
    }
    std::size_t c = cur;
    auto av = li.read(Reg::v(f->addr->a));
    if (!av) {
        fail(WHY);
    }
    V at = i32_of(*av);
    if (f->saddr && f->saddr->kind == Operand::Scalar && f->saddr->a != NULL_CODE) {
        auto bv = li.read(Reg::s(f->saddr->a));
        if (!bv) {
            fail(WHY);
        }
        V base = i32_of(*bv);
        at = s.m.bin(c, Bin::Add, base, at);
    }
    V k = s.m.i32(static_cast<std::uint32_t>(static_cast<std::int32_t>(f->offset)) + 4u * first);
    at = s.m.bin(c, Bin::Add, at, k);
    V shift = s.m.i32(sampler ? 4 : 5);
    return s.m.bin(c, Bin::LShr, at, shift);
}

V Lower::heap_index(Site load, std::uint8_t first, bool sampler) {
    auto it = scalar.find(load);
    if (it == scalar.end()) {
        fail("an untracked descriptor load");
    }
    Address a = it->second;
    if (a.param >= x.native->layout.size()) {
        fail("a descriptor outside the heaps");
    }
    std::uint32_t root = x.native->layout[a.param].root;
    if (!(root == 3 || root == 5 || root == 6)) {
        fail("a descriptor outside the heaps");
    }
    const Inst& li = p().blocks[load.first].insts[load.second];
    auto* f = std::get_if<SmemFields>(&li.fields);
    if (!f || f->base.kind != Operand::Scalar) {
        fail("a descriptor load");
    }
    std::size_t c = cur;
    auto base = li.read(Reg::s(f->base.a));
    if (!base) {
        fail("a descriptor load");
    }
    V addr = i32_of(*base);
    V k = s.m.i32(static_cast<std::uint32_t>(f->offset) + 4u * first);
    addr = s.m.bin(c, Bin::Add, addr, k);
    if (!(f->soffset.kind == Operand::Scalar && f->soffset.a == NULL_CODE)) {
        V so = operand(li, f->soffset);
        addr = s.m.bin(c, Bin::Add, addr, so);
    }
    V shift = s.m.i32(sampler ? 4 : 5);
    return s.m.bin(c, Bin::LShr, addr, shift);
}

}   
