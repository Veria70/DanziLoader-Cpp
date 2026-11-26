// SPDX-License-Identifier: GPL-3.0-or-later
// Static TLS of an executable mapped as a DLL (`victory-loader/src/tls.rs`;
// DanziLoader / victory-perf, see NOTICE.md). An exe always gets TLS index
// 0, so title code reaches its thread-local block as `mov r64, gs:[0x58]`
// (the thread's TLS array) followed by `[r64]`, without reading
// `_tls_index`. Mapped as a DLL, the image gets another index. Every such
// load becomes a call to a thunk that yields `&array[index]` instead, and the
// image's `_tls_index` (which CRT code does read, and adds to the array
// pointer) is set to 0.
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "loader.hpp"
#include "core/log.hpp"

namespace danzi::loader::tls {

namespace {

constexpr std::uint32_t SCN_MEM_EXECUTE = 0x20000000;
constexpr std::size_t TLS64_ADDRESS_OF_INDEX = 16;

constexpr std::size_t LOAD_LEN = 9;
constexpr std::size_t THUNK_SIZE = 32;

std::string os_error(DWORD code) { return std::system_category().message(static_cast<int>(code)) + " (os error " + std::to_string(code) + ")"; }


std::size_t patch(std::uint8_t* code, std::size_t len, std::uintptr_t at, std::uintptr_t thunks) {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i + LOAD_LEN <= len) {
        const std::uint8_t* c = code + i;
        std::uint8_t rex = c[1];
        std::size_t reg = static_cast<std::size_t>((c[3] >> 3) & 7) | ((rex & 4) != 0 ? 8 : 0);


        if (c[0] == 0x65 && (rex == 0x48 || rex == 0x4C) && c[2] == 0x8B && (c[3] & 0xC7) == 0x04 && c[4] == 0x25 && c[5] == 0x58 && c[6] == 0 &&
            c[7] == 0 && c[8] == 0 && reg != 4) {
            auto rel = static_cast<std::int64_t>(thunks + reg * THUNK_SIZE) - static_cast<std::int64_t>(at + i + 5);
            auto rel32 = static_cast<std::int32_t>(rel);
            code[i] = 0xE8;
            std::memcpy(code + i + 1, &rel32, 4);
            static constexpr std::uint8_t NOP4[4] = {0x0F, 0x1F, 0x40, 0x00};
            std::memcpy(code + i + 5, NOP4, 4);
            ++count;
            i += LOAD_LEN;
        } else {
            ++i;
        }
    }
    return count;
}


std::uintptr_t thunks(std::uint8_t* block, std::uint32_t index) {
    std::uint32_t disp = index * 8;
    for (std::uint8_t reg = 0; reg < 16; ++reg) {
        std::uint8_t r = reg >> 3, low = reg & 7;
        std::vector<std::uint8_t> code{0x65, static_cast<std::uint8_t>(0x48 | r << 2), 0x8B, static_cast<std::uint8_t>(low << 3 | 4), 0x25, 0x58, 0, 0, 0};
        code.push_back(static_cast<std::uint8_t>(0x48 | r << 2 | r));
        code.push_back(0x8D);
        code.push_back(static_cast<std::uint8_t>(0x80 | low << 3 | low));
        if (low == 4) {
            code.push_back(0x24);   
        }
        for (int k = 0; k < 4; ++k) {
            code.push_back(static_cast<std::uint8_t>(disp >> (8 * k)));
        }
        code.push_back(0xC3);
        std::memcpy(block + reg * THUNK_SIZE, code.data(), code.size());
    }
    DWORD old = 0;
    if (!VirtualProtect(block, 4096, PAGE_EXECUTE_READ, &old)) {
        throw LoaderError("VirtualProtect(TLS thunks) failed: " + os_error(GetLastError()));
    }
    return reinterpret_cast<std::uintptr_t>(block);
}


std::uint8_t* near_page(std::uintptr_t base, std::size_t size) {
    constexpr std::uintptr_t GRANULE = 0x10000;

    if (size >= (std::size_t{1} << 30)) {
        throw LoaderError("image too large for TLS thunks within rel32 reach");
    }
    std::uintptr_t above = (base + size + GRANULE - 1) / GRANULE * GRANULE;
    for (std::uintptr_t k = 0; k < (std::uintptr_t{1} << 14); ++k) {
        std::optional<std::uintptr_t> hints[2];
        if (base >= (k + 1) * GRANULE) {
            hints[0] = base - (k + 1) * GRANULE;
        }
        hints[1] = above + k * GRANULE;
        for (const auto& hint : hints) {
            if (!hint) {
                continue;
            }
            void* p = VirtualAlloc(reinterpret_cast<void*>(*hint), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (p) {
                return static_cast<std::uint8_t*>(p);
            }
        }
    }
    throw LoaderError("no free page near the image for TLS thunks");
}

}   

std::size_t redirect(std::uintptr_t base, const pe::ImageInfo& info) {
    if (!info.tls) {
        return 0;
    }


    auto* index_at = *reinterpret_cast<std::uint32_t* const*>(base + info.tls->directory_rva + TLS64_ADDRESS_OF_INDEX);
    std::uint32_t index = *index_at;
    if (index == 0) {
        return 0;
    }
    std::uintptr_t th = thunks(near_page(base, info.size_of_image), index);
    std::size_t patched = 0;
    for (const auto& s : info.sections) {
        if ((s.characteristics & SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        std::uintptr_t start = base + s.rva;
        std::size_t len = s.virtual_size;
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(start), len, PAGE_EXECUTE_READWRITE, &old)) {
            throw LoaderError("VirtualProtect(code) failed: " + os_error(GetLastError()));
        }
        patched += patch(reinterpret_cast<std::uint8_t*>(start), len, start, th);
        VirtualProtect(reinterpret_cast<void*>(start), len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(start), len);
    }
    *index_at = 0;
    log::info("victory_loader::tls", "static TLS redirected", {{"index", log::debug(index)}, {"patched", log::debug(static_cast<std::uint64_t>(patched))}});
    return patched;
}

}   
