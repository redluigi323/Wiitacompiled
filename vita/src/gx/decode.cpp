#include "mkwv/gx/decode.hpp"

#include "mkwv/gx/perf.hpp"
#include "mkwv/gx/vertex_tuning.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace mkwv::gx {
namespace { bool g_prepared_texgen = true; }
void set_prepared_texgen(bool enabled) { g_prepared_texgen=enabled; }
bool prepared_texgen_enabled() { return g_prepared_texgen; }
namespace {

void store_u16(void* dst, std::uint16_t value) {
    std::memcpy(dst, &value, sizeof(value));
}

std::uint16_t load_u16(const void* src) {
    std::uint16_t value = 0;
    std::memcpy(&value, src, sizeof(value));
    return value;
}

std::uint32_t load_u32(const void* src) {
    std::uint32_t value = 0;
    std::memcpy(&value, src, sizeof(value));
    return value;
}

std::uint16_t bswap16(std::uint16_t value) {
    return static_cast<std::uint16_t>((value << 8) | (value >> 8));
}

std::uint32_t bswap32(std::uint32_t value) {
    return (value << 24) | ((value << 8) & 0x00FF0000u) | ((value >> 8) & 0x0000FF00u) | (value >> 24);
}

std::uint16_t read_u16(const std::uint8_t* p, bool be) {
    const std::uint16_t v = load_u16(p);
    return be ? bswap16(v) : v;
}

std::uint32_t read_u32(const std::uint8_t* p, bool be) {
    const std::uint32_t v = load_u32(p);
    return be ? bswap32(v) : v;
}

float read_f32(const std::uint8_t* p, bool be) {
    const std::uint32_t bits = read_u32(p, be);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool array_be(const State& state, Attr attr, bool fifo_be) {
    return state.arrays[attr].le ? false : fifo_be;
}

std::uint8_t expand5(std::uint32_t v) {
    return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}

std::uint8_t expand6(std::uint32_t v) {
    return static_cast<std::uint8_t>((v << 2) | (v >> 4));
}

std::uint8_t expand4(std::uint32_t v) {
    return static_cast<std::uint8_t>((v << 4) | v);
}

std::uint32_t decode_color(const std::uint8_t*& p, CompType type, bool be) {
    switch (type) {
    case RGB565: {
        const std::uint16_t c = read_u16(p, be);
        p += 2;
        return pack_rgba(expand5((c >> 11) & 31u), expand6((c >> 5) & 63u), expand5(c & 31u), 255);
    }
    case RGB8: {
        const std::uint8_t r = *p++;
        const std::uint8_t g = *p++;
        const std::uint8_t b = *p++;
        return pack_rgba(r, g, b, 255);
    }
    case RGBX8: {
        const std::uint8_t r = *p++;
        const std::uint8_t g = *p++;
        const std::uint8_t b = *p++;
        ++p;
        return pack_rgba(r, g, b, 255);
    }
    case RGBA4: {
        const std::uint16_t c = read_u16(p, be);
        p += 2;
        return pack_rgba(expand4((c >> 12) & 15u), expand4((c >> 8) & 15u), expand4((c >> 4) & 15u), expand4(c & 15u));
    }
    case RGBA6: {
        const std::uint32_t c = (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                static_cast<std::uint32_t>(p[2]);
        p += 3;
        const auto expand6bit = [](std::uint32_t v) -> std::uint8_t {
            return static_cast<std::uint8_t>((v << 2) | (v >> 4));
        };
        return pack_rgba(expand6bit((c >> 18) & 63u), expand6bit((c >> 12) & 63u), expand6bit((c >> 6) & 63u),
                         expand6bit(c & 63u));
    }
    case RGBA8: {
        const std::uint8_t r = *p++;
        const std::uint8_t g = *p++;
        const std::uint8_t b = *p++;
        const std::uint8_t a = *p++;
        return pack_rgba(r, g, b, a);
    }
    default:
        return pack_rgba(255, 255, 255, 255);
    }
}

} // namespace

std::uint8_t comp_type_size(Attr attr, CompType type) {
    switch (attr) {
    case AttrPnMtxIdx:
    case AttrTex0MtxIdx:
    case AttrTex1MtxIdx:
    case AttrTex2MtxIdx:
    case AttrTex3MtxIdx:
    case AttrTex4MtxIdx:
    case AttrTex5MtxIdx:
    case AttrTex6MtxIdx:
    case AttrTex7MtxIdx:
        return 1;
    case AttrClr0:
    case AttrClr1:
        switch (type) {
        case RGB565:
        case RGBA4:
            return 2;
        case RGB8:
        case RGBA6:
            return 3;
        case RGBX8:
        case RGBA8:
            return 4;
        default:
            return 4;
        }
    default:
        switch (type) {
        case U8:
        case S8:
            return 1;
        case U16:
        case S16:
            return 2;
        case F32:
            return 4;
        default:
            return 4;
        }
    }
}

std::uint8_t comp_cnt_count(Attr attr, CompCnt cnt) {
    switch (attr) {
    case AttrPnMtxIdx:
    case AttrTex0MtxIdx:
    case AttrTex1MtxIdx:
    case AttrTex2MtxIdx:
    case AttrTex3MtxIdx:
    case AttrTex4MtxIdx:
    case AttrTex5MtxIdx:
    case AttrTex6MtxIdx:
    case AttrTex7MtxIdx:
        return 1;
    case AttrPos:
        return cnt == PosXY ? 2 : 3;
    case AttrNrm:
        return (cnt == NrmNBT || cnt == NrmNBT3) ? 9 : 3;
    case AttrClr0:
    case AttrClr1:
        return 1;
    case AttrTex0:
    case AttrTex1:
    case AttrTex2:
    case AttrTex3:
    case AttrTex4:
    case AttrTex5:
    case AttrTex6:
    case AttrTex7:
        return cnt == TexS ? 1 : 2;
    default:
        return 1;
    }
}

namespace {

// Reads n components of one type, dequantised by `scale` (an exact power of two; 1 for no
// fraction). One switch per attribute instead of one per component. Same values as
// read_comp: F32 ignores the fraction, an invalid type reads zeros.
inline void read_comps(const std::uint8_t* src, CompType type, float scale, bool be, float* out, unsigned n) {
    switch (type) {
    case U8:
        for (unsigned k = 0; k < n; ++k) out[k] = static_cast<float>(src[k]) * scale;
        return;
    case S8:
        for (unsigned k = 0; k < n; ++k) out[k] = static_cast<float>(static_cast<std::int8_t>(src[k])) * scale;
        return;
    case U16:
        for (unsigned k = 0; k < n; ++k) out[k] = static_cast<float>(read_u16(src + 2 * k, be)) * scale;
        return;
    case S16:
        for (unsigned k = 0; k < n; ++k)
            out[k] = static_cast<float>(static_cast<std::int16_t>(read_u16(src + 2 * k, be))) * scale;
        return;
    case F32:
        for (unsigned k = 0; k < n; ++k) out[k] = read_f32(src + 4 * k, be);
        return;
    default:
        for (unsigned k = 0; k < n; ++k) out[k] = 0.0f;
        return;
    }
}

float frac_scale(std::uint8_t frac) {
    if (frac == 0) {
        return 1.0f;
    }
    if (frac < 32) {
        const std::uint32_t bits = (127u - frac) << 23;
        float scale;
        std::memcpy(&scale, &bits, sizeof(scale));
        return scale;
    }
    return std::ldexp(1.0f, -static_cast<int>(frac));
}

enum AttrKind : std::uint8_t { kKindPnIdx, kKindTexIdx, kKindPos, kKindNrm, kKindNbt3Idx, kKindClr0, kKindClr1, kKindTex };

using ActiveAttr = VertexDecoder::Entry;

// An array element: in the host array, in the (cached) window of guest memory, or copied
// into `scratch` - across two windows when it straddles a page, else through the resolver's
// staging buffer (decoding thread only: the resolver reuses that buffer).
const std::uint8_t* fetch_elem(const ActiveAttr& e, VertexDecoder::Windows& w, unsigned wi, std::uint32_t index,
                               std::uint32_t nbytes, std::uint8_t* scratch, bool allow_staging) {
    const AttrArray& array = e.array;
    if (nbytes == 0 || nbytes > 64u) {
        return nullptr;
    }
    if (array.data != nullptr) {
        if (array.stride == 0) {
            return nullptr;
        }
        const std::uint32_t offset = index * static_cast<std::uint32_t>(array.stride);
        if (offset + nbytes > array.size) {
            return nullptr;
        }
        return array.data + offset;
    }
    if (array.guest_base == 0) {
        return nullptr;
    }
    const std::uint32_t addr = array.guest_base + index * static_cast<std::uint32_t>(array.stride);
    std::uint32_t off = addr - w.base[wi];
    if (w.win[wi] != nullptr && off < w.size[wi] && nbytes <= w.size[wi] - off) {
        return w.win[wi] + off;
    }
    std::uint32_t base = 0, size = 0;
    if (const std::uint8_t* win = guest_window(addr, &base, &size)) {
        off = addr - base;
        if (off < size) {
            w.win[wi] = win;
            w.base[wi] = base;
            w.size[wi] = size;
            if (nbytes <= size - off) {
                return win + off;
            }
            // Straddles the window's end: the rest comes from the next window.
            const std::uint32_t first = size - off;
            std::uint32_t base2 = 0, size2 = 0;
            const std::uint8_t* win2 = guest_window(base + size, &base2, &size2);
            if (win2 != nullptr && base2 == base + size && nbytes - first <= size2) {
                std::memcpy(scratch, win + off, first);
                std::memcpy(scratch + first, win2, nbytes - first);
                return scratch;
            }
        }
    }
    if (!allow_staging) {
        return nullptr;
    }
    const std::uint8_t* src = guest_fetch(addr, nbytes);
    if (src == nullptr) {
        return nullptr;
    }
    std::memcpy(scratch, src, nbytes);
    return scratch;
}

inline void set_nbt(DecodedVertex& v, const float* c, unsigned n) {
    v.nx = c[0];
    v.ny = c[1];
    v.nz = c[2];
    if (n >= 9) {
        v.bx = c[3];
        v.by = c[4];
        v.bz = c[5];
        v.tx = c[6];
        v.ty = c[7];
        v.tz = c[8];
    }
}

inline std::uint32_t read_index_n(const std::uint8_t* p, unsigned bytes, bool be) {
    return bytes == 1 ? p[0] : read_u16(p, be);
}

} // namespace

std::uint32_t VertexDecoder::prepare(const State& state, VtxFmt fmt, bool big_endian) {
    const auto& vtx = state.vtx_fmts[fmt];
    big_endian_ = big_endian;
    count_ = 0;
    vertex_bytes_ = 0;
    for (int a = AttrPnMtxIdx; a <= AttrTex7; ++a) {
        const AttrType desc = state.vtx_desc[a];
        if (desc == AttrNone) continue;
        const Attr attr = static_cast<Attr>(a);
        const AttrFormat& af = vtx.attrs[a];
        Entry& e = entries_[count_++];
        e.desc = static_cast<std::uint8_t>(desc);
        e.type = static_cast<std::uint8_t>(af.type);
        e.scale = frac_scale(af.frac);
        e.array = state.arrays[attr < AttrCount ? attr : AttrPos];
        e.slot = 0;
        e.index_bytes = desc == AttrIndex16 ? 2 : 1;
        if (attr <= AttrTex7MtxIdx) {
            // Matrix indices are always a direct byte, whatever the descriptor says.
            e.kind = attr == AttrPnMtxIdx ? kKindPnIdx : kKindTexIdx;
            e.slot = attr == AttrPnMtxIdx ? 0 : static_cast<std::uint8_t>(attr - AttrTex0MtxIdx);
            e.comp_bytes = e.nbytes = 0;
            e.ncomp = 0;
            e.be = big_endian;
            vertex_bytes_ += 1;
            continue;
        }
        e.comp_bytes = comp_type_size(attr, af.type);
        e.ncomp = comp_cnt_count(attr, af.cnt);
        e.nbytes = e.comp_bytes * e.ncomp;
        e.be = desc == AttrDirect ? big_endian : array_be(state, attr, big_endian);
        switch (attr) {
        case AttrPos:
            e.kind = kKindPos;
            break;
        case AttrNrm:
            e.kind = (af.cnt == NrmNBT3 && desc != AttrDirect) ? kKindNbt3Idx : kKindNrm;
            break;
        case AttrClr0:
            e.kind = kKindClr0;
            break;
        case AttrClr1:
            e.kind = kKindClr1;
            break;
        default:
            e.kind = kKindTex;
            e.slot = static_cast<std::uint8_t>(attr - AttrTex0);
            break;
        }
        if (desc == AttrDirect) {
            vertex_bytes_ += e.nbytes;
        } else {
            vertex_bytes_ += e.index_bytes * (e.kind == kKindNbt3Idx ? 3u : 1u);
        }
    }
    // Signature and compactability (the display-list vertex cache keys on these).
    {
        std::uint64_t h = 1469598103934665603ull ^ (static_cast<std::uint64_t>(count_) << 40) ^
                          (static_cast<std::uint64_t>(vertex_bytes_) << 8) ^ (big_endian ? 1u : 0u);
        auto mix = [&h](std::uint64_t w) {
            h ^= w + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h *= 0x100000001B3ull;
        };
        bool compact = count_ != 0;
        bool has_pos = false;
        has_pn_idx_ = false;
        for (unsigned a = 0; a < count_; ++a) {
            const Entry& e = entries_[a];
            std::uint32_t scale_bits = 0;
            std::memcpy(&scale_bits, &e.scale, 4);
            mix(static_cast<std::uint64_t>(e.kind) | static_cast<std::uint64_t>(e.desc) << 8 |
                static_cast<std::uint64_t>(e.type) << 16 | static_cast<std::uint64_t>(e.ncomp) << 24 |
                static_cast<std::uint64_t>(e.index_bytes) << 32 | static_cast<std::uint64_t>(e.slot) << 40 |
                static_cast<std::uint64_t>(e.be ? 1u : 0u) << 48);
            mix(static_cast<std::uint64_t>(scale_bits) | static_cast<std::uint64_t>(e.nbytes) << 32);
            if (e.desc != AttrDirect && e.kind != kKindPnIdx && e.kind != kKindTexIdx) {
                mix(static_cast<std::uint64_t>(e.array.guest_base) | static_cast<std::uint64_t>(e.array.stride) << 32);
                mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(e.array.data)) ^
                    static_cast<std::uint64_t>(e.array.size) << 32);
            }
            switch (e.kind) {
            case kKindPnIdx: has_pn_idx_ = true; break;
            case kKindTexIdx: compact = false; break;
            case kKindNbt3Idx: compact = false; break;
            case kKindNrm: if (e.ncomp != 3) compact = false; break;
            case kKindPos: has_pos = true; break;
            case kKindTex: if (e.slot > 1) compact = false; break;
            default: break;
            }
        }
        sig_ = h ^ (h >> 29);
        compactable_ = compact && has_pos;
    }
    base_ = DecodedVertex{};
    base_.color = pack_rgba(255, 255, 255, 255);
    base_.color1 = pack_rgba(255, 255, 255, 255);
    base_.pn_mtx = state.current_pn_mtx;
    base_.nz = 1.0f;
    for (unsigned t = 0; t < kMaxTexGens; ++t) {
        base_.tex_mtx[t] = state.tex_mtx_id[t];
        base_.tc[t][2] = 1.0f;
    }
    windows_.clear();
    return vertex_bytes_;
}

void pack_vertex(const DecodedVertex& v, unsigned ntc, std::uint8_t* out, bool lit) {
    const float pos[4] = {v.x, v.y, v.z, v.w};
    std::memcpy(out, pos, 16);
    // Colours are r | g<<8 | b<<16 | a<<24: RGBA byte order in memory on a little-endian host.
    std::memcpy(out + kPackOffColor0, &v.color, 4);
    std::memcpy(out + kPackOffColor1, &v.color1, 4);
    std::memcpy(out + kPackOffTex, v.tc, 12u * ntc);
    if (lit) {
        const float extra[6] = {v.bx, v.by, v.bz, v.tx, v.ty, v.tz};
        std::memcpy(out + kPackOffTex + 12u * ntc, extra, sizeof(extra));
    }
}

void VertexDecoder::store(const DecodedVertex& v, CachedVertex& c) {
    c.x = v.x;
    c.y = v.y;
    c.z = v.z;
    c.nx = v.nx;
    c.ny = v.ny;
    c.nz = v.nz;
    c.tc[0][0] = v.tc[0][0];
    c.tc[0][1] = v.tc[0][1];
    c.tc[1][0] = v.tc[1][0];
    c.tc[1][1] = v.tc[1][1];
    c.color = v.color;
    c.color1 = v.color1;
    c.pn_mtx = v.pn_mtx;
    c.pad[0] = c.pad[1] = c.pad[2] = 0;
}

void VertexDecoder::restore(const CachedVertex& c, DecodedVertex& v) const {
    v = base_;
    v.x = c.x;
    v.y = c.y;
    v.z = c.z;
    v.nx = c.nx;
    v.ny = c.ny;
    v.nz = c.nz;
    v.tc[0][0] = c.tc[0][0];
    v.tc[0][1] = c.tc[0][1];
    v.tc[1][0] = c.tc[1][0];
    v.tc[1][1] = c.tc[1][1];
    v.color = c.color;
    v.color1 = c.color1;
    if (has_pn_idx_) {
        v.pn_mtx = c.pn_mtx;
    }
}

void VertexDecoder::restore_for_pack(const CachedVertex& c, DecodedVertex& v) const {
    // Avoid the ~150-byte base_ copy for cache hits that need only position, normal,
    // colours and texcoords 0/1. No NBT, higher-coordinate or dynamic-matrix channel
    // may consume this partial vertex. Include q for zero texgens.
    v.x = c.x; v.y = c.y; v.z = c.z;
    v.nx = c.nx; v.ny = c.ny; v.nz = c.nz;
    for (unsigned i = 0; i < 2; ++i) {
        v.tc[i][0] = c.tc[i][0];
        v.tc[i][1] = c.tc[i][1];
        v.tc[i][2] = base_.tc[i][2];
    }
    v.color = c.color;
    v.color1 = c.color1;
    v.pn_mtx = has_pn_idx_ ? c.pn_mtx : base_.pn_mtx;
}

bool VertexDecoder::decode(const std::uint8_t* p, const std::uint8_t* end, DecodedVertex& v, const std::uint8_t** next,
                           Windows& windows, bool allow_staging) const {
    std::uint8_t scratch[3][64];
    const bool big_endian = big_endian_;
    v = base_;
    for (unsigned a = 0; a < count_; ++a) {
        const Entry& e = entries_[a];
        if (p >= end) {
            return false;
        }
        if (e.kind == kKindPnIdx) {
            v.pn_mtx = static_cast<std::uint8_t>((*p & 0x3Fu) / 3u);
            ++p;
            continue;
        }
        if (e.kind == kKindTexIdx) {
            v.tex_mtx[e.slot] = static_cast<std::uint8_t>(*p & 0x3Fu);
            ++p;
            continue;
        }
        const CompType type = static_cast<CompType>(e.type);
        if (e.kind == kKindNbt3Idx) {
            // Three separate indices: normal, binormal, tangent.
            const unsigned ib = e.index_bytes;
            if (p + 3u * ib > end) {
                return false;
            }
            const std::uint32_t i0 = read_index_n(p, ib, big_endian);
            const std::uint32_t i1 = read_index_n(p + ib, ib, big_endian);
            const std::uint32_t i2 = read_index_n(p + 2u * ib, ib, big_endian);
            p += 3u * ib;
            const std::uint32_t eb = e.comp_bytes * 3u;
            const std::uint8_t* ns = fetch_elem(e, windows, a, i0, eb, scratch[0], allow_staging);
            const std::uint8_t* bs = fetch_elem(e, windows, a, i1, eb, scratch[1], allow_staging);
            const std::uint8_t* ts = fetch_elem(e, windows, a, i2, eb, scratch[2], allow_staging);
            if (!ns || !bs || !ts) {
                return false;
            }
            float c[9];
            read_comps(ns, type, e.scale, e.be, c, 3);
            read_comps(bs, type, e.scale, e.be, c + 3, 3);
            read_comps(ts, type, e.scale, e.be, c + 6, 3);
            set_nbt(v, c, 9);
            continue;
        }
        const std::uint8_t* src;
        if (e.desc == AttrDirect) {
            if (p + e.nbytes > end) {
                return false;
            }
            src = p;
            p += e.nbytes;
        } else {
            const unsigned ib = e.index_bytes;
            if (p + ib > end) {
                return false;
            }
            const std::uint32_t index = read_index_n(p, ib, big_endian);
            p += ib;
            src = fetch_elem(e, windows, a, index, e.nbytes, scratch[0], allow_staging);
            if (!src) {
                return false;
            }
        }
        switch (e.kind) {
        case kKindPos: {
            float c[3] = {0.0f, 0.0f, 0.0f};
            read_comps(src, type, e.scale, e.be, c, e.ncomp);
            v.x = c[0];
            v.y = c[1];
            v.z = c[2];
            break;
        }
        case kKindNrm: {
            // 3 components (normal) or 9 (normal, binormal, tangent).
            float c[9];
            read_comps(src, type, e.scale, e.be, c, e.ncomp);
            set_nbt(v, c, e.ncomp);
            break;
        }
        case kKindClr0:
            v.color = decode_color(src, type, e.be);
            break;
        case kKindClr1:
            v.color1 = decode_color(src, type, e.be);
            break;
        default: {
            float c[2] = {0.0f, 0.0f};
            read_comps(src, type, e.scale, e.be, c, e.ncomp);
            v.tc[e.slot][0] = c[0];
            v.tc[e.slot][1] = c[1];
            break;
        }
        }
    }
    if (v.pn_mtx >= kMaxPnMtx) {
        v.pn_mtx = 0;
    }
    *next = p;
    return true;
}

bool decode_vertices(const State& state, VtxFmt fmt, const std::uint8_t* data, std::uint32_t bytes, bool big_endian,
                     DecodedVertex* out, std::uint16_t count) {
    if (!data || !out) {
        return false;
    }
    VertexDecoder decoder;
    decoder.prepare(state, fmt, big_endian);
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + bytes;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (!decoder.decode(p, end, out[i], &p)) {
            return false;
        }
    }
    return p <= end;
}

std::uint32_t expanded_index_count(Primitive prim, std::uint32_t vtx_count) {
    switch (prim) {
    case Quads:
        return (vtx_count / 4u) * 6u;  // a trailing partial quad draws nothing
    case Triangles:
        return (vtx_count / 3u) * 3u;
    case TriangleFan:
    case TriangleStrip:
        return vtx_count < 3u ? 0u : (vtx_count - 2u) * 3u;
    case Lines:
    case LineStrip:
    case Points:
        return 0;
    }
    return 0;
}

std::uint32_t expand_indices(Primitive prim, std::uint32_t vtx_count, std::uint16_t* out, std::uint32_t cap) {
    const std::uint32_t need = expanded_index_count(prim, vtx_count);
    if (!out || need > cap || vtx_count > 0x10000u) {
        return 0;
    }
    std::uint32_t n = 0;
    auto put = [&](std::uint32_t idx) { store_u16(out + n++, static_cast<std::uint16_t>(idx)); };
    if (prim == Quads) {
        for (std::uint32_t v = 0; v + 3u < vtx_count; v += 4u) {
            put(v);
            put(v + 1u);
            put(v + 2u);
            put(v + 2u);
            put(v + 3u);
            put(v);
        }
    } else if (prim == Triangles) {
        for (std::uint32_t v = 0; v + 2u < vtx_count; v += 3u) {
            put(v);
            put(v + 1u);
            put(v + 2u);
        }
    } else if (prim == TriangleFan) {
        for (std::uint32_t v = 2; v < vtx_count; ++v) {
            put(0);
            put(v - 1u);
            put(v);
        }
    } else if (prim == TriangleStrip) {
        for (std::uint32_t v = 2; v < vtx_count; ++v) {
            if ((v & 1u) == 0) {
                put(v - 2u);
                put(v - 1u);
            } else {
                put(v - 1u);
                put(v - 2u);
            }
            put(v);
        }
    }
    return n;
}

namespace {

// Offsets a clip-space vertex by an NDC-space delta: the delta is multiplied by w so that
// after the GPU's perspective divide the vertex moves by exactly (dx, dy) in NDC.
DecodedVertex offset_clip(const DecodedVertex& v, float dx, float dy) {
    DecodedVertex o = v;
    o.x = v.x + dx * v.w;
    o.y = v.y + dy * v.w;
    return o;
}

void emit_tri(DecodedVertex* out, std::uint32_t& n, const DecodedVertex& a, const DecodedVertex& b,
              const DecodedVertex& c) {
    out[n++] = a;
    out[n++] = b;
    out[n++] = c;
}

// A GX line is lw/6 pixels wide, measured perpendicular to the line in screen pixels
// (Aurora shader.cpp:909-994). vp_w/vp_h are the viewport size in EFB pixels.
bool stroke_line(const DecodedVertex& a, const DecodedVertex& b, float half_px, float vp_w, float vp_h,
                 DecodedVertex* out, std::uint32_t cap, std::uint32_t& n) {
    if (n + 6u > cap) {
        return false;
    }
    const float aw = std::fabs(a.w) > 1.0e-8f ? a.w : 1.0e-8f;
    const float bw = std::fabs(b.w) > 1.0e-8f ? b.w : 1.0e-8f;
    // Direction in pixels.
    float dx = (b.x / bw - a.x / aw) * vp_w * 0.5f;
    float dy = (b.y / bw - a.y / aw) * vp_h * 0.5f;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0e-8f) {
        dx = 1.0f;
        dy = 0.0f;
        len = 1.0f;
    }
    // Perpendicular, back to NDC.
    const float px = (-dy / len) * half_px * 2.0f / vp_w;
    const float py = (dx / len) * half_px * 2.0f / vp_h;
    const DecodedVertex a0 = offset_clip(a, px, py);
    const DecodedVertex a1 = offset_clip(a, -px, -py);
    const DecodedVertex b0 = offset_clip(b, px, py);
    const DecodedVertex b1 = offset_clip(b, -px, -py);
    emit_tri(out, n, a0, b0, b1);
    emit_tri(out, n, a0, b1, a1);
    return true;
}

} // namespace

std::uint32_t expand_strokes(Primitive prim, const DecodedVertex* in, std::uint32_t count, float half_px, float vp_w,
                             float vp_h, DecodedVertex* out, std::uint32_t cap) {
    if (!in || !out || count == 0 || vp_w < 1.0f || vp_h < 1.0f) {
        return 0;
    }
    std::uint32_t n = 0;
    if (prim == Points) {
        const float hx = half_px * 2.0f / vp_w;
        const float hy = half_px * 2.0f / vp_h;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (n + 6u > cap) {
                return n;
            }
            const DecodedVertex v0 = offset_clip(in[i], -hx, hy);
            const DecodedVertex v1 = offset_clip(in[i], hx, hy);
            const DecodedVertex v2 = offset_clip(in[i], hx, -hy);
            const DecodedVertex v3 = offset_clip(in[i], -hx, -hy);
            emit_tri(out, n, v0, v1, v2);
            emit_tri(out, n, v0, v2, v3);
        }
        return n;
    }
    if (prim == Lines) {
        for (std::uint32_t i = 0; i + 1 < count; i += 2) {
            if (!stroke_line(in[i], in[i + 1], half_px, vp_w, vp_h, out, cap, n)) {
                return n;
            }
        }
        return n;
    }
    if (prim == LineStrip) {
        for (std::uint32_t i = 0; i + 1 < count; ++i) {
            if (!stroke_line(in[i], in[i + 1], half_px, vp_w, vp_h, out, cap, n)) {
                return n;
            }
        }
        return n;
    }
    return 0;
}

namespace {

#if defined(__ARM_NEON)
// Four-wide dot. The adds pair (0+1)+(2+3) rather than folding left to right, so a result
// can differ from the scalar line by an ulp. Positions only; lighting stays scalar.
float dot4n(const float* row, float x, float y, float z, float w) {
    const float v[4] = {x, y, z, w};
    const float32x4_t p = vmulq_f32(vld1q_f32(row), vld1q_f32(v));
    const float32x2_t h = vadd_f32(vget_low_f32(p), vget_high_f32(p));
    return vget_lane_f32(vpadd_f32(h, h), 0);
}
#endif

void mul_3x4_vec(const float m[12], float x, float y, float z, float w, float* out) {
#if defined(__ARM_NEON)
    out[0] = dot4n(m, x, y, z, w);
    out[1] = dot4n(m + 4, x, y, z, w);
    out[2] = dot4n(m + 8, x, y, z, w);
#else
    out[0] = m[0] * x + m[1] * y + m[2] * z + m[3] * w;
    out[1] = m[4] * x + m[5] * y + m[6] * z + m[7] * w;
    out[2] = m[8] * x + m[9] * y + m[10] * z + m[11] * w;
#endif
}

// Reciprocal square root. On the Vita this is the NEON estimate plus two Newton steps:
// lighting results are quantized to 8-bit channels, and libm sqrt was the per-vertex cost
// on lit menu geometry (the unlit clip path does not run there).
inline float inv_sqrt(float s) {
#if defined(__ARM_NEON)
    float32x4_t x = vdupq_n_f32(s);
    float32x4_t e = vrsqrteq_f32(x);
    const float32x4_t half = vdupq_n_f32(0.5f);
    const float32x4_t three = vdupq_n_f32(1.5f);
    e = vmulq_f32(e, vsubq_f32(three, vmulq_f32(vmulq_f32(half, x), vmulq_f32(e, e))));
    e = vmulq_f32(e, vsubq_f32(three, vmulq_f32(vmulq_f32(half, x), vmulq_f32(e, e))));
    return vgetq_lane_f32(e, 0);
#else
    return 1.0f / std::sqrt(s);
#endif
}

inline void normalize3(float* v) {
    const float s = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (s < 1.0e-20f) {
        return;
    }
    const float inv = inv_sqrt(s);
    v[0] *= inv;
    v[1] *= inv;
    v[2] *= inv;
}

float dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::uint8_t ch8(std::uint32_t c, int shift) {
    return static_cast<std::uint8_t>(c >> shift);
}

// The model-view position and the matrix it came from.
template <typename S>
void model_view(const S& state, const DecodedVertex& v, float mv[3]) {
    const float* m = state.pn_mtx[v.pn_mtx < kMaxPnMtx ? v.pn_mtx : 0];
    mul_3x4_vec(m, v.x, v.y, v.z, 1.0f, mv);
}

} // namespace

// Model space -> GL clip space. GX clip z runs from -w (near) to 0 (far); GL's from -w to w,
// so z is remapped with 2z + w. x, y and w pass straight through: the GPU does the divide,
// which keeps every varying perspective-correct and lets the GPU clip against the near
// plane. (This used to divide on the CPU and hand the GPU w = 1.)
template <typename S>
static void project_vertex(const S& state, const float mv[3], float clip[4]) {
#if defined(__ARM_NEON)
    const float cx = dot4n(&state.proj[0][0], mv[0], mv[1], mv[2], 1.0f);
    const float cy = dot4n(&state.proj[1][0], mv[0], mv[1], mv[2], 1.0f);
    const float cz = dot4n(&state.proj[2][0], mv[0], mv[1], mv[2], 1.0f);
    const float cw = dot4n(&state.proj[3][0], mv[0], mv[1], mv[2], 1.0f);
#else
    const float cx = state.proj[0][0] * mv[0] + state.proj[0][1] * mv[1] + state.proj[0][2] * mv[2] + state.proj[0][3];
    const float cy = state.proj[1][0] * mv[0] + state.proj[1][1] * mv[1] + state.proj[1][2] * mv[2] + state.proj[1][3];
    const float cz = state.proj[2][0] * mv[0] + state.proj[2][1] * mv[1] + state.proj[2][2] * mv[2] + state.proj[2][3];
    const float cw = state.proj[3][0] * mv[0] + state.proj[3][1] * mv[1] + state.proj[3][2] * mv[2] + state.proj[3][3];
#endif
    clip[0] = cx;
    clip[1] = cy;
    clip[2] = 2.0f * cz + cw;
    clip[3] = cw;
}

void transform_vertex(const State& state, const DecodedVertex& in, float clip[4]) {
    float mv[3];
    model_view(state, in, mv);
    project_vertex(state, mv, clip);
}

// Texture coordinate generation, per Aurora shader.cpp:1161-1262:
//   source (TEXn -> (s,t,1,1); POS/NRM/BINRM/TANGENT -> (xyz,1); COLORn -> (rgb,1))
//   AB11 input form forces z = 1
//   2x4 / 3x4: tmp = M * src (M from the per-vertex TEXnMTXIDX or the texgen's matrix);
//   2x4 forces tmp.z = 1.  SRTG: tmp = (src.xy, 1)
//   dual-tex on: optional normalize, then post-matrix * (tmp, 1)
//   a zero q folds x/y into [-1, 1] (hardware quirk Aurora reproduces)
// The divide by q for projective texgens happens per pixel in the fragment shader.
namespace {

// `raw` supplies the texgen sources (position, normal, binormal, tangent, colours, matrix
// indices); `raw_tc` its texture coordinates. Only v.tc is written, so raw may be v itself
// as long as raw_tc is a copy.
template <typename S>
void texgen_impl(const S& state, const DecodedVertex& raw, const float (*raw_tc)[3], DecodedVertex& v) {
    const unsigned n = state.num_tex_gens > kMaxTexGens ? kMaxTexGens : static_cast<unsigned>(state.num_tex_gens);
    for (unsigned i = 0; i < n; ++i) {
        const TexGen& gen = state.texgens[i];
        float src[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        switch (gen.src) {
        case TgPos:
            src[0] = raw.x;
            src[1] = raw.y;
            src[2] = raw.z;
            break;
        case TgNrm:
            src[0] = raw.nx;
            src[1] = raw.ny;
            src[2] = raw.nz;
            break;
        case TgBinrm:
            src[0] = raw.bx;
            src[1] = raw.by;
            src[2] = raw.bz;
            break;
        case TgTangent:
            src[0] = raw.tx;
            src[1] = raw.ty;
            src[2] = raw.tz;
            break;
        case TgColor0:
        case TgColor1: {
            const std::uint32_t c = gen.src == TgColor0 ? raw.color : raw.color1;
            src[0] = static_cast<float>(ch8(c, 0)) / 255.0f;
            src[1] = static_cast<float>(ch8(c, 8)) / 255.0f;
            src[2] = static_cast<float>(ch8(c, 16)) / 255.0f;
            break;
        }
        default: {
            const unsigned t = static_cast<unsigned>(gen.src - TgTex0) & 7u;
            src[0] = raw_tc[t][0];
            src[1] = raw_tc[t][1];
            src[2] = 1.0f;
            break;
        }
        }
        if (gen.form_ab11) {
            src[2] = 1.0f;
        }
        float tmp[3] = {src[0], src[1], 1.0f};
        if (gen.type == TgSrtg) {
            tmp[2] = 1.0f;
        } else {
            const bool per_vertex = state.vtx_desc[AttrTex0MtxIdx + i] != AttrNone;
            const unsigned mtx_id = per_vertex ? raw.tex_mtx[i] : gen.mtx;
            if (mtx_id != kTexMtxIdentity && mtx_id * 4u + 12u <= kXfMemSize) {
                mul_3x4_vec(state.xf_mem + mtx_id * 4u, src[0], src[1], src[2], src[3], tmp);
            } else if (mtx_id != kTexMtxIdentity && mtx_id * 4u + 8u <= kXfMemSize) {
                const float* m = state.xf_mem + mtx_id * 4u;
                tmp[0] = m[0] * src[0] + m[1] * src[1] + m[2] * src[2] + m[3] * src[3];
                tmp[1] = m[4] * src[0] + m[5] * src[1] + m[6] * src[2] + m[7] * src[3];
                tmp[2] = 1.0f;
            } else {
                tmp[0] = src[0];
                tmp[1] = src[1];
                tmp[2] = src[2];
            }
            if (gen.type == TgMtx2x4) {
                tmp[2] = 1.0f;
            }
        }
        if (state.dual_tex && gen.normalize) {
            normalize3(tmp);
        }
        float out[3] = {tmp[0], tmp[1], tmp[2]};
        if (state.dual_tex && gen.post_mtx != kPtTexMtxIdentity && gen.post_mtx >= kPtTexMtx0) {
            const unsigned slot = static_cast<unsigned>(gen.post_mtx - kPtTexMtx0) * 4u;
            if (slot + 12u <= kXfDttSize) {
                mul_3x4_vec(state.xf_dtt + slot, tmp[0], tmp[1], tmp[2], 1.0f, out);
            }
        }
        if (gen.type != TgSrtg && out[2] == 0.0f) {
            out[0] = std::min(1.0f, std::max(-1.0f, out[0] * 0.5f));
            out[1] = std::min(1.0f, std::max(-1.0f, out[1] * 0.5f));
        }
        v.tc[i][0] = out[0];
        v.tc[i][1] = out[1];
        v.tc[i][2] = gen.type == TgMtx3x4 ? out[2] : 1.0f;
    }
}

} // namespace

void apply_texgen(const State& state, const DecodedVertex& raw, DecodedVertex& v) {
    texgen_impl(state, raw, raw.tc, v);
}

void apply_texgen(const State& state, DecodedVertex& v) {
    if (state.num_tex_gens == 0) {
        return;
    }
    float tc[kMaxTexGens][3];
    std::memcpy(tc, v.tc, sizeof(tc));
    texgen_impl(state, v, tc, v);
}

namespace {

struct LightGeometry {
    float direction[3], distance, distance2;
};
struct LightInputs {
    float mv_pos[3];
    float nrm[3];
    unsigned ready = 0;
    LightGeometry geometry[kMaxLights];
};

// One channel half (colour or alpha) of the XF lighting unit, Aurora shader.cpp:771-893.
// `comp` selects the channel bytes: {0, 8, 16} for RGB, {24} for alpha.
void light_channel(const State& state, const ChanCtrl& ctrl, unsigned chan, LightInputs& in, const PreparedLighting* prepared,
                   std::uint32_t vtx_color, const int* comps, unsigned ncomp, std::uint8_t* out) {
    const std::uint32_t mat = ctrl.mat_src == SrcVtx ? vtx_color : state.chan_mat[chan];
    if (!ctrl.lighting) {
        for (unsigned k = 0; k < ncomp; ++k) {
            out[k] = ch8(mat, comps[k]);
        }
        return;
    }
    const std::uint32_t amb = ctrl.amb_src == SrcVtx ? vtx_color : state.ambient[chan];
    int lacc[3] = {0, 0, 0};
    for (unsigned k = 0; k < ncomp; ++k) {
        lacc[k] = ch8(amb, comps[k]);
    }
    for (unsigned li = 0; li < kMaxLights; ++li) {
        if ((ctrl.light_mask & (1u << li)) == 0) {
            continue;
        }
        const Light& light = state.lights[li];
        LightGeometry local;
        LightGeometry& g = prepared ? in.geometry[li] : local;
        if (!prepared || !(in.ready & (1u << li))) {
            g.direction[0] = light.pos[0] - in.mv_pos[0];
            g.direction[1] = light.pos[1] - in.mv_pos[1];
            g.direction[2] = light.pos[2] - in.mv_pos[2];
            g.distance2 = dot3(g.direction, g.direction);
            g.distance = std::sqrt(g.distance2);
            for (unsigned c = 0; c < 3; ++c)
                g.direction[c] = g.distance > 0.0f ? g.direction[c] / g.distance : in.nrm[c];
            in.ready |= 1u << li;
        }
        const float* ldir = g.direction;
        const float dist = g.distance, dist2 = g.distance2;
        float local_direction[3];
        const float* ldir_l;
        if (prepared) {
            ldir_l = prepared->direction[li];
        } else {
            for (unsigned c = 0; c < 3; ++c) local_direction[c] = light.dir[c];
            normalize3(local_direction);
            ldir_l = local_direction;
        }
        float attn = 1.0f;
        if (ctrl.attn_fn == AfSpot) {
            const float cosine = std::max(0.0f, dot3(ldir, ldir_l));
            const float cos_attn = light.cos_att[0] + light.cos_att[1] * cosine + light.cos_att[2] * cosine * cosine;
            const float dist_attn = light.dist_att[0] + light.dist_att[1] * dist + light.dist_att[2] * dist2;
            attn = dist_attn != 0.0f ? std::max(0.0f, cos_attn / dist_attn) : 0.0f;
        } else if (ctrl.attn_fn == AfSpec) {
            attn = dot3(in.nrm, ldir) >= 0.0f ? std::max(0.0f, dot3(in.nrm, ldir_l)) : 0.0f;
            const float cos_attn = light.cos_att[0] + light.cos_att[1] * attn + light.cos_att[2] * attn * attn;
            float k[3] = {light.dist_att[0], light.dist_att[1], light.dist_att[2]};
            if (ctrl.diff_fn != DfNone) {
                if (prepared) {
                    for (unsigned c = 0; c < 3; ++c) k[c] = prepared->specular_distance[li][c];
                } else normalize3(k);
            }
            const float dist_attn = k[0] + k[1] * attn + k[2] * attn * attn;
            attn = dist_attn != 0.0f ? std::max(0.0f, cos_attn / dist_attn) : (cos_attn > 0.0f ? 1.0f : 0.0f);
        }
        float diff = 1.0f;
        if (ctrl.diff_fn == DfSign) {
            diff = dot3(ldir, in.nrm);
        } else if (ctrl.diff_fn == DfClamp) {
            diff = std::max(0.0f, dot3(ldir, in.nrm));
        }
        for (unsigned k = 0; k < ncomp; ++k) {
            lacc[k] += static_cast<int>(std::lround(attn * diff * static_cast<float>(ch8(light.color, comps[k]))));
        }
    }
    for (unsigned k = 0; k < ncomp; ++k) {
        const int l = std::min(255, std::max(0, lacc[k]));
        const int m = ch8(mat, comps[k]);
        out[k] = static_cast<std::uint8_t>((m * (l + (l >> 7))) >> 8);
    }
}

} // namespace

// COLOR0A0 and COLOR1A1 from the four XF channel controls (COLOR0, COLOR1, ALPHA0, ALPHA1).
// Vertex colour sources follow GX (Aurora gx.hpp:578-592): channel 0 reads CLR0, else CLR1,
// else opaque white; channel 1 reads CLR1 only when both colours are present.
static void lighting_impl(const State& state, DecodedVertex& v, const PreparedLighting* prepared, const float* mv) {
    if (state.num_chans == 0) {
        return;
    }
    const bool has0 = state.vtx_desc[AttrClr0] != AttrNone;
    const bool has1 = state.vtx_desc[AttrClr1] != AttrNone;
    const std::uint32_t white = pack_rgba(255, 255, 255, 255);
    const std::uint32_t vtx0 = has0 ? v.color : (has1 ? v.color1 : white);
    const std::uint32_t vtx1 = (has0 && has1) ? v.color1 : white;

    // With lighting disabled, the GX channel output is exactly its selected
    // material colour. The common UI/race quad path needs no normal transform,
    // light setup, or four per-channel helper calls.
    if (!state.chans[0].lighting && !state.chans[1].lighting &&
        !state.chans[2].lighting && !state.chans[3].lighting) {
        auto material = [&](unsigned ctrl, unsigned chan, std::uint32_t vertex) {
            return state.chans[ctrl].mat_src == SrcVtx ? vertex : state.chan_mat[chan];
        };
        const std::uint32_t rgb0 = material(0, 0, vtx0);
        const std::uint32_t a0 = material(2, 0, vtx0);
        const std::uint32_t rgb1 = material(1, 1, vtx1);
        const std::uint32_t a1 = material(3, 1, vtx1);
        v.color = (rgb0 & 0x00FFFFFFu) | (a0 & 0xFF000000u);
        v.color1 = (rgb1 & 0x00FFFFFFu) | (a1 & 0xFF000000u);
        return;
    }

    LightInputs in{};
    bool need_geometry = false;
    for (unsigned c = 0; c < 4u; ++c) {
        need_geometry = need_geometry || (state.chans[c].lighting && state.chans[c].light_mask != 0);
    }
    if (need_geometry) {
        if (mv) {
            for (unsigned c = 0; c < 3; ++c) in.mv_pos[c] = mv[c];
        } else model_view(state, v, in.mv_pos);
        const float* nm = state.nrm_mtx[v.pn_mtx < kMaxPnMtx ? v.pn_mtx : 0];
        in.nrm[0] = nm[0] * v.nx + nm[1] * v.ny + nm[2] * v.nz;
        in.nrm[1] = nm[3] * v.nx + nm[4] * v.ny + nm[5] * v.nz;
        in.nrm[2] = nm[6] * v.nx + nm[7] * v.ny + nm[8] * v.nz;
        normalize3(in.nrm);
    }
    static const int kRgb[3] = {0, 8, 16};
    static const int kA[1] = {24};
    std::uint8_t rgb[3];
    std::uint8_t a;
    light_channel(state, state.chans[0], 0, in, prepared, vtx0, kRgb, 3, rgb);
    light_channel(state, state.chans[2], 0, in, prepared, vtx0, kA, 1, &a);
    const std::uint32_t c0 = pack_rgba(rgb[0], rgb[1], rgb[2], a);
    light_channel(state, state.chans[1], 1, in, prepared, vtx1, kRgb, 3, rgb);
    light_channel(state, state.chans[3], 1, in, prepared, vtx1, kA, 1, &a);
    v.color1 = pack_rgba(rgb[0], rgb[1], rgb[2], a);
    v.color = c0;
}

void apply_lighting(const State& state, DecodedVertex& v) {
    lighting_impl(state, v, nullptr, nullptr);
}

void prepare_lighting(const State& state, PreparedLighting& out) {
    unsigned mask = 0;
    out.any_lit = false;
    for (unsigned h = 0; h < 4; ++h) {
        const ChanCtrl& ctrl = state.chans[h];
        PreparedLighting::Half& half = out.half[h];
        half.lit = ctrl.lighting ? 1 : 0;
        half.mat_vtx = ctrl.mat_src == SrcVtx ? 1 : 0;
        half.amb_vtx = ctrl.amb_src == SrcVtx ? 1 : 0;
        half.diff_fn = static_cast<std::uint8_t>(ctrl.diff_fn);
        half.attn_fn = static_cast<std::uint8_t>(ctrl.attn_fn);
        half.count = 0;
        if (!ctrl.lighting) {
            continue;
        }
        out.any_lit = true;
        mask |= ctrl.light_mask;
        for (unsigned li = 0; li < kMaxLights; ++li) {
            if (ctrl.light_mask & (1u << li)) {
                half.light[half.count++] = static_cast<std::uint8_t>(li);
            }
        }
    }
    out.geometry_count = 0;
    for (unsigned i = 0; i < kMaxLights; ++i) {
        if (!(mask & (1u << i))) continue;
        out.geometry_light[out.geometry_count++] = static_cast<std::uint8_t>(i);
        for (unsigned c = 0; c < 3; ++c) {
            out.direction[i][c] = state.lights[i].dir[c];
            out.specular_distance[i][c] = state.lights[i].dist_att[c];
        }
        normalize3(out.direction[i]);
        normalize3(out.specular_distance[i]);
        for (unsigned c = 0; c < 4; ++c) {
            out.color[i][c] = static_cast<float>(ch8(state.lights[i].color, static_cast<int>(c * 8u)));
        }
    }
}

bool gpu_lit_eligible(const State& state, const PreparedLighting& prepared) {
    return gpu_lighting_enabled() && state.num_chans != 0 && prepared.any_lit;
}

void fill_gpu_lit_params(const State& state, const PreparedLighting& prepared, GpuLitParams& out) {
    out = GpuLitParams{};
    out.on = 1;
    unsigned used = 0;
    for (unsigned h = 0; h < 4u; ++h) {
        const PreparedLighting::Half& half = prepared.half[h];
        out.lit[h] = half.lit;
        out.mat_vtx[h] = half.mat_vtx;
        out.amb_vtx[h] = half.amb_vtx;
        out.diff_fn[h] = half.lit ? half.diff_fn : 0;
        out.attn_fn[h] = half.lit ? half.attn_fn : 0;
        std::uint8_t mask = 0;
        if (half.lit) {
            for (unsigned j = 0; j < half.count; ++j) {
                mask = static_cast<std::uint8_t>(mask | (1u << half.light[j]));
            }
        }
        out.light_mask[h] = mask;
        used |= mask;
    }
    for (unsigned c = 0; c < 2u; ++c) {
        for (unsigned k = 0; k < 4u; ++k) {
            out.mat[c][k] = static_cast<float>(ch8(state.chan_mat[c], static_cast<int>(k * 8u)));
            out.amb[c][k] = static_cast<float>(ch8(state.ambient[c], static_cast<int>(k * 8u)));
        }
    }
    for (unsigned li = 0; li < kMaxLights; ++li) {
        if (!(used & (1u << li))) {
            continue;
        }
        const Light& light = state.lights[li];
        for (unsigned k = 0; k < 3u; ++k) {
            out.pos[li][k] = light.pos[k];
            out.dir[li][k] = prepared.direction[li][k];
            out.spec[li][k] = prepared.specular_distance[li][k];
            out.cosa[li][k] = light.cos_att[k];
            out.dista[li][k] = light.dist_att[k];
        }
        for (unsigned k = 0; k < 4u; ++k) {
            out.col[li][k] = prepared.color[li][k];
        }
    }
}

namespace {

float round_away(float x) {
    return x >= 0.0f ? std::floor(x + 0.5f) : -std::floor(-x + 0.5f);
}

// One half of gpu_lit_reference, the same steps the emitted Cg takes (cg_emit.cpp,
// emit_tev_vertex_cg_lit), in float.
void gpu_lit_half(const GpuLitParams& p, unsigned h, unsigned chan, const float* vtx, const float geo_dir[][3],
                  const float* geo_dist, const float* nrm, unsigned first, unsigned n, float* out) {
    const float* mat = p.mat_vtx[h] ? vtx : p.mat[chan];
    if (!p.lit[h]) {
        for (unsigned k = 0; k < n; ++k) out[k] = mat[first + k];
        return;
    }
    const float* amb = p.amb_vtx[h] ? vtx : p.amb[chan];
    float l[4];
    for (unsigned k = 0; k < n; ++k) l[k] = amb[first + k];
    for (unsigned li = 0; li < kMaxLights; ++li) {
        if (!(p.light_mask[h] & (1u << li))) continue;
        const float* ldir = geo_dir[li];
        const float dist = geo_dist[li];
        float attn = 1.0f;
        if (p.attn_fn[h] == AfSpot) {
            const float cosine = std::max(0.0f, dot3(ldir, p.dir[li]));
            const float ca = p.cosa[li][0] + p.cosa[li][1] * cosine + p.cosa[li][2] * cosine * cosine;
            const float da = p.dista[li][0] + p.dista[li][1] * dist + p.dista[li][2] * dist * dist;
            attn = da != 0.0f ? std::max(0.0f, ca / da) : 0.0f;
        } else if (p.attn_fn[h] == AfSpec) {
            attn = dot3(nrm, ldir) >= 0.0f ? std::max(0.0f, dot3(nrm, p.dir[li])) : 0.0f;
            const float ca = p.cosa[li][0] + p.cosa[li][1] * attn + p.cosa[li][2] * attn * attn;
            const float* k3 = p.diff_fn[h] != DfNone ? p.spec[li] : p.dista[li];
            const float da = k3[0] + k3[1] * attn + k3[2] * attn * attn;
            attn = da != 0.0f ? std::max(0.0f, ca / da) : (ca > 0.0f ? 1.0f : 0.0f);
        }
        float diff = 1.0f;
        if (p.diff_fn[h] == DfSign) {
            diff = dot3(ldir, nrm);
        } else if (p.diff_fn[h] == DfClamp) {
            diff = std::max(0.0f, dot3(ldir, nrm));
        }
        const float ad = attn * diff;
        for (unsigned k = 0; k < n; ++k) l[k] += round_away(ad * p.col[li][first + k]);
    }
    for (unsigned k = 0; k < n; ++k) {
        const float lc = std::min(255.0f, std::max(0.0f, l[k]));
        out[k] = std::floor(mat[first + k] * (lc + std::floor(lc / 128.0f)) / 256.0f);
    }
}

std::uint32_t pack_bytes(const float* c) {
    std::uint32_t r = 0;
    for (unsigned k = 0; k < 4u; ++k) {
        const float v = std::min(255.0f, std::max(0.0f, c[k]));
        r |= static_cast<std::uint32_t>(v) << (8u * k);
    }
    return r;
}

} // namespace

void gpu_lit_reference(const GpuLitParams& p, const float eye[3], const float nrm_in[3], std::uint32_t vtx0,
                       std::uint32_t vtx1, std::uint32_t* color0, std::uint32_t* color1) {
    float nrm[3] = {nrm_in[0], nrm_in[1], nrm_in[2]};
    const float s = dot3(nrm, nrm);
    if (s >= 1.0e-20f) {
        const float inv = 1.0f / std::sqrt(s);
        nrm[0] *= inv;
        nrm[1] *= inv;
        nrm[2] *= inv;
    }
    float geo_dir[kMaxLights][3] = {};
    float geo_dist[kMaxLights] = {};
    for (unsigned li = 0; li < kMaxLights; ++li) {
        float d[3] = {p.pos[li][0] - eye[0], p.pos[li][1] - eye[1], p.pos[li][2] - eye[2]};
        const float d2 = dot3(d, d);
        if (d2 > 1.0e-20f) {
            const float inv = 1.0f / std::sqrt(d2);
            geo_dist[li] = d2 * inv;
            for (unsigned k = 0; k < 3u; ++k) geo_dir[li][k] = d[k] * inv;
        } else {
            geo_dist[li] = 0.0f;
            for (unsigned k = 0; k < 3u; ++k) geo_dir[li][k] = nrm[k];
        }
    }
    float v0[4], v1[4];
    for (unsigned k = 0; k < 4u; ++k) {
        v0[k] = static_cast<float>(ch8(vtx0, static_cast<int>(k * 8u)));
        v1[k] = static_cast<float>(ch8(vtx1, static_cast<int>(k * 8u)));
    }
    float c0[4], c1[4];
    gpu_lit_half(p, 0, 0, v0, geo_dir, geo_dist, nrm, 0, 3, c0);
    gpu_lit_half(p, 2, 0, v0, geo_dir, geo_dist, nrm, 3, 1, c0 + 3);
    gpu_lit_half(p, 1, 1, v1, geo_dir, geo_dist, nrm, 0, 3, c1);
    gpu_lit_half(p, 3, 1, v1, geo_dir, geo_dist, nrm, 3, 1, c1 + 3);
    *color0 = pack_bytes(c0);
    *color1 = pack_bytes(c1);
}

namespace {

// std::lround (half away from zero) without the libm call; x - trunc(x) is exact in binary32.
// NaN gives 0, which is what the truncated lround result amounts to.
inline int round_half_away(float x) {
    if (x >= 0.0f) {
        if (x >= 2147483520.0f) return 0x7FFFFF80;
        int r = static_cast<int>(x);
        if (x - static_cast<float>(r) >= 0.5f) ++r;
        return r;
    }
    if (x < 0.0f) {
        if (x <= -2147483520.0f) return -0x7FFFFF80;
        int r = static_cast<int>(x);
        if (static_cast<float>(r) - x >= 0.5f) --r;
        return r;
    }
    return 0;
}

// One channel half through the prepared plan. Bit-identical to light_channel: the same
// operations in the same order (the vertex reuse test compares the two exactly).
template <unsigned N, typename S>
inline void light_half(const S& state, const PreparedLighting& P, const PreparedLighting::Half& h, unsigned chan,
                       const LightGeometry* geo, const float* nrm, std::uint32_t vtx_color, unsigned first_comp,
                       std::uint8_t* out) {
    const std::uint32_t mat = h.mat_vtx ? vtx_color : state.chan_mat[chan];
    if (!h.lit) {
        for (unsigned k = 0; k < N; ++k) out[k] = ch8(mat, static_cast<int>((first_comp + k) * 8u));
        return;
    }
    const std::uint32_t amb = h.amb_vtx ? vtx_color : state.ambient[chan];
    int lacc[N];
    for (unsigned k = 0; k < N; ++k) lacc[k] = ch8(amb, static_cast<int>((first_comp + k) * 8u));
    for (unsigned j = 0; j < h.count; ++j) {
        const unsigned li = h.light[j];
        const Light& light = state.lights[li];
        const LightGeometry& g = geo[li];
        const float* ldir = g.direction;
        const float* ldir_l = P.direction[li];
        float attn = 1.0f;
        if (h.attn_fn == AfSpot) {
            const float cosine = std::max(0.0f, dot3(ldir, ldir_l));
            const float cos_attn = light.cos_att[0] + light.cos_att[1] * cosine + light.cos_att[2] * cosine * cosine;
            const float dist_attn = light.dist_att[0] + light.dist_att[1] * g.distance + light.dist_att[2] * g.distance2;
            attn = dist_attn != 0.0f ? std::max(0.0f, cos_attn / dist_attn) : 0.0f;
        } else if (h.attn_fn == AfSpec) {
            attn = dot3(nrm, ldir) >= 0.0f ? std::max(0.0f, dot3(nrm, ldir_l)) : 0.0f;
            const float cos_attn = light.cos_att[0] + light.cos_att[1] * attn + light.cos_att[2] * attn * attn;
            const float* k = h.diff_fn != DfNone ? P.specular_distance[li] : light.dist_att;
            const float dist_attn = k[0] + k[1] * attn + k[2] * attn * attn;
            attn = dist_attn != 0.0f ? std::max(0.0f, cos_attn / dist_attn) : (cos_attn > 0.0f ? 1.0f : 0.0f);
        }
        float diff = 1.0f;
        if (h.diff_fn == DfSign) {
            diff = dot3(ldir, nrm);
        } else if (h.diff_fn == DfClamp) {
            diff = std::max(0.0f, dot3(ldir, nrm));
        }
        const float ad = attn * diff;
        for (unsigned k = 0; k < N; ++k) lacc[k] += round_half_away(ad * P.color[li][first_comp + k]);
    }
    for (unsigned k = 0; k < N; ++k) {
        const int l = std::min(255, std::max(0, lacc[k]));
        const int m = ch8(mat, static_cast<int>((first_comp + k) * 8u));
        out[k] = static_cast<std::uint8_t>((m * (l + (l >> 7))) >> 8);
    }
}

template <typename S>
void lighting_prepared(const S& state, DecodedVertex& v, const PreparedLighting& P, const float* mv) {
    if (state.num_chans == 0) {
        return;
    }
    const bool has0 = state.vtx_desc[AttrClr0] != AttrNone;
    const bool has1 = state.vtx_desc[AttrClr1] != AttrNone;
    const std::uint32_t white = pack_rgba(255, 255, 255, 255);
    const std::uint32_t vtx0 = has0 ? v.color : (has1 ? v.color1 : white);
    const std::uint32_t vtx1 = (has0 && has1) ? v.color1 : white;
    if (!P.any_lit) {
        const std::uint32_t rgb0 = P.half[0].mat_vtx ? vtx0 : state.chan_mat[0];
        const std::uint32_t a0 = P.half[2].mat_vtx ? vtx0 : state.chan_mat[0];
        const std::uint32_t rgb1 = P.half[1].mat_vtx ? vtx1 : state.chan_mat[1];
        const std::uint32_t a1 = P.half[3].mat_vtx ? vtx1 : state.chan_mat[1];
        v.color = (rgb0 & 0x00FFFFFFu) | (a0 & 0xFF000000u);
        v.color1 = (rgb1 & 0x00FFFFFFu) | (a1 & 0xFF000000u);
        return;
    }
    LightGeometry geo[kMaxLights];
    float nrm[3] = {0.0f, 0.0f, 0.0f};
    if (P.geometry_count != 0) {
        const float* nm = state.nrm_mtx[v.pn_mtx < kMaxPnMtx ? v.pn_mtx : 0];
#if defined(__ARM_NEON)
        {
            const float32x4_t n = {v.nx, v.ny, v.nz, 0.0f};
            auto row = [&](const float* r) {
                const float32x4_t m = {r[0], r[1], r[2], 0.0f};
                const float32x4_t p = vmulq_f32(m, n);
                return vgetq_lane_f32(p, 0) + vgetq_lane_f32(p, 1) + vgetq_lane_f32(p, 2);
            };
            nrm[0] = row(nm);
            nrm[1] = row(nm + 3);
            nrm[2] = row(nm + 6);
        }
#else
        nrm[0] = nm[0] * v.nx + nm[1] * v.ny + nm[2] * v.nz;
        nrm[1] = nm[3] * v.nx + nm[4] * v.ny + nm[5] * v.nz;
        nrm[2] = nm[6] * v.nx + nm[7] * v.ny + nm[8] * v.nz;
#endif
        normalize3(nrm);
        for (unsigned j = 0; j < P.geometry_count; ++j) {
            const unsigned li = P.geometry_light[j];
            const Light& light = state.lights[li];
            LightGeometry& g = geo[li];
            g.direction[0] = light.pos[0] - mv[0];
            g.direction[1] = light.pos[1] - mv[1];
            g.direction[2] = light.pos[2] - mv[2];
            g.distance2 = dot3(g.direction, g.direction);
            if (g.distance2 > 1.0e-20f) {
                const float inv = inv_sqrt(g.distance2);
                g.distance = g.distance2 * inv;
                g.direction[0] *= inv;
                g.direction[1] *= inv;
                g.direction[2] *= inv;
            } else {
                g.distance = 0.0f;
                g.direction[0] = nrm[0];
                g.direction[1] = nrm[1];
                g.direction[2] = nrm[2];
            }
        }
    }
    std::uint8_t rgb[3];
    std::uint8_t a;
    light_half<3>(state, P, P.half[0], 0, geo, nrm, vtx0, 0, rgb);
    light_half<1>(state, P, P.half[2], 0, geo, nrm, vtx0, 3, &a);
    const std::uint32_t c0 = pack_rgba(rgb[0], rgb[1], rgb[2], a);
    light_half<3>(state, P, P.half[1], 1, geo, nrm, vtx1, 0, rgb);
    light_half<1>(state, P, P.half[3], 1, geo, nrm, vtx1, 3, &a);
    v.color1 = pack_rgba(rgb[0], rgb[1], rgb[2], a);
    v.color = c0;
}

} // namespace

namespace {

template <typename S>
inline void light_and_transform_impl(const S& state, const PreparedLighting& prepared, DecodedVertex& v) {
    float mv[3], clip[4];
    model_view(state, v, mv);
    lighting_prepared(state, v, prepared, mv);
    project_vertex(state, mv, clip);
    v.x = clip[0]; v.y = clip[1]; v.z = clip[2]; v.w = clip[3];
    v.pn_mtx = 0;
}

// Texgen, the SU texture-coordinate scale, lighting and the transform: everything done to a
// decoded vertex. One template for the live State and for a VertexSnapshot, so the render
// thread and the vertex worker produce bit-identical vertices.
template <typename S>
inline void process_vertex_impl(const S& state, const PreparedLighting& prepared, const float (*su_scale)[2], bool su_any,
                                DecodedVertex& v) {
    if (state.num_tex_gens != 0) {
        float tc[kMaxTexGens][3];
        std::memcpy(tc, v.tc, sizeof(tc));
        texgen_impl(state, v, tc, v);  // texgen reads the raw colours and model position
    }
    if (su_any) {
        for (unsigned c = 0; c < kMaxTexGens; ++c) {
            v.tc[c][0] *= su_scale[c][0];
            v.tc[c][1] *= su_scale[c][1];
        }
    }
    light_and_transform_impl(state, prepared, v);
}

template <typename S>
inline void select_raw_colors(const S& state, DecodedVertex& v) {
    const bool has0 = state.vtx_desc[AttrClr0] != AttrNone;
    const bool has1 = state.vtx_desc[AttrClr1] != AttrNone;
    const std::uint32_t white = pack_rgba(255, 255, 255, 255);
    const std::uint32_t vtx0 = has0 ? v.color : (has1 ? v.color1 : white);
    const std::uint32_t vtx1 = (has0 && has1) ? v.color1 : white;
    v.color = vtx0;
    v.color1 = vtx1;
}

// Eye position and (unnormalised) eye normal into the binormal/tangent slots; see pack_vertex.
template <typename S>
inline void store_eye(const S& state, DecodedVertex& v, const float mv[3]) {
    const float* nm = state.nrm_mtx[v.pn_mtx < kMaxPnMtx ? v.pn_mtx : 0];
    const float nx = nm[0] * v.nx + nm[1] * v.ny + nm[2] * v.nz;
    const float ny = nm[3] * v.nx + nm[4] * v.ny + nm[5] * v.nz;
    const float nz = nm[6] * v.nx + nm[7] * v.ny + nm[8] * v.nz;
    v.bx = mv[0];
    v.by = mv[1];
    v.bz = mv[2];
    v.tx = nx;
    v.ty = ny;
    v.tz = nz;
}

template <typename S>
inline void process_vertex_gpu_lit_impl(const S& state, const float (*su_scale)[2], bool su_any, DecodedVertex& v) {
    if (state.num_tex_gens != 0) {
        float tc[kMaxTexGens][3];
        std::memcpy(tc, v.tc, sizeof(tc));
        texgen_impl(state, v, tc, v);
    }
    if (su_any) {
        for (unsigned c = 0; c < kMaxTexGens; ++c) {
            v.tc[c][0] *= su_scale[c][0];
            v.tc[c][1] *= su_scale[c][1];
        }
    }
    float mv[3], clip[4];
    model_view(state, v, mv);
    store_eye(state, v, mv);
    select_raw_colors(state, v);
    project_vertex(state, mv, clip);
    v.x = clip[0]; v.y = clip[1]; v.z = clip[2]; v.w = clip[3];
    v.pn_mtx = 0;
}

} // namespace

void process_vertex_gpu_lit(const State& state, const float (*su_scale)[2], bool su_any, DecodedVertex& v) {
    process_vertex_gpu_lit_impl(state, su_scale, su_any, v);
}

// ---- GPU transform (static70) ----

bool gpu_xf_eligible(const State& state, const VertexDecoder& decoder) {
    if (!gpu_xf_enabled() || !gpu_lighting_enabled() || state.num_chans == 0 || !decoder.compactable() ||
        state.num_tex_gens > kMaxTexGens) {
        return false;
    }
    // Diagnostic limits (markers xf_no_skin, xf_no_lit, xf_no_texgen): narrow down a rendering
    // problem to one part of the transform program without a rebuild.
    const unsigned limits = gpu_xf_limits();
    if ((limits & kGpuXfNoSkin) && decoder.has_pn_idx()) {
        return false;
    }
    if ((limits & kGpuXfNoTexgen) && state.num_tex_gens != 0) {
        return false;
    }
    if (limits & kGpuXfNoLit) {
        for (unsigned c = 0; c < 4u; ++c) {
            if (state.chans[c].lighting) {
                return false;
            }
        }
    }
    for (unsigned i = 0; i < state.num_tex_gens; ++i) {
        const TexGen& gen = state.texgens[i];
        if (gen.type != TgMtx2x4 && gen.type != TgMtx3x4 && gen.type != TgSrtg) {
            return false;
        }
        if (gen.src != TgPos && gen.src != TgNrm && gen.src != TgTex0 && gen.src != TgTex1) {
            return false;
        }
        if (state.vtx_desc[AttrTex0MtxIdx + i] != AttrNone) {
            return false;  // per-vertex texture matrices (a compactable layout has none anyway)
        }
    }
    return true;
}

namespace {

void set_row(float* r, float a, float b, float c, float d) {
    r[0] = a;
    r[1] = b;
    r[2] = c;
    r[3] = d;
}

std::uint32_t g_xf_serial = 0;

} // namespace

void fill_gpu_xf_params(const State& state, const VertexDecoder& decoder, const float (*su_scale)[2], bool su_any,
                        GpuXfParams& out) {
    const bool has_pn = decoder.has_pn_idx();
    out.num_pn = has_pn ? static_cast<std::uint8_t>(kMaxPnMtx) : 1u;
    for (unsigned m = 0; m < out.num_pn; ++m) {
        unsigned src = m;
        if (!has_pn) {
            src = decoder.default_pn_mtx();
            src = src < kMaxPnMtx ? src : 0u;
        }
        for (unsigned r = 0; r < 3u; ++r) {
            std::memcpy(out.pn[m * 3u + r], &state.pn_mtx[src][r * 4u], sizeof(float) * 4u);
            const float* nm = &state.nrm_mtx[src][r * 3u];
            set_row(out.nrm[m * 3u + r], nm[0], nm[1], nm[2], 0.0f);
        }
    }
    std::memcpy(out.proj, state.proj, sizeof(out.proj));
    out.misc[0] = 0.0f;  // without PNMTXIDX the default matrix sits in slot 0
    out.misc[1] = has_pn ? 1.0f : 0.0f;
    out.misc[2] = state.vtx_desc[AttrClr0] != AttrNone ? 1.0f : 0.0f;
    out.misc[3] = state.vtx_desc[AttrClr1] != AttrNone ? 1.0f : 0.0f;
    const unsigned n = state.num_tex_gens > kMaxTexGens ? kMaxTexGens : static_cast<unsigned>(state.num_tex_gens);
    out.num_tex_gens = static_cast<std::uint8_t>(n);
    for (unsigned i = 0; i < n; ++i) {
        const TexGen& gen = state.texgens[i];
        float* w = out.tg_src[i];
        set_row(w, gen.src == TgPos ? 1.0f : 0.0f, gen.src == TgNrm ? 1.0f : 0.0f, gen.src == TgTex0 ? 1.0f : 0.0f,
                gen.src == TgTex1 ? 1.0f : 0.0f);
        float (*m)[4] = out.tg_mtx[i];
        if (gen.type == TgSrtg) {
            set_row(m[0], 1, 0, 0, 0);
            set_row(m[1], 0, 1, 0, 0);
            set_row(m[2], 0, 0, 0, 1);
        } else {
            const unsigned id = gen.mtx;
            if (id != kTexMtxIdentity && id * 4u + 12u <= kXfMemSize) {
                std::memcpy(m, state.xf_mem + id * 4u, sizeof(float) * 12u);
            } else if (id != kTexMtxIdentity && id * 4u + 8u <= kXfMemSize) {
                std::memcpy(m, state.xf_mem + id * 4u, sizeof(float) * 8u);
                set_row(m[2], 0, 0, 0, 1);
            } else {
                set_row(m[0], 1, 0, 0, 0);
                set_row(m[1], 0, 1, 0, 0);
                set_row(m[2], 0, 0, 1, 0);
            }
            if (gen.type == TgMtx2x4) {
                set_row(m[2], 0, 0, 0, 1);
            }
        }
        float (*p)[4] = out.tg_post[i];
        const unsigned slot = static_cast<unsigned>(gen.post_mtx - kPtTexMtx0) * 4u;
        if (state.dual_tex && gen.post_mtx != kPtTexMtxIdentity && gen.post_mtx >= kPtTexMtx0 && slot + 12u <= kXfDttSize) {
            std::memcpy(p, state.xf_dtt + slot, sizeof(float) * 12u);
        } else {
            set_row(p[0], 1, 0, 0, 0);
            set_row(p[1], 0, 1, 0, 0);
            set_row(p[2], 0, 0, 1, 0);
        }
        set_row(out.tg_flags[i], gen.form_ab11 ? 1.0f : 0.0f, (state.dual_tex && gen.normalize) ? 1.0f : 0.0f,
                gen.type != TgSrtg ? 1.0f : 0.0f, gen.type == TgMtx3x4 ? 1.0f : 0.0f);
        set_row(out.tg_scale[i], su_any ? su_scale[i][0] : 1.0f, su_any ? su_scale[i][1] : 1.0f, 1.0f, 1.0f);
    }
    // Coordinates past the texgen count keep their raw values (texcoords 0 and 1; the others
    // are not in a compactable layout), as on the CPU path.
    for (unsigned i = n; i < kMaxTexGens; ++i) {
        set_row(out.tg_src[i], 0.0f, 0.0f, i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f);
        float (*m)[4] = out.tg_mtx[i];
        float (*p)[4] = out.tg_post[i];
        set_row(m[0], 1, 0, 0, 0);
        set_row(m[1], 0, 1, 0, 0);
        set_row(m[2], 0, 0, 1, 0);
        set_row(p[0], 1, 0, 0, 0);
        set_row(p[1], 0, 1, 0, 0);
        set_row(p[2], 0, 0, 1, 0);
        set_row(out.tg_flags[i], 0.0f, 0.0f, 0.0f, 0.0f);
        set_row(out.tg_scale[i], su_any ? su_scale[i][0] : 1.0f, su_any ? su_scale[i][1] : 1.0f, 1.0f, 1.0f);
    }
    out.serial = ++g_xf_serial;
    if (out.serial == 0) {
        out.serial = ++g_xf_serial;
    }
}

void gpu_xf_reference(const GpuXfParams& p, const CachedVertex& v, float clip[4], float tc[kMaxTexGens][3],
                      std::uint32_t* raw0, std::uint32_t* raw1, float eye[3], float eye_nrm[3]) {
    auto dot4 = [](const float* r, float x, float y, float z, float w) { return r[0] * x + r[1] * y + r[2] * z + r[3] * w; };
    unsigned idx = p.misc[1] > 0.5f ? v.pn_mtx : static_cast<unsigned>(p.misc[0]);
    if (idx >= kMaxPnMtx) {
        idx = 0;
    }
    for (unsigned r = 0; r < 3u; ++r) {
        eye[r] = dot4(p.pn[idx * 3u + r], v.x, v.y, v.z, 1.0f);
        eye_nrm[r] = dot4(p.nrm[idx * 3u + r], v.nx, v.ny, v.nz, 0.0f);
    }
    const float cx = dot4(p.proj[0], eye[0], eye[1], eye[2], 1.0f);
    const float cy = dot4(p.proj[1], eye[0], eye[1], eye[2], 1.0f);
    const float cz = dot4(p.proj[2], eye[0], eye[1], eye[2], 1.0f);
    const float cw = dot4(p.proj[3], eye[0], eye[1], eye[2], 1.0f);
    clip[0] = cx;
    clip[1] = cy;
    clip[2] = 2.0f * cz + cw;
    clip[3] = cw;
    const std::uint32_t white = pack_rgba(255, 255, 255, 255);
    const bool has0 = p.misc[2] > 0.5f;
    const bool has1 = p.misc[3] > 0.5f;
    *raw0 = has0 ? v.color : (has1 ? v.color1 : white);
    *raw1 = (has0 && has1) ? v.color1 : white;
    for (unsigned i = 0; i < kMaxTexGens; ++i) {
        const float* w = p.tg_src[i];
        float s[4] = {v.tc[1][0], v.tc[1][1], 1.0f, 1.0f};
        if (w[0] > 0.5f) {
            s[0] = v.x; s[1] = v.y; s[2] = v.z;
        } else if (w[1] > 0.5f) {
            s[0] = v.nx; s[1] = v.ny; s[2] = v.nz;
        } else if (w[2] > 0.5f) {
            s[0] = v.tc[0][0]; s[1] = v.tc[0][1];
        }
        if (p.tg_flags[i][0] > 0.5f) {
            s[2] = 1.0f;
        }
        float t[3];
        for (unsigned r = 0; r < 3u; ++r) {
            t[r] = dot4(p.tg_mtx[i][r], s[0], s[1], s[2], s[3]);
        }
        if (p.tg_flags[i][1] > 0.5f) {
            const float l = t[0] * t[0] + t[1] * t[1] + t[2] * t[2];
            if (l >= 1.0e-20f) {
                const float inv = 1.0f / std::sqrt(l);
                t[0] *= inv;
                t[1] *= inv;
                t[2] *= inv;
            }
        }
        float o[3];
        for (unsigned r = 0; r < 3u; ++r) {
            o[r] = dot4(p.tg_post[i][r], t[0], t[1], t[2], 1.0f);
        }
        if (p.tg_flags[i][2] > 0.5f && o[2] == 0.0f) {
            o[0] = std::min(1.0f, std::max(-1.0f, o[0] * 0.5f));
            o[1] = std::min(1.0f, std::max(-1.0f, o[1] * 0.5f));
        }
        tc[i][0] = o[0] * p.tg_scale[i][0];
        tc[i][1] = o[1] * p.tg_scale[i][1];
        tc[i][2] = p.tg_flags[i][3] > 0.5f ? o[2] : 1.0f;
    }
}

void light_and_transform(const State& state, const PreparedLighting& prepared, DecodedVertex& v) {
    light_and_transform_impl(state, prepared, v);
}

void process_vertex(const State& state, const PreparedLighting& prepared, const float (*su_scale)[2], bool su_any,
                    DecodedVertex& v) {
    process_vertex_impl(state, prepared, su_scale, su_any, v);
}

void process_vertex(const VertexSnapshot& snap, DecodedVertex& v) {
    process_vertex_impl(snap, snap.prepared, snap.su_scale, snap.su_any, v);
}

void capture_vertex_snapshot(const State& state, const PreparedLighting& prepared, const float (*su_scale)[2],
                             bool su_any, VertexSnapshot& out) {
    // Only what the vertex path will read: texgen matrices when there are texgens, lights
    // and normal matrices when something is lit. The rest of the snapshot is never read.
    std::memcpy(out.vtx_desc, state.vtx_desc, sizeof(out.vtx_desc));
    std::memcpy(out.pn_mtx, state.pn_mtx, sizeof(out.pn_mtx));
    std::memcpy(out.proj, state.proj, sizeof(out.proj));
    out.ambient[0] = state.ambient[0];
    out.ambient[1] = state.ambient[1];
    out.chan_mat[0] = state.chan_mat[0];
    out.chan_mat[1] = state.chan_mat[1];
    out.num_chans = state.num_chans;
    out.num_tex_gens = state.num_tex_gens;
    out.dual_tex = state.dual_tex;
    out.su_any = su_any;
    std::memcpy(out.su_scale, su_scale, sizeof(out.su_scale));
    out.prepared = prepared;
    out.gpu_lit = false;  // vertex_worker_snapshot sets it
    if (state.num_chans != 0 && prepared.any_lit) {
        std::memcpy(out.nrm_mtx, state.nrm_mtx, sizeof(out.nrm_mtx));
        std::memcpy(out.lights, state.lights, sizeof(out.lights));
    }
    if (state.num_tex_gens != 0) {
        std::memcpy(out.texgens, state.texgens, sizeof(out.texgens));
        std::memcpy(out.xf_mem, state.xf_mem, sizeof(out.xf_mem));
        if (state.dual_tex) {
            std::memcpy(out.xf_dtt, state.xf_dtt, sizeof(out.xf_dtt));
        }
    }
}

// One GL clip row: projection row Q times the 3x4 position matrix. Q's translation is added
// into the constant term. Floating-point order differs from project-after-model-view by an ulp,
// so only the vertex worker uses this; the inline path stays on the scalar fold the tests check.
void compose_clip_row(const float q[4], const float pn[12], float out[4]) {
    for (int k = 0; k < 4; ++k) {
        out[k] = q[0] * pn[k] + q[1] * pn[4 + k] + q[2] * pn[8 + k];
    }
    out[3] += q[3];
}

float dot4(const float* row, float x, float y, float z, float w) {
#if defined(__ARM_NEON)
    return dot4n(row, x, y, z, w);
#else
    return row[0] * x + row[1] * y + row[2] * z + row[3] * w;
#endif
}

// A fixed texgen program is prepared once per job. Linear post transforms are
// composed here; normalized channels keep the two matrices separate.
struct FastTexGen {
    TexGenSrc source;
    bool ab11, projective, srtg, normalize, post;
    float matrix[12], post_matrix[12];
#if defined(__ARM_NEON)
    float columns[16], post_columns[16];
#endif
};

void prepare_fast_texgen(const VertexSnapshot& snap, const TexGen& gen, FastTexGen& out) {
    out.source=gen.src; out.ab11=gen.form_ab11;
    out.projective=gen.type==TgMtx3x4; out.srtg=gen.type==TgSrtg;
    out.normalize=snap.dual_tex && gen.normalize;
    const float identity[12]={1,0,0,0, 0,1,0,0, 0,0,1,0};
    std::memcpy(out.matrix,identity,sizeof(identity));
    const unsigned offset=unsigned(gen.mtx)*4u;
    if(!out.srtg && gen.mtx!=kTexMtxIdentity) {
        if(offset+12<=kXfMemSize)std::memcpy(out.matrix,snap.xf_mem+offset,sizeof(out.matrix));
        else if(offset+8<=kXfMemSize){
            std::memcpy(out.matrix,snap.xf_mem+offset,8*sizeof(float));
            const float q[4]={0,0,0,1};std::memcpy(out.matrix+8,q,sizeof(q));
        }
    }
    if(out.srtg || gen.type==TgMtx2x4) {
        const float q[4]={0,0,0,1}; std::memcpy(out.matrix+8,q,sizeof(q));
    }
    const unsigned post=unsigned(gen.post_mtx-kPtTexMtx0)*4u;
    out.post=snap.dual_tex && gen.post_mtx!=kPtTexMtxIdentity && gen.post_mtx>=kPtTexMtx0 && post+12<=kXfDttSize;
    if(out.post) {
        std::memcpy(out.post_matrix,snap.xf_dtt+post,sizeof(out.post_matrix));
        if(!out.normalize) {
            float composed[12];
            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c) {
                const float* row=out.post_matrix+r*4;
                composed[r*4+c]=(row[0]*out.matrix[c]+row[1]*out.matrix[4+c])+row[2]*out.matrix[8+c];
                if(c==3)composed[r*4+c]+=row[3];
            }
            std::memcpy(out.matrix,composed,sizeof(composed));out.post=false;
        }
    }
#if defined(__ARM_NEON)
    // Evaluate all three output coordinates together, avoiding three horizontal
    // NEON dot-product reductions for every vertex and every texture channel.
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r) {
        out.columns[c*4+r]=r<3?out.matrix[r*4+c]:0;
        if(out.post)out.post_columns[c*4+r]=r<3?out.post_matrix[r*4+c]:0;
    }
#endif
}

inline void apply_fast_tex_matrix(const FastTexGen& gen, bool post, float x,float y,float z,float* result) {
#if defined(__ARM_NEON)
    const float* c=post?gen.post_columns:gen.columns;
    float32x4_t v=vmulq_n_f32(vld1q_f32(c),x);
    v=vmlaq_n_f32(v,vld1q_f32(c+4),y);
    v=vmlaq_n_f32(v,vld1q_f32(c+8),z);
    v=vaddq_f32(v,vld1q_f32(c+12));
    vst1_f32(result,vget_low_f32(v));result[2]=vgetq_lane_f32(v,2);
#else
    mul_3x4_vec(post?gen.post_matrix:gen.matrix,x,y,z,1,result);
#endif
}

void fast_texgen(const VertexSnapshot& snap, const FastTexGen* gens, DecodedVertex& v) {
    if(snap.num_tex_gens==0)return;
    float raw_tc[kMaxTexGens][3];std::memcpy(raw_tc,v.tc,sizeof(raw_tc));
    for(unsigned i=0;i<snap.num_tex_gens && i<kMaxTexGens;++i) {
        const auto& g=gens[i];float x=0,y=0,z=1;
        switch(g.source) {
        case TgPos:x=v.x;y=v.y;z=v.z;break;
        case TgNrm:x=v.nx;y=v.ny;z=v.nz;break;
        case TgBinrm:x=v.bx;y=v.by;z=v.bz;break;
        case TgTangent:x=v.tx;y=v.ty;z=v.tz;break;
        case TgColor0:case TgColor1:{
            const auto c=g.source==TgColor0?v.color:v.color1;
            x=float(ch8(c,0))/255.0f;y=float(ch8(c,8))/255.0f;z=float(ch8(c,16))/255.0f;break;
        }
        default:{const unsigned slot=unsigned(g.source-TgTex0)&7u;x=raw_tc[slot][0];y=raw_tc[slot][1];break;}
        }
        if(g.ab11)z=1;
        float result[3];apply_fast_tex_matrix(g,false,x,y,z,result);
        if(g.normalize)normalize3(result);
        if(g.post){float tmp[3];apply_fast_tex_matrix(g,true,result[0],result[1],result[2],tmp);std::memcpy(result,tmp,sizeof(tmp));}
        if(!g.srtg && result[2]==0){result[0]=std::min(1.0f,std::max(-1.0f,result[0]*0.5f));result[1]=std::min(1.0f,std::max(-1.0f,result[1]*0.5f));}
        v.tc[i][0]=result[0]*(snap.su_any?snap.su_scale[i][0]:1);
        v.tc[i][1]=result[1]*(snap.su_any?snap.su_scale[i][1]:1);
        v.tc[i][2]=g.projective?result[2]:1;
    }
}

// Why a job's vertices could not take the fast path (the PERF line's `slow:` split).
enum SlowWhy : int { kFastOk = 0, kSlowCpuLit, kSlowGenCount, kSlowGenKind };

struct FastXf {
    bool use = false;
    bool lit = false;  // GPU-lit: raw colours, eye position and normal instead of lighting
    unsigned ngen = 0;
    // Clip, eye and normal matrices are composed per position-matrix slot on first use (a job
    // touches one to three of the ten; composing all ten per job was ~1500 cycles).
    std::uint32_t have = 0;
    // Host: the clip rows. NEON: the clip columns (x, y, z, 1 multipliers) for vmla.
    float clip[kMaxPnMtx][16];
#if defined(__ARM_NEON)
    float eye[kMaxPnMtx][16];  // pn_mtx columns, w lane 0
    float nrm[kMaxPnMtx][12];  // nrm_mtx columns, w lane 0
#endif
    FastTexGen tex[kMaxTexGens];
    bool dynamic_tex = false;
    bool compact_restore = true;
    float pz[4]{};
    int why = 0;  // SlowWhy when !use
};

// General texgens share the composed clip/eye/normal path too. Only genuinely
// CPU-lit jobs need the old vertex pipeline; dynamic texture matrices use the
// original texgen evaluator while retaining fast transform and packing.
bool setup_fast_xf(const VertexSnapshot& snap, FastXf& f) {
    f.why = kFastOk;
    if (snap.prepared.any_lit && !snap.gpu_lit) {
        f.why = kSlowCpuLit;
        return false;
    }
    if (!g_prepared_texgen) {
        if(snap.num_tex_gens>2 || (snap.num_tex_gens==0 && snap.su_any)){f.why=kSlowGenCount;return false;}
        for(unsigned i=0;i<snap.num_tex_gens;++i) {
            const auto& g=snap.texgens[i];
            if(g.type!=TgMtx2x4 || g.normalize || g.src!=static_cast<TexGenSrc>(TgTex0+i) || snap.vtx_desc[AttrTex0MtxIdx+i]!=AttrNone){f.why=kSlowGenKind;return false;}
        }
    }
    f.lit = snap.gpu_lit;
    for (unsigned i=0;i<snap.num_tex_gens && i<kMaxTexGens;++i) {
        if(snap.vtx_desc[AttrTex0MtxIdx+i]!=AttrNone)f.dynamic_tex=true;
        const auto src=snap.texgens[i].src;
        if(src==TgBinrm || src==TgTangent || (src>=TgTex2 && src<=TgTex7))f.compact_restore=false;
        prepare_fast_texgen(snap,snap.texgens[i],f.tex[i]);
    }
    if(f.dynamic_tex)f.compact_restore=false;
    const float* p2 = snap.proj[2];
    const float* p3 = snap.proj[3];
    for (unsigned k = 0; k < 4u; ++k) {
        f.pz[k] = 2.0f * p2[k] + p3[k];
    }
    f.have = 0;
    f.ngen = snap.num_tex_gens;
    f.use = true;
    return true;
}

inline void fast_compose(const VertexSnapshot& snap, FastXf& f, unsigned s) {
    const float* pn = snap.pn_mtx[s];
    float rows[16];
    compose_clip_row(snap.proj[0], pn, rows);
    compose_clip_row(snap.proj[1], pn, rows + 4);
    compose_clip_row(f.pz, pn, rows + 8);
    compose_clip_row(snap.proj[3], pn, rows + 12);
#if defined(__ARM_NEON)
    for (unsigned c = 0; c < 4u; ++c) {
        for (unsigned r = 0; r < 4u; ++r) {
            f.clip[s][c * 4u + r] = rows[r * 4u + c];
        }
    }
    if (f.lit) {
    for (unsigned c = 0; c < 4u; ++c) {
        f.eye[s][c * 4u + 0] = pn[c];
        f.eye[s][c * 4u + 1] = pn[4u + c];
        f.eye[s][c * 4u + 2] = pn[8u + c];
        f.eye[s][c * 4u + 3] = 0.0f;
    }
    const float* nm = snap.nrm_mtx[s];
    for (unsigned c = 0; c < 3u; ++c) {
        f.nrm[s][c * 4u + 0] = nm[c];
        f.nrm[s][c * 4u + 1] = nm[3u + c];
        f.nrm[s][c * 4u + 2] = nm[6u + c];
        f.nrm[s][c * 4u + 3] = 0.0f;
    }
    }
#else
    std::memcpy(f.clip[s], rows, sizeof(rows));
#endif
    f.have |= 1u << s;
}

// Texgen, colours, eye data and the clip transform of one decoded vertex, written straight to
// its packed slot (pack_vertex's layout). `v` keeps the processed values only when `mirror`.
inline void fast_xf_pack(const VertexSnapshot& snap, FastXf& f, DecodedVertex& v, std::uint8_t* out, unsigned ntc,
                         bool mirror) {
    if(f.dynamic_tex) {
        float raw_tc[kMaxTexGens][3];std::memcpy(raw_tc,v.tc,sizeof(raw_tc));
        texgen_impl(snap,v,raw_tc,v);
        if(snap.su_any)for(unsigned i=0;i<kMaxTexGens;++i){v.tc[i][0]*=snap.su_scale[i][0];v.tc[i][1]*=snap.su_scale[i][1];}
    } else {
        fast_texgen(snap,f.tex,v);
        // With no texgens the old pipeline still scales the default coordinate.
        if(snap.num_tex_gens==0 && snap.su_any)for(unsigned i=0;i<kMaxTexGens;++i){v.tc[i][0]*=snap.su_scale[i][0];v.tc[i][1]*=snap.su_scale[i][1];}
    }
    const unsigned slot = v.pn_mtx < kMaxPnMtx ? v.pn_mtx : 0;
    if ((f.have & (1u << slot)) == 0) {
        fast_compose(snap, f, slot);
    }
    const float x = v.x, y = v.y, z = v.z;
    std::uint8_t* extra = out + kPackOffTex + 12u * ntc;
#if defined(__ARM_NEON)
    // Everything is computed first and then written front to back: the ring is uncached memory,
    // where sequential stores merge in the write buffer.
    const float* c = f.clip[slot];
    float32x4_t clip = vmlaq_n_f32(vld1q_f32(c + 12), vld1q_f32(c), x);
    clip = vmlaq_n_f32(clip, vld1q_f32(c + 4), y);
    clip = vmlaq_n_f32(clip, vld1q_f32(c + 8), z);
    float ev[4], nv[4];
    if (f.lit) {
        const float* e = f.eye[slot];
        float32x4_t eye = vmlaq_n_f32(vld1q_f32(e + 12), vld1q_f32(e), x);
        eye = vmlaq_n_f32(eye, vld1q_f32(e + 4), y);
        eye = vmlaq_n_f32(eye, vld1q_f32(e + 8), z);
        const float* n = f.nrm[slot];
        float32x4_t nrm = vmulq_n_f32(vld1q_f32(n), v.nx);
        nrm = vmlaq_n_f32(nrm, vld1q_f32(n + 4), v.ny);
        nrm = vmlaq_n_f32(nrm, vld1q_f32(n + 8), v.nz);
        vst1q_f32(ev, eye);
        vst1q_f32(nv, nrm);
        if (mirror) {
            v.bx = ev[0]; v.by = ev[1]; v.bz = ev[2];
            v.tx = nv[0]; v.ty = nv[1]; v.tz = nv[2];
        }
        select_raw_colors(snap, v);
    } else {
        float mv[3] = {};
        lighting_prepared(snap, v, snap.prepared, mv);
    }
    vst1q_f32(reinterpret_cast<float*>(out), clip);
    if (mirror) {
        float cv[4];
        vst1q_f32(cv, clip);
        v.x = cv[0]; v.y = cv[1]; v.z = cv[2]; v.w = cv[3];
    }
    v.pn_mtx = 0;
    std::memcpy(out + kPackOffColor0, &v.color, 4);
    std::memcpy(out + kPackOffColor1, &v.color1, 4);
    std::memcpy(out + kPackOffTex, v.tc, 12u * ntc);
    if (f.lit) {
        const float ex[6] = {ev[0], ev[1], ev[2], nv[0], nv[1], nv[2]};
        std::memcpy(extra, ex, sizeof(ex));
    }
    return;
#else
    const float* m = f.clip[slot];
    if (f.lit) {
        float mv[3];
        mul_3x4_vec(snap.pn_mtx[slot], x, y, z, 1.0f, mv);
        store_eye(snap, v, mv);
        const float ex[6] = {v.bx, v.by, v.bz, v.tx, v.ty, v.tz};
        std::memcpy(extra, ex, sizeof(ex));
        select_raw_colors(snap, v);
    }
    v.x = dot4(m, x, y, z, 1.0f);
    v.y = dot4(m + 4, x, y, z, 1.0f);
    v.z = dot4(m + 8, x, y, z, 1.0f);
    v.w = dot4(m + 12, x, y, z, 1.0f);
    if (!f.lit) {
        float mv[3] = {};
        lighting_prepared(snap, v, snap.prepared, mv);
    }
    const float pos[4] = {v.x, v.y, v.z, v.w};
    std::memcpy(out, pos, 16);
#endif
    v.pn_mtx = 0;
    std::memcpy(out + kPackOffColor0, &v.color, 4);
    std::memcpy(out + kPackOffColor1, &v.color1, 4);
    std::memcpy(out + kPackOffTex, v.tc, 12u * ntc);
}

std::uint32_t process_vertex_job(const VertexJob& job) {
    const VertexSnapshot& snap = *job.snap;
    VertexDecoder::Windows windows;
    windows.clear();
    const unsigned ntc = pack_ntc(snap.num_tex_gens);
    FastXf fast;
    setup_fast_xf(snap, fast);
    const int slow_why = fast.why;
    const bool mirror = job.verts != nullptr;
    std::uint32_t errors = 0;
    std::uint32_t fast_n = 0;
    const std::uint8_t* p = job.bytes;
    DecodedVertex local;
    for (std::uint32_t i = 0; i < job.count; ++i) {
        const std::uint32_t idx = job.first + i;
        if (p != nullptr && i != 0) {
            p += job.stride;
        }
        if (job.remap[idx] >= 0) {
            continue;
        }
        DecodedVertex& v = mirror ? job.verts[idx] : local;
        if (job.cached != nullptr) {
            __builtin_prefetch(job.cached + i + 2);
            if (fast.use && !mirror && fast.compact_restore) {
                snap.decoder.restore_for_pack(job.cached[i], v);
            } else {
                snap.decoder.restore(job.cached[i], v);
            }
        } else {
            __builtin_prefetch(p + job.stride);
            const std::uint8_t* next = nullptr;
            if (!snap.decoder.decode(p, p + job.stride, v, &next, windows, false)) {
                ++errors;
            }
            if (job.fill != nullptr) {
                VertexDecoder::store(v, job.fill[i]);
            }
        }
        std::uint8_t* out = job.arena + job.offset[idx];
        if (fast.use) {
            fast_xf_pack(snap, fast, v, out, ntc, mirror);
            ++fast_n;
        } else {
            if (snap.gpu_lit) {
                process_vertex_gpu_lit_impl(snap, snap.su_scale, snap.su_any, v);
            } else {
                process_vertex(snap, v);
            }
            pack_vertex(v, ntc, out, snap.gpu_lit);
        }
    }
    if (fast_n != 0) {
        perf::count(perf::kFastVerts, fast_n);
    }
    if (snap.gpu_lit) {
        perf::count(perf::kGpuLitVerts, job.count);
    }
    if (!fast.use) {
        perf::count(slow_why == kSlowCpuLit ? perf::kSlowCpuLit
                    : slow_why == kSlowGenCount ? perf::kSlowGenCount : perf::kSlowGenKind, job.count);
    }
    return errors;
}

std::uint32_t process_snapshot_vertices(const VertexSnapshot& snap, DecodedVertex* verts, const std::uint8_t* bytes,
                                       std::uint32_t first, std::uint32_t count, std::uint32_t stride,
                                       const std::int32_t* remap, std::uint8_t* arena, const std::uint32_t* offset) {
    VertexJob job;
    job.snap = &snap;
    job.verts = verts;
    job.bytes = bytes;
    job.first = first;
    job.count = count;
    job.stride = stride;
    job.remap = remap;
    job.arena = arena;
    job.offset = offset;
    return process_vertex_job(job);
}

} // namespace mkwv::gx
