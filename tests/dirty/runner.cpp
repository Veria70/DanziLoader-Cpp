// SPDX-License-Identifier: GPL-3.0-or-later
// Runs a dirty-range scenario (docs/dirty-scenarios.md) against the C++
// port and writes `log.txt` plus one `shadow-<name>.bin` per host copy.
// `oracle/src/runner.rs` is the same runner for the Rust reference; the two
// must stay line-for-line equivalent in what they log.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/env.hpp"
#include "core/hybrid_watch.hpp"
#include "core/inplace.hpp"
#include "core/shadow.hpp"
#include "core/title_copy.hpp"
#include "core/watch.hpp"
#include "core/win.hpp"
#include "core/memcopy.hpp"
#include "gpu/card.hpp"
#include "gpu/lookup.hpp"
#include "gpu/rebar.hpp"
#include "gpu/device.hpp"

namespace fs = std::filesystem;
using danzi::usize;

namespace {

constexpr usize BLOCK = usize{64} << 10;
constexpr usize KPAGE = 4096;

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string hex(usize v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

long long num(const std::string& s) {
    bool neg = !s.empty() && s[0] == '-';
    std::string body = neg ? s.substr(1) : s;
    if (body.empty()) {
        throw Failure("not a number: " + s);
    }
    size_t used = 0;
    long long v = 0;
    try {
        if (body.rfind("0x", 0) == 0) {
            v = static_cast<long long>(std::stoull(body.substr(2), &used, 16));
            used += 2;
        } else {
            v = static_cast<long long>(std::stoull(body, &used, 10));
        }
    } catch (const std::exception&) {
        throw Failure("not a number: " + s);
    }
    if (used != body.size()) {
        throw Failure("not a number: " + s);
    }
    return neg ? -v : v;
}

usize unum(const std::string& s) {
    long long v = num(s);
    if (v < 0) {
        throw Failure("negative: " + s);
    }
    return static_cast<usize>(v);
}


std::uint8_t pattern(std::uint32_t seed, usize i) {
    std::uint32_t x = (seed * 0x9E3779B1u) ^ (static_cast<std::uint32_t>(i) * 0x85EBCA77u);
    x ^= x >> 15;
    return static_cast<std::uint8_t>(x);
}


long long now_ns() {
    static const long long freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<long long>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(freq));
}

std::uint64_t fnv64(const std::uint8_t* p, usize n) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (usize i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x00000100000001b3ull;
    }
    return h;
}


std::vector<std::uint8_t> read_title(usize at, usize len) {
    std::vector<std::uint8_t> out(len, 0);
    usize done = 0;
    while (done < len) {
        usize a = at + done;
        usize n = std::min((a / KPAGE + 1) * KPAGE - a, len - done);
        SIZE_T got = 0;
        BOOL ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a), out.data() + done, n, &got);
        if (!ok || got != n) {
            std::memset(out.data() + done, 0, n);
        }
        done += n;
    }
    return out;
}

bool writable_page(usize at) {
    MEMORY_BASIC_INFORMATION info{};
    SIZE_T n = VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof info);
    return n != 0 && info.State == MEM_COMMIT && (info.Protect & (PAGE_READWRITE | PAGE_READONLY)) != 0;
}


std::pair<usize, usize> title_write(usize at, usize len, std::uint32_t seed) {
    usize written = 0, skipped = 0, i = 0;
    while (i < len) {
        usize a = at + i;
        usize n = std::min((a / KPAGE + 1) * KPAGE - a, len - i);
        if (writable_page(a)) {
            for (usize k = 0; k < n; ++k) {

                *reinterpret_cast<volatile std::uint8_t*>(a + k) = pattern(seed, i + k);
            }
            written += n;
        } else {
            skipped += n;
        }
        i += n;
    }
    return {written, skipped};
}

struct Copy {
    std::string name;
    std::shared_ptr<danzi::shadow::Shadow> shadow;
};

void note_copy(void* context, usize at, usize len);
void note_write(void* context, usize at, usize len);

struct World {


    std::unique_ptr<danzi::gpu::Device> device;
    std::unique_ptr<danzi::gpu::CardHost> card_host;
    std::unique_ptr<danzi::gpu::Registry> registry_;

    std::map<std::string, std::pair<usize, std::uint64_t>> buffers;
    usize next_resource = 0;


    struct CardWrite {
        danzi::shadow::Shadow* shadow;
        std::uint64_t off;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<CardWrite> card_writes;

    std::deque<danzi::gpu::Flight> flights;

    danzi::memcopy::RangeSet range_set;
    std::map<std::string, usize> places;
    std::map<std::string, usize> place_len;
    std::vector<std::pair<usize, usize>> title_writes;   
    std::map<std::string, HANDLE> sections;
    std::vector<Copy> copies;
    std::vector<std::pair<usize, usize>> copied;   
    std::ostringstream log;

    bool quiet = false;

    std::ostringstream timing;
    size_t op_number = 0;

    usize place(const std::string& name) const {
        auto it = places.find(name);
        if (it == places.end()) {
            throw Failure("unknown place " + name);
        }
        return it->second;
    }

    HANDLE section(const std::string& name) const {
        auto it = sections.find(name);
        if (it == sections.end()) {
            throw Failure("unknown section " + name);
        }
        return it->second;
    }

    danzi::shadow::Shadow& copy(const std::string& name) {
        for (auto& c : copies) {
            if (c.name == name) {
                return *c.shadow;
            }
        }
        throw Failure("unknown shadow " + name);
    }


    std::vector<std::pair<usize, usize>> ranges(const std::vector<std::string>& t, size_t from) {
        if ((t.size() - from) % 3 != 0) {
            throw Failure("ranges come as <shadow> <off> <len>");
        }
        std::vector<std::pair<usize, usize>> out;
        for (size_t i = from; i < t.size(); i += 3) {
            auto& s = copy(t[i]);
            out.emplace_back(static_cast<usize>(static_cast<long long>(s.start()) + num(t[i + 1])), unum(t[i + 2]));
        }
        return out;
    }


    std::vector<danzi::shadow::Declared> declared(const std::vector<std::string>& t, size_t from) {
        if ((t.size() - from) % 3 != 0) {
            throw Failure("ranges come as <shadow> <off> <len>");
        }
        std::vector<std::pair<std::string, danzi::shadow::Declared>> out;
        for (size_t i = from; i < t.size(); i += 3) {
            auto& s = copy(t[i]);
            usize at = static_cast<usize>(static_cast<long long>(s.start()) + num(t[i + 1]));
            danzi::shadow::Range range{at, at + unum(t[i + 2])};
            auto it = std::find_if(out.begin(), out.end(), [&](const auto& d) { return d.first == t[i]; });
            if (it != out.end()) {
                it->second.second.push_back(range);
            } else {
                out.push_back({t[i], {&s, {range}}});
            }
        }
        std::vector<danzi::shadow::Declared> back;
        for (auto& [name, d] : out) {
            back.push_back(std::move(d));
        }
        return back;
    }

    danzi::gpu::CardHost& host() {
        if (!card_host) {
            auto v = danzi::env::get(L"DANZI_TEST_ADAPTER");
            bool warp = v && *v == L"warp";
            device = danzi::gpu::Device::create(warp ? danzi::gpu::Device::Kind::Warp : danzi::gpu::Device::Kind::Hardware);
            card_host = std::make_unique<danzi::gpu::CardHost>(*device);
        }
        return *card_host;
    }

    danzi::gpu::Registry& registry() {
        if (!registry_) {
            registry_ = std::make_unique<danzi::gpu::Registry>(host());
        }
        return *registry_;
    }


    void keep(const std::string& name, std::shared_ptr<danzi::shadow::Shadow> s) {
        s->set_copy_observer(note_copy, this);
        std::erase_if(copies, [&](const Copy& c) { return c.name == name; });
        copies.push_back(Copy{name, std::move(s)});
    }


    std::pair<danzi::gpu::Prelude, danzi::gpu::ComPtr<ID3D12Resource>> title_list() {
        auto& d = *device;
        auto p = d.make_prelude();
        if (!p) {
            throw Failure("a list");
        }
        usize total = 0;
        for (const auto& w : card_writes) {
            total += w.bytes.size();
        }
        danzi::gpu::ComPtr<ID3D12Resource> source;
        std::uint8_t* data = nullptr;
        if (total > 0) {
            auto b = d.shared_buffer(total);
            if (!b) {
                throw Failure("upload memory");
            }
            source = b->first;
            data = b->second;
        }
        p->allocator->Reset();
        p->list->Reset(p->allocator.Get(), nullptr);
        usize at = 0;
        for (const auto& w : card_writes) {
            std::memcpy(data + at, w.bytes.data(), w.bytes.size());
            p->list->CopyBufferRegion(w.shadow->buffer(), w.off, source.Get(), at, w.bytes.size());
            at += w.bytes.size();
        }
        p->list->Close();
        card_writes.clear();
        return {std::move(*p), source};
    }


    std::vector<std::uint8_t> read_back(ID3D12Resource* buffer, std::uint64_t size) {
        auto& d = *device;
        auto b = d.shared_buffer(size);
        auto p = d.make_prelude();
        if (!b || !p) {
            throw Failure("read-back memory");
        }
        p->allocator->Reset();
        p->list->Reset(p->allocator.Get(), nullptr);
        p->list->CopyBufferRegion(b->first.Get(), 0, buffer, 0, size);
        p->list->Close();
        d.execute_and_wait({p->list.Get()});
        return std::vector<std::uint8_t>(b->second, b->second + size);
    }


    void log_card(const std::string& name) {
        auto& s = copy(name);
        auto sides = s.card_sides();
        if (!sides) {
            return;
        }
        auto vram = read_back(s.buffer(), sides->size);
        char v[17], h[17] = "none";
        std::snprintf(v, sizeof v, "%016llx", static_cast<unsigned long long>(fnv64(vram.data(), vram.size())));
        if (sides->before) {
            auto hashes = read_back(sides->before.Get(), danzi::shadow::card_hash_bytes(sides->size));
            std::snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(fnv64(hashes.data(), hashes.size())));
        }
        log << "  card " << name << " vram=" << v << " hashes=" << h << "\n";
    }


    void log_card_stats() {
        auto c = danzi::shadow::card_stats();
        log << "  card stats sent=" << c[0] << " ranges=" << c[1] << " compared=" << c[2] << " fetched=" << c[3] << " fetched_bytes=" << c[4]
            << " waits=" << c[5] << " copies=" << c[7] << " bytes=" << c[8] << " hash_bytes=" << c[9] << "\n";
    }

    void log_writes() {
        for (auto [at, len] : title_writes) {
            log << "  title write " << hex(at) << " " << hex(len) << "\n";
        }
        title_writes.clear();
    }

    void log_copies() {
        for (auto [at, len] : copied) {
            log << "  copy " << hex(at) << " " << hex(len) << "\n";
        }
        copied.clear();
    }


    void log_state(const std::string& name, usize from, usize to, bool snap = false) {
        auto& s = copy(name);
        usize start = s.start(), len = s.len();
        const std::uint8_t* bytes = s.data();
        auto st = s.state();
        from = std::max(from, start);
        to = std::min(to, start + len);
        usize drift = 0;
        if (from < to) {
            auto title = read_title(from, to - from);
            for (usize k = 0; k < title.size(); ++k) {
                usize a = from + k;
                bool pending = false;
                for (auto [p, e] : st.pending) {
                    if (p <= a && a < e) {
                        pending = true;
                        break;
                    }
                }
                if (!pending && bytes[a - start] != title[k]) {
                    ++drift;
                }
            }
        }
        std::string pages;
        for (usize i = 0; i < st.at.size();) {
            usize j = i;
            while (j < st.at.size() && st.at[j] == st.at[i]) {
                ++j;
            }
            if (!pages.empty()) {
                pages += ',';
            }
            pages += std::to_string(st.at[i]) + "x" + std::to_string(j - i);
            i = j;
        }
        std::string pend;
        for (auto [a, e] : st.pending) {
            if (!pend.empty()) {
                pend += ',';
            }
            pend += hex(a) + "-" + hex(e);
        }
        char h[17], th[17];
        std::snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(fnv64(bytes, len)));
        auto title_now = read_title(start, len);


        usize stale = 0;
        for (usize k = 0; k < title_now.size(); ++k) {
            if (bytes[k] == title_now[k]) {
                continue;
            }
            usize a = start + k;
            bool in_pending = false;
            for (auto [p, e] : st.pending) {
                if (p <= a && a < e) {
                    in_pending = true;
                    break;
                }
            }
            stale += in_pending ? 0 : 1;
        }
        std::snprintf(th, sizeof th, "%016llx", static_cast<unsigned long long>(fnv64(title_now.data(), title_now.size())));
        std::string snap_text;
        if (snap) {
            std::vector<danzi::shadow::Range> offsets;
            for (auto [a, e] : st.pending) {
                offsets.emplace_back(std::clamp(a, start, start + len) - start, std::clamp(e, start, start + len) - start);
            }
            if (auto sh = s.snapshot_hash(offsets)) {
                char sb[17];
                std::snprintf(sb, sizeof sb, "%016llx", static_cast<unsigned long long>(*sh));
                snap_text = std::string(" snap=") + sb;
            } else {
                snap_text = " snap=none";
            }
        }
        log << "  state " << name << " mode=" << unsigned(st.mode) << " uncopied=" << st.uncopied << " pages=" << pages
            << " pending=[" << pend << "] hash=" << h << " title=" << th << " drift=" << drift << " stale=" << stale << snap_text << "\n";
    }

    void op(const std::vector<std::string>& t, const fs::path& out) {
        auto need = [&](size_t n) {
            if (t.size() != n) {
                throw Failure(t[0] + " takes " + std::to_string(n - 1) + " arguments");
            }
        };
        const std::string& o = t[0];
        if (o == "env") {

        } else if (o == "quiet") {
            need(2);
            quiet = unum(t[1]) != 0;
        } else if (o == "region") {
            need(5);
            usize addr = unum(t[3]), bytes = unum(t[4]);
            DWORD flags;
            if (t[2] == "watched") {
                flags = MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH;
            } else if (t[2] == "reserve") {
                flags = MEM_RESERVE | MEM_WRITE_WATCH;
            } else if (t[2] == "plain") {
                flags = MEM_RESERVE | MEM_COMMIT;
            } else {
                throw Failure("unknown region kind " + t[2]);
            }
            usize p = reinterpret_cast<usize>(VirtualAlloc(reinterpret_cast<void*>(addr), bytes, flags, PAGE_READWRITE));
            if (p != addr) {
                throw Failure("region " + t[1] + " not at " + hex(addr) + " (got " + hex(p) + ")");
            }
            places[t[1]] = addr;
            place_len[t[1]] = bytes;
        } else if (o == "section") {
            need(3);
            std::uint64_t bytes = unum(t[2]);
            HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(bytes >> 32),
                                          static_cast<DWORD>(bytes), nullptr);
            if (!h) {
                throw Failure("section " + t[1] + " failed");
            }
            sections[t[1]] = h;
        } else if (o == "view") {
            need(6);
            HANDLE h = section(t[2]);
            std::uint64_t offset = unum(t[3]);
            usize addr = unum(t[4]), bytes = unum(t[5]);
            usize v = reinterpret_cast<usize>(MapViewOfFileEx(h, FILE_MAP_WRITE, static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset),
                                                              bytes, reinterpret_cast<void*>(addr)));
            if (v != addr) {
                throw Failure("view " + t[1] + " not at " + hex(addr) + " (got " + hex(v) + ")");
            }
            danzi::watch::title_memory_remapped(addr, bytes);
            danzi::inplace::title_memory_view(addr, bytes, reinterpret_cast<usize>(h), offset);
            places[t[1]] = addr;
            place_len[t[1]] = bytes;
        } else if (o == "placeholder") {
            need(4);
            usize addr = unum(t[2]), blocks = unum(t[3]);
            usize p = reinterpret_cast<usize>(VirtualAlloc2(GetCurrentProcess(), reinterpret_cast<void*>(addr), blocks * BLOCK,
                                                            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
            if (p != addr) {
                throw Failure("placeholder " + t[1] + " not at " + hex(addr) + " (got " + hex(p) + ")");
            }
            places[t[1]] = addr;
            place_len[t[1]] = blocks * BLOCK;
        } else if (o == "map") {
            need(5);
            usize at = place(t[1]) + unum(t[2]) * BLOCK;
            HANDLE h = section(t[3]);
            std::uint64_t offset = unum(t[4]);


            VirtualFree(reinterpret_cast<void*>(at), BLOCK, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            usize v = reinterpret_cast<usize>(MapViewOfFile3(h, GetCurrentProcess(), reinterpret_cast<void*>(at), offset, BLOCK,
                                                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0));
            if (v != at) {
                throw Failure("map at " + hex(at) + " failed");
            }
            danzi::watch::title_memory_remapped(at, BLOCK);
            danzi::inplace::title_memory_view(at, BLOCK, reinterpret_cast<usize>(h), offset);
        } else if (o == "unmap") {
            need(3);
            usize at = place(t[1]) + unum(t[2]) * BLOCK;
            danzi::watch::title_memory_remapped(at, BLOCK);
            if (!UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(at), MEM_PRESERVE_PLACEHOLDER)) {
                throw Failure("unmap at " + hex(at) + " failed");
            }
        } else if (o == "commit" || o == "decommit") {
            need(4);
            usize at = place(t[1]) + unum(t[2]), len = unum(t[3]);
            bool ok = o == "commit" ? VirtualAlloc(reinterpret_cast<void*>(at), len, MEM_COMMIT, PAGE_READWRITE) != nullptr
                                    : VirtualFree(reinterpret_cast<void*>(at), len, MEM_DECOMMIT) != 0;
            log << "  ok " << (ok ? 1 : 0) << "\n";
            danzi::watch::title_memory_remapped(at, len);
        } else if (o == "remapped") {
            need(4);
            danzi::watch::title_memory_remapped(place(t[1]) + unum(t[2]), unum(t[3]));
        } else if (o == "write" || o == "kwrite") {
            need(5);
            usize at = place(t[1]) + unum(t[2]), len = unum(t[3]);
            auto seed = static_cast<std::uint32_t>(unum(t[4]));
            if (o == "kwrite") {
                danzi::watch::writing(at, len);
            }
            auto [w, s] = title_write(at, len, seed);
            log << "  wrote " << w << " skipped " << s << "\n";
        } else if (o == "wrote") {
            need(4);
            danzi::watch::wrote(place(t[1]) + unum(t[2]), unum(t[3]));
        } else if (o == "shadow") {
            need(5);
            usize start = place(t[2]) + unum(t[3]), len = unum(t[4]);
            auto* data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, std::max<usize>(len, 1), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!data) {
                throw Failure("host buffer");
            }
            auto s = std::make_unique<danzi::shadow::Shadow>(data, start, len);
            s->set_copy_observer(note_copy, this);
            copies.push_back(Copy{t[1], std::move(s)});
            if (!quiet) {
                log_state(t[1], 0, 0);
            }
        } else if (o == "card_shadow") {
            need(5);

            usize start = place(t[2]) + unum(t[3]), len = unum(t[4]);
            registry().register_buffer(++next_resource, start, len);
            buffers[t[1]] = {next_resource, start};
            auto s = registry().find(start, len);
            if (!s) {
                throw Failure("no copy");
            }
            log << "  carded " << (s->carded() ? 1 : 0) << "\n";
            keep(t[1], std::move(s));
            if (!quiet) {
                log_state(t[1], 0, 0);
            }
        } else if (o == "card_write") {
            need(5);
            auto& s = copy(t[1]);
            usize off = unum(t[2]), len = unum(t[3]);
            auto seed = static_cast<std::uint32_t>(unum(t[4]));
            usize total = s.len();
            usize from = std::min(off, total), to = std::min(danzi::saturating_add(off, len), total);
            std::vector<std::uint8_t> bytes(to - from);
            for (usize k = from; k < to; ++k) {
                bytes[k - from] = pattern(seed, k - off);
            }
            if (s.carded()) {
                card_writes.push_back({&s, from, std::move(bytes)});
            } else {


                std::memcpy(s.data() + from, bytes.data(), bytes.size());
            }
            log << "  gpu wrote " << (to - from) << "\n";
        } else if (o == "card_submit") {


            auto rs = ranges(t, 1);
            std::vector<danzi::gpu::GpuWrite> writes;
            std::vector<std::string> names;
            for (size_t i = 1, k = 0; i < t.size(); i += 3, ++k) {
                writes.push_back({&copy(t[i]), rs[k].first, rs[k].second});
                if (std::find(names.begin(), names.end(), t[i]) == names.end()) {
                    names.push_back(t[i]);
                }
            }
            auto back = declared(t, 1);
            auto& h = host();
            auto [title, source] = title_list();
            auto ns = h.submit(writes, title.list.Get(), back);
            timing << op_number << " card_before " << ns[0] << "\n";
            timing << op_number << " card_gpu " << ns[1] << "\n";
            timing << op_number << " card_back " << ns[2] << "\n";
            timing << op_number << " card_submit " << (ns[0] + ns[1] + ns[2]) << "\n";


            std::sort(title_writes.begin(), title_writes.end());
            log_writes();
            log_card_stats();
            if (!quiet) {
                for (const auto& name : names) {
                    log_state(name, 0, ~usize{0}, true);
                    log_card(name);
                }
            }
        } else if (o == "card_send") {

            auto rs = ranges(t, 1);
            std::vector<danzi::gpu::GpuWrite> writes;
            for (size_t i = 1, k = 0; i < t.size(); i += 3, ++k) {
                writes.push_back({&copy(t[i]), rs[k].first, rs[k].second});
            }
            auto back = declared(t, 1);
            auto& h = host();

            if (danzi::gpu::relay() && h.card_results_low()) {
                std::vector<danzi::gpu::Flight> all(std::make_move_iterator(flights.begin()), std::make_move_iterator(flights.end()));
                flights.clear();
                auto ns = h.land(std::move(all));
                timing << op_number << " card_land " << ns[1] << "\n";
                log << "  landed all first\n";
            }
            auto [title, source] = title_list();
            auto [flight, ns] = h.send(writes, std::move(title), source, std::move(back));
            flights.push_back(std::move(flight));
            timing << op_number << " card_before " << ns[0] << "\n";
            timing << op_number << " card_send " << (ns[0] + ns[1]) << "\n";
            log << "  in the air " << flights.size() << "\n";
        } else if (o == "card_land") {

            usize n = t.size() > 1 ? std::min<usize>(unum(t[1]), flights.size()) : flights.size();
            std::vector<danzi::gpu::Flight> landing;
            for (usize i = 0; i < n; ++i) {
                landing.push_back(std::move(flights.front()));
                flights.pop_front();
            }
            std::vector<std::string> names;
            for (const auto& f : landing) {
                for (const auto& c : copies) {
                    if (f.names(c.shadow.get()) && std::find(names.begin(), names.end(), c.name) == names.end()) {
                        names.push_back(c.name);
                    }
                }
            }
            auto ns = host().land(std::move(landing));
            timing << op_number << " card_wait " << ns[0] << "\n";
            timing << op_number << " card_land " << ns[1] << "\n";
            std::sort(title_writes.begin(), title_writes.end());
            log_writes();
            log_card_stats();
            if (!quiet) {
                for (const auto& name : names) {
                    log_state(name, 0, ~usize{0}, true);
                    log_card(name);
                }
            }

        } else if (o == "rebar_level") {
            need(2);
            auto v = t[1] == "-" ? std::nullopt : std::optional<std::string_view>(t[1]);
            log << "  level " << unsigned(danzi::gpu::rebar::level_of(v)) << "\n";
        } else if (o == "rebar_most") {
            need(2);
            auto b = t[1] == "-" ? std::nullopt : std::optional<std::uint64_t>(unum(t[1]));
            log << "  most " << danzi::gpu::rebar::default_most(b) << "\n";
        } else if (o == "rebar_decide") {
            need(10);
            danzi::gpu::rebar::Ask ask;
            ask.level = static_cast<std::uint8_t>(unum(t[1]));
            ask.supported = t[2] == "1";
            ask.kind = t[3] == "window" ? danzi::gpu::rebar::Kind::Window : danzi::gpu::rebar::Kind::Buffer;
            ask.size = unum(t[4]);
            ask.cpu_read = t[5] == "1";
            ask.placed = unum(t[6]);
            ask.most = unum(t[7]);
            if (t[8] != "-") {
                ask.card = std::make_pair<std::uint64_t, std::uint64_t>(unum(t[8]), unum(t[9]));
            }
            log << "  decision " << danzi::gpu::rebar::name(danzi::gpu::rebar::decide(ask)) << "\n";
        } else if (o == "ranges_insert") {
            need(3);
            range_set.insert(unum(t[1]), unum(t[2]));
        } else if (o == "ranges_remove") {
            need(3);
            log << "  removed " << (range_set.remove(unum(t[1]), unum(t[2])) ? 1 : 0) << "\n";
        } else if (o == "ranges_remove_at") {
            need(2);
            log << "  removed " << (range_set.remove_at(unum(t[1])) ? 1 : 0) << "\n";
        } else if (o == "ranges_overlaps") {
            need(3);
            std::uint64_t a = unum(t[1]), b = unum(t[2]);
            auto started = std::chrono::steady_clock::now();
            bool r = range_set.overlaps(a, b);
            timing << op_number << " ranges_overlaps "
                   << std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count() << "\n";
            log << "  overlaps " << (r ? 1 : 0) << " len " << range_set.size() << "\n";
        } else if (o == "lane_hash") {
            need(4);
            usize at = place(t[1]) + unum(t[2]), len = unum(t[3]);
            auto started = std::chrono::steady_clock::now();
            auto h = danzi::memcopy::lane_hash(reinterpret_cast<const std::uint8_t*>(at), len);
            timing << op_number << " lane_hash "
                   << std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count() << "\n";
            char hex[65];
            for (int i = 0; i < 32; ++i) {
                std::snprintf(hex + 2 * i, 3, "%02x", h[i]);
            }
            log << "  hash " << hex << "\n";
        } else if (o == "check_turn") {
            need(4);
            log << "  turn " << (danzi::memcopy::check_turn(unum(t[1]), unum(t[2]), unum(t[3])) ? 1 : 0) << "\n";
        } else if (o == "hash_every") {
            need(2);
            auto v = t[1] == "-" ? std::nullopt : std::optional<std::string_view>(t[1]);
            log << "  every " << danzi::memcopy::hash_every_of(v) << "\n";

        } else if (o == "buffer") {
            need(5);
            usize start = place(t[2]) + unum(t[3]), len = unum(t[4]);
            registry().register_buffer(++next_resource, start, len);
            buffers[t[1]] = {next_resource, start};
        } else if (o == "unbuffer") {
            need(2);
            auto it = buffers.find(t[1]);
            if (it == buffers.end()) {
                throw Failure("unknown buffer");
            }
            registry().unregister(it->second.first, it->second.second);
            buffers.erase(it);
        } else if (o == "find" || o == "find_any") {
            need(5);
            usize at = place(t[2]) + unum(t[3]), len = unum(t[4]);
            auto& reg = registry();
            auto started = std::chrono::steady_clock::now();
            auto s = o == "find" ? reg.find(at, len) : reg.find_any(at, len);
            timing << op_number << " " << o << " "
                   << std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count() << "\n";
            if (!s) {
                throw Failure("no copy");
            }
            log << "  found " << hex(s->start()) << "+" << hex(s->len()) << " window=" << (s->window() ? 1 : 0) << " carded=" << (s->carded() ? 1 : 0)
                << " rebar=" << (s->rebar() > 0 ? 1 : 0) << " placed=" << (s->placed() ? 1 : 0) << "\n";
            keep(t[1], std::move(s));
        } else if (o == "card_check") {
            need(2);
            log_card(t[1]);
        } else if (o == "refresh") {
            need(4);
            auto& s = copy(t[1]);
            usize at = static_cast<usize>(static_cast<long long>(s.start()) + num(t[2])), len = unum(t[3]);
            auto started = now_ns();
            usize n = s.update(at, len);
            timing << op_number << " refresh " << (now_ns() - started) << "\n";
            log_copies();
            log << "  ret " << n << "\n";
            if (!quiet) {
                log_state(t[1], at, danzi::saturating_add(at, len));
            }
        } else if (o == "flush") {

            auto rs = ranges(t, 1);
            std::vector<std::string> names;
            for (size_t i = 1; i < t.size(); i += 3) {
                names.push_back(t[i]);
            }
            bool ahead = rs.size() > 1;

            std::vector<danzi::shadow::Shadow*> shadows;
            shadows.reserve(names.size());
            for (const auto& name : names) {
                shadows.push_back(&copy(name));
            }
            auto started = now_ns();
            std::vector<std::pair<std::vector<std::pair<usize, usize>>, usize>> rets;
            rets.reserve(rs.size());
            if (ahead) {
                std::vector<danzi::watch::Range> asked;
                asked.reserve(rs.size());
                for (size_t i = 0; i < rs.size(); ++i) {
                    if (!shadows[i]->unwatched() && !shadows[i]->is_view_guarded()) {
                        asked.emplace_back(rs[i].first, rs[i].first + rs[i].second);
                    }
                }
                danzi::watch::ask_ahead(asked);
            }
            for (size_t i = 0; i < rs.size(); ++i) {
                usize n = shadows[i]->update(rs[i].first, rs[i].second);
                rets.emplace_back(std::move(copied), n);
                copied.clear();
            }
            if (ahead) {
                danzi::watch::ask_ahead_done();
            }
            timing << op_number << " flush " << (now_ns() - started) << "\n";
            for (size_t i = 0; i < rs.size(); ++i) {
                for (auto [a, l] : rets[i].first) {
                    log << "  copy " << hex(a) << " " << hex(l) << "\n";
                }
                log << "  ret " << names[i] << " " << rets[i].second << "\n";
            }
            if (!quiet) {
                for (size_t i = 0; i < rs.size(); ++i) {
                    log_state(names[i], rs[i].first, rs[i].first + rs[i].second);
                }
            }
        } else if (o == "stale") {
            need(4);
            auto& s = copy(t[1]);
            usize at = static_cast<usize>(static_cast<long long>(s.start()) + num(t[2])), len = unum(t[3]);
            auto started = now_ns();
            bool stale = s.stale_under_writes(at, len);
            timing << op_number << " stale " << (now_ns() - started) << "\n";
            log << "  stale " << (stale ? 1 : 0) << "\n";
        } else if (o == "prepare") {
            need(2);
            danzi::shadow::preparing(unum(t[1]));
        } else if (o == "gpu_declare") {
            need(4);
            auto& s = copy(t[1]);
            usize at = static_cast<usize>(static_cast<long long>(s.start()) + num(t[2])), len = unum(t[3]);
            s.writing(at, len);
            if (!quiet) {
                log_state(t[1], 0, 0);
            }
        } else if (o == "gpu_write") {
            need(5);
            auto& s = copy(t[1]);
            usize off = unum(t[2]), len = unum(t[3]);
            auto seed = static_cast<std::uint32_t>(unum(t[4]));
            usize total = s.len();
            usize from = std::min(off, total), to = std::min(danzi::saturating_add(off, len), total);
            for (usize k = from; k < to; ++k) {
                s.data()[k] = pattern(seed, k - off);
            }
            log << "  gpu wrote " << (to - from) << "\n";
        } else if (o == "gpu_done") {
            if (t.size() < 4 || (t.size() - 2) % 2 != 0) {
                throw Failure("gpu_done <shadow> <off> <len> [<off> <len>...]");
            }
            auto& s = copy(t[1]);
            auto start = static_cast<long long>(s.start());
            std::vector<danzi::shadow::Range> rs;
            for (size_t i = 2; i < t.size(); i += 2) {
                usize at = static_cast<usize>(start + num(t[i]));
                rs.emplace_back(at, at + unum(t[i + 1]));
            }
            s.copy_back_done(rs);
            if (!quiet) {
                log_state(t[1], 0, 0);
            }
        } else if (o == "ask_ahead" || o == "ask_once") {
            std::vector<danzi::watch::Range> rs;
            for (auto [a, l] : ranges(t, 1)) {
                rs.emplace_back(a, a + l);
            }
            if (o == "ask_ahead") {
                danzi::watch::ask_ahead(rs);
            } else {
                danzi::watch::ask_once(rs);
            }
        } else if (o == "snapshots") {
            auto started = now_ns();
            danzi::shadow::take_snapshots();
            timing << op_number << " snapshots " << (now_ns() - started) << "\n";
        } else if (o == "hand_back" || o == "relay_back") {
            auto back = declared(t, 1);
            auto started = now_ns();
            if (o == "hand_back") {
                danzi::shadow::hand_back(back);
            } else {
                danzi::shadow::relay_hand_back(back);
            }
            timing << op_number << " " << o << " " << (now_ns() - started) << "\n";
            log_writes();
            std::vector<std::string> names;
            for (size_t i = 1; i < t.size(); i += 3) {
                if (std::find(names.begin(), names.end(), t[i]) == names.end()) {
                    names.push_back(t[i]);
                }
            }
            if (!quiet) {
                for (const auto& name : names) {
                    log_state(name, 0, ~usize{0}, true);
                }
            }
        } else if (o == "copy_back") {
            need(4);
            auto& s = copy(t[1]);
            usize at = static_cast<usize>(static_cast<long long>(s.start()) + num(t[2])), len = unum(t[3]);
            auto started = now_ns();
            usize n = s.copy_back(at, len);
            timing << op_number << " copy_back " << (now_ns() - started) << "\n";
            log_writes();
            log << "  back " << n << "\n";
            if (!quiet) {
                log_state(t[1], 0, ~usize{0}, true);
            }
        } else if (o == "merge_under") {
            need(5);
            auto& s = copy(t[1]);
            usize at = static_cast<usize>(static_cast<long long>(s.start()) + num(t[2])), len = unum(t[3]);
            usize runs_left = unum(t[4]);
            auto started = now_ns();
            bool settle = s.merge_under_writes(at, len, runs_left);
            timing << op_number << " merge_under " << (now_ns() - started) << "\n";
            log_copies();
            log << "  settle " << (settle ? 1 : 0) << " runs_left " << runs_left << "\n";
            if (!quiet) {
                log_state(t[1], 0, ~usize{0}, true);
            }
        } else if (o == "ask_ahead_done") {
            danzi::watch::ask_ahead_done();
        } else if (o == "ask_once_end") {
            danzi::watch::ask_once_end();
        } else if (o == "check") {
            for (auto& c : copies) {
                log_state(c.name, 0, ~usize{0}, true);
            }
        } else if (o == "end") {
            for (auto& c : copies) {
                log_state(c.name, 0, ~usize{0}, true);
                std::ofstream f(out / ("shadow-" + c.name + ".bin"), std::ios::binary);
                f.write(reinterpret_cast<const char*>(c.shadow->data()), static_cast<std::streamsize>(c.shadow->len()));
                if (!f) {
                    throw Failure("cannot write shadow-" + c.name + ".bin");
                }
            }

            if (!quiet) {
                for (const auto& [name, at] : places) {
                    auto bytes = read_title(at, place_len[name]);
                    std::ofstream f(out / ("title-" + name + ".bin"), std::ios::binary);
                    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                }
            }
        } else {
            throw Failure("unknown operation " + o);
        }
    }
};

void note_copy(void* context, usize at, usize len) { static_cast<World*>(context)->copied.emplace_back(at, len); }

std::mutex WRITES_LOCK;
void note_write(void* context, usize at, usize len) {
    std::lock_guard lock(WRITES_LOCK);
    static_cast<World*>(context)->title_writes.emplace_back(at, len);
}

std::vector<std::vector<std::string>> parse(const std::string& text) {
    std::vector<std::vector<std::string>> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream words(line);
        std::vector<std::string> t;
        std::string w;
        while (words >> w) {
            t.push_back(w);
        }
        if (!t.empty()) {
            lines.push_back(std::move(t));
        }
    }
    return lines;
}

}   

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: dirty-run-cpp <scenario.txt> <out-dir>\n");
        return 2;
    }
    fs::path scenario = argv[1], out = argv[2];
    std::ifstream in(scenario, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "dirty-run-cpp: cannot read %s\n", argv[1]);
        return 1;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    fs::create_directories(out);
    auto lines = parse(text);

    for (const auto& t : lines) {
        if (t[0] == "env") {
            if (t.size() != 3) {
                std::fprintf(stderr, "dirty-run-cpp: env <NAME> <VALUE>\n");
                return 1;
            }
            SetEnvironmentVariableA(t[1].c_str(), t[2].c_str());
        }
    }
    World world;
    danzi::host::set_write_observer(note_write, &world);
    int result = 0;
    for (size_t n = 0; n < lines.size(); ++n) {
        const auto& t = lines[n];
        std::string joined;
        for (const auto& w : t) {
            joined += (joined.empty() ? "" : " ") + w;
        }
        world.log << "#" << n << " " << joined << "\n";
        world.op_number = n;
        try {
            world.op(t, out);
        } catch (const Failure& e) {
            world.log << "  error " << e.what() << "\n";
            std::fprintf(stderr, "dirty-run-cpp: operation %zu (%s): %s\n", n, joined.c_str(), e.what());
            result = 1;
            break;
        }
    }
    std::ofstream log(out / "log.txt", std::ios::binary);
    log << world.log.str();
    std::ofstream timing(out / "timing.txt", std::ios::binary);
    timing << world.timing.str();
    if (danzi::hybrid::enabled()) {
        auto h = danzi::hybrid::stats();
        std::ofstream(out / "hybrid.txt", std::ios::binary) << "faults " << h.faults << "\nmade_cold " << h.made_cold << "\nkernel_askings "
                                                           << h.kernel_askings << "\nkernel_pages " << h.kernel_pages << "\n";
    }
    return result;
}
