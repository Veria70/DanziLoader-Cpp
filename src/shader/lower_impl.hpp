// SPDX-License-Identifier: GPL-3.0-or-later
// The lowering's state (`emit/lower.rs` `Lower`; DanziLoader / victory-perf,
// see NOTICE.md), shared by the files that port its methods.
//
// Port's choices:
// - The reference's `Result<_, Why>` is an exception here (`WhyError`),
//   thrown where the reference returns `Err(why)` and caught where it
//   handles one (`.ok()`, `match`, `unwrap_or`); `Stop` is `StopRetry` or
//   a `ShaderError`.
// - Two constants or declarations the reference makes in one expression
//   are made in separate statements, in its order (see dxil.hpp).
// - The reference's hash maps are looked up, never iterated where the
//   order would show; its ordered maps are `std::map`.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dxil.hpp"
#include "error.hpp"
#include "lower.hpp"
#include "own_bits.hpp"
#include "wave.hpp"

namespace danzi::shader::lower {

using bitcode::Attr;
using bitcode::Bin;
using bitcode::Cast;
using bitcode::Rmw;
using bitcode::Tag;
using bitcode::Term;
using bitcode::Ty;
using bitcode::V;
namespace pred = bitcode::pred;
using container::SigElement;
using dxil::Shader;
using io::Barycentric;
using io::Export;
using io::Input;
using io::Interp;
using io::Target;
using ir::Inst;
using ir::Reg;
using ir::Value;
using resource::Address;
using resource::Site;
using resource::Source;
using structure::Atom;
using structure::Bool;
using structure::LaneExit;
using structure::ValueHash;

using Why = const char*;


struct WhyError {
    Why why;
};
[[noreturn]] inline void fail(Why why) { throw WhyError{why}; }


struct StopRetry {
    Value v;
    Why why;
};

[[noreturn]] inline void stop_unsupported(std::size_t offset, Why what) { unsupported(offset, what); }


struct Eval {

    bool weights = false;
    std::uint8_t mode = 0;

    bool p0 = false;
    std::optional<V> i, j;
    static Eval of_mode(std::uint8_t m) {
        Eval e;
        e.mode = m;
        return e;
    }
};

struct Val {
    enum Kind : std::uint8_t { I32, Bit, Missing } kind = Missing;
    V v{};
    Why why = nullptr;
    static Val i32(V x) { return {I32, x, nullptr}; }
    static Val bit(V x) { return {Bit, x, nullptr}; }
    static Val missing(Why w) { return {Missing, {}, w}; }
};


enum class MaskData : std::uint8_t { Console, Host, None };

enum class Numeric : std::uint8_t { Unorm, Snorm, Uscaled, Sscaled, Uint, Sint, Float };

struct PairHash {
    std::size_t operator()(const std::pair<std::size_t, std::size_t>& p) const { return std::hash<std::size_t>{}(p.first * 0x9E3779B97F4A7C15ull ^ p.second); }
};

struct HandleKey {
    std::size_t block;
    Site site;
    std::uint8_t first;
    bool sampler;
    std::pair<std::uint32_t, std::uint32_t> props;
    bool operator==(const HandleKey&) const = default;
};
struct HandleHash {
    std::size_t operator()(const HandleKey& k) const {
        std::uint64_t h = k.block * 0x9E3779B97F4A7C15ull;
        h ^= (k.site.first * 31 + k.site.second) * 0xC2B2AE3D27D4EB4Full;
        h ^= (std::uint64_t{k.first} << 1 | (k.sampler ? 1u : 0u)) * 0x165667B19E3779F9ull;
        h ^= (std::uint64_t{k.props.first} << 32 | k.props.second) * 0x27D4EB2F165667C5ull;
        return static_cast<std::size_t>(h);
    }
};
struct StrideKey {
    std::size_t block;
    Site site;
    std::uint8_t first;
    bool operator==(const StrideKey&) const = default;
};
struct StrideHash {
    std::size_t operator()(const StrideKey& k) const {
        return static_cast<std::size_t>((k.block * 0x9E3779B97F4A7C15ull) ^ ((k.site.first * 31 + k.site.second) * 0xC2B2AE3D27D4EB4Full) ^ k.first);
    }
};


std::optional<std::vector<bool>> all_lane_blocks(const structure::Structured& st);
std::uint32_t meeting_mode();
void need(std::size_t block);
bool f16_nearest_on();
std::uint32_t inline_f16(std::uint8_t c);
std::optional<std::pair<const std::vector<std::uint32_t>*, Numeric>> buffer_format(std::uint8_t format);
std::uint32_t half_shift(std::uint32_t width);
std::optional<std::uint8_t> interp_mode(Barycentric kind);
std::optional<std::pair<std::uint8_t, std::uint8_t>> packed_half(const std::vector<SigElement>& elements,
                                                                 const std::vector<std::pair<std::uint8_t, std::optional<std::uint8_t>>>& packs,
                                                                 std::uint8_t a, std::uint8_t h);
std::vector<std::pair<std::uint8_t, std::optional<std::uint8_t>>> input_packs(const StageIo& io, const std::vector<SigElement>& inputs);
const char* root_type_name(std::uint32_t root, std::string& buf);

constexpr std::uint16_t GS_ALLOC_REQ = 9;
constexpr float TWO_PI = 6.28318530717958647692f;

class Lower {
public:
    Lower(const Parts& x, const std::unordered_map<Value, Why, ValueHash>& bad);
    Lowered run();

    Shader s;
    const Parts& x;
    const std::unordered_map<Value, Why, ValueHash>& bad;
    std::unordered_map<Value, Why, ValueHash> lane_fail;
    std::unordered_map<Value, Val, ValueHash> vals;
    std::unordered_map<Value, V, ValueHash> floats;

    std::vector<std::optional<std::size_t>> block;
    std::size_t cur = 0;
    std::unordered_map<std::pair<std::size_t, std::size_t>, V, PairHash> bools;
    std::unordered_map<Value, V, ValueHash> lane_bits;
    std::unordered_map<Value, Site, ValueHash> phi_at;
    std::unordered_set<Value, ValueHash> stale;
    std::unordered_map<Value, V, ValueHash> mask_phis;
    std::vector<std::tuple<V, std::size_t, std::size_t>> pending_masks;
    std::vector<std::tuple<V, std::size_t, std::size_t>> data_phis;
    std::unordered_set<Value, ValueHash> read_phis;
    std::unordered_map<Value, std::pair<std::size_t, std::uint32_t>, ValueHash> params;
    std::optional<V> user_data;
    std::map<std::uint32_t, UserData> user_reads;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::unordered_map<std::pair<std::size_t, std::size_t>, const Source*, PairHash> descriptors_res, descriptors_smp;
    std::unordered_map<std::pair<std::size_t, std::size_t>, Address, PairHash> scalar;


    struct LoadUses {
        std::unordered_map<std::pair<std::size_t, std::size_t>, std::vector<std::pair<Site, std::uint8_t>>, PairHash> resource;
        std::unordered_set<std::pair<std::size_t, std::size_t>, PairHash> sampler;
    };
    std::optional<LoadUses> load_uses_;
    const LoadUses& load_uses();
    mutable std::optional<std::vector<std::pair<std::uint32_t, const Inst*>>> readers_;
    std::span<const std::pair<std::uint32_t, const Inst*>> readers(Value v) const;
    std::unordered_map<std::pair<std::size_t, std::size_t>, const Export*, PairHash> exports;
    std::unordered_map<std::pair<std::size_t, std::size_t>, const Interp*, PairHash> interps;
    std::unordered_map<HandleKey, V, HandleHash> handles;
    std::unordered_map<StrideKey, V, StrideHash> strides;

    std::unordered_map<std::pair<std::size_t, std::size_t>, std::size_t, PairHash> edges;
    std::vector<bool> exported;
    std::vector<std::size_t> rpo;
    std::vector<std::optional<std::size_t>> tail;
    bool writes_uavs = false;
    std::optional<V> src0;
    bool console_lanes = false;
    std::optional<V> flat_;
    std::unordered_set<std::size_t> no_barrier;
    std::unordered_set<std::size_t> fence_only;
    bool split_lds = false;
    std::optional<std::pair<std::vector<bool>, std::vector<std::pair<bool, bool>>>> lds_meet;


    struct Exchange {
        std::uint32_t first;
        bool small;
        std::vector<bool> all;
    };
    std::optional<Exchange> exchange;
    bool local = false, local_ok = false, local_any = false;
    std::unordered_set<Value, ValueHash> uniform;
    own_bits::OwnBits own_bits;
    std::optional<V> clane, pair;
    std::optional<std::uint32_t> halves;
    std::optional<V> wave_base_;
    std::unordered_map<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>> half_outputs;
    MaskData mask_data = MaskData::None;
    bool mask_ok = false;
    std::unordered_map<Value, std::pair<V, V>, ValueHash> ballots;
    std::unordered_set<Value, ValueHash> balloting;
    std::unordered_map<Value, std::size_t, ValueHash> def_block;
    std::size_t cur_ir = 0;
    std::unordered_map<std::size_t, std::unordered_map<std::size_t, wave::Section>> sections;
    std::unordered_set<Value, ValueHash> section_out;

    std::optional<std::tuple<V, std::uint32_t, std::uint32_t>> code_table;


    std::unordered_map<std::size_t, std::pair<V, std::vector<std::pair<V, std::size_t>>>> counters;
    std::optional<std::size_t> abort;

    const ir::Program& p() const { return *x.p; }
    const structure::Structured& st() const { return *x.st; }


    V code_dword(V byte);
    void flat(const Inst& inst, std::size_t b, std::size_t i);
    void find_sections();
    void section_inst(std::size_t b, std::size_t i, std::size_t start);
    V shape(const wave::Shape& shape, bool own);
    V active_f64_sum(Value lo, Value hi, Value mask, bool high);
    V add_halves(V a, V b);
    V half_sum_here(V x);
    V half_sum(V x);


    bool exported_out(std::size_t b) const;
    bool has_done_export(std::size_t b) const;
    void inputs();
    Val input(Input in);
    bool one_dimensional() const;
    Val compute_id(const char* name, std::uint32_t opcode, std::uint8_t c);
    std::optional<std::pair<std::size_t, std::uint8_t>> find_input(std::uint8_t row, std::uint8_t channel) const;
    Val load_input(std::size_t id, std::uint8_t col);
    Val load_any_input(std::size_t id, std::uint8_t col);
    Val get(Value v);
    bool lazy_barycentric(Value v);
    Val barycentric(Barycentric kind, std::uint8_t index);
    V i32_of(Value v);
    Val user(std::size_t param, std::uint32_t dword);
    std::uint32_t user_first() const;
    V host_word(std::uint32_t root, std::uint8_t slot, std::uint32_t dword);
    V user_at(std::uint32_t index);
    bool computed_with(Value v, std::uint32_t depth) const;
    std::optional<std::pair<V, V>> view_address(Site site);
    std::pair<V, V> address_plus(V lo, V hi, std::int32_t off);
    std::pair<V, V> memory(V lo, V hi, std::optional<V> guard);
    void memory_flat(const Inst& inst, std::size_t b, std::size_t i);


    void memory_smem(const Inst& inst);
    V mask_dword(Value v);
    V operand(const Inst& inst, const Operand& op);
    V f16_operand(const Inst& inst, const Operand& op);
    V as_float(V x);
    V as_int(V f);
    static std::optional<std::pair<std::uint8_t, bool>> sdwa_src(const Inst& inst, std::size_t k);
    V select_bits(V x, std::uint8_t sel, bool sext);
    V fsrc(const Inst& inst, std::size_t k);
    V src(const Inst& inst, std::size_t k);
    V unary(std::uint32_t op, V f);
    V binary(std::uint32_t op, V a, V b);
    V max_min(bool max, V a, V b);
    V fmad(V a, V b, V c);
    V f16_toward_zero(V f);
    V f16_nearest(V f);
    V bfe(V a, V off, V width, bool is_signed);
    V is_nan_f32(V f);
    V med3(V a, V b, V third);
    V fract(V a, float threshold, float below_one);
    V trig_turns(V x, bool cosine);
    V mul_hi(V a, V b, bool is_signed);
    V legacy_mul(V a, V b);
    V boolean(Bool b, std::size_t at);
    V atom(Atom a, std::size_t at);
    std::optional<V> wave_base();
    V console_thread();
    V console_lane();
    void group_sync(std::size_t block);
    std::pair<V, V> exchange_halves(std::size_t at, V mine);
    V exchange_pair(std::uint32_t slot);
    V scc_some(Value scc);
    V wave_wide(std::size_t b, Bool when);
    bool all_lanes(std::size_t b) const;
    V lane_bit(std::size_t at, V lo, std::optional<V> hi);
    std::optional<Value> pair_high(Value lo) const;
    std::optional<V> guard(std::size_t b, std::size_t i);
    void set(std::optional<Value> v, Val val);
    void write(const Inst& inst, Reg reg, V x, std::optional<V> guard);
    void write_placed(const Inst& inst, Reg reg, V x, std::optional<V> guard);


    V old_or_zero(const Inst& inst, Reg reg);
    static std::optional<std::pair<std::uint8_t, std::uint8_t>> sdwa_dst(const Inst& inst);
    V place_bits(const Inst& inst, Reg reg, V x, std::uint8_t sel, std::uint8_t unused);
    void write_f(const Inst& inst, V f, std::optional<V> guard);
    void write_i(const Inst& inst, V x, std::optional<V> guard);
    std::optional<std::pair<std::size_t, std::size_t>> begin_guarded(std::optional<V> guard);
    std::vector<V> end_guarded(std::optional<std::pair<std::size_t, std::size_t>> at, const std::vector<std::pair<V, V>>& values);
    void cross_lane(bool quad);


    Val thread_id(std::uint8_t c);
    void mesh_prologue(container::Mesh mesh);
    V mesh_thread();
    std::uint32_t wave() const;
    V lane_index();
    V quad_pick(V x, V src);
    std::pair<V, V> first_lane(V x, std::optional<V> guard);
    V read_lane(V x, V lane);
    V float_of(Value v);
    std::vector<V> vgprs(const Inst& inst, const Operand& op, std::uint8_t n);
    void ds(const Inst& inst, std::size_t b, std::size_t i);
    V lds_float_min_max(V p, V data, bool max);
    V lds_float_pick(V mem, V data, bool max);
    void inst(std::size_t b, std::size_t i);
    void lds_meeting();
    bool dead(Value v, std::vector<Value>& seen) const;
    bool uniform_test(const Inst& inst) const;
    bool is_mask(std::optional<Value> v) const;


    Why sop(const Inst& inst);
    std::pair<V, V> add_sub_carry(std::size_t c, bool add, V a, V b, std::optional<V> cin);
    V scc_in(const Inst& inst);


    Why smem(const Inst& inst, Site site);
    Why descriptor_data(const Inst& inst, Site site);
    void code_smem(const Inst& inst, Address a);
    V cbuffer_dword(V h, V a);
    std::vector<V> format_load(V h, V off, std::uint8_t format, std::uint32_t channels);
    std::pair<std::uint32_t, std::optional<V>> built_offset(const Source& src);
    std::optional<V> root_bias(std::size_t param);
    const UsedResource* used_kind(const Source& src) const;
    V handle(Site site, const Source& src, bool sampler, std::pair<std::uint32_t, std::uint32_t> props);
    V descriptor_phi(std::size_t block, const std::vector<Source>& args, bool sampler, std::uint32_t depth);
    std::optional<V> slot_stride(const Source& src);
    V lane_heap_index(const Source& src, bool sampler);
    V fetched_index(Site load, std::uint8_t first, bool sampler);
    V heap_index(Site load, std::uint8_t first, bool sampler);


    void valu(const Inst& inst, std::size_t b, std::size_t i);
    std::pair<V, std::optional<V>> dpp(const Inst& inst, const Dpp& d);
    void valu_op(const Inst& inst, std::optional<V> guard);


    V src64(const Inst& inst, std::size_t k);
    std::pair<V, V> f64_pair(const Inst& inst, std::size_t k);
    V f64_to_f32_bits(V lo, V hi);
    V join64(V lo, V hi);
    std::pair<V, V> split64(V x);
    V half_src(const Inst& inst, std::size_t k);
    V half_fsrc(const Inst& inst, std::size_t k);
    void write_half(const Inst& inst, V x, std::optional<V> guard);
    bool valu16(const Inst& inst, std::string_view name, std::optional<V> guard);
    void valu16_op(const Inst& inst, std::string_view name, std::optional<V> guard);
    V cvt_norm16(const Inst& inst, bool is_signed);
    V packed_src(const Inst& inst, std::size_t k, bool high, bool is_float);
    void packed16(const Inst& inst, std::string_view op, std::optional<V> guard);


    V integer(const Inst& inst, std::string_view name);
    void compare(const Inst& inst, std::string_view rest);
    void lane(const Inst& inst, std::optional<bool> value);
    void lane_bits_for(const Inst& inst, V r);


    void interp(const Inst& inst, Site site);
    bool per_vertex(std::uint8_t row) const;
    bool interpolated(std::uint8_t row) const;
    V eval_channel(std::uint8_t row, std::uint8_t ch, const Eval& eval);
    V vertex_input(std::size_t id, std::uint8_t col, std::uint8_t k);
    V low_half(const Inst& inst, std::uint8_t reg, V x);


    void image(const Inst& inst, std::size_t b, std::size_t i);
    std::array<V, 3> cube_direction(V s, V t, V face);


    void buffer(const Inst& inst, std::size_t b, std::size_t i);


    void export_(std::size_t b, std::size_t i);
    void emit_indices(const Export& e, std::optional<V> guard);
    std::optional<std::uint8_t> unnamed_row() const;
    void exit(std::size_t b);
    void enter_loop(std::size_t from, std::size_t to, std::size_t via);
    V count_iteration(std::size_t to, std::uint8_t p);
    std::size_t abort_block();
    std::size_t edge(std::size_t from, std::size_t to);
    void fill_phis();


    struct NodeAt {


        bool memory = false;
        V handle{}, byte{}, low{}, high{};
    };
    struct Ray {
        V extent;
        V origin[3], direction[3], inverse[3];
    };
    void bvh(const Inst& inst, std::size_t b, std::size_t i);
    std::vector<V> bvh_node_words(const NodeAt& at, std::optional<V> base, std::uint32_t offset, std::uint32_t count);
    std::pair<V, V> bvh_address(V word0, V word1, V pointer, std::optional<V> pointer_high);
    std::array<V, 4> bvh_boxes(const NodeAt& at, V kind, const Ray& ray, bool sort);
    std::array<V, 4> bvh_triangle(const NodeAt& at, V kind, const Ray& ray);
};

}   
