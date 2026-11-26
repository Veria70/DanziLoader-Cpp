// SPDX-License-Identifier: GPL-3.0-or-later
// COFF import libraries (`victory-pe/src/implib.rs`; DanziLoader /
// victory-perf, see NOTICE.md), with the `object` crate's archive and
// import-object reading (0.41; Apache-2.0 OR MIT). See implib.hpp.
#include "implib.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

namespace danzi::pe {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Data = std::span<const u8>;

[[noreturn]] void malformed(const char* text) { throw PeError(PeError::Malformed, text); }

std::optional<Data> read_bytes_at(Data data, u64 offset, u64 size) {
    if (size == 0) {
        return Data{};
    }
    if (offset > data.size() || size > data.size() - offset) {
        return std::nullopt;
    }
    return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
}
std::optional<Data> read_bytes(Data data, u64& offset, u64 size) {
    auto r = read_bytes_at(data, offset, size);
    if (r) {
        offset += size;
    }
    return r;
}


std::optional<u64> digits(Data d) {
    if (!d.empty() && d[0] == ' ') {
        return std::nullopt;
    }
    u64 r = 0;
    for (u8 c : d) {
        if (c == ' ') {
            return r;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        if (r > (UINT64_MAX - (c - '0')) / 10) {
            return std::nullopt;
        }
        r = r * 10 + (c - '0');
    }
    return r;
}

bool is_digit(u8 c) { return c >= '0' && c <= '9'; }

struct Member {
    Data name;
    u64 offset = 0, size = 0;

    Data data(Data file) const {
        if (offset == 0) {
            return {};
        }
        auto d = read_bytes_at(file, offset, size);
        if (!d) {
            malformed("Archive member size is too large");
        }
        return *d;
    }
};


Member parse_member(Data file, u64& offset, Data names, bool thin) {
    auto h = read_bytes(file, offset, 60);
    if (!h) {
        malformed("Invalid archive member header");
    }
    const u8* header = h->data();
    if (header[58] != '`' || header[59] != '\n') {
        malformed("Invalid archive terminator");
    }
    auto header_size = digits(Data(header + 48, 10));
    if (!header_size) {
        malformed("Invalid archive member size");
    }
    u64 file_offset = offset, file_size = *header_size;
    Data hname(header, 16);
    Data name;
    if (hname[0] == '/' && is_digit(hname[1])) {

        auto at = digits(hname.subspan(1));
        bool ok = at && *at <= names.size();
        if (ok) {
            Data rest = names.subspan(static_cast<std::size_t>(*at));
            std::size_t len = rest.size();
            for (std::size_t k = 0; k < rest.size(); ++k) {
                if (rest[k] == '\n' || rest[k] == 0) {
                    len = k;
                    break;
                }
            }
            if (len == rest.size()) {
                ok = false;
            } else if (rest[len] == '\n') {
                ok = len >= 1 && rest[len - 1] == '/';
                name = ok ? rest.first(len - 1) : Data{};
            } else {
                name = rest.first(len);
            }
        }
        if (!ok) {
            malformed("Invalid archive extended name offset");
        }
    } else if (std::memcmp(hname.data(), "#1/", 3) == 0 && is_digit(hname[3])) {

        auto len = digits(hname.subspan(3));
        bool ok = len && *len <= file_size;
        std::optional<Data> nd;
        if (ok) {
            file_size -= *len;
            nd = read_bytes(file, file_offset, *len);
            ok = nd.has_value();
        }
        if (!ok) {
            malformed("Invalid archive extended name length");
        }
        const void* z = nd->empty() ? nullptr : std::memchr(nd->data(), 0, nd->size());
        name = z ? nd->first(static_cast<std::size_t>(static_cast<const u8*>(z) - nd->data())) : *nd;
    } else if (hname[0] == '/') {
        const void* sp = std::memchr(hname.data(), ' ', 16);
        name = hname.first(sp ? static_cast<std::size_t>(static_cast<const u8*>(sp) - hname.data()) : 16);
    } else {
        const void* end = std::memchr(hname.data(), '/', 16);
        if (!end) {
            end = std::memchr(hname.data(), ' ', 16);
        }
        name = hname.first(end ? static_cast<std::size_t>(static_cast<const u8*>(end) - hname.data()) : 16);
    }
    auto is = [&](const char* s) { return name.size() == std::strlen(s) && std::memcmp(name.data(), s, name.size()) == 0; };
    if (thin && !is("/") && !is("//") && !is("/SYM64/")) {
        return Member{name, 0, file_size};
    }
    if (offset > UINT64_MAX - *header_size) {
        malformed("Archive member size is too large");
    }
    offset += *header_size;
    if ((*header_size & 1) != 0 && offset != UINT64_MAX) {
        offset += 1;
    }
    return Member{name, file_offset, file_size};
}

bool named(const Member& m, const char* s) { return m.name.size() == std::strlen(s) && std::memcmp(m.name.data(), s, m.name.size()) == 0; }


struct Archive {
    u64 members_offset = 0, end = 0;
    Data names;
    bool thin = false;
};

Archive parse_archive(Data file) {
    u64 len = file.size();
    u64 tail = 0;
    auto magic = read_bytes(file, tail, 8);
    if (!magic) {
        malformed("Invalid archive size");
    }
    if (std::memcmp(magic->data(), "<bigaf>\n", 8) == 0) {
        malformed("port: AIX big archives are not read");
    }
    static constexpr u8 ZOS[8] = {0x5A, 0x4C, 0x81, 0x99, 0x83, 0x88, 0x6E, 0x15};
    if (std::memcmp(magic->data(), ZOS, 8) == 0) {
        malformed("port: z/OS archives are not read");
    }
    Archive a;
    if (std::memcmp(magic->data(), "!<thin>\n", 8) == 0) {
        a.thin = true;
    } else if (std::memcmp(magic->data(), "!<arch>\n", 8) != 0) {
        malformed("Unsupported archive identifier");
    }
    a.members_offset = tail;
    a.end = len;
    auto next = [&](u64& t) -> std::optional<Member> {
        if (t < len) {
            return parse_member(file, t, a.names, a.thin);
        }
        return std::nullopt;
    };
    if (auto m = next(tail)) {
        if (named(*m, "/")) {
            a.members_offset = tail;
            if (auto m2 = next(tail)) {
                if (named(*m2, "//")) {
                    a.names = m2->data(file);
                    a.members_offset = tail;
                } else if (named(*m2, "/")) {
                    a.members_offset = tail;
                    auto m3 = next(tail);
                    if (m3 && named(*m3, "//")) {
                        a.names = m3->data(file);
                        a.members_offset = tail;
                        m3 = next(tail);
                    }
                    if (m3 && named(*m3, "/<ECSYMBOLS>/")) {
                        a.members_offset = tail;
                    }
                }
            }
        } else if (named(*m, "/SYM64/")) {
            a.members_offset = tail;
            if (auto m2 = next(tail)) {
                if (named(*m2, "//")) {
                    a.names = m2->data(file);
                    a.members_offset = tail;
                }
            }
        } else if (named(*m, "//")) {
            a.names = m->data(file);
            a.members_offset = tail;
        } else if (named(*m, "__.SYMDEF") || named(*m, "__.SYMDEF SORTED") || named(*m, "__.SYMDEF_64") || named(*m, "__.SYMDEF_64 SORTED")) {
            a.members_offset = tail;
        }
    }
    return a;
}

u16 le16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}
u32 le32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}


std::optional<LibImport> import_object(Data bytes) {
    if (bytes.size() < 20) {
        return std::nullopt;
    }
    const u8* h = bytes.data();
    if (le16(h) != 0 || le16(h + 2) != 0xFFFF || le16(h + 4) != 0) {
        return std::nullopt;
    }
    u32 size = le32(h + 12);
    if (size > bytes.size() - 20) {
        return std::nullopt;
    }
    Data data = bytes.subspan(20, size);
    auto string = [&](Data& d) -> std::optional<Data> {
        const void* z = d.empty() ? nullptr : std::memchr(d.data(), 0, d.size());
        if (!z) {
            d = {};
            return std::nullopt;
        }
        std::size_t n = static_cast<std::size_t>(static_cast<const u8*>(z) - d.data());
        Data s = d.first(n);
        d = d.subspan(n + 1);
        return s;
    };
    u16 name_type_field = le16(h + 18);
    u16 type = name_type_field & 3, name_type = (name_type_field >> 2) & 7;
    auto symbol = string(data);
    if (!symbol) {
        return std::nullopt;
    }
    auto dll = string(data);
    if (!dll) {
        return std::nullopt;
    }
    std::optional<Data> export_name;
    if (name_type == 4) {
        export_name = string(data);
        if (!export_name) {
            return std::nullopt;
        }
    }
    LibImport li;
    switch (type) {
        case 0: li.kind = LibImport::Code; break;
        case 1: li.kind = LibImport::Data; break;
        case 2: li.kind = LibImport::Const; break;
        default: return std::nullopt;
    }
    auto strip_prefix = [](Data s) {
        if (!s.empty() && (s[0] == '?' || s[0] == '@' || s[0] == '_')) {
            return s.subspan(1);
        }
        return s;
    };
    std::optional<Data> import;
    switch (name_type) {
        case 0: break;
        case 1: import = *symbol; break;
        case 2: import = strip_prefix(*symbol); break;
        case 3: {
            Data s = strip_prefix(*symbol);
            const void* at = s.empty() ? nullptr : std::memchr(s.data(), '@', s.size());
            import = at ? s.first(static_cast<std::size_t>(static_cast<const u8*>(at) - s.data())) : s;
            break;
        }
        case 4: import = export_name; break;
        default: return std::nullopt;
    }
    li.dll = utf8_lossy(*dll);
    li.export_ = import ? ImportSymbol::named(0, utf8_lossy(*import)) : ImportSymbol::of_ordinal(le16(h + 16));
    return li;
}

}   

ImportLibrary ImportLibrary::parse(std::span<const std::uint8_t> data) {
    Archive a = parse_archive(data);
    ImportLibrary lib;
    u64 offset = a.members_offset;
    while (offset < a.end) {
        Member m = parse_member(data, offset, a.names, a.thin);
        Data bytes = m.data(data);
        if (auto li = import_object(bytes)) {
            lib.imports.push_back(std::move(*li));
        } else {
            ++lib.other_members;
        }
    }
    std::sort(lib.imports.begin(), lib.imports.end());
    lib.imports.erase(std::unique(lib.imports.begin(), lib.imports.end()), lib.imports.end());
    return lib;
}

}   
