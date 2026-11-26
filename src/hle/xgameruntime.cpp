// SPDX-License-Identifier: GPL-3.0-or-later
// `xgameruntime.dll` (`victory-hle/src/modules/xgameruntime.rs`;
// DanziLoader / victory-perf, see NOTICE.md). See xgameruntime.hpp.
#include "xgameruntime.hpp"

#include <mutex>
#include <vector>

#include <atomic>

#include "areas.hpp"
#include "pe/image.hpp"

namespace danzi::hle::xgr {

std::uint32_t __stdcall add_ref(const Object*) { return 1; }

HRESULT_ put_string(const std::string& text, std::size_t size, char* buffer, std::size_t* used) {
    std::size_t len = text.size() + 1;
    if (!buffer || size < len) {
        return E_NOT_SUFFICIENT_BUFFER_;
    }
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = 0;
    if (used) {
        *used = len;
    }
    return S_OK_;
}

std::optional<std::string> text(const char* s) {
    if (!s) {
        return std::nullopt;
    }
    return pe::utf8_lossy(std::span(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)));
}

std::string utf16_lossy(std::wstring_view w) {
    std::string out;
    out.reserve(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::uint32_t c = w[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<std::uint32_t>(w[i + 1]) - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::wstring utf16(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::uint64_t token() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

namespace {
constexpr const char* T = "victory_hle::modules::xgameruntime";
std::string guid_text(const Guid& g) {
    char b[40];
    std::snprintf(b, sizeof b, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.data1, g.data2, g.data3, g.data4[0], g.data4[1], g.data4[2], g.data4[3], g.data4[4],
                  g.data4[5], g.data4[6], g.data4[7]);
    return b;
}
}   

HRESULT_ __stdcall XGameRuntimeInitialize() {
    log::info(T, "XGameRuntimeInitialize");
    return S_OK_;
}
void __stdcall XGameRuntimeUninitialize() { log::info(T, "XGameRuntimeUninitialize"); }


HRESULT_ __stdcall InitializeApiImpl(std::uint64_t version, std::uint64_t build) {
    log::info(T, "InitializeApiImpl", {{"version", log::debug(version)}, {"build", log::debug(build)}});
    return S_OK_;
}
HRESULT_ __stdcall InitializeApiImplEx(std::uint64_t version, std::uint64_t build, std::uint64_t flags) {
    log::info(T, "InitializeApiImplEx", {{"version", log::debug(version)}, {"build", log::debug(build)}, {"flags", log::debug(flags)}});
    return S_OK_;
}
HRESULT_ __stdcall InitializeApiImplEx2(std::uint64_t version, std::uint64_t build, std::uint64_t flags, const void* options) {
    log::info(T, "InitializeApiImplEx2",
              {{"version", log::debug(version)}, {"build", log::debug(build)}, {"flags", log::debug(flags)}, {"options", log::hex(reinterpret_cast<std::uintptr_t>(options))}});
    return S_OK_;
}
void __stdcall UninitializeApiImpl() { log::info(T, "UninitializeApiImpl"); }

const std::vector<Interface>& areas() {
    static const std::vector<Interface> all = all_area_interfaces();
    return all;
}


HRESULT_ __stdcall QueryApiImpl(const Guid* provider, const Guid* iid, void** out) {
    if (!provider || !iid) {
        return NOT_SUPPORTED;
    }
    for (const auto& a : areas()) {
        if (a.provider == *provider && a.iid == *iid) {
            if (!out) {
                break;
            }
            *out = const_cast<Object*>(a.object);
            return S_OK_;
        }
    }

    static std::mutex lock;
    static std::vector<std::pair<Guid, Guid>> queried;
    {
        std::lock_guard l(lock);
        for (const auto& [p, i] : queried) {
            if (p == *provider && i == *iid) {
                return NOT_SUPPORTED;
            }
        }
        queried.emplace_back(*provider, *iid);
    }
    log::warn(T, "runtime API area not implemented", {{"provider", guid_text(*provider)}, {"iid", guid_text(*iid)}});
    return NOT_SUPPORTED;
}

void __stdcall XErrorReport(HRESULT_ hr, const char* message) {
    char h[16];
    std::snprintf(h, sizeof h, "0x%08x", static_cast<unsigned>(hr));
    std::string m = message ? pe::utf8_lossy(std::span(reinterpret_cast<const std::uint8_t*>(message), std::strlen(message))) : std::string();
    log::warn(T, "XErrorReport", {{"hr", h}, {"message", m}});
}

}   
