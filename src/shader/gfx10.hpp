// SPDX-License-Identifier: GPL-3.0-or-later
// GFX10 machine code (`gfx10/mod.rs`, `gfx10/fields.rs`; DanziLoader /
// victory-perf, see NOTICE.md): instruction boundaries, encodings, opcode
// names, and the operand fields of each encoding. Instructions are found by
// following control flow from offset 0. Errors are the reference's.
//
// Port's choices: an instruction's dwords and an operand list are kept
// inline (no allocation per instruction); `Fields` is a variant of the
// reference's enum payloads.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace danzi::shader {

enum class Encoding : std::uint8_t {
    Sop2,
    Sopk,
    Sop1,
    Sopc,
    Sopp,
    Smem,
    Vop2,
    Vop1,
    Vopc,
    Vop3,
    Vop3p,
    Vintrp,
    Ds,
    Mubuf,
    Mtbuf,
    Mimg,
    Exp,
    Flat,
    Global,
    Scratch
};


std::string_view opcode_name(Encoding e, std::uint16_t op);


template <class T, std::size_t N>
struct Inline {
    std::array<T, N> items{};
    std::uint8_t count = 0;
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    void push_back(const T& v) { items[count++] = v; }
    void clear() { count = 0; }
    T& operator[](std::size_t i) { return items[i]; }
    const T& operator[](std::size_t i) const { return items[i]; }
    T* begin() { return items.data(); }
    T* end() { return items.data() + count; }
    const T* begin() const { return items.data(); }
    const T* end() const { return items.data() + count; }
    const T* data() const { return items.data(); }
    T& back() { return items[count - 1]; }
    const T& back() const { return items[count - 1]; }
    const T& front() const { return items[0]; }
    bool operator==(const Inline& o) const {
        if (count != o.count) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (!(items[i] == o.items[i])) {
                return false;
            }
        }
        return true;
    }
};

struct Instruction {

    std::size_t offset = 0;
    Encoding encoding = Encoding::Sopp;
    std::uint16_t opcode = 0;

    Inline<std::uint32_t, 6> words;
    std::string_view name() const { return opcode_name(encoding, opcode); }
    bool operator==(const Instruction&) const = default;
};


struct Operand {
    enum Kind : std::uint8_t { Scalar, Vector, Const, Literal, Attr } kind = Const;

    std::uint8_t a = 0;

    std::uint8_t b = 0;

    bool high = false;

    std::uint32_t literal = 0;

    static Operand scalar(std::uint8_t code, std::uint8_t dwords) { return {Scalar, code, dwords}; }
    static Operand vector(std::uint32_t reg, std::uint8_t dwords) { return {Vector, static_cast<std::uint8_t>(reg), dwords}; }
    static Operand constant(std::uint8_t code) { return {Const, code}; }
    static Operand lit(std::uint32_t v) { return {Literal, 0, 0, false, v}; }
    static Operand attr(std::uint8_t index, std::uint8_t chan, bool high) { return {Attr, index, chan, high}; }
    bool operator==(const Operand& o) const {
        return kind == o.kind && a == o.a && b == o.b && high == o.high && literal == o.literal;
    }
};


constexpr std::uint8_t NULL_CODE = 125, VCC_CODE = 106, EXEC_CODE = 126;

using Opt = std::optional<Operand>;
using Ops = Inline<Operand, 16>;

struct Cache {
    bool glc = false, slc = false, dlc = false;
    bool operator==(const Cache&) const = default;
};

struct Sdwa {
    std::uint8_t dst_sel, dst_unused;
    std::uint8_t src_sel[2];
    std::uint8_t sext;
    bool operator==(const Sdwa& o) const {
        return dst_sel == o.dst_sel && dst_unused == o.dst_unused && src_sel[0] == o.src_sel[0] && src_sel[1] == o.src_sel[1] && sext == o.sext;
    }
};

struct Dpp {
    enum Kind : std::uint8_t { Row, Lanes } kind;

    std::uint16_t ctrl = 0;
    std::uint8_t row_mask = 0, bank_mask = 0;
    bool bound_ctrl = false;

    std::uint32_t sel = 0;
    bool fi = false;
    bool operator==(const Dpp&) const = default;
};


struct Valu {
    Opt dst, carry;
    Ops src;
    std::uint8_t neg = 0, abs = 0, neg_hi = 0, opsel = 0, opsel_hi = 0;
    bool clamp = false;
    std::uint8_t omod = 0;
    std::optional<Sdwa> sdwa;
    std::optional<Dpp> dpp;
    bool operator==(const Valu&) const = default;
};

struct SopFields {
    Opt dst;
    Ops src;
    std::optional<std::uint16_t> imm;
};
struct SoppFields {
    std::uint16_t imm;
};
struct SmemFields {
    Opt data;
    Operand base;
    std::int32_t offset;
    Operand soffset;
    Cache cache;
};
struct VintrpFields {
    Operand dst, src, attr;
};
struct DsFields {
    Opt dst, addr;
    Ops data;
    std::uint8_t offset0, offset1;
    bool gds;
};
struct BufferFields {
    Opt data;
    Operand addr, rsrc, soffset;
    std::uint16_t offset;
    bool offen, idxen;
    std::optional<std::uint8_t> format;
    bool lds, tfe;
    Cache cache;
};
struct ImageFields {
    Operand data;
    Ops addr;
    Operand rsrc;
    Opt sampler;
    std::uint8_t dmask, dim;
    bool unorm, r128, a16, d16, tfe, lwe;
    Cache cache;
};
struct FlatFields {
    Opt dst, addr, data, saddr;
    std::int16_t offset;
    bool lds;
    Cache cache;
};
struct ExpFields {
    std::uint8_t target;
    Opt src[4];
    bool compr, done, vm;
};

using Fields = std::variant<SopFields, SoppFields, SmemFields, Valu, VintrpFields, DsFields, BufferFields, ImageFields, FlatFields, ExpFields>;


Fields fields(const Instruction& inst, bool wave64);


bool ds_pair(std::string_view name);


std::vector<Instruction> decode_checked(std::span<const std::uint8_t> code);


struct Flow {
    enum Kind : std::uint8_t { Next, Stop, Jump, Branch } kind = Next;
    std::size_t target = 0;
    bool operator==(const Flow&) const = default;
};


template <class Before>
Flow control(const Instruction& inst, std::size_t len, Before before);


Flow flow_of(const Instruction& inst, std::size_t next, std::size_t len);
std::optional<std::size_t> far_jump_with(const Instruction* (*before)(const void*, std::size_t), const void* ctx, const Instruction& setpc,
                                         std::size_t len);

template <class Before>
Flow control(const Instruction& inst, std::size_t len, Before before) {
    Flow f = flow_of(inst, inst.offset + 4 * inst.words.size(), len);
    if (f.kind == Flow::Stop && inst.name() == "s_setpc_b64") {
        auto thunk = [](const void* ctx, std::size_t at) -> const Instruction* { return (*static_cast<const Before*>(ctx))(at); };
        auto target = far_jump_with(thunk, &before, inst, len);
        return target ? Flow{Flow::Jump, *target} : Flow{Flow::Stop};
    }
    return f;
}

}   
