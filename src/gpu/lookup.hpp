// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the host copies' registry of crates/victory-gpu/src/shadow.rs


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
    std::mutex loose_lock_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::shared_ptr<shadow::Shadow>> loose_;
};

}   
