// SPDX-License-Identifier: GPL-3.0-or-later
#include "fastmem.hpp"

#include <immintrin.h>

#include <bit>
#include <cstring>

namespace danzi::fastmem {

namespace {

int fault_filter(unsigned long code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case STATUS_GUARD_PAGE_VIOLATION:
            return EXCEPTION_EXECUTE_HANDLER;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
}


inline std::uint32_t differ_mask(const std::uint8_t* a, const std::uint8_t* b) noexcept {
    __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a));
    __m256i y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b));
    return ~static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, y)));
}

}   

bool try_copy(void* dst, const void* src, usize n) noexcept {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (fault_filter(GetExceptionCode())) {
        return false;
    }
}

bool equal(const std::uint8_t* a, const std::uint8_t* b, usize n) noexcept {
    usize i = 0;
    for (; i + 128 <= n; i += 128) {
        __m256i d0 = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i)),
                                      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i)));
        __m256i d1 = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i + 32)),
                                      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i + 32)));
        __m256i d2 = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i + 64)),
                                      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i + 64)));
        __m256i d3 = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i + 96)),
                                      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i + 96)));
        __m256i d = _mm256_or_si256(_mm256_or_si256(d0, d1), _mm256_or_si256(d2, d3));
        if (!_mm256_testz_si256(d, d)) {
            return false;
        }
    }
    for (; i + 32 <= n; i += 32) {
        if (differ_mask(a + i, b + i)) {
            return false;
        }
    }
    for (; i < n; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

std::optional<usize> first_difference(const std::uint8_t* a, const std::uint8_t* b, usize n) noexcept {
    usize i = 0;
    for (; i + 32 <= n; i += 32) {
        if (std::uint32_t m = differ_mask(a + i, b + i)) {
            return i + static_cast<usize>(std::countr_zero(m));
        }
    }
    for (; i < n; ++i) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<usize> last_difference(const std::uint8_t* a, const std::uint8_t* b, usize n) noexcept {
    usize i = n;
    for (; i >= 32; i -= 32) {
        if (std::uint32_t m = differ_mask(a + i - 32, b + i - 32)) {
            return i - 32 + 31 - static_cast<usize>(std::countl_zero(m));
        }
    }
    while (i > 0) {
        --i;
        if (a[i] != b[i]) {
            return i;
        }
    }
    return std::nullopt;
}

bool merge_gpu_into_title(std::uint8_t* title, std::uint8_t* host, const std::uint8_t* old, usize n) noexcept {
    __m256i differs = _mm256_setzero_si256();
    usize i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i t = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(title + i));
        __m256i h = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(host + i));
        __m256i o = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(old + i));

        __m256i kept = _mm256_cmpeq_epi8(h, o);
        __m256i merged = _mm256_blendv_epi8(h, t, kept);
        differs = _mm256_or_si256(differs, _mm256_xor_si256(merged, t));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(title + i), merged);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(host + i), merged);
    }
    std::uint8_t tail = 0;
    for (; i < n; ++i) {
        std::uint8_t merged = host[i] != old[i] ? host[i] : title[i];
        tail |= merged ^ title[i];
        title[i] = merged;
        host[i] = merged;
    }
    return tail != 0 || !_mm256_testz_si256(differs, differs);
}

bool merge_piece(const std::uint8_t* now, std::uint8_t* held, std::uint8_t* title, usize n) noexcept {
    __m256i differs = _mm256_setzero_si256();
    usize i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i w = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(now + i));
        __m256i h = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(held + i));
        __m256i t = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(title + i));
        __m256i same = _mm256_cmpeq_epi8(w, h);
        __m256i merged = _mm256_blendv_epi8(w, t, same);
        differs = _mm256_or_si256(differs, _mm256_xor_si256(merged, t));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(title + i), merged);

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(held + i), w);
    }
    std::uint8_t tail = 0;
    for (; i < n; ++i) {
        std::uint8_t wrote = now[i] != held[i] ? 0xff : 0;
        std::uint8_t merged = static_cast<std::uint8_t>((title[i] & ~wrote) | (now[i] & wrote));
        tail |= merged ^ title[i];
        title[i] = merged;
        held[i] = static_cast<std::uint8_t>((held[i] & ~wrote) | (now[i] & wrote));
    }
    return tail != 0 || !_mm256_testz_si256(differs, differs);
}

bool merge_card(std::uint8_t* title, const std::uint8_t* gpu, std::uint8_t* old, usize n) noexcept {
    __m256i differs = _mm256_setzero_si256();
    usize i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i g = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(gpu + i));
        __m256i o = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(old + i));
        __m256i t = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(title + i));

        __m256i kept = _mm256_cmpeq_epi8(g, o);
        __m256i merged = _mm256_blendv_epi8(g, t, kept);
        differs = _mm256_or_si256(differs, _mm256_xor_si256(merged, t));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(title + i), merged);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(old + i), merged);
    }
    std::uint8_t tail = 0;
    for (; i < n; ++i) {
        std::uint8_t merged = gpu[i] != old[i] ? gpu[i] : title[i];
        tail |= merged ^ title[i];
        title[i] = merged;
        old[i] = merged;
    }
    return tail != 0 || !_mm256_testz_si256(differs, differs);
}

std::vector<std::pair<usize, usize>> changed_runs(const std::uint8_t* now, const std::uint8_t* was, usize n) {
    std::vector<std::pair<usize, usize>> runs;
    bool in_run = false;
    usize first = 0;
    auto bits = [&](usize base, std::uint64_t m, usize width) {

        usize at = 0;
        while (at < width) {
            if (in_run) {
                std::uint64_t ends = ~m >> at;
                if (at + static_cast<usize>(std::countr_zero(ends)) >= width || ends == 0) {
                    return;   
                }
                at += static_cast<usize>(std::countr_zero(ends));
                runs.emplace_back(first, base + at - first);
                in_run = false;
            } else {
                std::uint64_t starts = m >> at;
                if (starts == 0) {
                    return;
                }
                at += static_cast<usize>(std::countr_zero(starts));
                if (at >= width) {
                    return;
                }
                first = base + at;
                in_run = true;
            }
        }
    };
    usize i = 0;
    for (; i + 32 <= n; i += 32) {
        std::uint32_t m = differ_mask(now + i, was + i);
        if (!in_run && m == 0) {
            continue;
        }
        if (in_run && m == 0xffffffffu) {
            continue;
        }
        bits(i, m, 32);
    }
    for (; i < n; ++i) {
        bool d = now[i] != was[i];
        if (d && !in_run) {
            first = i;
            in_run = true;
        } else if (!d && in_run) {
            runs.emplace_back(first, i - first);
            in_run = false;
        }
    }
    if (in_run) {
        runs.emplace_back(first, n - first);
    }
    return runs;
}

}   
