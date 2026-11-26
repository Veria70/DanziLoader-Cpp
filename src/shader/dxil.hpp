// SPDX-License-Identifier: GPL-3.0-or-later
// A DXIL vertex, pixel, compute, mesh or geometry shader under construction
// (`emit/shader.rs`; DanziLoader / victory-perf, see NOTICE.md): the module,
// the `dx.op` declarations it calls, its signatures and bound resources,
// and the metadata and container that wrap it up (SM 6.6, validator 1.6).
//
// Port's rule, here and in the lowering: where the reference makes two
// constants or declarations in one expression (an array literal, a tuple,
// arguments), the port makes them in separate statements in the same
// order, because C++ leaves the order of function arguments open and the
// numbering written follows the order made.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "bitcode.hpp"
#include "container.hpp"

namespace danzi::shader::dxil {

using bitcode::Attr;
using bitcode::Md;
using bitcode::Module;
using bitcode::Ty;
using bitcode::V;
using container::SigElement;


constexpr std::uint32_t USER_DATA_SPACE = 0;

constexpr std::uint32_t SAMPLER_SPACE = 0;

constexpr std::uint32_t ROOT_CBV_SPACE = 1;

constexpr std::uint8_t SLOT_STRIDES = 255;
constexpr std::uint32_t SLOT_STRIDES_COUNT = 1u << 21;

constexpr std::uint8_t SLOT_MEMORY = 254;
constexpr std::uint8_t SLOT_FAULTS = 253;
constexpr std::uint32_t MEMORY_PAGES = 1u << 21;
constexpr std::uint32_t MEMORY_FAULTS = 4096;

constexpr std::uint32_t USER_MEMORY_BASE = 0x100;
constexpr std::uint32_t USER_VIEW_ADDRESS = 0x101;
constexpr std::uint32_t USER_VIEW_BIAS = 0x103;
constexpr std::uint32_t USER_GROUP_BASE = 0x104;


namespace flags {
constexpr std::uint64_t RAW_BUFFERS = 1ull << 4;
constexpr std::uint64_t DOUBLES = 1ull << 2;
constexpr std::uint64_t STENCIL_REF = 1ull << 11;
constexpr std::uint64_t TYPED_UAV_LOADS = 1ull << 13;
constexpr std::uint64_t WAVE_OPS = 1ull << 19;
constexpr std::uint64_t BARYCENTRICS = 1ull << 22;
constexpr std::uint64_t INT64 = 1ull << 20;
constexpr std::uint64_t ATOMIC_TYPED64 = 1ull << 27;
constexpr std::uint64_t ATOMIC_HEAP64 = 1ull << 32;
constexpr std::uint64_t RESOURCE_HEAP = 1ull << 30;
constexpr std::uint64_t SAMPLER_HEAP = 1ull << 31;
}   

struct Types {
    Ty void_, i1, i32, f32, handle, props, bind, cbuf_ret, res_ret;
};

using container::Mesh;


struct Geometry {
    std::uint32_t max_vertices = 0;
};


void set_view_heap_limit(std::uint32_t null_slot);
std::uint32_t view_heap_limit();


std::uint32_t shader_opt();
std::uint32_t parse_shader_opt(const char* value);

struct Shader {
    explicit Shader(bool pixel);

    Module m;
    Types t;
    bool pixel;
    std::vector<SigElement> inputs, outputs, prims;
    std::optional<Mesh> mesh;
    std::optional<Geometry> geometry;

    std::uint32_t user_data = 0;

    std::uint32_t samplers = 0;

    std::uint64_t cmp_samplers = 0;
    std::vector<std::pair<std::uint8_t, V>> root_cbvs, root_srvs, root_uavs;
    std::uint64_t flags = 0;
    std::uint32_t nonuniform_handles = 0;
    std::optional<std::array<std::uint32_t, 3>> threads;
    std::optional<std::uint32_t> wave_size;
    std::uint32_t lds_dwords = 0;
    std::size_t entry = 0;


    V lds();

    V op(std::size_t block, std::string_view name, Attr attr, Ty ret, std::uint32_t opcode, const std::vector<V>& args);
    V user_data_handle();
    V root_cbv_handle(std::uint8_t slot);
    V root_srv_handle(std::uint8_t slot);
    V root_uav_handle(std::uint8_t slot);
    V sampler_handle(std::size_t block, std::uint32_t n);
    V annotate(std::size_t block, V h, std::uint32_t props0, std::uint32_t props1);
    V heap_handle(std::size_t block, V index, bool sampler, bool nonuniform, std::pair<std::uint32_t, std::uint32_t> props);

    std::vector<std::uint8_t> finish();

private:
    std::optional<V> lds_;
    V main_;
    std::string name_buf_;

    void fix_user_data_size(std::uint32_t size);
    std::optional<Md> signature_md(const std::vector<SigElement>& elements);
    Md i32c(std::uint32_t v);
};

}   
