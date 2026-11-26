// SPDX-License-Identifier: GPL-3.0-or-later
// Resource tracking (`resource.rs`; DanziLoader / victory-perf, see
// NOTICE.md): where each descriptor a program uses comes from, in terms of
// the root signature. Each descriptor operand and scalar memory address is
// traced through the SSA values back to the root parameters, by data flow:
// copies, phis that agree, and additions.
#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ir.hpp"
#include "native.hpp"
#include "structure.hpp"

namespace danzi::shader::resource {


using Site = std::pair<std::size_t, std::size_t>;


struct Address {

    std::size_t param = 0;
    std::uint32_t offset = 0;
    std::optional<ir::Value> dynamic;
    bool operator==(const Address&) const = default;
};


constexpr std::size_t CODE = SIZE_MAX;

constexpr std::size_t MEMORY = SIZE_MAX - 1;


struct Source {
    enum Kind : std::uint8_t { Loaded, Built, Constant, Phi, Select, Fetched, Lanes, LanePhi } kind = Constant;

    Site load{};
    std::uint8_t first = 0;

    Address address{};
    std::uint32_t bits = 0;
    std::uint32_t words[2] = {};

    std::vector<std::uint32_t> consts;

    std::size_t block = 0;

    ir::Value value{};


    std::vector<Source> args;
    std::vector<bool> present;


    std::vector<const Source*> leaves() const;
    bool operator==(const Source& o) const;
};

enum class Role : std::uint8_t { Resource, Sampler };

struct Descriptor {
    Site site;
    Role role;
    Source source;
};

struct RootIndexedView {
    std::uint8_t table_slot, constant_slot;
    std::uint32_t constant_dword, byte_offset;
    std::uint8_t shift;
};


struct BvhNodes {
    enum Kind : std::uint8_t { Resource, Memory } kind = Resource;
    std::uint32_t word1 = 0, word3 = 0;
};


struct Waterfall {
    std::vector<Site> compares;
    std::vector<ir::Value> reads;
    std::vector<Site> loads;
};

struct Resources {

    std::vector<std::pair<Site, Address>> scalar;
    std::vector<Descriptor> descriptors;
    std::vector<RootIndexedView> root_indexed_views;

    std::vector<std::tuple<ir::Value, std::size_t, std::uint32_t>> params;
    std::vector<Site> lane_copied;
    Waterfall waterfall;
    std::vector<std::pair<Site, BvhNodes>> bvh;
};


Resources track(const ir::Program& p, const std::vector<RootParam>& layout, HwStage stage, const structure::Structured* lanes);


std::optional<std::uint32_t> inline_constant(std::uint8_t code);

}   
