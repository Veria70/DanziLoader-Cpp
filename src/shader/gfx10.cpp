// SPDX-License-Identifier: GPL-3.0-or-later
// GFX10 machine code (`gfx10/mod.rs`, `gfx10/fields.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See gfx10.hpp.
#include "gfx10.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>

#include "error.hpp"

namespace danzi::shader {

namespace {

struct Opcode {
    std::uint16_t op;
    const char* name;
};
#include "gfx10_opcodes.inc"

template <std::size_t N>
std::string_view lookup(const Opcode (&table)[N], std::uint16_t op) {
    auto it = std::lower_bound(std::begin(table), std::end(table), op, [](const Opcode& o, std::uint16_t v) { return o.op < v; });
    return it != std::end(table) && it->op == op ? std::string_view(it->name) : std::string_view();
}

bool in(std::string_view name, std::initializer_list<std::string_view> list) {
    for (auto n : list) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

}   

std::string_view opcode_name(Encoding e, std::uint16_t op) {
    switch (e) {
        case Encoding::Sop2: return lookup(SOP2, op);
        case Encoding::Sopk: return lookup(SOPK, op);
        case Encoding::Sop1: return lookup(SOP1, op);
        case Encoding::Sopc: return lookup(SOPC, op);
        case Encoding::Sopp: return lookup(SOPP, op);
        case Encoding::Smem: return lookup(SMEM, op);
        case Encoding::Vop2: return lookup(VOP2, op);
        case Encoding::Vop1: return lookup(VOP1, op);
        case Encoding::Vopc: return lookup(VOPC, op);
        case Encoding::Vop3:
            if (op < 0x100) {
                return lookup(VOPC, op);
            }
            if (op < 0x140) {
                return lookup(VOP2, static_cast<std::uint16_t>(op - 0x100));
            }
            if (op >= 0x180 && op < 0x200) {
                return lookup(VOP1, static_cast<std::uint16_t>(op - 0x180));
            }
            return lookup(VOP3, op);
        case Encoding::Vop3p: return lookup(VOP3P, op);
        case Encoding::Vintrp: return lookup(VINTRP, op);
        case Encoding::Ds: return lookup(DS, op);
        case Encoding::Mubuf: return lookup(MUBUF, op);
        case Encoding::Mtbuf: return lookup(MTBUF, op);
        case Encoding::Mimg: return lookup(MIMG, op);
        case Encoding::Exp: return "exp";
        case Encoding::Flat: return lookup(FLAT, op);
        case Encoding::Global: return lookup(GLOBAL, op);
        case Encoding::Scratch: return lookup(SCRATCH, op);
    }
    return {};
}

namespace {

Instruction instruction(std::span<const std::uint8_t> code, std::size_t offset) {
    auto word = [&](std::size_t i) -> std::uint32_t {
        std::size_t at = offset + 4 * i;
        if (at > code.size() || code.size() - at < 4) {
            isa(offset, "instruction runs past the end of the code");
        }
        std::uint32_t v;
        std::memcpy(&v, code.data() + at, 4);
        return v;
    };
    std::uint32_t w = word(0);
    Encoding encoding;
    std::uint32_t opcode;
    std::size_t len;
    if (w >> 31 == 0) {
        std::uint32_t src0 = w & 0x1FF;
        std::size_t extra = (src0 == 0xFF || src0 == 0xF9 || src0 == 0xFA || src0 == 0xE9 || src0 == 0xEA) ? 1 : 0;
        std::uint32_t top = w >> 25;
        if (top == 0x3F) {
            encoding = Encoding::Vop1;
            opcode = (w >> 9) & 0xFF;
            len = 1 + extra;
        } else if (top == 0x3E) {
            encoding = Encoding::Vopc;
            opcode = (w >> 17) & 0xFF;
            len = 1 + extra;
        } else {
            auto n = opcode_name(Encoding::Vop2, static_cast<std::uint16_t>(top));
            bool mk_ak = in(n, {"v_madmk_f32", "v_madak_f32", "v_fmamk_f32", "v_fmaak_f32", "v_madmk_f16", "v_madak_f16", "v_fmamk_f16",
                                "v_fmaak_f16"});
            encoding = Encoding::Vop2;
            opcode = top;
            len = 1 + std::max<std::size_t>(extra, mk_ak ? 1 : 0);
        }
    } else if (w >> 30 == 0b10) {
        auto literal = [](std::initializer_list<std::uint32_t> srcs) -> std::size_t {
            for (auto s : srcs) {
                if (s == 0xFF) {
                    return 2;
                }
            }
            return 1;
        };
        std::uint32_t top = w >> 23;
        if (top == 0x17D) {
            encoding = Encoding::Sop1;
            opcode = (w >> 8) & 0xFF;
            len = literal({w & 0xFF});
        } else if (top == 0x17E) {
            encoding = Encoding::Sopc;
            opcode = (w >> 16) & 0x7F;
            len = literal({w & 0xFF, (w >> 8) & 0xFF});
        } else if (top == 0x17F) {
            encoding = Encoding::Sopp;
            opcode = (w >> 16) & 0x7F;
            len = 1;
        } else if (w >> 28 == 0b1011) {
            std::uint32_t op = (w >> 23) & 0x1F;
            bool imm32 = opcode_name(Encoding::Sopk, static_cast<std::uint16_t>(op)) == "s_setreg_imm32_b32";
            encoding = Encoding::Sopk;
            opcode = op;
            len = 1 + (imm32 ? 1 : 0);
        } else {
            encoding = Encoding::Sop2;
            opcode = (w >> 23) & 0x7F;
            len = literal({w & 0xFF, (w >> 8) & 0xFF});
        }
    } else {
        auto vop3 = [](std::uint32_t w1) -> std::size_t {
            return 2 + (((w1 & 0x1FF) == 0xFF || ((w1 >> 9) & 0x1FF) == 0xFF || ((w1 >> 18) & 0x1FF) == 0xFF) ? 1 : 0);
        };
        switch (w >> 26) {
            case 0x32: encoding = Encoding::Vintrp; opcode = (w >> 16) & 0x3; len = 1; break;
            case 0x33: encoding = Encoding::Vop3p; opcode = (w >> 16) & 0x7F; len = vop3(word(1)); break;
            case 0x35: encoding = Encoding::Vop3; opcode = (w >> 16) & 0x3FF; len = vop3(word(1)); break;
            case 0x36: encoding = Encoding::Ds; opcode = (w >> 18) & 0xFF; len = 2; break;
            case 0x37:
                switch ((w >> 14) & 3) {
                    case 0: encoding = Encoding::Flat; break;
                    case 1: encoding = Encoding::Scratch; break;
                    case 2: encoding = Encoding::Global; break;
                    default: isa(offset, "reserved FLAT segment");
                }
                opcode = (w >> 18) & 0x7F;
                len = 2;
                break;
            case 0x38: encoding = Encoding::Mubuf; opcode = ((w >> 18) & 0x7F) | ((w >> 25 & 1) << 7); len = 2; break;
            case 0x3A: encoding = Encoding::Mtbuf; opcode = ((w >> 16) & 0x7) | ((word(1) >> 21 & 1) << 3); len = 2; break;
            case 0x3C: encoding = Encoding::Mimg; opcode = ((w >> 18) & 0x7F) | ((w & 1) << 7); len = 2 + (w >> 1 & 3); break;
            case 0x3D: encoding = Encoding::Smem; opcode = (w >> 18) & 0xFF; len = 2; break;
            case 0x3E: encoding = Encoding::Exp; opcode = 0; len = 2; break;
            default: isa(offset, "unknown encoding");
        }
    }
    Instruction inst;
    inst.offset = offset;
    inst.encoding = encoding;
    inst.opcode = static_cast<std::uint16_t>(opcode);
    for (std::size_t i = 0; i < len; ++i) {
        inst.words.push_back(word(i));
    }
    return inst;
}

}   

Flow flow_of(const Instruction& inst, std::size_t next, std::size_t len) {
    auto target = [&]() -> std::size_t {
        std::int64_t delta = 4 * std::int64_t{static_cast<std::int16_t>(static_cast<std::uint16_t>(inst.words[0]))};
        std::int64_t t = static_cast<std::int64_t>(next) + delta;
        if (t < 0 || static_cast<std::uint64_t>(t) >= len) {
            isa(inst.offset, "branch target outside the code");
        }
        return static_cast<std::size_t>(t);
    };
    auto n = inst.name();
    switch (inst.encoding) {
        case Encoding::Sopp:
            if (n == "s_endpgm" || n == "s_endpgm_saved" || n == "s_endpgm_ordered_ps_done" || n == "s_code_end") {
                return {Flow::Stop};
            }
            if (n == "s_branch") {
                return {Flow::Jump, target()};
            }
            if (n.starts_with("s_cbranch_")) {
                return {Flow::Branch, target()};
            }
            break;
        case Encoding::Sopk:
            if (n == "s_call_b64") {
                return {Flow::Branch, target()};
            }
            break;
        case Encoding::Sop1:
            if (n == "s_setpc_b64") {
                return {Flow::Stop};
            }
            break;
        default: break;
    }
    return {Flow::Next};
}

std::optional<std::size_t> far_jump_with(const Instruction* (*before)(const void*, std::size_t), const void* ctx, const Instruction& setpc,
                                         std::size_t len) {
    std::uint32_t n = setpc.words[0] & 0xFF;
    std::optional<std::int64_t> delta;
    std::size_t at = setpc.offset;
    for (int k = 0; k < 4; ++k) {
        const Instruction* prev = before(ctx, at);
        if (!prev) {
            break;
        }
        if (prev->offset + 4 * prev->words.size() != at) {
            break;
        }
        std::uint32_t w = prev->words[0];
        std::uint32_t sdst = (w >> 16) & 0x7F, ssrc0 = w & 0xFF, ssrc1 = (w >> 8) & 0xFF;
        auto name = prev->name();
        if (prev->encoding == Encoding::Sop2 && (name == "s_add_u32" || name == "s_sub_u32") && sdst == n && ssrc0 == n && ssrc1 == 0xFF &&
            !delta) {
            std::int64_t literal = static_cast<std::int32_t>(prev->words[1]);
            delta = name == "s_add_u32" ? literal : -literal;
        } else if (prev->encoding == Encoding::Sop1 && name == "s_getpc_b64" && sdst == n) {
            if (!delta) {
                break;
            }
            std::int64_t t = static_cast<std::int64_t>(prev->offset) + 4 + *delta;
            if (t < 0 || static_cast<std::uint64_t>(t) >= len) {
                isa(setpc.offset, "far jump target outside the code");
            }
            return static_cast<std::size_t>(t);
        }
        at = prev->offset;
    }
    return std::nullopt;
}

std::vector<Instruction> decode_checked(std::span<const std::uint8_t> code) {
    if (code.size() % 4 != 0) {
        isa(code.size(), "code size is not a multiple of 4");
    }
    std::map<std::size_t, Instruction> found;
    std::vector<std::size_t> pending{0};
    while (!pending.empty()) {
        std::size_t offset = pending.back();
        pending.pop_back();
        while (!found.contains(offset)) {
            Instruction inst = instruction(code, offset);
            std::size_t next = offset + 4 * inst.words.size();
            Flow f = control(inst, code.size(), [&](std::size_t at) -> const Instruction* {
                auto it = found.lower_bound(at);
                if (it == found.begin()) {
                    return nullptr;
                }
                return &std::prev(it)->second;
            });
            found.emplace(offset, inst);
            if (f.kind == Flow::Next) {
                offset = next;
            } else if (f.kind == Flow::Stop) {
                break;
            } else if (f.kind == Flow::Jump) {
                offset = f.target;
            } else {
                pending.push_back(f.target);
                offset = next;
            }
        }
    }
    std::vector<Instruction> list;
    list.reserve(found.size());
    for (auto& [o, i] : found) {
        list.push_back(i);
    }
    for (std::size_t k = 1; k < list.size(); ++k) {
        if (list[k - 1].offset + 4 * list[k - 1].words.size() > list[k].offset) {
            isa(list[k].offset, "a branch lands inside an instruction");
        }
    }
    return list;
}


namespace {

Operand scalar(std::uint8_t code, std::uint8_t dwords) { return Operand::scalar(code, dwords); }
Operand vector(std::uint32_t reg, std::uint8_t dwords) { return Operand::vector(reg, dwords); }


Operand source(std::uint32_t code, std::uint8_t dwords, std::uint32_t literal) {
    if (code <= 127) {
        return scalar(static_cast<std::uint8_t>(code), dwords);
    }
    if (code == 255) {
        return Operand::lit(literal);
    }
    if (code >= 256) {
        return vector(code - 256, dwords);
    }
    return Operand::constant(static_cast<std::uint8_t>(code));
}


Operand valu_dst(std::string_view name, std::uint32_t field, std::uint8_t dwords) {
    if (name == "v_readlane_b32" || name == "v_readfirstlane_b32") {
        return scalar(static_cast<std::uint8_t>(field), 1);
    }
    return vector(field, dwords);
}


std::uint8_t type_dwords(std::string_view token) { return in(token, {"b64", "u64", "i64", "f64"}) ? 2 : 1; }

std::string_view last_token(std::string_view name) {
    auto at = name.rfind('_');
    return at == std::string_view::npos ? name : name.substr(at + 1);
}


std::array<std::uint8_t, 3> salu_dwords(std::string_view name) {
    if (in(name, {"s_lshl_b64", "s_lshr_b64", "s_ashr_i64", "s_bfe_u64", "s_bfe_i64", "s_bitcmp0_b64", "s_bitcmp1_b64"})) {
        return {2, 2, 1};
    }
    if (in(name, {"s_bfm_b64", "s_bitset0_b64", "s_bitset1_b64", "s_bitreplicate_b64_b32"})) {
        return {2, 1, 1};
    }
    if (in(name, {"s_bcnt0_i32_b64", "s_bcnt1_i32_b64", "s_ff0_i32_b64", "s_ff1_i32_b64", "s_flbit_i32_b64", "s_flbit_i32_i64"})) {
        return {1, 2, 2};
    }
    std::uint8_t n = type_dwords(last_token(name));
    return {n, n, n};
}


struct Tokens {
    std::string_view parts[12];
    std::size_t count = 0;
    explicit Tokens(std::string_view s) {
        std::size_t start = 0;
        while (count < 12) {
            auto at = s.find('_', start);
            parts[count++] = s.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start);
            if (at == std::string_view::npos) {
                break;
            }
            start = at + 1;
        }
    }
};


std::pair<std::uint8_t, std::array<std::uint8_t, 3>> valu_dwords(std::string_view name) {
    if (in(name, {"v_lshlrev_b64", "v_lshrrev_b64", "v_ashrrev_i64"})) {
        return {2, {1, 2, 2}};
    }
    if (in(name, {"v_ldexp_f64", "v_trig_preop_f64", "v_cmp_class_f64", "v_cmpx_class_f64"})) {
        return {2, {2, 1, 2}};
    }
    if (in(name, {"v_mad_u64_u32", "v_mad_i64_i32"})) {
        return {2, {1, 1, 2}};
    }
    if (in(name, {"v_qsad_pk_u16_u8", "v_mqsad_pk_u16_u8"})) {
        return {2, {2, 1, 2}};
    }
    if (name == "v_mqsad_u32_u8") {
        return {4, {2, 1, 4}};
    }
    if (name == "v_frexp_exp_i32_f64") {
        return {1, {2, 2, 2}};
    }
    Tokens parts(name);
    if (parts.count == 4 && parts.parts[1] == "cvt") {
        std::uint8_t s = type_dwords(parts.parts[3]);
        return {type_dwords(parts.parts[2]), {s, s, s}};
    }
    std::uint8_t n = type_dwords(parts.count ? parts.parts[parts.count - 1] : std::string_view());
    return {n, {n, n, n}};
}


std::uint8_t data_dwords(std::string_view name) {
    std::uint8_t n = 1;
    Tokens t(name);
    for (std::size_t i = 0; i < t.count; ++i) {
        auto token = t.parts[i];
        if (in(token, {"dwordx2", "x2", "xy", "b64", "u64", "i64", "f64"})) {
            n = 2;
        } else if (in(token, {"dwordx3", "xyz", "b96"})) {
            n = 3;
        } else if (in(token, {"dwordx4", "xyzw", "b128"})) {
            n = 4;
        } else if (token == "dwordx8") {
            n = 8;
        } else if (token == "dwordx16") {
            n = 16;
        }
    }
    if (contains(name, "cmpswap")) {
        n = static_cast<std::uint8_t>(n * 2);
    }
    if (contains(name, "format_d16")) {
        n = static_cast<std::uint8_t>((n + 1) / 2);
    }
    return n;
}


std::uint8_t image_addr_dwords(std::string_view name, std::uint8_t dim, bool a16) {
    static constexpr std::uint8_t DIMS[8][2] = {{1, 1}, {2, 2}, {3, 3}, {3, 2}, {2, 1}, {3, 2}, {3, 2}, {4, 2}};
    std::uint8_t coords = DIMS[dim & 7][0], grads = DIMS[dim & 7][1];
    if (name == "image_bvh_intersect_ray") {
        return a16 ? 8 : 11;
    }
    if (name == "image_bvh64_intersect_ray") {
        return a16 ? 9 : 12;
    }
    if (name == "image_get_resinfo") {
        return 1;
    }
    std::uint8_t whole = 0, derivs = 0, packed = coords;
    std::string_view rest;
    bool sampled = false;
    if (name.starts_with("image_sample")) {
        rest = name.substr(12);
        sampled = true;
    } else if (name.starts_with("image_gather4")) {
        rest = name.substr(13);
        sampled = true;
    }
    if (sampled) {
        Tokens t(rest);
        for (std::size_t i = 0; i < t.count; ++i) {
            auto token = t.parts[i];
            if (token == "o" || token == "b" || token == "c") {
                ++whole;
            } else if (token == "d" || token == "cd") {
                derivs = grads;
            } else if (token == "l" || token == "cl") {
                ++packed;
            }
        }
    } else if (contains(name, "_mip")) {
        ++packed;
    }
    if (a16) {
        return static_cast<std::uint8_t>(whole + 2 * ((derivs + 1) / 2) + (packed + 1) / 2);
    }
    return static_cast<std::uint8_t>(whole + 2 * derivs + packed);
}

constexpr std::string_view VOP3_TWO_SOURCES[] = {
    "v_add_f64",           "v_mul_f64",           "v_min_f64",           "v_max_f64",          "v_ldexp_f64",          "v_mul_lo_u32",
    "v_mul_hi_u32",        "v_mul_lo_i32",        "v_mul_hi_i32",        "v_trig_preop_f64",   "v_lshlrev_b64",        "v_lshrrev_b64",
    "v_ashrrev_i64",       "v_add_nc_u16",        "v_sub_nc_u16",        "v_mul_lo_u16",       "v_lshrrev_b16",        "v_ashrrev_i16",
    "v_max_u16",           "v_max_i16",           "v_min_u16",           "v_min_i16",          "v_add_nc_i16",         "v_sub_nc_i16",
    "v_add_co_u32",        "v_sub_co_u32",        "v_subrev_co_u32",     "v_pack_b32_f16",     "v_cvt_pknorm_i16_f16", "v_cvt_pknorm_u16_f16",
    "v_lshlrev_b16",       "v_interp_p1ll_f16",   "v_readlane_b32",      "v_writelane_b32",    "v_ldexp_f32",          "v_bfm_b32",
    "v_bcnt_u32_b32",      "v_mbcnt_lo_u32_b32",  "v_mbcnt_hi_u32_b32",  "v_cvt_pknorm_i16_f32", "v_cvt_pknorm_u16_f32", "v_cvt_pk_u16_u32",
    "v_cvt_pk_i16_i32",    "v_sub_nc_i32",        "v_add_nc_i32",
};

constexpr std::string_view VOP3B[] = {"v_add_co_u32",      "v_sub_co_u32",    "v_subrev_co_u32", "v_add_co_ci_u32", "v_sub_co_ci_u32",
                                      "v_subrev_co_ci_u32", "v_div_scale_f32", "v_div_scale_f64", "v_mad_u64_u32",   "v_mad_i64_i32"};

template <std::size_t N>
bool listed(const std::string_view (&list)[N], std::string_view name) {
    for (auto n : list) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

bool bit(std::uint32_t word, std::uint32_t n) { return (word >> n & 1) != 0; }

Valu valu(const Instruction& inst, std::string_view name, std::uint8_t lanes) {

    if (name == "v_nop" || name == "v_pipeflush" || name == "v_clrexcp") {
        return Valu{};
    }
    auto w = [&](std::size_t i) -> std::uint32_t { return i < inst.words.size() ? inst.words[i] : 0; };
    std::uint32_t w0 = w(0);
    auto [dst_n, src_n] = valu_dwords(name);
    bool compare = inst.encoding == Encoding::Vopc || (inst.encoding == Encoding::Vop3 && inst.opcode < 0x100);
    bool vop3b = listed(VOP3B, name);
    bool carry_in = name.ends_with("_co_ci_u32") || name == "v_cndmask_b32";
    Valu v;
    switch (inst.encoding) {
        case Encoding::Vop1:
        case Encoding::Vop2:
        case Encoding::Vopc: {
            std::uint32_t ext = w(1);
            std::uint32_t src0 = w0 & 0x1FF;
            std::uint32_t vsrc1 = 256 + (w0 >> 9 & 0xFF);
            std::uint32_t src1 = vsrc1;
            if (src0 == 0xF9) {
                Sdwa sdwa{static_cast<std::uint8_t>(ext >> 8 & 7),
                          static_cast<std::uint8_t>(ext >> 11 & 3),
                          {static_cast<std::uint8_t>(ext >> 16 & 7), static_cast<std::uint8_t>(ext >> 24 & 7)},
                          static_cast<std::uint8_t>((ext >> 19 & 1) | (ext >> 26 & 2))};

                src0 = (ext & 0xFF) | ((ext >> 23 & 1) == 0 ? 256 : 0);
                if (ext >> 31 == 1) {
                    src1 = vsrc1 - 256;
                }
                v.neg = static_cast<std::uint8_t>((ext >> 20 & 1) | (ext >> 27 & 2));
                v.abs = static_cast<std::uint8_t>((ext >> 21 & 1) | (ext >> 28 & 2));
                if (compare) {
                    if ((ext >> 15 & 1) == 1) {
                        v.dst = scalar(static_cast<std::uint8_t>(ext >> 8 & 0x7F), lanes);
                    }
                    Sdwa c = sdwa;
                    c.dst_sel = 6;
                    c.dst_unused = 0;
                    v.sdwa = c;
                } else {
                    v.clamp = (ext >> 13 & 1) == 1;
                    v.omod = static_cast<std::uint8_t>(ext >> 14 & 3);
                    v.sdwa = sdwa;
                }
            } else if (src0 == 0xFA) {
                src0 = 256 + (ext & 0xFF);
                v.neg = static_cast<std::uint8_t>((ext >> 20 & 1) | (ext >> 21 & 2));
                v.abs = static_cast<std::uint8_t>((ext >> 21 & 1) | (ext >> 22 & 2));
                Dpp d{Dpp::Row};
                d.ctrl = static_cast<std::uint16_t>(ext >> 8 & 0x1FF);
                d.row_mask = static_cast<std::uint8_t>(ext >> 28);
                d.bank_mask = static_cast<std::uint8_t>(ext >> 24 & 0xF);
                d.bound_ctrl = (ext >> 19 & 1) == 1;
                d.fi = (ext >> 18 & 1) == 1;
                v.dpp = d;
            } else if (src0 == 0xE9 || src0 == 0xEA) {
                Dpp d{Dpp::Lanes};
                d.sel = ext >> 8;
                d.fi = src0 == 0xEA;
                v.dpp = d;
                src0 = 256 + (ext & 0xFF);
            }
            Operand op0 = source(src0, src_n[0], ext);
            Operand op1 = source(src1, src_n[1], ext);
            if (inst.encoding == Encoding::Vopc) {
                std::uint8_t mask = name.starts_with("v_cmpx") ? EXEC_CODE : VCC_CODE;
                if (!v.dst) {
                    v.dst = scalar(mask, lanes);
                }
                v.src.push_back(op0);
                v.src.push_back(op1);
            } else if (inst.encoding == Encoding::Vop1) {
                v.dst = valu_dst(name, w0 >> 17 & 0xFF, dst_n);
                v.src.push_back(op0);
            } else {
                v.dst = vector(w0 >> 17 & 0xFF, dst_n);
                if (name.ends_with("mk_f32") || name.ends_with("mk_f16")) {
                    v.src.push_back(op0);
                    v.src.push_back(Operand::lit(ext));
                    v.src.push_back(op1);
                } else if (name.ends_with("ak_f32") || name.ends_with("ak_f16")) {
                    v.src.push_back(op0);
                    v.src.push_back(op1);
                    v.src.push_back(Operand::lit(ext));
                } else {
                    v.src.push_back(op0);
                    v.src.push_back(op1);
                }
                if (carry_in) {
                    v.src.push_back(scalar(VCC_CODE, lanes));
                }
                if (name.ends_with("_co_ci_u32")) {
                    v.carry = scalar(VCC_CODE, lanes);
                }
            }
            break;
        }
        case Encoding::Vop3:
        case Encoding::Vop3p: {
            std::uint32_t w1 = w(1);
            auto field = [&](std::uint32_t i) { return w1 >> (9 * i) & 0x1FF; };
            std::size_t count;
            if (inst.encoding == Encoding::Vop3p) {
                count = (name.starts_with("v_pk_") && !contains(name, "_fma_") && !contains(name, "_mad_")) ? 2 : 3;
            } else if (inst.opcode < 0x100) {
                count = 2;
            } else if (inst.opcode < 0x140) {
                count = 2 + (carry_in ? 1 : 0);
            } else if (inst.opcode >= 0x180 && inst.opcode < 0x200) {
                count = 1;
            } else if (listed(VOP3_TWO_SOURCES, name)) {
                count = 2;
            } else {
                count = 3;
            }
            for (std::size_t i = 0; i < count; ++i) {
                std::uint32_t code = field(static_cast<std::uint32_t>(i));
                if (i == 0 && name.starts_with("v_interp_")) {
                    v.src.push_back(Operand::attr(static_cast<std::uint8_t>(code & 0x3F), static_cast<std::uint8_t>(code >> 6 & 3), (code >> 8 & 1) == 1));
                } else if (i == 2 && carry_in) {
                    v.src.push_back(source(code, lanes, w(2)));
                } else {
                    v.src.push_back(source(code, src_n[i], w(2)));
                }
            }
            v.clamp = (w0 >> 15 & 1) == 1;
            v.neg = static_cast<std::uint8_t>(w1 >> 29);
            if (inst.encoding == Encoding::Vop3p) {
                v.neg_hi = static_cast<std::uint8_t>(w0 >> 8 & 7);
                v.opsel = static_cast<std::uint8_t>(w0 >> 11 & 7);
                v.opsel_hi = static_cast<std::uint8_t>((w1 >> 27 & 3) | (w0 >> 12 & 4));
            } else {
                v.omod = static_cast<std::uint8_t>(w1 >> 27 & 3);
                if (vop3b) {
                    v.carry = scalar(static_cast<std::uint8_t>(w0 >> 8 & 0x7F), lanes);
                } else {
                    v.abs = static_cast<std::uint8_t>(w0 >> 8 & 7);
                    v.opsel = static_cast<std::uint8_t>(w0 >> 11 & 0xF);
                }
            }
            std::uint32_t vdst = w0 & 0xFF;
            v.dst = compare ? scalar(static_cast<std::uint8_t>(vdst), lanes) : valu_dst(name, vdst, dst_n);
            break;
        }
        default: break;
    }
    return v;
}

}   

bool ds_pair(std::string_view name) {
    Tokens t(name);
    for (std::size_t i = 0; i < t.count; ++i) {
        auto token = t.parts[i];
        if (token.starts_with("read2") || token.starts_with("write2") || token.starts_with("wrxchg2")) {
            return true;
        }
    }
    return false;
}

Fields fields(const Instruction& inst, bool wave64) {
    std::string_view name = inst.name();
    auto w = [&](std::size_t i) -> std::uint32_t { return i < inst.words.size() ? inst.words[i] : 0; };
    std::uint32_t w0 = w(0), w1 = w(1);
    auto cache = [](bool glc, bool slc, bool dlc) { return Cache{glc, slc, dlc}; };
    switch (inst.encoding) {
        case Encoding::Sop2:
        case Encoding::Sop1:
        case Encoding::Sopc: {
            auto [dst_n, src0_n, src1_n] = salu_dwords(name);
            Operand sdst = scalar(static_cast<std::uint8_t>(w0 >> 16 & 0x7F), dst_n);
            auto ssrc = [&](std::uint32_t shift, std::uint8_t dwords) { return source(w0 >> shift & 0xFF, dwords, w1); };
            SopFields f;
            if (inst.encoding == Encoding::Sop2) {
                f.dst = sdst;
                f.src.push_back(ssrc(0, src0_n));
                f.src.push_back(ssrc(8, src1_n));
            } else if (inst.encoding == Encoding::Sopc) {
                f.src.push_back(ssrc(0, src0_n));
                f.src.push_back(ssrc(8, src1_n));
            } else if (name == "s_getpc_b64") {
                f.dst = sdst;
            } else if (name == "s_setpc_b64" || name == "s_rfe_b64") {
                f.src.push_back(ssrc(0, src0_n));
            } else {
                f.dst = sdst;
                f.src.push_back(ssrc(0, src0_n));
            }
            return f;
        }
        case Encoding::Sopk: {
            Operand sdst = scalar(static_cast<std::uint8_t>(w0 >> 16 & 0x7F), static_cast<std::uint8_t>(1 + (name == "s_call_b64" ? 1 : 0)));
            SopFields f;
            if (name == "s_setreg_imm32_b32") {
                f.src.push_back(Operand::lit(w1));
            } else if (name == "s_version") {
            } else if (name.starts_with("s_cmpk_") || name.starts_with("s_waitcnt_") || name == "s_setreg_b32") {
                f.src.push_back(sdst);
            } else {
                f.dst = sdst;
            }
            f.imm = static_cast<std::uint16_t>(w0);
            return f;
        }
        case Encoding::Sopp: return SoppFields{static_cast<std::uint16_t>(w0)};
        case Encoding::Smem: {
            std::optional<std::uint8_t> data;
            if (name == "s_gl1_inv" || name == "s_dcache_inv" || name == "s_dcache_wb" || name.starts_with("s_dcache_discard")) {
            } else if (name == "s_memtime" || name == "s_memrealtime") {
                data = std::uint8_t{2};
            } else {
                data = data_dwords(name);
            }
            std::uint8_t base = static_cast<std::uint8_t>(2 + 2 * (name.starts_with("s_buffer_") ? 1 : 0));
            SmemFields f;
            if (data) {
                f.data = scalar(static_cast<std::uint8_t>(w0 >> 6 & 0x7F), *data);
            }
            f.base = scalar(static_cast<std::uint8_t>((w0 & 0x3F) << 1), base);
            f.offset = static_cast<std::int32_t>(w1 << 11) >> 11;
            f.soffset = scalar(static_cast<std::uint8_t>(w1 >> 25), 1);
            f.cache = cache(bit(w0, 16), false, bit(w0, 14));
            return f;
        }
        case Encoding::Vop1:
        case Encoding::Vop2:
        case Encoding::Vopc:
        case Encoding::Vop3:
        case Encoding::Vop3p: return valu(inst, name, wave64 ? 2 : 1);
        case Encoding::Vintrp: {
            std::uint8_t vsrc = static_cast<std::uint8_t>(w0 & 0xFF);
            VintrpFields f;
            f.dst = vector(w0 >> 18, 1);
            f.src = name == "v_interp_mov_f32" ? Operand::constant(vsrc) : vector(vsrc, 1);
            f.attr = Operand::attr(static_cast<std::uint8_t>(w0 >> 10 & 0x3F), static_cast<std::uint8_t>(w0 >> 8 & 3), false);
            return f;
        }
        case Encoding::Ds: {
            std::uint8_t ty = data_dwords(name);
            bool pair = ds_pair(name);
            bool returns = contains(name, "read") || contains(name, "_rtn") ||
                           in(name, {"ds_swizzle_b32", "ds_permute_b32", "ds_bpermute_b32", "ds_append", "ds_consume", "ds_ordered_count"});
            bool no_addr = name.starts_with("ds_gws") || contains(name, "addtid") || in(name, {"ds_nop", "ds_append", "ds_consume"});
            int data;
            if (contains(name, "read") || contains(name, "src2") ||
                in(name, {"ds_swizzle_b32", "ds_nop", "ds_append", "ds_consume", "ds_ordered_count", "ds_gws_sema_v", "ds_gws_sema_p",
                          "ds_gws_sema_release_all"})) {
                data = 0;
            } else if (pair || contains(name, "cmpst") || contains(name, "mskor")) {
                data = std::uint8_t{2};
            } else {
                data = 1;
            }
            DsFields f;
            if (returns) {
                f.dst = vector(w1 >> 24, static_cast<std::uint8_t>(ty * (1 + (pair ? 1 : 0))));
            }
            if (!no_addr) {
                f.addr = vector(w1 & 0xFF, 1);
            }
            for (int i = 0; i < data; ++i) {
                f.data.push_back(vector(w1 >> (8 + 8 * i) & 0xFF, ty));
            }
            f.offset0 = static_cast<std::uint8_t>(w0);
            f.offset1 = static_cast<std::uint8_t>(w0 >> 8);
            f.gds = bit(w0, 17);
            return f;
        }
        case Encoding::Mubuf:
        case Encoding::Mtbuf: {
            bool offen = bit(w0, 12), idxen = bit(w0, 13), tfe = bit(w1, 23);
            bool mtbuf = inst.encoding == Encoding::Mtbuf;
            BufferFields f;
            if (!name.ends_with("_inv")) {
                f.data = vector(w1 >> 8 & 0xFF, static_cast<std::uint8_t>(data_dwords(name) + (tfe ? 1 : 0)));
            }
            f.addr = vector(w1 & 0xFF, static_cast<std::uint8_t>(1 + ((offen && idxen) ? 1 : 0)));
            f.rsrc = scalar(static_cast<std::uint8_t>((w1 >> 16 & 0x1F) << 2), 4);
            f.soffset = source(w1 >> 24, 1, 0);
            f.offset = static_cast<std::uint16_t>(w0 & 0xFFF);
            f.offen = offen;
            f.idxen = idxen;
            if (mtbuf) {
                f.format = static_cast<std::uint8_t>(w0 >> 19 & 0x7F);
            }
            f.lds = !mtbuf && bit(w0, 16);
            f.tfe = tfe;
            f.cache = cache(bit(w0, 14), bit(w1, 22), bit(w0, 15));
            return f;
        }
        case Encoding::Mimg: {
            std::uint8_t dim = static_cast<std::uint8_t>(w0 >> 3 & 7), dmask = static_cast<std::uint8_t>(w0 >> 8 & 0xF);
            bool a16 = bit(w1, 30), d16 = bit(w1, 31);
            bool r128 = bit(w0, 15), tfe = bit(w0, 16), lwe = bit(w0, 17);
            std::uint8_t data = name.starts_with("image_gather4") ? 4 : static_cast<std::uint8_t>(std::max(std::popcount(unsigned{dmask}), 1));
            if (d16) {
                data = static_cast<std::uint8_t>((data + 1) / 2);
            }
            std::uint8_t count = image_addr_dwords(name, dim, a16);
            ImageFields f;
            if ((w0 >> 1 & 3) == 0) {
                f.addr.push_back(vector(w1 & 0xFF, count));
            } else {


                for (std::size_t i = 0; i < count; ++i) {
                    if (i == 0) {
                        f.addr.push_back(vector(w1 & 0xFF, 1));
                    } else {
                        f.addr.push_back(vector(w(2 + (i - 1) / 4) >> (8 * ((i - 1) % 4)) & 0xFF, 1));
                    }
                }
            }
            bool sampled = name.starts_with("image_sample") || name.starts_with("image_gather4") || name.starts_with("image_get_lod");
            f.data = vector(w1 >> 8 & 0xFF, static_cast<std::uint8_t>(data + ((tfe || lwe) ? 1 : 0)));
            f.rsrc = scalar(static_cast<std::uint8_t>((w1 >> 16 & 0x1F) << 2), r128 ? 4 : 8);
            if (sampled) {
                f.sampler = scalar(static_cast<std::uint8_t>((w1 >> 21 & 0x1F) << 2), 4);
            }
            f.dmask = dmask;
            f.dim = dim;
            f.unorm = bit(w0, 12);
            f.r128 = r128;
            f.a16 = a16;
            f.d16 = d16;
            f.tfe = tfe;
            f.lwe = lwe;
            f.cache = cache(bit(w0, 13), bit(w0, 25), bit(w0, 7));
            return f;
        }
        case Encoding::Flat:
        case Encoding::Global:
        case Encoding::Scratch: {
            std::uint8_t saddr = static_cast<std::uint8_t>(w1 >> 16 & 0x7F);
            bool glc = bit(w0, 16);
            std::uint8_t n = data_dwords(name);
            bool load = contains(name, "_load_"), atomic = contains(name, "_atomic_");
            FlatFields f;
            if (inst.encoding == Encoding::Flat) {
                f.addr = vector(w1 & 0xFF, 2);
            } else if (inst.encoding == Encoding::Global) {
                f.addr = vector(w1 & 0xFF, static_cast<std::uint8_t>(1 + (saddr == NULL_CODE ? 1 : 0)));
            } else if (saddr == NULL_CODE) {

                f.addr = vector(w1 & 0xFF, 1);
            }
            std::uint8_t ret = (atomic && contains(name, "cmpswap")) ? static_cast<std::uint8_t>(n / 2) : n;
            if (load || (atomic && glc)) {
                f.dst = vector(w1 >> 24, ret);
            }
            if (!load) {
                f.data = vector(w1 >> 8 & 0xFF, n);
            }
            if (inst.encoding != Encoding::Flat) {
                f.saddr = scalar(saddr, static_cast<std::uint8_t>(1 + (inst.encoding == Encoding::Global ? 1 : 0)));
            }
            f.offset = inst.encoding == Encoding::Flat ? static_cast<std::int16_t>(w0 & 0x7FF)
                                                       : static_cast<std::int16_t>(static_cast<std::int16_t>(static_cast<std::uint16_t>(w0 << 4)) >> 4);
            f.lds = bit(w0, 13);
            f.cache = cache(glc, bit(w0, 17), bit(w0, 12));
            return f;
        }
        case Encoding::Exp: {
            bool compr = bit(w0, 10);
            ExpFields f;
            f.target = static_cast<std::uint8_t>(w0 >> 4 & 0x3F);
            for (std::uint32_t i = 0; i < 4; ++i) {
                std::uint32_t slot = compr ? i / 2 : i;
                if (bit(w0, i)) {
                    f.src[i] = vector(w1 >> (8 * slot) & 0xFF, 1);
                }
            }
            f.compr = compr;
            f.done = bit(w0, 11);
            f.vm = bit(w0, 12);
            return f;
        }
    }
    return SoppFields{0};
}

}   
