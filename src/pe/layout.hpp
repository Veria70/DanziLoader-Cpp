// SPDX-License-Identifier: GPL-3.0-or-later
// Raw byte offsets of PE32+ header fields, for code that patches images
// (`victory-pe/src/layout.rs`; DanziLoader / victory-perf, see NOTICE.md).
// All offsets are relative to the start of the structure named in the
// constant's prefix, as in the published PE/COFF specification.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace danzi::pe::layout {


constexpr std::size_t DOS_E_LFANEW = 0x3C;

constexpr std::size_t NT_SIGNATURE_SIZE = 4;

constexpr std::size_t FILE_HEADER_SIZE = 20;

constexpr std::size_t FILE_HEADER_CHARACTERISTICS = 18;

constexpr std::size_t OPT_ADDRESS_OF_ENTRY_POINT = 16;
constexpr std::size_t OPT_CHECKSUM = 64;
constexpr std::size_t OPT_DLL_CHARACTERISTICS = 70;
constexpr std::size_t OPT_NUMBER_OF_RVA_AND_SIZES = 108;

constexpr std::size_t OPT_DATA_DIRECTORY = 112;
constexpr std::size_t DATA_DIRECTORY_SIZE = 8;

constexpr std::size_t TLS64_ADDRESS_OF_CALLBACKS = 24;


namespace dir {
constexpr std::size_t EXPORT = 0;
constexpr std::size_t IMPORT = 1;
constexpr std::size_t EXCEPTION = 3;
constexpr std::size_t BASERELOC = 5;
constexpr std::size_t DEBUG = 6;
constexpr std::size_t TLS = 9;
constexpr std::size_t LOAD_CONFIG = 10;
constexpr std::size_t BOUND_IMPORT = 11;
constexpr std::size_t IAT = 12;
constexpr std::size_t DELAY_IMPORT = 13;
}   


namespace file_flags {
constexpr std::uint16_t RELOCS_STRIPPED = 0x0001;
constexpr std::uint16_t DLL = 0x2000;
}   


namespace dll_flags {
constexpr std::uint16_t HIGH_ENTROPY_VA = 0x0020;
constexpr std::uint16_t DYNAMIC_BASE = 0x0040;
constexpr std::uint16_t FORCE_INTEGRITY = 0x0080;
constexpr std::uint16_t NX_COMPAT = 0x0100;
constexpr std::uint16_t APPCONTAINER = 0x1000;
constexpr std::uint16_t GUARD_CF = 0x4000;
}   


namespace subsystem {
constexpr std::uint16_t WINDOWS_GUI = 2;
constexpr std::uint16_t WINDOWS_CUI = 3;
constexpr std::uint16_t XBOX = 14;
constexpr std::uint16_t XBOX_CODE_CATALOG = 17;
}   

inline std::optional<std::uint32_t> read_u32(std::span<const std::uint8_t> data, std::size_t at) {
    if (at > data.size() || data.size() - at < 4) {
        return std::nullopt;
    }
    std::uint32_t v;
    std::memcpy(&v, data.data() + at, 4);
    return v;
}

inline std::optional<std::uint64_t> read_u64(std::span<const std::uint8_t> data, std::size_t at) {
    if (at > data.size() || data.size() - at < 8) {
        return std::nullopt;
    }
    std::uint64_t v;
    std::memcpy(&v, data.data() + at, 8);
    return v;
}


inline std::optional<std::size_t> optional_header_offset(std::span<const std::uint8_t> data) {
    auto lfanew = read_u32(data, DOS_E_LFANEW);
    if (!lfanew) {
        return std::nullopt;
    }
    return std::size_t{*lfanew} + NT_SIGNATURE_SIZE + FILE_HEADER_SIZE;
}


inline std::optional<std::size_t> file_header_offset(std::span<const std::uint8_t> data) {
    auto lfanew = read_u32(data, DOS_E_LFANEW);
    if (!lfanew) {
        return std::nullopt;
    }
    return std::size_t{*lfanew} + NT_SIGNATURE_SIZE;
}


inline std::optional<std::size_t> data_directory_offset(std::span<const std::uint8_t> data, std::size_t index) {
    auto opt = optional_header_offset(data);
    if (!opt) {
        return std::nullopt;
    }
    auto count = read_u32(data, *opt + OPT_NUMBER_OF_RVA_AND_SIZES);
    if (!count || index >= *count) {
        return std::nullopt;
    }
    return *opt + OPT_DATA_DIRECTORY + index * DATA_DIRECTORY_SIZE;
}

}   
