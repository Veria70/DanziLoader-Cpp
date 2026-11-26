// SPDX-License-Identifier: GPL-3.0-or-later
// `victory`: the command-line front end (`apps/victory/src/main.rs`,
// `inspect.rs`, `census.rs`, `run.rs`; DanziLoader / victory-perf, see
// NOTICE.md).
//
// Every command reads the user's own files and writes only to stdout or to
// the user's cache directory (outside the repository).
#include <windows.h>

#include <blake3.h>

#include <fcntl.h>
#include <io.h>

#include <condition_variable>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "core/log.hpp"
#include "core/policy.hpp"
#include "d3d12x/device.hpp"
#include "hle/modules.hpp"
#include "hle/registry.hpp"
#include "hle/runtime.hpp"
#include "loader/crash.hpp"
#include "loader/loader.hpp"
#include "loader/prepare.hpp"
#include "pe/image.hpp"
#include "pe/implib.hpp"
#include "pe/layout.hpp"
#include "shaders.hpp"

using namespace danzi;
namespace fs = std::filesystem;
using config::Error;

namespace {

constexpr const char* VERSION = "0.1.0";


void out(const std::string& line) {
    std::string l = line + "\n";
    std::fwrite(l.data(), 1, l.size(), stdout);
}

std::string show(const fs::path& p) { return config::display(p); }

fs::path arg_path(const std::wstring& s) { return fs::path(s); }

std::string utf8(const std::wstring& w) { return config::display(fs::path(w)); }

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        DWORD e = GetLastError();
        throw Error({"reading " + show(path), std::system_category().message(static_cast<int>(e)) + " (os error " + std::to_string(e) + ")"});
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return data;
}

std::string blake3_hex(std::span<const std::uint8_t> data) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data.data(), data.size());
    std::uint8_t out[32];
    blake3_hasher_finalize(&h, out, 32);
    std::string s;
    for (auto b : out) {
        s += std::format("{:02x}", b);
    }
    return s;
}


const char* yes(bool b) { return b ? "yes" : "no"; }

std::string dll_flags(std::uint16_t v) {
    using namespace pe::layout::dll_flags;
    const std::pair<std::uint16_t, const char*> names[] = {{HIGH_ENTROPY_VA, "high-entropy-va"}, {DYNAMIC_BASE, "dynamic-base"}, {FORCE_INTEGRITY, "force-integrity"},
                                                          {NX_COMPAT, "nx"},                     {APPCONTAINER, "appcontainer"},  {GUARD_CF, "cfg"}};
    std::string set;
    for (auto [f, n] : names) {
        if (v & f) {
            set += (set.empty() ? "" : ", ") + std::string(n);
        }
    }
    return set.empty() ? "" : "  (" + set + ")";
}

pe::ImageInfo parse_image(const fs::path& file, const std::vector<std::uint8_t>& data) {
    try {
        return pe::ImageInfo::parse(data);
    } catch (const std::exception& e) {
        throw Error::context("parsing " + show(file), e);
    }
}

void inspect(const fs::path& file, bool list_imports, bool list_exports) {
    auto data = read_file(file);
    auto info = parse_image(file, data);
    out("file           " + show(file));
    out("blake3         " + blake3_hex(data));
    if (info.codeview) {
        out("pdb            " + info.codeview->pdb_path + "  (key " + info.codeview->symbol_key() + ")");
    }
    out(std::string("kind           ") + (info.is_dll() ? "dll" : "exe"));
    out(std::format("subsystem      {} ({})", info.subsystem, info.subsystem_name()));
    out(std::format("image base     {:#x}  size {:#x}", info.image_base, info.size_of_image));
    out(std::format("entry rva      {:#x}", info.entry_rva));
    out(std::format("stack          reserve {:#x} commit {:#x}", info.stack_reserve, info.stack_commit));
    out(std::format("dll flags      {:#06x}{}", info.dll_characteristics, dll_flags(info.dll_characteristics)));
    out(std::format("relocations    {}   exception table {}   load config {}", yes(info.has_relocations), yes(info.has_exception_table), yes(info.has_load_config)));
    if (info.tls) {
        out(std::format("tls            {} callback(s)", info.tls->callback_rvas.size()));
    }
    out("\nsections");
    for (const auto& s : info.sections) {
        std::string name = s.name;
        while (!name.empty() && name.back() == '\0') {
            name.pop_back();
        }
        out(std::format("  {:<8} rva {:#010x} vsize {:#010x} raw {:#010x} flags {:#010x}", name, s.rva, s.virtual_size, s.file_size, s.characteristics));
    }
    out(std::format("\nimports        {} functions from {} DLLs", info.import_count(), info.imports.size()));
    auto modules = [&](const std::vector<pe::ImportModule>& list) {
        for (const auto& m : list) {
            out(std::format("  {:<48} {:>5}", m.dll, m.entries.size()));
            if (list_imports) {
                for (const auto& e : m.entries) {
                    out("      " + e.symbol.to_string());
                }
            }
        }
    };
    modules(info.imports);
    if (!info.delay_imports.empty()) {
        out("\ndelay-load imports");
        modules(info.delay_imports);
    }
    out(std::format("\nexports        {}", info.exports.size()));
    if (list_exports) {
        for (const auto& e : info.exports) {
            std::string name = e.name.value_or("(by ordinal)");
            if (e.forward) {
                out(std::format("  {:>5}  {} -> {}", e.ordinal, name, *e.forward));
            } else if (e.rva) {
                out(std::format("  {:>5}  {}  {:#x}", e.ordinal, name, *e.rva));
            } else {
                out(std::format("  {:>5}  {}", e.ordinal, name));
            }
        }
    }
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return s;
}

void lib_exports(const fs::path& lib, bool list) {
    auto data = read_file(lib);
    pe::ImportLibrary parsed;
    try {
        parsed = pe::ImportLibrary::parse(data);
    } catch (const std::exception& e) {
        throw Error::context("parsing " + show(lib), e);
    }
    std::map<std::string, std::vector<std::string>> by_dll;
    for (const auto& i : parsed.imports) {
        by_dll[lower(i.dll)].push_back(i.export_.to_string());
    }
    out(std::format("{} exports from {} DLLs ({} other members)", parsed.imports.size(), by_dll.size(), parsed.other_members));
    for (const auto& [dll, names] : by_dll) {
        out(std::format("{:<60} {:>5}", dll, names.size()));
        if (list) {
            for (const auto& n : names) {
                out("    " + n);
            }
        }
    }
}


std::vector<fs::path> default_paths(std::vector<fs::path> paths) {
    if (!paths.empty()) {
        return paths;
    }
    auto cfg = config::Config::load();
    std::vector<fs::path> all;
    if (cfg.game_dir) {
        all.push_back(*cfg.game_dir);
    }
    all.insert(all.end(), cfg.test_titles.begin(), cfg.test_titles.end());
    if (all.empty()) {
        throw Error({"no paths given and none configured (see victory.example.toml)"});
    }
    return all;
}


bool looks_like_pe(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[2];
    return in.read(magic, 2) && magic[0] == 'M' && magic[1] == 'Z';
}


void walk(const fs::path& root, std::vector<fs::path>& files) {
    std::error_code ec;
    auto st = fs::symlink_status(root, ec);
    if (ec) {
        return;
    }
    if (fs::is_regular_file(st)) {
        files.push_back(root);
        return;
    }
    if (!fs::is_directory(st)) {
        return;
    }
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end(), [](const fs::path& a, const fs::path& b) { return a.filename().native() < b.filename().native(); });
    for (const auto& e : entries) {
        walk(e, files);
    }
}

void census(std::vector<fs::path> paths, bool strict, bool list_missing) {
    paths = default_paths(std::move(paths));
    hle::HleResolver resolver(strict ? hle::HostPolicy::none() : hle::HostPolicy::standard());
    struct DllStats {
        std::set<pe::ImportSymbol> functions;
        std::size_t hle = 0, host = 0, missing = 0;
        std::set<fs::path> images;
    };
    std::size_t images = 0;
    std::vector<std::pair<fs::path, std::string>> failed;
    std::map<std::string, DllStats> dlls;
    std::map<std::pair<std::string, std::string>, std::set<fs::path>> missing;
    for (const auto& root : paths) {
        std::vector<fs::path> files;
        walk(root, files);
        for (const auto& path : files) {
            if (!looks_like_pe(path)) {
                continue;
            }
            pe::ImageInfo info;
            try {
                auto data = read_file(path);
                info = pe::ImageInfo::parse(data);
            } catch (const Error& e) {
                failed.emplace_back(path, e.chain().back());
                continue;
            } catch (const std::exception& e) {
                failed.emplace_back(path, e.what());
                continue;
            }
            ++images;
            auto each = [&](const std::vector<pe::ImportModule>& list) {
                for (const auto& module : list) {
                    std::string key = lower(module.dll);
                    auto& stats = dlls[key];
                    stats.images.insert(path);
                    for (const auto& e : module.entries) {
                        auto cls = resolver.classify(module.dll, e.symbol);
                        if (!cls) {
                            missing[{key, e.symbol.to_string()}].insert(path);
                        }

                        if (stats.functions.insert(e.symbol).second) {
                            if (!cls) {
                                ++stats.missing;
                            } else if (*cls == loader::Provenance::Hle) {
                                ++stats.hle;
                            } else {
                                ++stats.host;
                            }
                        }
                    }
                }
            };
            each(info.imports);
            each(info.delay_imports);
        }
    }
    out(std::format("{} images scanned, {} unreadable", images, failed.size()));
    for (const auto& [p, e] : failed) {
        out("  skipped " + show(p) + ": " + e);
    }
    out(std::format("\n{:<56} {:>6} {:>6} {:>6} {:>8} {:>7}", "dll", "funcs", "hle", "host", "missing", "images"));
    std::size_t f = 0, h = 0, o = 0, m = 0;
    for (const auto& [dll, s] : dlls) {
        out(std::format("{:<56} {:>6} {:>6} {:>6} {:>8} {:>7}", dll, s.functions.size(), s.hle, s.host, s.missing, s.images.size()));
        f += s.functions.size();
        h += s.hle;
        o += s.host;
        m += s.missing;
    }
    out(std::format("{:<56} {:>6} {:>6} {:>6} {:>8}", "TOTAL", f, h, o, m));
    if (list_missing) {
        std::vector<std::pair<std::pair<std::string, std::string>, std::size_t>> rows;
        for (const auto& [k, users] : missing) {
            rows.emplace_back(k, users.size());
        }
        std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        out("\nmissing (by number of images using it)");
        for (const auto& [k, n] : rows) {
            out(std::format("  {:>4}  {}!{}", n, k.first, k.second));
        }
    }
}


void show_config() {
    auto cfg = config::Config::load();
    auto status = [](const std::optional<fs::path>& p) -> std::string {
        if (!p) {
            return "(not set)";
        }
        std::error_code ec;
        return fs::exists(*p, ec) ? show(*p) : show(*p) + "  (does not exist)";
    };
    out("config file : " + (cfg.source ? show(*cfg.source) : std::string("(none)")));
    out("game_dir    : " + status(cfg.game_dir));
    out("cache_dir   : " + show(cfg.cache_dir));
    out("data_dir    : " + show(cfg.data_dir));
    for (const auto& t : cfg.test_titles) {
        out("test_title  : " + status(t));
    }
    std::vector<fs::path> all;
    if (cfg.game_dir) {
        all.push_back(*cfg.game_dir);
    }
    all.insert(all.end(), cfg.test_titles.begin(), cfg.test_titles.end());
    all.push_back(cfg.cache_dir);
    all.push_back(cfg.data_dir);
    for (const auto& p : all) {
        try {
            policy::ensure_outside_repo(p);
        } catch (const std::exception& e) {
            out(std::string("POLICY VIOLATION: ") + e.what());
        }
    }
}


enum class Unimplemented { Trap, Zero };
enum class ConsoleArg { SeriesX, SeriesS };


bool title_dll(const hle::HostPolicy& host, const std::string& dll) {
    if (host.allows(dll)) {
        return false;
    }
    for (const auto& m : hle::registry()) {
        if (m.serves(dll)) {
            return false;
        }
    }
    return true;
}


class LoaderLock {
public:
    template <class F>
    auto hold(F f) {
        DWORD me = GetCurrentThreadId();
        {
            std::unique_lock l(lock_);
            free_.wait(l, [&] { return owner_ == 0 || owner_ == me; });
            owner_ = me;
            ++depth_;
        }
        struct Release {
            LoaderLock* self;
            ~Release() {
                std::lock_guard l(self->lock_);
                if (--self->depth_ == 0) {
                    self->owner_ = 0;
                    self->free_.notify_one();
                }
            }
        } release{this};
        return f();
    }

private:
    std::mutex lock_;
    std::condition_variable free_;
    DWORD owner_ = 0;
    std::size_t depth_ = 0;
};

using Show = std::function<void(const std::string&, const loader::BindReport&)>;


struct LateDlls {
    hle::HostPolicy host;
    fs::path title_dir, cache_dir;
    loader::UnimplementedPolicy policy;

    std::mutex mapped_lock;
    loader::TitleDlls mapped;
    LoaderLock lock;


    std::optional<std::uintptr_t> load(const std::string& file, const Show& show) {
        return lock.hold([&]() -> std::optional<std::uintptr_t> {
            if (auto base = hle::runtime::image_named(file)) {
                return base;
            }
            loader::TitleDlls added;
            {
                std::lock_guard l(mapped_lock);
                auto wanted = [&](const std::string& dll) { return title_dll(host, dll) && !hle::runtime::image_named(dll); };
                added = loader::load_title_dll(file, title_dir, cache_dir, wanted);
                hle::HleResolver hle_(host);
                loader::WithTitleDlls old(hle_, mapped);
                loader::WithTitleDlls resolver(old, added);
                for (const auto& [name, dll] : added) {
                    out(std::format("title dll      {} at {:#x} (loaded by the title)", name, dll->base()));
                    show(name, dll->bind_imports(resolver, policy));
                    hle::runtime::set_image_path(dll->base(), title_dir / fs::path(std::u8string(name.begin(), name.end())));
                }

                mapped.insert(mapped.end(), added.begin(), added.end());
            }
            std::size_t i = 0;
            try {
                for (; i < added.size(); ++i) {
                    added[i].second->attach();
                }
            } catch (const std::exception& e) {


                std::string why = "attaching " + added[i].first;
                {
                    std::lock_guard l(mapped_lock);
                    std::erase_if(mapped, [&](const auto& d) {
                        for (const auto& a : added) {
                            if (a.second->base() == d.second->base()) {
                                return true;
                            }
                        }
                        return false;
                    });
                }
                for (std::size_t k = i + 1; k-- > 0;) {
                    added[k].second->detach();
                }
                for (const auto& [name, dll] : added) {
                    hle::runtime::forget_image(dll->base());
                    dll->unload();
                }
                throw Error({why, e.what()});
            }
            if (added.empty()) {
                return std::nullopt;
            }
            return added.back().second->base();
        });
    }
};


Error as_error(const std::exception& e) {
    if (auto* l = dynamic_cast<const loader::LoaderError*>(&e)) {
        return Error(l->chain());
    }
    if (auto* c = dynamic_cast<const Error*>(&e)) {
        return *c;
    }
    return Error({e.what()});
}

int run(const fs::path& exe, Unimplemented unimplemented, bool strict, bool execute, ConsoleArg console) {

    hle::lifecycle::wait_for_previous();
    auto cfg = config::Config::load();
    auto original = read_file(exe);
    loader::PreparedImage prepared;
    try {
        prepared = loader::prepare(original);
    } catch (const std::exception& e) {
        throw as_error(e);
    }
    std::string hash;
    for (auto b : prepared.original_hash) {
        hash += std::format("{:02x}", b);
    }

    out("build          unknown (" + hash + ")");

    auto images = cfg.cache_subdir("images");
    hle::d3d12x_module::use_shader_cache(cfg.cache_subdir("shaders"));
    std::shared_ptr<loader::LoadedImage> image;
    try {
        image = loader::load(prepared, images);
    } catch (const std::exception& e) {
        throw as_error(e);
    }


    prepared = {};
    original = {};
    hle::runtime::set_main_image(image->base());
    hle::runtime::set_image_path(image->base(), exe);
    out(std::format("mapped at      {:#x} (preferred {:#x})", image->base(), image->info().image_base));
    hle::HostPolicy host = strict ? hle::HostPolicy::none() : hle::HostPolicy::standard();
    hle::HleResolver resolver(host);
    auto wanted = [&](const std::string& dll) { return title_dll(host, dll); };
    fs::path title_dir = exe.has_parent_path() ? exe.parent_path() : fs::path(L".");

    std::string manifest;
    {
        std::ifstream in(title_dir / L"MicrosoftGame.config", std::ios::binary);
        if (in) {
            manifest.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    auto title = hle::runtime::Title::from_manifest(manifest);
    auto storage = cfg.data_subdir(std::format("{:08X}/storage", title.id));
    out(std::format("title          {:08X}, storage {} ({} MB)", title.id, show(storage), title.storage_bytes >> 20));

    hle::files::mount_at(L'G', title_dir);
    auto temp = cfg.data_subdir(std::format("{:08X}/temp", title.id));
    hle::files::mount_at(L'T', temp);
    title.storage = storage;
    hle::runtime::set_title(title);
    d3d12x::set_console(console == ConsoleArg::SeriesS ? d3d12x::Console::SeriesS : d3d12x::Console::SeriesX);
    out(std::string("console        ") + (d3d12x::current_console() == d3d12x::Console::SeriesS ? "Xbox Series S" : "Xbox Series X"));
    loader::TitleDlls dlls;
    try {
        dlls = loader::load_title_dlls(image->info(), title_dir, images, wanted);
    } catch (const std::exception& e) {
        throw as_error(e);
    }
    for (const auto& [name, dll] : dlls) {
        out(std::format("title dll      {} at {:#x}", name, dll->base()));
        hle::runtime::set_image_path(dll->base(), title_dir / fs::path(std::u8string(name.begin(), name.end())));
    }
    auto policy = unimplemented == Unimplemented::Trap ? loader::UnimplementedPolicy::Trap : loader::UnimplementedPolicy::ReturnZero;
    loader::WithTitleDlls with_dlls(resolver, dlls);
    Show show_report = [](const std::string& name, const loader::BindReport& report) {
        out(std::format("bound          {}: {} hle, {} host, {} title, {} missing", name, report.hle, report.host, report.title, report.missing.size()));
        for (const auto& m : report.missing) {
            out("  missing      " + m.dll + "!" + m.symbol);
        }
    };
    try {
        for (const auto& [name, dll] : dlls) {
            show_report(name, dll->bind_imports(with_dlls, policy));
        }
        show_report(show(exe.filename()), image->bind_imports(with_dlls, policy));
    } catch (const std::exception& e) {
        throw as_error(e);
    }
    if (!execute) {
        out("not executing (pass --execute to start the title)");
        return 0;
    }
    loader::crash::report_crashes();
    loader::crash::log_crashes_to(temp.parent_path() / L"crashes.log");
    hle::runtime::use_console_timers();
    auto late = std::make_shared<LateDlls>();
    late->host = host;
    late->title_dir = title_dir;
    late->cache_dir = images;
    late->policy = policy;
    late->mapped = dlls;
    hle::runtime::set_title_dll_loader([late, show_report](const std::string& file) -> std::pair<std::optional<std::uintptr_t>, std::optional<std::string>> {
        try {
            return {late->load(file, show_report), std::nullopt};
        } catch (const Error& e) {
            return {std::nullopt, config::display_alternate(e.chain())};
        } catch (const std::exception& e) {
            return {std::nullopt, std::string(e.what())};
        }
    });
    std::uint32_t code;
    try {
        auto main_thread = loader::spawn_main(image, dlls);
        code = main_thread.join();
    } catch (const std::exception& e) {
        throw as_error(e);
    }
    out(std::format("title exited with {}", code));
    return 0;
}


[[noreturn]] void usage_error(const std::string& message) {
    std::fprintf(stderr, "error: %s\n\nFor more information, try '--help'.\n", message.c_str());
    std::exit(2);
}

const char* HELP =
    "Run an Xbox Series title natively on Windows\n"
    "\n"
    "Usage: victory.exe <COMMAND>\n"
    "\n"
    "Commands:\n"
    "  inspect      Summarise one PE image: headers, sections, imports, TLS, build id\n"
    "  census       Aggregate the imports of every PE image under the given paths and show which ones our runtime covers. Defaults to the configured game and test-title directories\n"
    "  lib-exports  List the `dll!export` pairs an import library describes\n"
    "  shader-info  Find shader containers in files or directories (also inside engine archives, if uncompressed) and report their layout\n"
    "  translate    Run every shader found (containers, SGD5 files) through the translator and report what it cannot handle yet\n"
    "  precompile   Translate shaders into the runtime cache ahead of time (resumable). With --pipelines, also compile compute pipelines into the host pipeline library and GPU driver cache\n"
    "  run          Prepare, map and bind a title executable; with --execute, run it\n"
    "  config       Show the resolved configuration and check it\n"
    "  help         Print this message or the help of the given subcommand(s)\n"
    "\n"
    "Options:\n"
    "  -h, --help     Print help\n"
    "  -V, --version  Print version\n";

struct Args {
    std::vector<std::wstring> positional;
    std::set<std::string> flags;
    std::map<std::string, std::wstring> values;
    std::vector<std::wstring> last;
};

Args parse_args(int argc, wchar_t** argv, int from, const std::set<std::string>& flags, const std::set<std::string>& valued) {
    Args a;
    for (int i = from; i < argc; ++i) {
        std::wstring w = argv[i];
        if (w == L"--") {
            for (++i; i < argc; ++i) {
                a.last.emplace_back(argv[i]);
            }
            break;
        }
        if (w.starts_with(L"--")) {
            std::wstring body = w.substr(2);
            std::optional<std::wstring> value;
            if (auto eq = body.find(L'='); eq != std::wstring::npos) {
                value = body.substr(eq + 1);
                body = body.substr(0, eq);
            }
            std::string name = utf8(body);
            if (flags.count(name) && !value) {
                a.flags.insert(name);
            } else if (valued.count(name)) {
                if (!value) {
                    if (i + 1 >= argc) {
                        usage_error("a value is required for '--" + name + " <" + name + ">' but none was supplied");
                    }
                    value = argv[++i];
                }
                a.values[name] = *value;
            } else {
                usage_error("unexpected argument '--" + name + "' found");
            }
        } else {
            a.positional.push_back(w);
        }
    }
    return a;
}

int dispatch(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fputs(HELP, stderr);
        return 2;
    }
    std::wstring command = argv[1];
    if (command == L"-h" || command == L"--help" || command == L"help") {
        std::fputs(HELP, stdout);
        return 0;
    }
    if (command == L"-V" || command == L"--version") {
        out(std::string("victory ") + VERSION);
        return 0;
    }
    if (command == L"inspect") {
        auto a = parse_args(argc, argv, 2, {"imports", "exports"}, {});
        if (a.positional.size() != 1) {
            usage_error("the following required arguments were not provided:\n  <FILE>");
        }
        inspect(arg_path(a.positional[0]), a.flags.count("imports") > 0, a.flags.count("exports") > 0);
        return 0;
    }
    if (command == L"census") {
        auto a = parse_args(argc, argv, 2, {"strict", "missing"}, {});
        std::vector<fs::path> paths;
        for (auto& p : a.positional) {
            paths.push_back(arg_path(p));
        }
        census(paths, a.flags.count("strict") > 0, a.flags.count("missing") > 0);
        return 0;
    }
    if (command == L"lib-exports") {
        auto a = parse_args(argc, argv, 2, {"list"}, {});
        if (a.positional.size() != 1) {
            usage_error("the following required arguments were not provided:\n  <LIB>");
        }
        lib_exports(arg_path(a.positional[0]), a.flags.count("list") > 0);
        return 0;
    }
    if (command == L"config") {
        parse_args(argc, argv, 2, {}, {});
        show_config();
        return 0;
    }
    if (command == L"run") {
        auto a = parse_args(argc, argv, 2, {"strict", "execute"}, {"unimplemented", "console"});
        if (a.positional.size() != 1) {
            usage_error("the following required arguments were not provided:\n  <EXE>");
        }
        Unimplemented u = Unimplemented::Trap;
        if (auto v = a.values.find("unimplemented"); v != a.values.end()) {
            if (v->second == L"trap") {
                u = Unimplemented::Trap;
            } else if (v->second == L"zero") {
                u = Unimplemented::Zero;
            } else {
                usage_error("invalid value '" + utf8(v->second) + "' for '--unimplemented <UNIMPLEMENTED>'\n  [possible values: trap, zero]");
            }
        }
        ConsoleArg c = ConsoleArg::SeriesX;
        if (auto v = a.values.find("console"); v != a.values.end()) {
            if (v->second == L"series-x") {
                c = ConsoleArg::SeriesX;
            } else if (v->second == L"series-s") {
                c = ConsoleArg::SeriesS;
            } else {
                usage_error("invalid value '" + utf8(v->second) + "' for '--console <CONSOLE>'\n  [possible values: series-x, series-s]");
            }
        }
        return run(arg_path(a.positional[0]), u, a.flags.count("strict") > 0, a.flags.count("execute") > 0, c);
    }
    if (command == L"shader-info" || command == L"translate" || command == L"precompile") {

        victory::binary_output();
        std::set<std::string> flags, valued;
        if (command == L"shader-info") {
            flags = {"verbose", "disasm"};
        } else if (command == L"translate") {
            flags = {"validate", "pipelines", "list"};
            valued = {"jobs"};
        } else {
            flags = {"pipelines"};
            valued = {"only", "limit", "jobs", "pipeline-jobs"};
        }
        auto value_name = [](std::string name) {
            for (char& c : name) {
                c = c == '-' ? '_' : static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
            }
            return name;
        };

        for (int i = 2; i < argc; ++i) {
            std::wstring w = argv[i];
            if (w == L"--") {
                break;
            }
            if (w.starts_with(L"--") && valued.count(utf8(w.substr(2))) && i + 1 == argc) {
                std::string name = utf8(w.substr(2));
                usage_error("a value is required for '--" + name + " <" + value_name(name) + ">' but none was supplied");
            }
        }
        auto a = parse_args(argc, argv, 2, flags, valued);
        std::vector<fs::path> paths;
        for (auto& p : a.positional) {
            paths.push_back(arg_path(p));
        }
        for (const auto& w : a.last) {
            paths.push_back(arg_path(w));
        }
        auto flag = [&](const char* name) { return a.flags.count(name) > 0; };
        auto number = [&](const std::string& name) -> std::optional<std::size_t> {
            auto v = a.values.find(name);
            if (v == a.values.end()) {
                return std::nullopt;
            }
            std::string error;
            auto n = victory::parse_usize(utf8(v->second), &error);
            if (!n) {
                usage_error("invalid value '" + utf8(v->second) + "' for '--" + name + " <" + value_name(name) + ">': " + error);
            }
            return n;
        };
        if (command == L"shader-info") {
            victory::shader_info(paths, &default_paths, flag("verbose") || flag("disasm"), flag("disasm"));
        } else if (command == L"translate") {
            auto jobs = number("jobs");
            victory::translate_env();
            victory::translate(paths, &default_paths, flag("validate"), flag("pipelines"), flag("list"), jobs);
        } else {
            std::optional<victory::StageFilter> only;
            if (auto v = a.values.find("only"); v != a.values.end()) {
                const std::pair<const wchar_t*, victory::StageFilter> stages[] = {
                    {L"vs", victory::StageFilter::Vs}, {L"ps", victory::StageFilter::Ps}, {L"ms", victory::StageFilter::Ms}, {L"cs", victory::StageFilter::Cs}};
                for (const auto& [n, s] : stages) {
                    if (v->second == n) {
                        only = s;
                    }
                }
                if (!only) {
                    usage_error("invalid value '" + utf8(v->second) + "' for '--only <ONLY>'\n  [possible values: vs, ps, ms, cs]");
                }
            }
            auto limit = number("limit");
            auto jobs = number("jobs");
            auto pipeline_jobs = number("pipeline-jobs");
            victory::precompile(paths, &default_paths, flag("pipelines"), only, limit, jobs, pipeline_jobs);
        }
        return 0;
    }
    usage_error("unrecognized subcommand '" + utf8(command) + "'");
}

}   

extern "C" __declspec(dllexport) int danzi_launcher_main(int argc, wchar_t** argv) {


    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    log::init("info");
    try {
        return dispatch(argc, argv);
    } catch (const Error& e) {
        std::fprintf(stderr, "Error: %s\n", config::display_chain(e.chain()).c_str());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
