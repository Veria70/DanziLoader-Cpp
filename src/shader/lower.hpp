// SPDX-License-Identifier: GPL-3.0-or-later
// IR to DXIL, one machine instruction at a time (`emit/lower.rs`;
// DanziLoader / victory-perf, see NOTICE.md). Every register dword is an
// `i32` (SCC an `i1`); float operations bitcast their operands. Lane masks
// exist only as the per-lane booleans of structurisation; branches and
// guards are built from them. A value the emitter cannot express is
// missing, with a reason, and only fails the program when something with
// an effect needs it.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "container.hpp"
#include "io.hpp"
#include "ir.hpp"
#include "native.hpp"
#include "resource.hpp"
#include "structure.hpp"

namespace danzi::shader::lower {


struct UserData {

    std::uint32_t index = 0;

    std::uint8_t slot = 0;

    std::uint32_t root = 0;

    std::uint32_t dword = 0;
    bool operator==(const UserData&) const = default;
};

struct Lowered {
    std::vector<std::uint8_t> dxil;

    std::uint64_t cmp_samplers = 0;
    std::vector<std::uint8_t> root_cbvs, root_srvs, root_uavs;
    std::vector<UserData> user_data;
    std::vector<std::array<std::uint32_t, 4>> samplers;

    std::optional<std::uint32_t> wave_size;
};

struct Parts {
    const NativeProgram* native;
    const ir::Program* p;
    const structure::Structured* st;
    const io::Io* io;
    const resource::Resources* res;
    std::vector<container::SigElement> inputs, outputs, prims;

    std::uint32_t mesh_stride = 0;
};


Lowered lower(const Parts& parts);


extern const char* const LANE_MASK;


extern const char* const MASK_ELSEWHERE;

constexpr std::uint32_t LOOP_LIMIT = 1u << 20;


bool reads_mask_as_data(const structure::Structured& st, const ir::Inst& i);


std::optional<std::vector<std::size_t>> branches_around(const structure::Structured& st, const std::vector<std::size_t>& needs);

bool needs_every_lane(std::string_view what);

std::vector<std::size_t> take_needs();


std::optional<std::pair<std::size_t, const char*>> wave_local(const ir::Program& p, const structure::Structured& st);


void set_host_max_wave(std::uint32_t lanes);
std::uint32_t host_max_wave();


template <class F>
auto with_local_waves(F f) -> decltype(f());
template <class F>
auto with_local_waves_any(F f) -> decltype(f());


bool& local_waves_flag();
bool& local_any_flag();

template <class F>
auto with_local_waves(F f) -> decltype(f()) {
    bool was = local_waves_flag();
    local_waves_flag() = true;
    struct Restore {
        bool was;
        ~Restore() { local_waves_flag() = was; }
    } restore{was};
    return f();
}

template <class F>
auto with_local_waves_any(F f) -> decltype(f()) {
    bool was = local_any_flag();
    local_any_flag() = true;
    struct Restore {
        bool was;
        ~Restore() { local_any_flag() = was; }
    } restore{was};
    return with_local_waves(f);
}


const char* root_type_name(std::uint32_t root, std::string& buf);


std::uint8_t group_sync();
std::uint32_t loop_limit();

}   
