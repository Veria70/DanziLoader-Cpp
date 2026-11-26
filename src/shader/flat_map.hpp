// SPDX-License-Identifier: GPL-3.0-or-later
// A hash map with open addressing for small trivially copyable keys (the
// translator's tables keyed by values, nodes and their tuples): lookups and
// inserts without an allocation per entry. As hashbrown (the reference's
// maps), a dense array of control bytes (0: empty, else 0x80 and 7 bits of
// the hash) is probed first, so a lookup that misses - most of the BDD
// memo's - reads bytes, not slots; slots are compared only on a tag match.
// Iteration order is not the reference's; nothing that iterates one
// depends on its order.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace danzi::shader {

template <class K>
std::uint64_t hash_bytes(const K& k) {
    static_assert(std::is_trivially_copyable_v<K>);

    const auto* p = reinterpret_cast<const unsigned char*>(&k);
    std::uint64_t h = 0;
    std::size_t n = sizeof(K);
    while (n >= 4) {
        std::uint32_t w;
        std::memcpy(&w, p, 4);
        h = (std::rotl(h, 5) ^ w) * 0x517cc1b727220a95ull;
        p += 4;
        n -= 4;
    }
    while (n > 0) {
        h = (std::rotl(h, 5) ^ *p) * 0x517cc1b727220a95ull;
        ++p;
        --n;
    }
    return h;
}

template <class K, class V>
class FlatMap {
public:
    FlatMap() = default;
    FlatMap(const FlatMap& o) { *this = o; }
    FlatMap(FlatMap&& o) noexcept { *this = std::move(o); }
    FlatMap& operator=(const FlatMap& o) {
        if (this != &o) {
            cap_ = o.cap_;
            size_ = o.size_;
            shift_ = o.shift_;
            ctrl_.reset(cap_ ? new std::uint8_t[cap_] : nullptr);
            slots_.reset(cap_ ? new Slot[cap_] : nullptr);
            if (cap_) {
                std::memcpy(ctrl_.get(), o.ctrl_.get(), cap_);
                for (std::size_t i = 0; i < cap_; ++i) {
                    if (ctrl_[i]) {
                        slots_[i] = o.slots_[i];
                    }
                }
            }
        }
        return *this;
    }
    FlatMap& operator=(FlatMap&& o) noexcept {
        cap_ = std::exchange(o.cap_, 0);
        size_ = std::exchange(o.size_, 0);
        shift_ = std::exchange(o.shift_, 64);
        ctrl_ = std::move(o.ctrl_);
        slots_ = std::move(o.slots_);
        return *this;
    }

    V* find(const K& k) {
        if (cap_ == 0) {
            return nullptr;
        }
        std::uint64_t h = mixed(k);
        std::uint8_t tag = tag_of(h);
        std::size_t mask = cap_ - 1;
        for (std::size_t i = h >> shift_;; i = (i + 1) & mask) {
            std::uint8_t c = ctrl_[i];
            if (c == 0) {
                return nullptr;
            }
            if (c == tag && std::memcmp(&slots_[i].key, &k, sizeof(K)) == 0) {
                return &slots_[i].value;
            }
        }
    }
    const V* find(const K& k) const { return const_cast<FlatMap*>(this)->find(k); }
    bool contains(const K& k) const { return find(k) != nullptr; }


    void insert(const K& k, const V& v) { *try_emplace(k, v).first = v; }

    std::pair<V*, bool> try_emplace(const K& k, const V& v) {
        if ((size_ + 1) * 2 > cap_) {
            grow();
        }
        std::uint64_t h = mixed(k);
        std::uint8_t tag = tag_of(h);
        std::size_t mask = cap_ - 1;
        for (std::size_t i = h >> shift_;; i = (i + 1) & mask) {
            std::uint8_t c = ctrl_[i];
            if (c == 0) {
                ctrl_[i] = tag;
                std::memcpy(&slots_[i].key, &k, sizeof(K));
                slots_[i].value = v;
                ++size_;
                return {&slots_[i].value, true};
            }
            if (c == tag && std::memcmp(&slots_[i].key, &k, sizeof(K)) == 0) {
                return {&slots_[i].value, false};
            }
        }
    }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    void clear() {
        ctrl_.reset();
        slots_.reset();
        cap_ = 0;
        size_ = 0;
        shift_ = 64;
    }

    void clear_keep() {
        if (cap_) {
            std::memset(ctrl_.get(), 0, cap_);
        }
        size_ = 0;
    }
    template <class F>
    void for_each(F f) const {
        for (std::size_t i = 0; i < cap_; ++i) {
            if (ctrl_[i]) {
                f(slots_[i].key, slots_[i].value);
            }
        }
    }

private:
    struct Slot {
        K key;
        V value;
    };
    std::unique_ptr<std::uint8_t[]> ctrl_;
    std::unique_ptr<Slot[]> slots_;
    std::size_t cap_ = 0, size_ = 0;
    unsigned shift_ = 64;


    static std::uint64_t mixed(const K& k) { return hash_bytes(k) * 0x9E3779B97F4A7C15ull; }
    static std::uint8_t tag_of(std::uint64_t h) { return static_cast<std::uint8_t>(0x80 | ((h >> 7) & 0x7F)); }

    void grow() {
        std::size_t n = cap_ ? cap_ * 2 : 16;
        auto old_ctrl = std::move(ctrl_);
        auto old_slots = std::move(slots_);
        std::size_t old_cap = cap_;
        ctrl_.reset(new std::uint8_t[n]());
        slots_.reset(new Slot[n]);
        cap_ = n;
        shift_ = static_cast<unsigned>(64 - std::countr_zero(n));
        size_ = 0;
        std::size_t mask = n - 1;
        for (std::size_t j = 0; j < old_cap; ++j) {
            if (!old_ctrl[j]) {
                continue;
            }
            std::uint64_t h = mixed(old_slots[j].key);
            std::size_t i = h >> shift_;
            while (ctrl_[i]) {
                i = (i + 1) & mask;
            }
            ctrl_[i] = tag_of(h);
            slots_[i] = old_slots[j];
            ++size_;
        }
    }
};


class Triples {
public:
    static constexpr std::uint32_t EMPTY = UINT32_MAX;

    const std::uint32_t* find(std::uint32_t a, std::uint32_t b, std::uint32_t c) const {
        if (cap_ == 0) {
            return nullptr;
        }
        std::size_t mask = cap_ - 1;
        for (std::size_t i = index(a, b, c);; i = (i + 1) & mask) {
            const Entry& e = e_[i];
            if (e.a == a && e.b == b && e.c == c) {
                return &e.v;
            }
            if (e.a == EMPTY) {
                return nullptr;
            }
        }
    }

    void insert(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t v) {
        if ((size_ + 1) * 2 > cap_) {
            grow();
        }
        std::size_t mask = cap_ - 1;
        for (std::size_t i = index(a, b, c);; i = (i + 1) & mask) {
            Entry& e = e_[i];
            if (e.a == EMPTY) {
                e = {a, b, c, v};
                ++size_;
                return;
            }
            if (e.a == a && e.b == b && e.c == c) {
                e.v = v;
                return;
            }
        }
    }
    std::size_t size() const { return size_; }

    void clear_keep() {
        if (cap_) {
            std::memset(e_.get(), 0xFF, cap_ * sizeof(Entry));
        }
        size_ = 0;
    }

private:
    struct Entry {
        std::uint32_t a, b, c, v;
    };
    std::unique_ptr<Entry[]> e_;
    std::size_t cap_ = 0, size_ = 0;
    unsigned shift_ = 64;

    std::size_t index(std::uint32_t a, std::uint32_t b, std::uint32_t c) const {
        std::uint64_t h = static_cast<std::uint64_t>(a) * 0x517cc1b727220a95ull;
        h = (std::rotl(h, 5) ^ b) * 0x517cc1b727220a95ull;
        h = (std::rotl(h, 5) ^ c) * 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(h >> shift_);
    }
    void grow() {
        std::size_t n = cap_ ? cap_ * 2 : 64;
        auto old = std::move(e_);
        std::size_t old_cap = cap_;
        e_.reset(new Entry[n]);
        std::memset(e_.get(), 0xFF, n * sizeof(Entry));
        cap_ = n;
        shift_ = static_cast<unsigned>(64 - std::countr_zero(n));
        std::size_t mask = n - 1;
        for (std::size_t j = 0; j < old_cap; ++j) {
            const Entry& o = old[j];
            if (o.a == EMPTY) {
                continue;
            }
            std::size_t i = index(o.a, o.b, o.c);
            while (e_[i].a != EMPTY) {
                i = (i + 1) & mask;
            }
            e_[i] = o;
        }
    }
};


class TripleCache {
public:
    static constexpr std::uint32_t EMPTY = UINT32_MAX;
    static constexpr std::size_t MIN = std::size_t{1} << 12;

    const std::uint32_t* find(std::uint32_t a, std::uint32_t b, std::uint32_t c) const {
        if (cap_ == 0) {
            return nullptr;
        }
        const Entry& e = e_[index(a, b, c)];
        return e.a == a && e.b == b && e.c == c ? &e.v : nullptr;
    }
    void insert(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t v) {
        if (cap_ == 0) {
            resize(MIN);
        }
        e_[index(a, b, c)] = {a, b, c, v};
    }

    void reserve(std::size_t n, std::size_t max) {
        n = std::min(std::bit_ceil(std::max<std::size_t>(n, MIN)), max);
        if (n > cap_) {
            resize(n);
        }
    }
    std::size_t capacity() const { return cap_; }
    void clear_keep() {
        if (cap_) {
            std::memset(e_.get(), 0xFF, cap_ * sizeof(Entry));
        }
    }

private:
    struct Entry {
        std::uint32_t a, b, c, v;
    };
    std::unique_ptr<Entry[]> e_;
    std::size_t cap_ = 0;
    unsigned shift_ = 64;

    std::size_t index(std::uint32_t a, std::uint32_t b, std::uint32_t c) const {
        std::uint64_t h = static_cast<std::uint64_t>(a) * 0x517cc1b727220a95ull;
        h = (std::rotl(h, 5) ^ b) * 0x517cc1b727220a95ull;
        h = (std::rotl(h, 5) ^ c) * 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(h >> shift_);
    }
    void resize(std::size_t n) {
        auto old = std::move(e_);
        std::size_t old_cap = cap_;
        e_.reset(new Entry[n]);
        std::memset(e_.get(), 0xFF, n * sizeof(Entry));
        cap_ = n;
        shift_ = static_cast<unsigned>(64 - std::countr_zero(n));
        for (std::size_t j = 0; j < old_cap; ++j) {
            const Entry& o = old[j];
            if (o.a != EMPTY) {
                e_[index(o.a, o.b, o.c)] = o;
            }
        }
    }
};

}   
