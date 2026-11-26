// SPDX-License-Identifier: GPL-3.0-or-later
// `xmem.dll`: console memory, served from host memory
// (`victory-hle/src/modules/xmem.rs`; DanziLoader / victory-perf, see
// NOTICE.md). The GPU side works from CPU addresses (unified memory), so
// memory types and bandwidth classes change nothing here; what constrains
// addresses (alignment, 2 MiB pages, address-bit limits) is honoured.
// Log lines: milestone 7.
#include "modules.hpp"

#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

#include "core/inplace.hpp"
#include "core/watch.hpp"
#include "d3d12x/device.hpp"
#include "d3d12x/host_heap.hpp"
#include "core/log.hpp"
#include "loader/crash.hpp"
#include "modules.hpp"
#include "runtime.hpp"

#pragma comment(lib, "onecore.lib")

namespace danzi::hle {

void title_memory_remapped(std::uintptr_t at, std::size_t len) {


    d3d12x::remapped(at, len);
    watch::title_memory_remapped(at, len);
}

namespace xmem {

namespace {

constexpr std::size_t MIB = 1 << 20;


std::atomic<std::size_t> TRANSFERRED{0};


constexpr std::uint32_t PAGES_2M = 0x20000000;

constexpr std::uint32_t PAGES_64K_BIT = 0x400000;
constexpr std::uint32_t PAGES_64K = PAGES_2M | PAGES_64K_BIT;


constexpr std::uint32_t HOST_PROTECTION = 0x1ff;

constexpr std::uint64_t GRAPHICS = 3;

constexpr std::uint64_t ADDRESS_32BIT = 4;
constexpr std::uint32_t MEM_WRITE_WATCH_ = 0x200000;


std::pair<std::uintptr_t, std::uintptr_t> address_range(std::uint64_t flags) {
    if (flags & ADDRESS_32BIT) {
        return {0, 0xFFFFFFFFu};
    }
    std::uint64_t field = flags >> 12 & 7;
    unsigned bits = field == 0 ? 40 : static_cast<unsigned>(47 - field);
    return {std::uintptr_t{1} << 32, (std::uintptr_t{1} << bits) - 1};
}

}   


bool on_wine() {
    static const bool wine = [] {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version");
    }();
    return wine;
}

void* WINAPI XMemVirtualAlloc(void* base, std::size_t size, std::uint32_t kind, std::uint64_t flags, std::uint32_t protect) {
    auto [lowest, highest] = address_range(flags);
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.LowestStartingAddress = reinterpret_cast<void*>(lowest);
    requirements.HighestEndingAddress = reinterpret_cast<void*>(highest);
    requirements.Alignment = (kind & (PAGES_2M | PAGES_64K_BIT)) == PAGES_2M ? 2 * MIB : 0;
    std::uint32_t prot = protect & HOST_PROTECTION;
    if (prot == 0) {
        prot = PAGE_NOACCESS;
    }


    bool placeholder = (kind & PAGES_64K) == PAGES_64K && (kind & MEM_COMMIT) == 0;
    if (placeholder) {
        if (kind & MEM_REPLACE_PLACEHOLDER) {

            return base;
        }
        prot = PAGE_NOACCESS;
    }
    kind &= ~(PAGES_2M | PAGES_64K_BIT);
    if (placeholder) {
        kind |= MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
    }
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;


    bool constrained = base == nullptr;
    if (constrained) {
        kind |= MEM_RESERVE;


        if (!(kind & MEM_RESERVE_PLACEHOLDER)) {
            kind |= MEM_WRITE_WATCH_;
        }
    }
    return VirtualAlloc2(GetCurrentProcess(), base, size, kind, prot, constrained ? &parameter : nullptr, constrained ? 1 : 0);
}


struct BasicInformation {
    MEMORY_BASIC_INFORMATION host;
    std::uint64_t flags;
    std::uint32_t tag_code;
    std::uint8_t tag[4];
};


BOOL WINAPI XMemVirtualQuery(HANDLE process, const void* address, BasicInformation* out) {
    if (VirtualQueryEx(process, address, reinterpret_cast<MEMORY_BASIC_INFORMATION*>(out), sizeof(MEMORY_BASIC_INFORMATION)) == 0) {
        return 0;
    }
    out->flags = out->host.State == MEM_COMMIT ? GRAPHICS : 0;
    out->tag_code = 0;
    std::memset(out->tag, 0, 4);
    return 1;
}

namespace {


constexpr unsigned ALIGNMENT_SHIFT = 24;
constexpr std::uint64_t ALLOC_32BIT = std::uint64_t{1} << 33;


constexpr std::size_t HEADER = 2 * sizeof(std::size_t);

using AllocRoutine = void*(WINAPI*)(std::size_t, std::uint64_t);
using FreeRoutine = void(WINAPI*)(void*, std::uint64_t);


std::array<std::uintptr_t, 2> hooks() {
    static std::mutex lock;
    static std::optional<std::uintptr_t> at;
    std::uintptr_t a;
    {
        std::lock_guard l(lock);
        if (!at) {
            auto base = runtime::main_image();
            if (!base) {
                return {0, 0};
            }
            at = reinterpret_cast<std::uintptr_t>(GetProcAddress(reinterpret_cast<HMODULE>(*base), "__XMemAllocationHooks__"));
            log::info("victory_hle::modules::xmem", "xmem allocation hooks", {{"found", log::debug(*at != 0)}});
        }
        a = *at;
    }
    if (a == 0) {
        return {0, 0};
    }
    std::array<std::uintptr_t, 2> h;
    std::memcpy(h.data(), reinterpret_cast<const void*>(a), sizeof h);
    return h;
}

}   

void* WINAPI XMemAllocDefault(std::size_t size, std::uint64_t attributes);
void WINAPI XMemFreeDefault(void* address, std::uint64_t attributes);


void* WINAPI XMemAlloc(std::size_t size, std::uint64_t attributes) {
    if (auto hook = hooks()[0]) {
        return reinterpret_cast<AllocRoutine>(hook)(size, attributes);
    }
    return XMemAllocDefault(size, attributes);
}

void WINAPI XMemFree(void* address, std::uint64_t attributes) {
    if (auto hook = hooks()[1]) {
        reinterpret_cast<FreeRoutine>(hook)(address, attributes);
        return;
    }
    XMemFreeDefault(address, attributes);
}


void* WINAPI XMemAllocDefault(std::size_t size, std::uint64_t attributes) {
    if (attributes & ALLOC_32BIT) {
        return XMemVirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, GRAPHICS | ADDRESS_32BIT, PAGE_READWRITE);
    }

    unsigned log2 = std::max<unsigned>(static_cast<unsigned>(attributes >> ALIGNMENT_SHIFT & 0x1f), 4);
    std::size_t align = std::size_t{1} << log2;

    if (size > SIZE_MAX - align || size + align > static_cast<std::size_t>(PTRDIFF_MAX) - (align - 1)) {
        return nullptr;
    }
    std::size_t total = size + align;
    auto* block = static_cast<std::uint8_t*>(_aligned_malloc(total, align));
    if (!block) {
        return nullptr;
    }
    std::uint8_t* user = block + align;
    std::size_t header[2] = {total, align};
    std::memcpy(user - HEADER, header, sizeof header);
    return user;
}


void WINAPI XMemFreeDefault(void* address, std::uint64_t attributes) {
    if (!address) {
        return;
    }
    if (attributes & ALLOC_32BIT) {
        VirtualFree(address, 0, MEM_RELEASE);
        return;
    }
    auto* user = static_cast<std::uint8_t*>(address);
    std::size_t header[2];
    std::memcpy(header, user - HEADER, sizeof header);
    _aligned_free(user - header[1]);
}


BOOL WINAPI XMemIsToolsMemAvailable() { return 0; }


namespace {

constexpr std::size_t PAGE = 64 << 10;


constexpr std::size_t FRAMES = PAGE >> 12;

struct Section {
    HANDLE handle;
    explicit Section(HANDLE h) : handle(h) {}
    Section(const Section&) = delete;

    ~Section() { CloseHandle(handle); }
};


struct Slice {
    std::shared_ptr<Section> section;
    std::size_t index = 0;

    std::pair<std::uintptr_t, std::size_t> key(std::size_t k) const { return {reinterpret_cast<std::uintptr_t>(section.get()), index + k}; }
};

struct PairHash {
    std::size_t operator()(const std::pair<std::uintptr_t, std::size_t>& p) const { return std::hash<std::uintptr_t>()(p.first) * 31 + std::hash<std::size_t>()(p.second); }
};

struct Physical {

    std::size_t next_page = 0x100000;
    std::unordered_map<std::size_t, Slice> pages;

    std::map<std::uintptr_t, std::pair<std::size_t, Slice>> views;

    std::unordered_map<std::pair<std::uintptr_t, std::size_t>, std::vector<std::uintptr_t>, PairHash> shown;
};

std::mutex PHYSICAL_LOCK;
Physical& physical_state() {
    static Physical p;
    return p;
}

std::string os_error(DWORD e) { return std::system_category().message(static_cast<int>(e)) + " (os error " + std::to_string(e) + ")"; }

std::string commit_limit_message(std::size_t len, DWORD error, const PERFORMANCE_INFORMATION* info) {
    std::string message = "VIctory: Windows commit limit reached while committing " + std::to_string(len) + " bytes of physical pages: " +
                          std::system_category().message(static_cast<int>(error)) + " (os error " + std::to_string(error) + ").\n";
    if (info) {
        std::size_t committed = info->CommitTotal * info->PageSize / MIB;
        std::size_t limit = info->CommitLimit * info->PageSize / MIB;
        message += "System commit: " + std::to_string(committed) + " MiB of " + std::to_string(limit) + " MiB in use.\n";
    }
    message += "Close other applications or increase the Windows paging file size, then retry.\n";
    return message;
}


bool commit(std::uintptr_t base, std::size_t len) {
    if (on_wine()) {
        return true;   
    }
    if (VirtualAlloc(reinterpret_cast<void*>(base), len, MEM_COMMIT, PAGE_READWRITE)) {
        return true;
    }
    DWORD error = GetLastError();
    if (error == ERROR_COMMITMENT_LIMIT) {
        PERFORMANCE_INFORMATION info{};
        info.cb = sizeof info;
        bool queried = K32GetPerformanceInfo(&info, info.cb) != 0;
        loader::crash::log_crash(commit_limit_message(len, error, queried ? &info : nullptr));
    } else {
        log::warn("victory_hle::modules::xmem", "committing physical pages failed", {{"len", log::debug(static_cast<std::uint64_t>(len))}, {"error", os_error(error)}});
    }
    return false;
}


class View {
public:

    static std::unique_ptr<View> map(const Slice& slice, std::size_t n) {
        std::uint64_t offset = static_cast<std::uint64_t>(slice.index) * PAGE;
        std::size_t len = n * PAGE;
        void* at = MapViewOfFile(slice.section->handle, FILE_MAP_WRITE, static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset), len);
        if (!at) {
            return nullptr;
        }
        auto view = std::unique_ptr<View>(new View(at, len));
        if (!commit(reinterpret_cast<std::uintptr_t>(at), len)) {
            return nullptr;
        }
        return view;
    }
    ~View() { UnmapViewOfFile(at_); }
    std::uint8_t* bytes() { return static_cast<std::uint8_t*>(at_); }
    std::size_t len() const { return len_; }

private:
    View(void* at, std::size_t len) : at_(at), len_(len) {}
    void* at_;
    std::size_t len_;
};


std::optional<std::vector<std::pair<Slice, std::size_t>>> runs(std::span<const std::size_t> pages, std::size_t offset, std::size_t len) {
    if (offset > SIZE_MAX - len) {
        return std::nullopt;
    }
    std::size_t end = offset + len;
    std::size_t first = offset / PAGE, last = end / PAGE + (end % PAGE != 0);
    if (first > last || last > pages.size()) {
        return std::nullopt;
    }
    std::lock_guard l(PHYSICAL_LOCK);
    auto& physical = physical_state();
    std::vector<std::pair<Slice, std::size_t>> out;
    for (std::size_t i = first; i < last; ++i) {
        auto it = physical.pages.find(pages[i]);
        if (it == physical.pages.end()) {
            return std::nullopt;
        }
        const Slice& slice = it->second;
        if (!out.empty() && out.back().first.section == slice.section && slice.index == out.back().first.index + out.back().second) {
            ++out.back().second;
        } else {
            out.emplace_back(slice, 1);
        }
    }
    return out;
}


void written(const Slice& slice, std::size_t n) {
    std::vector<std::uintptr_t> shown;
    {
        std::lock_guard l(PHYSICAL_LOCK);
        auto& physical = physical_state();
        for (std::size_t k = 0; k < n; ++k) {
            if (auto it = physical.shown.find(slice.key(k)); it != physical.shown.end()) {
                shown.insert(shown.end(), it->second.begin(), it->second.end());
            }
        }
    }
    std::sort(shown.begin(), shown.end());
    std::size_t i = 0;
    while (i < shown.size()) {
        std::size_t j = i + 1;
        while (j < shown.size() && shown[j - 1] + PAGE == shown[j]) {
            ++j;
        }
        kernel_will_write(shown[i], (j - i) * PAGE);
        i = j;
    }
}

bool map(Physical& physical, std::uintptr_t base, std::size_t len, Slice slice);


void unmap(Physical& physical, std::uintptr_t base, std::size_t len) {

    if (len == 0) {
        return;
    }
    std::uintptr_t end = base + len;


    std::vector<std::uintptr_t> hit;
    auto it = physical.views.upper_bound(base);
    if (it != physical.views.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second.first > base) {
            hit.push_back(prev->first);
        }
    }
    for (auto i = physical.views.lower_bound(base + 1); i != physical.views.end() && i->first < end; ++i) {
        hit.push_back(i->first);
    }
    for (std::uintptr_t at : hit) {
        auto found = physical.views.find(at);
        if (found == physical.views.end()) {
            continue;
        }
        auto [size, slice] = std::move(found->second);
        physical.views.erase(found);
        for (std::size_t k = 0; k < size / PAGE; ++k) {
            if (auto s = physical.shown.find(slice.key(k)); s != physical.shown.end()) {
                std::erase(s->second, at + k * PAGE);
                if (s->second.empty()) {
                    physical.shown.erase(s);
                }
            }
        }
        title_memory_remapped(at, size);

        UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(at), MEM_PRESERVE_PLACEHOLDER);
        if (at < base) {
            map(physical, at, base - at, slice);
        }
        if (at + size > end) {
            Slice rest{slice.section, slice.index + (end - at) / PAGE};
            map(physical, end, at + size - end, rest);
        }
    }
}


bool map(Physical& physical, std::uintptr_t base, std::size_t len, Slice slice) {


    VirtualFree(reinterpret_cast<void*>(base), len, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    void* view = MapViewOfFile3(slice.section->handle, GetCurrentProcess(), reinterpret_cast<void*>(base), static_cast<std::uint64_t>(slice.index) * PAGE, len,
                                MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (reinterpret_cast<std::uintptr_t>(view) != base) {
        log::warn("victory_hle::modules::xmem", "mapping physical pages failed (not a placeholder?)",
                  {{"address", log::hex(base)}, {"len", log::debug(static_cast<std::uint64_t>(len))}, {"error", os_error(GetLastError())}});
        return false;
    }
    if (!commit(base, len)) {
        UnmapViewOfFile2(GetCurrentProcess(), view, MEM_PRESERVE_PLACEHOLDER);
        return false;
    }

    title_memory_remapped(base, len);

    inplace::title_memory_view(base, len, reinterpret_cast<std::uintptr_t>(slice.section->handle), static_cast<std::uint64_t>(slice.index) * PAGE);
    for (std::size_t k = 0; k < len / PAGE; ++k) {
        physical.shown[slice.key(k)].push_back(base + k * PAGE);
    }
    physical.views.insert_or_assign(base, std::pair{len, std::move(slice)});
    return true;
}

}   


BOOL WINAPI XMemAllocatePhysicalPages(std::uint64_t, std::size_t* count, std::size_t* pages) {
    if (!count || !pages) {
        return 0;
    }
    std::size_t n = *count;
    if (n == 0 || n > SIZE_MAX / PAGE) {
        return 0;
    }
    std::size_t size = n * PAGE;


    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | (on_wine() ? SEC_COMMIT : SEC_RESERVE), static_cast<DWORD>(static_cast<std::uint64_t>(size) >> 32),
                                       static_cast<DWORD>(size), nullptr);
    if (!handle) {
        log::warn("victory_hle::modules::xmem", "XMemAllocatePhysicalPages failed", {{"n", log::debug(static_cast<std::uint64_t>(n))}, {"error", os_error(GetLastError())}});
        *count = 0;
        return 0;
    }


    inplace::title_memory_section(reinterpret_cast<std::uintptr_t>(handle), size, on_wine());
    auto section = std::make_shared<Section>(handle);
    std::lock_guard l(PHYSICAL_LOCK);
    auto& physical = physical_state();
    std::size_t first = physical.next_page;
    physical.next_page += n * FRAMES;
    for (std::size_t index = 0; index < n; ++index) {
        std::size_t number = first + index * FRAMES;
        physical.pages.insert_or_assign(number, Slice{section, index});
        pages[index] = number;
    }
    return 1;
}


BOOL WINAPI XMemFreePhysicalPages(std::size_t n, const std::size_t* pages) {
    if (!pages) {
        return 0;
    }
    std::lock_guard l(PHYSICAL_LOCK);
    auto& physical = physical_state();
    for (std::size_t i = 0; i < n; ++i) {
        physical.pages.erase(pages[i]);
    }
    return 1;
}


void* WINAPI XMemMapPhysicalPages(void* address, std::size_t n, const std::size_t* pages) {
    auto base = reinterpret_cast<std::uintptr_t>(address);
    std::lock_guard l(PHYSICAL_LOCK);
    auto& physical = physical_state();


    auto changing = watch::title_memory_remapping();
    unmap(physical, base, n * PAGE);
    if (!pages) {
        return address;
    }
    std::size_t i = 0;
    while (i < n) {
        auto it = physical.pages.find(pages[i]);
        if (it == physical.pages.end()) {
            log::warn("victory_hle::modules::xmem", "XMemMapPhysicalPages: unknown page", {{"page", log::debug(static_cast<std::uint64_t>(pages[i]))}});
            return nullptr;
        }
        Slice first = it->second;

        std::size_t run = 1;
        while (i + run < n) {
            auto s = physical.pages.find(pages[i + run]);
            if (s == physical.pages.end() || s->second.section != first.section || s->second.index != first.index + run) {
                break;
            }
            ++run;
        }
        if (!map(physical, base + i * PAGE, run * PAGE, first)) {
            return nullptr;
        }
        i += run;
    }
    return address;
}


BOOL WINAPI XMemMapPhysicalPagesScatter(void* const* addresses, std::size_t n, const std::size_t* pages) {
    if (!addresses) {
        return 0;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t* page = !pages || pages[i] == 0 ? nullptr : pages + i;
        if (!XMemMapPhysicalPages(addresses[i], 1, page)) {
            return 0;
        }
    }
    return 1;
}

bool pages_known(std::span<const std::size_t> pages, std::size_t offset, std::size_t len) { return runs(pages, offset, len).has_value(); }

bool write_physical(std::span<const std::size_t> pages, std::size_t offset, std::span<const std::uint8_t> data) {
    auto r = runs(pages, offset, data.size());
    if (!r) {
        return false;
    }
    std::size_t within = offset % PAGE, done = 0;
    for (auto& [slice, n] : *r) {
        auto view = View::map(slice, n);
        if (!view) {
            return false;
        }
        std::size_t k = std::min(n * PAGE - within, data.size() - done);
        std::memcpy(view->bytes() + within, data.data() + done, k);
        within = 0;
        done += k;
        written(slice, n);
    }
    return true;
}

struct PageWindow::State {
    std::unique_ptr<View> view;
    std::size_t within, len;
    Slice slice;
};

PageWindow::PageWindow(std::unique_ptr<State> s) : state_(std::move(s)) {}
PageWindow::PageWindow(PageWindow&&) noexcept = default;
PageWindow::~PageWindow() {
    if (state_) {
        written(state_->slice, state_->view->len() / PAGE);
    }
}
std::span<std::uint8_t> PageWindow::bytes() { return {state_->view->bytes() + state_->within, state_->len}; }

std::optional<PageWindow> page_window(std::span<const std::size_t> pages, std::size_t offset, std::size_t len) {
    auto r = runs(pages, offset, len);
    if (!r || r->size() != 1) {
        return std::nullopt;
    }
    auto& [slice, n] = (*r)[0];
    auto view = View::map(slice, n);
    if (!view) {
        return std::nullopt;
    }
    return PageWindow(std::make_unique<PageWindow::State>(PageWindow::State{std::move(view), offset % PAGE, len, slice}));
}


bool map_alias(std::uint64_t to_, std::uint64_t from_, std::uint64_t len_) {
    auto to = static_cast<std::uintptr_t>(to_), from = static_cast<std::uintptr_t>(from_);
    auto len = static_cast<std::size_t>(len_);
    std::lock_guard l(PHYSICAL_LOCK);
    auto& physical = physical_state();
    auto changing = watch::title_memory_remapping();
    unmap(physical, to, len);
    for (std::size_t k = 0; k < len; k += PAGE) {
        if (from == 0) {
            break;
        }
        std::uintptr_t at = from + k;
        auto it = physical.views.upper_bound(at);
        if (it == physical.views.begin()) {
            return false;
        }
        --it;
        if (!(at < it->first + it->second.first)) {
            return false;
        }
        Slice slice{it->second.second.section, it->second.second.index + (at - it->first) / PAGE};
        if (!map(physical, to + k, PAGE, slice)) {
            return false;
        }
    }
    return true;
}


struct WorkingSet {
    std::size_t title_limit, title_used, tools_limit, tools_used;
    std::uint32_t stack_used, physical_pages;
    std::size_t gpu_optimal_limit, gpu_optimal_used, tools_gpu_optimal_limit, tools_gpu_optimal_used;
    std::uint32_t kernel_pool_used, kernel_pool_limit;
};


BOOL WINAPI XMemGetWorkingSetStatistics(std::uint32_t which, WorkingSet* out) {
    if (!out) {
        return 0;
    }
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof counters;
    K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, counters.cb);
    auto [gpu_optimal, standard] = d3d12x::title_memory(d3d12x::current_console());
    WorkingSet stats{};
    stats.title_limit = gpu_optimal + standard - TRANSFERRED.load(std::memory_order_relaxed);
    stats.title_used = counters.PagefileUsage;
    stats.gpu_optimal_limit = gpu_optimal;

    static std::atomic<std::uint64_t> asked{0};
    std::uint64_t a = asked.fetch_add(1, std::memory_order_relaxed);
    if (a % 1024 == 0) {
        log::info("victory_hle::modules::xmem", "the title asks how much memory it uses",
                  {{"asked", log::debug(a + 1)}, {"which", log::debug(which)}, {"limit_mb", log::debug(static_cast<std::uint64_t>(stats.title_limit >> 20))},
                   {"used_mb", log::debug(static_cast<std::uint64_t>(stats.title_used >> 20))}});
    }
    *out = stats;
    return 1;
}


BOOL WINAPI XMemTransferMemory(std::uint32_t kind, std::size_t size) {
    constexpr std::uint32_t TITLE_TO_SYSTEM = 0;
    log::info("victory_hle::modules::xmem", "XMemTransferMemory", {{"kind", log::debug(kind)}, {"size", log::debug(static_cast<std::uint64_t>(size))}});
    if (size == 0) {
        return 1;
    }
    std::size_t step = 2 * MIB;
    if (size > SIZE_MAX - (step - 1)) {
        return 0;
    }
    size = (size + step - 1) / step * step;
    std::size_t standard = d3d12x::title_memory(d3d12x::current_console()).second;
    if (kind != TITLE_TO_SYSTEM) {
        return 0;
    }
    std::size_t t = TRANSFERRED.load(std::memory_order_relaxed);
    for (;;) {
        if (t > SIZE_MAX - size || t + size > standard) {
            return 0;
        }
        if (TRANSFERRED.compare_exchange_weak(t, t + size, std::memory_order_relaxed)) {
            return 1;
        }
    }
}

#define E(f) HleExport{#f, reinterpret_cast<std::uintptr_t>(&f)}
std::vector<HleExport> exports() {
    return {E(XMemAlloc),
            E(XMemFree),
            E(XMemAllocDefault),
            E(XMemFreeDefault),
            E(XMemIsToolsMemAvailable),
            E(XMemAllocatePhysicalPages),
            E(XMemFreePhysicalPages),
            E(XMemMapPhysicalPages),
            E(XMemMapPhysicalPagesScatter),
            E(XMemVirtualAlloc),
            E(XMemVirtualQuery),
            E(XMemGetWorkingSetStatistics),
            E(XMemTransferMemory)};
}
#undef E

}   
}   
