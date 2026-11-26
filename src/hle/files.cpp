// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual drives over host folders (`victory-hle/src/modules/files.rs`;
// DanziLoader / victory-perf, see NOTICE.md): on the console, title storage
// is a drive of its own, and titles build paths on that assumption.
//
// Every title import that takes a path is bound here. A path on a virtual
// drive (`X:\..`, `X:/..`, `X:`, `\\?\X:\..`) is rewritten to the host
// folder behind it and handed to the host's own function; any other path
// goes to the host unchanged. A rewritten narrow path goes to the host's
// wide twin, so host folders outside the ANSI code page work; never to our
// wide wrapper, which would rewrite the host path a second time when its
// drive letter is mounted too. Letters are chosen among those the host does
// not use, so no host drive is hidden. Log lines: milestone 7.
#include <cstring>
#include <mutex>

#include "core/log.hpp"
#include "core/watch.hpp"
#include "d3d12x/host_heap.hpp"
#include "modules.hpp"
#include "registry.hpp"

namespace danzi::hle {

void kernel_will_write(std::uintptr_t at, std::size_t len) {

    DWORD error = GetLastError();
    watch::writing(at, len);

    d3d12x::remapped(at, len);
    SetLastError(error);
}

namespace files {

namespace {


std::mutex DRIVES_LOCK;
std::vector<std::pair<wchar_t, std::wstring>> DRIVES;

bool separator(wchar_t c) { return c == L'\\' || c == L'/'; }
constexpr std::wstring_view VERBATIM = L"\\\\?\\";


constexpr std::size_t LONG = 248;


std::wstring root_wide(const std::filesystem::path& root) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(root, ec);
    std::wstring wide = ec ? root.wstring() : abs.wstring();
    while (wide.size() > 3 && separator(wide.back())) {
        wide.pop_back();
    }
    return wide;
}

}   

std::optional<wchar_t> mount(const std::filesystem::path& root) {
    std::wstring wide = root_wide(root);
    std::lock_guard l(DRIVES_LOCK);
    for (auto& [letter, r] : DRIVES) {
        if (r == wide) {
            return letter;
        }
    }
    DWORD host = GetLogicalDrives();
    for (wchar_t l = L'Z'; l >= L'D'; --l) {
        bool used = (host >> (l - L'A')) & 1;
        for (auto& [m, r] : DRIVES) {
            used = used || m == l;
        }
        if (!used) {
            log::info("victory_hle::modules::files", "virtual drive", {{"drive", std::string(1, static_cast<char>(l))}, {"root", xgr::utf16_lossy(root.native())}});
            DRIVES.emplace_back(l, std::move(wide));
            return l;
        }
    }
    return std::nullopt;
}

void mount_at(wchar_t letter, const std::filesystem::path& root) {
    if (letter >= L'a' && letter <= L'z') {
        letter = static_cast<wchar_t>(letter - 32);
    }
    if (letter < L'A' || letter > L'Z') {
        return;
    }
    bool hides_host_drive = (GetLogicalDrives() >> (letter - L'A')) & 1;
    log::info("victory_hle::modules::files", "virtual drive",
              {{"drive", std::string(1, static_cast<char>(letter))}, {"root", xgr::utf16_lossy(root.native())}, {"hides_host_drive", log::debug(hides_host_drive)}});
    std::wstring wide = root_wide(root);
    std::lock_guard l(DRIVES_LOCK);
    std::erase_if(DRIVES, [&](auto& d) { return d.first == letter; });
    DRIVES.emplace_back(letter, std::move(wide));
}

std::wstring verbatim(std::wstring_view root, std::wstring_view tail) {
    std::wstring out;
    if (root.starts_with(VERBATIM)) {
        out = root;
    } else if (root.starts_with(L"\\\\")) {
        out = VERBATIM;
        out += L"UNC\\";
        out += root.substr(2);
    } else {
        out = VERBATIM;
        out += root;
    }
    while (!out.empty() && out.back() == L'\\') {
        out.pop_back();
    }
    std::vector<std::wstring_view> parts;
    std::size_t i = 0;
    for (;;) {
        std::size_t j = i;
        while (j < tail.size() && !separator(tail[j])) {
            ++j;
        }
        auto part = tail.substr(i, j - i);
        if (part.empty() || part == L".") {
        } else if (part == L"..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else {
            parts.push_back(part);
        }
        if (j >= tail.size()) {
            break;
        }
        i = j + 1;
    }
    for (auto p : parts) {
        out += L'\\';
        out += p;
    }
    return out;
}

std::optional<std::wstring> translate(std::wstring_view path) {
    std::wstring_view prefix, rest = path;
    if (path.starts_with(VERBATIM)) {
        prefix = VERBATIM;
        rest = path.substr(VERBATIM.size());
    }
    if (rest.size() < 2 || rest[1] != L':') {
        return std::nullopt;
    }
    wchar_t letter = rest[0];
    if (letter >= L'a' && letter <= L'z') {
        letter = static_cast<wchar_t>(letter - 32);
    } else if (!(letter >= L'A' && letter <= L'Z')) {
        return std::nullopt;
    }
    std::lock_guard l(DRIVES_LOCK);
    const std::wstring* root = nullptr;
    for (auto& [m, r] : DRIVES) {
        if (m == letter) {
            root = &r;
            break;
        }
    }
    if (!root) {
        return std::nullopt;
    }
    auto tail = rest.substr(2);
    if (prefix.empty() && root->size() + tail.size() >= LONG) {
        std::wstring out = verbatim(*root, tail);
        out.push_back(L'\0');
        return out;
    }
    std::wstring out(prefix);
    out += *root;

    if (tail.empty() || !separator(tail.front())) {
        out += L'\\';
    }
    bool verbatim_form = !prefix.empty();
    for (wchar_t c : tail) {
        out += verbatim_form && c == L'/' ? L'\\' : c;
    }
    out.push_back(L'\0');
    return out;
}

namespace {


struct Wide {
    const wchar_t* title;
    std::optional<std::wstring> host;
    const wchar_t* ptr() const { return host ? host->c_str() : title; }
};

Wide wide(const wchar_t* path) {
    if (!path) {
        return {path, std::nullopt};
    }
    return {path, translate(std::wstring_view(path))};
}


std::optional<std::wstring> narrow(const char* path) {
    if (!path) {
        return std::nullopt;
    }
    std::string_view bytes(path);
    std::string_view rest = bytes.starts_with("\\\\?\\") ? bytes.substr(4) : bytes;

    if (rest.size() < 2 || rest[1] != ':' || !((rest[0] >= 'a' && rest[0] <= 'z') || (rest[0] >= 'A' && rest[0] <= 'Z'))) {
        return std::nullopt;
    }
    if (bytes.size() > 0x7fffffff) {
        return std::nullopt;
    }
    int len = static_cast<int>(bytes.size());
    int n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), len, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n < 0 ? 0 : n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), len, w.data(), n);
    return translate(w);
}


std::wstring widen(const char* mode) {
    std::wstring w;
    if (mode) {
        for (const char* p = mode; *p; ++p) {
            w += static_cast<wchar_t>(static_cast<unsigned char>(*p));
        }
    }
    return w;
}

using Handle = HANDLE;
using File = void*;
#define K32(name, type) HOST("kernel32.dll", name, type)
#define UCRT(name, type) HOST("ucrtbase.dll", name, type)

}   


HANDLE WINAPI CreateFileW(const wchar_t* path, DWORD access, DWORD share, const void* security, DWORD disposition, DWORD flags, Handle tmpl) {
    auto p = wide(path);
    return K32("CreateFileW", Handle(WINAPI*)(const wchar_t*, DWORD, DWORD, const void*, DWORD, DWORD, Handle))(p.ptr(), access, share, security, disposition, flags, tmpl);
}

HANDLE WINAPI CreateFileA(const char* path, DWORD access, DWORD share, const void* security, DWORD disposition, DWORD flags, Handle tmpl) {
    if (auto p = narrow(path)) {
        return K32("CreateFileW", Handle(WINAPI*)(const wchar_t*, DWORD, DWORD, const void*, DWORD, DWORD, Handle))(p->c_str(), access, share, security, disposition, flags, tmpl);
    }
    return K32("CreateFileA", Handle(WINAPI*)(const char*, DWORD, DWORD, const void*, DWORD, DWORD, Handle))(path, access, share, security, disposition, flags, tmpl);
}


HANDLE WINAPI CreateFile2(const wchar_t* path, DWORD access, DWORD share, DWORD disposition, const void* params) {
    auto p = wide(path);
    return K32("CreateFile2", Handle(WINAPI*)(const wchar_t*, DWORD, DWORD, DWORD, const void*))(p.ptr(), access, share, disposition, params);
}

BOOL WINAPI DeleteFileA(const char* path) {
    if (auto p = narrow(path)) {
        return K32("DeleteFileW", BOOL(WINAPI*)(const wchar_t*))(p->c_str());
    }
    return K32("DeleteFileA", BOOL(WINAPI*)(const char*))(path);
}

BOOL WINAPI DeleteFileW(const wchar_t* path) {
    auto p = wide(path);
    return K32("DeleteFileW", BOOL(WINAPI*)(const wchar_t*))(p.ptr());
}

BOOL WINAPI RemoveDirectoryW(const wchar_t* path) {
    auto p = wide(path);
    return K32("RemoveDirectoryW", BOOL(WINAPI*)(const wchar_t*))(p.ptr());
}

BOOL WINAPI SetFileAttributesW(const wchar_t* path, DWORD attributes) {
    auto p = wide(path);
    return K32("SetFileAttributesW", BOOL(WINAPI*)(const wchar_t*, DWORD))(p.ptr(), attributes);
}

DWORD WINAPI GetFileAttributesA(const char* path) {
    if (auto p = narrow(path)) {
        return K32("GetFileAttributesW", DWORD(WINAPI*)(const wchar_t*))(p->c_str());
    }
    return K32("GetFileAttributesA", DWORD(WINAPI*)(const char*))(path);
}

BOOL WINAPI CreateDirectoryW(const wchar_t* path, const void* security) {
    auto p = wide(path);
    return K32("CreateDirectoryW", BOOL(WINAPI*)(const wchar_t*, const void*))(p.ptr(), security);
}

BOOL WINAPI GetFileAttributesExW(const wchar_t* path, int level, void* info) {
    auto p = wide(path);
    return K32("GetFileAttributesExW", BOOL(WINAPI*)(const wchar_t*, int, void*))(p.ptr(), level, info);
}

DWORD WINAPI GetFileAttributesW(const wchar_t* path) {
    auto p = wide(path);
    return K32("GetFileAttributesW", DWORD(WINAPI*)(const wchar_t*))(p.ptr());
}

BOOL WINAPI GetDiskFreeSpaceExW(const wchar_t* path, std::uint64_t* available, std::uint64_t* total, std::uint64_t* free) {
    auto p = wide(path);
    return K32("GetDiskFreeSpaceExW", BOOL(WINAPI*)(const wchar_t*, std::uint64_t*, std::uint64_t*, std::uint64_t*))(p.ptr(), available, total, free);
}


BOOL WINAPI GetDiskFreeSpaceW(const wchar_t* root, DWORD* sectors_per_cluster, DWORD* bytes_per_sector, DWORD* free_clusters, DWORD* clusters) {
    auto p = wide(root);
    if (p.host && p.host->size() >= 3 && (*p.host)[1] == L':' && separator((*p.host)[2])) {
        *p.host = std::wstring{(*p.host)[0], (*p.host)[1], (*p.host)[2], L'\0'};
    }
    return K32("GetDiskFreeSpaceW", BOOL(WINAPI*)(const wchar_t*, DWORD*, DWORD*, DWORD*, DWORD*))(p.ptr(), sectors_per_cluster, bytes_per_sector, free_clusters, clusters);
}

HANDLE WINAPI FindFirstFileW(const wchar_t* path, void* data) {
    auto p = wide(path);
    return K32("FindFirstFileW", Handle(WINAPI*)(const wchar_t*, void*))(p.ptr(), data);
}

HANDLE WINAPI FindFirstFileExW(const wchar_t* path, int level, void* data, int search, void* filter, DWORD flags) {
    auto p = wide(path);
    return K32("FindFirstFileExW", Handle(WINAPI*)(const wchar_t*, int, void*, int, void*, DWORD))(p.ptr(), level, data, search, filter, flags);
}

BOOL WINAPI MoveFileExW(const wchar_t* from, const wchar_t* to, DWORD flags) {
    auto f = wide(from);
    auto t = wide(to);
    return K32("MoveFileExW", BOOL(WINAPI*)(const wchar_t*, const wchar_t*, DWORD))(f.ptr(), t.ptr(), flags);
}

BOOL WINAPI SetCurrentDirectoryA(const char* path) {
    if (auto p = narrow(path)) {
        return K32("SetCurrentDirectoryW", BOOL(WINAPI*)(const wchar_t*))(p->c_str());
    }
    return K32("SetCurrentDirectoryA", BOOL(WINAPI*)(const char*))(path);
}


void* __cdecl wfopen(const wchar_t* path, const wchar_t* mode) {
    auto p = wide(path);
    return UCRT("_wfopen", File(__cdecl*)(const wchar_t*, const wchar_t*))(p.ptr(), mode);
}

int __cdecl wfopen_s(void** out, const wchar_t* path, const wchar_t* mode) {
    auto p = wide(path);
    return UCRT("_wfopen_s", int(__cdecl*)(File*, const wchar_t*, const wchar_t*))(out, p.ptr(), mode);
}

void* __cdecl wfsopen(const wchar_t* path, const wchar_t* mode, int share) {
    auto p = wide(path);
    return UCRT("_wfsopen", File(__cdecl*)(const wchar_t*, const wchar_t*, int))(p.ptr(), mode, share);
}


BOOL WINAPI ReadFile(HANDLE file, std::uint8_t* buffer, DWORD len, DWORD* read, void* overlapped) {
    kernel_will_write(reinterpret_cast<std::uintptr_t>(buffer), len);
    return K32("ReadFile", BOOL(WINAPI*)(HANDLE, std::uint8_t*, DWORD, DWORD*, void*))(file, buffer, len, read, overlapped);
}


std::size_t __cdecl fread(std::uint8_t* buffer, std::size_t size, std::size_t count, void* stream) {
    auto read = UCRT("fread", std::size_t(__cdecl*)(std::uint8_t*, std::size_t, std::size_t, File));
    std::size_t total = count != 0 && size > SIZE_MAX / count ? SIZE_MAX : size * count;
    if (!buffer || total == 0) {

        return read(buffer, size, count, stream);
    }
    constexpr std::size_t CHUNK = 1 << 20;
    std::vector<std::uint8_t> beside(std::min(total, CHUNK));
    std::size_t done = 0;
    while (done < total) {
        std::size_t want = std::min(total - done, CHUNK);
        std::size_t n = read(beside.data(), 1, want, stream);
        if (n > 0) {
            kernel_will_write(reinterpret_cast<std::uintptr_t>(buffer) + done, n);
            std::memcpy(buffer + done, beside.data(), n);
            done += n;
        }
        if (n < want) {
            break;
        }
    }
    return done / size;
}

void* __cdecl fopen(const char* path, const char* mode) {
    if (auto p = narrow(path)) {
        return UCRT("_wfopen", File(__cdecl*)(const wchar_t*, const wchar_t*))(p->c_str(), widen(mode).c_str());
    }
    return UCRT("fopen", File(__cdecl*)(const char*, const char*))(path, mode);
}

int __cdecl fopen_s(void** out, const char* path, const char* mode) {
    if (auto p = narrow(path)) {
        return UCRT("_wfopen_s", int(__cdecl*)(File*, const wchar_t*, const wchar_t*))(out, p->c_str(), widen(mode).c_str());
    }
    return UCRT("fopen_s", int(__cdecl*)(File*, const char*, const char*))(out, path, mode);
}


int __cdecl open(const char* path, int flags, int mode) {
    if (auto p = narrow(path)) {
        return UCRT("_wopen", int(__cdecl*)(const wchar_t*, int, int))(p->c_str(), flags, mode);
    }
    return UCRT("_open", int(__cdecl*)(const char*, int, int))(path, flags, mode);
}


int __cdecl wopen(const wchar_t* path, int flags, int mode) {
    auto p = wide(path);
    return UCRT("_wopen", int(__cdecl*)(const wchar_t*, int, int))(p.ptr(), flags, mode);
}

int __cdecl wstat64(const wchar_t* path, void* stat) {
    auto p = wide(path);
    return UCRT("_wstat64", int(__cdecl*)(const wchar_t*, void*))(p.ptr(), stat);
}

int __cdecl stat64i32(const char* path, void* stat) {
    if (auto p = narrow(path)) {
        return UCRT("_wstat64i32", int(__cdecl*)(const wchar_t*, void*))(p->c_str(), stat);
    }
    return UCRT("_stat64i32", int(__cdecl*)(const char*, void*))(path, stat);
}

int __cdecl remove(const char* path) {
    if (auto p = narrow(path)) {
        return UCRT("_wremove", int(__cdecl*)(const wchar_t*))(p->c_str());
    }
    return UCRT("remove", int(__cdecl*)(const char*))(path);
}

int __cdecl wchmod(const wchar_t* path, int mode) {
    auto p = wide(path);
    return UCRT("_wchmod", int(__cdecl*)(const wchar_t*, int))(p.ptr(), mode);
}

int __cdecl wutime64(const wchar_t* path, void* times) {
    auto p = wide(path);
    return UCRT("_wutime64", int(__cdecl*)(const wchar_t*, void*))(p.ptr(), times);
}


#define FIOPEN_NARROW "?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z"
#define FIOPEN_WIDE "?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z"

void* __cdecl fiopen_wide(const wchar_t* path, int mode, int protection) {
    auto p = wide(path);
    return HOST("msvcp140.dll", FIOPEN_WIDE, File(__cdecl*)(const wchar_t*, int, int))(p.ptr(), mode, protection);
}

void* __cdecl fiopen(const char* path, int mode, int protection) {
    if (auto p = narrow(path)) {
        return HOST("msvcp140.dll", FIOPEN_WIDE, File(__cdecl*)(const wchar_t*, int, int))(p->c_str(), mode, protection);
    }
    return HOST("msvcp140.dll", FIOPEN_NARROW, File(__cdecl*)(const char*, int, int))(path, mode, protection);
}

#undef K32
#undef UCRT

#define E(name, f) HleExport{name, reinterpret_cast<std::uintptr_t>(&f)}
std::vector<HleExport> exports() {
    return {E("ReadFile", ReadFile),           E("fread", fread),
            E("CreateFileW", CreateFileW),     E("CreateFileA", CreateFileA),
            E("CreateFile2", CreateFile2),     E("DeleteFileA", DeleteFileA),
            E("DeleteFileW", DeleteFileW),     E("RemoveDirectoryW", RemoveDirectoryW),
            E("SetFileAttributesW", SetFileAttributesW), E("GetFileAttributesA", GetFileAttributesA),
            E("CreateDirectoryW", CreateDirectoryW),     E("GetFileAttributesW", GetFileAttributesW),
            E("GetFileAttributesExW", GetFileAttributesExW), E("GetDiskFreeSpaceExW", GetDiskFreeSpaceExW),
            E("GetDiskFreeSpaceW", GetDiskFreeSpaceW), E("FindFirstFileW", FindFirstFileW),
            E("FindFirstFileExW", FindFirstFileExW), E("MoveFileExW", MoveFileExW),
            E("SetCurrentDirectoryA", SetCurrentDirectoryA), E("_wfopen", wfopen),
            E("_wfopen_s", wfopen_s),           E("_wfsopen", wfsopen),
            E("fopen", fopen),                 E("fopen_s", fopen_s),
            E("_open", open),                  E("_wopen", wopen),
            E("_wstat64", wstat64),            E("_stat64i32", stat64i32),
            E("remove", remove),               E("_wchmod", wchmod),
            E("_wutime64", wutime64),          E(FIOPEN_NARROW, fiopen),
            E(FIOPEN_WIDE, fiopen_wide)};
}
#undef E

}   
}   
