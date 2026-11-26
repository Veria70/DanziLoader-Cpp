// SPDX-License-Identifier: GPL-3.0-or-later
// Port of crates/victory-gpu/src/descriptor.rs (see descriptor.hpp).
#include "descriptor.hpp"

#include <windows.h>

#include <nmmintrin.h>

#include <algorithm>
#include <bit>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "core/env.hpp"
#include "core/win.hpp"
#include "host_heap.hpp"
#include "resource.hpp"
#include "resource_iids.hpp"

namespace danzi::d3d12x {

namespace {

const std::array<Iid, 4> HEAP_IIDS = {
    guid(0x8efb471d, 0x616c, 0x4f49, {0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51}),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
};

constexpr std::uint32_t VIEWS_KIND = 0;
constexpr std::uint32_t SAMPLERS = 1;
constexpr std::uint32_t SHADER_VISIBLE = 1;


constexpr std::uint32_t INCREMENTS[4] = {32, 16, 128, 128};
inline std::uint32_t increment(std::uint32_t kind) { return kind < 4 ? INCREMENTS[kind] : 0; }


struct Zeroed {
    std::uint8_t* ptr = nullptr;
    bool pages = false;
    Zeroed() = default;
    Zeroed(const Zeroed&) = delete;
    Zeroed& operator=(const Zeroed&) = delete;
    ~Zeroed() {
        if (ptr) {
            if (pages) {
                VirtualFree(ptr, 0, MEM_RELEASE);
            } else {
                _aligned_free(ptr);
            }
        }
    }
    bool make(std::uint64_t size) {
        if (size >= (64u << 10)) {
            ptr = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            pages = true;
        } else {
            ptr = static_cast<std::uint8_t*>(_aligned_malloc(static_cast<std::size_t>(size), 64));
            if (ptr) {
                std::memset(ptr, 0, static_cast<std::size_t>(size));
            }
        }
        return ptr != nullptr;
    }
};

}   

struct DescHeap {
    Object object;
    DescriptorHeapDesc desc;

    std::uint64_t start;
    Zeroed memory;


    danzi::FastLock pending_lock;
    std::vector<std::uint32_t> pending;
    std::atomic<HostHeap*> host{nullptr};
    std::atomic<bool> host_tried{false};
    std::unique_ptr<HostHeap> host_owned;


    bool index(std::uint64_t handle, std::uint32_t& out) const {
        std::uint64_t step = increment(desc.kind);
        if (step == 0 || handle < start) {
            return false;
        }
        std::uint64_t i = (handle - start) >> std::countr_zero(step);
        if (i >= desc.count) {
            return false;
        }
        out = static_cast<std::uint32_t>(i);
        return true;
    }

    ~DescHeap();
};

namespace {


struct Visible {
    std::uint64_t start, end;
    DescHeap* heap;
};


danzi::FastLock SHADER_HEAPS_LOCK;
std::vector<Visible> SHADER_HEAPS;
std::atomic<const std::vector<Visible>*> SHADER_SNAPSHOT{nullptr};
std::vector<std::unique_ptr<std::vector<Visible>>> OLD_SNAPSHOTS;


void publish_heaps() {
    auto next = std::make_unique<std::vector<Visible>>(SHADER_HEAPS);
    SHADER_SNAPSHOT.store(next->empty() ? nullptr : next.get(), std::memory_order_release);
    OLD_SNAPSHOTS.push_back(std::move(next));
}


void mirror(std::uint64_t first, std::uint64_t count, std::uint64_t step) {
    const std::vector<Visible>* heaps = SHADER_SNAPSHOT.load(std::memory_order_acquire);
    if (!heaps) {
        return;
    }
    DescHeap* held = nullptr;
    for (std::uint64_t k = 0; k < count; ++k) {
        std::uint64_t handle = first + k * step;
        DescHeap* heap = nullptr;
        for (const Visible& v : *heaps) {
            if (handle >= v.start && handle < v.end) {
                heap = v.heap;
                break;
            }
        }
        std::uint32_t index;
        if (!heap || !heap->index(handle, index)) {
            continue;
        }

        if (HostHeap* host = heap->host.load(std::memory_order_acquire)) {
            if (held) {
                held->pending_lock.unlock();
                held = nullptr;
            }
            host->mirror(index, handle, false);
            continue;
        }
        if (heap != held) {
            if (held) {
                held->pending_lock.unlock();
            }
            heap->pending_lock.lock();
            held = heap;
        }

        if (HostHeap* host = heap->host.load(std::memory_order_acquire)) {
            held->pending_lock.unlock();
            held = nullptr;
            host->mirror(index, handle, false);
            continue;
        }


        if (heap->pending.size() == heap->pending.capacity()) {
            heap->pending.reserve(std::max<std::size_t>(64, heap->pending.capacity() * 2));
        }
        heap->pending.push_back(index);
    }
    if (held) {
        held->pending_lock.unlock();
    }
}


constexpr std::size_t WORDS[] = {0, 10, 10, 13, 27, 30};


std::uint32_t crc_words(std::uint32_t h, const std::uint32_t* w, std::size_t n) {
    std::uint64_t h0 = h, h1 = 0x85ebca6bu;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        std::uint64_t x, y;
        std::memcpy(&x, w + i, 8);
        std::memcpy(&y, w + i + 2, 8);
        h0 = _mm_crc32_u64(h0, x);
        h1 = _mm_crc32_u64(h1, y);
    }
    if (i + 2 <= n) {
        std::uint64_t x;
        std::memcpy(&x, w + i, 8);
        h0 = _mm_crc32_u64(h0, x);
        i += 2;
    }
    if (i < n) {
        h1 = _mm_crc32_u32(static_cast<std::uint32_t>(h1), w[i]);
    }
    return static_cast<std::uint32_t>(h0) ^ static_cast<std::uint32_t>(h1 * 0x9e3779b1u);
}


bool same_words(const std::uint32_t* x, const std::uint32_t* y, std::size_t n) {
    std::size_t i = 0;
    std::uint64_t diff = 0;
    for (; i + 2 <= n; i += 2) {
        std::uint64_t a, b;
        std::memcpy(&a, x + i, 8);
        std::memcpy(&b, y + i, 8);
        diff |= a ^ b;
    }
    if (i < n) {
        diff |= x[i] ^ y[i];
    }
    return diff == 0;
}

std::uint32_t view_hash(const View& v) {
    std::uint32_t h = _mm_crc32_u32(0x9e3779b9u, std::uint32_t{v.kind} | std::uint32_t{v.flag} << 8);
    h = static_cast<std::uint32_t>(_mm_crc32_u64(h, v.a));
    h = static_cast<std::uint32_t>(_mm_crc32_u64(h, v.b));
    return crc_words(h, v.words.data(), WORDS[v.kind]);
}


struct ViewIndex {
    struct Slot {
        std::uint32_t hash = 0;
        std::uint32_t value = 0;
    };
    std::vector<Slot> slots = std::vector<Slot>(64);
    std::size_t used = 0;

    std::size_t mask() const { return slots.size() - 1; }


    template <class List>
    std::optional<std::uint32_t> find(const View& view, std::uint32_t h, const List& list) const {
        for (std::size_t at = h & mask();; at = (at + 1) & mask()) {
            const Slot& s = slots[at];
            if (s.value == 0) {
                return std::nullopt;
            }
            if (s.hash == h && list[s.value - 1].first == view) {
                return s.value - 1;
            }
        }
    }

    void insert(std::uint32_t h, std::uint32_t i) {
        if ((used + 1) * 2 > slots.size()) {
            std::vector<Slot> old = std::move(slots);
            slots.assign(old.size() * 2, Slot{});
            used = 0;
            for (const Slot& s : old) {
                if (s.value != 0) {
                    put(s);
                }
            }
        }
        put(Slot{h, i + 1});
    }

    void put(Slot s) {
        std::size_t at = s.hash & mask();
        while (slots[at].value != 0) {
            at = (at + 1) & mask();
        }
        slots[at] = s;
        ++used;
    }


    void erase(std::uint32_t h, std::uint32_t i) {
        std::size_t at = h & mask();
        while (slots[at].value != i + 1) {
            if (slots[at].value == 0) {
                return;
            }
            at = (at + 1) & mask();
        }

        std::size_t hole = at;
        for (std::size_t next = (hole + 1) & mask(); slots[next].value != 0; next = (next + 1) & mask()) {
            std::size_t home = slots[next].hash & mask();

            bool movable = hole <= next ? (home <= hole || home > next) : (home <= hole && home > next);
            if (movable) {
                slots[hole] = slots[next];
                hole = next;
            }
        }
        slots[hole] = Slot{};
        --used;
    }
};


struct Shape {
    bool unordered;
    std::array<std::uint32_t, 28> words;
    bool operator==(const Shape& o) const { return unordered == o.unordered && same_words(words.data(), o.words.data(), 28); }
};
struct ShapeHash {
    std::size_t operator()(const Shape& s) const { return crc_words(s.unordered ? 1u : 2u, s.words.data(), s.words.size()); }
};


struct Views {
    std::vector<std::pair<View, std::uint32_t>> list;
    ViewIndex index;
    std::unordered_map<std::uintptr_t, std::vector<std::uint32_t>> by_resource;
    std::vector<std::uint32_t> free;
    std::vector<Shape> shapes;
    std::unordered_map<Shape, std::uint32_t, ShapeHash> shape_index;
};
danzi::FastLock VIEWS_LOCK;
Views VIEWS;

constexpr std::uint32_t VIEW_TAG = 0x45495676;   
constexpr std::uint32_t RTV_TAG = 0x56545276;    
constexpr std::uint32_t DSV_TAG = 0x56534476;    


constexpr std::uint32_t KIND_SHIFT = 30;
constexpr std::uint32_t GENERATIONS = (1u << KIND_SHIFT) - 1;
constexpr std::uint32_t CONSTANT = 1;
constexpr std::uint32_t PLACED = 2;
constexpr std::uint32_t CONSTANT_ADDRESS_BITS = 47;
constexpr std::uint32_t PLACED_ADDRESS_BITS = 40;
constexpr std::size_t SHAPES = std::size_t{1} << (62 - PLACED_ADDRESS_BITS);

bool inline_views() {
    static const bool on = danzi::env::not_set_or_not(L"VICTORY_VIEW_INLINE", L"0");
    return on;
}


std::atomic<std::uint64_t> NAMED_WHOLE[2]{};

struct Words {
    std::uint32_t i, generation;
};

Words kind_words(std::uint32_t kind, std::uint64_t payload) {
    return {static_cast<std::uint32_t>(payload), kind << KIND_SHIFT | static_cast<std::uint32_t>(payload >> 32)};
}

std::uint64_t payload(std::uint32_t i, std::uint32_t generation) { return std::uint64_t{generation & GENERATIONS} << 32 | i; }


bool constant_words(std::uint64_t address, std::uint32_t size, Words& out) {
    if (address >> CONSTANT_ADDRESS_BITS != 0) {
        return false;
    }
    std::uint64_t code;
    if (size < (1u << 14)) {
        code = size;
    } else if (size % 256 == 0 && (size >> 8) < (1u << 14)) {
        code = std::uint64_t{1} << 14 | (size >> 8);
    } else {
        return false;
    }
    out = kind_words(CONSTANT, code << CONSTANT_ADDRESS_BITS | address);
    return true;
}

View constant_of(std::uint32_t i, std::uint32_t generation) {
    std::uint64_t p = payload(i, generation);
    auto code = static_cast<std::uint32_t>(p >> CONSTANT_ADDRESS_BITS);
    std::uint32_t size = code >> 14 == 0 ? code : (code & 0x3fff) << 8;
    View v{View::Constant};
    v.a = p & ((std::uint64_t{1} << CONSTANT_ADDRESS_BITS) - 1);
    v.b = size;
    return v;
}


Shape shape_of(const View& v, std::uint64_t& address) {
    Shape s{v.flag, {}};

    std::memcpy(s.words.data(), v.words.data(), 14 * 4);
    std::memcpy(s.words.data() + 14, v.words.data() + 14, 10 * 4);
    std::memcpy(s.words.data() + 24, v.words.data() + 14 + 12, 4 * 4);
    address = std::uint64_t{v.words[14 + 10]} | std::uint64_t{v.words[14 + 11]} << 32;
    return s;
}


bool shape_number(const Shape& shape, std::uint32_t& out) {


    static std::uint32_t last = UINT32_MAX;
    if (last < VIEWS.shapes.size() && VIEWS.shapes[last] == shape) {
        out = last;
        return true;
    }
    if (auto it = VIEWS.shape_index.find(shape); it != VIEWS.shape_index.end()) {
        out = it->second;
        last = out;
        return true;
    }
    if (VIEWS.shapes.size() >= SHAPES) {
        return false;
    }
    out = static_cast<std::uint32_t>(VIEWS.shapes.size());
    VIEWS.shapes.push_back(shape);
    VIEWS.shape_index.emplace(shape, out);
    last = out;
    return true;
}


std::optional<View> placed_of(std::uint32_t i, std::uint32_t generation) {
    std::uint64_t p = payload(i, generation);
    std::uint64_t address = p & ((std::uint64_t{1} << PLACED_ADDRESS_BITS) - 1);
    std::uint64_t n = p >> PLACED_ADDRESS_BITS;
    if (n >= VIEWS.shapes.size()) {
        return std::nullopt;
    }
    const Shape& s = VIEWS.shapes[n];
    View v{View::Placed};
    v.flag = s.unordered;
    std::memcpy(v.words.data(), s.words.data(), 14 * 4);
    std::memcpy(v.words.data() + 14, s.words.data() + 14, 10 * 4);
    v.words[14 + 10] = static_cast<std::uint32_t>(address);
    v.words[14 + 11] = static_cast<std::uint32_t>(address >> 32);
    std::memcpy(v.words.data() + 14 + 12, s.words.data() + 24, 4 * 4);
    return v;
}


std::array<std::uintptr_t, 2> resources(const View& v) {
    if (v.kind == View::Shader) {
        return {static_cast<std::uintptr_t>(v.a), 0};
    }
    if (v.kind == View::Unordered) {
        return {static_cast<std::uintptr_t>(v.a), static_cast<std::uintptr_t>(v.b)};
    }
    return {0, 0};
}


Words entry(const View& view) {
    std::uint32_t i;
    std::uint32_t h = view_hash(view);
    if (auto found = VIEWS.index.find(view, h, VIEWS.list)) {
        i = *found;
    } else {
        if (!VIEWS.free.empty()) {
            i = VIEWS.free.back();
            VIEWS.free.pop_back();
            VIEWS.list[i].first = view;
        } else {
            VIEWS.list.emplace_back(view, 1);
            i = static_cast<std::uint32_t>(VIEWS.list.size() - 1);
        }
        for (std::uintptr_t r : resources(view)) {
            if (r != 0) {
                VIEWS.by_resource[r].push_back(i);
            }
        }
        VIEWS.index.insert(h, i);
    }
    return {i, VIEWS.list[i].second};
}


Words words_for(const View& view) {
    if (inline_views()) {
        if (view.kind == View::Constant) {
            Words w;
            if (constant_words(view.a, static_cast<std::uint32_t>(view.b), w)) {
                NAMED_WHOLE[0].fetch_add(1, std::memory_order_relaxed);
                return w;
            }
        } else if (view.kind == View::Placed) {
            std::uint64_t address;
            Shape shape = shape_of(view, address);
            if (address >> PLACED_ADDRESS_BITS == 0) {
                std::lock_guard lock(VIEWS_LOCK);
                std::uint32_t n;
                if (shape_number(shape, n)) {
                    NAMED_WHOLE[1].fetch_add(1, std::memory_order_relaxed);
                    return kind_words(PLACED, std::uint64_t{n} << PLACED_ADDRESS_BITS | address);
                }
            }
        }
    }
    std::lock_guard lock(VIEWS_LOCK);
    return entry(view);
}

void record(std::uint64_t handle, const View& view) {
    if (handle == 0) {
        return;
    }
    Words w = words_for(view);
    const std::uint32_t r[3] = {VIEW_TAG, w.i, w.generation};
    std::memcpy(reinterpret_cast<void*>(handle), r, sizeof r);
    mirror(handle, 1, 1);
}

std::optional<View> view_at(std::uint32_t i, std::uint32_t generation) {
    switch (generation >> KIND_SHIFT) {
        case 0: {
            std::lock_guard lock(VIEWS_LOCK);
            if (i >= VIEWS.list.size() || VIEWS.list[i].second != generation) {
                return std::nullopt;
            }
            return VIEWS.list[i].first;
        }
        case CONSTANT: return constant_of(i, generation);
        case PLACED: {
            std::lock_guard lock(VIEWS_LOCK);
            return placed_of(i, generation);
        }
        default: return std::nullopt;
    }
}

static_assert(sizeof(RtvRecord) == 40);

std::optional<RtvRecord> tagged_record(std::uint64_t handle, std::uint32_t tag) {
    if (handle == 0) {
        return std::nullopt;
    }
    RtvRecord r;
    std::memcpy(&r, reinterpret_cast<const void*>(handle), sizeof r);
    if (r.tag != tag) {
        return std::nullopt;
    }
    return r;
}


HRESULT __stdcall heap_query(This self, const Iid* iid, This* out) { return answer(DESCRIPTOR_HEAP, iid, is(iid, HEAP_IIDS) ? self : nullptr, out); }
std::uint32_t __stdcall heap_release(This self) { return release_boxed<DescHeap>(self); }
void __stdcall heap_desc(This self, DescriptorHeapDesc* out) {
    if (out) {
        *out = reinterpret_cast<DescHeap*>(self)->desc;
    }
}

std::uint64_t __stdcall heap_start(This self) { return reinterpret_cast<DescHeap*>(self)->start; }

const void* const* heap_functions() {

    static const Table<24> table = [] {
        auto f = d3d12_object(missing_table<DESCRIPTOR_HEAP, 24>());
        f[0] = fn(&heap_query);
        f[2] = fn(&heap_release);
        f[7] = fn(&parent_device);
        f[8] = fn(&heap_desc);
        f[9] = fn(&heap_start);
        f[10] = fn(&heap_start);
        return f;
    }();
    return table.data();
}
const bool MADE = (heap_functions(), true);

}   

std::optional<RtvRecord> rtv_record(std::uint64_t handle) { return tagged_record(handle, RTV_TAG); }
std::optional<RtvRecord> dsv_record(std::uint64_t handle) { return tagged_record(handle, DSV_TAG); }

std::optional<std::array<std::uint32_t, 6>> RtvRecord::host_desc(bool depth) const {
    std::array<std::uint32_t, 6> words{};
    if (described != 0) {
        std::memcpy(words.data(), desc, sizeof desc);
    } else {
        auto format = view_format(resource);
        if (!format) {
            return std::nullopt;
        }

        words = {*format, depth ? 3u : 4u, 0, 0, 0, 0};
    }
    words[0] = target_format(pc_format(words[0]));
    return words;
}

bool View::operator==(const View& o) const {
    return kind == o.kind && a == o.a && b == o.b && flag == o.flag && same_words(words.data(), o.words.data(), WORDS[kind]);
}

std::string view_debug(const View& v, bool hex) {
    auto number = [&](std::uint64_t n) {
        char b[24];
        std::snprintf(b, sizeof b, hex ? "%llx" : "%llu", static_cast<unsigned long long>(n));
        return std::string(b);
    };
    auto words = [&](usize from, usize n) {
        std::string s = "[";
        for (usize i = 0; i < n; ++i) {
            s += (i ? ", " : "") + number(v.words[from + i]);
        }
        return s + "]";
    };
    auto described = [&] { return v.flag ? "Some(" + words(0, 10) + ")" : std::string("None"); };
    switch (v.kind) {
        case View::Constant: return "Constant(" + number(v.a) + ", " + number(v.b) + ")";
        case View::Shader: return "Shader(" + number(v.a) + ", " + described() + ")";
        case View::Unordered: return "Unordered(" + number(v.a) + ", " + number(v.b) + ", " + described() + ")";
        case View::Sampler: return "Sampler(" + words(0, 13) + ")";
        case View::ConsoleSampler: return "ConsoleSampler(" + words(0, 27) + ")";
        case View::Placed: return std::string("Placed(") + (v.flag ? "true" : "false") + ", " + words(0, 14) + ", " + words(14, 16) + ")";
    }
    return {};
}

HRESULT __stdcall create_descriptor_heap(This, const DescriptorHeapDesc* desc, const Iid* iid, This* out) {
    if (!desc) {
        return E_INVALIDARG_;
    }
    std::uint32_t step = increment(desc->kind);
    if (step == 0) {
        return E_INVALIDARG_;
    }
    std::uint64_t size = std::uint64_t{step} * std::max(desc->count, 1u);


    struct Placement {
        std::uint64_t address, size;
    } placement;
    std::memcpy(&placement, reinterpret_cast<const std::uint8_t*>(desc) + sizeof *desc, sizeof placement);
    bool placed = placement.address != 0 && placement.size == size;
    auto* h = new DescHeap{Object{heap_functions()}, *desc, 0};
    if (placed) {
        h->start = placement.address;
    } else {
        if (!h->memory.make(size)) {
            delete h;
            return E_OUTOFMEMORY_;
        }
        h->start = reinterpret_cast<std::uint64_t>(h->memory.ptr);
    }
    if ((desc->flags & SHADER_VISIBLE) != 0 && (desc->kind == VIEWS_KIND || desc->kind == SAMPLERS)) {
        std::lock_guard lock(SHADER_HEAPS_LOCK);
        SHADER_HEAPS.push_back({h->start, h->start + size, h});
        publish_heaps();
    }
    return hand_out(DESCRIPTOR_HEAP, h, iid, HEAP_IIDS, out);
}

std::uint32_t __stdcall descriptor_increment_size(This, std::uint32_t kind) { return increment(kind); }

void __stdcall create_cbv(This, const std::uint32_t* desc, std::uint64_t handle) {
    if (desc) {
        View v{View::Constant};
        v.a = std::uint64_t{desc[0]} | std::uint64_t{desc[1]} << 32;
        v.b = desc[2];
        record(handle, v);
    }
}

void __stdcall create_srv(This, This resource, const std::uint32_t* desc, std::uint64_t handle) {
    View v{View::Shader};
    v.a = reinterpret_cast<std::uint64_t>(resource);
    if (desc) {
        v.flag = true;
        std::memcpy(v.words.data(), desc, 10 * 4);
    }
    record(handle, v);
}

void __stdcall create_uav(This, This resource, This counter, const std::uint32_t* desc, std::uint64_t handle) {
    View v{View::Unordered};
    v.a = reinterpret_cast<std::uint64_t>(resource);
    v.b = reinterpret_cast<std::uint64_t>(counter);
    if (desc) {
        v.flag = true;
        std::memcpy(v.words.data(), desc, 10 * 4);
    }
    record(handle, v);
}

void __stdcall create_rtv(This, This resource, const std::uint32_t* desc, std::uint64_t handle) {
    if (handle == 0) {
        return;
    }


    auto* at = reinterpret_cast<std::uint8_t*>(handle);
    const std::uint32_t head[2] = {RTV_TAG, desc ? 1u : 0u};
    std::memcpy(at, head, 8);
    std::memcpy(at + 8, &resource, 8);
    if (desc) {
        std::memcpy(at + 16, desc, 24);
    } else {
        std::memset(at + 16, 0, 24);
    }
}


void __stdcall create_dsv(This self, This resource, const std::uint32_t* desc, std::uint64_t handle) {
    create_rtv(self, resource, desc, handle);
    if (handle != 0) {
        std::memcpy(reinterpret_cast<void*>(handle), &DSV_TAG, 4);
    }
}


void __stdcall create_placed_srv(This, const std::uint32_t* resource, const std::uint32_t* desc, std::uint64_t handle) {
    if (resource && desc) {
        View v{View::Placed};
        std::memcpy(v.words.data(), resource, 14 * 4);
        std::memcpy(v.words.data() + 14, desc, 16 * 4);
        record(handle, v);
    }
}

void __stdcall create_placed_uav(This, const std::uint32_t* resource, const std::uint32_t* desc, std::uint64_t handle) {
    if (resource && desc) {
        View v{View::Placed};
        v.flag = true;
        std::memcpy(v.words.data(), resource, 14 * 4);
        std::memcpy(v.words.data() + 14, desc, 16 * 4);
        record(handle, v);
    }
}

void __stdcall create_sampler(This, const std::uint32_t* desc, std::uint64_t handle) {
    if (desc) {
        View v{View::Sampler};
        std::memcpy(v.words.data(), desc, 13 * 4);
        record(handle, v);
    }
}


void __stdcall create_sampler_x(This, const std::uint32_t* desc, std::uint64_t handle) {
    if (desc) {
        View v{View::ConsoleSampler};
        std::memcpy(v.words.data(), desc, 27 * 4);
        record(handle, v);
    }
}


void __stdcall copy_descriptors_simple(This, std::uint32_t count, std::uint64_t dst, std::uint64_t src, std::uint32_t kind) {
    std::uint64_t step = increment(kind);
    std::uint64_t len = std::uint64_t{count} * step;
    if (dst != 0 && src != 0) {
        std::memmove(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), static_cast<std::size_t>(len));
        mirror(dst, count, step);
    }
}


void __stdcall copy_descriptors(This, std::uint32_t dst_ranges, const std::uint64_t* dst_starts, const std::uint32_t* dst_sizes, std::uint32_t src_ranges,
                                const std::uint64_t* src_starts, const std::uint32_t* src_sizes, std::uint32_t kind) {
    std::uint32_t step = increment(kind);
    if (step == 0 || !dst_starts || !src_starts) {
        return;
    }
    std::uint32_t d = 0;
    std::uint64_t to = 0;
    std::uint32_t room = 0;
    if (dst_ranges > 0) {
        to = dst_starts[0];
        room = dst_sizes ? dst_sizes[0] : 1;
        d = 1;
    }
    for (std::uint32_t s = 0; s < src_ranges; ++s) {
        std::uint64_t from = src_starts[s];
        std::uint32_t left = src_sizes ? src_sizes[s] : 1;
        while (left > 0) {
            while (room == 0) {
                if (d >= dst_ranges) {
                    return;
                }
                to = dst_starts[d];
                room = dst_sizes ? dst_sizes[d] : 1;
                ++d;
            }
            std::uint32_t n = std::min(left, room);
            copy_descriptors_simple(nullptr, n, to, from, kind);
            std::uint64_t bytes = std::uint64_t{n} * step;
            to += bytes;
            from += bytes;
            room -= n;
            left -= n;
        }
    }
}

void forget_views(std::uintptr_t resource) {
    std::lock_guard lock(VIEWS_LOCK);
    auto it = VIEWS.by_resource.find(resource);
    if (it == VIEWS.by_resource.end()) {
        return;
    }
    std::vector<std::uint32_t> entries = std::move(it->second);
    VIEWS.by_resource.erase(it);
    for (std::uint32_t i : entries) {
        auto& [v, generation] = VIEWS.list[i];


        std::uint32_t h = view_hash(v);
        auto found = VIEWS.index.find(v, h, VIEWS.list);
        auto rs = resources(v);
        if (!found || *found != i || (rs[0] != resource && rs[1] != resource)) {
            continue;
        }
        VIEWS.index.erase(h, i);
        generation = std::max((generation + 1) & GENERATIONS, 1u);
        VIEWS.free.push_back(i);
    }
}

DescHeap::~DescHeap() {
    std::lock_guard lock(SHADER_HEAPS_LOCK);
    if (std::erase_if(SHADER_HEAPS, [&](const Visible& v) { return v.heap == this; }) > 0) {
        publish_heaps();
    }
}

std::optional<View> view_at_words(std::uint32_t i, std::uint32_t generation) { return view_at(i, generation); }

bool heap_samplers(const DescHeap& heap) { return heap.desc.kind == SAMPLERS; }
std::uint64_t heap_start(const DescHeap& heap) { return heap.start; }

std::optional<std::uint32_t> heap_index(const DescHeap& heap, std::uint64_t handle) {
    std::uint32_t i;
    return heap.index(handle, i) ? std::optional(i) : std::nullopt;
}

std::optional<std::uint64_t> heap_handle(const DescHeap& heap, std::uint32_t index) {
    std::uint64_t step = increment(heap.desc.kind);
    if (step == 0 || index >= heap.desc.count) {
        return std::nullopt;
    }
    return heap.start + std::uint64_t{index} * step;
}

HostHeap* heap_host(DescHeap& heap) {
    if (HostHeap* h = heap.host.load(std::memory_order_acquire)) {
        return h;
    }
    if (heap.host_tried.load(std::memory_order_acquire)) {
        return nullptr;
    }
    if ((heap.desc.flags & SHADER_VISIBLE) == 0 || (heap.desc.kind != VIEWS_KIND && heap.desc.kind != SAMPLERS)) {
        return nullptr;
    }
    std::lock_guard lock(heap.pending_lock);
    if (heap.host_tried.load(std::memory_order_relaxed)) {
        return heap.host.load(std::memory_order_relaxed);
    }
    std::uint64_t step = increment(heap.desc.kind);
    std::unique_ptr<HostHeap> h = step ? HostHeap::make(heap.desc.kind == SAMPLERS, heap.desc.count) : nullptr;
    if (h) {


        for (std::uint32_t i : heap.pending) {
            h->mirror(i, heap.start + std::uint64_t{i} * step, false);
        }
        heap.pending.clear();
        heap.pending.shrink_to_fit();
        heap.host_owned = std::move(h);
        heap.host.store(heap.host_owned.get(), std::memory_order_release);
    }
    heap.host_tried.store(true, std::memory_order_release);
    return heap.host.load(std::memory_order_relaxed);
}

std::uint64_t evict_kept_resources(const std::vector<std::uintptr_t>& victims) {
    if (victims.empty()) {
        return 0;
    }
    std::unordered_set<std::uintptr_t> set(victims.begin(), victims.end());
    std::lock_guard lock(SHADER_HEAPS_LOCK);
    std::uint64_t cleared = 0;
    for (const Visible& v : SHADER_HEAPS) {
        if (HostHeap* h = v.heap->host.load(std::memory_order_acquire)) {
            cleared += h->evict_kept(set);
        }
    }
    return cleared;
}

void release_dynamic(std::uintptr_t at, std::uint32_t first, std::uint32_t n) {
    std::lock_guard lock(SHADER_HEAPS_LOCK);
    for (const Visible& v : SHADER_HEAPS) {
        if (reinterpret_cast<std::uintptr_t>(v.heap) == at) {
            if (HostHeap* h = v.heap->host.load(std::memory_order_acquire)) {
                h->release(first, n);
            }
            return;
        }
    }
}

namespace {


std::mutex WAITING_LOCK;
std::map<std::uintptr_t, std::vector<std::uint64_t>> WAITING;
std::mutex MADE_LATE_LOCK;
std::vector<std::uintptr_t> MADE_LATE;


std::optional<std::pair<DescHeap*, std::uint32_t>> shader_slot(std::uint64_t handle) {
    std::lock_guard lock(SHADER_HEAPS_LOCK);
    for (const Visible& v : SHADER_HEAPS) {
        if (handle >= v.start && handle < v.end) {
            std::uint32_t i;
            if (!v.heap->index(handle, i)) {
                return std::nullopt;
            }
            return std::pair{v.heap, i};
        }
    }
    return std::nullopt;
}

}   

void texture_made(std::uintptr_t resource) {
    std::lock_guard lock(MADE_LATE_LOCK);
    MADE_LATE.push_back(resource);
}

void wait_for(std::uintptr_t resource, std::uint64_t handle) {
    constexpr std::size_t MOST = 64;
    bool many;
    {
        std::lock_guard lock(WAITING_LOCK);
        auto& slots = WAITING[resource];
        if (std::find(slots.begin(), slots.end(), handle) == slots.end()) {
            slots.push_back(handle);
        }
        many = slots.size() > MOST;
    }
    if (many) {

        (void)host_copy(reinterpret_cast<This>(resource));
    } else if (!host_waits(reinterpret_cast<This>(resource))) {
        texture_made(resource);
    }
}

void make_waiting() {
    std::vector<std::uintptr_t> made;
    {
        std::lock_guard lock(MADE_LATE_LOCK);
        made.swap(MADE_LATE);
    }
    for (std::uintptr_t resource : made) {
        std::vector<std::uint64_t> handles;
        {
            std::lock_guard lock(WAITING_LOCK);
            auto it = WAITING.find(resource);
            if (it == WAITING.end()) {
                continue;
            }
            handles = std::move(it->second);
            WAITING.erase(it);
        }
        for (std::uint64_t handle : handles) {
            auto slot = shader_slot(handle);
            if (!slot) {
                continue;
            }
            auto v = view(handle);
            bool same = v && (v->kind == View::Shader || v->kind == View::Unordered) && v->a == resource;
            if (same) {
                if (HostHeap* h = slot->first->host.load(std::memory_order_acquire)) {
                    h->mirror(slot->second, handle, true);
                }
            }
        }
    }
}

std::optional<View> view(std::uint64_t handle) {
    if (handle == 0) {
        return std::nullopt;
    }
    std::uint32_t r[3];
    std::memcpy(r, reinterpret_cast<const void*>(handle), sizeof r);
    if (r[0] != VIEW_TAG) {
        return std::nullopt;
    }
    return view_at(r[1], r[2]);
}

}   
