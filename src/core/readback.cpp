// SPDX-License-Identifier: GPL-3.0-or-later
// Port of parts of crates/victory-gpu/src/readback.rs (see readback.hpp).
#include "readback.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "env.hpp"
#include "log.hpp"
#include "memcopy.hpp"

namespace danzi::readback {

namespace {

SrwLock TAGGED_LOCK;
memcopy::RangeSet TAGGED;

template <class T>
T read_at(usize at) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof v);
    return v;
}

}   

bool enabled() {
    static const bool on = [] {
        auto v = env::get(L"VICTORY_COPY_BACK_ONLY_READBACK");
        if (!v) {
            return false;
        }
        std::wstring s = *v;
        auto b = s.find_first_not_of(L" \t\r\n");
        auto e = s.find_last_not_of(L" \t\r\n");
        bool only = b != std::wstring::npos && s.substr(b, e - b + 1) == L"1";
        if (only) {
            log::warn("victory_gpu::readback", "VICTORY_COPY_BACK_ONLY_READBACK: shader writes are copied back only for the title's read-back buffers");
        }
        return only;
    }();
    return on;
}

std::optional<std::pair<usize, usize>> function_at(usize base, std::uint32_t rva) {
    usize nt = base + read_at<std::uint32_t>(base + 0x3c);
    if (read_at<std::uint32_t>(nt) != 0x4550) {
        return std::nullopt;
    }


    usize dir = nt + 0x18 + 0x70 + 3 * 8;
    usize at = read_at<std::uint32_t>(dir);
    usize size = read_at<std::uint32_t>(dir + 4);
    struct Entry {
        std::uint32_t begin, end, unwind;
    };
    auto* entries = reinterpret_cast<const Entry*>(base + at);
    usize n = size / 12;
    auto it = std::lower_bound(entries, entries + n, rva, [](const Entry& e, std::uint32_t r) { return e.begin < r; });
    if (it == entries + n || it->begin != rva) {
        return std::nullopt;
    }
    return std::make_pair(base + it->begin, base + it->end);
}

void tag(std::uint64_t start, std::uint64_t len) {
    std::lock_guard lock(TAGGED_LOCK);
    TAGGED.insert(start, start + len);
    usize n = TAGGED.size();
    if (n != 0 && (n & (n - 1)) == 0) {
        log::warn("victory_gpu::readback", "read-back buffers tagged", {{"n", log::debug(n)}, {"start", log::hex(start)}, {"len", log::debug(len)}});
    }
}

bool tagged_in(std::uint64_t start, std::uint64_t len) {
    std::lock_guard lock(TAGGED_LOCK);
    return TAGGED.overlaps(start, saturating_add(start, len));
}

bool copy_back(std::uint64_t start, std::uint64_t len) {
    if (!enabled()) {
        return true;
    }
    bool back = tagged_in(start, len);
    if (back) {
        memcopy::add(memcopy::M::BackGoes, 1);
    } else {
        memcopy::add(memcopy::M::BackKept, 1);
        memcopy::add(memcopy::M::BackKeptBytes, len);
    }
    return back;
}

void untag(std::uint64_t start) {
    if (enabled()) {
        std::lock_guard lock(TAGGED_LOCK);
        TAGGED.remove_at(start);
    }
}

usize tagged() {
    std::lock_guard lock(TAGGED_LOCK);
    return TAGGED.size();
}

}   
