// SPDX-License-Identifier: GPL-3.0-or-later
// XGameSave (`victory-hle/src/modules/xgamesave.rs`; DanziLoader /
// victory-perf, see NOTICE.md): the user's game saves, kept on the host
// under the title's data directory, next to persistent local storage
// (`runtime::Title::saves`), never in the cache. A provider is a folder per
// user and service configuration, a container a folder in it.
//
// An update is all or nothing, as documented: each one writes a complete
// new version of the container (`v<N>.new/`, unchanged blobs hard-linked
// from the previous version), flushed to disk, and one directory rename
// commits it (`v<N>`); the highest committed version is the container. A
// crash leaves the old version or the new one, never a mix; leftovers are
// removed on sight. Deleting a container renames it away first.
//
// Names are exact: a version's `manifest` holds the container's name, its
// display name and the blob names (blob `i` is the file `i`), so case,
// characters the host forbids, reserved device names and length never reach
// a file name. A container's folder is its name made safe (and shortened)
// plus a hash of the exact name. Offline there is no cloud copy, so nothing
// ever needs a sync. Log lines: milestone 7.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "core/log.hpp"
#include "modules.hpp"
#include "pe/image.hpp"
#include "runtime.hpp"
#include "xasync.hpp"

namespace danzi::hle::xgamesave {

namespace {

namespace fs = std::filesystem;
using xasync::AsyncBlock;
using This = const Object*;

const Guid PROVIDER{0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f, 0xe2}};


const Guid IID_2026{0x1bfff3af, 0xf14a, 0x40a3, {0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9}};


constexpr HRESULT_ INVALID_CONTAINER_NAME = static_cast<HRESULT_>(0x80830001u);
constexpr HRESULT_ NO_ACCESS = static_cast<HRESULT_>(0x80830002u);
constexpr HRESULT_ OUT_OF_LOCAL_STORAGE = static_cast<HRESULT_>(0x80830003u);
constexpr HRESULT_ UPDATE_TOO_BIG = static_cast<HRESULT_>(0x80830005u);
constexpr HRESULT_ QUOTA_EXCEEDED = static_cast<HRESULT_>(0x80830006u);
constexpr HRESULT_ BUFFER_TOO_SMALL = static_cast<HRESULT_>(0x80830007u);
constexpr HRESULT_ BLOB_NOT_FOUND = static_cast<HRESULT_>(0x80830008u);


constexpr std::uint64_t QUOTA = 256ull << 20;

constexpr std::size_t MAX_UPDATE = 16u << 20;
constexpr const wchar_t* MANIFEST = L"manifest";

constexpr std::size_t READABLE = 100;


std::mutex SAVES;


std::error_code win_error(DWORD code) { return {static_cast<int>(code), std::system_category()}; }
bool not_found(const std::error_code& e) { return e.value() == ERROR_FILE_NOT_FOUND || e.value() == ERROR_PATH_NOT_FOUND; }

const std::error_code BAD_MANIFEST = win_error(ERROR_INVALID_DATA);

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path, std::error_code& ec) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ec = win_error(GetLastError());
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size)) {
        out.reserve(static_cast<std::size_t>(size.QuadPart));
    }
    std::uint8_t buf[65536];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof buf, &n, nullptr)) {
            ec = win_error(GetLastError());
            CloseHandle(h);
            return std::nullopt;
        }
        if (n == 0) {
            break;
        }
        out.insert(out.end(), buf, buf + n);
    }
    CloseHandle(h);
    return out;
}


bool write_file(const fs::path& path, const std::uint8_t* data, std::size_t len, std::error_code& ec) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ec = win_error(GetLastError());
        return false;
    }
    std::size_t done = 0;
    while (done < len) {
        DWORD n = 0;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(len - done, 1u << 30));
        if (!WriteFile(h, data + done, want, &n, nullptr)) {
            ec = win_error(GetLastError());
            CloseHandle(h);
            return false;
        }
        done += n;
    }
    if (!FlushFileBuffers(h)) {
        ec = win_error(GetLastError());
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    return true;
}

std::string lossy_name(const fs::path& p) { return xgr::utf16_lossy(p.filename().native()); }
bool valid_utf8(const std::string& s) { return pe::utf8_lossy(std::span(reinterpret_cast<const std::uint8_t*>(s.data()), s.size())) == s; }


std::string file_name(const std::string& name) {
    std::string out;
    for (unsigned char b : name) {
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-' || b == '_') {
            out += static_cast<char>(b);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", b);
            out += hex;
        }
    }
    return out;
}

int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}


std::optional<std::string> decode(const std::string& file) {
    if (file.find('.') != std::string::npos) {
        return std::nullopt;
    }
    std::string out;
    for (std::size_t i = 0; i < file.size(); ++i) {
        auto b = static_cast<unsigned char>(file[i]);
        if (b != '%') {
            out += static_cast<char>(b);
            continue;
        }
        if (i + 2 >= file.size()) {
            return std::nullopt;
        }
        auto h = static_cast<unsigned char>(file[i + 1]), l = static_cast<unsigned char>(file[i + 2]);
        i += 2;

        int v;
        if (h == '+') {
            v = hex_digit(l);
        } else {
            int a = hex_digit(h), c = hex_digit(l);
            v = a < 0 || c < 0 ? -1 : a * 16 + c;
        }
        if (v < 0) {
            return std::nullopt;
        }
        out += static_cast<char>(v);
    }
    if (!valid_utf8(out)) {
        return std::nullopt;
    }
    return out;
}


fs::path container_dir(const fs::path& root, const std::string& name) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char b : name) {
        hash = (hash ^ b) * 0x100000001b3ull;
    }
    std::string readable = file_name(name);
    if (readable.size() > READABLE) {
        readable.resize(READABLE);
    }
    char h[20];
    std::snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(hash));
    return root / xgr::utf16(readable + "~" + h);
}

bool alnum(unsigned char b) { return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9'); }


bool valid_container(const std::string& name) {
    auto slash = name.rfind('/');
    std::string path = slash == std::string::npos ? "" : name.substr(0, slash);
    std::string file = slash == std::string::npos ? name : name.substr(slash + 1);
    if (name.size() > 256 || file.empty() || file.back() == '.' || file.find("..") != std::string::npos) {
        return false;
    }
    for (unsigned char b : path) {
        if (!(alnum(b) || b == '_' || b == '/')) {
            return false;
        }
    }
    for (unsigned char b : file) {
        if (!(alnum(b) || b == '_' || b == '.' || b == '-')) {
            return false;
        }
    }
    return true;
}


struct Version {
    fs::path dir;
    std::uint64_t number = 0;
    std::string name, display;

    std::vector<std::string> blobs;

    fs::path blob(std::size_t i) const { return dir / std::to_wstring(i); }
    std::vector<std::pair<std::string, std::uint64_t>> sizes() const {
        std::vector<std::pair<std::string, std::uint64_t>> out;
        for (std::size_t i = 0; i < blobs.size(); ++i) {
            std::error_code ec;
            auto s = fs::file_size(blob(i), ec);
            out.emplace_back(blobs[i], ec ? 0 : s);
        }
        return out;
    }
    std::uint64_t total() const {
        std::uint64_t t = 0;
        for (auto& [n, s] : sizes()) {
            t += s;
        }
        return t;
    }
};


std::vector<std::uint8_t> manifest(const std::string& name, const std::string& display, const std::vector<std::string>& blobs) {
    std::vector<std::uint8_t> out;
    auto add = [&](const std::string& s) {
        auto len = static_cast<std::uint32_t>(s.size());
        out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&len), reinterpret_cast<const std::uint8_t*>(&len) + 4);
        out.insert(out.end(), s.begin(), s.end());
    };
    add(name);
    add(display);
    for (auto& b : blobs) {
        add(b);
    }
    return out;
}

std::optional<std::vector<std::string>> parse_manifest(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < bytes.size()) {
        if (bytes.size() - at < 4) {
            return std::nullopt;
        }
        std::uint32_t len;
        std::memcpy(&len, bytes.data() + at, 4);
        at += 4;
        if (bytes.size() - at < len) {
            return std::nullopt;
        }
        std::string s(reinterpret_cast<const char*>(bytes.data() + at), len);
        if (!valid_utf8(s)) {
            return std::nullopt;
        }
        out.push_back(std::move(s));
        at += len;
    }
    if (out.size() < 2) {
        return std::nullopt;
    }
    return out;
}


std::optional<std::uint64_t> parse_u64(std::string_view s) {
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return std::nullopt;
        }
        v = v * 10 + d;
    }
    return v;
}


std::optional<Version> current(const fs::path& dir, std::error_code& ec) {
    std::vector<std::uint64_t> committed;
    {
        fs::directory_iterator it(dir, ec), end;
        if (ec) {
            return std::nullopt;
        }
        std::vector<std::pair<std::string, fs::path>> entries;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                break;
            }
            entries.emplace_back(lossy_name(it->path()), it->path());
        }
        for (auto& [file, path] : entries) {
            if (file.ends_with(".new")) {
                fs::remove_all(path, ec);
                if (ec) {
                    return std::nullopt;
                }
            } else if (file.starts_with('v')) {
                if (auto n = parse_u64(std::string_view(file).substr(1))) {
                    committed.push_back(*n);
                }
            }
        }
    }
    std::sort(committed.begin(), committed.end());
    if (committed.empty()) {
        return std::nullopt;
    }
    std::uint64_t number = committed.back();
    for (std::size_t i = 0; i + 1 < committed.size(); ++i) {
        fs::remove_all(dir / (L"v" + std::to_wstring(committed[i])), ec);
        if (ec) {
            return std::nullopt;
        }
    }
    fs::path version = dir / (L"v" + std::to_wstring(number));
    auto bytes = read_file(version / MANIFEST, ec);
    if (!bytes) {
        return std::nullopt;
    }
    auto names = parse_manifest(*bytes);
    if (!names) {
        ec = BAD_MANIFEST;
        return std::nullopt;
    }
    Version v;
    v.dir = version;
    v.number = number;
    v.name = (*names)[0];
    v.display = (*names)[1];
    v.blobs.assign(names->begin() + 2, names->end());
    return v;
}


std::optional<Version> current_named(const fs::path& dir, const std::string& name, std::error_code& ec) {
    auto v = current(dir, ec);
    if (ec) {
        if (not_found(ec)) {
            ec.clear();
        }
        return std::nullopt;
    }
    if (v && v->name != name) {
        return std::nullopt;
    }
    return v;
}


struct Source {
    const std::vector<std::uint8_t>* data;   
    fs::path kept;
};


bool commit(const fs::path& dir, std::uint64_t number, const std::string& name, const std::string& display, const std::vector<std::pair<std::string, Source>>& blobs, std::error_code& ec) {
    fs::path staged = dir / (L"v" + std::to_wstring(number) + L".new");
    if (fs::exists(staged, ec)) {
        fs::remove_all(staged, ec);
        if (ec) {
            return false;
        }
    }
    ec.clear();
    fs::create_directories(staged, ec);
    if (ec) {
        return false;
    }
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        fs::path path = staged / std::to_wstring(i);
        const Source& source = blobs[i].second;
        if (source.data) {
            if (!write_file(path, source.data->data(), source.data->size(), ec)) {
                return false;
            }
        } else {

            fs::create_hard_link(source.kept, path, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(source.kept, path, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    return false;
                }
            }
        }
    }
    std::vector<std::string> names;
    for (auto& [n, s] : blobs) {
        names.push_back(n);
    }
    auto m = manifest(name, display, names);
    if (!write_file(staged / MANIFEST, m.data(), m.size(), ec)) {
        return false;
    }
    fs::rename(staged, dir / (L"v" + std::to_wstring(number)), ec);
    return !ec;
}

std::optional<fs::path> migrate(const fs::path& root, const std::string& name, const fs::path& old);


std::vector<std::pair<std::string, fs::path>> containers(const fs::path& root) {
    std::vector<std::pair<std::string, fs::path>> out;
    std::vector<fs::path> entries;
    {
        std::error_code ec;
        fs::directory_iterator it(root, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            entries.push_back(it->path());
        }
    }
    for (auto& path : entries) {
        std::string file = lossy_name(path);
        std::error_code ec;
        if (!fs::is_directory(path, ec)) {
            continue;
        }
        if (file.ends_with(".gone")) {
            fs::remove_all(path, ec);
        } else if (file.find('~') != std::string::npos) {
            if (auto v = current(path, ec); v && !ec) {
                out.emplace_back(v->name, path);
            }
        } else if (auto n = decode(file)) {
            if (auto dir = migrate(root, *n, path)) {
                out.emplace_back(*n, *dir);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first < b.first : a.second.native() < b.second.native(); });
    return out;
}


std::optional<fs::path> migrate(const fs::path& root, const std::string& name, const fs::path& old) {
    fs::path dir = container_dir(root, name);
    std::error_code ec;
    auto done = current_named(dir, name, ec);   
    if (ec) {
        return std::nullopt;
    }
    if (!done) {
        std::string display;
        if (auto d = read_file(old / L".display", ec)) {
            std::string s(d->begin(), d->end());
            if (valid_utf8(s)) {
                display = std::move(s);
            }
        }
        ec.clear();
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> blobs;
        fs::directory_iterator it(old, ec), end;
        if (ec) {
            return std::nullopt;
        }
        for (; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }

            std::wstring w = it->path().filename().native();
            std::string n = xgr::utf16_lossy(w);
            if (xgr::utf16(n) != w) {
                continue;
            }
            auto blob = decode(n);
            if (!blob) {
                continue;
            }
            std::error_code e2;
            if (auto data = read_file(it->path(), e2)) {
                blobs.emplace_back(*blob, std::move(*data));
            }
        }
        std::sort(blobs.begin(), blobs.end());
        std::vector<std::pair<std::string, Source>> sources;
        for (auto& [n, d] : blobs) {
            sources.push_back({n, Source{&d, {}}});
        }
        ec.clear();
        if (!commit(dir, 1, name, display, sources, ec)) {
            return std::nullopt;
        }
    }
    fs::remove_all(old, ec);
    if (ec) {
        return std::nullopt;
    }
    log::info("victory_hle::modules::xgamesave", "game save moved to the versioned layout", {{"container", log::debug(name)}});
    return dir;
}

std::uint64_t used_bytes(const fs::path& root) {
    std::uint64_t total = 0;
    for (auto& [name, dir] : containers(root)) {
        std::error_code ec;
        if (auto v = current_named(dir, name, ec)) {
            total += v->total();
        }
    }
    return total;
}


std::uint64_t remaining(const fs::path& root) {
    std::uint64_t used = used_bytes(root);
    return std::min(used >= QUOTA ? 0 : QUOTA - used, storage::disk_free(root));
}


std::optional<HRESULT_> no_room(std::uint64_t written, std::uint64_t replaced, std::uint64_t used, std::uint64_t free) {
    std::uint64_t left = used >= QUOTA ? 0 : QUOTA - used;
    std::uint64_t room = left + replaced < left ? UINT64_MAX : left + replaced;
    if (written > room) {
        return QUOTA_EXCEEDED;
    }
    if (written > free) {
        return OUT_OF_LOCAL_STORAGE;
    }
    return std::nullopt;
}

HRESULT_ io_error(const fs::path& dir, const std::error_code& e) {
    log::warn("victory_hle::modules::xgamesave", "game save", {{"dir", xgr::utf16_lossy(dir.native())}, {"e", e.message() + " (os error " + std::to_string(e.value()) + ")"}});
    return e.value() == ERROR_DISK_FULL || e.value() == ERROR_HANDLE_DISK_FULL ? OUT_OF_LOCAL_STORAGE : NO_ACCESS;
}

struct Provider {
    fs::path root;
};
struct Container {
    std::string name;
    fs::path dir;
};
struct Update {
    std::string name;
    fs::path dir;
    std::optional<std::string> display;

    std::vector<std::pair<std::string, std::optional<std::vector<std::uint8_t>>>> changes;


    HRESULT_ submit() const {
        std::lock_guard l(SAVES);
        std::error_code ec;
        auto cur = current(dir, ec);
        if (ec) {
            if (!not_found(ec)) {
                return io_error(dir, ec);
            }
            cur.reset();
        }
        if (cur && cur->name != name) {

            return NO_ACCESS;
        }

        std::vector<std::pair<std::string, Source>> blobs;
        if (cur) {
            for (std::size_t i = 0; i < cur->blobs.size(); ++i) {
                blobs.push_back({cur->blobs[i], Source{nullptr, cur->blob(i)}});
            }
        }
        for (auto& [n, data] : changes) {
            std::erase_if(blobs, [&](auto& b) { return b.first == n; });
            if (data) {
                blobs.push_back({n, Source{&*data, {}}});
            }
        }
        std::stable_sort(blobs.begin(), blobs.end(), [](auto& a, auto& b) { return a.first < b.first; });
        std::size_t written = 0;
        for (auto& [n, s] : blobs) {
            written += s.data ? s.data->size() : 0;
        }
        if (written > MAX_UPDATE) {
            return UPDATE_TOO_BIG;
        }
        std::uint64_t replaced = 0;
        if (cur) {
            for (auto& [n, size] : cur->sizes()) {
                for (auto& [c, d] : changes) {
                    if (c == n) {
                        replaced += size;
                        break;
                    }
                }
            }
        }
        fs::path root = dir.has_parent_path() ? dir.parent_path() : dir;
        std::uint64_t free = storage::disk_free(root);
        if (auto hr = no_room(written, replaced, used_bytes(root), free)) {
            return *hr;
        }
        std::string disp = display ? *display : cur ? cur->display : "";
        std::uint64_t number = cur ? cur->number + 1 : 1;
        if (!commit(dir, number, name, disp, blobs, ec)) {
            return io_error(dir, ec);
        }

        if (cur) {
            fs::remove_all(cur->dir, ec);
        }
        return S_OK_;
    }
};


HRESULT_ remove(const fs::path& root, const std::string& name) {
    std::lock_guard l(SAVES);
    fs::path dir = container_dir(root, name);
    std::error_code ec;
    auto v = current_named(dir, name, ec);
    if (ec) {
        return io_error(dir, ec);
    }
    if (!v) {
        return S_OK_;
    }
    fs::path gone = dir;
    gone.replace_extension(L"gone");
    fs::remove_all(gone, ec);
    fs::rename(dir, gone, ec);
    if (ec) {
        return io_error(dir, ec);
    }
    fs::remove_all(gone, ec);
    return S_OK_;
}

using Blobs = std::vector<std::pair<std::string, std::vector<std::uint8_t>>>;


std::variant<Blobs, HRESULT_> read(const Container& c, std::optional<std::vector<std::string>> names) {
    std::lock_guard l(SAVES);
    std::error_code ec;
    auto version = current_named(c.dir, c.name, ec);
    if (ec) {
        return io_error(c.dir, ec);
    }
    if (!version) {
        return BLOB_NOT_FOUND;
    }
    auto wanted = names ? std::move(*names) : version->blobs;
    Blobs out;
    for (auto& n : wanted) {
        auto at = std::find(version->blobs.begin(), version->blobs.end(), n);
        if (at == version->blobs.end()) {
            return BLOB_NOT_FOUND;
        }
        auto data = read_file(version->blob(static_cast<std::size_t>(at - version->blobs.begin())), ec);
        if (!data) {
            return io_error(c.dir, ec);
        }
        out.emplace_back(n, std::move(*data));
    }
    return out;
}


constexpr std::size_t BLOB = 24;


std::vector<std::uint8_t> layout(const Blobs& blobs, std::uintptr_t base) {
    std::vector<std::uint8_t> out(blobs.size() * BLOB);
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        auto& [name, data] = blobs[i];
        std::uint64_t name_at = base + out.size();
        out.insert(out.end(), name.begin(), name.end());
        out.push_back(0);
        std::uint64_t data_at = base + out.size();
        out.insert(out.end(), data.begin(), data.end());
        auto size = static_cast<std::uint32_t>(data.size());
        std::memcpy(out.data() + i * BLOB, &name_at, 8);
        std::memcpy(out.data() + i * BLOB + 8, &size, 4);
        std::memcpy(out.data() + i * BLOB + 16, &data_at, 8);
    }
    return out;
}


std::optional<std::vector<std::string>> names(const char* const* names, std::uint32_t count) {
    if (!names) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(xgr::text(names[i]).value_or(""));
    }
    return out;
}

struct ContainerInfo {
    const char* name;
    const char* display_name;
    std::uint32_t blob_count;
    std::uint64_t total_size;
    std::int64_t last_modified;
    bool needs_sync;
};
struct BlobInfo {
    const char* name;
    std::uint32_t size;
};
using ContainerCallback = bool(__stdcall*)(const ContainerInfo*, void*);
using BlobCallback = bool(__stdcall*)(const BlobInfo*, void*);

std::string cstring(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c != 0) {
            out += c;
        }
    }
    return out;
}


std::int64_t modified(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    std::uint64_t t = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    constexpr std::uint64_t EPOCH = 116444736000000000ull;
    return t < EPOCH ? 0 : static_cast<std::int64_t>((t - EPOCH) / 10000000);
}


template <class Keep>
void report(const fs::path& root, Keep keep, void* context, ContainerCallback callback) {
    struct Info {
        std::string name, display;
        std::uint32_t count;
        std::uint64_t size;
        std::int64_t modified;
    };
    std::vector<Info> infos;
    {
        std::lock_guard l(SAVES);
        for (auto& [name, dir] : containers(root)) {
            if (!keep(name)) {
                continue;
            }
            std::error_code ec;
            auto v = current_named(dir, name, ec);
            if (!v) {
                continue;
            }
            infos.push_back({cstring(v->name), cstring(v->display), static_cast<std::uint32_t>(v->blobs.size()), v->total(), modified(v->dir / MANIFEST)});
        }
    }
    for (auto& i : infos) {
        ContainerInfo info{i.name.c_str(), i.display.c_str(), i.count, i.size, i.modified, false};
        if (!callback(&info, context)) {
            break;
        }
    }
}

std::uint8_t INIT = 0, QUOTA_CALL = 0, DELETE_CALL = 0, READ = 0, SUBMIT = 0;

HRESULT_ __stdcall initialize_async(This, const void*, const char* config, bool, AsyncBlock* block) {
    fs::path saves = runtime::title().saves();
    xasync::Result result = NO_ACCESS;
    if (!saves.parent_path().empty()) {


        auto c = xgr::text(config);
        std::string cfg = c && !c->empty() ? *c : "default";
        char xuid[20];
        std::snprintf(xuid, sizeof xuid, "%016llX", static_cast<unsigned long long>(xuser::XUID));
        fs::path root = saves / xuid / xgr::utf16(file_name(cfg));
        std::error_code ec;
        fs::create_directories(root, ec);
        if (!ec) {
            {
                std::lock_guard l(SAVES);
                containers(root);   
            }
            result = xasync::bytes(new Provider{root});
        }
    }
    return xasync::finished_with(block, &INIT, std::move(result));
}
HRESULT_ __stdcall initialize_result(This, AsyncBlock* block, Provider** out) { return xasync::result(block, &INIT, out); }
void __stdcall close_provider(This, Provider* provider) { delete provider; }

HRESULT_ __stdcall quota_async(This, const Provider* provider, AsyncBlock* block) {
    xasync::Result result = E_INVALIDARG_;
    if (provider) {
        std::uint64_t left;
        {
            std::lock_guard l(SAVES);
            left = remaining(provider->root);
        }
        result = xasync::bytes(static_cast<std::int64_t>(left));
    }
    return xasync::finished_with(block, &QUOTA_CALL, std::move(result));
}
HRESULT_ __stdcall quota_result(This, AsyncBlock* block, std::int64_t* out) { return xasync::result(block, &QUOTA_CALL, out); }

HRESULT_ __stdcall delete_async(This, const Provider* provider, const char* name, AsyncBlock* block) {
    auto n = xgr::text(name);
    HRESULT_ hr = !provider || !n ? E_INVALIDARG_ : valid_container(*n) ? remove(provider->root, *n) : INVALID_CONTAINER_NAME;
    return xasync::finished(block, &DELETE_CALL, hr);
}
HRESULT_ __stdcall delete_result(This, AsyncBlock* block) { return xasync::XAsyncGetResult(block, &DELETE_CALL, 0, nullptr, nullptr); }


HRESULT_ __stdcall container_info(This, const Provider* provider, const char* name, void* context, ContainerCallback callback) {
    auto n = xgr::text(name);
    if (!provider || !n || !callback) {
        return E_INVALIDARG_;
    }
    if (!valid_container(*n)) {
        return INVALID_CONTAINER_NAME;
    }
    report(provider->root, [&](const std::string& c) { return c == *n; }, context, callback);
    return S_OK_;
}

HRESULT_ __stdcall enumerate_containers(This, const Provider* provider, void* context, ContainerCallback callback) {
    if (!provider || !callback) {
        return E_INVALIDARG_;
    }
    report(provider->root, [](const std::string&) { return true; }, context, callback);
    return S_OK_;
}


HRESULT_ __stdcall create_container(This, const Provider* provider, const char* name, Container** out) {
    auto n = xgr::text(name);
    if (!provider || !n) {
        return E_INVALIDARG_;
    }
    if (!valid_container(*n)) {
        return INVALID_CONTAINER_NAME;
    }
    if (!out) {
        return E_INVALIDARG_;
    }
    *out = new Container{*n, container_dir(provider->root, *n)};
    return S_OK_;
}
void __stdcall close_container(This, Container* container) { delete container; }

HRESULT_ __stdcall enumerate_blobs(This, const Container* c, const char* prefix, void* context, BlobCallback callback) {
    if (!c || !callback) {
        return E_INVALIDARG_;
    }
    std::string p = xgr::text(prefix).value_or("");
    std::vector<std::pair<std::string, std::uint64_t>> blobs;
    {
        std::lock_guard l(SAVES);
        std::error_code ec;
        if (auto v = current_named(c->dir, c->name, ec)) {
            blobs = v->sizes();
        }
    }
    for (auto& [name, size] : blobs) {
        if (!name.starts_with(p)) {
            continue;
        }
        std::string n = cstring(name);
        BlobInfo info{n.c_str(), static_cast<std::uint32_t>(size)};
        if (!callback(&info, context)) {
            break;
        }
    }
    return S_OK_;
}

HRESULT_ __stdcall read_blobs(This, const Container* c, const char* const* blob_names, std::uint32_t* count, std::size_t size, std::uint8_t* out) {
    if (!c) {
        return E_INVALIDARG_;
    }
    std::uint32_t wanted = count ? *count : 0;
    auto r = read(*c, names(blob_names, wanted));
    if (auto* hr = std::get_if<HRESULT_>(&r)) {
        return *hr;
    }
    auto& blobs = std::get<Blobs>(r);
    auto bytes = layout(blobs, reinterpret_cast<std::uintptr_t>(out));
    if (!out || size < bytes.size()) {
        return BUFFER_TOO_SMALL;
    }
    std::memcpy(out, bytes.data(), bytes.size());
    return xgr::put(count, static_cast<std::uint32_t>(blobs.size()));
}


HRESULT_ __stdcall read_async(This, const Container* c, const char* const* blob_names, std::uint32_t count, AsyncBlock* block) {
    xasync::Result result = E_INVALIDARG_;
    if (c) {
        auto r = read(*c, names(blob_names, count));
        if (auto* hr = std::get_if<HRESULT_>(&r)) {
            result = *hr;
        } else {
            result = layout(std::get<Blobs>(r), 0);
        }
    }
    return xasync::finished_with(block, &READ, std::move(result));
}
HRESULT_ __stdcall read_result(This, AsyncBlock* block, std::size_t size, std::uint8_t* out, std::uint32_t* count) {
    std::size_t used = 0;
    HRESULT_ hr = xasync::XAsyncGetResult(block, &READ, size, out, &used);


    if (hr == E_NOT_SUFFICIENT_BUFFER_) {
        return BUFFER_TOO_SMALL;
    }
    if (hr < 0) {
        return hr;
    }

    std::size_t blobs = 0;
    if (used != 0) {
        std::uint64_t first;
        std::memcpy(&first, out, 8);
        blobs = static_cast<std::size_t>(first / BLOB);
    }
    for (std::size_t i = 0; i < blobs; ++i) {
        for (std::size_t field : {0, 16}) {
            std::uint64_t p;
            std::memcpy(&p, out + i * BLOB + field, 8);
            p += reinterpret_cast<std::uintptr_t>(out);
            std::memcpy(out + i * BLOB + field, &p, 8);
        }
    }
    return xgr::put(count, static_cast<std::uint32_t>(blobs));
}

HRESULT_ __stdcall create_update(This, const Container* c, const char* display, Update** out) {
    if (!c) {
        return E_INVALIDARG_;
    }
    if (!out) {
        return E_INVALIDARG_;
    }
    *out = new Update{c->name, c->dir, xgr::text(display), {}};
    return S_OK_;
}
void __stdcall close_update(This, Update* update) { delete update; }

HRESULT_ __stdcall write_blob(This, Update* update, const char* name, const std::uint8_t* data, std::size_t size) {
    auto n = xgr::text(name);
    if (!update || !n) {
        return E_INVALIDARG_;
    }
    if (n->empty() || (!data && size > 0)) {
        return E_INVALIDARG_;
    }
    if (size > MAX_UPDATE) {
        return UPDATE_TOO_BIG;
    }
    std::vector<std::uint8_t> bytes;
    if (size) {
        bytes.assign(data, data + size);
    }
    update->changes.emplace_back(*n, std::move(bytes));
    return S_OK_;
}

HRESULT_ __stdcall delete_blob(This, Update* update, const char* name) {
    auto n = xgr::text(name);
    if (!update || !n || n->empty()) {
        return E_INVALIDARG_;
    }
    update->changes.emplace_back(*n, std::nullopt);
    return S_OK_;
}

HRESULT_ __stdcall submit_async(This, const Update* update, AsyncBlock* block) { return xasync::finished(block, &SUBMIT, update ? update->submit() : E_INVALIDARG_); }
HRESULT_ __stdcall submit_result(This, AsyncBlock* block) { return xasync::XAsyncGetResult(block, &SUBMIT, 0, nullptr, nullptr); }

#define F(x) reinterpret_cast<const void*>(&x)
using xgr::add_ref;
using xgr::missing;
const void* const VTABLE[30] = {
    F(missing<0>),
    F(add_ref),
    F(add_ref),
    F(missing<3>),             
    F(initialize_async),       
    F(initialize_result),      
    F(close_provider),         
    F(missing<7>),             
    F(quota_async),            
    F(quota_result),           
    F(missing<10>),            
    F(delete_async),           
    F(delete_result),          
    F(container_info),         
    F(enumerate_containers),   
    F(missing<15>),            
    F(create_container),       
    F(close_container),        
    F(missing<18>),            
    F(enumerate_blobs),        
    F(read_blobs),             
    F(read_async),             
    F(read_result),            
    F(create_update),          
    F(close_update),           
    F(write_blob),             
    F(delete_blob),            
    F(missing<27>),            
    F(submit_async),           
    F(submit_result),          
};
#undef F
const Object OBJECT{VTABLE, "game save"};

}   

std::vector<Interface> interfaces() { return {{PROVIDER, PROVIDER, &OBJECT}, {PROVIDER, IID_2026, &OBJECT}}; }

}   
