// SPDX-License-Identifier: GPL-3.0-or-later
// Media Foundation (`victory-hle/src/modules/media.rs`; DanziLoader /
// victory-perf, see NOTICE.md) is the host's (`mfplat.dll` passes through),
// except for the source resolver: the host's would fetch `http`, `rtsp` and
// other URLs over the network, past `net`. Titles get a proxy that opens
// local files only, with paths on our virtual drives rewritten to their host
// folders, and forwards everything else to the host's resolver.
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/log.hpp"
#include "modules.hpp"
#include "pe/image.hpp"
#include "registry.hpp"

namespace danzi::hle::media {

namespace {

constexpr HRESULT_ E_NOINTERFACE_ = static_cast<HRESULT_>(0x80004002u);
constexpr HRESULT_ E_POINTER_ = static_cast<HRESULT_>(0x80004003u);

constexpr HRESULT_ UNSUPPORTED_SCHEME = static_cast<HRESULT_>(0xC00D36C3u);

const Guid IID_IUNKNOWN_{0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const Guid IID_RESOLVER{0xFBE5A32D, 0xA497, 0x4B61, {0xBB, 0x85, 0x97, 0xB1, 0xA8, 0x48, 0xA6, 0xE3}};


struct Refused {
    std::string scheme, host;
};

bool drive(std::string_view s) {
    return s.size() >= 2 && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) && s[1] == ':';
}

std::string host_of(std::string_view rest) {
    std::size_t end = rest.find_first_of("/\\");
    return std::string(rest.substr(0, end));
}

std::wstring with_nul(std::wstring_view s) {
    std::wstring out(s);
    out.push_back(L'\0');
    return out;
}

bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
std::uint8_t hex_value(char c) {
    return static_cast<std::uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
}


std::string percent_decode(std::string_view s) {
    std::vector<std::uint8_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '%' && i + 3 <= s.size()) {
            char a = s[i + 1], b = s[i + 2];
            if ((hex(a) || a == '+') && hex(b)) {
                out.push_back(a == '+' ? hex_value(b) : static_cast<std::uint8_t>(hex_value(a) << 4 | hex_value(b)));
                i += 3;
                continue;
            }
        }
        out.push_back(static_cast<std::uint8_t>(s[i]));
        i += 1;
    }
    return pe::utf8_lossy(std::span<const std::uint8_t>(out));
}


std::variant<std::wstring, Refused> local(std::wstring_view url) {
    std::string text = xgr::utf16_lossy(url);
    std::string_view t = text;
    std::string path;
    if (t.starts_with("\\\\?\\")) {
        std::string_view rest = t.substr(4);
        if (!drive(rest)) {
            while (rest.starts_with("UNC\\")) {
                rest.remove_prefix(4);
            }
            return Refused{"unc", host_of(rest)};
        }
        path = text;
    } else if (t.starts_with("\\\\") || t.starts_with("//")) {
        return Refused{"unc", host_of(t.substr(2))};
    } else if (drive(t)) {
        path = text;
    } else {
        std::size_t colon = t.find(':');
        std::string_view scheme_part = colon == std::string_view::npos ? std::string_view() : t.substr(0, colon);
        bool scheme_ok = colon != std::string_view::npos && scheme_part.size() > 1;
        for (char c : scheme_part) {
            bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            scheme_ok = scheme_ok && (alnum || c == '+' || c == '-' || c == '.');
        }
        if (!scheme_ok) {

            return with_nul(url);
        }
        std::string scheme(scheme_part);
        for (char& c : scheme) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + 32);
            }
        }
        std::string_view rest = t.substr(colon + 1);
        std::optional<std::string_view> authority;
        if (rest.starts_with("//")) {
            authority = rest.substr(2);
        }
        if (scheme != "file") {
            return Refused{scheme, authority ? host_of(*authority) : std::string()};
        }
        std::string_view p = rest;
        if (authority) {
            std::string host = host_of(*authority);
            bool is_localhost = host.size() == 9;
            for (std::size_t i = 0; is_localhost && i < 9; ++i) {
                char c = host[i];
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c + 32);
                }
                is_localhost = c == "localhost"[i];
            }
            if (!host.empty() && !is_localhost) {
                return Refused{scheme, host};
            }
            p = authority->substr(host.size());
        }
        while (!p.empty() && (p.front() == '/' || p.front() == '\\')) {
            p.remove_prefix(1);
        }
        path = percent_decode(p);
        for (char& c : path) {
            if (c == '/') {
                c = '\\';
            }
        }
        if (!drive(path)) {

            return with_nul(url);
        }
    }
    std::wstring wide = xgr::utf16(path);
    if (auto host = files::translate(wide)) {
        return std::move(*host);
    }
    return with_nul(wide);
}


std::variant<std::wstring, HRESULT_> checked(const wchar_t* url) {
    if (!url) {
        return E_POINTER_;
    }
    auto r = local(std::wstring_view(url));
    if (auto* ok = std::get_if<std::wstring>(&r)) {
        return std::move(*ok);
    }
    auto& [scheme, host] = std::get<Refused>(r);
    static std::mutex lock;
    static std::set<std::pair<std::string, std::string>> refused;
    std::lock_guard l(lock);
    if (refused.emplace(scheme, host).second) {
        log::warn("victory_hle::modules::media", "media: refused a URL that is not a local file", {{"scheme", log::debug(scheme)}, {"host", log::debug(host)}});
    }
    return UNSUPPORTED_SCHEME;
}


struct Host {
    const std::uintptr_t* vtable;
};


template <class F>
F method(Host* host, std::size_t n) {
    return reinterpret_cast<F>(host->vtable[n]);
}

using Unknown = void*;

}   

struct Resolver {
    const void* const* vtable;
    Host* host;
    std::atomic<std::uint32_t> refs;
};
static_assert(sizeof(Resolver) == 24);

namespace {

using This = Resolver*;


Host* host(This self) { return self->host; }

std::uint32_t WINAPI add_ref(This self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }


HRESULT_ WINAPI query_interface(This self, const Guid* iid, void** out) {
    if (!iid || !out) {
        return E_POINTER_;
    }
    Guid id;
    std::memcpy(&id, iid, sizeof id);
    bool found = id == IID_IUNKNOWN_ || id == IID_RESOLVER;
    *out = found ? self : nullptr;
    if (!found) {
        return E_NOINTERFACE_;
    }
    add_ref(self);
    return S_OK_;
}

std::uint32_t WINAPI release(This self) {
    std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {


        Host* h = self->host;
        delete self;

        method<std::uint32_t(WINAPI*)(Host*)>(h, 2)(h);
    }
    return left;
}

HRESULT_ WINAPI create_from_url(This self, const wchar_t* url, std::uint32_t flags, Unknown props, std::uint32_t* kind, Unknown* object) {
    auto u = checked(url);
    if (auto* error = std::get_if<HRESULT_>(&u)) {
        if (object) {
            *object = nullptr;
        }
        return *error;
    }
    Host* h = host(self);

    auto f = method<HRESULT_(WINAPI*)(Host*, const wchar_t*, std::uint32_t, Unknown, std::uint32_t*, Unknown*)>(h, 3);
    return f(h, std::get<std::wstring>(u).c_str(), flags, props, kind, object);
}

HRESULT_ WINAPI begin_from_url(This self, const wchar_t* url, std::uint32_t flags, Unknown props, Unknown* cookie, Unknown callback, Unknown state) {
    auto u = checked(url);
    if (auto* error = std::get_if<HRESULT_>(&u)) {
        return *error;
    }
    Host* h = host(self);

    auto f = method<HRESULT_(WINAPI*)(Host*, const wchar_t*, std::uint32_t, Unknown, Unknown*, Unknown, Unknown)>(h, 5);
    return f(h, std::get<std::wstring>(u).c_str(), flags, props, cookie, callback, state);
}

HRESULT_ WINAPI create_from_stream(This self, Unknown stream, const wchar_t* url, std::uint32_t flags, Unknown props, std::uint32_t* kind, Unknown* object) {
    Host* h = host(self);


    auto f = method<HRESULT_(WINAPI*)(Host*, Unknown, const wchar_t*, std::uint32_t, Unknown, std::uint32_t*, Unknown*)>(h, 4);
    return f(h, stream, url, flags, props, kind, object);
}

HRESULT_ WINAPI end_from_url(This self, Unknown result, std::uint32_t* kind, Unknown* object) {
    Host* h = host(self);

    return method<HRESULT_(WINAPI*)(Host*, Unknown, std::uint32_t*, Unknown*)>(h, 6)(h, result, kind, object);
}

HRESULT_ WINAPI begin_from_stream(This self, Unknown stream, const wchar_t* url, std::uint32_t flags, Unknown props, Unknown* cookie, Unknown callback, Unknown state) {
    Host* h = host(self);

    auto f = method<HRESULT_(WINAPI*)(Host*, Unknown, const wchar_t*, std::uint32_t, Unknown, Unknown*, Unknown, Unknown)>(h, 7);
    return f(h, stream, url, flags, props, cookie, callback, state);
}

HRESULT_ WINAPI end_from_stream(This self, Unknown result, std::uint32_t* kind, Unknown* object) {
    Host* h = host(self);

    return method<HRESULT_(WINAPI*)(Host*, Unknown, std::uint32_t*, Unknown*)>(h, 8)(h, result, kind, object);
}

HRESULT_ WINAPI cancel(This self, Unknown cookie) {
    Host* h = host(self);

    return method<HRESULT_(WINAPI*)(Host*, Unknown)>(h, 9)(h, cookie);
}

#define F(x) reinterpret_cast<const void*>(&x)
const void* const METHODS[10] = {
    F(query_interface), F(add_ref),        F(release),      F(create_from_url),   F(create_from_stream),
    F(begin_from_url),  F(end_from_url),   F(begin_from_stream), F(end_from_stream), F(cancel),
};
#undef F

}   


HRESULT_ WINAPI MFCreateSourceResolver(Resolver** out) {
    if (!out) {
        return E_POINTER_;
    }
    static const std::optional<std::uintptr_t> CREATE = host_function("mfplat.dll", "MFCreateSourceResolver");
    if (!CREATE) {
        return UNSUPPORTED_SCHEME;
    }

    auto create = reinterpret_cast<HRESULT_(WINAPI*)(Host**)>(*CREATE);
    Host* h = nullptr;
    HRESULT_ hr = create(&h);
    if (hr < 0 || !h) {
        return hr;
    }
    *out = new Resolver{METHODS, h, 1};
    return S_OK_;
}

#define E(name, f) HleExport{name, reinterpret_cast<std::uintptr_t>(&f)}
std::vector<HleExport> exports() { return {E("MFCreateSourceResolver", MFCreateSourceResolver)}; }
#undef E

}   
