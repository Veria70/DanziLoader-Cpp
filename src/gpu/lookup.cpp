// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the host copies' registry of crates/victory-gpu/src/shadow.rs
// (see lookup.hpp).
#include "lookup.hpp"

#include <vector>

#include "core/inplace.hpp"
#include "core/readback.hpp"
#include "inplace_heap.hpp"
#include "core/windows.hpp"

namespace danzi::gpu {

Registry::Registry(CardHost& host) : host_(host) {
    rebar::probe(host_.device());
    danzi::inplace::set_freeing_hook([](void* self, std::uint64_t at, std::uint64_t len) { return static_cast<Registry*>(self)->in_place_let_go(at, len); },
                                     this);
}

Registry::~Registry() { danzi::inplace::set_freeing_hook(nullptr, nullptr); }

std::uint64_t Registry::in_place_let_go(std::uint64_t at, std::uint64_t len) {
    std::uint64_t end = saturating_add(at, len);
    std::vector<std::shared_ptr<shadow::Shadow>> gone;
    {
        std::lock_guard lock(loose_lock_);
        for (auto it = loose_.lower_bound({saturating_sub(at, windows::WINDOW), 0}); it != loose_.end() && it->first < std::pair{end, std::uint64_t{0}};) {
            if (it->second->placed() && it->first.first + it->first.second > at) {
                gone.push_back(std::move(it->second));
                it = loose_.erase(it);
            } else {
                ++it;
            }
        }
    }
    {

        std::lock_guard lock(buffers_lock_);
        auto it = buffers_.lower_bound({end, 0});
        for (int seen = 0; it != buffers_.begin() && seen < 256; ++seen) {
            --it;
            if (it->second.end > at && it->second.shadow && it->second.shadow->placed()) {
                gone.push_back(std::move(it->second.shadow));
                it->second.shadow = nullptr;
            }
        }
    }
    return gone.size();
}

std::shared_ptr<shadow::Shadow> Registry::placed_copy(std::uint64_t start, std::uint64_t len, bool window) {
    auto p = inplace::place_for(host_.device(), start, len, window);
    if (!p) {
        return nullptr;
    }
    auto s = std::make_shared<shadow::Shadow>(reinterpret_cast<std::uint8_t*>(start), start, len);
    s->make_placed(std::move(p->buffer), std::move(p->heap), p->bias, window);
    return s;
}

void Registry::register_buffer(usize resource, std::uint64_t start, std::uint64_t len) {
    std::lock_guard lock(buffers_lock_);
    buffers_[{start, resource}] = Entry{start + len, nullptr};
}

void Registry::unregister(usize resource, std::uint64_t start) {
    std::lock_guard lock(buffers_lock_);
    buffers_.erase({start, resource});
}

std::shared_ptr<shadow::Shadow> Registry::find(std::uint64_t address, std::uint64_t len) { return lookup(address, len, false); }

std::shared_ptr<shadow::Shadow> Registry::find_any(std::uint64_t address, std::uint64_t len) {


    if (!windows::fine()) {
        return find(address, len);
    }
    return lookup(address, len, true);
}

std::shared_ptr<shadow::Shadow> Registry::lookup(std::uint64_t address, std::uint64_t len, bool any) {
    std::uint64_t ask = std::max<std::uint64_t>(len, 1);
    if (address > ~std::uint64_t{0} - ask) {
        return nullptr;
    }
    std::uint64_t end = address + ask;
    std::unique_lock lock(buffers_lock_);


    auto it = buffers_.upper_bound({address, ~usize{0}});
    Entry* entry = nullptr;
    std::uint64_t start = 0;
    for (int seen = 0; it != buffers_.begin() && seen < 64; ++seen) {
        --it;
        if (it->second.end >= end) {
            entry = &it->second;
            start = it->first.first;
            break;
        }
    }
    if (!entry) {
        lock.unlock();
        return loose(address, end, len <= 1, any);
    }
    if (!entry->shadow) {
        std::uint64_t blen = entry->end - start;
        std::uint64_t size = (blen + 0xffff) & ~std::uint64_t{0xffff};


        bool big = shadow::cards() && size >= shadow::card_limits().first;
        if (!big) {
            entry->shadow = placed_copy(start, blen, false);
        }


        if (!entry->shadow) {
            entry->shadow = host_.make_carded(start, blen);
        }
    }
    return entry->shadow;
}

std::shared_ptr<shadow::Shadow> Registry::rebar_copy(rebar::Kind kind, std::uint64_t start, std::uint64_t len, std::uint64_t size, bool window) {
    bool cpu_read = readback::tagged_in(start, len);
    if (!rebar::reserve(host_.device(), kind, size, cpu_read)) {
        return nullptr;
    }
    auto b = host_.device().gpu_upload_buffer(size);
    if (!b) {
        rebar::released(size);
        return nullptr;
    }


    auto s = std::make_shared<shadow::Shadow>(b->second, start, len);
    s->hold_buffer(b->first);
    s->set_window(window);
    s->set_rebar(size);
    s->on_release([size] { rebar::released(size); });
    return s;
}

std::shared_ptr<shadow::Shadow> Registry::loose(std::uint64_t address, std::uint64_t end, bool unsized_ask, bool any) {
    using windows::Window;
    std::lock_guard lock(loose_lock_);
    std::uint64_t cell = windows::window_of(address, end, windows::WINDOW).first;
    Window named = windows::window_of(address, end, unsized_ask ? windows::WINDOW : windows::window());
    auto there = [&] {
        std::vector<Window> keys;
        for (auto it = loose_.lower_bound({cell, 0}); it != loose_.end() && it->first <= Window{address, ~std::uint64_t{0}}; ++it) {
            keys.push_back(it->first);
        }
        return keys;
    };
    Window w = named;
    if (unsized_ask && windows::tails()) {
        auto keys = there();
        w = windows::longest_tail(keys, address, end).value_or(windows::tail_of(address, end));
    } else if (any && windows::fine() && !unsized_ask) {
        auto keys = there();
        w = windows::holder(keys, address, end).value_or(named);
    }
    auto [start, len] = w;
    std::uint64_t now = clock_ ? clock_() : 0;
    if (auto found = loose_.find(w); found != loose_.end()) {
        if (found->second->used.load(std::memory_order_relaxed) != now) {
            found->second->used.store(now, std::memory_order_relaxed);
        }
        return found->second;
    }

    if (auto s = placed_copy(start, len, true)) {
        s->used.store(now, std::memory_order_relaxed);
        loose_[w] = s;
        return s;
    }
    if (auto s = rebar_copy(rebar::Kind::Window, start, len, len, true)) {
        s->used.store(now, std::memory_order_relaxed);
        loose_[w] = s;
        return s;
    }
    auto staging = host_.device().shared_buffer(len);
    if (!staging) {
        return nullptr;
    }
    auto s = std::make_shared<shadow::Shadow>(staging->second, start, len);
    s->set_window(true);
    s->used.store(now, std::memory_order_relaxed);


    if (auto on_card = host_.window_card_for(len)) {
        auto card = std::make_unique<shadow::Card>();
        card->staging = staging->first;
        card->size = len;
        s->make_carded(std::move(on_card), std::move(card));
    } else {
        s->hold_buffer(staging->first);
    }
    loose_[w] = s;
    return s;
}

}   
