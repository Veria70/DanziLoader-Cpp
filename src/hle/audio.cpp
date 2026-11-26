// SPDX-License-Identifier: GPL-3.0-or-later
// The console's audio hardware, `AcpHal.dll` (`victory-hle/src/modules/
// audio.rs`; DanziLoader / victory-perf, see NOTICE.md):
//
// - the APU heap: host memory with 32-bit device addresses;
// - SHAPE context arrays allocated from it;
// - the audio coprocessor's command and message queues, where every command
//   completes at once and is acknowledged as the hardware would;
// - the XMA decoder: enabled XMA contexts are decoded on a thread of ours
//   into their output rings, like the hardware does.
//
// SHAPE's other units (rate conversion, EQ, filters, mixing through
// flowgraphs) are not emulated, so voices that need them stay silent. Mixed
// audio leaves through the host's WASAPI, which titles and middleware reach
// through COM like on PC. fmod's console build sets up the coprocessor
// inside `System_Create` and waits there for its first command to be
// acknowledged; the title uses only XMA contexts.
//
// Debug log lines are not ported (a release build logs info and above);
// neither is `dump_input` (`VICTORY_XMA_DUMP`, the reference's
// `diagnostics` feature only). The recorder of the `diagnostics` feature
// (`audio/capture.rs`) is in audio_capture.cpp.
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <vector>

#include "core/log.hpp"
#include "formats/xma.hpp"
#include "modules.hpp"

namespace danzi::hle::audio {

namespace {

namespace xma = formats::xma;

constexpr const char* T = "victory_hle::modules::audio";

constexpr HRESULT_ E_FAIL_ = static_cast<HRESULT_>(0x80004005u);
constexpr HRESULT_ E_OUTOFMEMORY_ = static_cast<HRESULT_>(0x8007000Eu);


void* alloc_zeroed(std::size_t size, std::size_t align, void*& raw) {
    HANDLE heap = GetProcessHeap();
    if (align <= 16 && align <= size) {
        raw = HeapAlloc(heap, HEAP_ZERO_MEMORY, size);
        return raw;
    }
    if (size > SIZE_MAX - align) {
        return nullptr;
    }
    raw = HeapAlloc(heap, HEAP_ZERO_MEMORY, size + align);
    if (!raw) {
        return nullptr;
    }
    auto at = reinterpret_cast<std::uintptr_t>(raw);
    return reinterpret_cast<void*>(at + (align - (at & (align - 1))));
}


struct Block {
    std::size_t size;
    std::uint32_t apu;

    std::size_t layout_size;
    void* raw;
};


constexpr std::uint32_t APU_BASE = 0x0001'0000;

struct Heap {

    std::map<std::uintptr_t, Block> blocks;

    std::map<std::uint32_t, std::uintptr_t> by_apu;

    std::map<std::uint32_t, std::uint32_t> free;

    bool created = false;


    std::optional<std::uint32_t> take(std::uint32_t size, std::uint32_t align) {
        if (free.empty() && !created) {
            free.emplace(APU_BASE, UINT32_MAX);
        }
        created = true;
        for (auto it = free.begin(); it != free.end(); ++it) {
            std::uint32_t start = it->first, end = it->second;
            if (align == 0) {
                continue;
            }
            std::uint64_t apu = start % align == 0 ? start : std::uint64_t{start} + (align - start % align);
            if (apu > UINT32_MAX || apu + size > UINT32_MAX || apu + size > end) {
                continue;
            }
            free.erase(it);
            auto a = static_cast<std::uint32_t>(apu);
            if (start < a) {
                free[start] = a;
            }
            if (a + size < end) {
                free[a + size] = end;
            }
            return a;
        }
        return std::nullopt;
    }

    void give_back(std::uint32_t start, std::uint32_t end) {
        auto it = free.lower_bound(start);
        if (it != free.begin()) {
            auto before = std::prev(it);
            if (before->second == start) {
                start = before->first;
                free.erase(before);
            }
        }
        if (auto after = free.find(end); after != free.end()) {
            end = after->second;
            free.erase(after);
        }
        free[start] = end;
    }
};


using Arrays = std::array<std::uintptr_t, 6>;


struct Voice {
    std::optional<xma::Decoder> decoder;

    std::vector<std::int16_t> pcm;
    std::size_t head = 0;

    std::size_t skip = 0;
    bool ended = false;

    std::size_t pcm_len() const { return pcm.size() - head; }
    std::int16_t pop_front() {
        if (head == pcm.size()) {
            return 0;
        }
        return pcm[head++];
    }
    void extend(const std::int16_t* from, std::size_t n) {
        if (head == pcm.size()) {
            pcm.clear();
            head = 0;
        } else if (head >= 4096) {
            pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(head));
            head = 0;
        }
        pcm.insert(pcm.end(), from, from + n);
    }
};


struct Slot {
    std::mutex lock;
    std::optional<Voice> voice;
};


struct Context {
    std::uint32_t* words;


    std::uint32_t get(std::size_t word) const { return *static_cast<volatile std::uint32_t*>(words + word); }
    void set(std::size_t word, std::uint32_t value) const { *static_cast<volatile std::uint32_t*>(words + word) = value; }
    std::uint32_t field(std::size_t word, std::uint32_t shift, std::uint32_t bits) const { return (get(word) >> shift) & ((1u << bits) - 1); }
    void set_field(std::size_t word, std::uint32_t shift, std::uint32_t bits, std::uint32_t value) const {
        std::uint32_t mask = ((1u << bits) - 1) << shift;
        set(word, (get(word) & ~mask) | ((value << shift) & mask));
    }
    std::size_t channels() const { return 1 + static_cast<std::size_t>(field(1, 29, 1)); }
};

struct Xma {

    std::uintptr_t array = 0;
    std::unique_ptr<Slot[]> voices;
    std::size_t count = 0;

    std::optional<Context> context(std::uint32_t index) const {
        if (index < count && array != 0) {
            return Context{reinterpret_cast<std::uint32_t*>(array + std::size_t{index} * 64)};
        }
        return std::nullopt;
    }
    void enable(std::uint32_t index) const;
    void disable(std::uint32_t index) const;
    void advance_read(std::uint32_t index, std::uint32_t blocks) const;
    void pump() const;
};


struct Globals {
    std::mutex heap_lock;
    Heap heap;
    std::mutex contexts_lock;
    std::optional<Arrays> contexts;

    std::shared_mutex xma_lock;
    Xma xma;
};
Globals& G() {
    static Globals* g = new Globals;
    return *g;
}


std::optional<std::pair<std::uint8_t*, std::uint32_t>> heap_alloc(std::size_t size, std::size_t align) {
    std::size_t layout_size = std::max<std::size_t>(size, 1);
    std::size_t layout_align = std::max<std::size_t>(align, 16);
    if ((layout_align & (layout_align - 1)) != 0 || layout_size > static_cast<std::size_t>(PTRDIFF_MAX) - (layout_align - 1)) {
        return std::nullopt;
    }
    Globals& g = G();
    std::unique_lock heap(g.heap_lock);
    if (layout_size > UINT32_MAX) {
        return std::nullopt;
    }
    auto apu = g.heap.take(static_cast<std::uint32_t>(layout_size), static_cast<std::uint32_t>(align));
    if (!apu) {
        return std::nullopt;
    }
    void* raw = nullptr;
    auto* ptr = static_cast<std::uint8_t*>(alloc_zeroed(layout_size, layout_align, raw));
    if (!ptr) {
        g.heap.give_back(*apu, *apu + static_cast<std::uint32_t>(layout_size));
        return std::nullopt;
    }
    auto host = reinterpret_cast<std::uintptr_t>(ptr);
    g.heap.blocks[host] = Block{size, *apu, layout_size, raw};
    g.heap.by_apu[*apu] = host;


    if (size >= std::size_t{32} << 20) {
        log::info(T, "audio memory for the title from our allocator", {{"mb", log::debug(static_cast<std::uint64_t>(size >> 20))}, {"at", log::hex(host)}});
    }
    return std::pair{ptr, *apu};
}

void heap_free(void* ptr) {
    Globals& g = G();
    std::unique_lock heap(g.heap_lock);
    auto it = g.heap.blocks.find(reinterpret_cast<std::uintptr_t>(ptr));
    if (it == g.heap.blocks.end()) {
        return;
    }
    Block block = it->second;
    g.heap.blocks.erase(it);
    g.heap.by_apu.erase(block.apu);
    g.heap.give_back(block.apu, block.apu + static_cast<std::uint32_t>(block.layout_size));
    heap.unlock();
    HeapFree(GetProcessHeap(), 0, block.raw);
}


std::uint32_t heap_map(std::uintptr_t ptr) {
    Globals& g = G();
    std::lock_guard heap(g.heap_lock);
    auto it = g.heap.blocks.upper_bound(ptr);
    if (it == g.heap.blocks.begin()) {
        return 0;
    }
    --it;
    if (ptr < it->first + std::max<std::size_t>(it->second.size, 1)) {
        return it->second.apu + static_cast<std::uint32_t>(ptr - it->first);
    }
    return 0;
}


std::uint8_t* heap_unmap(std::uint32_t apu) {
    Globals& g = G();
    std::lock_guard heap(g.heap_lock);
    auto it = g.heap.by_apu.upper_bound(apu);
    if (it == g.heap.by_apu.begin()) {
        return nullptr;
    }
    --it;
    std::uint32_t start = it->first;
    std::uintptr_t host = it->second;
    const Block& block = g.heap.blocks.at(host);
    std::uint32_t size = static_cast<std::uint32_t>(std::max<std::size_t>(block.size, 1));
    std::uint32_t limit = start > UINT32_MAX - size ? UINT32_MAX : start + size;
    if (apu < limit) {
        return reinterpret_cast<std::uint8_t*>(host + (apu - start));
    }
    return nullptr;
}


std::optional<std::span<std::uint8_t>> heap_bytes(std::uint32_t apu, std::size_t len) {
    Globals& g = G();
    std::lock_guard heap(g.heap_lock);
    auto it = g.heap.by_apu.upper_bound(apu);
    if (it == g.heap.by_apu.begin()) {
        return std::nullopt;
    }
    --it;
    std::size_t offset = apu - it->first;
    std::uintptr_t host = it->second;
    if (offset + len <= g.heap.blocks.at(host).size) {
        return std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(host + offset), len);
    }
    return std::nullopt;
}

}   


HRESULT_ ApuCreateHeap(std::uint32_t cached, std::uint32_t) {
    constexpr HRESULT_ ALREADY_ALLOCATED = static_cast<HRESULT_>(0x8ACA0001u);
    if (cached == 0) {
        return E_INVALIDARG_;
    }
    Globals& g = G();
    std::lock_guard heap(g.heap_lock);
    if (g.heap.created) {
        return ALREADY_ALLOCATED;
    }
    g.heap.created = true;
    g.heap.free[APU_BASE] = UINT32_MAX;
    return S_OK_;
}


void* ApuMapApuAddress(std::uint32_t apu) { return heap_unmap(apu); }

HRESULT_ ApuAlloc(void** out, std::uint32_t* apu, std::uint32_t size, std::uint32_t align, std::uint32_t) {
    if (!out || size == 0 || align < 4 || (align & (align - 1)) != 0) {
        return E_INVALIDARG_;
    }
    auto block = heap_alloc(size, align);
    if (!block) {
        return E_OUTOFMEMORY_;
    }
    *out = block->first;
    if (apu) {
        *apu = block->second;
    }
    return S_OK_;
}

void ApuFree(void* ptr) { heap_free(ptr); }

std::uint32_t ApuMapVirtualAddress(const void* ptr) { return heap_map(reinterpret_cast<std::uintptr_t>(ptr)); }

bool ApuIsVirtualAddressValid(const void* ptr, std::uint32_t align) {
    std::uint32_t apu = heap_map(reinterpret_cast<std::uintptr_t>(ptr));
    return apu != 0 && (align == 0 || apu % align == 0);
}


#pragma pack(push, 1)
struct ShapeContexts {
    std::uint32_t counts[6];
    void* arrays[6];
    std::uint32_t apu[6];
};
#pragma pack(pop)
static_assert(sizeof(ShapeContexts) == 0x60);

namespace {


constexpr std::size_t CONTEXT_BYTES[6] = {32, 256, 256, 16, 64, 256};
}   

HRESULT_ AcpHalAllocateShapeContexts(ShapeContexts* contexts) {
    if (!contexts) {
        return E_INVALIDARG_;
    }
    Globals& g = G();
    std::lock_guard allocated(g.contexts_lock);
    if (g.contexts) {
        return E_FAIL_;
    }

    std::uint32_t counts[6];
    std::memcpy(counts, contexts->counts, sizeof counts);
    void* arrays[6] = {};
    std::uint32_t addresses[6] = {};
    for (std::size_t i = 0; i < 6; ++i) {
        if (counts[i] == 0) {
            continue;
        }
        auto block = heap_alloc(std::size_t{counts[i]} * CONTEXT_BYTES[i], 256);
        if (!block) {
            for (void* p : arrays) {
                heap_free(p);
            }
            return E_OUTOFMEMORY_;
        }
        arrays[i] = block->first;
        addresses[i] = block->second;
    }
    std::memcpy(contexts->arrays, arrays, sizeof arrays);
    std::memcpy(contexts->apu, addresses, sizeof addresses);
    Arrays kept{};
    for (std::size_t i = 0; i < 6; ++i) {
        kept[i] = reinterpret_cast<std::uintptr_t>(arrays[i]);
    }
    g.contexts = kept;
    std::unique_lock xma(g.xma_lock);
    g.xma.array = kept[4];
    g.xma.voices = std::make_unique<Slot[]>(counts[4]);
    g.xma.count = counts[4];
    return S_OK_;
}

HRESULT_ AcpHalReleaseShapeContexts() {
    Globals& g = G();
    std::lock_guard allocated(g.contexts_lock);
    if (!g.contexts) {
        return E_FAIL_;
    }
    Arrays arrays = *g.contexts;
    g.contexts.reset();
    {
        std::unique_lock xma(g.xma_lock);
        g.xma.array = 0;
        g.xma.voices.reset();
        g.xma.count = 0;
    }
    for (std::uintptr_t p : arrays) {
        heap_free(reinterpret_cast<void*>(p));
    }
    return S_OK_;
}


namespace {

constexpr std::uint32_t ENABLE_XMA = 5;
constexpr std::uint32_t ENABLE_XMAS = 6;
constexpr std::uint32_t DISABLE_XMA = 7;
constexpr std::uint32_t DISABLE_XMAS = 8;
constexpr std::uint32_t ADVANCE_XMA_READ = 18;

constexpr std::size_t BLOCK_BYTES = 256;
constexpr std::size_t SAMPLES_PER_BLOCK = BLOCK_BYTES / 2;

constexpr std::uint32_t INPUT_INVALID = 4;

void wake_decoder();

void Xma::enable(std::uint32_t index) const {
    auto context = this->context(index);
    if (!context) {
        return;
    }
    {
        std::lock_guard slot(voices[index].lock);
        context->set_field(2, 26, 6, 0);
        Voice v;
        v.skip = static_cast<std::size_t>(context->field(1, 24, 3)) * xma::SUBFRAME_SAMPLES;
        voices[index].voice = std::move(v);
    }
    wake_decoder();
}

void Xma::disable(std::uint32_t index) const {
    if (!context(index)) {
        return;
    }
    std::lock_guard slot(voices[index].lock);
    voices[index].voice.reset();
}


void Xma::advance_read(std::uint32_t index, std::uint32_t blocks) const {
    auto context = this->context(index);
    if (!context) {
        return;
    }
    {

        std::lock_guard slot(voices[index].lock);
        std::uint32_t size = std::max(context->field(0, 22, 5), 1u);

        std::uint32_t read = (context->field(9, 0, 5) + blocks % size) % size;
        context->set_field(9, 0, 5, read);
        if (blocks > 0) {
            context->set_field(1, 31, 1, 1);
        }
    }
    wake_decoder();
}

bool decode(Context context, Voice& voice);


void pump(Context context, Voice& voice) {
    std::uint32_t size = context.field(0, 22, 5);
    if (size == 0) {
        return;
    }
    while (context.field(1, 31, 1) == 1) {
        if (voice.pcm_len() < SAMPLES_PER_BLOCK && !decode(context, voice)) {
            return;
        }
        std::uint32_t write = context.field(0, 27, 5);
        std::size_t address = std::size_t{context.get(7)} + std::size_t{write} * BLOCK_BYTES;
        auto out = heap_bytes(static_cast<std::uint32_t>(address), BLOCK_BYTES);
        if (!out) {
            return;
        }
        for (std::size_t i = 0; i + 1 < out->size(); i += 2) {
            auto sample = static_cast<std::uint16_t>(voice.pop_front());
            (*out)[i] = static_cast<std::uint8_t>(sample);
            (*out)[i + 1] = static_cast<std::uint8_t>(sample >> 8);
        }
        write = (write + 1) % size;
        context.set_field(0, 27, 5, write);
        if (write == context.field(9, 0, 5)) {

            context.set_field(1, 31, 1, 0);
        }
    }
}


void Xma::pump() const {
    for (std::uint32_t index = 0; index < count; ++index) {
        auto context = this->context(index);
        std::lock_guard slot(voices[index].lock);
        if (context && voices[index].voice) {
            audio::pump(*context, *voices[index].voice);
        }
    }
}


bool decode(Context context, Voice& voice) {
    if (voice.ended) {
        return false;
    }
    std::size_t channels = context.channels();
    std::uint32_t current = context.field(4, 31, 1);
    std::uint32_t valid = context.field(0, 20, 2);
    if ((valid & (1u << current)) == 0) {
        if ((valid & (1u << (1 - current))) == 0) {

            if (voice.decoder) {
                std::vector<std::int16_t> tail;
                voice.decoder->flush(tail);
                voice.extend(tail.data(), tail.size());
            }
            voice.ended = true;
            context.set_field(2, 26, 5, INPUT_INVALID);
            context.set_field(2, 31, 1, 1);
            return voice.pcm_len() != 0;
        }
        current = 1 - current;
        context.set_field(4, 31, 1, current);
        context.set_field(2, 0, 26, 0);
    }
    std::uint32_t address = current == 0 ? context.get(5) : context.get(6);
    std::uint32_t packets = current == 0 ? context.field(0, 0, 12) : context.field(1, 0, 12);
    auto bytes = heap_bytes(address & ~0x7ffu, std::size_t{packets} * xma::PACKET_BYTES);
    if (!bytes) {
        context.set_field(0, 20 + current, 1, 0);
        return true;
    }
    std::span<const std::uint8_t> data(bytes->data(), bytes->size());
    std::size_t at = context.field(2, 0, 26);
    if (at < 32) {
        auto p = xma::packet(data, 0);
        at = 32 + (p ? p->first_frame_bit : 0);
    }
    auto frame = xma::read_frame(data, at);
    if (!frame) {

        context.set_field(0, 20 + current, 1, 0);
        return true;
    }
    if (!voice.decoder) {
        constexpr std::uint32_t RATES[4] = {24'000, 32'000, 44'100, 48'000};
        voice.decoder.emplace(channels, RATES[context.field(1, 27, 2)]);
    }
    std::vector<std::int16_t> pcm;
    pcm.reserve(xma::FRAME_SAMPLES * channels);
    voice.decoder->decode(frame->data, frame->bits, pcm);
    std::size_t next = frame->next.value_or(0);
    std::uint32_t loops = context.field(0, 12, 8);
    std::size_t loop_end = context.field(4, 0, 26);
    bool ends_loop = loop_end != 0 && (frame->start == loop_end || (frame->start < loop_end && (!frame->next || loop_end < *frame->next)));
    if (loops > 0 && ends_loop) {
        std::size_t keep = (static_cast<std::size_t>(context.field(1, 12, 2)) + 1) * xma::SUBFRAME_SAMPLES * channels;
        if (pcm.size() > keep) {
            pcm.resize(keep);
        }
        next = context.field(3, 0, 26);
        voice.skip = static_cast<std::size_t>(context.field(1, 17, 3)) * xma::SUBFRAME_SAMPLES;
        if (loops != 255) {
            context.set_field(0, 12, 8, loops - 1);
        }
    }
    std::size_t skip = std::min(voice.skip * channels, pcm.size());
    voice.skip -= skip / channels;
    voice.extend(pcm.data() + skip, pcm.size() - skip);
    if (next == 0) {
        context.set_field(0, 20 + current, 1, 0);
    }
    context.set_field(2, 0, 26, static_cast<std::uint32_t>(next));
    return true;
}


void wake_decoder() {
    static const HANDLE WAKE = [] {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE thread = CreateThread(
            nullptr, 0,
            [](void* wake) -> DWORD {
                SetThreadDescription(GetCurrentThread(), L"victory XMA decoder");
                for (;;) {
                    {
                        Globals& g = G();
                        std::shared_lock xma(g.xma_lock);
                        g.xma.pump();
                    }

                    WaitForSingleObject(static_cast<HANDLE>(wake), 2);
                }
            },
            event, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
        return event;
    }();
    SetEvent(WAKE);
}


struct Words {
    const void* data;
    std::optional<std::uint32_t> operator()(std::size_t at) const {
        if (!data) {
            return std::nullopt;
        }
        std::uint32_t v;
        std::memcpy(&v, static_cast<const std::uint8_t*>(data) + at * 4, 4);
        return v;
    }
};


bool xma_command(std::uint32_t command, const Words& word) {
    Globals& g = G();
    std::shared_lock lock(g.xma_lock);
    const Xma& xma = g.xma;
    switch (command) {
        case ENABLE_XMA: xma.enable(word(0).value_or(UINT32_MAX)); break;
        case DISABLE_XMA: xma.disable(word(0).value_or(UINT32_MAX)); break;
        case ENABLE_XMAS:
        case DISABLE_XMAS:
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(xma.count); ++i) {
                if ((word(i / 32).value_or(0) & (1u << (i % 32))) != 0) {
                    if (command == ENABLE_XMAS) {
                        xma.enable(i);
                    } else {
                        xma.disable(i);
                    }
                }
            }
            break;
        case ADVANCE_XMA_READ: xma.advance_read(word(0).value_or(UINT32_MAX), word(1).value_or(0)); break;
        default: return false;
    }
    return true;
}


constexpr std::uint32_t LOAD_FLOWGRAPH = 1;
constexpr std::uint32_t REGISTER_MESSAGES = 2;
constexpr std::uint32_t UNREGISTER_MESSAGES = 3;

constexpr std::uint32_t FLOWGRAPH_COMPLETED = 0x02;
constexpr std::uint32_t COMMAND_COMPLETED = 0x10;
constexpr std::uint32_t DISCONNECTED = 0x80;


constexpr std::uint64_t FRAMES_PER_SECOND = 375;

#pragma pack(push, 1)
struct Message {
    std::uint32_t kind;
    std::uint32_t dropped;
    std::uint32_t usec;
    std::uint8_t data[16];
};
#pragma pack(pop)
static_assert(sizeof(Message) == 28);

struct State {
    bool connected = false;

    std::uint32_t mask = 0;
    std::deque<Message> messages;

    std::size_t capacity = 0;

    std::uint32_t dropped = 0;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();

    std::uint64_t usec() const {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
    }
    std::uint32_t frame() const { return static_cast<std::uint32_t>(usec() * FRAMES_PER_SECOND / 1'000'000); }


    void post(std::uint32_t kind, const void* data, std::size_t len) {
        if ((mask & kind) == 0) {
            return;
        }
        if (messages.size() >= capacity) {
            dropped = dropped == UINT32_MAX ? dropped : dropped + 1;
            return;
        }
        Message message{kind, dropped, static_cast<std::uint32_t>(usec()), {}};
        dropped = 0;
        if (len > 0) {
            std::memcpy(message.data, data, len);
        }
        messages.push_back(message);
    }
};


struct Acp {
    const void* const* vtable;
    std::mutex lock;
    State state;
};

HRESULT_ __stdcall connect(Acp* self, std::uint32_t commands, std::uint32_t messages) {
    if (commands == 0) {
        return E_INVALIDARG_;
    }
    std::lock_guard l(self->lock);
    self->state.connected = true;
    self->state.capacity = messages;
    return S_OK_;
}

HRESULT_ __stdcall disconnect(Acp* self) {
    std::lock_guard l(self->lock);
    if (!self->state.connected) {
        return E_FAIL_;
    }
    self->state.post(DISCONNECTED, nullptr, 0);
    self->state.connected = false;
    return S_OK_;
}


HRESULT_ __stdcall submit(Acp* self, std::uint32_t command, std::uint64_t id, std::uint32_t, const void* data, std::uint32_t notification) {

    static std::mutex seen_lock;
    static std::uint64_t seen = 0;
    if (command < 64) {
        std::unique_lock s(seen_lock);
        if ((seen & std::uint64_t{1} << command) == 0) {
            seen |= std::uint64_t{1} << command;
            s.unlock();
            log::info(T, "first coprocessor command of this kind", {{"command", log::debug(command)}});
        }
    }
    std::lock_guard l(self->lock);
    State& state = self->state;

    Words word{data};
    switch (command) {
        case REGISTER_MESSAGES: state.mask |= word(0).value_or(0); break;
        case UNREGISTER_MESSAGES: state.mask &= ~word(0).value_or(0); break;
        case LOAD_FLOWGRAPH: {
            std::uint32_t flowgraph = word(1).value_or(0);
            state.post(FLOWGRAPH_COMPLETED, &flowgraph, 4);
            break;
        }
        default:


            xma_command(command, word);
            break;
    }
    if (id != 0) {
        std::uint8_t done[16];
        std::uint32_t frame = state.frame();
        std::memcpy(done, &command, 4);
        std::memcpy(done + 4, &id, 8);
        std::memcpy(done + 12, &frame, 4);
        state.post(COMMAND_COMPLETED, done, 16);
    }
    if (notification != 0) {
        if (std::uint8_t* word32 = heap_unmap(notification)) {

            *reinterpret_cast<volatile std::uint32_t*>(word32) = 1;
        }
    }
    return S_OK_;
}

bool __stdcall pop_message(Acp* self, Message* out) {
    Message message;
    {
        std::lock_guard l(self->lock);
        if (self->state.messages.empty()) {
            return false;
        }
        message = self->state.messages.front();
        self->state.messages.pop_front();
    }
    if (out) {

        std::memcpy(out, &message, sizeof message);
    }
    return true;
}

std::uint32_t __stdcall message_count(Acp* self) {
    std::lock_guard l(self->lock);
    return static_cast<std::uint32_t>(self->state.messages.size());
}


void __stdcall release(Acp* self) { delete self; }

const void* const ACP_METHODS[] = {
    reinterpret_cast<const void*>(&connect),       reinterpret_cast<const void*>(&disconnect),
    reinterpret_cast<const void*>(&submit),        reinterpret_cast<const void*>(&pop_message),
    reinterpret_cast<const void*>(&message_count), reinterpret_cast<const void*>(&release),
};

}   

HRESULT_ AcpHalCreate(Acp** out) {
    if (!out) {
        return E_INVALIDARG_;
    }
    auto* acp = new Acp{ACP_METHODS, {}, {}};
    *out = acp;
    return S_OK_;
}

#define E(f) HleExport{#f, reinterpret_cast<std::uintptr_t>(&f)}
std::vector<HleExport> exports() {
    return {E(AcpHalAllocateShapeContexts), E(AcpHalReleaseShapeContexts), E(AcpHalCreate), E(ApuAlloc), E(ApuCreateHeap), E(ApuFree),
            E(ApuIsVirtualAddressValid),    E(ApuMapApuAddress),           E(ApuMapVirtualAddress)};
}
#undef E

}   
