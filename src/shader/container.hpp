// SPDX-License-Identifier: GPL-3.0-or-later
// The PC container around emitted DXIL (`emit/container.rs`; DanziLoader /
// victory-perf, see NOTICE.md): feature flags (`SFI0`), signatures (`ISG1`,
// `OSG1`, `PSG1`), pipeline state validation (`PSV0`, runtime info version
// 2) and the program, laid out as validator 1.6 lays them out, with the
// container hash D3D12 checks.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace danzi::shader::container {


struct SigElement {
    std::string name;
    std::uint32_t index = 0;

    std::uint8_t kind = 0;

    std::uint8_t comp = 0;

    std::uint8_t interp = 0;
    std::optional<std::uint32_t> row;
    std::uint8_t col = 0, cols = 0;

    std::uint8_t usage = 0;

    bool half = false;
    bool operator==(const SigElement&) const = default;

    std::uint32_t system_value() const;
    std::uint32_t sig_comp() const;
    std::uint8_t mask() const { return static_cast<std::uint8_t>(((1u << cols) - 1) << col); }
};


std::vector<std::uint8_t> signature(const std::vector<SigElement>& elements, bool input);


constexpr std::uint64_t MESH_OUTPUT_LIMIT = 32 * 1024;
std::uint64_t mesh_output_bytes(const std::vector<SigElement>& vertex, const std::vector<SigElement>& primitive, std::uint32_t max_vertices,
                                std::uint32_t max_primitives);


struct Binding {

    std::uint32_t kind, space, reg, dxil_kind;
};


struct Mesh {
    std::uint32_t max_vertices = 0, max_primitives = 0;

    std::uint8_t topology = 0;
};

struct Psv {

    std::uint8_t stage = 0;

    bool flag = false;
    const std::vector<SigElement>* inputs;
    const std::vector<SigElement>* outputs;
    const std::vector<SigElement>* prims;

    std::optional<std::pair<Mesh, std::uint32_t>> mesh;

    std::optional<std::uint32_t> geometry;
    const std::vector<Binding>* bindings;
    std::array<std::uint32_t, 3> threads{};
    std::optional<std::uint32_t> wave_size;
};

std::vector<std::uint8_t> psv(const Psv& p);


std::vector<std::uint8_t> build(const std::vector<std::pair<const char*, std::span<const std::uint8_t>>>& parts);


std::array<std::uint8_t, 16> hash(std::span<const std::uint8_t> data);

}   
