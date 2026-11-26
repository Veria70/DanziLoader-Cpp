// SPDX-License-Identifier: GPL-3.0-or-later
// The DXIL validator in `dxil.dll` (`crates/victory-shader/src/emit/
// validator.rs`; DanziLoader / victory-perf, see NOTICE.md). See
// validator.hpp.
#include "validator.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <system_error>

#include "core/rust_text.hpp"

namespace danzi::shader::emit::validator {

namespace {

using Hresult = std::int32_t;

const GUID CLSID_DXC_VALIDATOR = {0x8ca3e215, 0xf728, 0x4cf3, {0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1}};
const GUID IID_IDXC_VALIDATOR = {0xa6e82bd2, 0x1fd7, 0x4826, {0x98, 0x11, 0x28, 0x57, 0xe7, 0x97, 0xf4, 0x9a}};

constexpr std::uint32_t IN_PLACE_EDIT = 1;

using CreateInstance = Hresult(__stdcall*)(const GUID*, const GUID*, void**);


using Object = void* const**;

template <class F>
F method(void* obj, std::size_t slot) {
    return reinterpret_cast<F>((*static_cast<Object>(obj))[slot]);
}

void release(void* obj) {
    if (obj) {

        method<std::uint32_t(__stdcall*)(void*)>(obj, 2)(obj);
    }
}


struct Blob;
struct BlobVtable {
    Hresult(__stdcall* qi)(Blob*, const std::uint8_t (*)[16], void**);
    std::uint32_t(__stdcall* add_ref)(Blob*);
    std::uint32_t(__stdcall* release)(Blob*);
    std::uint8_t*(__stdcall* ptr)(Blob*);
    std::size_t(__stdcall* len)(Blob*);
};
struct Blob {
    const BlobVtable* vtable;
    std::uint8_t* data;
    std::size_t len;
};


constexpr std::uint8_t IUNKNOWN[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
constexpr std::uint8_t IDXC_BLOB[16] = {0x08, 0xfb, 0xa5, 0x8b, 0x95, 0x51, 0xe2, 0x40, 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02};

Hresult __stdcall blob_qi(Blob* self, const std::uint8_t (*iid)[16], void** out) {
    if (std::memcmp(*iid, IUNKNOWN, 16) == 0 || std::memcmp(*iid, IDXC_BLOB, 16) == 0) {
        *out = self;
        return 0;
    }
    *out = nullptr;
    return static_cast<Hresult>(0x8000'4002u);   
}
std::uint32_t __stdcall blob_ref(Blob*) { return 1; }
std::uint8_t* __stdcall blob_ptr(Blob* self) { return self->data; }
std::size_t __stdcall blob_len(Blob* self) { return self->len; }

const BlobVtable BLOB_VTABLE = {blob_qi, blob_ref, blob_ref, blob_ptr, blob_len};


std::string hex(Hresult v) {
    char t[16];
    std::snprintf(t, sizeof t, "%x", static_cast<unsigned>(v));
    return std::string("0x") + t;
}

std::string utf8(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}


std::string from_utf8_lossy(std::span<const std::uint8_t> b) {
    std::string out;
    std::size_t i = 0;
    while (i < b.size()) {
        std::uint8_t c = b[i];
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        std::size_t need = 0;
        std::uint8_t lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            lo = c == 0xE0 ? 0xA0 : 0x80;
            hi = c == 0xED ? 0x9F : 0xBF;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            lo = c == 0xF0 ? 0x90 : 0x80;
            hi = c == 0xF4 ? 0x8F : 0xBF;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        std::size_t k = 1;
        for (; k <= need && i + k < b.size(); ++k) {
            std::uint8_t x = b[i + k];
            if (x < (k == 1 ? lo : 0x80) || x > (k == 1 ? hi : 0xBF)) {
                break;
            }
        }
        if (k == need + 1) {
            out.append(reinterpret_cast<const char*>(b.data() + i), need + 1);
            i += need + 1;
        } else {
            out += "\xEF\xBF\xBD";
            i += k;
        }
    }
    return out;
}

}   

std::optional<std::filesystem::path> default_path() {
    SetLastError(0);
    DWORD n = GetEnvironmentVariableW(L"VICTORY_DXIL", nullptr, 0);
    if (n > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
        std::wstring v(n, L'\0');
        DWORD got = n ? GetEnvironmentVariableW(L"VICTORY_DXIL", v.data(), n) : 0;
        v.resize(got);
        return std::filesystem::path(v);
    }
    std::filesystem::path kits = LR"(C:\Program Files (x86)\Windows Kits\10\bin)";
    std::error_code ec;
    std::filesystem::directory_iterator it(kits, ec);
    if (ec) {
        return std::nullopt;
    }
    std::vector<std::filesystem::path> versions;
    for (std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::filesystem::path p = it->path() / L"x64" / L"dxil.dll";
        std::error_code e2;
        if (std::filesystem::exists(p, e2)) {
            versions.push_back(p);
        }
    }
    if (versions.empty()) {
        return std::nullopt;
    }

    return *std::max_element(versions.begin(), versions.end(),
                             [](const std::filesystem::path& a, const std::filesystem::path& b) { return a.native() < b.native(); });
}

std::optional<Validator> Validator::load(const std::optional<std::filesystem::path>& path, std::string& error) {

    std::string lossy = path ? utf8(path->native()) : std::string("dxil.dll");
    std::wstring name = wide(lossy);
    HMODULE module = LoadLibraryW(name.c_str());
    if (!module) {

        error = "cannot load " + lossy + std::string(1, '\0');
        return std::nullopt;
    }
    auto create = reinterpret_cast<CreateInstance>(reinterpret_cast<void*>(GetProcAddress(module, "DxcCreateInstance")));
    if (!create) {
        error = "dxil.dll lacks DxcCreateInstance";
        return std::nullopt;
    }
    void* object = nullptr;
    Hresult hr = create(&CLSID_DXC_VALIDATOR, &IID_IDXC_VALIDATOR, &object);
    if (hr < 0 || !object) {
        error = "DxcCreateInstance(validator) failed: " + hex(hr);
        return std::nullopt;
    }
    return Validator(object);
}

std::optional<std::string> Validator::validate(std::vector<std::uint8_t>& container) {
    Blob blob{&BLOB_VTABLE, container.data(), container.size()};


    void* result = nullptr;
    auto validate_fn = method<Hresult(__stdcall*)(void*, Blob*, std::uint32_t, void**)>(object_, 3);
    Hresult hr = validate_fn(object_, &blob, IN_PLACE_EDIT, &result);
    if (hr < 0 || !result) {
        return "Validate failed: " + hex(hr);
    }
    Hresult status = 0;
    method<Hresult(__stdcall*)(void*, Hresult*)>(result, 3)(result, &status);
    std::string text;
    if (status < 0) {
        void* errors = nullptr;
        if (method<Hresult(__stdcall*)(void*, void**)>(result, 5)(result, &errors) >= 0 && errors) {
            auto ptr = method<const std::uint8_t*(__stdcall*)(void*)>(errors, 3)(errors);
            auto len = method<std::size_t(__stdcall*)(void*)>(errors, 4)(errors);
            std::string all = from_utf8_lossy(std::span(ptr, len));
            std::string_view v = all;
            while (!v.empty() && v.back() == '\0') {
                v.remove_suffix(1);
            }
            text = std::string(rust_text::trim(v));
            release(errors);
        }
        if (text.empty()) {
            text = "rejected (" + hex(status) + ")";
        }
    }
    release(result);
    if (status < 0) {
        return text;
    }
    return std::nullopt;
}

Validator& Validator::operator=(Validator&& o) noexcept {
    if (this != &o) {
        release(object_);
        object_ = o.object_;
        o.object_ = nullptr;
    }
    return *this;
}

Validator::~Validator() { release(object_); }

}   
