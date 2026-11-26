// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the host copies' registry of crates/victory-gpu/src/shadow.rs
// (`register`, `unregister`, `find`, `find_any`, `lookup`, `loose`,
// `rebar_copy`; DanziLoader / victory-perf, see NOTICE.md): which copy an
// ask for title memory gets. The innermost title buffer that holds the ask
// gives its copy (made on first use: `CardHost::make_carded`); memory
// outside every buffer gets a copy of an aligned window (`loose`), placed
// in CPU-visible card memory with `VICTORY_REBAR`, else in the PC's memory
// with its card side when `window_card_for` allows.
//
// In place first, with `VICTORY_INPLACE` (milestone 3.10). Not yet: letting
// idle windows go (`let_go_idle`, which needs the frame clock: milestone 4).
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include "card.hpp"
#include "core/shadow.hpp"
#include "rebar.hpp"

namespace danzi::gpu {

class Registry {
public:
    explicit Registry(CardHost& host);
    ~Registry();


    std::uint64_t in_place_let_go(std::uint64_t at, std::uint64_t len);


    void register_buffer(usize resource, std::uint64_t start, std::uint64_t len);

    void unregister(usize resource, std::uint64_t start);


    std::shared_ptr<shadow::Shadow> find(std::uint64_t address, std::uint64_t len);

    std::shared_ptr<shadow::Shadow> find_any(std::uint64_t address, std::uint64_t len);


    void set_clock(std::uint64_t (*clock)()) { clock_ = clock; }


    template <class Idle, class References>
    std::vector<std::shared_ptr<shadow::Shadow>> let_go_idle(Idle idle, References references) {
        std::vector<std::shared_ptr<shadow::Shadow>> gone;
        std::lock_guard lock(loose_lock_);
        for (auto it = loose_.begin(); it != loose_.end();) {
            const auto& s = it->second;
            if (s.use_count() == 1 && idle(s->used.load(std::memory_order_relaxed)) && s->buffer() && references(s->buffer()) == 1) {
                gone.push_back(std::move(it->second));
                it = loose_.erase(it);
            } else {
                ++it;
            }
        }
        return gone;
    }

private:
    struct Entry {
        std::uint64_t end;
        std::shared_ptr<shadow::Shadow> shadow;
    };
    std::shared_ptr<shadow::Shadow> lookup(std::uint64_t address, std::uint64_t len, bool any);
    std::shared_ptr<shadow::Shadow> loose(std::uint64_t address, std::uint64_t end, bool unsized_ask, bool any);
    std::shared_ptr<shadow::Shadow> placed_copy(std::uint64_t start, std::uint64_t len, bool window);
    std::shared_ptr<shadow::Shadow> rebar_copy(rebar::Kind kind, std::uint64_t start, std::uint64_t len, std::uint64_t size, bool window);

    CardHost& host_;
    std::mutex buffers_lock_;
    std::map<std::pair<std::uint64_t, usize>, Entry> buffers_;
    std::uint64_t (*clock_)() = nullptr;
    std::mutex loose_lock_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::shared_ptr<shadow::Shadow>> loose_;
};

}   
