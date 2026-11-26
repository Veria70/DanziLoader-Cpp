// SPDX-License-Identifier: GPL-3.0-or-later
// Instruction text in AMD's SP3 assembly syntax (`gfx10/sp3.rs`) and the
// `{:?}` forms of `RootParam` and `UsedResource` (`native.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See sp3.hpp.
#include "sp3.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace danzi::shader {

namespace {

std::string scalar_name(std::uint8_t code) {
    if (code <= 105) {
        return std::format("s{}", code);
    }
    switch (code) {
        case 106: return "vcc_lo";
        case 107: return "vcc_hi";
        case 124: return "m0";
        case 125: return "null";
        case 126: return "exec_lo";
        default: break;
    }
    if (code >= 108 && code <= 123) {
        return std::format("ttmp{}", code - 108);
    }
    return "exec_hi";
}

std::string const_name(std::uint8_t code) {
    if (code >= 128 && code <= 192) {
        return std::to_string(int{code} - 128);
    }
    if (code >= 193 && code <= 208) {
        return std::to_string(192 - int{code});
    }
    switch (code) {
        case 240: return "0.5";
        case 241: return "-0.5";
        case 242: return "1.0";
        case 243: return "-1.0";
        case 244: return "2.0";
        case 245: return "-2.0";
        case 246: return "4.0";
        case 247: return "-4.0";
        case 248: return "0.15915494";
        case 235: return "src_shared_base";
        case 236: return "src_shared_limit";
        case 237: return "src_private_base";
        case 238: return "src_private_limit";
        case 239: return "src_pops_exiting_wave_id";
        case 251: return "src_vccz";
        case 252: return "src_execz";
        case 253: return "src_scc";
        case 254: return "src_lds_direct";
        default: return std::format("<src {}>", code);
    }
}


std::string text(const Operand& o) {
    switch (o.kind) {
        case Operand::Scalar: {
            std::uint8_t code = o.a, dwords = o.b;
            if (dwords == 1) {
                return scalar_name(code);
            }
            if (code == 106 && dwords == 2) {
                return "vcc";
            }
            if (code == 126 && dwords == 2) {
                return "exec";
            }
            if (code < 106) {
                return std::format("s[{}:{}]", code, static_cast<std::uint8_t>(code + dwords - 1));
            }
            if (code >= 108 && code < 124) {
                return std::format("ttmp[{}:{}]", code - 108, static_cast<std::uint8_t>(code - 108 + dwords - 1));
            }
            return scalar_name(code);
        }
        case Operand::Vector:
            if (o.b == 1) {
                return std::format("v{}", o.a);
            }
            return std::format("v[{}:{}]", o.a, std::uint32_t{o.a} + std::uint32_t{o.b} - 1);
        case Operand::Const: return const_name(o.a);
        case Operand::Literal: return std::format("lit(0x{:08x})", o.literal);
        case Operand::Attr: {
            static const char CHANS[4] = {'x', 'y', 'z', 'w'};
            return std::format("attr{}.{}", o.a, CHANS[o.b & 3]);
        }
    }
    return {};
}

std::string join(const std::vector<std::string>& ops, std::string_view sep = ", ") {
    std::string out;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (i > 0) {
            out += sep;
        }
        out += ops[i];
    }
    return out;
}


void flags(std::string& out, const Cache& cache, bool dlc_first) {
    if (dlc_first && cache.dlc) {
        out += " dlc";
    }
    if (cache.glc) {
        out += " glc";
    }
    if (cache.slc) {
        out += " slc";
    }
    if (!dlc_first && cache.dlc) {
        out += " dlc";
    }
    if (!out.empty() && out.front() == ' ') {
        out.erase(0, 1);
    }
}

std::string label(const Instruction& inst, std::uint16_t imm) {
    std::int64_t target = static_cast<std::int64_t>(inst.offset) + 4 * static_cast<std::int64_t>(inst.words.size()) +
                          4 * static_cast<std::int64_t>(static_cast<std::int16_t>(imm));

    return std::format("label_{:04X}", static_cast<std::uint64_t>(target));
}

std::string hwreg(std::uint16_t imm) {
    unsigned id = imm & 0x3F, offset = imm >> 6 & 0x1F, size = (imm >> 11 & 0x1F) + 1;
    std::string name;
    switch (id) {
        case 1: name = "HW_REG_MODE"; break;
        case 20: name = "HW_REG_SHADER_FLAT_SCRATCH_LO"; break;
        case 21: name = "HW_REG_SHADER_FLAT_SCRATCH_HI"; break;
        case 25: name = "HW_REG_POPS_PACKER"; break;
        default: name = std::to_string(id); break;
    }
    if (offset == 0 && size == 32) {
        return std::format("hwreg({})", name);
    }
    return std::format("hwreg({}, {}, {})", name, offset, size);
}

std::string waitcnt(std::uint16_t imm) {
    unsigned vm = (imm & 0xF) | (imm >> 14) << 4;
    unsigned exp = imm >> 4 & 7;
    unsigned lgkm = imm >> 8 & 0x3F;
    std::vector<std::string> parts;
    if (vm != 0x3F) {
        parts.push_back(std::format("vmcnt({})", vm));
    }
    if (exp != 7) {
        parts.push_back(std::format("expcnt({})", exp));
    }
    if (lgkm != 0x3F) {
        parts.push_back(std::format("lgkmcnt({})", lgkm));
    }
    if (parts.empty() || parts.size() == 3) {
        return std::format("0x{:04x}", imm);
    }
    return join(parts, " & ");
}

std::string sendmsg(std::uint16_t imm) {
    unsigned msg = imm & 0xF, op = imm >> 4 & 7, stream = imm >> 8 & 3;
    std::string msg_name;
    switch (msg) {
        case 2: msg_name = "MSG_GS"; break;
        case 3: msg_name = "MSG_GS_DONE"; break;
        case 7: msg_name = "MSG_ORDERED_PS_DONE"; break;
        case 9: msg_name = "MSG_GS_ALLOC_REQ"; break;
        default: msg_name = std::to_string(msg); break;
    }
    static const char* const OPS[4] = {"GS_OP_NOP", "GS_OP_CUT", "GS_OP_EMIT", "GS_OP_EMIT_CUT"};
    std::string op_name = op < 4 ? std::string(OPS[op]) : std::to_string(op);
    if (msg == 3 && op == 0) {
        return std::format("sendmsg({}, {})", msg_name, op_name);
    }
    return std::format("sendmsg({}, {}, {})", msg_name, op_name, stream);
}

std::string buffer_format(std::uint8_t format) {
    static const char* const NAMES[78] = {
        "INVALID",           "8_UNORM",           "8_SNORM",           "8_USCALED",          "8_SSCALED",          "8_UINT",
        "8_SINT",            "16_UNORM",          "16_SNORM",          "16_USCALED",         "16_SSCALED",         "16_UINT",
        "16_SINT",           "16_FLOAT",          "8_8_UNORM",         "8_8_SNORM",          "8_8_USCALED",        "8_8_SSCALED",
        "8_8_UINT",          "8_8_SINT",          "32_UINT",           "32_SINT",            "32_FLOAT",           "16_16_UNORM",
        "16_16_SNORM",       "16_16_USCALED",     "16_16_SSCALED",     "16_16_UINT",         "16_16_SINT",         "16_16_FLOAT",
        "10_11_11_UNORM",    "10_11_11_SNORM",    "10_11_11_USCALED",  "10_11_11_SSCALED",   "10_11_11_UINT",      "10_11_11_SINT",
        "10_11_11_FLOAT",    "11_11_10_UNORM",    "11_11_10_SNORM",    "11_11_10_USCALED",   "11_11_10_SSCALED",   "11_11_10_UINT",
        "11_11_10_SINT",     "11_11_10_FLOAT",    "10_10_10_2_UNORM",  "10_10_10_2_SNORM",   "10_10_10_2_USCALED", "10_10_10_2_SSCALED",
        "10_10_10_2_UINT",   "10_10_10_2_SINT",   "2_10_10_10_UNORM",  "2_10_10_10_SNORM",   "2_10_10_10_USCALED", "2_10_10_10_SSCALED",
        "2_10_10_10_UINT",   "2_10_10_10_SINT",   "8_8_8_8_UNORM",     "8_8_8_8_SNORM",      "8_8_8_8_USCALED",    "8_8_8_8_SSCALED",
        "8_8_8_8_UINT",      "8_8_8_8_SINT",      "32_32_UINT",        "32_32_SINT",         "32_32_FLOAT",        "16_16_16_16_UNORM",
        "16_16_16_16_SNORM", "16_16_16_16_USCALED", "16_16_16_16_SSCALED", "16_16_16_16_UINT", "16_16_16_16_SINT", "16_16_16_16_FLOAT",
        "32_32_32_UINT",     "32_32_32_SINT",     "32_32_32_FLOAT",    "32_32_32_32_UINT",   "32_32_32_32_SINT",   "32_32_32_32_FLOAT",
    };
    if (format < 78) {
        return std::string("BUF_FMT_") + NAMES[format];
    }
    return std::to_string(format);
}

std::string valu(std::string_view name, const Valu& v, Encoding encoding) {
    auto bit = [](std::uint8_t mask, std::size_t i) { return (mask >> i & 1) != 0; };
    bool mix = name.starts_with("v_fma_mix");
    auto src = [&](std::size_t i) {
        const Operand& o = v.src[i];
        std::string s = text(o);
        if (v.sdwa && bit(v.sdwa->sext, i)) {
            s = "sext(" + s + ")";
        }
        if (encoding == Encoding::Vop3p) {
            if (mix) {
                if (bit(v.opsel_hi, i)) {
                    s = std::string(bit(v.opsel, i) ? "sel_hi" : "sel_lo") + "(" + s + ")";
                }
                if (bit(v.neg_hi, i)) {
                    s = "abs(" + s + ")";
                }
                if (bit(v.neg, i)) {
                    s = "-" + s;
                }
            } else {
                if (bit(v.neg, i) && bit(v.neg_hi, i)) {
                    s = "-" + s;
                }
                bool lo = bit(v.opsel, i), hi = bit(v.opsel_hi, i);
                if (!lo && hi) {
                } else if (!lo && !hi) {
                    s = "sel_lo(" + s + ")";
                } else if (lo && hi) {
                    s = "sel_hi(" + s + ")";
                } else {
                    s = "sel_hi_lo(" + s + ")";
                }
            }
        } else {
            if (bit(v.abs, i)) {
                s = "abs(" + s + ")";
            }
            if (bit(v.neg, i)) {
                s = "-" + s;
            }

            if (bit(v.opsel, i) && !name.starts_with("v_permlane")) {
                s = "sel_hi(" + s + ")";
            }
        }
        return s;
    };
    std::vector<std::string> ops;
    if (v.dst) {
        ops.push_back(text(*v.dst));
    }
    if (v.carry) {
        ops.push_back(text(*v.carry));
    }
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < v.src.size(); ++i) {
        order.push_back(i);
    }
    if (name.starts_with("v_interp_") && order.size() > 1) {

        std::swap(order[0], order[1]);
    }
    for (std::size_t i : order) {
        ops.push_back(src(i));
    }
    std::string out = join(ops);
    auto suffix = [&](std::string_view s) {
        out += ' ';
        out += s;
    };
    if (!v.src.empty() && v.src[0].kind == Operand::Attr && v.src[0].high) {
        suffix("high");
    }
    static const char* const OMOD[4] = {"", "mul:2", "mul:4", "div:2"};
    std::string_view omod = OMOD[v.omod & 3];
    if (name.starts_with("v_permlane")) {
        if (v.opsel & 1) {
            suffix("fi:1");
        }
        if (v.opsel & 2) {
            suffix("bound_ctrl:1");
        }
    }
    if (v.sdwa) {
        const Sdwa& sdwa = *v.sdwa;
        if (v.clamp) {
            suffix("clamp");
        }
        if (!omod.empty()) {
            suffix(omod);
        }
        static const char* const SEL[8] = {"BYTE_0", "BYTE_1", "BYTE_2", "BYTE_3", "WORD_0", "WORD_1", "DWORD", "?"};
        if (sdwa.dst_sel != 6) {
            suffix(std::format("dst_sel:{}", SEL[sdwa.dst_sel & 7]));
        }
        if (sdwa.dst_unused != 0) {
            static const char* const UNUSED[4] = {"UNUSED_PAD", "UNUSED_SEXT", "UNUSED_PRESERVE", "?"};
            suffix(std::format("dst_unused:{}", UNUSED[sdwa.dst_unused & 3]));
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (sdwa.src_sel[i] != 6) {
                suffix(std::format("src{}_sel:{}", i, SEL[sdwa.src_sel[i] & 7]));
            }
        }
    } else {
        if (!omod.empty()) {
            suffix(omod);
        }
        if (v.clamp) {
            suffix("clamp");
        }
    }
    if (v.dpp) {
        const Dpp& d = *v.dpp;
        if (d.kind == Dpp::Row) {
            unsigned ctrl = d.ctrl;
            std::string c;
            if (ctrl <= 0x0FF) {
                c = std::format("quad_perm:[{},{},{},{}]", ctrl & 3, ctrl >> 2 & 3, ctrl >> 4 & 3, ctrl >> 6 & 3);
            } else if (ctrl >= 0x101 && ctrl <= 0x10F) {
                c = std::format("row_shl:{}", ctrl & 0xF);
            } else if (ctrl >= 0x111 && ctrl <= 0x11F) {
                c = std::format("row_shr:{}", ctrl & 0xF);
            } else if (ctrl >= 0x121 && ctrl <= 0x12F) {
                c = std::format("row_ror:{}", ctrl & 0xF);
            } else if (ctrl == 0x140) {
                c = "row_mirror";
            } else if (ctrl == 0x141) {
                c = "row_half_mirror";
            } else if (ctrl >= 0x150 && ctrl <= 0x15F) {
                c = std::format("row_share:{}", ctrl & 0xF);
            } else if (ctrl >= 0x160 && ctrl <= 0x16F) {
                c = std::format("row_xmask:{}", ctrl & 0xF);
            } else {
                c = std::format("dpp_ctrl:0x{:x}", ctrl);
            }
            suffix(c);
            if (d.row_mask != 0xF) {
                suffix(std::format("row_mask:0x{:x}", d.row_mask));
            }
            if (d.bank_mask != 0xF) {
                suffix(std::format("bank_mask:0x{:x}", d.bank_mask));
            }
            if (d.bound_ctrl) {
                suffix("bound_ctrl:1");
            }
            if (d.fi) {
                suffix("fi:1");
            }
        } else {
            std::vector<std::string> lanes;
            for (unsigned i = 0; i < 8; ++i) {
                lanes.push_back(std::to_string(d.sel >> (3 * i) & 7));
            }
            suffix(std::format("dpp8:[{}]", join(lanes, ",")));
            if (d.fi) {
                suffix("fi:1");
            }
        }
    }
    return out;
}

struct OperandsOf {
    const Instruction& inst;
    std::string_view name;
    Encoding encoding;

    std::string operator()(const SopFields& f) const {
        std::vector<std::string> list;
        if (f.dst) {
            list.push_back(text(*f.dst));
        }
        for (const auto& o : f.src) {
            list.push_back(text(o));
        }
        std::string regs = join(list);
        if (!f.imm) {
            return regs;
        }
        std::uint16_t imm = *f.imm;
        if (name == "s_getreg_b32") {
            return regs + ", " + hwreg(imm);
        }
        if (name == "s_setreg_b32" || name == "s_setreg_imm32_b32") {
            return hwreg(imm) + ", " + regs;
        }
        if (name == "s_call_b64") {
            return regs + ", " + label(inst, imm);
        }
        if (regs.empty()) {
            return std::format("0x{:04x}", imm);
        }
        return std::format("{}, 0x{:04x}", regs, imm);
    }

    std::string operator()(const SoppFields& f) const {
        if (name == "s_endpgm" || name == "s_barrier" || name == "s_code_end" || name == "s_endpgm_saved" || name == "s_endpgm_ordered_ps_done" ||
            name == "s_wakeup" || name == "s_icache_inv" || name == "s_ttracedata") {
            return {};
        }
        if (name == "s_branch" || name.starts_with("s_cbranch_")) {
            return label(inst, f.imm);
        }
        if (name == "s_waitcnt") {
            return waitcnt(f.imm);
        }
        if (name == "s_sendmsg" || name == "s_sendmsghalt") {
            return sendmsg(f.imm);
        }
        return std::format("0x{:04x}", f.imm);
    }

    std::string operator()(const SmemFields& f) const {
        std::string out;
        if (f.data) {
            bool null = f.soffset.kind == Operand::Scalar && f.soffset.a == NULL_CODE;
            std::string off;
            if (f.offset == 0) {
                off = text(f.soffset);
            } else if (null) {
                off = std::format("0x{:06x}", f.offset & 0x1F'FFFF);
            } else {
                off = std::format("{} offset:0x{:06x}", text(f.soffset), f.offset & 0x1F'FFFF);
            }
            out = text(*f.data) + ", " + text(f.base) + ", " + off;
        }
        flags(out, f.cache, false);
        return out;
    }

    std::string operator()(const Valu& v) const { return valu(name, v, encoding); }

    std::string operator()(const VintrpFields& f) const {
        std::string src;
        if (name == "v_interp_mov_f32" && f.src.kind == Operand::Const) {
            static const char* const P[3] = {"p10", "p20", "p0"};
            src = f.src.a < 3 ? P[f.src.a] : "p?";
        } else {
            src = text(f.src);
        }
        return text(f.dst) + ", " + src + ", " + text(f.attr);
    }

    std::string operator()(const DsFields& f) const {
        std::vector<std::string> list;
        if (f.dst) {
            list.push_back(text(*f.dst));
        }
        if (f.addr) {
            list.push_back(text(*f.addr));
        }
        for (const auto& o : f.data) {
            list.push_back(text(o));
        }
        std::string out = join(list);
        if (ds_pair(name)) {
            if (f.offset0 != 0) {
                out += std::format(" offset0:{}", f.offset0);
            }
            if (f.offset1 != 0) {
                out += std::format(" offset1:{}", f.offset1);
            }
        } else {
            unsigned offset = unsigned{f.offset1} << 8 | f.offset0;
            if (offset != 0) {
                out += std::format(" offset:{}", offset);
            }
        }
        if (f.gds) {
            out += " gds";
        }
        return out;
    }

    std::string operator()(const BufferFields& f) const {
        std::string out;
        if (f.data) {
            out = text(*f.data) + ", " + text(f.addr) + ", " + text(f.rsrc) + ", " + text(f.soffset);
            if (f.idxen) {
                out += " idxen";
            }
            if (f.offen) {
                out += " offen";
            }
            if (f.offset != 0) {
                out += std::format(" offset:{}", f.offset);
            }
            if (f.format) {
                out += " format:[" + buffer_format(*f.format) + "]";
            }
        }
        flags(out, f.cache, false);
        if (f.lds) {
            out += " lds";
        }
        if (f.tfe) {
            out += " tfe";
        }
        return out;
    }

    std::string operator()(const ImageFields& f) const {
        std::string addr;
        if (f.addr.size() == 1) {
            addr = text(f.addr[0]);
        } else {
            std::vector<std::string> many;
            for (const auto& o : f.addr) {
                many.push_back(text(o));
            }
            addr = "[" + join(many, ",") + "]";
        }
        std::vector<std::string> list{text(f.data), addr, text(f.rsrc)};
        if (f.sampler) {
            list.push_back(text(*f.sampler));
        }
        std::string out = join(list);
        if (f.dmask != 1) {
            out += std::format(" dmask:0x{:x}", f.dmask);
        }
        static const char* const DIMS[8] = {"1D", "2D", "3D", "CUBE", "1D_ARRAY", "2D_ARRAY", "2D_MSAA", "2D_MSAA_ARRAY"};
        out += std::string(" dim:SQ_RSRC_IMG_") + DIMS[f.dim & 7];
        if (f.r128) {
            out += " r128";
        }
        if (f.unorm) {
            out += " unorm";
        }
        flags(out, f.cache, true);
        if (f.a16) {
            out += " a16";
        }
        if (f.d16) {
            out += " d16";
        }
        if (f.tfe) {
            out += " tfe";
        }
        if (f.lwe) {
            out += " lwe";
        }
        return out;
    }

    std::string operator()(const FlatFields& f) const {
        std::vector<std::string> list;
        if (f.dst) {
            list.push_back(text(*f.dst));
        }
        list.push_back(f.addr ? text(*f.addr) : std::string("off"));
        if (f.data) {
            list.push_back(text(*f.data));
        }
        if (f.saddr) {
            list.push_back(text(*f.saddr));
        }
        std::string out = join(list);
        if (f.offset != 0) {
            out += std::format(" inst_offset:{}", f.offset);
        }
        flags(out, f.cache, false);
        if (f.lds) {
            out += " lds";
        }
        return out;
    }

    std::string operator()(const ExpFields& f) const {
        unsigned t = f.target;
        std::string target;
        if (t <= 7) {
            target = std::format("mrt{}", t);
        } else if (t == 8) {
            target = "mrtz";
        } else if (t == 9) {
            target = "null";
        } else if (t >= 12 && t <= 15) {
            target = std::format("pos{}", t - 12);
        } else if (t == 20) {
            target = "prim";
        } else if (t >= 32 && t <= 63) {
            target = std::format("param{}", t - 32);
        } else {
            target = std::format("<target {}>", t);
        }
        std::vector<std::string> list{target};
        for (const auto& s : f.src) {
            list.push_back(s ? text(*s) : std::string("off"));
        }
        std::string out = join(list);
        if (f.done) {
            out += " done";
        }
        if (f.compr) {
            out += " compr";
        }
        if (f.vm) {
            out += " vm";
        }
        return out;
    }
};

std::string root_type(std::uint32_t r) {
    switch (r) {
        case 0: return "Constants";
        case 1: return "Descriptor";
        case 3: return "Table";
        case 4: return "ImmediateSampler";
        case 5: return "ResourceHeap";
        case 6: return "SamplerHeap";
        default: return std::format("Other({})", r);
    }
}

std::string resource_type(std::uint8_t k) {
    switch (k) {
        case 0: return "Srv";
        case 1: return "Sampler";
        case 2: return "Cbv";
        case 3: return "Uav";
        default: return std::format("Other({})", k);
    }
}

}   

std::string instruction_text(const Instruction& i, bool wave64) {
    std::string_view name = i.name();
    if (name.empty()) {
        name = "?";
    }
    std::string ops = std::visit(OperandsOf{i, name, i.encoding}, fields(i, wave64));
    if (ops.empty()) {
        return std::string(name);
    }
    return std::string(name) + " " + ops;
}

std::string debug_text(const RootParam& p) {
    return std::format("RootParam {{ root: {}, table_offset_dwords: {}, sgprs: {}, base_register: {}, user_data: {}, slot: {} }}", root_type(p.root),
                       p.table_offset_dwords, p.sgprs, p.base_register, p.user_data, p.slot);
}

std::string debug_text(const UsedResource& u) {
    return std::format(
        "UsedResource {{ kind: {}, root: {}, table_offset_dwords: {}, lower_bound: {}, stride: {}, dwords: {}, user_data: {}, slot: {} }}",
        resource_type(u.kind), root_type(u.root), u.table_offset_dwords, u.lower_bound, u.stride, u.dwords, u.user_data, u.slot);
}

}   
