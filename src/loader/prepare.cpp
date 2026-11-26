// SPDX-License-Identifier: GPL-3.0-or-later
// The prepared image (`victory-loader/src/prepare.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See prepare.hpp.
#include "prepare.hpp"

#include <blake3.h>

#include <cstring>
#include <utility>

#include "pe/layout.hpp"

namespace danzi::loader {

namespace {

namespace layout = pe::layout;


constexpr std::uint32_t IMPORT_DESCRIPTOR_SIZE = 20;

void write(std::vector<std::uint8_t>& bytes, std::size_t at, const void* v, std::size_t n) {
    if (at > bytes.size() || n > bytes.size() - at) {
        throw PrepareError("header field outside of file");
    }
    std::memcpy(bytes.data() + at, v, n);
}
void write_u16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) { write(b, at, &v, 2); }
void write_u32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) { write(b, at, &v, 4); }
void write_u64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) { write(b, at, &v, 8); }


std::vector<std::pair<std::size_t, std::uint32_t>> dir64_relocations(std::span<const std::uint8_t> bytes, const pe::ImageInfo& info) {
    constexpr std::uint16_t DIR64 = 10;
    std::vector<std::pair<std::size_t, std::uint32_t>> out;
    auto d = layout::data_directory_offset(bytes, layout::dir::BASERELOC);
    if (!d) {
        return out;
    }
    auto rva = layout::read_u32(bytes, *d);
    auto size = layout::read_u32(bytes, *d + 4);
    if (!rva || !size) {
        return out;
    }
    auto start = info.rva_to_file_offset(*rva);
    if (!start || *size == 0) {
        return out;
    }
    std::size_t at = *start, end = std::size_t{*start} + *size;
    while (at + 8 <= end) {
        auto page = layout::read_u32(bytes, at);
        auto block32 = layout::read_u32(bytes, at + 4);
        if (!page || !block32) {
            break;
        }
        std::size_t block = *block32;
        if (block < 8 || at + block > end) {
            break;
        }
        for (std::size_t entry = at + 8; entry < at + block; entry += 2) {
            if (entry + 2 > bytes.size()) {
                break;
            }
            std::uint16_t e;
            std::memcpy(&e, bytes.data() + entry, 2);
            if (e >> 12 == DIR64) {
                out.emplace_back(entry, *page + std::uint32_t{e & 0xFFFu});
            }
        }
        at += block;
    }
    return out;
}

}   

std::string PreparedImage::file_name() const {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string s = "victory-image-";
    for (std::size_t k = 0; k < 8; ++k) {
        s.push_back(HEX[original_hash[k] >> 4]);
        s.push_back(HEX[original_hash[k] & 15]);
    }
    return s + ".dll";
}

PreparedImage prepare(std::span<const std::uint8_t> original) {
    PreparedImage p;
    try {
        p.info = pe::ImageInfo::parse(original);
    } catch (const pe::PeError& e) {
        throw PrepareError(e);
    }
    const pe::ImageInfo& info = p.info;
    std::vector<std::uint8_t>& bytes = p.bytes;
    bytes.assign(original.begin(), original.end());

    auto fh = layout::file_header_offset(bytes);
    if (!fh) {
        throw PrepareError("no file header");
    }
    auto opt = layout::optional_header_offset(bytes);
    if (!opt) {
        throw PrepareError("no optional header");
    }
    write_u16(bytes, *fh + layout::FILE_HEADER_CHARACTERISTICS, static_cast<std::uint16_t>(info.characteristics | layout::file_flags::DLL));
    write_u32(bytes, *opt + layout::OPT_ADDRESS_OF_ENTRY_POINT, 0);
    write_u32(bytes, *opt + layout::OPT_CHECKSUM, 0);
    write_u16(bytes, *opt + layout::OPT_DLL_CHARACTERISTICS,
              static_cast<std::uint16_t>(info.dll_characteristics & ~layout::dll_flags::FORCE_INTEGRITY));


    if (auto at = layout::data_directory_offset(bytes, layout::dir::IMPORT)) {
        auto rva = layout::read_u32(bytes, *at);
        if (!rva) {
            throw PrepareError("no import RVA");
        }
        if (*rva != 0) {
            std::uint64_t offset = static_cast<std::uint64_t>(info.imports.size()) * IMPORT_DESCRIPTOR_SIZE;
            if (info.imports.size() > 0xFFFFFFFFu || offset > 0xFFFFFFFFu || *rva + offset > 0xFFFFFFFFu) {
                throw PrepareError("import terminator RVA overflow");
            }
            write_u32(bytes, *at, static_cast<std::uint32_t>(*rva + offset));
            write_u32(bytes, *at + 4, IMPORT_DESCRIPTOR_SIZE);
        }
    }
    if (auto at = layout::data_directory_offset(bytes, layout::dir::BOUND_IMPORT)) {
        std::uint8_t zeros[layout::DATA_DIRECTORY_SIZE] = {};
        write(bytes, *at, zeros, sizeof zeros);
    }

    if (info.tls && info.tls->callbacks_va != 0) {
        auto at = info.rva_to_file_offset(info.tls->directory_rva);
        if (!at) {
            throw PrepareError("TLS directory is not file-backed");
        }
        write_u64(bytes, std::size_t{*at} + layout::TLS64_ADDRESS_OF_CALLBACKS, 0);


        std::uint32_t field_rva = info.tls->directory_rva + static_cast<std::uint32_t>(layout::TLS64_ADDRESS_OF_CALLBACKS);
        for (auto [entry, target] : dir64_relocations(bytes, info)) {
            if (target == field_rva) {
                write_u16(bytes, entry, 0);   
            }
        }
    }

    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, original.data(), original.size());
    blake3_hasher_finalize(&h, p.original_hash.data(), p.original_hash.size());
    return p;
}

}   
