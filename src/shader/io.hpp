// SPDX-License-Identifier: GPL-3.0-or-later
// I/O raising (`io.rs`; DanziLoader / victory-perf, see NOTICE.md): what a
// program's input registers, its interpolation and its exports mean. Pixel
// inputs in `SPI_PS_INPUT_ADDR` order, vertex ids and fetched attributes,
// compute thread and group ids, a mesh program's group input; `v_interp`
// pairs; `exp` targets by signature row.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ir.hpp"
#include "native.hpp"
#include "resource.hpp"

namespace danzi::shader::io {

using resource::Site;

enum class Barycentric : std::uint8_t {
    PerspectiveSample,
    PerspectiveCenter,
    PerspectiveCentroid,
    PerspectivePullModel,
    LinearSample,
    LinearCenter,
    LinearCentroid,
};

struct Input {
    enum Kind : std::uint8_t {
        VertexId,
        PrimitiveId,
        InstanceId,

        Attribute,

        Bary,

        Position,
        FrontFace,
        Ancillary,
        SampleCoverage,
        FixedPosition,
        LineStipple,

        GroupId,

        ThreadIdInGroup,
        GroupInfo,

        MeshGroup,

        MergedWaveInfo,
    } kind = VertexId;
    std::uint8_t row = 0, channel = 0;
    Barycentric bary = Barycentric::PerspectiveSample;
    std::uint8_t index = 0;
    bool operator==(const Input&) const = default;
};


struct Attribute {
    Usage usage;
    std::uint8_t row = 0, channel = 0;

    bool high = false;
    bool operator==(const Attribute&) const = default;
};

struct Interp {
    enum Kind : std::uint8_t {


        Interpolated,

        Vertex,

        Weighted,
    } kind = Weighted;
    Attribute attribute;
    Barycentric barycentric = Barycentric::PerspectiveSample;
    std::vector<Site> first;
    std::uint8_t vertex = 0;
};

struct Target {
    enum Kind : std::uint8_t { Color, Depth, Null, Position, Param, Unread, Primitive, PrimitiveParam } kind = Null;

    std::uint8_t index = 0;

    Usage usage;
    std::uint8_t row = 0;
    bool operator==(const Target&) const = default;
};

struct Export {
    Site site;
    Target target;

    std::array<std::optional<ir::Value>, 4> values;
    bool compressed = false;

    bool done = false;
};

struct Io {
    std::vector<std::pair<ir::Value, Input>> inputs;
    std::vector<std::pair<Site, Interp>> interps;
    std::vector<Export> exports;

    std::optional<std::uint8_t> initialized_sgprs;
};


Io raise(const ir::Program& p, const NativeProgram& native);

}   
