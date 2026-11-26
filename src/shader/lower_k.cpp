// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering (`emit/lower.rs`; DanziLoader / victory-perf, see NOTICE.md):
// image instructions (atomics, stores, samples, gathers, loads and size
// queries) and cube directions. See lower_impl.hpp.
#include <algorithm>
#include <bit>
#include <numbers>

#include "lower_impl.hpp"
#include "stand_in.hpp"

namespace danzi::shader::lower {

namespace {
std::uint32_t fb(float f) { return std::bit_cast<std::uint32_t>(f); }
}   

void Lower::image(const Inst& inst, std::size_t b, std::size_t i) {
    const auto& f = std::get<ImageFields>(inst.fields);
    if (f.tfe || f.lwe) {
        fail("an image access with tfe or lwe");
    }
    auto name = inst.name();
    std::uint8_t dmask = f.dmask == 0 ? 1 : f.dmask;
    Site site{b, i};
    std::optional<V> g = guard(b, i);
    std::size_t c = cur;
    std::uint32_t kind;
    std::size_t coords;
    switch (f.dim) {
        case 0:
            kind = 1;
            coords = 1;
            break;
        case 1:
            kind = 2;
            coords = 2;
            break;
        case 2:
            kind = 4;
            coords = 3;
            break;
        case 3:
            if (name.starts_with("image_sample") || name.starts_with("image_gather") || name == "image_get_resinfo") {
                kind = 5;
                coords = 3;
                break;
            }
            fail("an image dimension (cube loads or MSAA)");
        case 4:
            kind = 6;
            coords = 2;
            break;
        case 5:
            kind = 7;
            coords = 3;
            break;
        default: fail("an image dimension (cube loads or MSAA)");
    }
    std::vector<std::uint8_t> regs;
    for (const Operand& a : f.addr) {
        if (a.kind == Operand::Vector) {
            for (std::uint8_t k = 0; k < a.b; ++k) {
                regs.push_back(static_cast<std::uint8_t>(a.a + k));
            }
        }
    }
    auto addr_u32 = [&](std::size_t reg_idx) -> V {
        if (reg_idx >= regs.size()) {
            fail("an image address");
        }
        auto v = inst.read(Reg::v(regs[reg_idx]));
        if (!v) {
            fail("an image address");
        }
        return i32_of(*v);
    };
    auto addr_packed = [&](std::size_t base_reg, std::size_t k) -> V {
        V xv = addr_u32(base_reg + (f.a16 ? k / 2 : k));
        if (!f.a16) {
            return xv;
        }
        std::size_t cc = cur;
        V sh = s.m.i32(16 * static_cast<std::uint32_t>(k % 2));
        V mask = s.m.i32(0xFFFF);
        xv = s.m.bin(cc, Bin::LShr, xv, sh);
        return s.m.bin(cc, Bin::And, xv, mask);
    };
    auto addr_val = [&](std::size_t k) { return addr_packed(0, k); };
    auto dit = descriptors_res.find(site);
    if (dit == descriptors_res.end()) {
        fail("an untracked image");
    }
    const Source& src = *dit->second;
    bool store = name.starts_with("image_store");
    const UsedResource* uk = used_kind(src);
    bool uav = store || (uk && uk->kind == 3);
    std::pair<std::uint32_t, std::uint32_t> props{kind | (uav ? 0x1000 : 0), 0x409};
    if (name.starts_with("image_atomic_")) {
        auto op = name.substr(13);
        std::uint32_t code;
        if (op == "add" || op == "sub") {
            code = 0;
        } else if (op == "and") {
            code = 1;
        } else if (op == "or") {
            code = 2;
        } else if (op == "xor") {
            code = 3;
        } else if (op == "smin") {
            code = 4;
        } else if (op == "smax") {
            code = 5;
        } else if (op == "umin") {
            code = 6;
        } else if (op == "umax") {
            code = 7;
        } else if (op == "swap") {
            code = 8;
        } else if (op == "cmpswap") {
            code = UINT32_MAX;
        } else {
            fail(name.data());
        }
        if (f.d16 || f.a16) {
            fail("a 16-bit image atomic");
        }
        bool compare = code == UINT32_MAX;
        bool wide;
        if ((!compare && dmask == 1) || (compare && dmask == 3)) {
            wide = false;
        } else if ((!compare && dmask == 3) || (compare && dmask == 15)) {
            wide = true;
        } else {
            fail("an image atomic data mask");
        }
        V iu = s.m.undef(s.t.i32);
        std::vector<V> cs(3, iu);
        for (std::size_t k = 0; k < coords && k < 3; ++k) {
            cs[k] = addr_val(k);
        }
        if (f.data.kind != Operand::Vector) {
            fail("image atomic data");
        }
        std::uint8_t reg = f.data.a;
        std::uint8_t width = wide ? 2 : 1;
        std::vector<V> vs = vgprs(inst, f.data, compare ? 2 * width : width);
        Ty ty;
        std::uint32_t component;
        if (wide) {
            s.flags |= dxil::flags::INT64 | dxil::flags::ATOMIC_TYPED64 | dxil::flags::ATOMIC_HEAP64;
            ty = s.m.int_(64);
            component = 0x107;
        } else {
            ty = s.t.i32;
            component = 0x105;
        }
        V original = wide ? join64(vs[0], vs[1]) : vs[0];
        V value = original;
        if (op == "sub") {
            V zero = s.m.int_const(ty, 0);
            value = s.m.bin(c, Bin::Sub, zero, original);
        }
        V h = handle(site, src, false, {kind | 0x1000, component});
        std::vector<V> args{h};
        const char* intrinsic;
        std::uint32_t opcode;
        if (compare) {
            V comparand = wide ? join64(vs[2], vs[3]) : vs[1];
            args.insert(args.end(), cs.begin(), cs.end());
            args.push_back(comparand);
            args.push_back(value);
            intrinsic = wide ? "atomicCompareExchange.i64" : "atomicCompareExchange.i32";
            opcode = 79;
        } else {
            args.push_back(s.m.i32(code));
            args.insert(args.end(), cs.begin(), cs.end());
            args.push_back(value);
            intrinsic = wide ? "atomicBinOp.i64" : "atomicBinOp.i32";
            opcode = 78;
        }
        auto at = begin_guarded(g);
        V r = s.op(cur, intrinsic, Attr::NoUnwind, ty, opcode, args);
        if (f.cache.glc) {
            V old = end_guarded(at, {{r, original}})[0];
            if (wide) {
                auto [lo, hi] = split64(old);
                write(inst, Reg::v(reg), lo, std::nullopt);
                write(inst, Reg::v(static_cast<std::uint8_t>(reg + 1)), hi, std::nullopt);
            } else {
                write(inst, Reg::v(reg), old, std::nullopt);
            }
        } else {
            end_guarded(at, {});
        }
        return;
    }
    if (store) {
        if (name != "image_store") {
            fail("an image store to a mip level");
        }
        if (dmask != 0xF) {
            fail("an image store of some channels");
        }
        V iu = s.m.undef(s.t.i32);
        std::vector<V> cs(3, iu);
        for (std::size_t k = 0; k < coords && k < 3; ++k) {
            cs[k] = addr_val(k);
        }
        if (f.data.kind != Operand::Vector) {
            fail("image store data");
        }
        std::uint8_t reg = f.data.a;
        std::vector<V> vs;
        std::optional<std::uint32_t> slot;
        if (src.kind == Source::Loaded) {
            if (auto* sm = std::get_if<SmemFields>(&p().blocks[src.load.first].insts[src.load.second].fields)) {
                slot = static_cast<std::uint32_t>(sm->offset) / 4 + src.first;
            }
        }
        if (auto bits = stand_in::store_plan_bits(slot)) {
            for (std::uint32_t bb : *bits) {
                vs.push_back(s.m.f32(bb));
            }
        }
        for (std::uint8_t k = 0; k < 4 && vs.size() != 4; ++k) {
            std::uint8_t r = f.d16 ? k / 2 : k;
            auto v = inst.read(Reg::v(static_cast<std::uint8_t>(reg + r)));
            if (!v) {
                fail("image store data");
            }
            if (f.d16) {
                V xv = i32_of(*v);
                if (k % 2 == 1) {
                    V k16 = s.m.i32(16);
                    xv = s.m.bin(cur, Bin::LShr, xv, k16);
                }
                vs.push_back(s.op(cur, "legacyF16ToF32", Attr::ReadNone, s.t.f32, 131, {xv}));
            } else {
                vs.push_back(float_of(*v));
            }
        }
        V h = handle(site, src, false, props);
        auto at = begin_guarded(g);
        std::vector<V> args{h};
        args.insert(args.end(), cs.begin(), cs.end());
        args.insert(args.end(), vs.begin(), vs.end());
        args.push_back(s.m.i8(0xF));
        s.op(cur, "textureStore.f32", Attr::NoUnwind, s.t.void_, 67, args);
        end_guarded(at, {});
        return;
    }
    bool has_cl = name.ends_with("_cl");
    std::uint32_t op;
    bool sample;
    if (name == "image_sample" || name == "image_sample_o" || name == "image_sample_cl") {
        op = 60;
        sample = true;
    } else if (name == "image_sample_lz" || name == "image_sample_l" || name == "image_sample_lz_o") {
        op = 62;
        sample = true;
    } else if (name == "image_sample_b" || name == "image_sample_b_cl") {
        op = 61;
        sample = true;
    } else if (name == "image_sample_c_lz") {
        op = 65;
        sample = true;
    } else if (name == "image_sample_d" || name == "image_sample_d_cl") {
        op = 63;
        sample = true;
    } else if (name == "image_get_lod" && s.pixel) {
        op = 81;
        sample = true;
    } else if ((name == "image_sample_c" || name == "image_sample_c_cl") && s.pixel) {
        op = 64;
        sample = true;
    } else if (name == "image_gather4" || name == "image_gather4_lz" || name == "image_gather4_lz_o" || name == "image_gather4_l" ||
               name == "image_gather4_l_o") {
        op = 73;
        sample = true;
    } else if (name == "image_load" || name == "image_load_mip") {
        op = 66;
        sample = false;
    } else if (name == "image_get_resinfo") {
        op = 72;
        sample = false;
    } else {
        fail(name.data());
    }
    if (op == 72) {
        V h = handle(site, src, false, props);
        V mip = uav ? s.m.undef(s.t.i32) : addr_val(0);
        Ty i32t = s.t.i32;
        Ty dims = s.m.named_struct("dx.types.Dimensions", {i32t, i32t, i32t, i32t});
        V d = s.op(c, "getDimensions", Attr::ReadOnly, dims, 72, {h, mip});
        V one = s.m.i32(1);
        bool present[4];
        switch (kind) {
            case 1:
                present[0] = true;
                present[1] = false;
                present[2] = false;
                present[3] = true;
                break;
            case 2:
            case 5:
            case 6:
                present[0] = true;
                present[1] = true;
                present[2] = false;
                present[3] = true;
                break;
            default: present[0] = present[1] = present[2] = present[3] = true; break;
        }
        if (f.data.kind != Operand::Vector) {
            fail("an image result");
        }
        std::uint8_t reg = f.data.a;
        std::uint8_t k = 0;
        for (std::uint32_t ch = 0; ch < 4; ++ch) {
            if ((dmask >> ch & 1) == 0) {
                continue;
            }
            V xv = (ch == 3 && uav) || !present[ch] ? one : s.m.extract(c, d, ch);
            write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), xv, g);
            ++k;
        }
        return;
    }
    if (uav && op != 66) {
        fail("sampling a UAV");
    }
    V h = handle(site, src, false, props);
    V fu = s.m.undef(s.t.f32);
    V iu = s.m.undef(s.t.i32);
    std::optional<std::array<V, 4>> gathered;
    V r;
    if (sample) {
        auto sit = descriptors_smp.find(site);
        if (sit == descriptors_smp.end()) {
            fail("an untracked sampler");
        }
        const Source& ss = *sit->second;
        bool cmp = op == 64 || op == 65;
        if (cmp && (f.a16 || std::popcount(dmask) != 1)) {
            fail("a comparison sample of 16-bit coordinates or several channels");
        }
        V sh = handle(site, ss, true, {14u | (cmp ? 0x8000u : 0u), 0});
        bool offset_word = name.ends_with("_o");
        std::size_t first = (offset_word ? 1 : 0) + (name.starts_with("image_sample_b") ? 1 : 0) + (cmp ? 1 : 0);
        std::size_t spatial = coords - ((kind == 6 || kind == 7) ? 1 : 0);
        std::size_t grads = kind == 5 ? 2 : spatial;
        std::size_t deriv_regs = op == 63 ? (f.a16 ? (grads + 1) / 2 : grads) : 0;
        std::size_t coord_base = first + 2 * deriv_regs;
        std::vector<V> cs(4, fu);
        auto to_float = [&](V xv) {
            if (f.a16) {
                return s.op(cur, "legacyF16ToF32", Attr::ReadNone, s.t.f32, 131, {xv});
            }
            return as_float(xv);
        };
        if (kind == 5 && (f.unorm || op == 81)) {
            fail("a cube sample with texel coordinates or a level query");
        }
        for (std::size_t k = 0; k < coords && k < 4; ++k) {
            V xv = addr_packed(coord_base, k);
            cs[k] = to_float(xv);
        }
        if (kind == 5) {
            auto dir = cube_direction(cs[0], cs[1], cs[2]);
            cs[0] = dir[0];
            cs[1] = dir[1];
            cs[2] = dir[2];
        }
        if (f.unorm) {
            Ty i32t = s.t.i32;
            Ty dims = s.m.named_struct("dx.types.Dimensions", {i32t, i32t, i32t, i32t});
            V z = s.m.i32(0);
            V d = s.op(c, "getDimensions", Attr::ReadOnly, dims, 72, {h, z});
            for (std::size_t k = 0; k < spatial && k < 4; ++k) {
                V n = s.m.extract(c, d, static_cast<std::uint32_t>(k));
                n = s.m.cast(c, Cast::UiToFp, n, s.t.f32);
                cs[k] = s.m.bin(c, Bin::SDiv, cs[k], n);
            }
        }
        if (op == 81) {
            if (f.data.kind != Operand::Vector) {
                fail("an image result");
            }
            std::uint8_t reg = f.data.a;
            Ty f32t = s.t.f32;
            std::uint8_t k = 0;
            for (std::uint32_t ch = 0; ch < 2; ++ch) {
                if ((dmask >> ch & 1) == 0) {
                    continue;
                }
                V clamped = s.m.boolean(ch == 0);
                V lod = s.op(c, "calculateLOD.f32", Attr::ReadOnly, f32t, 81, {h, sh, cs[0], cs[1], cs[2], clamped});
                V xv = as_int(lod);
                write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), xv, g);
                ++k;
            }
            return;
        }
        std::size_t offsets = kind == 5 ? 0 : std::min<std::size_t>(coords - ((kind == 6 || kind == 7) ? 1 : 0), 3);
        V zero = s.m.i32(0);
        std::vector<V> os(3, iu);
        for (std::size_t k = 0; k < offsets; ++k) {
            os[k] = zero;
        }
        if (offset_word && offsets > 0) {
            V w = addr_val(0);
            std::pair<std::size_t, std::uint32_t> fields[2] = {{0, 26}, {1, 18}};
            for (std::size_t q = 0; q < std::min<std::size_t>(offsets, 2); ++q) {
                auto [k, at] = fields[q];
                V left = s.m.i32(at);
                V t = s.m.bin(c, Bin::Shl, w, left);
                V k26 = s.m.i32(26);
                os[k] = s.m.bin(c, Bin::AShr, t, k26);
            }
        }
        std::vector<V> args{h, sh};
        args.insert(args.end(), cs.begin(), cs.end());
        V clamp;
        if (has_cl) {
            V xv = addr_packed(coord_base, coords);
            clamp = to_float(xv);
        } else {
            clamp = fu;
        }
        if (op == 60 && !s.pixel) {
            args.insert(args.end(), os.begin(), os.end());
            args.push_back(s.m.f32(0));
            r = s.op(c, "sampleLevel.f32", Attr::ReadOnly, s.t.res_ret, 62, args);
        } else if (op == 60) {
            if (offset_word) {
                V w = addr_val(0);
                if (auto wv = s.m.int_value(w)) {
                    auto wu = static_cast<std::uint32_t>(*wv);
                    auto field = [&](std::size_t k) {
                        auto xv = static_cast<std::int32_t>(wu >> (8 * k) & 0x3F);
                        return xv >= 32 ? xv - 64 : xv;
                    };
                    std::size_t n = std::min<std::size_t>(offsets, 2);
                    bool in_range = true;
                    for (std::size_t k = 0; k < n; ++k) {
                        if (field(k) < -8 || field(k) > 7) {
                            in_range = false;
                        }
                    }
                    if (in_range) {
                        for (std::size_t k = 0; k < n; ++k) {
                            os[k] = s.m.i32(static_cast<std::uint32_t>(field(k)));
                        }
                    }
                }
            }
            args.insert(args.end(), os.begin(), os.end());
            args.push_back(clamp);
            r = s.op(c, "sample.f32", Attr::ReadOnly, s.t.res_ret, 60, args);
        } else if (op == 61) {
            V bias = addr_u32(offset_word ? 1 : 0);
            if (f.a16) {
                V low = s.m.i32(0xFFFF);
                bias = s.m.bin(c, Bin::And, bias, low);
                bias = to_float(bias);
            } else {
                bias = as_float(bias);
            }
            args.insert(args.end(), os.begin(), os.end());
            if (s.pixel) {
                args.push_back(bias);
                args.push_back(clamp);
                r = s.op(c, "sampleBias.f32", Attr::ReadOnly, s.t.res_ret, 61, args);
            } else {
                args.push_back(bias);
                r = s.op(c, "sampleLevel.f32", Attr::ReadOnly, s.t.res_ret, 62, args);
            }
        } else if (op == 63 && kind == 5) {
            V len[2] = {fu, fu};
            for (std::size_t axis = 0; axis < 2; ++axis) {
                V sum = s.m.f32(0);
                for (std::size_t k = 0; k < 2; ++k) {
                    V xv = addr_packed(first + axis * deriv_regs, k);
                    xv = to_float(xv);
                    V sq = s.m.bin(c, Bin::Mul, xv, xv);
                    sum = s.m.bin(c, Bin::Add, sum, sq);
                }
                len[axis] = unary(24, sum);
            }
            V longer = binary(35, len[0], len[1]);
            Ty i32t = s.t.i32;
            Ty dims = s.m.named_struct("dx.types.Dimensions", {i32t, i32t, i32t, i32t});
            V z = s.m.i32(0);
            V d = s.op(c, "getDimensions", Attr::ReadOnly, dims, 72, {h, z});
            V width = s.m.extract(c, d, 0);
            width = s.m.cast(c, Cast::UiToFp, width, s.t.f32);
            V texels = s.m.bin(c, Bin::Mul, longer, width);
            V ln = unary(23, texels);
            V per_ln2 = s.m.f32(fb(std::numbers::log2e_v<float>));
            V lod = s.m.bin(c, Bin::Mul, ln, per_ln2);
            args.insert(args.end(), os.begin(), os.end());
            args.push_back(lod);
            r = s.op(c, "sampleLevel.f32", Attr::ReadOnly, s.t.res_ret, 62, args);
        } else if (op == 63) {
            std::vector<V> gr(6, fu);
            for (std::size_t k = 0; k < spatial; ++k) {
                V xv = addr_packed(first, k);
                gr[k] = to_float(xv);
                V yv = addr_packed(first + deriv_regs, k);
                gr[3 + k] = to_float(yv);
            }
            args.insert(args.end(), os.begin(), os.end());
            args.insert(args.end(), gr.begin(), gr.end());
            args.push_back(clamp);
            r = s.op(c, "sampleGrad.f32", Attr::ReadOnly, s.t.res_ret, 63, args);
        } else if (op == 64 || op == 65) {
            V reference = addr_u32(first - 1);
            reference = as_float(reference);
            args.insert(args.end(), os.begin(), os.end());
            args.push_back(reference);
            const char* nm;
            if (op == 64) {
                args.push_back(clamp);
                nm = "sampleCmp.f32";
            } else {
                nm = "sampleCmpLevelZero.f32";
            }
            r = s.op(c, nm, Attr::ReadOnly, s.t.res_ret, op, args);
        } else if (op == 62) {
            V lod;
            if (name == "image_sample_l") {
                V xv = addr_packed(coord_base, coords);
                lod = to_float(xv);
            } else {
                lod = s.m.f32(0);
            }
            args.insert(args.end(), os.begin(), os.end());
            args.push_back(lod);
            r = s.op(c, "sampleLevel.f32", Attr::ReadOnly, s.t.res_ret, 62, args);
        } else {
            if (name == "image_gather4") {
                fail("a gather with implicit level");
            }
            if (std::popcount(dmask) != 1) {
                fail("a gather of several channels");
            }
            if ((name == "image_gather4_l" || name == "image_gather4_l_o") && coords == 2 && spatial == 2) {
                V lod = addr_packed(coord_base, coords);
                lod = to_float(lod);
                Ty i32t = s.t.i32, f32t = s.t.f32;
                V zf = s.m.f32(0);
                lod = binary(35, lod, zf);
                lod = unary(26, lod);
                V mip = s.m.cast(c, Cast::FpToUi, lod, i32t);
                Ty dims = s.m.named_struct("dx.types.Dimensions", {i32t, i32t, i32t, i32t});
                V d = s.op(c, "getDimensions", Attr::ReadOnly, dims, 72, {h, mip});
                V half = s.m.f32(fb(0.5f));
                V one = s.m.f32(fb(1.0f));
                V lo[2] = {iu, iu}, hi[2] = {iu, iu};
                for (std::uint32_t k = 0; k < 2; ++k) {
                    V n = s.m.extract(c, d, k);
                    n = s.m.cast(c, Cast::UiToFp, n, f32t);
                    V last = s.m.bin(c, Bin::Sub, n, one);
                    V t = s.m.bin(c, Bin::Mul, cs[k], n);
                    t = s.m.bin(c, Bin::Sub, t, half);
                    V fst = unary(27, t);
                    V by = s.m.cast(c, Cast::SiToFp, os[k], f32t);
                    fst = s.m.bin(c, Bin::Add, fst, by);
                    V next = s.m.bin(c, Bin::Add, fst, one);
                    std::pair<V*, V> slots[2] = {{&lo[k], fst}, {&hi[k], next}};
                    for (auto& [slot, xv] : slots) {
                        V t2 = binary(35, xv, zf);
                        t2 = binary(36, t2, last);
                        *slot = s.m.cast(c, Cast::FpToSi, t2, i32t);
                    }
                }
                auto ch = static_cast<std::uint32_t>(std::countr_zero(dmask));
                std::array<V, 4> gv{fu, fu, fu, fu};
                std::optional<V> read;
                std::pair<V, V> texels[4] = {{lo[0], hi[1]}, {hi[0], hi[1]}, {hi[0], lo[1]}, {lo[0], lo[1]}};
                for (std::size_t q = 0; q < 4; ++q) {
                    V rr = s.op(c, "textureLoad.f32", Attr::ReadOnly, s.t.res_ret, 66, {h, mip, texels[q].first, texels[q].second, iu, iu, iu, iu});
                    gv[q] = s.m.extract(c, rr, ch);
                    read = rr;
                }
                gathered = gv;
                r = *read;
            } else {
                args.insert(args.end(), os.begin(), os.begin() + 2);
                args.push_back(s.m.i32(static_cast<std::uint32_t>(std::countr_zero(dmask))));
                r = s.op(c, "textureGather.f32", Attr::ReadOnly, s.t.res_ret, 73, args);
            }
        }
    } else {
        std::vector<V> cs(3, iu);
        for (std::size_t k = 0; k < coords && k < 3; ++k) {
            cs[k] = addr_val(k);
        }
        V mip;
        if (name == "image_load_mip") {
            mip = addr_val(coords);
        } else if (uav) {
            s.flags |= dxil::flags::TYPED_UAV_LOADS;
            mip = iu;
        } else {
            mip = s.m.i32(0);
        }
        std::vector<V> args{h, mip};
        args.insert(args.end(), cs.begin(), cs.end());
        args.insert(args.end(), {iu, iu, iu});
        r = s.op(c, "textureLoad.f32", Attr::ReadOnly, s.t.res_ret, 66, args);
    }
    if (f.data.kind != Operand::Vector) {
        fail("an image result");
    }
    std::uint8_t reg = f.data.a;
    std::vector<std::uint32_t> channels;
    if (op == 73) {
        channels = {0, 1, 2, 3};
    } else {
        for (std::uint32_t ch = 0; ch < 4; ++ch) {
            if (dmask >> ch & 1) {
                channels.push_back(ch);
            }
        }
    }
    std::vector<V> xs;
    for (std::uint32_t ch : channels) {
        V xv = gathered ? (*gathered)[ch] : s.m.extract(c, r, ch);
        if (f.d16) {
            xs.push_back(s.op(c, "legacyF32ToF16", Attr::ReadNone, s.t.i32, 130, {xv}));
        } else {
            xs.push_back(as_int(xv));
        }
    }
    if (f.d16) {
        V k16 = s.m.i32(16);
        std::vector<V> packed;
        for (std::size_t q = 0; q < xs.size(); q += 2) {
            if (q + 1 < xs.size()) {
                V hi = s.m.bin(c, Bin::Shl, xs[q + 1], k16);
                packed.push_back(s.m.bin(c, Bin::Or, xs[q], hi));
            } else {
                packed.push_back(xs[q]);
            }
        }
        xs = std::move(packed);
    }
    for (std::size_t k = 0; k < xs.size(); ++k) {
        write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), xs[k], g);
    }
}

std::array<V, 3> Lower::cube_direction(V sv, V tv, V face) {
    std::size_t c = cur;
    V two = s.m.f32(fb(2.0f));
    V three = s.m.f32(fb(3.0f));
    V u = s.m.bin(c, Bin::Mul, sv, two);
    u = s.m.bin(c, Bin::Sub, u, three);
    V v = s.m.bin(c, Bin::Mul, tv, two);
    v = s.m.bin(c, Bin::Sub, v, three);
    V z = s.m.f32(0x80000000u);
    V nu = s.m.bin(c, Bin::Sub, z, u);
    V nv = s.m.bin(c, Bin::Sub, z, v);
    V one = s.m.f32(fb(1.0f));
    V mone = s.m.f32(fb(-1.0f));
    V fc = s.m.cast(c, Cast::FpToUi, face, s.t.i32);
    const V table[3][6] = {
        {one, mone, u, u, u, nu},
        {nv, nv, one, mone, nv, nv},
        {nu, u, v, nv, one, mone},
    };
    std::array<V, 3> out;
    for (int row = 0; row < 3; ++row) {
        V r = table[row][0];
        for (std::uint32_t n = 1; n < 6; ++n) {
            V kn = s.m.i32(n);
            V is = s.m.cmp(c, pred::EQ, fc, kn);
            r = s.m.select(c, is, table[row][n], r);
        }
        out[row] = r;
    }
    return out;
}

}   
