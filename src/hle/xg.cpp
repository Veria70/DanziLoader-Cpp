// SPDX-License-Identifier: GPL-3.0-or-later
// `xg_xs.dll`, the console's texture layout library (`victory-hle/src/
// modules/xg.rs`; DanziLoader / victory-perf, see NOTICE.md). Titles ask it
// which swizzle mode a texture should use, how large it is, where each mip
// and slice lies, and to copy linear data into tiled memory and back.
// Served from `formats::Texture`.
//
// The title imports `XGCreateTextureComputer` and `XGSuggestSwizzleMode`
// and calls these computer methods: Release (0x08), the layout (0x10),
// copies into (0x40) and out of (0x48) a subresource, and the 64 KiB tiling
// (0x50). The other slots log and fail. Debug log lines are not ported (a
// release build logs info and above).
#include <atomic>
#include <cstring>

#include "core/log.hpp"
#include "formats/texture.hpp"
#include "modules.hpp"

namespace danzi::hle::xg {

namespace {

using formats::Swizzle;

constexpr HRESULT_ E_FAIL_ = static_cast<HRESULT_>(0x80004005u);

constexpr std::uint32_t SWIZZLED = 0x100;
constexpr std::uint32_t ROW_MAJOR = 1;
constexpr std::uint32_t ALLOW_RENDER_TARGET = 1;
constexpr std::uint32_t ALLOW_DEPTH_STENCIL = 2;
constexpr std::uint32_t TEXTURE_1D = 2;
constexpr std::uint32_t TEXTURE_3D = 4;

}   


struct ResourceDesc {
    std::uint32_t dimension;
    std::uint64_t alignment;
    std::uint64_t width;
    std::uint32_t height;
    std::uint16_t depth_or_array;
    std::uint16_t mips;
    std::uint32_t format;
    std::uint32_t samples;
    std::uint32_t quality;
    std::uint32_t layout;
    std::uint32_t flags;
};
static_assert(sizeof(ResourceDesc) == 0x38);

namespace {

constexpr const char* T = "victory_hle::modules::xg";


std::string debug(const ResourceDesc& d) {
    std::string s = "ResourceDesc { dimension: " + log::debug(d.dimension);
    s += ", alignment: " + log::debug(d.alignment);
    s += ", width: " + log::debug(d.width);
    s += ", height: " + log::debug(d.height);
    s += ", depth_or_array: " + log::debug(std::uint32_t{d.depth_or_array});
    s += ", mips: " + log::debug(std::uint32_t{d.mips});
    s += ", format: " + log::debug(d.format);
    s += ", samples: " + log::debug(d.samples);
    s += ", quality: " + log::debug(d.quality);
    s += ", layout: " + log::debug(d.layout);
    s += ", flags: " + log::debug(d.flags) + " }";
    return s;
}


const char* debug(formats::Error e) {
    switch (e) {
        case formats::Error::Invalid: return "Invalid";
        case formats::Error::Unsupported: return "Unsupported";
        case formats::Error::TooSmall: return "TooSmall";
        default: return "None";
    }
}

}   

namespace {


std::uint32_t suggested(const ResourceDesc& desc) {
    if (desc.dimension == TEXTURE_1D) {
        return Swizzle::LINEAR;
    }
    if ((desc.flags & ALLOW_DEPTH_STENCIL) != 0) {
        return Swizzle::SW_64KB_Z_X;
    }
    if ((desc.flags & ALLOW_RENDER_TARGET) != 0) {
        return Swizzle::SW_64KB_R_X;
    }
    return Swizzle::SW_4KB_S;
}

}   


HRESULT_ XGSuggestSwizzleMode(const ResourceDesc* desc, std::uint32_t, std::uint32_t* out) {

    if (!desc || !out) {
        return E_INVALIDARG_;
    }
    *out = SWIZZLED | suggested(*desc);
    return S_OK_;
}

namespace {


constexpr std::size_t MIPS = 15;

struct Computer {
    const void* const* vtable;
    std::atomic<std::uint32_t> refs;
    ResourceDesc desc;
    std::uint32_t mips;
    formats::Texture texture;
};
using This = Computer*;

std::optional<formats::Texture> texture(const ResourceDesc& desc) {
    formats::Dimension dimension;
    switch (desc.dimension) {
        case TEXTURE_1D: dimension = formats::Dimension::Tex1d; break;
        case 3: dimension = formats::Dimension::Tex2d; break;
        case TEXTURE_3D: dimension = formats::Dimension::Tex3d; break;
        default: return std::nullopt;
    }
    Swizzle swizzle;
    if (desc.layout == 0) {
        swizzle.mode = suggested(desc);
    } else if (desc.layout == ROW_MAJOR) {
        swizzle.mode = Swizzle::LINEAR;
    } else if ((desc.layout & ~0xffu) == SWIZZLED) {
        swizzle.mode = desc.layout & 0xff;
    } else {
        return std::nullopt;
    }
    if (desc.width > UINT32_MAX) {
        return std::nullopt;
    }
    formats::TextureDesc td{dimension, static_cast<std::uint32_t>(desc.width), desc.height, desc.depth_or_array, desc.mips, desc.format, std::max(desc.samples, 1u),
                            swizzle};
    formats::Texture t;
    if (formats::Texture::make(td, t) != formats::Error::None) {
        return std::nullopt;
    }
    if (t.planes[0].layout.mips.size() > MIPS) {
        return std::nullopt;
    }
    return t;
}

}   


struct MipOut {
    std::uint64_t size;
    std::uint64_t offset;
    std::uint64_t slice_size;
    std::uint32_t pitch_texels;
    std::uint32_t pitch_bytes;
    std::uint32_t alignment;

    std::uint32_t padded[3];
    std::uint32_t elements[2];
    std::uint32_t texels[2];
    std::uint32_t depth_or_array;
    std::uint32_t samples;
    std::uint32_t swizzle;
    std::uint64_t bank_rotation[2];
    std::uint32_t slice_depth;
    std::uint32_t in_tail;

    std::uint32_t tail_position;
};

struct PlaneOut {

    std::uint32_t usage;
    std::uint64_t size;
    std::uint64_t slice_size;
    std::uint64_t offset;
    std::uint64_t alignment;
    std::uint32_t element_bytes;
    std::uint32_t standard_mips;
    std::uint32_t tail_mips;
    MipOut mips[MIPS];
};

struct LayoutOut {
    std::uint64_t size;
    std::uint64_t alignment;
    std::uint32_t mips;
    std::uint32_t planes;
    PlaneOut plane[4];
    std::uint32_t dimension;
};
static_assert(sizeof(MipOut) == 0x70);
static_assert(sizeof(PlaneOut) == 0x6c8);
static_assert(sizeof(LayoutOut) == 0x1b40);

namespace {


std::uint32_t tail_swizzle(Swizzle swizzle) {
    switch (swizzle.mode) {
        case 2:
        case 6:
        case 10:
        case 18:
        case 22:
        case 26: return Swizzle::SW_256B_D;
        case 23:
        case 27: return 3;
        case 24: return swizzle.mode;
        default: return Swizzle::SW_256B_S;
    }
}

void mip_out(const ResourceDesc& desc, const formats::Layout& l, std::size_t i, MipOut& out) {
    const formats::Mip& m = l.mips[i];
    std::uint64_t bpe = l.desc.element_bytes;
    std::uint32_t block = l.desc.block;
    bool three_d = desc.dimension == TEXTURE_3D;
    std::uint32_t array = std::max<std::uint32_t>(desc.depth_or_array, 1);
    std::memset(&out, 0, sizeof out);
    out.pitch_texels = m.pitch * block;
    out.pitch_bytes = m.pitch * l.desc.element_bytes;
    out.padded[0] = m.pitch;
    out.padded[1] = m.rows;
    out.padded[2] = three_d ? m.depth_pitch : array;
    out.elements[0] = m.width;
    out.elements[1] = m.height;
    out.texels[0] = std::max(static_cast<std::uint32_t>(desc.width >> i), 1u);
    out.texels[1] = std::max(desc.height >> i, 1u);
    out.depth_or_array = three_d ? m.depth : array;
    out.samples = l.desc.samples;
    out.slice_depth = l.block[2];
    std::uint64_t slice = std::uint64_t{m.pitch} * m.rows * bpe;
    std::uint64_t depth = three_d ? m.depth_pitch : 1;
    if (!m.tail) {
        out.offset = m.offset;
        out.slice_size = slice * l.desc.samples;
        out.size = out.slice_size * depth;
        out.alignment = static_cast<std::uint32_t>(l.alignment);
        out.swizzle = l.desc.swizzle.mode;
    } else {
        const formats::Tail& t = *m.tail;
        out.in_tail = 1;
        out.alignment = 256;
        out.swizzle = tail_swizzle(l.desc.swizzle);
        if (l.thick) {

            out.slice_size = l.alignment / l.block[2];
            out.tail_position = t.x | t.y << 11;
        } else {
            out.offset = t.offset;
            out.slice_size = slice;
        }
        out.size = out.slice_size * depth;
    }
}

std::uint32_t __stdcall add_ref(This self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

std::uint32_t __stdcall release(This self) {
    std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {

        delete self;
    }
    return left;
}

HRESULT_ __stdcall layout(This self, LayoutOut* out) {
    if (!out) {
        return E_INVALIDARG_;
    }
    const Computer& c = *self;


    std::memset(out, 0, sizeof *out);
    out->size = c.texture.size;
    out->alignment = c.texture.alignment;
    out->mips = c.mips;
    out->planes = static_cast<std::uint32_t>(c.texture.planes.size());
    out->dimension = c.desc.dimension;


    bool depth = (c.desc.flags & ALLOW_DEPTH_STENCIL) != 0 && (c.texture.planes.size() > 1 || c.mips == 1);
    for (std::size_t k = 0; k < 4 && k < c.texture.planes.size(); ++k) {
        PlaneOut& plane = out->plane[k];
        const formats::Plane& p = c.texture.planes[k];
        const formats::Layout& l = p.layout;
        plane.usage = p.stencil ? 8 : depth ? 7 : 1;
        plane.size = l.size;
        plane.slice_size = l.slice_size;
        plane.offset = p.offset;
        plane.alignment = l.alignment;
        plane.element_bytes = l.desc.element_bytes;
        plane.standard_mips = l.first_tail_mip;
        plane.tail_mips = c.mips - l.first_tail_mip;
        for (std::size_t i = 0; i < MIPS && i < l.mips.size(); ++i) {
            mip_out(c.desc, l, i, plane.mips[i]);
        }
    }
    return S_OK_;
}


struct CopyArgs {
    const formats::Layout* layout;
    std::uint32_t mip;
    std::uint32_t slice;
    std::size_t tiled;
    std::size_t linear;
};

std::optional<CopyArgs> copy_args(const Computer& c, std::uint32_t plane, std::uint32_t subresource, std::uint32_t row, std::uint32_t depth) {
    if (plane >= c.texture.planes.size()) {
        return std::nullopt;
    }
    const formats::Plane& p = c.texture.planes[plane];
    const formats::Layout& l = p.layout;
    std::uint32_t mip = subresource % c.mips, slice = subresource / c.mips;
    if (mip >= l.mips.size()) {
        return std::nullopt;
    }
    const formats::Mip& m = l.mips[mip];
    std::size_t bytes = std::size_t{m.width} * l.desc.element_bytes;
    std::size_t depths = l.subresource_depth(mip);
    std::size_t linear = (depths - 1) * depth + (std::size_t{m.height} - 1) * row + bytes;
    return CopyArgs{&l, mip, slice, static_cast<std::size_t>(p.offset + l.size), linear};
}

HRESULT_ result(formats::Error e) {
    switch (e) {
        case formats::Error::None: return S_OK_;
        case formats::Error::Unsupported: return E_NOTIMPL_;
        default: return E_INVALIDARG_;
    }
}


HRESULT_ __stdcall copy_into(This self, std::uint8_t* tiled, std::uint32_t plane, std::uint32_t subresource, const std::uint8_t* linear, std::uint32_t row,
                             std::uint32_t depth) {
    const Computer& c = *self;
    auto a = copy_args(c, plane, subresource, row, depth);
    if (!a) {
        return E_INVALIDARG_;
    }
    if (!tiled || !linear) {
        return E_INVALIDARG_;
    }

    auto offset = static_cast<std::size_t>(c.texture.planes[plane].offset);
    formats::Error r = a->layout->tile(tiled + offset, a->tiled - offset, a->mip, a->slice, linear, a->linear, row, depth);
    if (r != formats::Error::None) {
        log::warn(T, "copy into a texture failed",
                  {{"r", std::string("Err(") + debug(r) + ")"}, {"desc", debug(c.desc)}, {"subresource", log::debug(subresource)}});
    }
    return result(r);
}


HRESULT_ __stdcall copy_from(This self, std::uint8_t* linear, std::uint32_t plane, std::uint32_t subresource, std::uint32_t, const std::uint8_t* tiled,
                             std::uint32_t row, std::uint32_t depth) {
    const Computer& c = *self;
    auto a = copy_args(c, plane, subresource, row, depth);
    if (!a) {
        return E_INVALIDARG_;
    }
    if (!tiled || !linear) {
        return E_INVALIDARG_;
    }
    auto offset = static_cast<std::size_t>(c.texture.planes[plane].offset);
    formats::Error r = a->layout->detile(tiled + offset, a->tiled - offset, a->mip, a->slice, linear, a->linear, row, depth);
    if (r != formats::Error::None) {
        log::warn(T, "copy out of a texture failed",
                  {{"r", std::string("Err(") + debug(r) + ")"}, {"desc", debug(c.desc)}, {"subresource", log::debug(subresource)}});
    }
    return result(r);
}

}   

struct PackedMips {
    std::uint8_t standard;
    std::uint8_t packed;
    std::uint32_t tiles;
    std::uint32_t start;
};

struct SubresourceTiling {
    std::uint32_t width;
    std::uint16_t height;
    std::uint16_t depth;
    std::uint32_t start;
};

namespace {

constexpr std::uint64_t TILE = 64 << 10;


HRESULT_ __stdcall tiling(This self, std::uint32_t* tiles, PackedMips* packed, std::uint32_t (*shape)[3], std::uint32_t* count, std::uint32_t first,
                          SubresourceTiling* out) {
    const Computer& c = *self;
    const formats::Layout& l = c.texture.planes[0].layout;
    if (l.alignment != TILE) {
        return E_FAIL_;
    }
    if (l.thick) {
        return E_NOTIMPL_;
    }
    std::uint32_t standard = l.first_tail_mip;
    bool tail = standard < c.mips;
    std::uint64_t tail_tiles = standard == 0 ? l.slice_size / TILE : standard < c.mips ? l.mips[standard - 1].offset / TILE : 0;
    std::uint32_t block = l.desc.block;


    if (tiles) {
        *tiles = static_cast<std::uint32_t>(l.size / TILE);
    }
    if (packed) {
        *packed = PackedMips{static_cast<std::uint8_t>(standard), static_cast<std::uint8_t>(c.mips - standard), static_cast<std::uint32_t>(tail_tiles), 0};
    }
    if (shape) {
        (*shape)[0] = l.block[0] * block;
        (*shape)[1] = l.block[1] * block;
        (*shape)[2] = l.block[2];
    }
    if (count) {
        std::uint32_t slices = tail ? std::min(l.slices, 2u) : l.slices;
        std::uint32_t all = standard * slices;
        std::uint32_t wanted = std::min(all > first ? all - first : 0, *count);
        for (std::uint32_t i = 0; i < wanted; ++i) {
            std::uint32_t slice = (first + i) / standard, mip = (first + i) % standard;
            const formats::Mip& m = l.mips[mip];
            SubresourceTiling entry{};
            if (!(tail && slice > 0)) {
                entry = SubresourceTiling{m.pitch / l.block[0], static_cast<std::uint16_t>(m.rows / l.block[1]), 1,
                                          static_cast<std::uint32_t>((std::uint64_t{slice} * l.slice_size + m.offset) / TILE)};
            }
            out[i] = entry;
        }
        *count = wanted;
    }
    return S_OK_;
}


std::uint64_t __stdcall element_offset(This self, std::uint32_t plane, std::uint32_t mip, std::uint64_t x, std::uint32_t y, std::uint32_t z, std::uint32_t,
                                       std::uint32_t* bit) {
    if (bit) {

        *bit = 0;
    }
    if (plane >= self->texture.planes.size()) {
        return 0;
    }
    const formats::Plane& p = self->texture.planes[plane];
    auto x32 = x > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(x);
    std::uint64_t at = 0;
    if (formats::Error e = p.layout.offset(mip, x32, y, z, at); e != formats::Error::None) {
        log::warn(T, "texel outside its texture",
                  {{"e", debug(e)}, {"mip", log::debug(mip)}, {"x", log::debug(x32)}, {"y", log::debug(y)}, {"z", log::debug(z)}});
        return 0;
    }
    return p.offset + at;
}


HRESULT_ __stdcall unused(This self) {
    log::warn(T, "unimplemented texture computer method", {{"desc", debug(self->desc)}});
    return E_NOTIMPL_;
}

const void* const VTABLE[13] = {
    reinterpret_cast<const void*>(&add_ref),          
    reinterpret_cast<const void*>(&release),          
    reinterpret_cast<const void*>(&layout),           
    reinterpret_cast<const void*>(&unused),           
    reinterpret_cast<const void*>(&unused),           
    reinterpret_cast<const void*>(&unused),           
    reinterpret_cast<const void*>(&element_offset),   
    reinterpret_cast<const void*>(&unused),           
    reinterpret_cast<const void*>(&copy_into),        
    reinterpret_cast<const void*>(&copy_from),        
    reinterpret_cast<const void*>(&tiling),           
    reinterpret_cast<const void*>(&unused),           
    reinterpret_cast<const void*>(&unused),           
};

}   

HRESULT_ XGCreateTextureComputer(const ResourceDesc* desc, Computer** out) {

    if (!desc || !out) {
        return E_INVALIDARG_;
    }
    ResourceDesc d = *desc;
    auto t = texture(d);
    if (!t) {
        log::warn(T, "no texture layout", {{"desc", debug(d)}});
        return E_INVALIDARG_;
    }
    auto mips = static_cast<std::uint32_t>(t->planes[0].layout.mips.size());
    *out = new Computer{VTABLE, 1, d, mips, std::move(*t)};
    return S_OK_;
}

#define E(f) HleExport{#f, reinterpret_cast<std::uintptr_t>(&f)}
std::vector<HleExport> exports() { return {E(XGCreateTextureComputer), E(XGSuggestSwizzleMode)}; }
#undef E

}   
