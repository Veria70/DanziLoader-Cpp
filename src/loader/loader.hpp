// SPDX-License-Identifier: GPL-3.0-or-later
// Runs a title's original x86-64 code natively, inside our process
// (`victory-loader`; DanziLoader / victory-perf, see NOTICE.md). Xbox Series
// X|S executables are x86-64 PE32+ images built for a Zen 2, so the machine
// code needs no translation. The loader:
// 1. `prepare`s a patched copy (prepare.hpp: a DLL with no entry point, no
//    import directory, no TLS callbacks the OS would run);
// 2. `load`s it as a real module (debuggers and stack walks see it);
// 3. maps the DLLs the title ships the same way (`load_title_dlls`);
// 4. binds every import itself through an `ImportResolver` (the runtime,
//    the host, the title's DLLs); what nobody provides gets a stub that
//    reports exactly what was called;
// 5. initialises the title's DLLs, then runs TLS callbacks and the entry
//    point on a thread with the stack the image asks for, and from then on
//    passes every thread start and end to the attached images.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pe/image.hpp"
#include "prepare.hpp"

namespace danzi::loader {

constexpr std::uint32_t DLL_PROCESS_DETACH_ = 0;
constexpr std::uint32_t DLL_PROCESS_ATTACH_ = 1;


class LoaderError : public std::runtime_error {
public:
    explicit LoaderError(const std::string& message) : std::runtime_error(message), chain_{message} {}


    explicit LoaderError(std::vector<std::string> chain) : std::runtime_error(join(chain)), chain_(std::move(chain)) {}
    const std::vector<std::string>& chain() const { return chain_; }

private:
    static std::string join(const std::vector<std::string>& c) {
        std::string s;
        for (std::size_t i = 0; i < c.size(); ++i) {
            s += (i ? ": " : "") + c[i];
        }
        return s;
    }
    std::vector<std::string> chain_;
};


enum class Provenance { Hle, Host, Title };

struct Resolution {
    bool found = false;
    std::uintptr_t address = 0;
    Provenance provenance = Provenance::Hle;
    static Resolution missing() { return {}; }
    static Resolution at(std::uintptr_t a, Provenance p) { return {true, a, p}; }
};


class ImportResolver {
public:
    virtual ~ImportResolver() = default;
    virtual Resolution resolve(std::string_view dll, const pe::ImportSymbol& symbol) const = 0;
};


enum class UnimplementedPolicy {

    Trap,

    ReturnZero,
};


std::uint64_t unimplemented_call_count();


std::uintptr_t not_ported_stub(const std::string& module, const std::string& name);

struct MissingImport {
    std::string dll, symbol;
    bool operator==(const MissingImport&) const = default;
};


struct BindReport {
    std::size_t hle = 0, host = 0, title = 0;
    std::vector<MissingImport> missing;
};

class LoadedImage;
using TitleDlls = std::vector<std::pair<std::string, std::shared_ptr<LoadedImage>>>;


class LoadedImage {
public:
    LoadedImage(HMODULE module, std::filesystem::path path, pe::ImageInfo info) : module_(module), path_(std::move(path)), info_(std::move(info)) {}

    std::uintptr_t base() const { return reinterpret_cast<std::uintptr_t>(module_); }
    HMODULE module() const { return module_; }
    const std::filesystem::path& path() const { return path_; }

    const pe::ImageInfo& info() const { return info_; }


    std::optional<std::uintptr_t> export_(const std::string& name) const;
    std::optional<std::uintptr_t> export_symbol(const pe::ImportSymbol& symbol) const;


    BindReport bind_imports(const ImportResolver& resolver, UnimplementedPolicy policy) const;


    void run_tls_callbacks(std::uint32_t reason) const;


    void attach() const;


    void detach() const;


    std::optional<int> notify(std::uint32_t reason, bool dll_main, void* reserved) const;


    void unload() const;

private:
    void write_slots(const std::vector<std::pair<std::uint32_t, std::uintptr_t>>& slots) const;
    void tls_callbacks(std::uint32_t reason, void* reserved) const;

    HMODULE module_;
    std::filesystem::path path_;
    pe::ImageInfo info_;
};


class MainThread {
public:
    MainThread() = default;
    MainThread(MainThread&&) noexcept;
    MainThread& operator=(MainThread&&) noexcept;
    ~MainThread();


    std::uint32_t join();

    struct State;

private:
    friend MainThread spawn_main(std::shared_ptr<LoadedImage>, TitleDlls);
    HANDLE thread_ = nullptr;
    std::unique_ptr<State> state_;
};


std::shared_ptr<LoadedImage> load(const PreparedImage& prepared, const std::filesystem::path& dir);


TitleDlls load_title_dlls(const pe::ImageInfo& main, const std::filesystem::path& title_dir, const std::filesystem::path& cache_dir,
                          const std::function<bool(const std::string&)>& wanted);


TitleDlls load_title_dll(const std::string& name, const std::filesystem::path& title_dir, const std::filesystem::path& cache_dir,
                         const std::function<bool(const std::string&)>& wanted);


class WithTitleDlls : public ImportResolver {
public:
    WithTitleDlls(const ImportResolver& inner, const TitleDlls& dlls) : inner_(inner), dlls_(dlls) {}
    Resolution resolve(std::string_view dll, const pe::ImportSymbol& symbol) const override;

private:
    const ImportResolver& inner_;
    const TitleDlls& dlls_;
};


MainThread spawn_main(std::shared_ptr<LoadedImage> image, TitleDlls dlls);


std::optional<bool> disable_thread_calls(std::uintptr_t base);

namespace threads {
void mapped(const std::shared_ptr<LoadedImage>& image);
void attached(std::uintptr_t base);
void detached(std::uintptr_t base);
void unmapped(std::uintptr_t base);
}   

namespace tls {


std::size_t redirect(std::uintptr_t base, const pe::ImageInfo& info);
}   

namespace hooks {
constexpr const char* NO_WATCHDOG_ENV = "VICTORY_NO_WATCHDOG";
constexpr const char* RENDER_SCALE_ENV = "VICTORY_RENDER_SCALE";
constexpr const char* OWN_UPSCALER_ENV = "VICTORY_OWN_UPSCALER";

std::size_t disarm_watchdog(std::uintptr_t base, const std::array<std::uint8_t, 32>& hash);

std::size_t resolution_levers(std::uintptr_t base, const std::array<std::uint8_t, 32>& hash);

std::optional<float> parse_scale(std::string_view text);
}   

}   
