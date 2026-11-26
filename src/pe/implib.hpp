// SPDX-License-Identifier: GPL-3.0-or-later
// COFF import libraries (`.lib`) (`victory-pe/src/implib.rs`; DanziLoader /
// victory-perf, see NOTICE.md): which DLL provides each API a console
// title may link against. Read as the reference reads them through the
// `object` crate (0.41: `read::archive`, `read::coff::import`): GNU, COFF
// and BSD archives, thin ones included. AIX big and z/OS archives (never a
// Windows library) are refused here, where the reference would read them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "image.hpp"

namespace danzi::pe {


struct LibImport {
    enum Kind : std::uint8_t { Code, Data, Const };
    std::string dll;
    ImportSymbol export_;
    Kind kind = Code;
    std::strong_ordering operator<=>(const LibImport& o) const {
        if (auto c = dll.compare(o.dll) <=> 0; c != 0) {
            return c;
        }
        if (auto c = export_ <=> o.export_; c != 0) {
            return c;
        }
        return kind <=> o.kind;
    }
    bool operator==(const LibImport& o) const { return (*this <=> o) == 0; }
};

struct ImportLibrary {
    std::vector<LibImport> imports;


    std::size_t other_members = 0;


    static ImportLibrary parse(std::span<const std::uint8_t> data);
};

}   
