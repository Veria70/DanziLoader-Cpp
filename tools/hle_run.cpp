// SPDX-License-Identifier: GPL-3.0-or-later
// Milestone 6: calls the port runtime's exports by name from a scenario
// (tests/m6/hle), as `m6-oracle hle-run` calls the reference's
// (oracle-m6/src/hle_run.rs, where the commands are described), and prints
// the same.
//
//   hle-run <scenario> <out.txt>
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hle/modules.hpp"
#include "hle/registry.hpp"
#include "hle/runtime.hpp"
#include "hle/xasync.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <span>
#include <thread>
#include "pe/image.hpp"

using namespace danzi;

namespace {

void bump(std::uint8_t* at) { reinterpret_cast<std::atomic<std::uint32_t>*>(at)->fetch_add(1, std::memory_order_seq_cst); }
void __stdcall cb_count(void* context, bool canceled) {
    bump(static_cast<std::uint8_t*>(context));
    if (canceled) {
        bump(static_cast<std::uint8_t*>(context) + 4);
    }
}
void __stdcall cb_term(void* context) { bump(static_cast<std::uint8_t*>(context) + 8); }
void __stdcall cb_monitor(void* context, const void*, std::uint32_t port) {
    bump(static_cast<std::uint8_t*>(context) + 12);
    std::memcpy(static_cast<std::uint8_t*>(context) + 20, &port, 4);
}
void __stdcall cb_complete(hle::xasync::AsyncBlock* block) { bump(static_cast<std::uint8_t*>(block->context) + 16); }
std::int32_t __stdcall cb_work(hle::xasync::AsyncBlock*) { return 7; }
std::int32_t __stdcall cb_provider(std::uint32_t op, const hle::xasync::ProviderData* data) {
    switch (op) {
        case 0: return hle::xasync::XAsyncSchedule(data->block, 0);
        case 1: hle::xasync::XAsyncComplete(data->block, 0, 8); return 0;
        case 2: {
            std::uint64_t v = 0x1122334455667788ull;
            std::memcpy(data->buffer, &v, 8);
            return 0;
        }
        default: return 0;
    }
}

std::int32_t __stdcall cb_idle(std::uint32_t, const hle::xasync::ProviderData*) { return 0; }


std::mutex LOG_LOCK;
std::vector<std::string> LOG;

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
std::string cstr(const char* p) { return pe::utf8_lossy(std::span(reinterpret_cast<const std::uint8_t*>(p), std::strlen(p))); }
void note_container(const ContainerInfo* i) {
    std::string line = "container " + cstr(i->name) + " [" + cstr(i->display_name) + "] blobs " + std::to_string(i->blob_count) + " size " + std::to_string(i->total_size) +
                       " sync " + (i->needs_sync ? "true" : "false") + " dated " + (i->last_modified > 0 ? "true" : "false");
    std::lock_guard l(LOG_LOCK);
    LOG.push_back(line);
}
bool __stdcall cb_container(const ContainerInfo* info, void*) {
    note_container(info);
    return true;
}
bool __stdcall cb_container1(const ContainerInfo* info, void*) {
    note_container(info);
    return false;
}
void note_blob(const BlobInfo* i) {
    std::lock_guard l(LOG_LOCK);
    LOG.push_back("blob " + cstr(i->name) + " " + std::to_string(i->size));
}
bool __stdcall cb_blob(const BlobInfo* info, void*) {
    note_blob(info);
    return true;
}
bool __stdcall cb_blob1(const BlobInfo* info, void*) {
    note_blob(info);
    return false;
}


std::uint64_t var(std::map<std::string, std::uint64_t>& vars, const std::string& v) {
    auto plus = v.find('+');
    if (plus == std::string::npos) {
        return vars[v];
    }
    std::string off = v.substr(plus + 1);
    std::uint64_t o = off.starts_with("0x") ? std::stoull(off.substr(2), nullptr, 16) : std::stoull(off);
    return vars[v.substr(0, plus)] + o;
}

std::optional<std::uint64_t> builtin(const std::string& name) {
    if (name == "@container") return reinterpret_cast<std::uint64_t>(&cb_container);
    if (name == "@container1") return reinterpret_cast<std::uint64_t>(&cb_container1);
    if (name == "@blob") return reinterpret_cast<std::uint64_t>(&cb_blob);
    if (name == "@blob1") return reinterpret_cast<std::uint64_t>(&cb_blob1);
    if (name == "@count") return reinterpret_cast<std::uint64_t>(&cb_count);
    if (name == "@term") return reinterpret_cast<std::uint64_t>(&cb_term);
    if (name == "@monitor") return reinterpret_cast<std::uint64_t>(&cb_monitor);
    if (name == "@complete") return reinterpret_cast<std::uint64_t>(&cb_complete);
    if (name == "@work") return reinterpret_cast<std::uint64_t>(&cb_work);
    if (name == "@provider") return reinterpret_cast<std::uint64_t>(&cb_provider);
    if (name == "@idle") return reinterpret_cast<std::uint64_t>(&cb_idle);
    return std::nullopt;
}

std::vector<std::uint8_t> parse_guid(std::string s) {
    std::erase(s, '-');
    std::vector<std::uint8_t> g(16);
    std::uint32_t d1 = static_cast<std::uint32_t>(std::stoul(s.substr(0, 8), nullptr, 16));
    std::uint16_t d2 = static_cast<std::uint16_t>(std::stoul(s.substr(8, 4), nullptr, 16));
    std::uint16_t d3 = static_cast<std::uint16_t>(std::stoul(s.substr(12, 4), nullptr, 16));
    std::memcpy(g.data(), &d1, 4);
    std::memcpy(g.data() + 4, &d2, 2);
    std::memcpy(g.data() + 6, &d3, 2);
    for (int i = 8; i < 16; ++i) {
        g[i] = static_cast<std::uint8_t>(std::stoul(s.substr(2 * i, 2), nullptr, 16));
    }
    return g;
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return s;
}

std::string module_file(HMODULE m) {
    wchar_t path[260];
    DWORD len = GetModuleFileNameW(m, path, 260);
    std::wstring p(path, len);
    std::size_t cut = p.find_last_of(L'\\');
    std::wstring f = cut == std::wstring::npos ? p : p.substr(cut + 1);
    int n = WideCharToMultiByte(CP_UTF8, 0, f.data(), static_cast<int>(f.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, f.data(), static_cast<int>(f.size()), s.data(), n, nullptr, nullptr);
    return lower(s);
}

std::string show(std::uint64_t v) {
    if (v == 0) {
        return "0";
    }
    for (const auto& m : hle::registry()) {
        if (reinterpret_cast<std::uint64_t>(&m) == v) {
            return std::string("pseudo ") + m.name;
        }
        for (const auto& e : m.exports) {
            if (e.address == v) {
                return std::string("export ") + m.name + "!" + e.name;
            }
        }
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(v), &module) &&
        module == GetModuleHandleW(nullptr)) {
        return "self";
    }
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(v), &module)) {
        auto base = reinterpret_cast<std::uint64_t>(module);
        char off[24];
        std::snprintf(off, sizeof off, "0x%llx", static_cast<unsigned long long>(v - base));
        return base == v ? "module " + module_file(module) : "in " + module_file(module) + "+" + off;
    }
    if (v <= 0xFFFFFFFFull) {
        return std::to_string(v);
    }
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::uintptr_t host_module(const std::string& name) {
    std::wstring w(name.begin(), name.end());
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(w.c_str()));
}

std::string esc(const std::string& s) {
    std::string out;
    for (unsigned char b : s) {
        if (b >= 0x21 && b <= 0x7e && b != '\\') {
            out.push_back(static_cast<char>(b));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", b);
            out += buf;
        }
    }
    return out;
}

std::string utf16_to_utf8(const std::vector<std::uint16_t>& w) {

    std::wstring ws(w.begin(), w.end());
    std::string out;
    for (std::size_t i = 0; i < ws.size(); ++i) {
        std::uint32_t c = static_cast<std::uint16_t>(ws[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < ws.size() && ws[i + 1] >= 0xDC00 && ws[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<std::uint16_t>(ws[i + 1]) - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out += {static_cast<char>(0xC0 | c >> 6), static_cast<char>(0x80 | (c & 0x3F))};
        } else if (c < 0x10000) {
            out += {static_cast<char>(0xE0 | c >> 12), static_cast<char>(0x80 | ((c >> 6) & 0x3F)), static_cast<char>(0x80 | (c & 0x3F))};
        } else {
            out += {static_cast<char>(0xF0 | c >> 18), static_cast<char>(0x80 | ((c >> 12) & 0x3F)), static_cast<char>(0x80 | ((c >> 6) & 0x3F)),
                    static_cast<char>(0x80 | (c & 0x3F))};
        }
    }
    return out;
}

std::vector<std::uint8_t> to_utf16le(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    std::vector<std::uint8_t> b;
    for (wchar_t c : w) {
        b.push_back(static_cast<std::uint8_t>(c));
        b.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    b.push_back(0);
    b.push_back(0);
    return b;
}

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return "";
    }
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}


std::string rest(const std::string& line, std::size_t k) {
    std::size_t at = 0;
    for (std::size_t w = 0; w < k; ++w) {
        at = line.find_first_not_of(" \t", at);
        at = line.find_first_of(" \t", at);
        if (at == std::string::npos) {
            return "";
        }
    }
    return trim(line.substr(at));
}

}   

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: hle-run <scenario> <out.txt>\n");
        return 2;
    }


    LoadLibraryW(L"oleaut32.dll");
    std::ifstream in(argv[1], std::ios::binary);
    std::map<std::string, std::vector<std::uint8_t>> bufs;
    std::string out;
    std::string raw;
    std::uint64_t last = 0;
    std::map<std::string, std::uint64_t> vars;

    std::string drive = "?";
    for (std::size_t n = 1; std::getline(in, raw); ++n) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream words(line);
        std::vector<std::string> t;
        for (std::string w; words >> w;) {
            t.push_back(w);
        }
        out += "#" + std::to_string(n) + " " + line + "\n";
        auto drive_text = [&](std::string text) {
            for (std::size_t at; (at = text.find("%D")) != std::string::npos;) {
                text.replace(at, 2, drive);
            }
            return text;
        };
        if (t[0] == "mount") {
            auto l = hle::files::mount(std::filesystem::path(std::u8string(t[1].begin(), t[1].end())));
            drive = l ? std::string(1, static_cast<char>(*l)) : "?";
        } else if (t[0] == "mount-at") {
            hle::files::mount_at(static_cast<wchar_t>(t[1][0]), std::filesystem::path(std::u8string(t[2].begin(), t[2].end())));
        } else if (t[0] == "buf") {
            bufs[t[1]] = std::vector<std::uint8_t>(std::stoull(t[2]), 0);
        } else if (t[0] == "str") {
            std::string s = drive_text(rest(line, 2));
            std::vector<std::uint8_t> b(s.begin(), s.end());
            b.push_back(0);
            bufs[t[1]] = b;
        } else if (t[0] == "wstr") {
            bufs[t[1]] = to_utf16le(drive_text(rest(line, 2)));
        } else if (t[0] == "title") {
            hle::runtime::Title title;
            title.id = static_cast<std::uint32_t>(std::stoul(t[1].starts_with("0x") ? t[1].substr(2) : t[1], nullptr, 16));
            title.store_id = t[2] == "-" ? "" : t[2];
            title.storage = t[3] == "-" ? std::filesystem::path() : std::filesystem::path(std::u8string(t[3].begin(), t[3].end()));
            title.storage_bytes = std::stoull(t[4]);
            hle::runtime::set_title(title);
        } else if (t[0] == "mkdir") {
            std::filesystem::create_directories(std::filesystem::path(std::u8string(t[1].begin(), t[1].end())));
        } else if (t[0] == "mkfile") {
            std::string text = rest(line, 2);
            std::ofstream(std::filesystem::path(std::u8string(t[1].begin(), t[1].end())), std::ios::binary) << text;
        } else if (t[0] == "tree") {

            std::filesystem::path base(L"\\\\?\\" +std::filesystem::absolute(std::filesystem::path(std::u8string(t[1].begin(), t[1].end()))).wstring());
            std::vector<std::string> lines;
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(base, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                auto rel = std::filesystem::relative(it->path(), base).generic_u8string();
                std::string r(rel.begin(), rel.end());
                if (it->is_directory()) {
                    lines.push_back(r + "/");
                } else {
                    std::error_code e2;
                    auto size = std::filesystem::file_size(it->path(), e2);
                    lines.push_back(r + " " + std::to_string(e2 ? 0 : size));
                }
            }
            std::sort(lines.begin(), lines.end());
            for (auto& l : lines) {
                out += "  " + l + "\n";
            }
        } else if (t[0] == "log") {
            std::lock_guard l(LOG_LOCK);
            for (auto& l2 : LOG) {
                out += "  " + esc(l2) + "\n";
            }
            LOG.clear();
        } else if (t[0] == "main-image") {
            hle::runtime::set_main_image(host_module(t[1]));
        } else if (t[0] == "main-image-none") {
            hle::runtime::set_main_image(0);
        } else if (t[0] == "image-path") {
            std::string p = rest(line, 2);
            hle::runtime::set_image_path(host_module(t[1]), std::filesystem::path(std::u8string(p.begin(), p.end())));
        } else if (t[0] == "guid") {
            bufs[t[1]] = parse_guid(t[2]);
        } else if (t[0] == "let") {
            vars[t[1]] = last;
        } else if (t[0] == "load") {
            const auto& b = bufs[t[2]];
            std::uint64_t v = 0;
            std::memcpy(&v, b.data() + std::stoull(t[3]), 8);
            vars[t[1]] = v;
        } else if (t[0] == "store") {
            const std::string& a = t[3];
            std::uint64_t v;
            if (a[0] == '&') {
                v = reinterpret_cast<std::uint64_t>(bufs[a.substr(1)].data());
            } else if (auto f = builtin(a)) {
                v = *f;
            } else if (a[0] == '$') {
                v = var(vars, a.substr(1));
            } else if (a.starts_with("0x")) {
                v = std::stoull(a.substr(2), nullptr, 16);
            } else {
                v = std::stoull(a);
            }
            std::memcpy(bufs[t[1]].data() + std::stoull(t[2]), &v, 8);
        } else if (t[0] == "sleep") {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::stoull(t[1])));
        } else if (t[0] == "dump") {
            const auto& b = bufs[t[1]];
            std::size_t from = t.size() > 3 ? std::min<std::size_t>(std::stoull(t[3]), b.size()) : 0;
            std::size_t k = std::min<std::size_t>(from + std::stoull(t[2]), b.size());
            std::string hex;
            for (std::size_t i = from; i < k; ++i) {
                char x[4];
                std::snprintf(x, sizeof x, "%02x", b[i]);
                hex += x;
            }
            out += "  " + hex + "\n";
        } else if (t[0] == "text") {
            const auto& b = bufs[t[1]];
            std::size_t end = 0;
            while (end < b.size() && b[end] != 0) {
                ++end;
            }
            out += "  " + esc(pe::utf8_lossy(std::span(b.data(), end))) + "\n";
        } else if (t[0] == "wtext") {
            const auto& b = bufs[t[1]];
            std::vector<std::uint16_t> w;
            for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
                std::uint16_t c = static_cast<std::uint16_t>(b[i] | b[i + 1] << 8);
                if (c == 0) {
                    break;
                }
                w.push_back(c);
            }
            out += "  " + esc(utf16_to_utf8(w)) + "\n";
        } else if (t[0] == "call" || t[0] == "vcall") {
            std::string name = t[2];
            int width = 64;
            if (auto colon = name.find(':'); colon != std::string::npos) {
                width = std::stoi(name.substr(colon + 1));
                name = name.substr(0, colon);
            }
            std::uintptr_t address = 0;
            std::uint64_t args[8] = {};
            std::size_t shift = 0;
            if (t[0] == "vcall") {
                std::uint64_t object = vars[t[1].substr(1)];
                std::uint64_t slot = std::stoull(name.starts_with("0x") ? name.substr(2) : name, nullptr, 16);
                args[0] = object;
                shift = 1;
                address = reinterpret_cast<const std::uintptr_t*>(*reinterpret_cast<const std::uintptr_t*>(object))[slot / 8];
            }
            for (const auto& m : t[0] == "vcall" ? std::vector<hle::HleModule>{} : hle::registry()) {
                if (!m.serves(t[1])) {
                    continue;
                }
                for (const auto& e : m.exports) {
                    if (name == e.name) {
                        address = e.address;
                        break;
                    }
                }
                if (address) {
                    break;
                }
            }
            if (!address) {
                out += "  no such export\n";
                continue;
            }
            for (std::size_t k = 3; k < t.size() && k + shift < 11; ++k) {
                const std::string& a = t[k];
                std::uint64_t* arg = &args[k - 3 + shift];
                if (a[0] == '&') {
                    std::string nm = a.substr(1);
                    std::size_t off = 0;
                    if (auto plus = nm.find('+'); plus != std::string::npos) {
                        off = std::stoull(nm.substr(plus + 1));
                        nm = nm.substr(0, plus);
                    }
                    *arg = reinterpret_cast<std::uint64_t>(bufs[nm].data()) + off;
                } else if (auto f = builtin(a)) {
                    *arg = *f;
                } else if (a[0] == '$' && a != "$last") {
                    *arg = var(vars, a.substr(1));
                } else if (a == "null") {
                    *arg = 0;
                } else if (a == "$last") {
                    *arg = last;
                } else if (a.starts_with("0x")) {
                    *arg = std::stoull(a.substr(2), nullptr, 16);
                } else if (a[0] == '-') {
                    *arg = 0 - std::stoull(a.substr(1));
                } else {
                    *arg = std::stoull(a);
                }
            }
            using F = std::uint64_t(__stdcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                std::uint64_t);
            SetLastError(0);
            std::uint64_t r = reinterpret_cast<F>(address)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
            DWORD err = GetLastError();
            last = r;
            std::string shown = width == 0 ? "-" : width == 8 ? show(r & 0xFF) : width == 32 ? show(r & 0xFFFFFFFFull) : show(r);


            out += "  -> " + shown + (t[0] == "vcall" ? std::string() : " err " + std::to_string(err)) + "\n";
        } else {
            std::fprintf(stderr, "unknown command %s\n", t[0].c_str());
            return 2;
        }
    }
    std::ofstream(argv[2], std::ios::binary) << out;
    return 0;
}
