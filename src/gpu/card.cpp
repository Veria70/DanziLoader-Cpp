// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the host's card path, crates/victory-gpu/src/host.rs (see

#include "card.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "core/env.hpp"
#include "core/readback.hpp"
#include "core/windows.hpp"
#include "core/wide.hpp"
#include "rebar.hpp"

#pragma comment(lib, "d3dcompiler.lib")

namespace danzi::gpu {

namespace {


const char CARD_COMPARE[] =
#include "card_compare.inc"
    ;


std::string card_compare_source() {
    std::string s = CARD_COMPARE;
    if (!env::not_set_or_not(L"VICTORY_CARD_COMPARE_WIDE_LOADS", L"0")) {
        return s;
    }
    const std::string hash_loop =
        "    [loop]\n"
        "    for (uint off = lane * 16u; off < span_bytes; off += 1024u) {\n"
        "        h = (h ^ now.Load4(lo + off)) * uint4(0x01000193u, 0x9E3779B1u, 0x85EBCA77u, 0xC2B2AE3Du);\n"
        "        h = h.yzwx ^ (h >> 15);\n"
        "    }\n";
    std::string step;
    std::string loads;
    for (int k = 0; k < 8; ++k) {
        loads += "        uint4 c" + std::to_string(k) + " = now.Load4(lo + off + " + std::to_string(k * 1024) + "u);\n";
        step += "        h = (h ^ c" + std::to_string(k) + ") * uint4(0x01000193u, 0x9E3779B1u, 0x85EBCA77u, 0xC2B2AE3Du);\n"
                "        h = h.yzwx ^ (h >> 15);\n";
    }
    const std::string hash_wide = "    uint off = lane * 16u;\n"
                                  "    [loop]\n"
                                  "    for (; off + 7168u < span_bytes; off += 8192u) {\n" +
                                  loads + step +
                                  "    }\n"
                                  "    [loop]\n"
                                  "    for (; off < span_bytes; off += 1024u) {\n"
                                  "        h = (h ^ now.Load4(lo + off)) * uint4(0x01000193u, 0x9E3779B1u, 0x85EBCA77u, 0xC2B2AE3Du);\n"
                                  "        h = h.yzwx ^ (h >> 15);\n"
                                  "    }\n";
    const std::string copy_loop =
        "        [loop]\n"
        "        for (uint off = lane * 16u; off < span_bytes; off += 1024u) {\n"
        "            inline_buf.Store4(dst_base + off, now.Load4(lo + off));\n"
        "        }\n";
    std::string copy_loads, copy_stores;
    for (int k = 0; k < 8; ++k) {
        copy_loads += "            uint4 d" + std::to_string(k) + " = now.Load4(lo + o + " + std::to_string(k * 1024) + "u);\n";
        copy_stores += "            inline_buf.Store4(dst_base + o + " + std::to_string(k * 1024) + "u, d" + std::to_string(k) + ");\n";
    }
    const std::string copy_wide = "        uint o = lane * 16u;\n"
                                  "        [loop]\n"
                                  "        for (; o + 7168u < span_bytes; o += 8192u) {\n" +
                                  copy_loads + copy_stores +
                                  "        }\n"
                                  "        [loop]\n"
                                  "        for (; o < span_bytes; o += 1024u) {\n"
                                  "            inline_buf.Store4(dst_base + o, now.Load4(lo + o));\n"
                                  "        }\n";
    auto a = s.find(hash_loop);
    auto b = s.find(copy_loop);
    if (a == std::string::npos || b == std::string::npos || b < a) {
        return CARD_COMPARE;
    }
    s.replace(b, copy_loop.size(), copy_wide);
    s.replace(a, hash_loop.size(), hash_wide);
    return s;
}


constexpr std::uint64_t RESULTS = 1 << 20;


const char RING_MARKS[] = R"hlsl(
#define RS "RootConstants(num32BitConstants=3, b0), UAV(u0), UAV(u1)"
cbuffer C : register(b0) { uint phase; uint reset; uint limit; };
RWByteAddressBuffer ring : register(u0);
RWByteAddressBuffer marks : register(u1);
[RootSignature(RS)]
[numthreads(128, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (phase == 0u) {
        if (id.x == 0u) {
            if (reset != 0u) {
                ring.Store(0u, 0u);
            }
            marks.Store(1024u, reset != 0u ? 0u : ring.Load(0u));
        }
        return;
    }
    uint first = marks.Load(1024u);
    uint end = ring.Load(0u);
    uint stop = ((int)(end - limit) > 0) ? limit : end;
    uint count = ((int)(stop - first) > 0) ? min(stop - first, 2048u) : 0u;
    uint s0 = first & 2047u;
    uint e = s0 + count;
    uint lo = id.x * 16u;
    uint hi = lo + 16u;
    uint on = (count > 0u && ((lo < e && hi > s0) || (e > 2048u && lo < e - 2048u))) ? 1u : 0u;
    marks.Store2(id.x * 8u, uint2(on, 0u));
    if (id.x < 12u) {
        
        uint glo = id.x == 0u ? 0u : (1u << (id.x - 1u));
        uint ghi = 1u << id.x;
        uint gon = (count > 0u && ((glo < e && ghi > s0) || (e > 2048u && glo < e - 2048u))) ? 1u : 0u;
        marks.Store2(1088u + id.x * 8u, uint2(gon, 0u));
    }
}
)hlsl";


constexpr std::uint32_t RING_BLOCKS = 128;
constexpr std::uint64_t RING_BLOCK_BYTES = 16 * CARD_INLINE_STRIDE;


constexpr std::uint64_t WIDE_MERGE_LEAST = std::uint64_t{4} << 20;


std::mutex QUEUE_ORDER_LOCK;
std::uint64_t QUEUE_ORDER = 0;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("host GPU failure: " + what); }

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char b[48];
        std::snprintf(b, sizeof b, " (0x%08lx)", static_cast<unsigned long>(hr));
        fail(std::string(what) + b);
    }
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

std::uint64_t micros_since(std::chrono::steady_clock::time_point t) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count());
}

std::uint64_t nanos(std::chrono::steady_clock::duration d) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}


std::uint32_t inline_pieces_within(const std::vector<CardRange>& ranges, std::uint64_t budget) {
    thread_local std::vector<std::pair<std::uint64_t, std::uint32_t>> sizes;   
    sizes.clear();
    for (const auto& r : ranges) {
        if (r.compared && r.span.pieces > 0) {
            std::uint64_t most = std::min<std::uint64_t>(65536, std::uint64_t{r.span.to} - r.span.from) + 32;
            sizes.emplace_back(most, r.span.pieces);
        }
    }
    std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::uint64_t used = 0;
    std::uint32_t fit = 0;
    for (const auto& [most, pieces] : sizes) {
        std::uint64_t room = (budget - used) / most;
        if (room < pieces) {
            return fit + static_cast<std::uint32_t>(room);
        }
        used += most * pieces;
        fit += pieces;
    }
    return fit;
}

}   

bool card_inline_fetch() {
    static const bool on = env::not_set_or_not(L"VICTORY_CARD_INLINE_FETCH", L"0");
    return on;
}

bool rehash_runs() {
    static const bool on = env::not_set_or_not(L"VICTORY_CARD_REHASH_RUNS", L"0");
    return on;
}

bool wide_merge() {
    static const bool on = env::is(L"VICTORY_WIDE_MERGE", L"1");
    return on;
}

std::uint64_t card_inline_budget() {
    static const std::uint64_t budget = [] {


        constexpr std::uint64_t standard = std::uint64_t{1088} << 10;
        auto v = env::get(L"VICTORY_CARD_INLINE_KB");
        if (!v) {
            return standard;
        }
        try {
            return static_cast<std::uint64_t>(std::stoull(*v)) << 10;
        } catch (const std::exception&) {
            return standard;
        }
    }();
    return budget;
}

bool card_vram_ring() {
    static const bool on = env::not_set_or_not(L"VICTORY_CARD_RING", L"pc");
    return on;
}

std::uint64_t card_budget_percent() {
    static const std::uint64_t percent = [] {
        auto v = env::get(L"VICTORY_CARD_BUDGET_PERCENT");
        if (!v) {
            return std::uint64_t{20};
        }
        try {
            return std::clamp<std::uint64_t>(std::stoull(*v), 5, 95);
        } catch (const std::exception&) {
            return std::uint64_t{20};
        }
    }();
    return percent;
}

bool card_results_in_vram() {
    static const bool on = env::not_set_or_not(L"VICTORY_CARD_RESULTS_VRAM", L"0");
    return on;
}

bool card_relay_copy_queue() {
    static const bool on = env::is(L"VICTORY_CARD_RELAY_COPY_QUEUE", L"1");
    return on;
}

bool card_copy_queue() {
    static const bool on = env::not_set_or_not(L"VICTORY_CARD_COPY_QUEUE", L"0");
    return on;
}

bool card_pipeline() {
    static const bool on = env::not_set_or_not(L"VICTORY_CARD_PIPELINE", L"0");
    return on;
}

std::uint8_t late_stage() {
    static const std::uint8_t stage = []() -> std::uint8_t {
        if (env::get(L"VICTORY_GPU_TIMING")) {
            return 0;
        }
        auto v = env::get(L"VICTORY_LATE_WAIT");
        if (v) {
            if (*v == L"1") return 1;
            if (*v == L"2") return 2;
            if (*v == L"3") return 3;
            return 0;
        }

        return shadow::relay() ? 3 : 0;
    }();
    return stage;
}

bool relay() {
    static const bool on = late_stage() > 0 && shadow::relay();
    return on;
}

std::uint64_t queue_order() {
    std::lock_guard lock(QUEUE_ORDER_LOCK);
    return QUEUE_ORDER;
}


struct CardHost::Work {
    std::vector<Prelude> sync;    
    std::vector<Prelude> check;   
    Prelude fetch;                
    std::vector<Prelude> fetches;   


    bool fetch_on_copy_queue = false;
    ComPtr<ID3D12Fence> copy_fence;
    HANDLE copy_event = nullptr;
    std::uint64_t copy_value = 0;
    ~Work() {
        if (copy_event) {
            CloseHandle(copy_event);
        }
    }
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;


    ComPtr<ID3D12Resource> differs;


    ComPtr<ID3D12Resource> differs_vram;
    ID3D12Resource* differs_target() const { return differs_vram ? differs_vram.Get() : differs.Get(); }
    const std::uint32_t* differs_data = nullptr;
    std::uint64_t differs_bytes = 0;


    ComPtr<ID3D12Resource> inline_buf;
    std::uint8_t* inline_data = nullptr;
    std::uint64_t inline_bytes = 0;
    std::uint32_t inline_slots = 0;
    std::uint32_t inline_tail = 0;

    ComPtr<ID3D12Resource> fetched;
    std::uint8_t* fetched_data = nullptr;
    std::uint64_t fetched_bytes = 0;


    ComPtr<ID3D12Resource> ring;
    ComPtr<ID3D12Resource> marks;
    ComPtr<ID3D12RootSignature> marks_root;
    ComPtr<ID3D12PipelineState> marks_pso;
    bool reset_pending = false;


    std::vector<Prelude> downloads;
    std::vector<Prelude> downloads_from_zero;

    ID3D12Resource* ring_target() const { return ring ? ring.Get() : inline_buf.Get(); }
    std::uint32_t taken = 0;   
    std::uint32_t out = 0;     
};

CardHost::CardHost(Device& device) : device_(device) {}


struct CardHost::RelayFetch {
    Prelude list;
    std::vector<Prelude> parts;   
    bool on_copy_queue = false;   
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    std::uint64_t sent = 0;
    ComPtr<ID3D12Resource> buffer;
    std::uint8_t* data = nullptr;
    std::uint64_t bytes = 0;
    ~RelayFetch() {
        if (event) {
            CloseHandle(event);
        }
    }
};

CardHost::~CardHost() = default;

std::uint64_t CardHost::own_vram() {
    std::lock_guard lock(work_lock_);
    if (!work_) {
        return 0;
    }
    std::uint64_t bytes = 0;
    if (work_->ring) {
        bytes += work_->inline_bytes;
    }
    if (work_->differs_vram) {
        bytes += RESULTS;
    }
    if (work_->marks) {
        bytes += 2048;
    }
    return bytes;
}

std::unique_ptr<CardHost::RelayFetch> CardHost::make_relay_fetch() {
    bool copy = card_relay_copy_queue() && device_.copy_queue();
    auto list = device_.make_prelude(copy ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT);
    if (!list) {
        return nullptr;
    }
    auto f = std::make_unique<RelayFetch>();
    f->list = std::move(*list);
    f->on_copy_queue = copy;
    if (FAILED(device_.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f->fence)))) {
        return nullptr;
    }
    f->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!f->event) {
        return nullptr;
    }
    return f;
}

std::optional<Prelude> CardHost::prelude() { return device_.make_prelude(); }

bool CardHost::card_ready() {
    std::call_once(ready_once_, [&] { ready_ = make_work(); });
    return ready_;
}

bool CardHost::make_work() {
    ComPtr<ID3DBlob> code;
    const std::string source = card_compare_source();
    if (FAILED(D3DCompile(source.data(), source.size(), "card compare", nullptr, nullptr, "main", "cs_5_1", 0, 0, &code, nullptr)) || !code) {
        return false;
    }
    auto w = std::make_unique<Work>();
    auto results = device_.shared_buffer(RESULTS);
    if (!results) {
        return false;
    }
    w->differs = results->first;
    if (card_results_in_vram()) {
        w->differs_vram = device_.uav_buffer(RESULTS);
    }
    w->differs_data = reinterpret_cast<const std::uint32_t*>(results->second);
    w->differs_bytes = RESULTS;
    auto small_ring = [&]() -> bool {
        auto b = device_.shared_buffer(64);
        if (!b) {
            return false;
        }
        w->inline_buf = b->first;
        w->inline_data = b->second;
        w->inline_bytes = 64;
        w->inline_slots = 0;
        return true;
    };
    if (card_inline_fetch()) {
        std::uint64_t bytes = CARD_INLINE_HEADER + std::uint64_t{CARD_INLINE_SLOTS} * CARD_INLINE_STRIDE;
        if (auto b = device_.shared_buffer(bytes)) {
            w->inline_buf = b->first;
            w->inline_data = b->second;
            w->inline_bytes = bytes;
            w->inline_slots = CARD_INLINE_SLOTS;
        } else if (!small_ring()) {
            return false;
        }
    } else if (!small_ring()) {
        return false;
    }
    *reinterpret_cast<volatile std::uint32_t*>(w->inline_data) = 0;


    std::uint64_t share = 0;
    if (auto vm = device_.video_memory()) {
        share = vm->budget / 100 * card_budget_percent();
    }
    if (w->inline_slots > 0 && card_vram_ring() && share >= 4 * w->inline_bytes) {


        ComPtr<ID3DBlob> marks_code;
        auto ring = device_.uav_buffer(w->inline_bytes);
        auto marks = device_.uav_buffer(2048);
        if (ring && marks &&
            SUCCEEDED(D3DCompile(RING_MARKS, std::strlen(RING_MARKS), "ring marks", nullptr, nullptr, "main", "cs_5_1", 0, 0, &marks_code, nullptr)) &&
            SUCCEEDED(device_.device()->CreateRootSignature(0, marks_code->GetBufferPointer(), marks_code->GetBufferSize(), IID_PPV_ARGS(&w->marks_root)))) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC md{};
            md.pRootSignature = w->marks_root.Get();
            md.CS = {marks_code->GetBufferPointer(), marks_code->GetBufferSize()};
            if (SUCCEEDED(device_.device()->CreateComputePipelineState(&md, IID_PPV_ARGS(&w->marks_pso)))) {
                w->ring = ring;
                w->marks = marks;
                for (int i = 0; i < 2; ++i) {
                    w->downloads.push_back(std::move(*record_download(*w, false)));
                    w->downloads_from_zero.push_back(std::move(*record_download(*w, true)));
                }
            }
        }
    }
    if (FAILED(device_.device()->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&w->root)))) {
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = w->root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(device_.device()->CreateComputePipelineState(&pd, IID_PPV_ARGS(&w->pso)))) {
        return false;
    }
    auto a = prelude(), b = prelude(), c = prelude();
    if (!a || !b || !c) {
        return false;
    }
    w->sync.push_back(std::move(*a));
    w->check.push_back(std::move(*b));
    w->fetch = std::move(*c);
    if (card_pipeline()) {


        if (auto f = device_.shared_buffer(std::uint64_t{16} << 20)) {
            w->fetched = f->first;
            w->fetched_data = f->second;
            w->fetched_bytes = std::uint64_t{16} << 20;
        }
        if (card_copy_queue() && device_.copy_queue() &&
            SUCCEEDED(device_.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&w->copy_fence)))) {
            w->copy_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            w->fetch_on_copy_queue = w->copy_event != nullptr;
        }


        std::vector<wide::Job> warm(wide::threads() + 1, [] { shadow::warm_thread(); });
        wide::run(warm);
    }
    std::lock_guard lock(work_lock_);
    work_ = std::move(w);
    return true;
}

ComPtr<ID3D12Resource> CardHost::card_for(std::uint64_t size, bool small_ok) {
    if (!shadow::cards()) {
        return nullptr;
    }
    auto [least, most] = shadow::card_limits();
    if ((size < least && !small_ok) || size >= (std::uint64_t{1} << 30)) {
        return nullptr;
    }
    if (!card_ready()) {
        return nullptr;
    }

    if (auto vm = device_.video_memory(); vm && vm->usage + size > vm->budget / 100 * shadow::card_vram_percent()) {
        return nullptr;
    }
    if (shadow::CARD_BYTES[0].fetch_add(size, std::memory_order_relaxed) + size > most) {
        shadow::CARD_BYTES[0].fetch_sub(size, std::memory_order_relaxed);
        return nullptr;
    }


    if (auto vm = device_.video_memory()) {
        std::uint64_t mine = shadow::CARD_BYTES[0].load(std::memory_order_relaxed) + shadow::CARD_BYTES[1].load(std::memory_order_relaxed) +
                             shadow::WINDOW_CARD_BYTES.load(std::memory_order_relaxed) + shadow::card_hash_bytes(size) + own_vram();
        if (mine > vm->budget / 100 * card_budget_percent()) {
            shadow::CARD_BYTES[0].fetch_sub(size, std::memory_order_relaxed);
            return nullptr;
        }
    }
    auto buffer = device_.uav_buffer(size);
    if (!buffer) {
        shadow::CARD_BYTES[0].fetch_sub(size, std::memory_order_relaxed);
    }
    return buffer;
}

ComPtr<ID3D12Resource> CardHost::window_card_for(std::uint64_t size) {
    std::uint64_t most = windows::window_card_most();
    if (most == 0 || !shadow::cards() || size < (std::uint64_t{64} << 10) || size >= (std::uint64_t{1} << 30)) {
        return nullptr;
    }
    if (!card_ready()) {
        return nullptr;
    }
    auto vm = device_.video_memory();
    if (vm && vm->usage + size > vm->budget / 100 * 72) {
        return nullptr;
    }
    if (shadow::WINDOW_CARD_BYTES.fetch_add(size, std::memory_order_relaxed) + size > most) {
        shadow::WINDOW_CARD_BYTES.fetch_sub(size, std::memory_order_relaxed);
        return nullptr;
    }

    if (vm) {
        std::uint64_t mine = shadow::CARD_BYTES[0].load(std::memory_order_relaxed) + shadow::CARD_BYTES[1].load(std::memory_order_relaxed) +
                             shadow::WINDOW_CARD_BYTES.load(std::memory_order_relaxed) + own_vram();
        if (mine > vm->budget / 100 * card_budget_percent()) {
            shadow::WINDOW_CARD_BYTES.fetch_sub(size, std::memory_order_relaxed);
            return nullptr;
        }
    }
    auto buffer = device_.uav_buffer(size);
    if (!buffer) {
        shadow::WINDOW_CARD_BYTES.fetch_sub(size, std::memory_order_relaxed);
    }
    return buffer;
}

std::unique_ptr<shadow::Shadow> CardHost::make_carded(std::uint64_t start, std::uint64_t len) {
    std::uint64_t size = (len + 0xffff) & ~std::uint64_t{0xffff};


    rebar::probe(device_);
    bool big = shadow::cards() && size >= shadow::card_limits().first;
    if (!big && rebar::reserve(device_, rebar::Kind::Buffer, size, readback::tagged_in(start, len))) {
        if (auto staging = device_.shared_buffer(size)) {
            if (auto on_card = card_for(size, true)) {
                auto card = std::make_unique<shadow::Card>();
                card->staging = staging->first;
                card->size = size;
                auto s = std::make_unique<shadow::Shadow>(staging->second, start, len);
                s->make_carded(std::move(on_card), std::move(card));
                s->on_release([size] { rebar::released(size); });
                return s;
            }
        }
        rebar::released(size);
    }
    auto staging = device_.shared_buffer(size);
    if (!staging) {
        return nullptr;
    }
    auto on_card = card_for(size);
    if (!on_card) {

        auto s = std::make_unique<shadow::Shadow>(staging->second, start, len);
        s->hold_buffer(staging->first);
        return s;
    }
    auto card = std::make_unique<shadow::Card>();
    card->staging = staging->first;
    card->size = size;
    auto s = std::make_unique<shadow::Shadow>(staging->second, start, len);
    s->make_carded(std::move(on_card), std::move(card));
    return s;
}


std::optional<Prelude> CardHost::record_download(Work& work, bool from_zero) {
    auto p = prelude();
    if (!p) {
        fail("no host list to bring the compare's ring down");
    }
    check(p->allocator->Reset(), "download allocator reset");
    check(p->list->Reset(p->allocator.Get(), nullptr), "download list reset");
    if (from_zero) {
        for (std::uint32_t k = 0; k < 12; ++k) {
            std::uint64_t lo = k == 0 ? 0 : std::uint64_t{1} << (k - 1), hi = std::uint64_t{1} << k;
            std::uint64_t at = CARD_INLINE_HEADER + lo * CARD_INLINE_STRIDE;
            p->list->SetPredication(work.marks.Get(), 1088 + std::uint64_t{k} * 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
            p->list->CopyBufferRegion(work.inline_buf.Get(), at, work.ring.Get(), at, (hi - lo) * CARD_INLINE_STRIDE);
        }
    } else {
        for (std::uint32_t b = 0; b < RING_BLOCKS; ++b) {
            std::uint64_t at = CARD_INLINE_HEADER + b * RING_BLOCK_BYTES;
            p->list->SetPredication(work.marks.Get(), std::uint64_t{b} * 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
            p->list->CopyBufferRegion(work.inline_buf.Get(), at, work.ring.Get(), at, RING_BLOCK_BYTES);
        }
    }
    p->list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    check(p->list->Close(), "download list close");
    return p;
}

std::optional<CardPlan> CardHost::card_before(std::span<const GpuWrite> writes, bool late) {
    using shadow::Range;
    using shadow::Shadow;
    if (!shadow::cards()) {
        return std::nullopt;
    }
    bool flown = late && relay();


    std::vector<std::pair<Shadow*, std::vector<Range>>> declared;
    for (const auto& w : writes) {
        if (!w.shadow->carded()) {
            continue;
        }
        Range range{w.address, w.address + w.len};
        auto it = std::find_if(declared.begin(), declared.end(), [&](const auto& d) { return d.first == w.shadow; });
        if (it == declared.end()) {
            declared.emplace_back(w.shadow, std::vector<Range>{range});
        } else if (it->second.back() != range) {
            it->second.push_back(range);
        }
    }


    std::unique_lock<std::mutex> order;
    std::uint64_t sent_as = 0;
    if (relay()) {
        order = std::unique_lock(QUEUE_ORDER_LOCK);
        sent_as = ++QUEUE_ORDER;
    }
    auto dirty = shadow::card_dirty(sent_as);
    if (declared.empty() && dirty.empty()) {
        return std::nullopt;
    }
    std::lock_guard work_guard(work_lock_);
    if (!work_) {
        return std::nullopt;
    }
    Work& work = *work_;


    std::vector<Shadow*> whole;
    for (const auto& [shadow, ranges] : declared) {
        if (shadow->card_make_before([&](std::uint64_t size) { return device_.uav_buffer(shadow::card_hash_bytes(size)); })) {
            whole.push_back(shadow);
            if (std::none_of(dirty.begin(), dirty.end(), [&](const auto& d) { return d.first == shadow; })) {
                dirty.emplace_back(shadow, std::vector<Range>{});
            }
        }
    }
    std::vector<std::pair<int, Prelude>> used;
    if (!dirty.empty()) {
        std::optional<Prelude> sync;
        if (!work.sync.empty()) {
            sync = std::move(work.sync.back());
            work.sync.pop_back();
        } else {
            sync = prelude();
        }
        if (!sync) {
            fail("no host list to bring host copies in the card's memory up to date");
        }
        ID3D12GraphicsCommandList* list = sync->list.Get();
        check(sync->allocator->Reset(), "sync allocator reset");
        check(list->Reset(sync->allocator.Get(), nullptr), "sync list reset");
        std::vector<shadow::CardSpan> runs;
        for (const auto& [shadow, ranges] : dirty) {
            auto sides = shadow->card_sides();
            if (!sides) {
                continue;
            }
            ID3D12Resource* buffer = shadow->buffer();
            for (const auto& [a, b] : ranges) {
                list->CopyBufferRegion(buffer, a, sides->staging, a, b - a);
                shadow::CARD_STATS[0].fetch_add(b - a, std::memory_order_relaxed);
            }
            shadow::CARD_STATS[1].fetch_add(ranges.size(), std::memory_order_relaxed);
            if (!sides->before) {
                continue;
            }


            if (!ranges.empty()) {
                auto b = transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                list->ResourceBarrier(1, &b);
            }
            list->SetComputeRootSignature(work.root.Get());
            list->SetPipelineState(work.pso.Get());
            list->SetComputeRootShaderResourceView(1, buffer->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2, sides->before->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, work.differs_target()->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, work.ring_target()->GetGPUVirtualAddress());
            runs.clear();
            if (std::find(whole.begin(), whole.end(), shadow) != whole.end()) {
                runs.push_back(shadow->card_run(0, ~usize{0}));
            } else {
                runs.reserve(ranges.size());
                for (const auto& [a, b] : ranges) {
                    runs.push_back(shadow->card_run(a, b));
                }
            }
            if (rehash_runs()) {
                runs = shadow::joined_piece_runs(std::move(runs));
            }
            for (const auto& run : runs) {
                if (run.pieces == 0) {
                    continue;
                }
                std::uint32_t constants[7] = {run.first, run.skew, static_cast<std::uint32_t>(sides->size), run.pieces, 0, 0, 0};
                list->SetComputeRoot32BitConstants(0, 7, constants, 0);
                list->Dispatch(run.pieces, 1, 1);
            }
        }
        check(list->Close(), "sync list close");
        ID3D12CommandList* lists[] = {list};
        device_.queue()->ExecuteCommandLists(1, lists);
        used.emplace_back(0, std::move(*sync));
    }
    if (order.owns_lock()) {
        order.unlock();
    }
    CardPlan plan;
    plan.sent_as = sent_as;
    plan.synced.reserve(dirty.size());
    for (const auto& d : dirty) {
        plan.synced.push_back(d.first);
    }
    plan.lists = std::move(used);


    std::uint32_t at = work.taken;
    const std::uint32_t first_dword = at;
    for (auto& [shadow, ranges] : declared) {
        std::sort(ranges.begin(), ranges.end());
        std::vector<Range> spans;
        for (const auto& [from, to] : ranges) {
            if (!spans.empty() && from <= spans.back().second) {
                spans.back().second = std::max(spans.back().second, to);
            } else {
                spans.emplace_back(from, to);
            }
        }
        auto sides = shadow->card_sides();
        bool twin = sides && sides->before;
        for (const auto& [from, to] : spans) {
            auto span = shadow->card_span(from, to);
            if (!span) {
                continue;
            }
            bool compared = twin && (std::uint64_t{at} + span->pieces) * 4 <= work.differs_bytes;
            plan.ranges.push_back({shadow, {from, to}, *span, at, compared});
            if (compared) {
                at += span->pieces;
            }
        }
    }
    work.taken = at;
    if (std::any_of(plan.ranges.begin(), plan.ranges.end(), [](const CardRange& r) { return r.compared; })) {
        std::optional<Prelude> chk;
        if (!work.check.empty()) {
            chk = std::move(work.check.back());
            work.check.pop_back();
        } else {
            chk = prelude();
        }
        if (!chk) {
            fail("no host list for the compare of host copies in the card's memory");
        }
        bool use_inline = work.inline_slots > 0 && (flown || !late);
        std::uint32_t mode = flown ? (use_inline ? 4u : 2u) : (use_inline ? 3u : 1u);


        std::uint32_t ring = flown || work.ring ? work.inline_slots : std::min(work.inline_slots, inline_pieces_within(plan.ranges, card_inline_budget()));
        std::uint32_t inline_limit = work.inline_tail + ring;
        bool marks = use_inline && work.ring;
        ID3D12GraphicsCommandList* list = chk->list.Get();
        check(chk->allocator->Reset(), "compare allocator reset");
        check(list->Reset(chk->allocator.Get(), nullptr), "compare list reset");

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = nullptr;
        list->ResourceBarrier(1, &uav);
        auto marks_pass = [&](std::uint32_t phase, std::uint32_t reset) {
            list->SetComputeRootSignature(work.marks_root.Get());
            list->SetPipelineState(work.marks_pso.Get());
            std::uint32_t c[3] = {phase, reset, inline_limit};
            list->SetComputeRoot32BitConstants(0, 3, c, 0);
            list->SetComputeRootUnorderedAccessView(1, work.ring->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2, work.marks->GetGPUVirtualAddress());
            list->Dispatch(1, 1, 1);
            list->ResourceBarrier(1, &uav);
        };
        if (marks) {
            marks_pass(0, work.reset_pending ? 1 : 0);
            work.reset_pending = false;
        }
        list->SetComputeRootSignature(work.root.Get());
        list->SetPipelineState(work.pso.Get());
        list->SetComputeRootUnorderedAccessView(3, work.differs_target()->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(4, work.ring_target()->GetGPUVirtualAddress());
        for (const auto& r : plan.ranges) {
            if (!r.compared || r.span.pieces == 0) {
                continue;
            }
            auto sides = r.shadow->card_sides();
            if (!sides || !sides->before) {
                continue;
            }
            std::uint32_t constants[7] = {r.span.first, r.span.skew, static_cast<std::uint32_t>(sides->size), r.span.pieces, r.at * 4, mode, inline_limit};
            list->SetComputeRoot32BitConstants(0, 7, constants, 0);
            list->SetComputeRootShaderResourceView(1, r.shadow->buffer()->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2, sides->before->GetGPUVirtualAddress());
            list->Dispatch(r.span.pieces, 1, 1);
        }
        if (marks) {


            list->ResourceBarrier(1, &uav);
            marks_pass(1, 0);
            D3D12_RESOURCE_BARRIER to[2] = {transition(work.marks.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PREDICATION),
                                            transition(work.ring.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
            list->ResourceBarrier(2, to);
        }
        if (work.differs_vram) {

            auto to = transition(work.differs_vram.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to);
            list->CopyBufferRegion(work.differs.Get(), std::uint64_t{first_dword} * 4, work.differs_vram.Get(), std::uint64_t{first_dword} * 4,
                                   std::uint64_t{at - first_dword} * 4);
        }
        check(list->Close(), "compare list close");
        plan.check = chk->list;
        plan.lists.emplace_back(1, std::move(*chk));
        if (marks) {


            bool from_zero = work.out == 0;
            auto& pool = from_zero ? work.downloads_from_zero : work.downloads;
            std::optional<Prelude> down;
            if (!pool.empty()) {
                down = std::move(pool.back());
                pool.pop_back();
            } else {
                down = record_download(work, from_zero);
            }
            plan.download = down->list;
            plan.lists.emplace_back(from_zero ? 3 : 2, std::move(*down));
        }
    }
    work.out += 1;
    return plan;
}

bool CardHost::card_results_low() {
    std::lock_guard lock(work_lock_);
    return work_ && std::uint64_t{work_->taken} * 8 > work_->differs_bytes;
}

void CardHost::card_done(CardPlan plan) {
    std::lock_guard lock(work_lock_);
    if (!work_) {
        return;
    }
    Work& work = *work_;
    for (auto& [kind, list] : plan.lists) {
        (kind == 3 ? work.downloads_from_zero : kind == 2 ? work.downloads : kind == 1 ? work.check : work.sync).push_back(std::move(list));
    }
    work.out = work.out > 0 ? work.out - 1 : 0;


    if (!relay() || work.out == 0) {
        work.taken = 0;
        work.inline_tail = 0;
        *reinterpret_cast<volatile std::uint32_t*>(work.inline_data) = 0;


        work.reset_pending = work.ring != nullptr;
    }
}

void CardHost::card_back(const CardPlan& plan, Seen* seen) {
    if (plan.ranges.empty()) {
        return;
    }
    std::lock_guard lock(work_lock_);
    if (!work_) {
        return;
    }
    Work& work = *work_;
    const std::uint32_t* differs = work.differs_data;
    struct Inline {
        std::uint32_t range;
        usize lo, hi, off;
        usize where() const { return off; }
    };
    struct Fetch {
        std::uint32_t range;
        usize lo, hi;
        std::uint64_t at;
        usize where() const { return static_cast<usize>(at); }
    };


    thread_local std::vector<Inline> inline_kept;
    thread_local std::vector<Fetch> pieces_kept;
    auto& inline_pieces = inline_kept;
    auto& pieces = pieces_kept;
    inline_pieces.clear();
    pieces.clear();
    std::uint64_t inline_bytes = 0, total = 0;
    std::uint32_t inline_mask = work.inline_slots > 0 ? work.inline_slots - 1 : 0;
    usize inline_len = static_cast<usize>(work.inline_bytes);
    for (std::uint32_t i = 0; i < plan.ranges.size(); ++i) {
        const CardRange& r = plan.ranges[i];
        for (std::uint32_t p = 0; p < r.span.pieces; ++p) {
            std::uint32_t d = r.compared ? differs[r.at + p] : 1;
            if (d == 0) {
                continue;
            }
            auto [lo, hi] = r.shadow->card_piece(r.span, p);


            if (lo >= hi || (seen && !seen->emplace(reinterpret_cast<usize>(r.shadow), lo, hi).second)) {
                continue;
            }
            if (d >= 2 && work.inline_slots > 0) {
                std::uint32_t slot = (d - 2) & inline_mask;
                usize shader_lo = saturating_sub(static_cast<usize>(r.span.first + p) * 65536, static_cast<usize>(r.span.skew)) & ~usize{15};
                usize off = CARD_INLINE_HEADER + static_cast<usize>(slot) * CARD_INLINE_STRIDE + (lo - shader_lo);
                if (lo >= shader_lo && off + (hi - lo) <= inline_len) {
                    inline_pieces.push_back({i, lo, hi, off});
                    inline_bytes += hi - lo;
                    continue;
                }
            }
            pieces.push_back({i, lo, hi, total});
            total += hi - lo;
        }
        shadow::CARD_STATS[2].fetch_add(r.span.pieces, std::memory_order_relaxed);
    }


    auto merge_run = [&](auto* first, usize count, const std::uint8_t* base, std::uint64_t bytes, bool wide_ok) {
        auto one = [&](usize k) {
            const auto& pc = first[k];
            plan.ranges[pc.range].shadow->card_merge(pc.lo, base + pc.where(), pc.hi - pc.lo);
        };
        if (wide_ok && count >= 8 && bytes >= WIDE_MERGE_LEAST) {
            usize parts = std::min<usize>(count, wide::threads() + 1);
            usize each = (count + parts - 1) / parts;


            std::vector<std::vector<std::pair<shadow::Shadow*, shadow::Range>>> noted((count + each - 1) / each);
            std::vector<wide::Job> jobs;
            for (usize from = 0, part = 0; from < count; from += each, ++part) {
                usize to = std::min(count, from + each);
                jobs.emplace_back([&, from, to, part] {
                    std::vector<shadow::Range> changed;
                    auto& mine = noted[part];
                    for (usize k = from; k < to; ++k) {
                        const auto& pc = first[k];
                        changed.clear();
                        plan.ranges[pc.range].shadow->card_merge(pc.lo, base + pc.where(), pc.hi - pc.lo, &changed);
                        for (const auto& r : changed) {
                            mine.emplace_back(plan.ranges[pc.range].shadow, r);
                        }
                    }
                });
            }
            wide::run(jobs);
            for (const auto& part : noted) {
                for (const auto& [s, r] : part) {
                    s->card_changed(r.first, r.second);
                }
            }
        } else {
            for (usize k = 0; k < count; ++k) {
                one(k);
            }
        }
    };
    auto merge_inline = [&] {
        if (inline_pieces.empty()) {
            return;
        }
        shadow::CARD_STATS[3].fetch_add(inline_pieces.size(), std::memory_order_relaxed);
        shadow::CARD_STATS[4].fetch_add(inline_bytes, std::memory_order_relaxed);


        merge_run(inline_pieces.data(), inline_pieces.size(), work.inline_data, inline_bytes, work.ring != nullptr && card_pipeline());
    };
    if (pieces.empty()) {
        merge_inline();
        return;
    }
    if (!work.fetched || work.fetched_bytes < total) {
        std::uint64_t size = (total + (std::uint64_t{16} << 20) - 1) / (std::uint64_t{16} << 20) * (std::uint64_t{16} << 20);
        work.fetched = nullptr;
        auto b = device_.shared_buffer(size);
        work.fetched = b ? b->first : nullptr;
        work.fetched_data = b ? b->second : nullptr;
        work.fetched_bytes = b ? size : 0;
    }
    if (!work.fetched) {
        fail("no memory to fetch what shaders wrote to the card's memory (" + std::to_string(total) + " bytes)");
    }
    auto waiting = std::chrono::steady_clock::now();
    if (!card_pipeline()) {


        merge_inline();
        Prelude& f = work.fetch;
        check(f.allocator->Reset(), "fetch allocator reset");
        check(f.list->Reset(f.allocator.Get(), nullptr), "fetch list reset");
        for (const auto& pc : pieces) {
            f.list->CopyBufferRegion(work.fetched.Get(), pc.at, plan.ranges[pc.range].shadow->buffer(), pc.lo, pc.hi - pc.lo);
        }
        check(f.list->Close(), "fetch list close");
        device_.execute_and_wait({f.list.Get()});
        merge_run(pieces.data(), pieces.size(), work.fetched_data, total, wide_merge());
    } else {


        std::uint64_t part_bytes = std::clamp<std::uint64_t>(total / 4, std::uint64_t{2} << 20, std::uint64_t{16} << 20);
        struct Part {
            usize first, count;
            std::uint64_t bytes, fence;
        };
        std::vector<Part> parts;
        usize lists = 0;
        for (usize i = 0; i < pieces.size();) {
            usize j = i;
            std::uint64_t bytes = 0;
            while (j < pieces.size() && (j == i || bytes + (pieces[j].hi - pieces[j].lo) <= part_bytes)) {
                bytes += pieces[j].hi - pieces[j].lo;
                ++j;
            }
            if (lists == work.fetches.size()) {
                auto made = device_.make_prelude(work.fetch_on_copy_queue ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT);
                if (!made) {
                    fail("no host list to fetch what shaders wrote to the card's memory");
                }
                work.fetches.push_back(std::move(*made));
            }
            Prelude& f = work.fetches[lists++];
            check(f.allocator->Reset(), "fetch allocator reset");
            check(f.list->Reset(f.allocator.Get(), nullptr), "fetch list reset");
            for (usize k = i; k < j; ++k) {
                const auto& pc = pieces[k];
                f.list->CopyBufferRegion(work.fetched.Get(), pc.at, plan.ranges[pc.range].shadow->buffer(), pc.lo, pc.hi - pc.lo);
            }
            check(f.list->Close(), "fetch list close");
            ID3D12CommandList* l[] = {f.list.Get()};
            std::uint64_t fence;
            if (work.fetch_on_copy_queue) {


                device_.copy_queue()->ExecuteCommandLists(1, l);
                fence = ++work.copy_value;
                check(device_.copy_queue()->Signal(work.copy_fence.Get(), fence), "fetch signal");
            } else {
                device_.queue()->ExecuteCommandLists(1, l);
                fence = device_.signal();
            }
            parts.push_back({i, j - i, bytes, fence});
            i = j;
        }
        merge_inline();
        for (const auto& part : parts) {
            if (work.fetch_on_copy_queue) {
                if (work.copy_fence->GetCompletedValue() < part.fence) {
                    check(work.copy_fence->SetEventOnCompletion(part.fence, work.copy_event), "fetch wait");
                    WaitForSingleObject(work.copy_event, INFINITE);
                }
            } else {
                device_.wait(part.fence);
            }
            merge_run(pieces.data() + part.first, part.count, work.fetched_data, part.bytes, true);
        }
    }
    shadow::CARD_STATS[3].fetch_add(pieces.size(), std::memory_order_relaxed);
    shadow::CARD_STATS[4].fetch_add(total, std::memory_order_relaxed);
    shadow::CARD_STATS[5].fetch_add(1, std::memory_order_relaxed);
    shadow::CARD_STATS[6].fetch_add(micros_since(waiting), std::memory_order_relaxed);
}

void CardHost::relay_card_back(std::span<const CardPlan* const> plans) {
    using shadow::Range;
    using shadow::Shadow;

    const std::uint32_t* differs = nullptr;
    usize dwords = 0;
    const std::uint8_t* inline_data = nullptr;
    usize inline_len = 0;
    std::uint32_t inline_slots = 0;
    bool have_work = false;
    {
        std::lock_guard lock(work_lock_);
        if (work_) {
            have_work = true;
            differs = work_->differs_data;
            dwords = static_cast<usize>(work_->differs_bytes / 4);
            inline_data = work_->inline_data;
            inline_len = static_cast<usize>(work_->inline_bytes);
            inline_slots = work_->inline_slots;
        }
    }
    struct Key {
        usize copy;
        std::uint32_t piece;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const noexcept { return std::hash<usize>{}(k.copy * 0x9E3779B97F4A7C15ull ^ k.piece); }
    };
    struct PieceWork {
        Shadow* shadow;
        std::uint32_t skew, piece;
        bool inline_capture;
        std::uint32_t slot;
        std::uint64_t sent_as;
        std::vector<Range> subranges;
    };
    std::unordered_map<Key, usize, KeyHash> by_piece;   
    std::vector<PieceWork> works;
    std::optional<std::uint32_t> max_ticket;
    bool inline_enabled = have_work && inline_slots > 0;
    if (have_work) {
        std::uint32_t inline_mask = inline_slots > 0 ? inline_slots - 1 : 0;
        std::unordered_map<Key, std::optional<std::uint32_t>, KeyHash> plan_pieces;
        for (const CardPlan* plan : plans) {


            plan_pieces.clear();
            for (const auto& r : plan->ranges) {
                usize copy = reinterpret_cast<usize>(r.shadow);
                for (std::uint32_t p = 0; p < r.span.pieces; ++p) {
                    std::uint32_t d = r.compared ? (static_cast<usize>(r.at) + p < dwords ? differs[r.at + p] : 1) : 1;
                    if (d == 0) {
                        continue;
                    }
                    Key key{copy, r.span.first + p};
                    if (d >= 2 && inline_slots > 0) {
                        std::uint32_t ticket = d - 2;
                        if (!max_ticket || static_cast<std::int32_t>(ticket - *max_ticket) > 0) {
                            max_ticket = ticket;
                        }
                        plan_pieces[key] = ticket & inline_mask;
                    } else {
                        plan_pieces.try_emplace(key, std::nullopt);
                    }
                }
            }
            for (const auto& r : plan->ranges) {
                usize copy = reinterpret_cast<usize>(r.shadow);
                for (std::uint32_t p = 0; p < r.span.pieces; ++p) {
                    Key key{copy, r.span.first + p};
                    auto found = plan_pieces.find(key);
                    if (found == plan_pieces.end()) {
                        continue;
                    }
                    auto [lo, hi] = r.shadow->card_piece(r.span, p);
                    if (lo >= hi) {
                        continue;
                    }
                    bool inl = found->second.has_value();
                    std::uint32_t slot = inl ? *found->second : 0;
                    auto [it, fresh] = by_piece.try_emplace(key, works.size());
                    if (fresh) {
                        works.push_back({r.shadow, r.span.skew, key.piece, inl, slot, plan->sent_as, {{lo, hi}}});
                    } else {


                        PieceWork& w = works[it->second];
                        w.inline_capture = inl;
                        w.slot = slot;
                        w.sent_as = plan->sent_as;
                        if (std::find(w.subranges.begin(), w.subranges.end(), Range{lo, hi}) == w.subranges.end()) {
                            w.subranges.emplace_back(lo, hi);
                        }
                    }
                }
                shadow::CARD_STATS[2].fetch_add(r.span.pieces, std::memory_order_relaxed);
            }
        }
    }
    struct Fallback {
        Shadow* shadow;
        usize lo, hi;
        std::uint64_t at;
    };
    std::vector<Fallback> fallback;
    std::uint64_t fallback_total = 0, inline_pieces = 0, inline_bytes = 0;
    for (const auto& w : works) {
        if (w.inline_capture && have_work) {
            usize shader_lo = saturating_sub(static_cast<usize>(w.piece) * 65536, static_cast<usize>(w.skew)) & ~usize{15};
            usize slot_base = CARD_INLINE_HEADER + static_cast<usize>(w.slot) * CARD_INLINE_STRIDE;
            for (const auto& [lo, hi] : w.subranges) {
                usize off = slot_base + (lo - shader_lo);
                usize len = hi - lo;
                if (lo >= shader_lo && off + len <= inline_len) {
                    w.shadow->card_merge_relay(lo, inline_data + off, len, w.sent_as);
                    ++inline_pieces;
                    inline_bytes += len;
                } else {
                    fallback.push_back({w.shadow, lo, hi, fallback_total});
                    fallback_total += len;
                }
            }
        } else {
            for (const auto& [lo, hi] : w.subranges) {
                fallback.push_back({w.shadow, lo, hi, fallback_total});
                fallback_total += hi - lo;
            }
        }
    }

    if (max_ticket) {
        std::lock_guard lock(work_lock_);
        if (work_) {
            std::uint32_t next_tail = *max_ticket + 1;
            if (static_cast<std::int32_t>(next_tail - work_->inline_tail) > 0) {
                work_->inline_tail = next_tail;
            }
        }
    }
    if (inline_pieces > 0) {
        shadow::CARD_STATS[3].fetch_add(inline_pieces, std::memory_order_relaxed);
        shadow::CARD_STATS[4].fetch_add(inline_bytes, std::memory_order_relaxed);
    }
    std::optional<std::uint64_t> fallback_after;
    if (!fallback.empty()) {
        if (!relay_fetch_) {
            relay_fetch_ = make_relay_fetch();
        }
        if (!relay_fetch_) {
            fail("no list, fence or event to fetch what shaders wrote to the card's memory (VICTORY_RELAY=1)");
        }
        RelayFetch& f = *relay_fetch_;
        if (!f.buffer || f.bytes < fallback_total) {
            std::uint64_t size = (fallback_total + (std::uint64_t{16} << 20) - 1) / (std::uint64_t{16} << 20) * (std::uint64_t{16} << 20);
            f.buffer = nullptr;
            auto b = device_.shared_buffer(size);
            f.buffer = b ? b->first : nullptr;
            f.data = b ? b->second : nullptr;
            f.bytes = b ? size : 0;
        }
        if (!f.buffer) {
            fail("no memory to fetch what shaders wrote to the card's memory (" + std::to_string(fallback_total) + " bytes, VICTORY_RELAY=1)");
        }
        auto waiting = std::chrono::steady_clock::now();


        struct Part {
            usize first, count;
            std::uint64_t value;
        };
        std::vector<Part> parts;
        std::vector<ID3D12CommandList*> lists;
        auto type = f.on_copy_queue ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT;
        std::uint64_t part_bytes = card_pipeline() ? std::clamp<std::uint64_t>(fallback_total / 4, std::uint64_t{2} << 20, std::uint64_t{16} << 20)
                                                   : ~std::uint64_t{0};
        for (usize i = 0; i < fallback.size();) {
            usize j = i;
            std::uint64_t bytes = 0;
            while (j < fallback.size() && (j == i || bytes + (fallback[j].hi - fallback[j].lo) <= part_bytes)) {
                bytes += fallback[j].hi - fallback[j].lo;
                ++j;
            }
            Prelude* pl = &f.list;
            if (!parts.empty()) {
                usize k = parts.size() - 1;
                if (k == f.parts.size()) {
                    auto made = device_.make_prelude(type);
                    if (!made) {
                        fail("no host list to fetch what shaders wrote to the card's memory (VICTORY_RELAY=1)");
                    }
                    f.parts.push_back(std::move(*made));
                }
                pl = &f.parts[k];
            }
            check(pl->allocator->Reset(), "relay fetch allocator reset");
            check(pl->list->Reset(pl->allocator.Get(), nullptr), "relay fetch list reset");
            for (usize k = i; k < j; ++k) {
                const auto& fb = fallback[k];
                pl->list->CopyBufferRegion(f.buffer.Get(), fb.at, fb.shadow->buffer(), fb.lo, fb.hi - fb.lo);
            }
            check(pl->list->Close(), "relay fetch list close");
            lists.push_back(pl->list.Get());
            parts.push_back({i, j - i, 0});
            i = j;
        }
        std::uint64_t after;
        ID3D12CommandQueue* q = f.on_copy_queue ? device_.copy_queue() : device_.queue();
        {


            std::lock_guard order(QUEUE_ORDER_LOCK);
            if (f.on_copy_queue) {
                f.sent += 1;
                check(device_.queue()->Signal(f.fence.Get(), f.sent), "relay fetch order signal");
                check(q->Wait(f.fence.Get(), f.sent), "relay fetch order wait");
            }
            for (usize k = 0; k < parts.size(); ++k) {
                q->ExecuteCommandLists(1, &lists[k]);
                f.sent += 1;
                check(q->Signal(f.fence.Get(), f.sent), "relay fetch signal");
                parts[k].value = f.sent;
            }
            after = QUEUE_ORDER;
        }
        fallback_after = after;
        for (const auto& part : parts) {
            if (f.fence->GetCompletedValue() < part.value) {
                check(f.fence->SetEventOnCompletion(part.value, f.event), "relay fetch wait");
                WaitForSingleObject(f.event, INFINITE);
            }
            for (usize k = part.first; k < part.first + part.count; ++k) {
                const auto& fb = fallback[k];
                fb.shadow->card_merge_relay(fb.lo, f.data + fb.at, fb.hi - fb.lo, after);
            }
        }
        std::uint64_t us = micros_since(waiting);
        shadow::CARD_STATS[3].fetch_add(fallback.size(), std::memory_order_relaxed);
        shadow::CARD_STATS[4].fetch_add(fallback_total, std::memory_order_relaxed);
        shadow::CARD_STATS[5].fetch_add(1, std::memory_order_relaxed);
        shadow::CARD_STATS[6].fetch_add(us, std::memory_order_relaxed);
    }


    if (inline_enabled) {
        if (!plans.empty()) {
            shadow::relay_fetched(plans.back()->sent_as);
        }
    } else if (fallback_after) {
        shadow::relay_fetched(*fallback_after);
    } else {
        shadow::relay_fetched(queue_order());
    }
}

std::pair<Flight, std::array<std::uint64_t, 2>> CardHost::send(std::span<const GpuWrite> writes, Prelude title, ComPtr<ID3D12Resource> source,
                                                                std::vector<shadow::Declared> back) {
    auto started = std::chrono::steady_clock::now();
    auto plan = card_before(writes, true);
    ID3D12CommandList* lists[3] = {title.list.Get(), nullptr, nullptr};
    UINT n = 1;
    if (plan && plan->check) {
        lists[n++] = plan->check.Get();
    }
    if (plan && plan->download) {
        lists[n++] = plan->download.Get();
    }
    auto prepared = std::chrono::steady_clock::now();
    device_.queue()->ExecuteCommandLists(n, lists);
    std::uint64_t fence = device_.signal();
    auto sent = std::chrono::steady_clock::now();
    Flight f{std::move(plan), std::move(back), fence, std::move(title), std::move(source)};
    return {std::move(f), {nanos(prepared - started), nanos(sent - prepared)}};
}

std::array<std::uint64_t, 2> CardHost::land(std::vector<Flight> flights) {
    auto started = std::chrono::steady_clock::now();
    if (!flights.empty()) {
        device_.wait(flights.back().fence);
    }
    auto ran = std::chrono::steady_clock::now();
    std::vector<const CardPlan*> plans;
    for (const auto& f : flights) {
        if (f.card) {
            plans.push_back(&*f.card);
        }
    }
    relay_card_back(plans);

    std::vector<shadow::Declared> back;
    for (const auto& f : flights) {
        for (const auto& [s, ranges] : f.back) {
            auto it = std::find_if(back.begin(), back.end(), [&](const shadow::Declared& d) { return d.first == s; });
            if (it == back.end()) {
                back.emplace_back(s, std::vector<shadow::Range>{});
                it = back.end() - 1;
            }
            for (const auto& r : ranges) {
                if (std::find(it->second.begin(), it->second.end(), r) == it->second.end()) {
                    it->second.push_back(r);
                }
            }
        }
    }
    shadow::relay_hand_back(back);


    for (const auto& f : flights) {
        for (const auto& [s, ranges] : f.back) {
            s->copy_back_done(ranges);
        }
    }
    for (auto& f : flights) {
        if (f.card) {
            card_done(std::move(*f.card));
        }
    }
    auto done = std::chrono::steady_clock::now();
    return {nanos(ran - started), nanos(done - ran)};
}

std::array<std::uint64_t, 3> CardHost::submit(std::span<const GpuWrite> writes, ID3D12CommandList* title, std::span<const shadow::Declared> back) {
    auto started = std::chrono::steady_clock::now();
    auto plan = card_before(writes, false);
    ID3D12CommandList* lists[3] = {title, nullptr, nullptr};
    UINT n = 1;
    if (plan && plan->check) {
        lists[n++] = plan->check.Get();
    }
    if (plan && plan->download) {
        lists[n++] = plan->download.Get();
    }
    auto prepared = std::chrono::steady_clock::now();
    device_.queue()->ExecuteCommandLists(n, lists);
    device_.wait(device_.signal());
    auto ran = std::chrono::steady_clock::now();
    if (plan) {
        card_back(*plan);
    }
    shadow::hand_back(back);
    if (plan) {
        card_done(std::move(*plan));
    }
    if (relay()) {
        shadow::relay_fetched(queue_order());
    }
    auto done = std::chrono::steady_clock::now();
    return {nanos(prepared - started), nanos(ran - prepared), nanos(done - ran)};
}

}   
