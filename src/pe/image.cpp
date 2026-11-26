// SPDX-License-Identifier: GPL-3.0-or-later
// PE32+ images (`victory-pe/src/image.rs`; DanziLoader / victory-perf, see
// NOTICE.md), with the parts of the `object` crate (0.41, `read::pe`,
// `read::util`, `read::read_ref`; Apache-2.0 OR MIT) the reference reads
// them through: the same bounds checks, in the same order, and the same
// error texts. See image.hpp.
#include "image.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "layout.hpp"

namespace danzi::pe {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Data = std::span<const u8>;


struct ObjError {
    const char* text;
};
[[noreturn]] void fail(const char* text) { throw ObjError{text}; }

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
u64 le64(const u8* p) {
    u64 v;
    std::memcpy(&v, p, 8);
    return v;
}


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


struct Bytes {
    Data d;
    bool skip(std::size_t offset) {
        if (offset > d.size()) {
            d = {};
            return false;
        }
        d = d.subspan(offset);
        return true;
    }

    const u8* read(std::size_t n) {
        if (n > d.size()) {
            d = {};
            return nullptr;
        }
        const u8* p = d.data();
        d = d.subspan(n);
        return p;
    }
    std::optional<Data> read_bytes(std::size_t n) {
        if (n > d.size()) {
            d = {};
            return std::nullopt;
        }
        Data head = d.first(n);
        d = d.subspan(n);
        return head;
    }
    std::optional<Data> read_string() {
        const void* z = d.empty() ? nullptr : std::memchr(d.data(), 0, d.size());
        if (!z) {
            d = {};
            return std::nullopt;
        }
        std::size_t len = static_cast<std::size_t>(static_cast<const u8*>(z) - d.data());
        Data s = d.first(len);
        d = d.subspan(len + 1);
        return s;
    }
    std::optional<Data> read_string_at(std::size_t offset) const {
        Bytes b = *this;
        if (!b.skip(offset)) {
            return std::nullopt;
        }
        return b.read_string();
    }
};

std::string text(Data b) { return utf8_lossy(b); }


struct Section {
    const u8* p;
    u32 virtual_size() const { return le32(p + 8); }
    u32 virtual_address() const { return le32(p + 12); }
    u32 size_of_raw_data() const { return le32(p + 16); }
    u32 pointer_to_raw_data() const { return le32(p + 20); }
    u32 characteristics() const { return le32(p + 36); }
    Data raw_name() const {
        const void* z = std::memchr(p, 0, 8);
        return Data(p, z ? static_cast<std::size_t>(static_cast<const u8*>(z) - p) : 8);
    }

    std::pair<u32, u32> file_range() const { return {pointer_to_raw_data(), std::min(virtual_size(), size_of_raw_data())}; }
    std::optional<std::pair<u32, u32>> file_range_at(u32 va) const {
        if (va < virtual_address()) {
            return std::nullopt;
        }
        u32 offset = va - virtual_address();
        auto [section_offset, section_size] = file_range();
        if (offset < section_size) {
            u64 sum = u64{section_offset} + offset;
            if (sum > 0xFFFFFFFFu) {
                return std::nullopt;
            }
            return std::pair{static_cast<u32>(sum), section_size - offset};
        }
        return std::nullopt;
    }
    std::optional<Data> data_at(Data data, u32 va) const {
        auto r = file_range_at(va);
        if (!r) {
            return std::nullopt;
        }
        return read_bytes_at(data, r->first, r->second);
    }
    std::optional<std::pair<Data, u32>> data_containing(Data data, u32 va) const {
        if (va < virtual_address()) {
            return std::nullopt;
        }
        u32 offset = va - virtual_address();
        auto [section_offset, section_size] = file_range();
        if (offset < section_size) {
            auto d = read_bytes_at(data, section_offset, section_size);
            if (!d) {
                return std::nullopt;
            }
            return std::pair{*d, virtual_address()};
        }
        return std::nullopt;
    }
};

struct SectionTable {
    std::vector<Section> sections;
    std::optional<Data> pe_data_at(Data data, u32 va) const {
        for (const Section& s : sections) {
            if (auto r = s.data_at(data, va)) {
                return r;
            }
        }
        return std::nullopt;
    }
    std::optional<std::pair<Data, u32>> pe_data_containing(Data data, u32 va) const {
        for (const Section& s : sections) {
            if (auto r = s.data_containing(data, va)) {
                return r;
            }
        }
        return std::nullopt;
    }
};

struct DataDirectory {
    u32 virtual_address, size;

    Data data(Data file, const SectionTable& sections) const {
        auto d = sections.pe_data_at(file, virtual_address);
        if (!d) {
            fail("Invalid data dir virtual address");
        }
        if (size > d->size()) {
            fail("Invalid data dir size");
        }
        return d->first(size);
    }
};


struct PeFile {
    Data data;
    const u8* file_header;   
    const u8* optional;      
    std::vector<DataDirectory> directories;
    SectionTable sections;

    u16 machine() const { return le16(file_header); }
    u16 number_of_sections() const { return le16(file_header + 2); }
    u32 time_date_stamp() const { return le32(file_header + 4); }
    u16 size_of_optional_header() const { return le16(file_header + 16); }
    u16 characteristics() const { return le16(file_header + 18); }
    u32 address_of_entry_point() const { return le32(optional + 16); }
    u64 image_base() const { return le64(optional + 24); }
    u32 size_of_image() const { return le32(optional + 56); }
    u16 subsystem() const { return le16(optional + 68); }
    u16 dll_characteristics() const { return le16(optional + 70); }
    u64 size_of_stack_reserve() const { return le64(optional + 72); }
    u64 size_of_stack_commit() const { return le64(optional + 80); }
    u32 number_of_rva_and_sizes() const { return le32(optional + 108); }


    const DataDirectory* directory(std::size_t index) const {
        if (index >= directories.size() || directories[index].virtual_address == 0) {
            return nullptr;
        }
        return &directories[index];
    }

    static PeFile parse(Data data) {
        PeFile f;
        f.data = data;

        auto dos = read_bytes_at(data, 0, 64);
        if (!dos) {
            fail("Invalid DOS header size or alignment");
        }
        if (le16(dos->data()) != 0x5A4D) {
            fail("Invalid DOS magic");
        }
        u64 offset = le32(dos->data() + 0x3C);


        auto nt = read_bytes(data, offset, 4 + 20 + 112);
        if (!nt) {
            fail("Invalid PE headers offset or size");
        }
        if (le32(nt->data()) != 0x00004550) {
            fail("Invalid PE magic");
        }
        f.file_header = nt->data() + 4;
        f.optional = nt->data() + 24;
        if (le16(f.optional) != 0x20B) {
            fail("Invalid PE optional header magic");
        }
        if (f.size_of_optional_header() < 112) {
            fail("PE optional header size is too small");
        }
        u64 optional_size = u64{f.size_of_optional_header()} - 112;
        auto optional_data = read_bytes(data, offset, optional_size);
        if (!optional_data) {
            fail("Invalid PE optional header size");
        }

        u64 count = f.number_of_rva_and_sizes();
        if (count * 8 > optional_data->size()) {
            fail("Invalid PE number of RVA and sizes");
        }
        for (u64 i = 0; i < count; ++i) {
            const u8* e = optional_data->data() + i * 8;
            f.directories.push_back({le32(e), le32(e + 4)});
        }

        u64 n = f.number_of_sections();
        auto table = read_bytes_at(data, offset, n * 40);
        if (!table) {
            fail("Invalid COFF/PE section headers");
        }
        for (u64 i = 0; i < n; ++i) {
            f.sections.sections.push_back(Section{table->data() + i * 40});
        }
        return f;
    }
};


const char* file_kind(Data data) {
    auto magic = read_bytes_at(data, 0, 16);
    if (!magic) {
        fail("Could not read file magic");
    }
    const u8* m = magic->data();
    if (std::memcmp(m, "!<arch>\n", 8) == 0 || std::memcmp(m, "!<thin>\n", 8) == 0) {
        return "Archive";
    }
    if (m[0] == 'M' && m[1] == 'Z') {

        auto dos = read_bytes_at(data, 0, 64);
        if (!dos || le16(dos->data()) != 0x5A4D) {
            fail("Unknown MS-DOS file");
        }
        u64 off = le32(dos->data() + 0x3C);

        auto nt = read_bytes_at(data, off, 4 + 20 + 96);
        if (!nt || le32(nt->data()) != 0x00004550) {
            fail("Unknown MS-DOS file");
        }
        u16 opt = le16(nt->data() + 24);
        if (opt == 0x10B) {
            return "Pe32";
        }
        if (opt == 0x20B) {
            return "Pe64";
        }
        fail("Unknown MS-DOS file");
    }
    u16 first = le16(m);
    switch (first) {
        case 0x01c4:
        case 0xaa64:
        case 0xa641:
        case 0x01f0:
        case 0x01f1:
        case 0x01f2:
        case 0x014c:
        case 0x8664: return "Coff";
        default: break;
    }
    if (m[0] == 0 && m[1] == 0 && m[2] == 0xff && m[3] == 0xff && m[4] == 0 && m[5] == 0) {
        return "CoffImport";
    }
    if (m[0] == 0 && m[1] == 0 && m[2] == 0xff && m[3] == 0xff && m[4] == 2 && m[5] == 0) {

        static constexpr u8 BIGOBJ[16] = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B, 0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};
        auto id = read_bytes_at(data, 12, 16);
        if (id && std::memcmp(id->data(), BIGOBJ, 16) == 0) {
            return "CoffBig";
        }
        fail("Unknown anon object file");
    }
    fail("Unknown file magic");
}


bool thunk_ordinal(u64 t) { return (t & 0x8000000000000000ull) != 0; }
u32 thunk_address(u64 t) { return static_cast<u32>(t) & 0x7fffffffu; }


struct ImportTables {
    Bytes import_data;
    u32 import_address = 0, import_section_address = 0;
    Bytes name_data;
    u32 name_section_address = 0;
    bool delay = false;

    Data name(u32 address) const {
        auto s = name_data.read_string_at(address - name_section_address);
        if (!s) {
            fail("Invalid PE import descriptor name");
        }
        return *s;
    }
    Bytes thunks(u32 address) const {
        Bytes b = import_data;
        if (!b.skip(address - import_section_address)) {
            fail(delay ? "Invalid PE delay load import thunk table address" : "Invalid PE import thunk table address");
        }
        return b;
    }
    ImportSymbol import(u64 thunk) const {
        if (thunk_ordinal(thunk)) {
            return ImportSymbol::of_ordinal(static_cast<u16>(thunk));
        }
        Bytes b = name_data;
        if (!b.skip(thunk_address(thunk) - name_section_address)) {
            fail(delay ? "Invalid PE delay load import thunk address" : "Invalid PE import thunk address");
        }
        const u8* h = b.read(2);
        if (!h) {
            fail(delay ? "Missing PE delay load import thunk hint" : "Missing PE import thunk hint");
        }
        auto s = b.read_string();
        if (!s || s->empty()) {
            fail(delay ? "Missing PE delay load import thunk name" : "Missing PE import thunk name");
        }
        return ImportSymbol::named(le16(h), text(*s));
    }
};


std::vector<ImportEntry> entries(const ImportTables& t, Bytes thunks, u32 iat_rva, u32 size_of_image) {
    std::vector<ImportEntry> out;
    for (;;) {

        const u8* p = thunks.read(8);
        if (!p) {
            fail("Missing PE null import thunk");
        }
        u64 thunk = le64(p);
        if (thunk_address(thunk) == 0) {
            break;
        }
        u64 offset = static_cast<u64>(out.size()) * 8;
        bool ok = offset <= 0xFFFFFFFFu;
        u64 slot = u64{iat_rva} + offset;
        ok = ok && slot <= 0xFFFFFFFFu && slot + 8 <= 0xFFFFFFFFu && slot + 8 <= size_of_image;
        if (!ok) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "IAT at 0x%x runs outside the image", iat_rva);
            throw PeError(PeError::Malformed, buf);
        }
        out.push_back(ImportEntry{t.import(thunk), static_cast<u32>(slot)});
    }
    return out;
}

std::vector<ImportModule> parse_imports(const PeFile& f) {
    std::vector<ImportModule> modules;
    const DataDirectory* dd = f.directory(layout::dir::IMPORT);
    if (!dd) {
        return modules;
    }

    auto sec = f.sections.pe_data_containing(f.data, dd->virtual_address);
    if (!sec) {
        fail("Invalid import data dir virtual address");
    }
    ImportTables t;
    t.import_data = Bytes{sec->first};
    t.import_address = dd->virtual_address;
    t.import_section_address = sec->second;
    t.name_data = Bytes{sec->first};
    t.name_section_address = sec->second;
    auto descriptors = [&]() -> std::optional<Bytes> {
        Bytes b = t.import_data;
        if (!b.skip(t.import_address - t.import_section_address)) {
            return std::nullopt;
        }
        return b;
    };

    if (auto d = descriptors()) {
        if (const u8* first = d->read(20)) {
            bool null = le32(first) == 0 && le32(first + 4) == 0 && le32(first + 8) == 0 && le32(first + 12) == 0 && le32(first + 16) == 0;
            if (!null) {
                if (auto ns = f.sections.pe_data_containing(f.data, le32(first + 12))) {
                    t.name_data = Bytes{ns->first};
                    t.name_section_address = ns->second;
                }
            }
        }
    }
    auto d = descriptors();
    if (!d) {
        fail("Invalid PE import descriptor address");
    }
    u32 size_of_image = f.size_of_image();
    for (;;) {
        const u8* desc = d->read(20);
        if (!desc) {
            fail("Missing PE null import descriptor");
        }
        u32 original_first_thunk = le32(desc), name = le32(desc + 12), first_thunk = le32(desc + 16);
        if (original_first_thunk == 0 && le32(desc + 4) == 0 && le32(desc + 8) == 0 && name == 0 && first_thunk == 0) {
            break;
        }


        u32 iat_rva = first_thunk;
        u32 int_rva = original_first_thunk == 0 ? iat_rva : original_first_thunk;
        ImportModule m;
        m.dll = text(t.name(name));
        m.entries = entries(t, t.thunks(int_rva), iat_rva, size_of_image);
        modules.push_back(std::move(m));
    }
    return modules;
}

std::vector<ImportModule> parse_delay_imports(const PeFile& f) {
    std::vector<ImportModule> modules;
    const DataDirectory* dd = f.directory(layout::dir::DELAY_IMPORT);
    if (!dd) {
        return modules;
    }
    auto sec = f.sections.pe_data_containing(f.data, dd->virtual_address);
    if (!sec) {
        fail("Invalid import data dir virtual address");
    }
    ImportTables t;
    t.delay = true;
    t.import_data = Bytes{sec->first};
    t.import_address = dd->virtual_address;
    t.import_section_address = sec->second;
    t.name_data = t.import_data;
    t.name_section_address = sec->second;
    Bytes d = t.import_data;
    if (!d.skip(t.import_address - t.import_section_address)) {
        fail("Invalid PE delay-load import descriptor address");
    }
    u32 size_of_image = f.size_of_image();
    for (;;) {

        const u8* desc = d.read(32);
        if (!desc) {
            fail("Missing PE null delay-load import descriptor");
        }
        bool null = true;
        for (int k = 0; k < 8; ++k) {
            null = null && le32(desc + 4 * k) == 0;
        }
        if (null) {
            break;
        }
        u32 dll_name_rva = le32(desc + 4), iat_rva = le32(desc + 12), int_rva = le32(desc + 16);
        ImportModule m;
        m.dll = text(t.name(dll_name_rva));
        m.entries = entries(t, t.thunks(int_rva), iat_rva, size_of_image);
        modules.push_back(std::move(m));
    }
    return modules;
}

std::vector<ExportInfo> parse_exports(const PeFile& f) {
    std::vector<ExportInfo> out;
    const DataDirectory* dd = f.directory(layout::dir::EXPORT);
    if (!dd) {
        return out;
    }
    u32 va = dd->virtual_address;
    Data data = dd->data(f.data, f.sections);

    if (data.size() < 40) {
        fail("Invalid PE export dir size");
    }
    const u8* dir = data.data();
    u32 base = le32(dir + 16);
    if (base > 0xFFFF) {
        fail("Invalid PE export ordinal base");
    }
    auto ordinal_base = static_cast<u16>(base);
    u32 number_of_functions = le32(dir + 20), number_of_names = le32(dir + 24);
    u32 address_of_functions = le32(dir + 28), address_of_names = le32(dir + 32), address_of_name_ordinals = le32(dir + 36);
    Bytes all{data};
    auto slice_at = [&](u32 address, u64 count, std::size_t width) -> std::optional<Data> {
        u64 offset = static_cast<u32>(address - va);
        u64 size = count * width;
        if (offset > data.size() || size > data.size() - offset) {
            return std::nullopt;
        }
        return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    };
    Data addresses, names, name_ordinals;
    if (address_of_functions != 0) {
        u64 max_index = 0xFFFFu - ordinal_base;
        if (number_of_functions > max_index + 1) {
            fail("Invalid PE export number of functions");
        }
        auto a = slice_at(address_of_functions, number_of_functions, 4);
        if (!a) {
            fail("Invalid PE export address table");
        }
        addresses = *a;
    }
    if (address_of_names != 0) {
        if (address_of_name_ordinals == 0) {
            fail("Missing PE export ordinal table");
        }
        auto n = slice_at(address_of_names, number_of_names, 4);
        if (!n) {
            fail("Invalid PE export name pointer table");
        }
        auto o = slice_at(address_of_name_ordinals, number_of_names, 2);
        if (!o) {
            fail("Invalid PE export ordinal table");
        }
        names = *n;
        name_ordinals = *o;
    }

    struct Raw {
        u16 ordinal;
        std::optional<Data> name;
        bool forward;
        u32 address;
        Data library, symbol;
        bool by_ordinal;
        u16 forward_ordinal;
    };
    std::vector<Raw> raw;
    std::size_t count = addresses.size() / 4;
    raw.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        u32 address = le32(addresses.data() + i * 4);
        Raw r{static_cast<u16>(ordinal_base + i), std::nullopt, false, address, {}, {}, false, 0};

        u64 offset = static_cast<u32>(address - va);
        if (offset < data.size()) {
            auto fwd = all.read_string_at(static_cast<std::size_t>(offset));
            if (!fwd) {
                fail("Invalid PE forwarded export address");
            }
            const u8* b = fwd->data();
            std::size_t n = fwd->size();
            std::size_t dot = n;
            for (std::size_t k = n; k-- > 0;) {
                if (b[k] == '.') {
                    dot = k;
                    break;
                }
            }
            if (dot == n) {
                fail("Missing PE forwarded export separator");
            }
            r.forward = true;
            r.library = fwd->first(dot);
            Data rest = fwd->subspan(dot + 1);
            if (!rest.empty() && rest[0] == '#') {
                Data digits = rest.subspan(1);
                bool ok = !digits.empty();
                u32 value = 0;
                for (u8 c : digits) {
                    if (c < '0' || c > '9') {
                        ok = false;
                        break;
                    }
                    value = value * 10 + (c - '0');
                    if (value > 0xFFFF) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) {
                    fail("Invalid PE forwarded export ordinal");
                }
                r.by_ordinal = true;
                r.forward_ordinal = static_cast<u16>(value);
            } else if (rest.empty()) {
                fail("Missing PE forwarded export name");
            } else {
                r.symbol = rest;
            }
        }
        raw.push_back(r);
    }
    std::size_t name_count = std::min(names.size() / 4, name_ordinals.size() / 2);
    for (std::size_t k = 0; k < name_count; ++k) {
        u32 pointer = le32(names.data() + k * 4);
        u16 index = le16(name_ordinals.data() + k * 2);
        auto name = all.read_string_at(static_cast<u32>(pointer - va));
        if (!name) {
            fail("Invalid PE export name pointer");
        }
        if (index >= raw.size()) {
            fail("Invalid PE export ordinal");
        }
        raw[index].name = *name;
    }
    for (const Raw& r : raw) {

        if (!r.forward && r.address == 0) {
            continue;
        }
        ExportInfo e;
        if (r.name) {
            e.name = text(*r.name);
        }
        e.ordinal = r.ordinal;
        if (r.forward) {
            e.forward = r.by_ordinal ? text(r.library) + ".#" + std::to_string(r.forward_ordinal) : text(r.library) + "." + text(r.symbol);
        } else {
            e.rva = r.address;
        }
        out.push_back(std::move(e));
    }
    return out;
}

u32 va_to_rva(u64 va, u64 image_base) {
    if (va < image_base || va - image_base > 0xFFFFFFFFu) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "VA 0x%llx is outside the image", static_cast<unsigned long long>(va));
        throw PeError(PeError::Malformed, buf);
    }
    return static_cast<u32>(va - image_base);
}

constexpr std::size_t MAX_TLS_CALLBACKS = 1024;

std::optional<TlsInfo> parse_tls(const PeFile& f) {
    const DataDirectory* dd = f.directory(layout::dir::TLS);
    if (!dd || dd->virtual_address == 0) {
        return std::nullopt;
    }
    u32 directory_rva = dd->virtual_address;
    auto tls = f.sections.pe_data_at(f.data, directory_rva);
    if (!tls) {
        throw PeError(PeError::Malformed, "TLS directory outside of sections");
    }
    auto callbacks_va = layout::read_u64(*tls, layout::TLS64_ADDRESS_OF_CALLBACKS);
    if (!callbacks_va) {
        throw PeError(PeError::Malformed, "truncated TLS directory");
    }
    TlsInfo info{directory_rva, *callbacks_va, {}};
    if (*callbacks_va != 0) {
        u32 array_rva = va_to_rva(*callbacks_va, f.image_base());
        auto array = f.sections.pe_data_at(f.data, array_rva);
        if (!array) {
            throw PeError(PeError::Malformed, "TLS callback array outside of sections");
        }
        std::size_t slots = std::min(array->size() / 8, MAX_TLS_CALLBACKS);
        for (std::size_t k = 0; k < slots; ++k) {
            u64 va = le64(array->data() + k * 8);
            if (va == 0) {
                break;
            }
            info.callback_rvas.push_back(va_to_rva(va, f.image_base()));
        }
    }
    return info;
}

std::optional<CodeViewInfo> parse_codeview(const PeFile& f) {
    const DataDirectory* dd = f.directory(layout::dir::DEBUG);
    if (!dd) {
        return std::nullopt;
    }
    Data debug = dd->data(f.data, f.sections);
    if (debug.size() % 28 != 0) {
        fail("Invalid PE debug dir size");
    }
    for (std::size_t k = 0; k < debug.size() / 28; ++k) {
        const u8* d = debug.data() + k * 28;
        if (le32(d + 12) != 2) {
            continue;
        }
        auto info_data = read_bytes_at(f.data, le32(d + 24), le32(d + 16));
        if (!info_data) {
            fail("Invalid CodeView Info address");
        }
        Bytes info{*info_data};
        auto sig = info.read_bytes(4);
        if (!sig) {
            fail("Invalid CodeView signature");
        }
        if (std::memcmp(sig->data(), "RSDS", 4) != 0) {
            continue;
        }
        auto guid = info.read_bytes(16);
        if (!guid) {
            fail("Invalid CodeView GUID");
        }
        const u8* age = info.read(4);
        if (!age) {
            fail("Invalid CodeView Age");
        }
        auto path = info.read_string();
        if (!path) {
            fail("Invalid CodeView file path");
        }
        CodeViewInfo cv;
        cv.pdb_path = text(*path);
        std::memcpy(cv.guid.data(), guid->data(), 16);
        cv.age = le32(age);
        return cv;
    }
    return std::nullopt;
}

}   

std::string utf8_lossy(std::span<const std::uint8_t> s) {
    std::string out;
    out.reserve(s.size());
    static constexpr char REPLACEMENT[] = "\xEF\xBF\xBD";
    std::size_t i = 0, n = s.size();
    auto cont = [&](std::size_t k) { return k < n && (s[k] & 0xC0) == 0x80; };
    while (i < n) {
        u8 b = s[i];
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
            ++i;
            continue;
        }
        std::size_t start = i, len = 0;
        bool bad = false;
        if (b >= 0xC2 && b <= 0xDF) {
            if (cont(i + 1)) {
                len = 2;
            } else {
                bad = true;
                len = 1;
            }
        } else if (b >= 0xE0 && b <= 0xEF) {
            bool second = i + 1 < n && ((b == 0xE0 && s[i + 1] >= 0xA0 && s[i + 1] <= 0xBF) || (b >= 0xE1 && b <= 0xEC && cont(i + 1)) ||
                                        (b == 0xED && s[i + 1] >= 0x80 && s[i + 1] <= 0x9F) || (b >= 0xEE && cont(i + 1)));
            if (!second) {
                bad = true;
                len = 1;
            } else if (!cont(i + 2)) {
                bad = true;
                len = 2;
            } else {
                len = 3;
            }
        } else if (b >= 0xF0 && b <= 0xF4) {
            bool second = i + 1 < n && ((b == 0xF0 && s[i + 1] >= 0x90 && s[i + 1] <= 0xBF) || (b >= 0xF1 && b <= 0xF3 && cont(i + 1)) ||
                                        (b == 0xF4 && s[i + 1] >= 0x80 && s[i + 1] <= 0x8F));
            if (!second) {
                bad = true;
                len = 1;
            } else if (!cont(i + 2)) {
                bad = true;
                len = 2;
            } else if (!cont(i + 3)) {
                bad = true;
                len = 3;
            } else {
                len = 4;
            }
        } else {
            bad = true;
            len = 1;
        }
        if (bad) {
            out += REPLACEMENT;
        } else {
            out.append(reinterpret_cast<const char*>(s.data() + start), len);
        }
        i = start + len;
    }
    return out;
}

std::string CodeViewInfo::symbol_key() const {
    const auto& g = guid;
    u32 d1 = le32(g.data());
    u16 d2 = le16(g.data() + 4), d3 = le16(g.data() + 6);
    char buf[80];
    int n = std::snprintf(buf, sizeof buf, "%08X%04X%04X", d1, d2, d3);
    std::string s(buf, static_cast<std::size_t>(n));
    for (std::size_t k = 8; k < 16; ++k) {
        std::snprintf(buf, sizeof buf, "%02X", g[k]);
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "%X", age);
    s += buf;
    return s;
}

ImageInfo ImageInfo::parse(std::span<const std::uint8_t> data) {
    try {
        const char* kind = file_kind(data);
        if (std::strcmp(kind, "Pe64") != 0) {
            throw PeError(PeError::Unsupported, std::string("expected PE32+, found ") + kind);
        }
        PeFile f = PeFile::parse(data);
        u16 machine = f.machine();
        if (machine != MACHINE_AMD64) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "machine 0x%04x is not AMD64", machine);
            throw PeError(PeError::Unsupported, buf);
        }
        ImageInfo info;
        for (const Section& s : f.sections.sections) {
            info.sections.push_back(SectionInfo{text(s.raw_name()), s.virtual_address(), s.virtual_size(), s.pointer_to_raw_data(),
                                                s.size_of_raw_data(), s.characteristics()});
        }
        auto present = [&](std::size_t index) {
            const DataDirectory* d = f.directory(index);
            return d && d->virtual_address != 0 && d->size != 0;
        };
        info.imports = parse_imports(f);
        info.delay_imports = parse_delay_imports(f);
        info.exports = parse_exports(f);
        info.tls = parse_tls(f);
        info.codeview = parse_codeview(f);
        info.machine = machine;
        info.characteristics = f.characteristics();
        info.timestamp = f.time_date_stamp();
        info.subsystem = f.subsystem();
        info.dll_characteristics = f.dll_characteristics();
        info.image_base = f.image_base();
        info.size_of_image = f.size_of_image();
        info.entry_rva = f.address_of_entry_point();
        info.stack_reserve = f.size_of_stack_reserve();
        info.stack_commit = f.size_of_stack_commit();
        info.has_relocations = present(layout::dir::BASERELOC) && (f.characteristics() & layout::file_flags::RELOCS_STRIPPED) == 0;
        info.has_exception_table = present(layout::dir::EXCEPTION);
        info.has_load_config = present(layout::dir::LOAD_CONFIG);
        return info;
    } catch (const ObjError& e) {
        throw PeError(PeError::Malformed, e.text);
    }
}

bool ImageInfo::is_dll() const { return (characteristics & layout::file_flags::DLL) != 0; }

std::optional<std::uint32_t> ImageInfo::rva_to_file_offset(std::uint32_t rva) const {
    for (const SectionInfo& s : sections) {
        if (rva < s.rva) {
            continue;
        }
        u32 delta = rva - s.rva;


        u32 backed = s.virtual_size == 0 ? s.file_size : std::min(s.virtual_size, s.file_size);
        if (delta < backed) {


            u64 off = u64{s.file_offset} + delta;
            if (off <= 0xFFFFFFFFu) {
                return static_cast<u32>(off);
            }
        }
    }
    return std::nullopt;
}

std::size_t ImageInfo::import_count() const {
    std::size_t n = 0;
    for (const auto& m : imports) {
        n += m.entries.size();
    }
    return n;
}

const char* ImageInfo::subsystem_name() const {
    switch (subsystem) {
        case layout::subsystem::WINDOWS_GUI: return "windows-gui";
        case layout::subsystem::WINDOWS_CUI: return "windows-cui";
        case layout::subsystem::XBOX: return "xbox";
        case layout::subsystem::XBOX_CODE_CATALOG: return "xbox-code-catalog";
        default: return "other";
    }
}

}   
