// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the title-memory reads and writes of crates/victory-gpu/src/host.rs.
#include "title_copy.hpp"

#include <algorithm>
#include <cstring>

#include "env.hpp"
#include "fastmem.hpp"

extern "C" NTSYSAPI LONG NTAPI NtReadVirtualMemory(HANDLE process, PVOID base, PVOID buffer, SIZE_T size, PSIZE_T read);
extern "C" NTSYSAPI LONG NTAPI NtWriteVirtualMemory(HANDLE process, PVOID base, PVOID buffer, SIZE_T size, PSIZE_T written);

namespace danzi::host {

namespace {

WriteObserver write_observer = nullptr;
void* write_observer_context = nullptr;


bool kernel_read(const std::uint8_t* src, std::uint8_t* dst, usize len) {
    SIZE_T done = 0;
    return NtReadVirtualMemory(GetCurrentProcess(), const_cast<std::uint8_t*>(src), dst, len, &done) >= 0;
}


bool direct_writes() {
    static const bool on = env::not_set_or_not(L"VICTORY_DIRECT_WRITES", L"0");
    return on;
}

bool kernel_write(usize at, const std::uint8_t* from, usize n) {
    SIZE_T done = 0;
    if (direct_writes()) {

        return NtWriteVirtualMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), const_cast<std::uint8_t*>(from), n, &done) >= 0;
    }
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), from, n, &done) != 0;
}

template <class F>
void for_pieces(usize start, usize len, usize step, F&& f) {
    usize end = start + len;
    usize at = start;
    while (at < end) {
        usize next = std::min((at / step + 1) * step, end);
        f(at, next - at);
        at = next;
    }
}

void read_pieces(const std::uint8_t* src, std::uint8_t* dst, usize len) {


    usize base = reinterpret_cast<usize>(src);
    for_pieces(base, len, usize{64} << 10, [&](usize at, usize n) {
        std::uint8_t* to = dst + (at - base);
        if (!kernel_read(reinterpret_cast<const std::uint8_t*>(at), to, n)) {
            for_pieces(at, n, usize{4} << 10, [&](usize at4, usize n4) {
                std::uint8_t* to4 = dst + (at4 - base);
                if (!kernel_read(reinterpret_cast<const std::uint8_t*>(at4), to4, n4)) {
                    std::memset(to4, 0, n4);
                }
            });
        }
    });
}


void read_sparse(const std::uint8_t* src, std::uint8_t* dst, usize len) {
    usize offset = 0;
    while (offset < len) {
        const std::uint8_t* at = src + offset;
        std::uint8_t* to = dst + offset;
        usize remaining = len - offset;
        MEMORY_BASIC_INFORMATION info{};
        SIZE_T size = VirtualQuery(at, &info, sizeof(info));
        usize n = std::min(saturating_sub(saturating_add(reinterpret_cast<usize>(info.BaseAddress), info.RegionSize), reinterpret_cast<usize>(at)), remaining);
        if (size != sizeof(info) || n == 0) {
            read_pieces(at, to, remaining);
            break;
        }
        if (info.State != MEM_COMMIT || info.Protect == PAGE_NOACCESS) {
            std::memset(to, 0, n);
        } else if (!kernel_read(at, to, n)) {

            read_pieces(at, to, n);
        }
        offset += n;
    }
}

}   

bool fast_access() {
    static const bool on = env::not_set_or_not(L"VICTORY_FAST_TITLE_ACCESS", L"0");
    return on;
}

void copy_into_host(std::uint8_t* dst, const std::uint8_t* src, usize len) {
    if (len == 0) {
        return;
    }
    if (fast_access() ? fastmem::try_copy(dst, src, len) : kernel_read(src, dst, len)) {
        return;
    }

    if (len >= (usize{64} << 10)) {
        read_sparse(src, dst, len);
    } else {
        read_pieces(src, dst, len);
    }
}

bool read_into(const std::uint8_t* src, std::uint8_t* dst, usize len) {
    return fast_access() ? fastmem::try_copy(dst, src, len) : kernel_read(src, dst, len);
}

void write_mapped(std::uint8_t* dst, const std::uint8_t* src, usize len) {
    if (write_observer) {
        write_observer(write_observer_context, reinterpret_cast<usize>(dst), len);
    }
    if (len == 0) {
        return;
    }


    if (fast_access() ? fastmem::try_copy(dst, src, len) : kernel_write(reinterpret_cast<usize>(dst), src, len)) {
        return;
    }
    usize base = reinterpret_cast<usize>(dst);
    for_pieces(base, len, usize{64} << 10, [&](usize at, usize n) {
        if (!kernel_write(at, src + (at - base), n)) {
            for_pieces(at, n, usize{4} << 10, [&](usize at4, usize n4) { kernel_write(at4, src + (at4 - base), n4); });
        }
    });
}

void set_write_observer(WriteObserver observer, void* context) {
    write_observer = observer;
    write_observer_context = context;
}

}   
