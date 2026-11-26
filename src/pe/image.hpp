// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only analysis of PE32+ (x86-64) images (`victory-pe/src/image.rs`
// and `lib.rs`; DanziLoader / victory-perf, see NOTICE.md). Xbox Series X|S
// titles are ordinary x86-64 PE32+ images, so the same parser serves titles,
// console system DLLs and host binaries alike. The reference reads images
// through the `object` crate (0.41); the parts it uses are ported here with
// their checks and error texts (`object_pe.cpp`), so the same images are
// accepted and the same ones refused, with the same message.
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace danzi::pe {


constexpr std::uint16_t MACHINE_AMD64 = 0x8664;


class PeError : public std::exception {
public:
    enum Kind { Unsupported, Malformed };
    PeError(Kind kind, std::string text) : kind_(kind), text_(std::move(text)), shown_(display_of(kind_, text_)) {}
    Kind kind() const { return kind_; }
    const std::string& text() const { return text_; }

    const std::string& display() const { return shown_; }
    const char* what() const noexcept override { return shown_.c_str(); }

private:
    static std::string display_of(Kind k, const std::string& t) { return (k == Unsupported ? "unsupported image: " : "malformed image: ") + t; }
    Kind kind_;
    std::string text_, shown_;
};

struct SectionInfo {
    std::string name;
    std::uint32_t rva = 0, virtual_size = 0, file_offset = 0, file_size = 0, characteristics = 0;
};


struct ImportSymbol {
    bool by_ordinal = false;
    std::uint16_t hint = 0;
    std::string name;
    std::uint16_t ordinal = 0;

    static ImportSymbol named(std::uint16_t hint, std::string name) { return {false, hint, std::move(name), 0}; }
    static ImportSymbol of_ordinal(std::uint16_t o) { return {true, 0, {}, o}; }

    std::string to_string() const { return by_ordinal ? "#" + std::to_string(ordinal) : name; }


    std::strong_ordering operator<=>(const ImportSymbol& o) const {
        if (by_ordinal != o.by_ordinal) {
            return by_ordinal ? std::strong_ordering::greater : std::strong_ordering::less;
        }
        if (by_ordinal) {
            return ordinal <=> o.ordinal;
        }
        if (auto c = hint <=> o.hint; c != 0) {
            return c;
        }
        return name.compare(o.name) <=> 0;
    }
    bool operator==(const ImportSymbol& o) const { return (*this <=> o) == 0; }
};

struct ImportEntry {
    ImportSymbol symbol;

    std::uint32_t iat_slot_rva = 0;
};


struct ImportModule {

    std::string dll;
    std::vector<ImportEntry> entries;
};

struct ExportInfo {
    std::optional<std::string> name;
    std::uint16_t ordinal = 0;

    std::optional<std::uint32_t> rva;

    std::optional<std::string> forward;
};

struct TlsInfo {

    std::uint32_t directory_rva = 0;

    std::uint64_t callbacks_va = 0;

    std::vector<std::uint32_t> callback_rvas;
};


struct CodeViewInfo {
    std::string pdb_path;
    std::array<std::uint8_t, 16> guid{};
    std::uint32_t age = 0;

    std::string symbol_key() const;
};


struct ImageInfo {
    std::uint16_t machine = 0;
    std::uint16_t characteristics = 0;
    std::uint32_t timestamp = 0;
    std::uint16_t subsystem = 0;
    std::uint16_t dll_characteristics = 0;
    std::uint64_t image_base = 0;
    std::uint32_t size_of_image = 0;
    std::uint32_t entry_rva = 0;
    std::uint64_t stack_reserve = 0, stack_commit = 0;
    std::vector<SectionInfo> sections;
    std::vector<ImportModule> imports, delay_imports;
    std::vector<ExportInfo> exports;
    std::optional<TlsInfo> tls;
    bool has_relocations = false, has_exception_table = false, has_load_config = false;
    std::optional<CodeViewInfo> codeview;


    static ImageInfo parse(std::span<const std::uint8_t> data);

    bool is_dll() const;

    std::optional<std::uint32_t> rva_to_file_offset(std::uint32_t rva) const;
    std::size_t import_count() const;
    const char* subsystem_name() const;
};


std::string utf8_lossy(std::span<const std::uint8_t> bytes);

}   
