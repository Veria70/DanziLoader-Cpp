// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "win.hpp"

namespace danzi {


class PageSet {
public:
    bool contains(usize key) const {
        auto it = chunks_.find(key >> CHUNK_SHIFT);
        if (it == chunks_.end()) {
            return false;
        }
        usize i = key & CHUNK_MASK;
        return (it->second[i / 64] >> (i % 64)) & 1;
    }

    void insert(usize key) {
        std::uint64_t* c = chunk_for_insert(key >> CHUNK_SHIFT);
        usize i = key & CHUNK_MASK;
        std::uint64_t bit = std::uint64_t{1} << (i % 64);
        if (!(c[i / 64] & bit)) {
            c[i / 64] |= bit;
            ++size_;
        }
    }


    void insert_range(usize first, usize last) {
        for (usize k = first; k < last; ++k) {
            insert(k);
        }
    }

    void erase(usize key) {
        auto it = chunks_.find(key >> CHUNK_SHIFT);
        if (it == chunks_.end()) {
            return;
        }
        usize i = key & CHUNK_MASK;
        std::uint64_t bit = std::uint64_t{1} << (i % 64);
        if (it->second[i / 64] & bit) {
            it->second[i / 64] &= ~bit;
            --size_;
        }
    }

    bool empty() const { return size_ == 0; }


    std::vector<usize> range(usize first, usize last) const {
        std::vector<usize> out;
        out.reserve(count(first, last));
        each(first, last, [&](usize k) { out.push_back(k); });
        return out;
    }


    usize count(usize first, usize last) const {
        usize n = 0;
        words(first, last, [&](usize, std::uint64_t w) { n += static_cast<usize>(std::popcount(w)); });
        return n;
    }

    template <class F>
    void each(usize first, usize last, F&& f) const {
        words(first, last, [&](usize base, std::uint64_t w) {
            while (w) {
                f(base + static_cast<usize>(std::countr_zero(w)));
                w &= w - 1;
            }
        });
    }

private:
    static constexpr usize CHUNK_SHIFT = 18;   
    static constexpr usize CHUNK_MASK = (usize{1} << CHUNK_SHIFT) - 1;
    static constexpr usize WORDS = (usize{1} << CHUNK_SHIFT) / 64;


    template <class F>
    void words(usize first, usize last, F&& f) const {
        if (first >= last) {
            return;
        }
        for (auto it = chunks_.lower_bound(first >> CHUNK_SHIFT); it != chunks_.end(); ++it) {
            usize base = it->first << CHUNK_SHIFT;
            if (base >= last) {
                break;
            }
            usize lo = first > base ? first - base : 0;
            usize hi = std::min<usize>(last - base, usize{1} << CHUNK_SHIFT);
            for (usize w = lo / 64; w * 64 < hi; ++w) {
                std::uint64_t word = it->second[w];
                if (!word) {
                    continue;
                }
                usize wbase = w * 64;
                if (wbase < lo) {
                    word &= ~std::uint64_t{0} << (lo - wbase);
                }
                if (hi - wbase < 64) {
                    word &= (std::uint64_t{1} << (hi - wbase)) - 1;
                }
                if (word) {
                    f(base + wbase, word);
                }
            }
        }
    }


    std::uint64_t* chunk_for_insert(usize index) {
        if (last_ && last_index_ == index) {
            return last_;
        }
        auto& c = chunks_[index];
        if (!c) {
            c = std::make_unique<std::uint64_t[]>(WORDS);
        }
        last_index_ = index;
        last_ = c.get();
        return last_;
    }

    std::map<usize, std::unique_ptr<std::uint64_t[]>> chunks_;
    usize size_ = 0;
    usize last_index_ = 0;
    std::uint64_t* last_ = nullptr;
};


class PageMap {
public:


    void insert_or_assign(usize key, std::uint64_t value) {
        assert(value != 0);
        usize index = key >> CHUNK_SHIFT;
        if (!last_ || last_index_ != index) {
            auto& c = chunks_[index];
            if (!c) {
                c = std::make_unique<std::uint64_t[]>(usize{1} << CHUNK_SHIFT);
            }
            last_index_ = index;
            last_ = c.get();
        }
        last_[key & CHUNK_MASK] = value;
    }


    std::uint64_t get(usize key) const {
        auto it = chunks_.find(key >> CHUNK_SHIFT);
        return it == chunks_.end() ? 0 : it->second[key & CHUNK_MASK];
    }

    void erase(usize key) {
        auto it = chunks_.find(key >> CHUNK_SHIFT);
        if (it != chunks_.end()) {
            it->second[key & CHUNK_MASK] = 0;
        }
    }


    std::vector<std::pair<usize, std::uint64_t>> range(usize first, usize last) const {
        std::vector<std::pair<usize, std::uint64_t>> out;
        if (first >= last) {
            return out;
        }

        usize n = 0;
        slots(first, last, [&](usize, std::uint64_t) { ++n; });
        out.reserve(n);
        slots(first, last, [&](usize key, std::uint64_t value) { out.emplace_back(key, value); });
        return out;
    }

private:
    static constexpr usize CHUNK_SHIFT = 12;   
    static constexpr usize CHUNK_MASK = (usize{1} << CHUNK_SHIFT) - 1;


    template <class F>
    void slots(usize first, usize last, F&& f) const {
        for (auto it = chunks_.lower_bound(first >> CHUNK_SHIFT); it != chunks_.end(); ++it) {
            usize base = it->first << CHUNK_SHIFT;
            if (base >= last) {
                break;
            }
            usize lo = first > base ? first - base : 0;
            usize hi = std::min<usize>(last - base, usize{1} << CHUNK_SHIFT);
            const std::uint64_t* v = it->second.get();
            for (usize i = lo; i < hi; ++i) {
                if (v[i]) {
                    f(base + i, v[i]);
                }
            }
        }
    }

    std::map<usize, std::unique_ptr<std::uint64_t[]>> chunks_;
    usize last_index_ = 0;
    std::uint64_t* last_ = nullptr;
};

}   
