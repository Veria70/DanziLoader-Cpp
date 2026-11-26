// SPDX-License-Identifier: GPL-3.0-or-later
// Port of parts of crates/victory-shader (DanziLoader / victory-perf, see
// NOTICE.md) that the GPU side needs before milestone 5 ports the
// translator: the DXBC container (`dxbc.rs`), the native GFX10 programs the
// console compiler stores in its `DXIL` part (`native.rs`: the XDAT index,
// XBCH zlib blobs, FSSM records, root layout and used-resource tables, stage
// data), and GFX10 instruction boundaries, encodings and opcode names
// (`gfx10/mod.rs`, with Mesa's opcode table).
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "gfx10.hpp"

namespace danzi::shader {


struct Part {
    std::uint32_t fourcc;
    std::span<const std::uint8_t> data;
};

std::optional<std::vector<Part>> container_parts(std::span<const std::uint8_t> data);
constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return std::uint32_t(std::uint8_t(s[0])) | std::uint32_t(std::uint8_t(s[1])) << 8 | std::uint32_t(std::uint8_t(s[2])) << 16 |
           std::uint32_t(std::uint8_t(s[3])) << 24;
}


std::optional<std::vector<std::uint8_t>> inflate_zlib_exact(std::span<const std::uint8_t> source, std::size_t expected);


std::optional<std::vector<std::uint8_t>> inflate_raw_exact(std::span<const std::uint8_t> source, std::size_t expected);


std::optional<std::size_t> inflate_zlib_into(std::span<const std::uint8_t> source, std::span<std::uint8_t> out);


enum class HwStage : std::uint8_t { Ls, Hs, Es, Gs, Vs, Ps, Ms, Cs };


enum class RootType : std::uint32_t { Constants = 0, Descriptor = 1, Table = 3, ImmediateSampler = 4, ResourceHeap = 5, SamplerHeap = 6 };

struct UsedResource {

    std::uint8_t kind;

    std::uint32_t root;
    std::uint32_t table_offset_dwords;
    std::uint32_t lower_bound;
    std::uint32_t stride;
    std::uint8_t dwords;
    std::uint8_t user_data;
    std::uint8_t slot;
};


struct RootParam {
    std::uint32_t root, table_offset_dwords, sgprs, base_register;
    std::uint8_t user_data, slot;
};


inline std::uint8_t user_sgpr(HwStage stage, std::uint8_t index) {
    switch (stage) {
        case HwStage::Ls:
        case HwStage::Hs:
        case HwStage::Es:
        case HwStage::Gs:
        case HwStage::Ms: return static_cast<std::uint8_t>(index + 8);
        default: return index;
    }
}


struct Usage {
    std::uint8_t raw = 0;
    bool generic() const { return raw == 6; }
    bool clip_distance() const { return raw == 7; }
    bool operator==(const Usage&) const = default;
};


struct PsInput {
    Usage usage;
    std::uint8_t row, mask, attr;
    bool flat, fp16, pack2;
    std::uint8_t row2;
};


struct VsInput {
    Usage usage;
    std::uint8_t row, mask, vgpr;
};


struct VsOutput {
    Usage usage;
    std::uint8_t row, param;
    bool fp16, pack2;
};


struct ExportPatch {
    std::uint8_t mrt;
    std::uint32_t offset;
    std::vector<std::uint32_t> code;
};


struct ComputeIo {
    std::uint32_t rsrc2 = 0;
    std::uint32_t threads[3] = {};
    std::uint8_t user_sgprs() const { return static_cast<std::uint8_t>(rsrc2 >> 1 & 0x1F); }


    void system_sgprs(std::uint8_t (&ids)[3], std::uint8_t& info) const {
        std::uint8_t next = user_sgprs();
        auto take = [&](std::uint32_t bit) -> std::uint8_t { return (rsrc2 >> bit & 1) ? next++ : std::uint8_t{0xFF}; };
        ids[0] = take(7);
        ids[1] = take(8);
        ids[2] = take(9);
        info = take(10);
    }

    std::uint8_t thread_id_vgprs() const { return static_cast<std::uint8_t>((rsrc2 >> 11 & 3) + 1); }

    std::uint32_t lds_bytes() const { return (rsrc2 >> 15 & 0x1FF) * 512; }

    std::uint8_t initialized_sgprs() const {
        std::uint8_t ids[3], info;
        system_sgprs(ids, info);
        int end = -1;
        for (std::uint8_t s : {ids[0], ids[1], ids[2], info}) {
            if (s != 0xFF) {
                end = std::max(end, int{s} + 1);
            }
        }
        int e = end < 0 ? 0 : end;
        return static_cast<std::uint8_t>(std::max(e, int{user_sgprs()}) + (rsrc2 & 1));
    }
};


struct MeshIo {
    std::vector<VsOutput> outputs;
    std::uint32_t clip_cntl = 0, threads = 0, max_vertices = 0, max_primitives = 0;
    std::uint8_t topology = 0;
    std::uint32_t lds_bytes = 0;
};


struct StageIo {
    enum Kind : std::uint8_t { None, Vs, Ps, Cs, Ms } kind = None;

    std::vector<VsInput> vs_inputs;
    std::vector<VsOutput> vs_outputs;

    std::vector<PsInput> ps_inputs;
    std::uint32_t input_addr = 0, db_shader_control = 0;
    std::vector<ExportPatch> patches;

    ComputeIo cs;
    MeshIo ms;
};

struct NativeProgram {
    HwStage stage;
    bool wave64;
    std::uint16_t vgprs;
    std::vector<RootParam> layout;
    std::vector<UsedResource> used;
    StageIo io;
    std::vector<std::uint8_t> code;

    std::uint32_t rsrc2 = 0;
    std::uint8_t user_sgprs() const { return static_cast<std::uint8_t>(rsrc2 >> 1 & 0x1F); }


    std::vector<std::uint8_t> patched_code() const;
};


bool is_spilled(const NativeProgram& p);


std::optional<std::vector<NativeProgram>> native_programs(std::span<const std::uint8_t> part);

std::vector<NativeProgram> native_programs_checked(std::span<const std::uint8_t> part);


std::optional<std::vector<Instruction>> decode(std::span<const std::uint8_t> code);


bool program_stores(const NativeProgram& program);
bool program_image_stores(const NativeProgram& program);

}   
