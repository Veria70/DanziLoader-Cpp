// SPDX-License-Identifier: GPL-3.0-or-later
// Loading a prepared image as a real Windows module and binding it
// (`victory-loader/src/windows.rs`), and the stubs for imports nobody
// implements (`thunks.rs`); DanziLoader / victory-perf, see NOTICE.md. See
// loader.hpp.
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

#include "core/env.hpp"
#include "core/log.hpp"
#include "core/policy.hpp"
#include "crash.hpp"
#include "loader.hpp"

namespace danzi::loader {

namespace fs = std::filesystem;

namespace {


std::string os_error(DWORD code) { return std::system_category().message(static_cast<int>(code)) + " (os error " + std::to_string(code) + ")"; }


std::string path_display(const std::filesystem::path& p) {
    const std::wstring& w = p.native();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
    }
    return out;
}

bool eq_ignore_ascii_case(std::string_view a, std::string_view b) { return a.size() == b.size() && lower_ascii(a) == lower_ascii(b); }


struct ImportSite {
    std::string dll, symbol;
    UnimplementedPolicy policy;
    std::atomic<std::uint64_t> calls{0};

    bool not_ported = false;
};

std::atomic<std::uint64_t> UNIMPLEMENTED_CALLS{0};

std::uint64_t __stdcall unimplemented_import(ImportSite* site) {
    UNIMPLEMENTED_CALLS.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t previous = site->calls.fetch_add(1, std::memory_order_relaxed);
    if (site->policy == UnimplementedPolicy::ReturnZero) {
        if (previous == 0) {
            log::warn("victory_loader::thunks", "unimplemented import called, returning 0", {{"dll", site->dll}, {"symbol", site->symbol}});
        }
        return 0;
    }
    log::error("victory_loader::thunks", "unimplemented import called", {{"dll", site->dll}, {"symbol", site->symbol}});


    if (site->not_ported) {
        crash::log_crash("port: runtime export " + site->dll + "!" + site->symbol + " is not ported yet");
    } else {
        crash::log_crash("VIctory: unimplemented import " + site->dll + "!" + site->symbol + " called");
    }
    if (IsDebuggerPresent()) {
        DebugBreak();
    }
    std::abort();
}

constexpr std::size_t THUNK_SIZE = 32;


class ThunkBlock {
public:
    explicit ThunkBlock(std::size_t count) {
        len_ = (std::max<std::size_t>(count, 1) * THUNK_SIZE + 4095) / 4096 * 4096;
        base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, len_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!base_) {
            throw LoaderError("VirtualAlloc for import stubs failed: " + os_error(GetLastError()));
        }
    }
    std::uintptr_t emit(ImportSite* site) {
        if (used_ + THUNK_SIZE > len_) {
            throw LoaderError("import stub block is full");
        }
        std::uint8_t code[THUNK_SIZE];
        std::memset(code, 0xCC, sizeof code);   
        code[0] = 0x48;
        code[1] = 0xB9;
        auto s = reinterpret_cast<std::uint64_t>(site);
        std::memcpy(code + 2, &s, 8);
        code[10] = 0x48;
        code[11] = 0xB8;
        auto h = reinterpret_cast<std::uint64_t>(&unimplemented_import);
        std::memcpy(code + 12, &h, 8);
        code[20] = 0xFF;
        code[21] = 0xE0;
        std::uint8_t* at = base_ + used_;
        std::memcpy(at, code, THUNK_SIZE);
        used_ += THUNK_SIZE;
        return reinterpret_cast<std::uintptr_t>(at);
    }


    void seal() {
        DWORD old = 0;
        if (!VirtualProtect(base_, len_, PAGE_EXECUTE_READ, &old)) {
            throw LoaderError("VirtualProtect for import stubs failed: " + os_error(GetLastError()));
        }
        FlushInstructionCache(GetCurrentProcess(), base_, len_);
    }

private:
    std::uint8_t* base_ = nullptr;
    std::size_t len_ = 0, used_ = 0;
};

void write_if_changed(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (in) {
            auto size = static_cast<std::size_t>(in.tellg());
            if (size == bytes.size()) {
                std::vector<std::uint8_t> existing(size);
                in.seekg(0);
                in.read(reinterpret_cast<char*>(existing.data()), static_cast<std::streamsize>(size));
                if (static_cast<std::size_t>(in.gcount()) == size && existing == bytes) {
                    return;
                }
            }
        }
    }
    fs::path tmp = path;
    tmp.replace_extension("tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        if (!out) {
            throw LoaderError("writing " + tmp.string() + ": " + os_error(GetLastError()));
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        throw LoaderError("renaming to " + path.string() + ": " + os_error(static_cast<DWORD>(ec.value())));
    }
}

void map_dll(const std::string& raw_name, const fs::path& title_dir, const fs::path& cache_dir, const std::function<bool(const std::string&)>& wanted,
             std::set<std::string>& seen, TitleDlls& loaded);

void map_imports(const pe::ImageInfo& info, const fs::path& title_dir, const fs::path& cache_dir, const std::function<bool(const std::string&)>& wanted,
                 std::set<std::string>& seen, TitleDlls& loaded) {
    for (const auto* list : {&info.imports, &info.delay_imports}) {
        for (const auto& m : *list) {
            map_dll(m.dll, title_dir, cache_dir, wanted, seen, loaded);
        }
    }
}

void map_dll(const std::string& raw_name, const fs::path& title_dir, const fs::path& cache_dir, const std::function<bool(const std::string&)>& wanted,
             std::set<std::string>& seen, TitleDlls& loaded) {
    std::string name = lower_ascii(raw_name);
    fs::path path = title_dir / fs::path(std::u8string(name.begin(), name.end()));
    if (!seen.insert(name).second || !wanted(name)) {
        return;
    }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return;
    }
    std::vector<std::uint8_t> original;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            throw LoaderError("reading " + path.string() + ": " + os_error(GetLastError()));
        }
        original.resize(static_cast<std::size_t>(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(original.data()), static_cast<std::streamsize>(original.size()));
    }
    PreparedImage prepared;
    try {
        prepared = prepare(original);
    } catch (const PrepareError& e) {
        throw LoaderError("preparing " + path.string() + ": " + e.display());
    }
    std::shared_ptr<LoadedImage> image;
    try {
        image = load(prepared, cache_dir);
    } catch (const std::exception& e) {
        throw LoaderError("loading " + path.string() + ": " + e.what());
    }
    map_imports(image->info(), title_dir, cache_dir, wanted, seen, loaded);
    loaded.emplace_back(name, image);
}

}   

std::uint64_t unimplemented_call_count() { return UNIMPLEMENTED_CALLS.load(std::memory_order_relaxed); }

std::uintptr_t not_ported_stub(const std::string& module, const std::string& name) {

    static std::mutex lock;
    std::lock_guard l(lock);
    ThunkBlock block(1);
    auto* site = new ImportSite{module, name, UnimplementedPolicy::Trap};
    site->not_ported = true;
    std::uintptr_t at = block.emit(site);
    block.seal();
    return at;
}

std::shared_ptr<LoadedImage> load(const PreparedImage& prepared, const fs::path& dir) {
    try {
        policy::ensure_outside_repo(dir);
    } catch (const policy::PolicyError& e) {
        throw LoaderError(e.what());
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw LoaderError("creating " + dir.string() + ": " + os_error(static_cast<DWORD>(ec.value())));
    }
    fs::path path = dir / prepared.file_name();
    write_if_changed(path, prepared.bytes);
    HMODULE module = LoadLibraryExW(path.wstring().c_str(), nullptr, 0);
    if (!module) {
        DWORD e = GetLastError();
        throw LoaderError("LoadLibraryExW(" + path.string() + ") failed: " + os_error(e) +
                          (prepared.info.has_relocations ? "" : " (image has no relocations; its preferred base may be taken)"));
    }
    auto loaded = std::make_shared<LoadedImage>(module, path, prepared.info);

    if (!loaded->info().is_dll()) {
        tls::redirect(loaded->base(), loaded->info());
    }
    if (env::get(L"VICTORY_NO_WATCHDOG")) {
        std::size_t applied = hooks::disarm_watchdog(loaded->base(), prepared.original_hash);
        log::info("victory_loader::windows", "title watchdog hooks (VICTORY_NO_WATCHDOG)", {{"applied", log::debug(static_cast<std::uint64_t>(applied))}});
    }
    if (std::size_t levers = hooks::resolution_levers(loaded->base(), prepared.original_hash); levers > 0) {
        log::warn("victory_loader::windows", "title resolution levers (VICTORY_RENDER_SCALE, VICTORY_OWN_UPSCALER)", {{"applied", log::debug(static_cast<std::uint64_t>(levers))}});
    }
    log::info("victory_loader::windows", "image mapped",
              {{"base", log::hex(loaded->base())}, {"preferred", log::hex(loaded->info().image_base)}, {"path", path_display(loaded->path())}});
    threads::mapped(loaded);
    return loaded;
}

TitleDlls load_title_dlls(const pe::ImageInfo& main, const fs::path& title_dir, const fs::path& cache_dir,
                          const std::function<bool(const std::string&)>& wanted) {
    TitleDlls loaded;
    std::set<std::string> seen;
    map_imports(main, title_dir, cache_dir, wanted, seen, loaded);
    return loaded;
}

TitleDlls load_title_dll(const std::string& name, const fs::path& title_dir, const fs::path& cache_dir,
                         const std::function<bool(const std::string&)>& wanted) {
    TitleDlls loaded;
    std::set<std::string> seen;
    map_dll(name, title_dir, cache_dir, wanted, seen, loaded);
    return loaded;
}

Resolution WithTitleDlls::resolve(std::string_view dll, const pe::ImportSymbol& symbol) const {
    Resolution found = inner_.resolve(dll, symbol);
    if (found.found) {
        return found;
    }
    for (const auto& [name, image] : dlls_) {
        if (eq_ignore_ascii_case(name, dll)) {
            if (auto a = image->export_symbol(symbol)) {
                return Resolution::at(*a, Provenance::Title);
            }
            return Resolution::missing();
        }
    }
    return Resolution::missing();
}

std::optional<std::uintptr_t> LoadedImage::export_(const std::string& name) const {
    if (name.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    FARPROC f = GetProcAddress(module_, name.c_str());
    return f ? std::optional(reinterpret_cast<std::uintptr_t>(f)) : std::nullopt;
}

std::optional<std::uintptr_t> LoadedImage::export_symbol(const pe::ImportSymbol& symbol) const {
    if (!symbol.by_ordinal) {
        return export_(symbol.name);
    }
    FARPROC f = GetProcAddress(module_, reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(symbol.ordinal)));
    return f ? std::optional(reinterpret_cast<std::uintptr_t>(f)) : std::nullopt;
}

BindReport LoadedImage::bind_imports(const ImportResolver& resolver, UnimplementedPolicy policy) const {
    BindReport report;
    std::vector<std::pair<std::uint32_t, std::uintptr_t>> slots;
    slots.reserve(info_.import_count());
    struct Unresolved {
        std::uint32_t slot;
        const std::string* dll;
        const pe::ImportSymbol* symbol;
    };
    std::vector<Unresolved> unresolved;


    for (const auto* list : {&info_.imports, &info_.delay_imports}) {
        for (const auto& m : *list) {
            for (const auto& e : m.entries) {
                Resolution r = resolver.resolve(m.dll, e.symbol);
                if (r.found) {
                    switch (r.provenance) {
                        case Provenance::Hle: ++report.hle; break;
                        case Provenance::Host: ++report.host; break;
                        case Provenance::Title: ++report.title; break;
                    }
                    slots.emplace_back(e.iat_slot_rva, r.address);
                } else {
                    unresolved.push_back({e.iat_slot_rva, &m.dll, &e.symbol});
                }
            }
        }
    }
    if (!unresolved.empty()) {
        ThunkBlock block(unresolved.size());
        for (const auto& u : unresolved) {
            auto* site = new ImportSite{*u.dll, u.symbol->to_string(), policy};
            slots.emplace_back(u.slot, block.emit(site));
            report.missing.push_back({site->dll, site->symbol});
        }
        block.seal();
    }
    write_slots(slots);
    log::info("victory_loader::windows", "imports bound",
              {{"hle", log::debug(static_cast<std::uint64_t>(report.hle))}, {"host", log::debug(static_cast<std::uint64_t>(report.host))},
               {"missing", log::debug(static_cast<std::uint64_t>(report.missing.size()))}});
    return report;
}


void LoadedImage::write_slots(const std::vector<std::pair<std::uint32_t, std::uintptr_t>>& slots) const {
    for (auto [rva, address] : slots) {
        auto* slot = reinterpret_cast<std::uint64_t*>(base() + rva);
        DWORD old = 0;
        if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "0x%x", rva);
            throw LoaderError(std::string("VirtualProtect(IAT slot ") + buf + ") failed: " + os_error(GetLastError()));
        }
        *static_cast<volatile std::uint64_t*>(slot) = address;
        DWORD ignored = 0;
        VirtualProtect(slot, 8, old, &ignored);
    }
}

void LoadedImage::run_tls_callbacks(std::uint32_t reason) const { tls_callbacks(reason, nullptr); }

void LoadedImage::tls_callbacks(std::uint32_t reason, void* reserved) const {
    if (!info_.tls) {
        return;
    }
    using TlsCallback = void(__stdcall*)(void*, DWORD, void*);
    for (std::uint32_t rva : info_.tls->callback_rvas) {
        reinterpret_cast<TlsCallback>(base() + rva)(module_, reason, reserved);
    }
}

void LoadedImage::attach() const {
    if (notify(DLL_PROCESS_ATTACH_, true, nullptr) == std::optional<int>(0)) {
        throw LoaderError("DllMain returned FALSE");
    }
    threads::attached(base());
}

void LoadedImage::detach() const {
    threads::detached(base());
    notify(DLL_PROCESS_DETACH_, true, nullptr);
}

std::optional<int> LoadedImage::notify(std::uint32_t reason, bool dll_main, void* reserved) const {
    tls_callbacks(reason, reserved);
    if (!dll_main || !info_.is_dll() || info_.entry_rva == 0) {
        return std::nullopt;
    }
    using DllMain = BOOL(__stdcall*)(void*, DWORD, void*);
    return reinterpret_cast<DllMain>(base() + info_.entry_rva)(module_, reason, reserved);
}

void LoadedImage::unload() const {
    threads::unmapped(base());
    FreeLibrary(module_);
}


struct MainThread::State {
    std::shared_ptr<LoadedImage> image;
    TitleDlls dlls;
    std::uint32_t result = 0;
    std::optional<std::vector<std::string>> error;
};

namespace {

DWORD WINAPI title_main(void* p) {
    auto* s = static_cast<MainThread::State*>(p);
    for (const auto& [name, dll] : s->dlls) {
        try {
            dll->attach();
        } catch (const LoaderError& e) {
            std::vector<std::string> chain{"attaching " + name};
            chain.insert(chain.end(), e.chain().begin(), e.chain().end());
            s->error = std::move(chain);
            return 0;
        } catch (const std::exception& e) {
            s->error = std::vector<std::string>{"attaching " + name, e.what()};
            return 0;
        }
    }
    s->image->run_tls_callbacks(DLL_PROCESS_ATTACH_);
    threads::attached(s->image->base());
    using Entry = DWORD(__stdcall*)(void*);
    s->result = reinterpret_cast<Entry>(s->image->base() + s->image->info().entry_rva)(nullptr);
    return 0;
}

}   

MainThread spawn_main(std::shared_ptr<LoadedImage> image, TitleDlls dlls) {
    if (image->info().entry_rva == 0) {
        throw LoaderError("image has no entry point");
    }
    std::uint64_t reserve = image->info().stack_reserve;
    std::size_t stack = static_cast<std::size_t>(std::max<std::uint64_t>(reserve, 1u << 20));
    MainThread t;
    t.state_ = std::make_unique<MainThread::State>();
    t.state_->image = std::move(image);
    t.state_->dlls = std::move(dlls);
    t.thread_ = CreateThread(nullptr, stack, title_main, t.state_.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t.thread_) {
        throw LoaderError("spawning title main thread: " + os_error(GetLastError()));
    }
    SetThreadDescription(t.thread_, L"title-main");
    return t;
}

MainThread::MainThread(MainThread&& o) noexcept : thread_(std::exchange(o.thread_, nullptr)), state_(std::move(o.state_)) {}
MainThread& MainThread::operator=(MainThread&& o) noexcept {
    std::swap(thread_, o.thread_);
    std::swap(state_, o.state_);
    return *this;
}
MainThread::~MainThread() {
    if (thread_) {
        CloseHandle(thread_);
    }
}

std::uint32_t MainThread::join() {
    WaitForSingleObject(thread_, INFINITE);
    if (state_->error) {
        throw LoaderError(*state_->error);
    }
    return state_->result;
}

}   
