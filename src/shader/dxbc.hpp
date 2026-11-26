// SPDX-License-Identifier: GPL-3.0-or-later
// The DXBC container (`dxbc.rs`; DanziLoader / victory-perf, see
// NOTICE.md): a header, then a table of FourCC-tagged parts. Throws the
// reference's errors.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace danzi::shader {

struct ContainerPart {
    std::uint32_t fourcc;

    std::size_t offset;
    std::span<const std::uint8_t> data;
};

struct Container {
    std::uint8_t digest[16];
    std::uint16_t version[2];
    std::vector<ContainerPart> parts;

    static Container parse(std::span<const std::uint8_t> data);
    const ContainerPart* part(std::uint32_t fourcc) const {
        for (const auto& p : parts) {
            if (p.fourcc == fourcc) {
                return &p;
            }
        }
        return nullptr;
    }
};


std::string fourcc_text(std::uint32_t fourcc);

}   
