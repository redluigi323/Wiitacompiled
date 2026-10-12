#include "mkwv/gx/fifo.hpp"
#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/tev.hpp"
#include "mkwv/gx/efb_copy.hpp"
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/perf.hpp"
#include "mkwv/gx/gpu_mesh.hpp"
#include "mkwv/gx/vertex_worker.hpp"
#include "mkwv/gx/capture.hpp"
#include "mkwv/gx/dl_cache.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace mkwv::gx {
namespace {

// Reuse vertices only within an uninterrupted run of primitive commands.
// Every state command, vertex-cache invalidation and process boundary retires
// the entries, so array bindings, matrices, lighting and texgen cannot go stale.
// Big enough for a whole mesh, not just the 8-deep GX cache: a 512-slot table thrashed
// once a state epoch held more than a few hundred unique packets (race reuse sat at ~20%).
constexpr unsigned kReuseSlots = 4096;
constexpr unsigned kReuseBytes = 32;
struct VertexReuse {
    std::uint64_t epoch = 0;
    std::uint32_t hash = 0;
    std::uint32_t index = 0;  // the processed vertex in Processor::verts (this batch)
    std::uint8_t bytes[kReuseBytes];
};
VertexReuse g_reuse[kReuseSlots];
// Frame skipping: state is decoded, primitives and EFB copies are not (draw_state.hpp).
bool g_skip_draws = false;
std::uint32_t g_skipped_prims = 0;
std::uint64_t g_reuse_epoch = 0;
// BP material, texture and raster writes cannot change VCD/VAT, arrays or default matrix
// indices. Keep layout preparation separate from processed-vertex/draw-state invalidation.
std::uint64_t g_layout_epoch = 0;
bool g_reuse_enabled = true;
std::uint32_t vertex_packet_hash(const std::uint8_t* p, unsigned bytes) {
    // A word at a time (collisions are settled by the memcmp against the stored packet).
    std::uint32_t h = 2166136261u ^ bytes;
    unsigned i = 0;
    for (; i + 4u <= bytes; i += 4u) {
        std::uint32_t w;
        std::memcpy(&w, p + i, 4);
        h = (h ^ w) * 0x9E3779B1u;
        h ^= h >> 15;
    }
    for (; i < bytes; ++i) h = (h ^ p[i]) * 16777619u;
    return h ^ (h >> 13);
}

// First FIFO decode error of the current frame. Deliberately file-scope rather than fields on
// Processor: mkwv/gx/fifo.hpp reaches every translated shard through memory_access.h ->
// hle_fifo.hpp, so growing that struct would force a full ~29k-TU rebuild for a diagnostic.
std::uint32_t g_err_site = 0;
std::uint32_t g_err_cmd = 0;
std::uint32_t g_err_offset = 0;
std::uint32_t g_err_depth = 0;
std::uint32_t g_err_size = 0;

void clear_fifo_error() {
    g_err_site = 0;
    g_err_cmd = 0;
    g_err_offset = 0;
    g_err_depth = 0;
    g_err_size = 0;
}

// Where this frame's XF loads actually went. All ten GX texture matrices read back as
// all-zero (texmtx zero=3FF) while the PN matrices are fine, and both land through the
// same copy_xf_mem, so the question is whether a write to the texture-matrix range ever
// arrives at all. Same file-scope trick and the same reason as the error state above.
std::uint32_t g_xf_pn = 0;     // XF 0x000-0x077, the position/normal matrices
std::uint32_t g_xf_tex = 0;    // XF 0x078-0x0EF, GX_TEXMTX0..9
std::uint32_t g_xf_ident = 0;  // XF 0x0F0-0x0FF, GX_IDENTITY
std::uint32_t g_xf_nrm = 0;    // XF 0x400 block
std::uint32_t g_xf_dtt = 0;    // XF 0x500, the post-transform matrices
std::uint32_t g_xf_light = 0;  // XF 0x600 block
std::uint32_t g_xf_reg = 0;    // XF 0x1000 registers
std::uint32_t g_xf_drop = 0;   // an address copy_xf_mem refuses and that is not a register
std::uint32_t g_xf_tex_addr = 0xFFFFu;  // first texture-matrix address seen, if any
std::uint32_t g_xf_tex_count = 0;       // and how many words it carried

// EFB -> texture copies recorded this frame, in stream order, plus the copy clear colour
// GXSetCopyClear programs into BP 0x4F/0x50. File-scope for the same reason as the rest of
// this block: Processor lives in a shard-visible header.
EfbCopyEvent g_efb_events[kMaxEfbCopyEvents];
unsigned g_efb_event_count = 0;
std::uint32_t g_copy_clear_rgba = 0;

GuestResolver g_guest_resolver = nullptr;
DirectResolver g_direct_resolver = nullptr;
WindowResolver g_window_resolver = nullptr;
bool g_prefix_mode = false;
std::uint32_t g_prefix_stop = 0;
FifoFullFn g_fifo_full = nullptr;
BatchFlushFn g_batch_flush = nullptr;
std::uint32_t g_batches_flushed = 0;  // batches handed to g_batch_flush since end_frame()

// TMEM image for TLUTs. BP 0x65 (GXLoadTlut) copies the palette out of main RAM into TMEM at
// load time, exactly as the hardware does, so a later rewrite of the source buffer does not
// change a palette that is already loaded. 512 KB covers every 10-bit offset in 512-byte units.
std::uint8_t g_tmem[1024u * 512u];
std::uint32_t g_tlut_src_phys = 0;  // BP 0x64: physical address >> 5, of the next load

// Per-draw raster/texture snapshot - see draw_state.hpp. ~680 bytes a draw.
DrawExtra g_draw_extra[kMaxDraws];
std::uint16_t g_draw_extra_slot[kMaxDraws];
std::uint64_t g_draw_snapshot_epoch = 0;
const Processor* g_draw_extra_owner = nullptr;

// The guest address and host bytes of the display list being processed at each call depth
// (0 is the FIFO itself): a DL primitive's vertex data is identified by its guest address.
std::uint32_t g_dl_guest[8];
const std::uint8_t* g_dl_host[8];

// Per draw: a 64-bit hash of everything the submission merger compares (draw_merge_key).
std::uint64_t g_draw_key[kMaxDraws];

std::uint64_t draw_key_of(const DrawCall& draw, const DrawExtra& x) {
    DrawCall d;
    std::memcpy(&d, &draw, sizeof(d));
    d.prim = Triangles;
    d.fmt = VtxFmt0;
    d.first_vertex = 0;
    d.vertex_count = 0;
    std::uint64_t h = hash_bytes64(&d, sizeof(d), 0x5bd1e995u);
    // The lighting parameters only matter when they are on (else all zero).
    const std::size_t head = offsetof(DrawExtra, lit);
    h = hash_bytes64(&x, static_cast<std::uint32_t>(head), h);
    if (x.lit.on != 0) {
        h = hash_bytes64(&x.lit, sizeof(x.lit), h);
    }
    const std::size_t tail0 = offsetof(DrawExtra, lit) + sizeof(GpuLitParams);
    const std::size_t tail1 = offsetof(DrawExtra, tg0_mtx);
    if (tail1 > tail0) {
        h = hash_bytes64(reinterpret_cast<const std::uint8_t*>(&x) + tail0, static_cast<std::uint32_t>(tail1 - tail0), h);
    }
    return h | 1u;  // never 0 (0 means "no key")
}

bool g_dl_cache_on = true;

// GPU lighting (draw_state.hpp): off until the renderer turns it on; per draw, whether its
// vertices were packed for it.
bool g_gpu_lighting = false;
std::uint8_t g_draw_glit[kMaxDraws];

void snapshot_draw_extra(const State& st, DrawExtra& x) {
    std::memcpy(x.tex, st.texmaps, sizeof(x.tex));
    for (unsigned t = 0; t < kMaxTexMaps; ++t) {
        const TexObj& tex = st.texmaps[t];
        // Every texture carries the invalidation epoch it was bound under; the renderer
        // re-validates a cached conversion when the epoch has moved on (GXInvalidateTexAll).
        x.tex[t].version = tex.version != 0 ? tex.version : st.tex_epoch;
        if (tex.tlut_tmem != 0xFFFFu) {
            // Hardware path: the map names a TMEM TLUT (BP 0x98+).
            const TmemTlut& slot = st.tmem_tlut[tex.tlut_tmem & 0x3FFu];
            x.tlut_valid[t] = slot.lines != 0;
            TlutObj tl{};
            tl.data = g_tmem + (static_cast<std::uint32_t>(tex.tlut_tmem & 0x3FFu) << 9);
            tl.format = tex.tlut_fmt;
            const std::uint32_t entries = static_cast<std::uint32_t>(slot.lines) * 16u;
            const std::uint32_t room = (sizeof(g_tmem) - (static_cast<std::uint32_t>(tex.tlut_tmem & 0x3FFu) << 9)) / 2u;
            tl.entries = static_cast<std::uint16_t>(entries < room ? (entries > 0xFFFFu ? 0xFFFFu : entries) : room);
            tl.id = tex.tlut_tmem;
            tl.version = slot.version;
            x.tlut[t] = x.tlut_valid[t] ? tl : TlutObj{};
            continue;
        }
        const std::uint32_t idx = tex.tlut;
        x.tlut_valid[t] = idx < kMaxTluts && st.tluts[idx].data != nullptr;
        x.tlut[t] = x.tlut_valid[t] ? st.tluts[idx] : TlutObj{};
    }
    std::memcpy(x.ind, st.ind, sizeof(x.ind));
    std::memcpy(x.ind_mtx, st.ind_mtx, sizeof(x.ind_mtx));
    x.num_ind_stages = st.num_ind_stages;
    std::memcpy(x.render_vp, st.render_vp, sizeof(x.render_vp));
    std::memcpy(x.render_scissor, st.render_scissor, sizeof(x.render_scissor));
    x.scissor_x = st.scissor_x;
    x.scissor_y = st.scissor_y;
    x.scissor_w = st.scissor_w;
    x.scissor_h = st.scissor_h;
    x.scissor_off_x = st.scissor_off_x;
    x.scissor_off_y = st.scissor_off_y;
    x.has_render_vp = st.has_render_vp;
    x.has_render_scissor = st.has_render_scissor;
    x.has_scissor = st.has_scissor;
    x.depth_test = st.depth_test;
    x.depth_write = st.depth_write;
    x.depth_func = st.depth_func;
    x.cull_mode = st.cull_mode;
    x.color_update = st.color_update;
    x.alpha_update = st.alpha_update;
    for (unsigned t = 0; t < kMaxTexGens; ++t) {
        x.tg_type[t] = st.texgens[t].type;
    }
    x.tg0_mtx = st.texgens[0].mtx;
    if (x.tg0_mtx != kTexMtxIdentity && static_cast<unsigned>(x.tg0_mtx) * 4u + 8u <= kXfMemSize) {
        std::memcpy(x.tg0_rows, st.xf_mem + static_cast<unsigned>(x.tg0_mtx) * 4u, sizeof(x.tg0_rows));
    } else {
        const float ident[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        std::memcpy(x.tg0_rows, ident, sizeof(x.tg0_rows));
    }
}
GuestFetchFailure g_fetch_fail;

// CP 0xAC-0xAF hold the physical base of the four indexed-XF arrays (position, normal,
// texture and light matrices) and 0xBC-0xBF their strides. The decoder never read them.
std::uint32_t g_xf_array_base[4] = {0, 0, 0, 0};
std::uint32_t g_xf_array_stride[4] = {0, 0, 0, 0};

// GX array registers hold physical addresses; guest memory is addressed by the cached
// effective address.
std::uint32_t physical_to_effective(std::uint32_t phys) {
    if (phys >= 0x80000000u) {
        return phys;
    }
    if (phys < 0x02000000u) {
        return phys | 0x80000000u;  // MEM1
    }
    if (phys >= 0x10000000u && phys < 0x14000000u) {
        return phys + 0x80000000u;  // MEM2
    }
    return phys;
}

void clear_efb_events() {
    g_efb_event_count = 0;
    g_fetch_fail = GuestFetchFailure{};
}

void clear_xf_stats() {
    g_xf_pn = 0;
    g_xf_tex = 0;
    g_xf_ident = 0;
    g_xf_nrm = 0;
    g_xf_dtt = 0;
    g_xf_light = 0;
    g_xf_reg = 0;
    g_xf_drop = 0;
    g_xf_tex_addr = 0xFFFFu;
    g_xf_tex_count = 0;
}

void note_xf_load(std::uint32_t addr, std::uint32_t count, bool accepted) {
    if (addr < 120u) {
        ++g_xf_pn;
    } else if (addr < 240u) {
        ++g_xf_tex;
        if (g_xf_tex_addr == 0xFFFFu) {
            g_xf_tex_addr = addr;
            g_xf_tex_count = count;
        }
    } else if (addr < 256u) {
        ++g_xf_ident;
    } else if (addr >= 0x400u && addr < 0x45Au) {
        ++g_xf_nrm;
    } else if (addr >= 0x500u && addr < 0x600u) {
        ++g_xf_dtt;
    } else if (addr >= 0x600u && addr < 0x680u) {
        ++g_xf_light;
    } else if (addr >= 0x1000u) {
        ++g_xf_reg;
    } else if (!accepted) {
        ++g_xf_drop;
    }
}

std::uint16_t bswap16(std::uint16_t value) {
    return static_cast<std::uint16_t>((value << 8) | (value >> 8));
}

std::uint32_t bswap32(std::uint32_t value) {
    return (value << 24) | ((value << 8) & 0x00FF0000u) | ((value >> 8) & 0x0000FF00u) | (value >> 24);
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

std::uint16_t read_u16(const std::uint8_t* p, bool be) {
    const std::uint16_t v = load_u16(p);
    return be ? bswap16(v) : v;
}

std::uint32_t read_u32(const std::uint8_t* p, bool be) {
    const std::uint32_t v = load_u32(p);
    return be ? bswap32(v) : v;
}

std::uint64_t read_u64(const std::uint8_t* p, bool be) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        const std::uint8_t b = be ? p[i] : p[7 - i];
        value = (value << 8) | b;
    }
    return value;
}

float read_f32(const std::uint8_t* p, bool be) {
    const std::uint32_t bits = read_u32(p, be);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool remain(std::uint32_t pos, std::uint32_t size, std::uint32_t need) {
    return pos <= size && (size - pos) >= need;
}

float decode_fog_partial(std::uint32_t value) {
    const std::uint32_t bits =
        (bp_bits(value, 1, 19) << 31) | (bp_bits(value, 8, 11) << 23) | (bp_bits(value, 11, 0) << 12);
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void refresh_fog_ab(State& state) {
    const std::uint32_t shift = state.fog.raw2 & 0x1Fu;
    state.fog.a = std::ldexp(decode_fog_partial(state.fog.raw0), static_cast<int>(shift));
    const float mant = static_cast<float>(state.fog.raw1 & 0x00FFFFFFu) / 8388638.0f;
    state.fog.b = std::ldexp(mant, static_cast<int>(shift) - 1);
}

void apply_tev_color_op(TevStage& s, std::uint32_t value) {
    s.color_clamp = bp_bits(value, 1, 19) != 0;
    s.color_dest = static_cast<std::uint8_t>(bp_bits(value, 2, 22));
    if (bp_bits(value, 2, 16) == 3) {
        const std::uint32_t hw = bp_bits(value, 1, 18) | (bp_bits(value, 2, 20) << 1);
        s.color_op = static_cast<std::uint8_t>(hw + 8u);
        s.color_bias = 0;
        s.color_shift = 0;
    } else {
        s.color_op = static_cast<std::uint8_t>(bp_bits(value, 1, 18));
        s.color_bias = static_cast<std::uint8_t>(bp_bits(value, 2, 16));
        s.color_shift = static_cast<std::uint8_t>(bp_bits(value, 2, 20));
    }
}

void apply_tev_alpha_op(TevStage& s, std::uint32_t value) {
    s.alpha_clamp = bp_bits(value, 1, 19) != 0;
    s.alpha_dest = static_cast<std::uint8_t>(bp_bits(value, 2, 22));
    if (bp_bits(value, 2, 16) == 3) {
        const std::uint32_t hw = bp_bits(value, 1, 18) | (bp_bits(value, 2, 20) << 1);
        s.alpha_op = static_cast<std::uint8_t>(hw + 8u);
        s.alpha_bias = 0;
        s.alpha_shift = 0;
    } else {
        s.alpha_op = static_cast<std::uint8_t>(bp_bits(value, 1, 18));
        s.alpha_bias = static_cast<std::uint8_t>(bp_bits(value, 2, 16));
        s.alpha_shift = static_cast<std::uint8_t>(bp_bits(value, 2, 20));
    }
}

void decode_tev_order(State& state, std::uint32_t reg, std::uint32_t value) {
    const unsigned idx = static_cast<unsigned>(reg - 0x28u);
    const unsigned stage0 = idx * 2u;
    const unsigned stage1 = stage0 + 1u;
    auto apply = [&](unsigned stage, unsigned map_sh, unsigned coord_sh, unsigned en_sh, unsigned chan_sh) {
        if (stage >= kTevStages) {
            return;
        }
        auto& s = state.tev[stage];
        s.texmap = static_cast<std::uint8_t>(bp_bits(value, 3, map_sh));
        s.texcoord = static_cast<std::uint8_t>(bp_bits(value, 3, coord_sh));
        s.tex_enabled = bp_bits(value, 1, en_sh) != 0;
        if (!s.tex_enabled) {
            s.texmap = kTexMapNull;
        }
        // Hardware channel codes (Aurora command_processor.cpp r2c): 0/2/4 COLOR0A0,
        // 1/3 COLOR1A1, 5 ALPHA_BUMP, 6 ALPHA_BUMPN, 7 ZERO.
        static const std::uint8_t kR2c[8] = {kRasColor0, kRasColor1,    kRasColor0,     kRasColor1,
                                             kRasColor0, kRasAlphaBump, kRasAlphaBumpN, kRasZero};
        s.ras_chan = kR2c[bp_bits(value, 3, chan_sh)];
    };
    apply(stage0, 0, 3, 6, 7);
    apply(stage1, 12, 15, 18, 19);
}

void decode_scissor(State& state) {
    const std::uint32_t scis0 = state.bp[0x20];
    const std::uint32_t scis1 = state.bp[0x21];
    const std::int32_t tp = static_cast<std::int32_t>(bp_bits(scis0, 11, 0)) - 342;
    const std::int32_t lf = static_cast<std::int32_t>(bp_bits(scis0, 11, 12)) - 342;
    const std::int32_t bm = static_cast<std::int32_t>(bp_bits(scis1, 11, 0)) - 342;
    const std::int32_t rt = static_cast<std::int32_t>(bp_bits(scis1, 11, 12)) - 342;
    state.scissor_x = lf;
    state.scissor_y = tp;
    state.scissor_w = rt - lf + 1;
    state.scissor_h = bm - tp + 1;
    if (state.scissor_w < 0) {
        state.scissor_w = 0;
    }
    if (state.scissor_h < 0) {
        state.scissor_h = 0;
    }
    state.has_scissor = true;
}

Primitive primitive_from_cmd(std::uint8_t cmd) {
    switch (cmd & kOpcodeMask) {
    case Quads:
    case 0x88:
        return Quads;
    case Triangles:
        return Triangles;
    case TriangleStrip:
        return TriangleStrip;
    case TriangleFan:
        return TriangleFan;
    case Lines:
        return Lines;
    case LineStrip:
        return LineStrip;
    case Points:
        return Points;
    default:
        return Triangles;
    }
}

void handle_cp(State& state, std::uint8_t addr, std::uint32_t value) {
    switch (addr) {
    case 0x50: {
        state.vtx_desc[AttrPnMtxIdx] = static_cast<AttrType>(bp_bits(value, 1, 0));
        state.vtx_desc[AttrTex0MtxIdx] = static_cast<AttrType>(bp_bits(value, 1, 1));
        state.vtx_desc[AttrTex1MtxIdx] = static_cast<AttrType>(bp_bits(value, 1, 2));
        state.vtx_desc[AttrTex2MtxIdx] = static_cast<AttrType>(bp_bits(value, 1, 3));
        state.vtx_desc[AttrTex3MtxIdx] = static_cast<AttrType>(bp_bits(value, 1, 4));
        state.vtx_desc[AttrTex4MtxIdx] = static_cast<AttrType>(bp_bits(value, 1, 5));
        state.vtx_desc[AttrTex5MtxIdx] = static_cast<AttrType>(bp_bits(value, 1, 6));
        state.vtx_desc[AttrTex6MtxIdx] = static_cast<AttrType>(bp_bits(value, 1, 7));
        state.vtx_desc[AttrTex7MtxIdx] = static_cast<AttrType>(bp_bits(value, 1, 8));
        state.vtx_desc[AttrPos] = static_cast<AttrType>(bp_bits(value, 2, 9));
        state.vtx_desc[AttrNrm] = static_cast<AttrType>(bp_bits(value, 2, 11));
        state.vtx_desc[AttrClr0] = static_cast<AttrType>(bp_bits(value, 2, 13));
        state.vtx_desc[AttrClr1] = static_cast<AttrType>(bp_bits(value, 2, 15));
        return;
    }
    case 0x60: {
        state.vtx_desc[AttrTex0] = static_cast<AttrType>(bp_bits(value, 2, 0));
        state.vtx_desc[AttrTex1] = static_cast<AttrType>(bp_bits(value, 2, 2));
        state.vtx_desc[AttrTex2] = static_cast<AttrType>(bp_bits(value, 2, 4));
        state.vtx_desc[AttrTex3] = static_cast<AttrType>(bp_bits(value, 2, 6));
        state.vtx_desc[AttrTex4] = static_cast<AttrType>(bp_bits(value, 2, 8));
        state.vtx_desc[AttrTex5] = static_cast<AttrType>(bp_bits(value, 2, 10));
        state.vtx_desc[AttrTex6] = static_cast<AttrType>(bp_bits(value, 2, 12));
        state.vtx_desc[AttrTex7] = static_cast<AttrType>(bp_bits(value, 2, 14));
        return;
    }
    case 0x30:
        state.mat_idx_a = value;
        state.current_pn_mtx = static_cast<std::uint8_t>(bp_bits(value, 6, 0) / 3u);
        if (state.current_pn_mtx >= kMaxPnMtx) {
            state.current_pn_mtx = 0;
        }
        state.tex_mtx_id[0] = static_cast<std::uint8_t>(bp_bits(value, 6, 6));
        state.tex_mtx_id[1] = static_cast<std::uint8_t>(bp_bits(value, 6, 12));
        state.tex_mtx_id[2] = static_cast<std::uint8_t>(bp_bits(value, 6, 18));
        state.tex_mtx_id[3] = static_cast<std::uint8_t>(bp_bits(value, 6, 24));
        for (unsigned i = 0; i < 4; ++i) {
            state.texgens[i].mtx = state.tex_mtx_id[i];
        }
        return;
    case 0x40:
        state.mat_idx_b = value;
        state.tex_mtx_id[4] = static_cast<std::uint8_t>(bp_bits(value, 6, 0));
        state.tex_mtx_id[5] = static_cast<std::uint8_t>(bp_bits(value, 6, 6));
        state.tex_mtx_id[6] = static_cast<std::uint8_t>(bp_bits(value, 6, 12));
        state.tex_mtx_id[7] = static_cast<std::uint8_t>(bp_bits(value, 6, 18));
        for (unsigned i = 0; i < 4; ++i) {
            state.texgens[i + 4u].mtx = state.tex_mtx_id[i + 4u];
        }
        return;
    default:
        break;
    }

    if (addr >= 0x70 && addr <= 0x77) {
        auto& vf = state.vtx_fmts[addr - 0x70];
        vf.attrs[AttrPos].cnt = static_cast<CompCnt>(bp_bits(value, 1, 0));
        vf.attrs[AttrPos].type = static_cast<CompType>(bp_bits(value, 3, 1));
        vf.attrs[AttrPos].frac = static_cast<std::uint8_t>(bp_bits(value, 5, 4));
        vf.attrs[AttrNrm].type = static_cast<CompType>(bp_bits(value, 3, 10));
        if (bp_bits(value, 1, 31) != 0) {
            vf.attrs[AttrNrm].cnt = NrmNBT3;
        } else {
            vf.attrs[AttrNrm].cnt = bp_bits(value, 1, 9) != 0 ? NrmNBT : NrmXYZ;
        }
        // Normals have no programmable fraction: the hardware fixes it by type (Aurora
        // command_processor.cpp normal_frac_bits: U8 7, S8 6, U16 15, S16 14).
        switch (vf.attrs[AttrNrm].type) {
        case U8:
            vf.attrs[AttrNrm].frac = 7;
            break;
        case S8:
            vf.attrs[AttrNrm].frac = 6;
            break;
        case U16:
            vf.attrs[AttrNrm].frac = 15;
            break;
        case S16:
            vf.attrs[AttrNrm].frac = 14;
            break;
        default:
            vf.attrs[AttrNrm].frac = 0;
            break;
        }
        vf.attrs[AttrClr0].cnt = static_cast<CompCnt>(bp_bits(value, 1, 13));
        vf.attrs[AttrClr0].type = static_cast<CompType>(bp_bits(value, 3, 14));
        vf.attrs[AttrClr1].cnt = static_cast<CompCnt>(bp_bits(value, 1, 17));
        vf.attrs[AttrClr1].type = static_cast<CompType>(bp_bits(value, 3, 18));
        vf.attrs[AttrTex0].cnt = static_cast<CompCnt>(bp_bits(value, 1, 21));
        vf.attrs[AttrTex0].type = static_cast<CompType>(bp_bits(value, 3, 22));
        vf.attrs[AttrTex0].frac = static_cast<std::uint8_t>(bp_bits(value, 5, 25));
    } else if (addr >= 0x80 && addr <= 0x87) {
        auto& vf = state.vtx_fmts[addr - 0x80];
        vf.attrs[AttrTex1].cnt = static_cast<CompCnt>(bp_bits(value, 1, 0));
        vf.attrs[AttrTex1].type = static_cast<CompType>(bp_bits(value, 3, 1));
        vf.attrs[AttrTex1].frac = static_cast<std::uint8_t>(bp_bits(value, 5, 4));
        vf.attrs[AttrTex2].cnt = static_cast<CompCnt>(bp_bits(value, 1, 9));
        vf.attrs[AttrTex2].type = static_cast<CompType>(bp_bits(value, 3, 10));
        vf.attrs[AttrTex2].frac = static_cast<std::uint8_t>(bp_bits(value, 5, 13));
        vf.attrs[AttrTex3].cnt = static_cast<CompCnt>(bp_bits(value, 1, 18));
        vf.attrs[AttrTex3].type = static_cast<CompType>(bp_bits(value, 3, 19));
        vf.attrs[AttrTex3].frac = static_cast<std::uint8_t>(bp_bits(value, 5, 22));
        vf.attrs[AttrTex4].cnt = static_cast<CompCnt>(bp_bits(value, 1, 27));
        vf.attrs[AttrTex4].type = static_cast<CompType>(bp_bits(value, 3, 28));
    } else if (addr >= 0x90 && addr <= 0x97) {
        auto& vf = state.vtx_fmts[addr - 0x90];
        vf.attrs[AttrTex4].frac = static_cast<std::uint8_t>(bp_bits(value, 5, 0));
        vf.attrs[AttrTex5].cnt = static_cast<CompCnt>(bp_bits(value, 1, 5));
        vf.attrs[AttrTex5].type = static_cast<CompType>(bp_bits(value, 3, 6));
        vf.attrs[AttrTex5].frac = static_cast<std::uint8_t>(bp_bits(value, 5, 9));
        vf.attrs[AttrTex6].cnt = static_cast<CompCnt>(bp_bits(value, 1, 14));
        vf.attrs[AttrTex6].type = static_cast<CompType>(bp_bits(value, 3, 15));
        vf.attrs[AttrTex6].frac = static_cast<std::uint8_t>(bp_bits(value, 5, 18));
        vf.attrs[AttrTex7].cnt = static_cast<CompCnt>(bp_bits(value, 1, 23));
        vf.attrs[AttrTex7].type = static_cast<CompType>(bp_bits(value, 3, 24));
        vf.attrs[AttrTex7].frac = static_cast<std::uint8_t>(bp_bits(value, 5, 27));
    } else if (addr >= 0xA0u && addr <= 0xABu) {
        // GXSetArray: the physical base of a vertex attribute array (POS, NRM, CLR0, CLR1,
        // TEX0..7). Elements are fetched from guest memory as vertices reference them.
        AttrArray& arr = state.arrays[static_cast<unsigned>(AttrPos) + (addr - 0xA0u)];
        arr.guest_base = physical_to_effective(value & 0x3FFFFFFFu);
        arr.data = nullptr;
        arr.size = 0;
        arr.le = false;
    } else if (addr >= 0xACu && addr <= 0xAFu) {
        g_xf_array_base[addr - 0xACu] = physical_to_effective(value & 0x3FFFFFFFu);
    } else if (addr >= 0xBCu && addr <= 0xBFu) {
        g_xf_array_stride[addr - 0xBCu] = value & 0xFFu;
    } else if (addr >= kCpArrayStride && addr <= static_cast<std::uint8_t>(kCpArrayStride + 0x0F)) {
        const unsigned attr = static_cast<unsigned>(AttrPos) + (addr - kCpArrayStride);
        if (attr < AttrCount) {
            state.arrays[attr].stride = static_cast<std::uint8_t>(value);
        }
    }
}

// TX_SETMODE0. The min filter is in the hardware encoding {0 NEAR, 1 NEAR_MIP_NEAR,
// 2 NEAR_MIP_LIN, 4 LINEAR, 5 LIN_MIP_NEAR, 6 LIN_MIP_LIN} (Aurora gfx/texture.hpp:94), not
// the GXTexFilter enum; reading it as the enum turned GX_LINEAR (hw 4) into LIN_MIP_NEAR.
void decode_tex_mode0(TexObj& tex, std::uint32_t value) {
    static const std::uint8_t kHwToFilter[8] = {TexNear,   TexNearMipNear, TexNearMipLin, TexNear,
                                                TexLinear, TexLinMipNear,  TexLinMipLin,  TexNear};
    tex.wrap_s = static_cast<std::uint8_t>(bp_bits(value, 2, 0));
    tex.wrap_t = static_cast<std::uint8_t>(bp_bits(value, 2, 2));
    tex.mag_filter = bp_bits(value, 1, 4) != 0 ? TexLinear : TexNear;
    tex.min_filter = kHwToFilter[bp_bits(value, 3, 5)];
    tex.edge_lod = bp_bits(value, 1, 8) == 0;
    tex.lod_bias = static_cast<std::int8_t>(bp_bits(value, 8, 9));
    tex.max_aniso = static_cast<std::uint8_t>(bp_bits(value, 2, 19));
    tex.mips = tex.min_filter != TexNear && tex.min_filter != TexLinear;
}

void handle_bp(State& state, std::uint32_t packed) {
    const std::uint32_t reg = (packed >> 24) & 0xFFu;
    const std::uint32_t ss = state.bp[0xFE] ? state.bp[0xFE] : 0x00FFFFFFu;
    if (reg == 0xFE) {
        state.bp[reg] = packed & 0x00FFFFFFu;
        return;
    }
    const std::uint32_t merged = (state.bp[reg] & ~ss) | (packed & ss);
    const std::uint32_t value = (reg << 24) | (merged & 0x00FFFFFFu);
    state.bp[reg] = value;
    // The BP mask applies to exactly one write (Aurora command_processor.cpp:632-640). It
    // used to stick, so after one masked write every later BP write only changed the masked
    // bits for the rest of the session.
    state.bp[0xFE] = 0x00FFFFFFu;

    switch (reg) {
    case 0x00: {
        state.num_tex_gens = static_cast<std::uint8_t>(bp_bits(value, 4, 0));
        state.num_chans = static_cast<std::uint8_t>(bp_bits(value, 3, 4));
        state.num_tev_stages = static_cast<std::uint8_t>(bp_bits(value, 4, 10) + 1u);
        state.num_ind_stages = static_cast<std::uint8_t>(bp_bits(value, 3, 16));
        const std::uint32_t hw_cull = bp_bits(value, 2, 14);
        switch (hw_cull) {
        case CullFront:
            state.cull_mode = CullBack;
            break;
        case CullBack:
            state.cull_mode = CullFront;
            break;
        default:
            state.cull_mode = static_cast<CullMode>(hw_cull);
            break;
        }
        break;
    }
    case 0x20:
    case 0x21:
        decode_scissor(state);
        break;
    case 0x40:
        state.depth_test = bp_bits(value, 1, 0) != 0;
        state.depth_func = static_cast<Compare>(bp_bits(value, 3, 1));
        state.depth_write = bp_bits(value, 1, 4) != 0;
        break;
    case 0x41: {
        const bool blend_en = bp_bits(value, 1, 0) != 0;
        const bool logic_en = bp_bits(value, 1, 1) != 0;
        const bool subtract = bp_bits(value, 1, 11) != 0;
        state.color_update = bp_bits(value, 1, 3) != 0;
        state.alpha_update = bp_bits(value, 1, 4) != 0;
        state.blend_dst = static_cast<BlendFactor>(bp_bits(value, 3, 5));
        state.blend_src = static_cast<BlendFactor>(bp_bits(value, 3, 8));
        if (subtract) {
            state.blend_mode = BlendSubtract;
        } else if (blend_en) {
            state.blend_mode = BlendMix;
        } else if (logic_en) {
            state.blend_mode = BlendLogic;
        } else {
            state.blend_mode = BlendNone;
        }
        state.logic_op = static_cast<LogicOp>(bp_bits(value, 4, 12));
        break;
    }
    case 0x22:
        state.line_width = static_cast<std::uint8_t>(bp_bits(value, 8, 0));
        state.point_size = static_cast<std::uint8_t>(bp_bits(value, 8, 8));
        state.line_tex_offset = static_cast<std::uint8_t>(bp_bits(value, 3, 16));
        state.point_tex_offset = static_cast<std::uint8_t>(bp_bits(value, 3, 19));
        break;
    case 0x42:
        // PE CMODE1: constant destination alpha (GXSetDstAlpha).
        state.dst_alpha = static_cast<std::uint8_t>(bp_bits(value, 8, 0));
        state.dst_alpha_enable = bp_bits(value, 1, 8) != 0;
        break;
    case 0x43:
        // PE control: pixel format, z format, z-compare location (GXSetPixelFmt/ZCompLoc).
        state.pixel_fmt = static_cast<std::uint8_t>(bp_bits(value, 3, 0));
        state.z_fmt = static_cast<std::uint8_t>(bp_bits(value, 3, 3));
        state.zcomploc = bp_bits(value, 1, 6) != 0;
        break;
    case 0x51:
        state.clear_z = bp_bits(value, 24, 0);
        break;
    case 0x59:
        // Scissor box offset, in 2-pixel units biased by 342.
        state.scissor_off_x = static_cast<std::int32_t>(bp_bits(value, 10, 0) << 1) - 342;
        state.scissor_off_y = static_cast<std::int32_t>(bp_bits(value, 10, 10) << 1) - 342;
        break;
    case 0x64:
        // LOADTLUT0: source of the next TLUT load, physical address >> 5.
        g_tlut_src_phys = bp_bits(value, 24, 0) << 5;
        break;
    case 0x65: {
        // LOADTLUT1: TMEM offset (512-byte units) and line count (16 entries, 32 bytes each).
        // Writing it performs the copy.
        const std::uint32_t tmem = bp_bits(value, 10, 0);
        const std::uint32_t lines = bp_bits(value, 11, 10);
        const std::uint32_t bytes = lines * 32u;
        const std::uint32_t dst = tmem << 9;
        if (lines != 0 && dst + bytes <= sizeof(g_tmem) && g_direct_resolver != nullptr) {
            const std::uint8_t* src = g_direct_resolver(physical_to_effective(g_tlut_src_phys));
            if (src != nullptr) {
                std::memcpy(g_tmem + dst, src, bytes);
                TmemTlut& slot = state.tmem_tlut[tmem];
                slot.lines = static_cast<std::uint16_t>(lines);
                slot.version = ++state.tlut_load_seq;
            }
        }
        break;
    }
    case 0x66:
        // TEXINVALIDATE (GXInvalidateTexAll): any texture may have been rewritten.
        ++state.tex_epoch;
        break;
    case 0x25:
        state.ind[0].scale_s = static_cast<std::uint8_t>(bp_bits(value, 4, 0));
        state.ind[0].scale_t = static_cast<std::uint8_t>(bp_bits(value, 4, 4));
        state.ind[1].scale_s = static_cast<std::uint8_t>(bp_bits(value, 4, 8));
        state.ind[1].scale_t = static_cast<std::uint8_t>(bp_bits(value, 4, 12));
        break;
    case 0x26:
        state.ind[2].scale_s = static_cast<std::uint8_t>(bp_bits(value, 4, 0));
        state.ind[2].scale_t = static_cast<std::uint8_t>(bp_bits(value, 4, 4));
        state.ind[3].scale_s = static_cast<std::uint8_t>(bp_bits(value, 4, 8));
        state.ind[3].scale_t = static_cast<std::uint8_t>(bp_bits(value, 4, 12));
        break;
    case 0x27:
        for (unsigned i = 0; i < kMaxIndStages; ++i) {
            state.ind[i].texmap = static_cast<std::uint8_t>(bp_bits(value, 3, i * 6u));
            state.ind[i].texcoord = static_cast<std::uint8_t>(bp_bits(value, 3, i * 6u + 3u));
        }
        break;
    case 0x4F: {
        const std::uint32_t r = bp_bits(value, 8, 0);
        const std::uint32_t a = bp_bits(value, 8, 8);
        g_copy_clear_rgba = (g_copy_clear_rgba & 0x00FFFF00u) | r | (a << 24);
        break;
    }
    case 0x50: {
        const std::uint32_t b = bp_bits(value, 8, 0);
        const std::uint32_t g = bp_bits(value, 8, 8);
        g_copy_clear_rgba = (g_copy_clear_rgba & 0xFF0000FFu) | (g << 8) | (b << 16);
        break;
    }
    case 0x49:
        state.efb.x = static_cast<std::int32_t>(bp_bits(value, 10, 0));
        state.efb.y = static_cast<std::int32_t>(bp_bits(value, 10, 10));
        break;
    case 0x4A:
        state.efb.w = static_cast<std::int32_t>(bp_bits(value, 10, 0) + 1u);
        state.efb.h = static_cast<std::int32_t>(bp_bits(value, 10, 10) + 1u);
        break;
    case 0x52:
        state.efb.format = bp_bits(value, 4, 3);
        state.efb.half_scale = bp_bits(value, 1, 9) != 0;
        break;
    case 0x4B:
        state.efb.to_xfb = bp_bits(value, 1, 11) != 0;
        state.efb.pending = true;
        break;
    case 0xF3:
        state.alpha_ref0 = static_cast<std::uint8_t>(bp_bits(value, 8, 0));
        state.alpha_ref1 = static_cast<std::uint8_t>(bp_bits(value, 8, 8));
        state.alpha_comp0 = static_cast<Compare>(bp_bits(value, 3, 16));
        state.alpha_comp1 = static_cast<Compare>(bp_bits(value, 3, 19));
        state.alpha_op = static_cast<std::uint8_t>(bp_bits(value, 2, 22));
        break;
    case 0xEE:
        state.fog.raw0 = value;
        refresh_fog_ab(state);
        break;
    case 0xEF:
        state.fog.raw1 = value;
        refresh_fog_ab(state);
        break;
    case 0xF0:
        state.fog.raw2 = value;
        refresh_fog_ab(state);
        break;
    case 0xF1:
        state.fog.type = static_cast<std::uint8_t>(bp_bits(value, 3, 21) | (bp_bits(value, 1, 20) << 3));
        state.fog.c = decode_fog_partial(value);
        break;
    case 0xF2:
        state.fog.color = pack_rgba(static_cast<std::uint8_t>(bp_bits(value, 8, 16)),
                                    static_cast<std::uint8_t>(bp_bits(value, 8, 8)),
                                    static_cast<std::uint8_t>(bp_bits(value, 8, 0)), 255);
        break;
    default:
        break;
    }

    if (reg >= 0x30 && reg <= 0x3F) {
        // SU texcoord scale, S then T per texcoord. Bit 20 is the HLE's "manual scale" marker
        // (GXSetTexCoordScaleManually); without it the scale is the bound texture's size.
        const unsigned coord = static_cast<unsigned>(reg - 0x30u) / 2u;
        const bool manual = bp_bits(value, 1, 20) != 0;
        if ((reg & 1u) == 0) {
            state.su_size_s[coord] = static_cast<std::uint16_t>(bp_bits(value, 16, 0) + 1u);
            state.su_manual[coord] = manual;
        } else {
            state.su_size_t[coord] = static_cast<std::uint16_t>(bp_bits(value, 16, 0) + 1u);
        }
    }

    // Texture image registers, the path GXLoadTexObj takes on hardware: IMAGE0 (size and
    // format), IMAGE3 (physical address >> 5) and the per-map TLUT register.
    auto texmap_for = [](std::uint32_t r, std::uint32_t base_lo, std::uint32_t base_hi) -> int {
        if (r >= base_lo && r < base_lo + 4u) {
            return static_cast<int>(r - base_lo);
        }
        if (r >= base_hi && r < base_hi + 4u) {
            return static_cast<int>(r - base_hi) + 4;
        }
        return -1;
    };
    if (const int m = texmap_for(reg, 0x88u, 0xA8u); m >= 0) {
        TexObj& tex = state.texmaps[m];
        tex.width = bp_bits(value, 10, 0) + 1u;
        tex.height = bp_bits(value, 10, 10) + 1u;
        tex.format = bp_bits(value, 4, 20);
        tex.version = 0;
        tex.tlut = 0;
        if (tex.format != TexCI4 && tex.format != TexCI8 && tex.format != TexCI14X2) {
            tex.tlut_tmem = 0xFFFFu;
        }
    } else if (const int m3 = texmap_for(reg, 0x94u, 0xB4u); m3 >= 0) {
        TexObj& tex = state.texmaps[m3];
        tex.phys = bp_bits(value, 24, 0) << 5;
        tex.data = g_direct_resolver != nullptr && tex.phys != 0
                       ? g_direct_resolver(physical_to_effective(tex.phys))
                       : nullptr;
        tex.version = 0;
        tex.id = tex.phys;
    } else if (const int mt = texmap_for(reg, 0x98u, 0xB8u); mt >= 0) {
        TexObj& tex = state.texmaps[mt];
        tex.tlut_tmem = static_cast<std::uint16_t>(bp_bits(value, 10, 0));
        tex.tlut_fmt = static_cast<std::uint8_t>(bp_bits(value, 2, 10));
    }

    if (reg >= 0x10 && reg <= 0x1F) {
        const unsigned stage = static_cast<unsigned>(reg - 0x10u);
        if (stage < kTevStages) {
            auto& s = state.tev[stage];
            s.ind_stage = static_cast<std::uint8_t>(bp_bits(value, 2, 0));
            s.ind_fmt = static_cast<std::uint8_t>(bp_bits(value, 2, 2));
            s.ind_bias = static_cast<std::uint8_t>(bp_bits(value, 3, 4));
            s.ind_alpha_sel = static_cast<std::uint8_t>(bp_bits(value, 2, 7));
            s.ind_mtx = static_cast<std::uint8_t>(bp_bits(value, 4, 9));
            s.ind_wrap_s = static_cast<std::uint8_t>(bp_bits(value, 3, 13));
            s.ind_wrap_t = static_cast<std::uint8_t>(bp_bits(value, 3, 16));
            s.ind_utc_lod = bp_bits(value, 1, 19) != 0;
            s.ind_add_prev = bp_bits(value, 1, 20) != 0;
        }
    }

    if (reg >= 0x06 && reg <= 0x0E) {
        const unsigned idx = static_cast<unsigned>(reg - 0x06u) / 3u;
        const unsigned column = static_cast<unsigned>(reg - 0x06u) % 3u;
        if (idx < kMaxIndMtx) {
            auto sign11 = [](std::uint32_t bits) {
                auto v = static_cast<std::int32_t>(bits);
                if ((v & 0x400) != 0) {
                    v |= ~0x7FF;
                }
                return v;
            };
            const float c0 = static_cast<float>(sign11(bp_bits(value, 11, 0))) / 1024.0f;
            const float c1 = static_cast<float>(sign11(bp_bits(value, 11, 11))) / 1024.0f;
            state.ind_mtx[idx].m[column] = c0;
            state.ind_mtx[idx].m[column + 3u] = c1;
            const std::uint32_t scale_bits = bp_bits(value, 2, 22);
            const unsigned shift = column * 2u;
            std::uint8_t raw = state.ind_mtx[idx].raw_scale;
            if (column == 2) {
                raw = static_cast<std::uint8_t>((raw & ~(1u << shift)) | ((scale_bits & 1u) << shift));
            } else {
                raw = static_cast<std::uint8_t>((raw & ~(3u << shift)) | (scale_bits << shift));
            }
            state.ind_mtx[idx].raw_scale = raw;
            state.ind_mtx[idx].scale_exp = static_cast<std::int8_t>(raw) - 17;
        }
    }

    if (reg >= 0x80 && reg <= 0x83) {
        const unsigned texmap = static_cast<unsigned>(reg - 0x80u);
        auto& tex = state.texmaps[texmap];
        tex.wrap_s = static_cast<std::uint8_t>(bp_bits(value, 2, 0));
        tex.wrap_t = static_cast<std::uint8_t>(bp_bits(value, 2, 2));
        decode_tex_mode0(tex, value);
    } else if (reg >= 0x84 && reg <= 0x87) {
        const unsigned texmap = static_cast<unsigned>(reg - 0x84u);
        state.texmaps[texmap].min_lod = static_cast<std::uint8_t>(bp_bits(value, 8, 0));
        state.texmaps[texmap].max_lod = static_cast<std::uint8_t>(bp_bits(value, 8, 8));
    } else if (reg >= 0xA0 && reg <= 0xA3) {
        const unsigned texmap = static_cast<unsigned>(reg - 0xA0u) + 4u;
        auto& tex = state.texmaps[texmap];
        tex.wrap_s = static_cast<std::uint8_t>(bp_bits(value, 2, 0));
        tex.wrap_t = static_cast<std::uint8_t>(bp_bits(value, 2, 2));
        decode_tex_mode0(tex, value);
    } else if (reg >= 0xA4 && reg <= 0xA7) {
        const unsigned texmap = static_cast<unsigned>(reg - 0xA4u) + 4u;
        state.texmaps[texmap].min_lod = static_cast<std::uint8_t>(bp_bits(value, 8, 0));
        state.texmaps[texmap].max_lod = static_cast<std::uint8_t>(bp_bits(value, 8, 8));
    }

    if (reg >= 0x28 && reg <= 0x2F) {
        decode_tev_order(state, reg, value);
    }

    if (reg >= 0xC0 && reg <= 0xDE && (reg & 1u) == 0) {
        const std::uint32_t stage = (reg - 0xC0) / 2u;
        if (stage < kTevStages) {
            auto& s = state.tev[stage];
            s.color_d = static_cast<std::uint8_t>(bp_bits(value, 4, 0));
            s.color_c = static_cast<std::uint8_t>(bp_bits(value, 4, 4));
            s.color_b = static_cast<std::uint8_t>(bp_bits(value, 4, 8));
            s.color_a = static_cast<std::uint8_t>(bp_bits(value, 4, 12));
            apply_tev_color_op(s, value);
        }
    } else if (reg >= 0xC1 && reg <= 0xDF && (reg & 1u) == 1) {
        const std::uint32_t stage = (reg - 0xC1) / 2u;
        if (stage < kTevStages) {
            auto& s = state.tev[stage];
            s.alpha_d = static_cast<std::uint8_t>(bp_bits(value, 3, 4));
            s.alpha_c = static_cast<std::uint8_t>(bp_bits(value, 3, 7));
            s.alpha_b = static_cast<std::uint8_t>(bp_bits(value, 3, 10));
            s.alpha_a = static_cast<std::uint8_t>(bp_bits(value, 3, 13));
            s.ras_swap = static_cast<std::uint8_t>(bp_bits(value, 2, 0));
            s.tex_swap = static_cast<std::uint8_t>(bp_bits(value, 2, 2));
            apply_tev_alpha_op(s, value);
        }
    } else if (reg >= 0xE0 && reg <= 0xE7) {
        const unsigned idx = static_cast<unsigned>(reg - 0xE0u) / 2u;
        const bool kon = bp_bits(value, 1, 23) != 0;
        if (kon && idx >= 4) {
            return;
        }
        if (!kon && idx >= 4) {
            return;
        }
        if (kon) {
            // Konst registers are 8-bit.
            std::uint32_t& packed = state.konst[idx];
            std::uint8_t r = static_cast<std::uint8_t>(packed);
            std::uint8_t g = static_cast<std::uint8_t>(packed >> 8);
            std::uint8_t b = static_cast<std::uint8_t>(packed >> 16);
            std::uint8_t a = static_cast<std::uint8_t>(packed >> 24);
            if ((reg & 1u) == 0) {
                r = static_cast<std::uint8_t>(bp_bits(value, 8, 0));
                a = static_cast<std::uint8_t>(bp_bits(value, 8, 12));
            } else {
                b = static_cast<std::uint8_t>(bp_bits(value, 8, 0));
                g = static_cast<std::uint8_t>(bp_bits(value, 8, 12));
            }
            packed = pack_rgba(r, g, b, a);
        } else {
            // TEV colour registers are signed 11-bit (GXSetTevColorS10; Aurora
            // command_processor.cpp:1126-1151). tev_color keeps a 0..255-clamped copy.
            auto s11 = [](std::uint32_t bits) {
                std::int32_t v = static_cast<std::int32_t>(bits & 0x7FFu);
                if ((v & 0x400) != 0) {
                    v -= 0x800;
                }
                return static_cast<std::int16_t>(v);
            };
            std::int16_t* rg = state.tev_reg[idx];
            if ((reg & 1u) == 0) {
                rg[0] = s11(bp_bits(value, 11, 0));
                rg[3] = s11(bp_bits(value, 11, 12));
            } else {
                rg[2] = s11(bp_bits(value, 11, 0));
                rg[1] = s11(bp_bits(value, 11, 12));
            }
            auto c8 = [](std::int16_t v) {
                return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            };
            state.tev_color[idx] = pack_rgba(c8(rg[0]), c8(rg[1]), c8(rg[2]), c8(rg[3]));
        }
    } else if (reg >= 0xF6 && reg <= 0xFD) {
        const unsigned even = static_cast<unsigned>(reg - 0xF6u) * 2u;
        if (even + 1 < kTevStages) {
            state.tev[even].kcsel = static_cast<std::uint8_t>(bp_bits(value, 5, 4));
            state.tev[even].kasel = static_cast<std::uint8_t>(bp_bits(value, 5, 9));
            state.tev[even + 1].kcsel = static_cast<std::uint8_t>(bp_bits(value, 5, 14));
            state.tev[even + 1].kasel = static_cast<std::uint8_t>(bp_bits(value, 5, 19));
        }
        const unsigned table = static_cast<unsigned>(reg - 0xF6u) / 2u;
        if (table < 4) {
            if ((reg & 1u) == 0) {
                state.tev_swap[table][0] = static_cast<std::uint8_t>(bp_bits(value, 2, 0));
                state.tev_swap[table][1] = static_cast<std::uint8_t>(bp_bits(value, 2, 2));
            } else {
                state.tev_swap[table][2] = static_cast<std::uint8_t>(bp_bits(value, 2, 0));
                state.tev_swap[table][3] = static_cast<std::uint8_t>(bp_bits(value, 2, 2));
            }
        }
    }
}

std::uint32_t xf_color(std::uint32_t val) {
    return pack_rgba(static_cast<std::uint8_t>(val >> 24), static_cast<std::uint8_t>(val >> 16),
                     static_cast<std::uint8_t>(val >> 8), static_cast<std::uint8_t>(val));
}

void decode_texgen(TexGen& gen, std::uint32_t val) {
    const bool proj = bp_bits(val, 1, 1) != 0;
    const std::uint32_t form = bp_bits(val, 1, 2);
    const std::uint32_t tg_type = bp_bits(val, 3, 4);
    const std::uint32_t src_row = bp_bits(val, 5, 7);
    gen.form_ab11 = form == 0;
    static const TexGenSrc kRowToSrc[] = {TgPos,  TgNrm,  TgColor0, TgBinrm, TgTangent, TgTex0, TgTex1,
                                          TgTex2, TgTex3, TgTex4,   TgTex5,  TgTex6,    TgTex7};
    if (src_row < 13) {
        gen.src = kRowToSrc[src_row];
    }
    if (tg_type == 0) {
        gen.type = proj ? TgMtx3x4 : TgMtx2x4;
    } else if (tg_type == 2 || tg_type == 3) {
        // SRTG: the source is a colour channel - 2 is COLOR0, 3 is COLOR1.
        gen.type = TgSrtg;
        gen.src = tg_type == 2 ? TgColor0 : TgColor1;
    } else {
        // Emboss/bump: not modelled (Aurora records it and has no shader path either); the
        // source coordinate passes through unchanged.
        gen.type = TgMtx2x4;
        gen.src = static_cast<TexGenSrc>(TgTex0 + (bp_bits(val, 3, 12) & 7u));
        gen.mtx = kTexMtxIdentity;
    }
}

void decode_chan_ctrl(ChanCtrl& chan, std::uint32_t val) {
    chan.mat_src = static_cast<ColorSrc>(bp_bits(val, 1, 0));
    chan.lighting = bp_bits(val, 1, 1) != 0;
    const std::uint32_t lights_lo = bp_bits(val, 4, 2);
    chan.amb_src = static_cast<ColorSrc>(bp_bits(val, 1, 6));
    chan.diff_fn = static_cast<DiffuseFn>(bp_bits(val, 2, 7));
    const std::uint32_t lights_hi = bp_bits(val, 4, 11);
    switch (bp_bits(val, 2, 9)) {
    case 1:
        chan.attn_fn = AfSpec;
        break;
    case 3:
        chan.attn_fn = AfSpot;
        break;
    default:
        chan.attn_fn = AfNone;
        break;
    }
    chan.light_mask = static_cast<std::uint8_t>(lights_lo | (lights_hi << 4));
}

bool copy_xf_mem(State& state, std::uint32_t addr, const std::uint8_t* data, std::uint32_t count, bool be) {
    if (addr < kXfMemSize) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t slot = addr + i;
            if (slot >= kXfMemSize) {
                break;
            }
            const float value = read_f32(data + i * 4u, be);
            state.xf_mem[slot] = value;
            if (slot < kMaxPnMtx * 12u) {
                state.pn_mtx[slot / 12u][slot % 12u] = value;
            }
        }
        return true;
    }
    if (addr >= 0x500 && addr < 0x600) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t slot = (addr - 0x500u) + i;
            if (slot < kXfDttSize) {
                state.xf_dtt[slot] = read_f32(data + i * 4u, be);
            }
        }
        return true;
    }
    if (addr >= 0x400 && addr < 0x45A) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t slot = (addr - 0x400u) + i;
            const unsigned mtx = slot / 9u;
            const unsigned off = slot % 9u;
            if (mtx < kMaxPnMtx) {
                state.nrm_mtx[mtx][off] = read_f32(data + i * 4u, be);
            }
        }
        return true;
    }
    if (addr >= 0x600 && addr < 0x680) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t slot = (addr - 0x600u) + i;
            const unsigned light_idx = slot / 16u;
            const unsigned field = slot % 16u;
            if (light_idx >= kMaxLights) {
                break;
            }
            Light& light = state.lights[light_idx];
            const float value = read_f32(data + i * 4u, be);
            const std::uint32_t ival = read_u32(data + i * 4u, be);
            switch (field) {
            case 3:
                light.color = xf_color(ival);
                break;
            case 4:
                light.cos_att[0] = value;
                break;
            case 5:
                light.cos_att[1] = value;
                break;
            case 6:
                light.cos_att[2] = value;
                break;
            case 7:
                light.dist_att[0] = value;
                break;
            case 8:
                light.dist_att[1] = value;
                break;
            case 9:
                light.dist_att[2] = value;
                break;
            case 10:
                light.pos[0] = value;
                break;
            case 11:
                light.pos[1] = value;
                break;
            case 12:
                light.pos[2] = value;
                break;
            case 13:
                light.dir[0] = value;
                break;
            case 14:
                light.dir[1] = value;
                break;
            case 15:
                light.dir[2] = value;
                break;
            default:
                break;
            }
        }
        return true;
    }
    return false;
}

bool handle_xf(State& state, const std::uint8_t* data, std::uint32_t& pos, std::uint32_t size, bool be) {
    if (!remain(pos, size, 4)) {
        return false;
    }
    const std::uint32_t header = read_u32(data + pos, be);
    pos += 4;
    const std::uint32_t count = ((header >> 16) & 0xFFFFu) + 1u;
    const std::uint32_t addr = header & 0xFFFFu;
    const std::uint32_t bytes = count * 4u;
    if (!remain(pos, size, bytes)) {
        pos = size;
        return false;
    }
    const std::uint8_t* xf = data + pos;

    const bool accepted = copy_xf_mem(state, addr, xf, count, be);
    note_xf_load(addr, count, accepted);
    if (accepted) {
        pos += bytes;
        return true;
    }

    if (addr >= 0x1000) {
        bool proj = false;
        const std::uint32_t xf_addr = addr - 0x1000;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t reg = xf_addr + i;
            const std::uint32_t val = read_u32(xf + i * 4u, be);
            switch (reg) {
            case 0x09:
                state.num_chans = static_cast<std::uint8_t>(val);
                break;
            case 0x0A:
                state.ambient[0] = xf_color(val);
                break;
            case 0x0B:
                state.ambient[1] = xf_color(val);
                break;
            case 0x0C:
                state.chan_mat[0] = xf_color(val);
                state.material = state.chan_mat[0];
                break;
            case 0x0D:
                state.chan_mat[1] = xf_color(val);
                break;
            case 0x0E:
            case 0x0F:
            case 0x10:
            case 0x11:
                decode_chan_ctrl(state.chans[reg - 0x0E], val);
                break;
            case 0x12:
                state.dual_tex = (val & 1u) != 0;
                break;
            case 0x18:
                // XF MATINDEX_A mirrors CP 0x30: the position matrix and texcoords 0-3.
                state.current_pn_mtx = static_cast<std::uint8_t>(bp_bits(val, 6, 0) / 3u);
                if (state.current_pn_mtx >= kMaxPnMtx) {
                    state.current_pn_mtx = 0;
                }
                for (unsigned t = 0; t < 4u; ++t) {
                    state.tex_mtx_id[t] = static_cast<std::uint8_t>(bp_bits(val, 6, 6u + 6u * t));
                    state.texgens[t].mtx = state.tex_mtx_id[t];
                }
                break;
            case 0x19:
                for (unsigned t = 0; t < 4u; ++t) {
                    state.tex_mtx_id[4u + t] = static_cast<std::uint8_t>(bp_bits(val, 6, 6u * t));
                    state.texgens[4u + t].mtx = state.tex_mtx_id[4u + t];
                }
                break;
            case 0x1A:
            case 0x1B:
            case 0x1C:
            case 0x1D:
            case 0x1E:
            case 0x1F:
                state.viewport[reg - 0x1A] = read_f32(xf + i * 4u, be);
                break;
            case 0x20:
            case 0x21:
            case 0x22:
            case 0x23:
            case 0x24:
            case 0x25:
                state.xf_proj_raw[reg - 0x20] = read_f32(xf + i * 4u, be);
                proj = true;
                break;
            case 0x26:
                state.proj_type = static_cast<ProjectionType>(val);
                proj = true;
                break;
            case 0x3F:
                state.num_tex_gens = static_cast<std::uint8_t>(val);
                break;
            default:
                if (reg >= 0x40 && reg <= 0x47) {
                    decode_texgen(state.texgens[reg - 0x40], val);
                } else if (reg >= 0x50 && reg <= 0x57) {
                    TexGen& gen = state.texgens[reg - 0x50];
                    gen.post_mtx = static_cast<std::uint8_t>(bp_bits(val, 6, 0) + 64u);
                    gen.normalize = bp_bits(val, 1, 8) != 0;
                }
                break;
            }
        }
        if (proj) {
            state.apply_projection();
        }
    }

    pos += bytes;
    return true;
}

bool handle_aurora(State& state, const std::uint8_t* data, std::uint32_t& pos, std::uint32_t size, bool be,
                   std::uint32_t* skipped) {
    if (!remain(pos, size, 2)) {
        return false;
    }
    const std::uint16_t sub = read_u16(data + pos, be);
    pos += 2;
    if (sub >= kAuroraArrayBase && sub <= static_cast<std::uint16_t>(kAuroraArrayBase + 0x0F)) {
        if (!remain(pos, size, 13)) {
            return false;
        }
        const unsigned attr = static_cast<unsigned>(AttrPos) + (sub - kAuroraArrayBase);
        const std::uint64_t addr = read_u64(data + pos, be);
        pos += 8;
        const std::uint32_t bytes = read_u32(data + pos, be);
        pos += 4;
        const bool le = data[pos++] == 1;
        if (attr < AttrCount) {
            state.arrays[attr].data = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(addr));
            state.arrays[attr].size = bytes;
            state.arrays[attr].le = le;
        }
        return true;
    }
    if (sub == kAuroraXfArray) {
        if (!remain(pos, size, 14)) {
            return false;
        }
        const std::uint8_t kind = data[pos++];
        const std::uint64_t addr = read_u64(data + pos, be);
        pos += 8;
        const std::uint32_t bytes = read_u32(data + pos, be);
        pos += 4;
        const std::uint8_t stride = data[pos++];
        if (kind < kMaxXfArrays) {
            state.xf_arrays[kind].data = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(addr));
            state.xf_arrays[kind].size = bytes;
            state.xf_arrays[kind].stride = stride;
            state.xf_arrays[kind].le = true;
        }
        return true;
    }
    if (sub == kAuroraTexObj) {
        if (!remain(pos, size, 34)) {
            return false;
        }
        const std::uint8_t texmap = data[pos++];
        const std::uint64_t addr = read_u64(data + pos, be);
        pos += 8;
        const std::uint32_t width = read_u32(data + pos, be);
        pos += 4;
        const std::uint32_t height = read_u32(data + pos, be);
        pos += 4;
        const std::uint32_t format = read_u32(data + pos, be);
        pos += 4;
        const std::uint32_t tlut = read_u32(data + pos, be);
        pos += 4;
        const bool mips = data[pos++] != 0;
        const std::uint32_t id = read_u32(data + pos, be);
        pos += 4;
        const std::uint32_t version = read_u32(data + pos, be);
        pos += 4;
        if (texmap < kMaxTexMaps) {
            TexObj& slot = state.texmaps[texmap];
            slot.data = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(addr));
            slot.width = width;
            slot.height = height;
            slot.format = format;
            slot.tlut = tlut;
            slot.id = id;
            slot.version = version;
            slot.mips = mips;
        }
        return true;
    }
    if (sub == kAuroraDestroyTexObj) {
        if (!remain(pos, size, 4)) {
            return false;
        }
        const std::uint32_t id = read_u32(data + pos, be);
        pos += 4;
        for (auto& slot : state.texmaps) {
            if (slot.id == id) {
                const std::uint8_t wrap_s = slot.wrap_s;
                const std::uint8_t wrap_t = slot.wrap_t;
                slot = {};
                slot.wrap_s = wrap_s;
                slot.wrap_t = wrap_t;
            }
        }
        return true;
    }
    if (sub == kAuroraTlut) {
        if (!remain(pos, size, 23)) {
            return false;
        }
        const std::uint8_t idx = data[pos++];
        const std::uint64_t addr = read_u64(data + pos, be);
        pos += 8;
        const std::uint32_t format = read_u32(data + pos, be);
        pos += 4;
        const std::uint16_t entries = read_u16(data + pos, be);
        pos += 2;
        const std::uint32_t id = read_u32(data + pos, be);
        pos += 4;
        const std::uint32_t version = read_u32(data + pos, be);
        pos += 4;
        if (idx < kMaxTluts) {
            TlutObj& slot = state.tluts[idx];
            slot.data = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(addr));
            slot.format = format;
            slot.entries = entries;
            slot.id = id;
            slot.version = version;
        }
        return true;
    }
    if (sub == kAuroraDestroyTlut) {
        if (!remain(pos, size, 4)) {
            return false;
        }
        const std::uint32_t id = read_u32(data + pos, be);
        pos += 4;
        for (auto& slot : state.tluts) {
            if (slot.id == id) {
                slot = {};
            }
        }
        return true;
    }
    if (sub == kAuroraViewport) {
        if (!remain(pos, size, 24)) {
            return false;
        }
        for (int i = 0; i < 6; ++i) {
            state.render_vp[i] = read_f32(data + pos, be);
            pos += 4;
        }
        state.has_render_vp = true;
        return true;
    }
    if (sub == kAuroraScissor) {
        if (!remain(pos, size, 16)) {
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            state.render_scissor[i] = static_cast<std::int32_t>(read_u32(data + pos, be));
            pos += 4;
        }
        state.has_render_scissor = true;
        return true;
    }
    std::uint32_t skip = 0;
    if (sub == kAuroraDestroyCopyTex) {
        skip = 8;
    } else if (sub == kAuroraInvalidateTex || sub == kAuroraDebugPop) {
        skip = 0;
    } else if (sub == kAuroraDebugPush || sub == kAuroraDebugMarker) {
        if (!remain(pos, size, 2)) {
            return false;
        }
        skip = read_u16(data + pos, be);
        pos += 2;
    } else {
        return false;
    }
    if (!remain(pos, size, skip)) {
        return false;
    }
    pos += skip;
    if (skipped) {
        ++(*skipped);
    }
    return true;
}

} // namespace

namespace {
bool g_reset_keeps_shadows = false;
} // namespace

// Empties the command buffer but keeps the register shadows (see draw_state.hpp).
void fifo_reset_buffer_only(Fifo& fifo) {
    g_reset_keeps_shadows = true;
    fifo.reset();
    g_reset_keeps_shadows = false;
}

void Fifo::reset() {
    size_ = 0;
    overflow_ = false;
    if (g_reset_keeps_shadows) {
        return;
    }
    mat_idx_a_ = 0;
    mat_idx_b_ = 0;
    std::memset(tev_ksel_, 0, sizeof(tev_ksel_));
    std::memset(tev_tref_, 0, sizeof(tev_tref_));
    std::memset(tev_color_reg_, 0, sizeof(tev_color_reg_));
    std::memset(tev_alpha_reg_, 0, sizeof(tev_alpha_reg_));
    for (unsigned t = 0; t < 4; ++t) {
        const unsigned even = t * 2u;
        tev_ksel_[even] = 0u | (1u << 2);
        tev_ksel_[even + 1] = 2u | (3u << 2);
    }
}

bool Fifo::write_bytes(const void* data, std::uint32_t bytes) {
    if (!overflow_ && size_ + bytes > kFifoCapacity && g_fifo_full != nullptr) {
        // The hardware FIFO is a ring the GP drains while the CPU writes; a frame larger
        // than this buffer used to lose everything past the first 64 KiB. The handler
        // decodes the complete commands buffered so far and keeps the unfinished tail.
        static bool s_in_handler = false;
        if (!s_in_handler) {
            s_in_handler = true;
            const std::uint32_t consumed = g_fifo_full(*this, buffer_, size_);
            s_in_handler = false;
            if (consumed != 0 && consumed <= size_) {
                std::memmove(buffer_, buffer_ + consumed, size_ - consumed);
                size_ -= consumed;
            }
        }
    }
    if (overflow_ || size_ + bytes > kFifoCapacity) {
        overflow_ = true;
        return false;
    }
    if (bytes != 0 && data != nullptr) {
        std::memcpy(buffer_ + size_, data, bytes);
    }
    size_ += bytes;
    return true;
}

bool Fifo::write_u64(std::uint64_t value) {
    const std::uint8_t bytes[8] = {
        static_cast<std::uint8_t>(value >> 56), static_cast<std::uint8_t>(value >> 48),
        static_cast<std::uint8_t>(value >> 40), static_cast<std::uint8_t>(value >> 32),
        static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 8),  static_cast<std::uint8_t>(value),
    };
    return write_bytes(bytes, 8);
}

bool Fifo::nop() {
    return write_u8(kNop);
}

bool Fifo::load_cp(std::uint8_t addr, std::uint32_t value) {
    if (addr == 0x30) {
        mat_idx_a_ = value;
    } else if (addr == 0x40) {
        mat_idx_b_ = value;
    }
    return write_u8(kLoadCp) && write_u8(addr) && write_u32(value);
}

bool Fifo::load_bp(std::uint8_t reg, std::uint32_t value24) {
    return write_u8(kLoadBp) && write_u32((static_cast<std::uint32_t>(reg) << 24) | (value24 & 0x00FFFFFFu));
}

bool Fifo::load_xf(std::uint16_t addr, const std::uint32_t* words, std::uint32_t count) {
    if (count == 0 || words == nullptr) {
        return false;
    }
    bool ok = write_u8(kLoadXf) && write_u16(static_cast<std::uint16_t>(count - 1u)) && write_u16(addr);
    for (std::uint32_t i = 0; i < count && ok; ++i) {
        ok = write_u32(words[i]);
    }
    return ok;
}

bool Fifo::load_xf_f32(std::uint16_t addr, const float* values, std::uint32_t count) {
    if (count == 0 || values == nullptr) {
        return false;
    }
    bool ok = write_u8(kLoadXf) && write_u16(static_cast<std::uint16_t>(count - 1u)) && write_u16(addr);
    for (std::uint32_t i = 0; i < count && ok; ++i) {
        ok = write_f32(values[i]);
    }
    return ok;
}

bool Fifo::set_array(Attr attr, const void* data, std::uint32_t size, std::uint8_t stride, bool little_endian) {
    if (attr < AttrPos || attr > AttrTex7) {
        return false;
    }
    const std::uint16_t sub = static_cast<std::uint16_t>(kAuroraArrayBase + (attr - AttrPos));
    const auto addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return write_u8(kLoadAurora) && write_u16(sub) && write_u64(addr) && write_u32(size) &&
           write_u8(little_endian ? 1 : 0) &&
           load_cp(static_cast<std::uint8_t>(kCpArrayStride + (attr - AttrPos)), stride);
}

bool Fifo::load_texobj(unsigned texmap, const void* data, std::uint32_t width, std::uint32_t height, std::uint32_t format,
                       unsigned wrap_s, unsigned wrap_t, bool mips, std::uint32_t id, std::uint32_t version,
                       std::uint32_t tlut) {
    if (texmap >= kMaxTexMaps) {
        return false;
    }
    // Hardware min-filter encoding: 4 = LINEAR, 6 = LIN_MIP_LIN.
    const std::uint32_t mode0 = (wrap_s & 3u) | ((wrap_t & 3u) << 2) | (1u << 4) | ((mips ? 6u : 4u) << 5);
    const auto addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    const std::uint8_t mode0_reg = static_cast<std::uint8_t>(texmap < 4u ? 0x80u + texmap : 0xA0u + texmap - 4u);
    return load_bp(mode0_reg, mode0) && write_u8(kLoadAurora) &&
           write_u16(kAuroraTexObj) && write_u8(static_cast<std::uint8_t>(texmap)) && write_u64(addr) &&
           write_u32(width) && write_u32(height) && write_u32(format) && write_u32(tlut) && write_u8(mips ? 1 : 0) &&
           write_u32(id) && write_u32(version);
}

bool Fifo::load_tlut(unsigned idx, const void* data, std::uint32_t format, std::uint16_t entries, std::uint32_t id,
                     std::uint32_t version) {
    if (idx >= kMaxTluts) {
        return false;
    }
    const auto addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return write_u8(kLoadAurora) && write_u16(kAuroraTlut) && write_u8(static_cast<std::uint8_t>(idx)) &&
           write_u64(addr) && write_u32(format) && write_u16(entries) && write_u32(id) && write_u32(version);
}

bool Fifo::set_tev_op(unsigned stage, TevOp mode) {
    if (stage >= kTevStages || mode >= TevOther) {
        return false;
    }
    std::uint8_t color_a = CcZero;
    std::uint8_t color_b = CcZero;
    std::uint8_t color_c = CcZero;
    std::uint8_t color_d = CcZero;
    std::uint8_t alpha_a = CaZero;
    std::uint8_t alpha_b = CaZero;
    std::uint8_t alpha_c = CaZero;
    std::uint8_t alpha_d = CaZero;
    // GXSetTevOp (Aurora GXTev.cpp:5-36): stage 0 combines with the rasterised colour, every
    // later stage with the previous stage's output.
    const std::uint8_t carg = stage == 0 ? CcRasC : CcCPrev;
    const std::uint8_t aarg = stage == 0 ? CaRasA : CaAPrev;
    switch (mode) {
    case TevModulate:
        color_b = CcTexC;
        color_c = carg;
        alpha_b = CaTexA;
        alpha_c = aarg;
        break;
    case TevDecal:
        color_a = carg;
        color_b = CcTexC;
        color_c = CcTexA;
        alpha_d = aarg;
        break;
    case TevBlend:
        color_a = carg;
        color_b = CcOne;
        color_c = CcTexC;
        alpha_b = CaTexA;
        alpha_c = aarg;
        break;
    case TevReplace:
        color_d = CcTexC;
        alpha_d = CaTexA;
        break;
    case TevPassClr:
        color_d = carg;
        alpha_d = aarg;
        break;
    default:
        return false;
    }
    const std::uint32_t color = (color_d & 0xFu) | ((color_c & 0xFu) << 4) | ((color_b & 0xFu) << 8) |
                                ((color_a & 0xFu) << 12) | (1u << 19);
    std::uint32_t alpha = ((alpha_d & 0x7u) << 4) | ((alpha_c & 0x7u) << 7) | ((alpha_b & 0x7u) << 10) |
                          ((alpha_a & 0x7u) << 13) | (1u << 19);
    tev_color_reg_[stage] = color;
    alpha |= tev_alpha_reg_[stage] & 0xFu;  // keep the swap selectors
    tev_alpha_reg_[stage] = alpha;
    return load_bp(static_cast<std::uint8_t>(0xC0u + stage * 2u), color) &&
           load_bp(static_cast<std::uint8_t>(0xC1u + stage * 2u), alpha);
}

// BP 0xC0 + stage*2: color_d at 0, color_c at 4, color_b at 8, color_a at 12, then
// bias 16-17, op 18, clamp 19, shift 20-21, dest 22-23 - the same layout the decoder
// reads back, so only the low 16 bits belong to the arguments.
bool Fifo::set_ind_tex_coord_scale(unsigned stage, std::uint8_t scale_s, std::uint8_t scale_t) {
    if (stage >= kMaxIndStages) {
        return false;
    }
    const std::uint8_t reg = (stage < 2u) ? 0x25u : 0x26u;
    const unsigned shift = (stage & 1u) ? 8u : 0u;
    std::uint32_t value = ind_scale_reg_[stage < 2u ? 0u : 1u];
    value &= ~(0xFFu << shift);
    value |= (static_cast<std::uint32_t>(scale_s) & 0xFu) << shift;
    value |= (static_cast<std::uint32_t>(scale_t) & 0xFu) << (shift + 4u);
    ind_scale_reg_[stage < 2u ? 0u : 1u] = value;
    return load_bp(reg, value);
}

bool Fifo::set_tev_color_in(unsigned stage, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    if (stage >= kTevStages) {
        return false;
    }
    std::uint32_t color = tev_color_reg_[stage] & ~0xFFFFu;
    color |= (static_cast<std::uint32_t>(d) & 0xFu);
    color |= (static_cast<std::uint32_t>(c) & 0xFu) << 4;
    color |= (static_cast<std::uint32_t>(b) & 0xFu) << 8;
    color |= (static_cast<std::uint32_t>(a) & 0xFu) << 12;
    tev_color_reg_[stage] = color;
    return load_bp(static_cast<std::uint8_t>(0xC0u + stage * 2u), color);
}

// BP 0xC1 + stage*2: ras_swap 0-1, tex_swap 2-3, then alpha_d at 4, alpha_c at 7,
// alpha_b at 10, alpha_a at 13 (three bits each), and the same op fields from 16 up.
// The swap selectors live in this register too, so they are preserved here and written
// by set_tev_swap_mode.
bool Fifo::set_tev_alpha_in(unsigned stage, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    if (stage >= kTevStages) {
        return false;
    }
    std::uint32_t alpha = tev_alpha_reg_[stage] & ~0xFFF0u;
    alpha |= (static_cast<std::uint32_t>(d) & 0x7u) << 4;
    alpha |= (static_cast<std::uint32_t>(c) & 0x7u) << 7;
    alpha |= (static_cast<std::uint32_t>(b) & 0x7u) << 10;
    alpha |= (static_cast<std::uint32_t>(a) & 0x7u) << 13;
    tev_alpha_reg_[stage] = alpha;
    return load_bp(static_cast<std::uint8_t>(0xC1u + stage * 2u), alpha);
}

bool Fifo::set_tev_alpha_op(unsigned stage, std::uint8_t op, std::uint8_t bias, std::uint8_t shift, bool clamp,
                            std::uint8_t dest) {
    if (stage >= kTevStages) {
        return false;
    }
    std::uint32_t alpha = tev_alpha_reg_[stage] & 0xFFFFu;
    if (clamp) {
        alpha |= 1u << 19;
    }
    alpha |= (static_cast<std::uint32_t>(dest) & 3u) << 22;
    if (op >= TevCompR8Gt) {
        const std::uint32_t hw = static_cast<std::uint32_t>(op - 8u);
        alpha |= 3u << 16;
        alpha |= (hw & 1u) << 18;
        alpha |= ((hw >> 1) & 3u) << 20;
    } else {
        alpha |= (static_cast<std::uint32_t>(bias) & 3u) << 16;
        alpha |= (static_cast<std::uint32_t>(op) & 1u) << 18;
        alpha |= (static_cast<std::uint32_t>(shift) & 3u) << 20;
    }
    tev_alpha_reg_[stage] = alpha;
    return load_bp(static_cast<std::uint8_t>(0xC1u + stage * 2u), alpha);
}

bool Fifo::set_tev_swap_mode(unsigned stage, std::uint8_t ras_sel, std::uint8_t tex_sel) {
    if (stage >= kTevStages) {
        return false;
    }
    std::uint32_t alpha = tev_alpha_reg_[stage] & ~0xFu;
    alpha |= static_cast<std::uint32_t>(ras_sel) & 3u;
    alpha |= (static_cast<std::uint32_t>(tex_sel) & 3u) << 2;
    tev_alpha_reg_[stage] = alpha;
    return load_bp(static_cast<std::uint8_t>(0xC1u + stage * 2u), alpha);
}

bool Fifo::set_gen_mode(std::uint8_t tex_gens, std::uint8_t chans, std::uint8_t tev_stages, std::uint8_t n_ind) {
    if (tev_stages == 0) {
        tev_stages = 1;
    }
    const std::uint32_t bp = static_cast<std::uint32_t>(tex_gens) | (static_cast<std::uint32_t>(chans) << 4) |
                             ((static_cast<std::uint32_t>(tev_stages) - 1u) << 10) |
                             ((static_cast<std::uint32_t>(n_ind) & 7u) << 16);
    const std::uint32_t xf_chans = chans;
    const std::uint32_t xf_tex = tex_gens;
    return load_bp(0x00, bp) && load_xf(0x1009, &xf_chans, 1) && load_xf(0x103F, &xf_tex, 1);
}

bool Fifo::set_tex_coord_gen(unsigned dst, TexGenType type, TexGenSrc src, std::uint8_t mtx, bool normalize,
                             std::uint8_t post_mtx) {
    if (dst >= kMaxTexGens) {
        return false;
    }
    std::uint32_t row = 5;
    std::uint32_t form = 0;
    switch (src) {
    case TgPos:
        row = 0;
        form = 1;
        break;
    case TgNrm:
        row = 1;
        form = 1;
        break;
    case TgColor0:
        row = 2;
        break;
    case TgBinrm:
        row = 3;
        form = 1;
        break;
    case TgTangent:
        row = 4;
        form = 1;
        break;
    case TgTex0:
        row = 5;
        break;
    case TgTex1:
        row = 6;
        break;
    case TgTex2:
        row = 7;
        break;
    case TgTex3:
        row = 8;
        break;
    case TgTex4:
        row = 9;
        break;
    case TgTex5:
        row = 10;
        break;
    case TgTex6:
        row = 11;
        break;
    case TgTex7:
        row = 12;
        break;
    case TgColor1:
        row = 2;
        break;
    }
    std::uint32_t reg = 0;
    if (type == TgMtx3x4) {
        reg |= 1u << 1;
    }
    if (type == TgSrtg) {
        reg |= ((src == TgColor1) ? 3u : 2u) << 4;
        row = 2;
    }
    reg |= (form & 1u) << 2;
    reg |= (row & 31u) << 7;
    std::uint32_t post = static_cast<std::uint32_t>(post_mtx >= kPtTexMtx0 ? (post_mtx - kPtTexMtx0) : post_mtx) & 0x3Fu;
    if (normalize) {
        post |= 1u << 8;
    }
    if (dst < 4) {
        const unsigned shift = 6u + dst * 6u;
        mat_idx_a_ = (mat_idx_a_ & ~(0x3Fu << shift)) | ((static_cast<std::uint32_t>(mtx) & 0x3Fu) << shift);
    } else {
        const unsigned shift = (dst - 4u) * 6u;
        mat_idx_b_ = (mat_idx_b_ & ~(0x3Fu << shift)) | ((static_cast<std::uint32_t>(mtx) & 0x3Fu) << shift);
    }
    const std::uint32_t xf_addr = static_cast<std::uint16_t>(0x1040u + dst);
    const std::uint32_t post_addr = static_cast<std::uint16_t>(0x1050u + dst);
    const bool mtx_ok =
        (dst < 4) ? load_cp(0x30, mat_idx_a_) : load_cp(0x40, mat_idx_b_);
    return load_xf(static_cast<std::uint16_t>(xf_addr), &reg, 1) &&
           load_xf(static_cast<std::uint16_t>(post_addr), &post, 1) && mtx_ok;
}

bool Fifo::load_tex_mtx(std::uint8_t id, const float* values, unsigned count) {
    if (values == nullptr || (count != 8 && count != 12)) {
        return false;
    }
    const std::uint16_t addr = (id >= kPtTexMtx0)
                                   ? static_cast<std::uint16_t>((id - kPtTexMtx0) * 4u + 0x500u)
                                   : static_cast<std::uint16_t>(id) * 4u;
    return load_xf_f32(addr, values, count);
}

bool Fifo::set_chan_ctrl(unsigned chan, bool lighting, ColorSrc amb_src, ColorSrc mat_src, std::uint8_t light_mask,
                         DiffuseFn diff_fn, AttnFn attn_fn) {
    if (chan > 3) {
        return false;
    }
    std::uint32_t reg = static_cast<std::uint32_t>(mat_src) & 1u;
    if (lighting) {
        reg |= 1u << 1;
    }
    reg |= (static_cast<std::uint32_t>(light_mask) & 0xFu) << 2;
    reg |= (static_cast<std::uint32_t>(amb_src) & 1u) << 6;
    const DiffuseFn packed_diff = (attn_fn == AfSpec) ? DfNone : diff_fn;
    reg |= (static_cast<std::uint32_t>(packed_diff) & 3u) << 7;
    if (attn_fn != AfNone) {
        reg |= 1u << 9;
    }
    if (attn_fn != AfSpec) {
        reg |= 1u << 10;
    }
    reg |= (static_cast<std::uint32_t>(light_mask >> 4) & 0xFu) << 11;
    const std::uint32_t addr = 0x100Eu + chan;
    return load_xf(static_cast<std::uint16_t>(addr), &reg, 1);
}

bool Fifo::set_chan_amb_color(unsigned chan, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    if (chan > 1) {
        return false;
    }
    const std::uint32_t packed = (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) |
                                 (static_cast<std::uint32_t>(b) << 8) | a;
    return load_xf(static_cast<std::uint16_t>(0x100Au + chan), &packed, 1);
}

bool Fifo::set_chan_mat_color(unsigned chan, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    if (chan > 1) {
        return false;
    }
    const std::uint32_t packed = (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) |
                                 (static_cast<std::uint32_t>(b) << 8) | a;
    return load_xf(static_cast<std::uint16_t>(0x100Cu + chan), &packed, 1);
}

bool Fifo::load_light(unsigned id, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a, float px, float py,
                      float pz, float nx, float ny, float nz) {
    if (id >= kMaxLights) {
        return false;
    }
    std::uint32_t words[16]{};
    words[3] = (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) |
               (static_cast<std::uint32_t>(b) << 8) | a;
    auto fbits = [](float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };
    words[4] = fbits(1.0f);
    words[7] = fbits(1.0f);
    words[10] = fbits(px);
    words[11] = fbits(py);
    words[12] = fbits(pz);
    words[13] = fbits(-nx);
    words[14] = fbits(-ny);
    words[15] = fbits(-nz);
    return load_xf(static_cast<std::uint16_t>(0x600u + id * 16u), words, 16);
}

bool Fifo::set_blend_mode(BlendMode mode, BlendFactor src, BlendFactor dst, LogicOp logic) {
    std::uint32_t cmode0 = (1u << 3) | (1u << 4);
    if (mode == BlendMix || mode == BlendSubtract) {
        cmode0 |= 1u;
    }
    if (mode == BlendLogic) {
        cmode0 |= 2u;
    }
    if (mode == BlendSubtract) {
        cmode0 |= (1u << 11);
    }
    cmode0 |= (static_cast<std::uint32_t>(dst) & 7u) << 5;
    cmode0 |= (static_cast<std::uint32_t>(src) & 7u) << 8;
    cmode0 |= (static_cast<std::uint32_t>(logic) & 15u) << 12;
    return load_bp(0x41, cmode0);
}

bool Fifo::set_alpha_compare(Compare comp0, std::uint8_t ref0, std::uint8_t op, Compare comp1, std::uint8_t ref1) {
    const std::uint32_t packed = static_cast<std::uint32_t>(ref0) | (static_cast<std::uint32_t>(ref1) << 8) |
                                 ((static_cast<std::uint32_t>(comp0) & 7u) << 16) |
                                 ((static_cast<std::uint32_t>(comp1) & 7u) << 19) | ((static_cast<std::uint32_t>(op) & 3u) << 22);
    return load_bp(0xF3, packed);
}

bool Fifo::set_tev_color(unsigned id, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    if (id > 3) {
        return false;
    }
    // Same BP pair as the konst registers, minus the type field that marks them konst.
    const std::uint32_t ra = static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(a) << 12);
    const std::uint32_t bg = static_cast<std::uint32_t>(b) | (static_cast<std::uint32_t>(g) << 12);
    return load_bp(static_cast<std::uint8_t>(0xE0u + id * 2u), ra) &&
           load_bp(static_cast<std::uint8_t>(0xE1u + id * 2u), bg);
}

bool Fifo::set_tev_kcolor(unsigned id, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    if (id > 3) {
        return false;
    }
    const std::uint32_t ra = static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(a) << 12) | (8u << 20);
    const std::uint32_t bg = static_cast<std::uint32_t>(b) | (static_cast<std::uint32_t>(g) << 12) | (8u << 20);
    return load_bp(static_cast<std::uint8_t>(0xE0u + id * 2u), ra) &&
           load_bp(static_cast<std::uint8_t>(0xE1u + id * 2u), bg);
}

bool Fifo::set_tev_kcolor_sel(unsigned stage, std::uint8_t sel) {
    if (stage >= kTevStages) {
        return false;
    }
    const unsigned idx = stage / 2u;
    const unsigned shift = (stage & 1u) != 0 ? 14u : 4u;
    tev_ksel_[idx] = (tev_ksel_[idx] & ~(0x1Fu << shift)) | ((static_cast<std::uint32_t>(sel) & 0x1Fu) << shift);
    return load_bp(static_cast<std::uint8_t>(0xF6u + idx), tev_ksel_[idx]);
}

bool Fifo::set_tev_kalpha_sel(unsigned stage, std::uint8_t sel) {
    if (stage >= kTevStages) {
        return false;
    }
    const unsigned idx = stage / 2u;
    const unsigned shift = (stage & 1u) != 0 ? 19u : 9u;
    tev_ksel_[idx] = (tev_ksel_[idx] & ~(0x1Fu << shift)) | ((static_cast<std::uint32_t>(sel) & 0x1Fu) << shift);
    return load_bp(static_cast<std::uint8_t>(0xF6u + idx), tev_ksel_[idx]);
}

bool Fifo::set_tev_swap_table(unsigned id, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    if (id > 3) {
        return false;
    }
    const unsigned even = id * 2u;
    tev_ksel_[even] = (tev_ksel_[even] & ~0xFu) | (static_cast<std::uint32_t>(r) & 3u) |
                      ((static_cast<std::uint32_t>(g) & 3u) << 2);
    tev_ksel_[even + 1] = (tev_ksel_[even + 1] & ~0xFu) | (static_cast<std::uint32_t>(b) & 3u) |
                          ((static_cast<std::uint32_t>(a) & 3u) << 2);
    return load_bp(static_cast<std::uint8_t>(0xF6u + even), tev_ksel_[even]) &&
           load_bp(static_cast<std::uint8_t>(0xF6u + even + 1u), tev_ksel_[even + 1]);
}

bool Fifo::set_xf_array(unsigned kind, const void* data, std::uint32_t size, std::uint8_t stride) {
    if (kind >= kMaxXfArrays || data == nullptr || size == 0 || stride == 0) {
        return false;
    }
    const auto addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return write_u8(kLoadAurora) && write_u16(kAuroraXfArray) && write_u8(static_cast<std::uint8_t>(kind)) &&
           write_u64(addr) && write_u32(size) && write_u8(stride);
}

bool Fifo::load_indx(std::uint8_t opcode, std::uint16_t index, std::uint8_t count, std::uint16_t dst) {
    if (opcode != kLoadIndxA && opcode != kLoadIndxB && opcode != kLoadIndxC && opcode != kLoadIndxD) {
        return false;
    }
    const std::uint32_t words = count == 0 ? 1u : static_cast<std::uint32_t>(count);
    const std::uint32_t packed = (static_cast<std::uint32_t>(index) << 16) | (((words - 1u) & 0xFu) << 12) |
                                 (static_cast<std::uint32_t>(dst) & 0xFFFu);
    return write_u8(opcode) && write_u32(packed);
}

bool Fifo::call_dl(std::uint32_t addr, std::uint32_t bytes) {
    return write_u8(kCallDl) && write_u32(addr) && write_u32(bytes);
}

bool Fifo::set_line_width(std::uint8_t line_width, std::uint8_t point_size) {
    return load_bp(0x22, static_cast<std::uint32_t>(line_width) | (static_cast<std::uint32_t>(point_size) << 8));
}

bool Fifo::set_ind_order(unsigned stage, std::uint8_t texcoord, std::uint8_t texmap) {
    if (stage >= kMaxIndStages) {
        return false;
    }
    std::uint32_t packed = 0;
    // Caller should set all four; we write one stage into a reconstructed register from zeros for tests.
    packed |= (static_cast<std::uint32_t>(texmap) & 7u) << (stage * 6u);
    packed |= (static_cast<std::uint32_t>(texcoord) & 7u) << (stage * 6u + 3u);
    return load_bp(0x27, packed);
}

bool Fifo::set_ind_mtx(unsigned id, const float m[6], std::int8_t scale_exp) {
    if (id >= kMaxIndMtx || m == nullptr) {
        return false;
    }
    const int adj = static_cast<int>(scale_exp) + 17;
    auto pack_col = [adj](float a, float b, unsigned col) {
        auto q = [](float v) {
            int x = static_cast<int>(v * 1024.0f);
            if (x < -1024) {
                x = -1024;
            }
            if (x > 1023) {
                x = 1023;
            }
            return static_cast<std::uint32_t>(x) & 0x7FFu;
        };
        const std::uint32_t scale = static_cast<std::uint32_t>((adj >> (col * 2)) & 3);
        return q(a) | (q(b) << 11) | (scale << 22);
    };
    return load_bp(static_cast<std::uint8_t>(0x06u + id * 3u), pack_col(m[0], m[3], 0)) &&
           load_bp(static_cast<std::uint8_t>(0x07u + id * 3u), pack_col(m[1], m[4], 1)) &&
           load_bp(static_cast<std::uint8_t>(0x08u + id * 3u), pack_col(m[2], m[5], 2));
}

bool Fifo::set_tev_indirect(unsigned stage, std::uint8_t ind_stage, std::uint8_t fmt, std::uint8_t bias, std::uint8_t mtx,
                            std::uint8_t wrap_s, std::uint8_t wrap_t, bool add_prev) {
    if (stage >= kTevStages) {
        return false;
    }
    const std::uint32_t packed = (static_cast<std::uint32_t>(ind_stage) & 3u) | ((static_cast<std::uint32_t>(fmt) & 3u) << 2) |
                                 ((static_cast<std::uint32_t>(bias) & 7u) << 4) | ((static_cast<std::uint32_t>(mtx) & 15u) << 9) |
                                 ((static_cast<std::uint32_t>(wrap_s) & 7u) << 13) |
                                 ((static_cast<std::uint32_t>(wrap_t) & 7u) << 16) | (add_prev ? (1u << 20) : 0u);
    return load_bp(static_cast<std::uint8_t>(0x10u + stage), packed);
}

bool Fifo::trigger_efb_copy(bool to_xfb) {
    return load_bp(0x4B, to_xfb ? (1u << 11) : 0u);
}

bool Fifo::set_tev_order(unsigned stage, std::uint8_t texcoord, std::uint8_t texmap, std::uint8_t ras_chan) {
    if (stage >= kTevStages) {
        return false;
    }
    const unsigned idx = stage / 2u;
    const bool odd = (stage & 1u) != 0;
    const std::uint32_t map = (texmap == kTexMapNull) ? 0u : (static_cast<std::uint32_t>(texmap) & 7u);
    const std::uint32_t coord = (texcoord == kTexCoordNull) ? 0u : (static_cast<std::uint32_t>(texcoord) & 7u);
    const std::uint32_t chan = (ras_chan == kRasZero) ? 7u : (ras_chan == kRasColor1 ? 1u : 0u);
    const std::uint32_t enable = (texmap != kTexMapNull) ? 1u : 0u;
    const unsigned map_sh = odd ? 12u : 0u;
    const unsigned coord_sh = odd ? 15u : 3u;
    const unsigned en_sh = odd ? 18u : 6u;
    const unsigned chan_sh = odd ? 19u : 7u;
    std::uint32_t& tref = tev_tref_[idx];
    const std::uint32_t mask = (7u << map_sh) | (7u << coord_sh) | (1u << en_sh) | (7u << chan_sh);
    const std::uint32_t bits = (map << map_sh) | (coord << coord_sh) | (enable << en_sh) | (chan << chan_sh);
    tref = (tref & ~mask) | bits;
    return load_bp(static_cast<std::uint8_t>(0x28u + idx), tref);
}

bool Fifo::set_tev_color_op(unsigned stage, std::uint8_t op, std::uint8_t bias, std::uint8_t shift, bool clamp,
                            std::uint8_t dest) {
    if (stage >= kTevStages) {
        return false;
    }
    std::uint32_t color = tev_color_reg_[stage] & 0xFFFFu;
    if (clamp) {
        color |= 1u << 19;
    }
    color |= (static_cast<std::uint32_t>(dest) & 3u) << 22;
    if (op >= TevCompR8Gt) {
        const std::uint32_t hw = static_cast<std::uint32_t>(op - 8u);
        color |= 3u << 16;
        color |= (hw & 1u) << 18;
        color |= ((hw >> 1) & 3u) << 20;
    } else {
        color |= (static_cast<std::uint32_t>(bias) & 3u) << 16;
        color |= (static_cast<std::uint32_t>(op) & 1u) << 18;
        color |= (static_cast<std::uint32_t>(shift) & 3u) << 20;
    }
    tev_color_reg_[stage] = color;
    return load_bp(static_cast<std::uint8_t>(0xC0u + stage * 2u), color);
}

bool Fifo::set_fog(std::uint8_t type, float start_z, float end_z, float near_z, float far_z, std::uint8_t r,
                   std::uint8_t g, std::uint8_t b) {
    const bool ortho = (type & 8u) != 0;
    float a = 0.0f;
    float c = 0.0f;
    std::uint32_t b_m = 0;
    std::uint32_t b_s = 0;
    if (!ortho) {
        if (far_z == near_z || end_z == start_z) {
            a = 0.0f;
            c = 0.0f;
            b_m = static_cast<std::uint32_t>(8.388638e6f * 0.5f);
        } else {
            const float A = (far_z * near_z) / ((far_z - near_z) * (end_z - start_z));
            float B = far_z / (far_z - near_z);
            c = start_z / (end_z - start_z);
            float B_mant = B;
            int expn = 0;
            while (B_mant > 1.0f) {
                B_mant *= 0.5f;
                ++expn;
            }
            while (B_mant > 0.0f && B_mant < 0.5f) {
                B_mant *= 2.0f;
                --expn;
            }
            a = std::ldexp(A, -(expn + 1));
            b_m = static_cast<std::uint32_t>(8.388638e6f * B_mant);
            b_s = static_cast<std::uint32_t>(expn + 1) & 0x1Fu;
        }
    } else if (far_z != near_z && end_z != start_z) {
        const float inv = 1.0f / (end_z - start_z);
        a = inv * (far_z - near_z);
        c = inv * (start_z - near_z);
    }
    auto pack_partial = [](float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return ((bits >> 12) & 0x7FFu) | (((bits >> 23) & 0xFFu) << 11) | (((bits >> 31) & 1u) << 19);
    };
    const std::uint32_t fog0 = pack_partial(a);
    const std::uint32_t fog1 = b_m & 0x00FFFFFFu;
    const std::uint32_t fog2 = b_s & 0x1Fu;
    const std::uint32_t fog3 = pack_partial(c) | ((ortho ? 1u : 0u) << 20) | ((static_cast<std::uint32_t>(type) & 7u) << 21);
    const std::uint32_t fogc = static_cast<std::uint32_t>(b) | (static_cast<std::uint32_t>(g) << 8) |
                               (static_cast<std::uint32_t>(r) << 16);
    return load_bp(0xEE, fog0) && load_bp(0xEF, fog1) && load_bp(0xF0, fog2) && load_bp(0xF1, fog3) &&
           load_bp(0xF2, fogc);
}

bool Fifo::set_viewport(float left, float top, float width, float height, float near_z, float far_z) {
    return write_u8(kLoadAurora) && write_u16(kAuroraViewport) && write_f32(left) && write_f32(top) && write_f32(width) &&
           write_f32(height) && write_f32(near_z) && write_f32(far_z);
}

bool Fifo::set_scissor(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    return write_u8(kLoadAurora) && write_u16(kAuroraScissor) && write_u32(static_cast<std::uint32_t>(x)) &&
           write_u32(static_cast<std::uint32_t>(y)) && write_u32(static_cast<std::uint32_t>(width)) &&
           write_u32(static_cast<std::uint32_t>(height));
}

bool Fifo::draw(Primitive prim, VtxFmt fmt, std::uint16_t count, const void* vertices, std::uint32_t vertex_bytes) {
    const std::uint8_t cmd = static_cast<std::uint8_t>(prim | (static_cast<std::uint8_t>(fmt) & kVatMask));
    return write_u8(cmd) && write_u16(count) && write_bytes(vertices, vertex_bytes);
}

void Processor::end_frame() {
    vertex_worker_drain();
    g_batches_flushed = 0;
    g_skipped_prims = 0;
    vert_count = 0;
    draw_count = 0;
    error_count = 0;
    clear_fifo_error();
    clear_xf_stats();
    clear_efb_events();
}

void Processor::reset() {
    vertex_worker_drain();
    state.reset();
    vert_count = 0;
    draw_count = 0;
    error_count = 0;
    clear_fifo_error();
    clear_xf_stats();
    clear_efb_events();
    skipped_indexed_xf = 0;
    skipped_dl = 0;
    skipped_aurora = 0;
    efb_copy_count = 0;
    map_count = 0;
    dl_depth = 0;
    std::memset(maps, 0, sizeof(maps));
}

void Processor::map_range(std::uint32_t addr, const void* host, std::uint32_t size) {
    if (host == nullptr || size == 0) {
        return;
    }
    for (std::uint32_t i = 0; i < map_count; ++i) {
        if (maps[i].addr == addr) {
            maps[i].host = static_cast<const std::uint8_t*>(host);
            maps[i].size = size;
            return;
        }
    }
    if (map_count >= kMaxMemMaps) {
        return;
    }
    maps[map_count].addr = addr;
    maps[map_count].size = size;
    maps[map_count].host = static_cast<const std::uint8_t*>(host);
    ++map_count;
}

const std::uint8_t* Processor::resolve(std::uint32_t addr, std::uint32_t size) const {
    for (std::uint32_t i = 0; i < map_count; ++i) {
        if (addr >= maps[i].addr && size <= maps[i].size &&
            (addr - maps[i].addr) + size <= maps[i].size) {
            return maps[i].host + (addr - maps[i].addr);
        }
    }
    if (g_guest_resolver != nullptr) {
        return g_guest_resolver(addr, size, static_cast<unsigned>(dl_depth));
    }
    return nullptr;
}

std::uint32_t vertex_stride(const State& state, VtxFmt fmt) {
    std::uint32_t size = 0;
    const auto& vtx = state.vtx_fmts[fmt];
    for (int i = AttrPnMtxIdx; i <= AttrTex7; ++i) {
        const auto attr = static_cast<Attr>(i);
        const auto& af = vtx.attrs[i];
        switch (state.vtx_desc[i]) {
        case AttrNone:
            break;
        case AttrDirect:
            size += static_cast<std::uint32_t>(comp_type_size(attr, af.type)) *
                    static_cast<std::uint32_t>(comp_cnt_count(attr, af.cnt));
            break;
        case AttrIndex8:
            size += (attr == AttrNrm && af.cnt == NrmNBT3) ? 3u : 1u;
            break;
        case AttrIndex16:
            size += (attr == AttrNrm && af.cnt == NrmNBT3) ? 6u : 2u;
            break;
        }
    }
    return size;
}

void configure_pos_clr0_direct(Fifo& fifo, VtxFmt fmt) {
    const std::uint32_t vcd_lo = (static_cast<std::uint32_t>(AttrDirect) << 9) |
                                 (static_cast<std::uint32_t>(AttrDirect) << 13);
    fifo.load_cp(0x50, vcd_lo);
    fifo.load_cp(0x60, 0);
    const std::uint32_t vat_a = (static_cast<std::uint32_t>(PosXYZ) << 0) | (static_cast<std::uint32_t>(F32) << 1) |
                                (static_cast<std::uint32_t>(ClrRGBA) << 13) |
                                (static_cast<std::uint32_t>(RGBA8) << 14);
    fifo.load_cp(static_cast<std::uint8_t>(0x70 + fmt), vat_a);
}

void configure_pos_clr0_tex0_direct(Fifo& fifo, VtxFmt fmt) {
    const std::uint32_t vcd_lo = (static_cast<std::uint32_t>(AttrDirect) << 9) |
                                 (static_cast<std::uint32_t>(AttrDirect) << 13);
    const std::uint32_t vcd_hi = static_cast<std::uint32_t>(AttrDirect);
    fifo.load_cp(0x50, vcd_lo);
    fifo.load_cp(0x60, vcd_hi);
    const std::uint32_t vat_a = (static_cast<std::uint32_t>(PosXYZ) << 0) | (static_cast<std::uint32_t>(F32) << 1) |
                                (static_cast<std::uint32_t>(ClrRGBA) << 13) | (static_cast<std::uint32_t>(RGBA8) << 14) |
                                (static_cast<std::uint32_t>(TexST) << 21) | (static_cast<std::uint32_t>(F32) << 22);
    fifo.load_cp(static_cast<std::uint8_t>(0x70 + fmt), vat_a);
}

void configure_pos_clr0_tex0_tex1_direct(Fifo& fifo, VtxFmt fmt) {
    const std::uint32_t vcd_lo = (static_cast<std::uint32_t>(AttrDirect) << 9) |
                                 (static_cast<std::uint32_t>(AttrDirect) << 13);
    const std::uint32_t vcd_hi = static_cast<std::uint32_t>(AttrDirect) | (static_cast<std::uint32_t>(AttrDirect) << 2);
    fifo.load_cp(0x50, vcd_lo);
    fifo.load_cp(0x60, vcd_hi);
    const std::uint32_t vat_a = (static_cast<std::uint32_t>(PosXYZ) << 0) | (static_cast<std::uint32_t>(F32) << 1) |
                                (static_cast<std::uint32_t>(ClrRGBA) << 13) | (static_cast<std::uint32_t>(RGBA8) << 14) |
                                (static_cast<std::uint32_t>(TexST) << 21) | (static_cast<std::uint32_t>(F32) << 22);
    const std::uint32_t vat_b = (static_cast<std::uint32_t>(TexST) << 0) | (static_cast<std::uint32_t>(F32) << 1);
    fifo.load_cp(static_cast<std::uint8_t>(0x70 + fmt), vat_a);
    fifo.load_cp(static_cast<std::uint8_t>(0x80 + fmt), vat_b);
}

void configure_pos_index16_clr0_index8(Fifo& fifo, VtxFmt fmt) {
    const std::uint32_t vcd_lo = (static_cast<std::uint32_t>(AttrIndex16) << 9) |
                                 (static_cast<std::uint32_t>(AttrIndex8) << 13);
    fifo.load_cp(0x50, vcd_lo);
    fifo.load_cp(0x60, 0);
    const std::uint32_t vat_a = (static_cast<std::uint32_t>(PosXYZ) << 0) | (static_cast<std::uint32_t>(F32) << 1) |
                                (static_cast<std::uint32_t>(ClrRGBA) << 13) |
                                (static_cast<std::uint32_t>(RGBA8) << 14);
    fifo.load_cp(static_cast<std::uint8_t>(0x70 + fmt), vat_a);
}

void configure_pos_index16_nrm_index8_clr0_index8(Fifo& fifo, VtxFmt fmt) {
    const std::uint32_t vcd_lo = (static_cast<std::uint32_t>(AttrIndex16) << 9) |
                                 (static_cast<std::uint32_t>(AttrIndex8) << 11) |
                                 (static_cast<std::uint32_t>(AttrIndex8) << 13);
    fifo.load_cp(0x50, vcd_lo);
    fifo.load_cp(0x60, 0);
    const std::uint32_t vat_a = (static_cast<std::uint32_t>(PosXYZ) << 0) | (static_cast<std::uint32_t>(F32) << 1) |
                                (static_cast<std::uint32_t>(F32) << 10) | (static_cast<std::uint32_t>(ClrRGBA) << 13) |
                                (static_cast<std::uint32_t>(RGBA8) << 14);
    fifo.load_cp(static_cast<std::uint8_t>(0x70 + fmt), vat_a);
}

bool Processor::process(const std::uint8_t* data, std::uint32_t size, bool big_endian) {
    // Records only the first decode error of the frame so the watchdog can report
    // exactly where process() gave up instead of just how many times it did.
    const auto note_err = [&](std::uint32_t site, std::uint32_t cmd_byte, std::uint32_t off) {
        if (error_count == 0) {
            g_err_site = site;
            g_err_cmd = cmd_byte;
            g_err_offset = off;
            g_err_depth = static_cast<std::uint32_t>(dl_depth);
            g_err_size = size;
        }
        error_count += 1u;
    };
    // A command cut off by the end of the buffer. In prefix mode (the HLE FIFO draining a full
    // buffer mid-frame) that is not an error: the command's remaining bytes are still to come,
    // so processing stops in front of it and the caller keeps it for the next pass.
    const auto note_trunc = [&](std::uint32_t site, std::uint32_t cmd_byte, std::uint32_t off) {
        if (g_prefix_mode && dl_depth == 0) {
            if (g_prefix_stop > off) {
                g_prefix_stop = off;
            }
            return;
        }
        note_err(site, cmd_byte, off);
    };
    if (!data) {
        note_err(1u, 0u, 0u);
        return false;
    }
    if (dl_depth == 0 && draw_count == 0) {
        gpu_mesh_begin_batch();
    }
    ++g_reuse_epoch;
    ++g_layout_epoch;  // process boundaries can observe externally changed arrays/state
    std::uint32_t pos = 0;
    std::uint32_t unknown_ops = 0;
    while (pos < size) {
        const std::uint8_t cmd = data[pos++];
        if (cmd < 0x80 || cmd > 0xBF) ++g_reuse_epoch;
        const std::uint32_t cmd_off = pos - 1u;
        const std::uint8_t opcode = static_cast<std::uint8_t>(cmd & kOpcodeMask);
        if ((cmd < 0x80 || cmd > 0xBF) && opcode != (kLoadBp & kOpcodeMask) &&
            opcode != kNop && cmd != 0x44) {
            // Conservatively retire on CP/XF/indexed loads, DLs and Aurora escapes.
            ++g_layout_epoch;
        }

        if (opcode == kNop || cmd == 0x44) {
            // 0x44 is GX_CMD_UNKNOWN_METRICS: a single byte, not a display-list call.
            continue;
        }
        if (opcode == (kLoadBp & kOpcodeMask)) {
            if (!remain(pos, size, 4)) {
                note_trunc(2u, cmd, cmd_off);
                break;
            }
            const std::uint32_t packed = read_u32(data + pos, big_endian);
            handle_bp(state, packed);
            if (((packed >> 24) & 0xFFu) == 0x4B) {
                ++efb_copy_count;
            }
            pos += 4;
            continue;
        }
        if (opcode == kLoadCp) {
            if (!remain(pos, size, 5)) {
                note_trunc(3u, cmd, cmd_off);
                break;
            }
            const std::uint8_t addr = data[pos++];
            handle_cp(state, addr, read_u32(data + pos, big_endian));
            pos += 4;
            continue;
        }
        if (opcode == kLoadXf) {
            if (!handle_xf(state, data, pos, size, big_endian)) {
                note_trunc(4u, cmd, cmd_off);
                break;
            }
            continue;
        }
        if (opcode == kLoadIndxA || opcode == kLoadIndxB || opcode == kLoadIndxC || opcode == kLoadIndxD) {
            if (!remain(pos, size, 4)) {
                note_trunc(5u, cmd, cmd_off);
                break;
            }
            const std::uint32_t value = read_u32(data + pos, big_endian);
            pos += 4;
            const unsigned kind = static_cast<unsigned>((opcode - kLoadIndxA) / 0x08u);
            const std::uint32_t src_idx = value >> 16;
            const std::uint32_t len = ((value >> 12) & 0xFu) + 1u;
            const std::uint16_t dst = static_cast<std::uint16_t>(value & 0xFFFu);
            const AttrArray& arr = state.xf_arrays[kind < kMaxXfArrays ? kind : 0];
            const std::uint32_t offset = src_idx * arr.stride;
            const std::uint32_t bytes = len * 4u;
            if (arr.data == nullptr || arr.stride == 0 || offset + bytes > arr.size) {
                // No escape-provided array: read the one GXSetArray programmed into CP
                // 0xAC-0xAF, straight from guest memory.
                const unsigned k = kind < 4u ? kind : 0u;
                const std::uint8_t* src = nullptr;
                if (g_guest_resolver != nullptr && g_xf_array_base[k] != 0 && g_xf_array_stride[k] != 0) {
                    src = g_guest_resolver(g_xf_array_base[k] + src_idx * g_xf_array_stride[k], bytes,
                                           static_cast<unsigned>(dl_depth) + 4u);
                }
                if (src == nullptr || !copy_xf_mem(state, dst, src, len, true)) {  // guest RAM is big-endian
                    ++skipped_indexed_xf;
                    g_fetch_fail.ixf_kind = k;
                    g_fetch_fail.ixf_base = g_xf_array_base[k];
                    g_fetch_fail.ixf_stride = g_xf_array_stride[k];
                    g_fetch_fail.ixf_index = src_idx;
                    g_fetch_fail.ixf_words = (static_cast<std::uint32_t>(dst) << 8) | len;
                    g_fetch_fail.ixf_why = (g_xf_array_base[k] == 0 || g_xf_array_stride[k] == 0) ? 1u
                                           : (src == nullptr)                                     ? 2u
                                                                                                  : 3u;
                }
                continue;
            }
            if (!copy_xf_mem(state, dst, arr.data + offset, len, big_endian && !arr.le)) {
                ++skipped_indexed_xf;
            }
            continue;
        }
        if (opcode == kCallDl) {
            if (!remain(pos, size, 8)) {
                note_trunc(6u, cmd, cmd_off);
                break;
            }
            std::uint32_t addr = read_u32(data + pos, big_endian);
            pos += 4;
            const std::uint32_t bytes = read_u32(data + pos, big_endian);
            pos += 4;
            // A list written by guest GX code carries a physical address.
            if (addr < 0x80000000u) {
                addr = physical_to_effective(addr & 0x3FFFFFFFu);
            }
            if (dl_depth >= 4) {
                ++skipped_dl;
                continue;
            }
            const std::uint8_t* host = resolve(addr, bytes);
            if (host == nullptr || bytes == 0) {
                ++skipped_dl;
                g_fetch_fail.dl_addr = addr;
                g_fetch_fail.dl_bytes = bytes;
                g_fetch_fail.dl_depth = static_cast<std::uint32_t>(dl_depth);
                continue;
            }
            // A display list is self-contained: an error inside it is recorded but does not
            // take the rest of this stream down with it.
            g_dl_guest[dl_depth + 1] = addr;
            g_dl_host[dl_depth + 1] = host;
            ++dl_depth;
            process(host, bytes, big_endian);
            --dl_depth;
            continue;
        }
        if (opcode == kInvalidateVtx) {
            gpu_mesh_invalidate();
            continue;
        }
        if (opcode == kLoadAurora && remain(pos, size, 2) && read_u16(data + pos, big_endian) == kAuroraEfbCopyTex) {
            // sub u16, src x/y/w/h u16 x4, dst w/h u16 x2, format u32, flags u8, dest guest
            // u32, dest host u64 - 31 bytes including the sub id.
            if (!remain(pos, size, 31)) {
                note_trunc(7u, cmd, cmd_off);
                break;
            }
            pos += 2;
            EfbCopyEvent ev;
            ev.draw_index = draw_count;
            ev.src_x = read_u16(data + pos, big_endian);
            ev.src_y = read_u16(data + pos + 2, big_endian);
            ev.src_w = read_u16(data + pos + 4, big_endian);
            ev.src_h = read_u16(data + pos + 6, big_endian);
            ev.dst_w = read_u16(data + pos + 8, big_endian);
            ev.dst_h = read_u16(data + pos + 10, big_endian);
            ev.format = read_u32(data + pos + 12, big_endian);
            const std::uint8_t flags = data[pos + 16];
            ev.clear = (flags & 1u) != 0;
            ev.dest_guest = read_u32(data + pos + 17, big_endian);
            ev.dest_host = reinterpret_cast<const std::uint8_t*>(
                static_cast<std::uintptr_t>(read_u64(data + pos + 21, big_endian)));
            ev.clear_rgba = g_copy_clear_rgba;
            // GXCopyTex clears only enabled write channels, at the copy's FIFO
            // position. Later draws may change these masks before submission.
            ev.pixel_fmt = state.pixel_fmt;
            ev.clear_color = ev.clear && state.color_update;
            ev.clear_alpha = ev.clear && state.alpha_update && state.pixel_fmt == 1;
            ev.clear_depth = ev.clear && state.depth_write;
            ev.clear_z = state.clear_z;
            pos += 29;
            // Kept while skipping frames: the copy reads the EFB as the last rendered frame
            // left it (the renderer does it without a swap), which beats a missing texture -
            // a one-off snapshot such as the pause menu's backdrop would otherwise be lost.
            ++efb_copy_count;
            if (g_efb_event_count < kMaxEfbCopyEvents) {
                g_efb_events[g_efb_event_count++] = ev;
            }
            continue;
        }
        if (opcode == kLoadAurora) {
            if (!handle_aurora(state, data, pos, size, big_endian, &skipped_aurora)) {
                note_trunc(7u, cmd, cmd_off);
                break;
            }
            continue;
        }
        if (cmd >= 0x80 && cmd <= 0xBF) {
            if (gpu_mesh_active() && !capture_active() && draw_count + 1u < kMaxDraws && !g_skip_draws) {
                std::uint32_t resume = 0, nidx = 0;
                if (gpu_mesh_try(state, data, cmd_off, size, big_endian, &resume, &nidx)) {
                    DrawCall draw{};
                    draw.prim = Triangles;
                    draw.fmt = VtxFmt0;
                    draw.first_vertex = 0;
                    draw.vertex_count = nidx;
                    draw.tex0 = false;
                    const unsigned nst = state.num_tev_stages == 0 ? 1u : static_cast<unsigned>(state.num_tev_stages);
                    for (unsigned st = 0; st < nst && st < kTevStages; ++st) {
                        if (tev_stage_can_sample(state, state.tev[st])) {
                            draw.tex0 = true;
                            break;
                        }
                    }
                    draw.tev = classify_tev(state);
                    draw.blend = state.blend_mode == BlendMix || state.blend_mode == BlendSubtract;
                    draw.blend_mode = state.blend_mode;
                    draw.logic_op = state.logic_op;
                    draw.blend_src = state.blend_src;
                    draw.blend_dst = state.blend_dst;
                    draw.num_tev_stages = state.num_tev_stages == 0 ? 1 : state.num_tev_stages;
                    draw.num_tex_gens = state.num_tex_gens;
                    draw.num_chans = state.num_chans;
                    std::memcpy(draw.tev_stages, state.tev, sizeof(draw.tev_stages));
                    std::memcpy(draw.tev_color, state.tev_color, sizeof(draw.tev_color));
                    std::memcpy(draw.tev_reg, state.tev_reg, sizeof(draw.tev_reg));
                    std::memcpy(draw.konst, state.konst, sizeof(draw.konst));
                    std::memcpy(draw.tev_swap, state.tev_swap, sizeof(draw.tev_swap));
                    draw.fog = state.fog;
                    draw.alpha_ref0 = state.alpha_ref0;
                    draw.alpha_ref1 = state.alpha_ref1;
                    draw.alpha_comp0 = state.alpha_comp0;
                    draw.alpha_comp1 = state.alpha_comp1;
                    draw.alpha_op = state.alpha_op;
                    draw.dst_alpha_enable = state.dst_alpha_enable;
                    draw.dst_alpha = state.dst_alpha;
                    draw.pixel_fmt = state.pixel_fmt;
                    draw.zcomploc = state.zcomploc;
                    g_draw_extra_owner = this;
                    snapshot_draw_extra(state, g_draw_extra[draw_count]);
                    g_draw_extra[draw_count].lit = GpuLitParams{};
                    g_draw_glit[draw_count] = 0;
                    g_draw_extra_slot[draw_count] = static_cast<std::uint16_t>(draw_count);
                    g_draw_key[draw_count] = 0;
                    g_draw_snapshot_epoch = g_reuse_epoch;
                    gpu_mesh_bind_draw(draw_count);
                    draws[draw_count++] = draw;
                    pos = resume;
                    continue;
                }
            }
            if (!remain(pos, size, 2)) {
                note_trunc(8u, cmd, cmd_off);
                break;
            }
            const std::uint16_t vtx_count = read_u16(data + pos, big_endian);
            pos += 2;
            const auto fmt = static_cast<VtxFmt>(cmd & kVatMask);
            Primitive prim = primitive_from_cmd(cmd);
            // VCD/VAT only change with a state command (a new epoch).
            static std::uint64_t s_stride_epoch = ~0ull;
            static VtxFmt s_stride_fmt = VtxFmt0;
            static std::uint32_t s_stride = 0;
            if (s_stride_epoch != g_layout_epoch || s_stride_fmt != fmt) {
                s_stride = vertex_stride(state, fmt);
                s_stride_epoch = g_layout_epoch;
                s_stride_fmt = fmt;
            }
            const std::uint32_t stride = s_stride;
            const std::uint32_t total = static_cast<std::uint32_t>(vtx_count) * stride;
            if (!remain(pos, size, total)) {
                note_trunc(9u, cmd, cmd_off);
                break;
            }
            if (g_skip_draws) {
                ++g_skipped_prims;
                pos += total;
                continue;
            }
            const bool stroke = prim == Lines || prim == LineStrip || prim == Points;
            // Lines and points become two triangles per segment/point.
            const std::uint32_t out_verts = stroke ? static_cast<std::uint32_t>(vtx_count) * 6u : vtx_count;
            if (out_verts > kMaxDecodedVerts) {
                note_err(9u, cmd, cmd_off);  // one primitive larger than a whole batch
                pos += total;
                continue;
            }
            if (vert_count + out_verts > kMaxDecodedVerts || draw_count >= kMaxDraws) {
                // The batch is full: hand what we have to the renderer and carry on.
                if (g_batch_flush == nullptr) {
                    note_err(9u, cmd, cmd_off);
                    break;
                }
                vertex_worker_drain();
                if (capture_armed() || capture_active()) {
                    resolve_vertex_copies(*this);  // the capture records processor.verts
                }
                g_batch_flush(*this);
                ++g_batches_flushed;
                vert_count = 0;
                draw_count = 0;
                clear_efb_events();
                gpu_mesh_begin_batch();
                // The reuse cache points into this batch's vertices.
                ++g_reuse_epoch;
            }
            // Stroke primitives decode into a scratch area past the batch's vertices, then
            // expand back into place.
            static DecodedVertex s_stroke_src[kMaxDecodedVerts / 6u + 1u];
            DecodedVertex* dst = stroke ? s_stroke_src : verts + vert_count;
            // Format is part of the cache epoch: consecutive draws may change VAT
            // without a register write. Endianness is fixed within this process call.
            static VtxFmt reuse_fmt = VtxFmt0;
            if (fmt != reuse_fmt) { ++g_reuse_epoch; reuse_fmt = fmt; }
            // BP writes retire processed vertices but keep the prepared layout valid.
            // CP/XF and process boundaries retire both.
            static VertexDecoder s_decoder;
            static std::uint64_t s_decoder_epoch = ~0ull;
            static VtxFmt s_decoder_fmt = VtxFmt0;
            static bool s_decoder_be = true;
            if (s_decoder_epoch != g_layout_epoch || s_decoder_fmt != fmt || s_decoder_be != big_endian) {
                s_decoder.prepare(state, fmt, big_endian);
                s_decoder_epoch = g_layout_epoch;
                s_decoder_fmt = fmt;
                s_decoder_be = big_endian;
            }
            // Vertices are fixed-size packets (the decoder and vertex_stride agree on the size);
            // if they ever disagree, decode sequentially and do not reuse.
            const bool packets = s_decoder.vertex_bytes() == stride;
            // The cache holds indices into this batch's vertices, so strokes (decoded into a
            // scratch area) do not take part.
            const bool reuse = g_reuse_enabled && !stroke && packets && stride != 0 && stride <= kReuseBytes;
            unsigned reused = 0;
            // GXSetTexCoordScaleManually: the coordinate is scaled by the SU size the game set
            // rather than by the size of the texture it samples. GL samples normalised
            // coordinates, so a manual coordinate is rescaled by su_size / texture size (Aurora
            // applies (scale + 1) / texture size per sampled coordinate; with automatic scaling
            // the two are equal).
            static float su_scale[kMaxTexGens][2];
            static bool su_any;
            static std::uint64_t su_epoch = 0;
            if (su_epoch != g_reuse_epoch) {
                su_epoch = g_reuse_epoch;
                su_any = false;
                for (unsigned c = 0; c < kMaxTexGens; ++c) {
                    su_scale[c][0] = 1.0f;
                    su_scale[c][1] = 1.0f;
                    if (!state.su_manual[c] || c >= state.num_tex_gens) {
                        continue;
                    }
                    const unsigned nst = state.num_tev_stages == 0 ? 1u : static_cast<unsigned>(state.num_tev_stages);
                    for (unsigned st = 0; st < nst && st < kTevStages; ++st) {
                        const TevStage& ts = state.tev[st];
                        if (!tev_stage_can_sample(state, ts) || tev_effective_texcoord(state, ts.texcoord) != static_cast<int>(c)) {
                            continue;
                        }
                        const TexObj& tex = state.texmaps[ts.texmap & 7u];
                        if (tex.width != 0 && tex.height != 0) {
                            su_scale[c][0] = static_cast<float>(state.su_size_s[c]) / static_cast<float>(tex.width);
                            su_scale[c][1] = static_cast<float>(state.su_size_t[c]) / static_cast<float>(tex.height);
                            su_any = true;
                        }
                        break;
                    }
                }
            }
            static PreparedLighting prepared;
            static std::uint64_t lighting_epoch = 0;
            static bool epoch_glit = false;
            if (lighting_epoch != g_reuse_epoch) {
                prepare_lighting(state, prepared);
                epoch_glit = gpu_lit_eligible(state, prepared);
                lighting_epoch = g_reuse_epoch;
            }
            // Lines and points are expanded in clip space here, from lit vertices: they keep
            // the CPU lighting.
            const bool glit = epoch_glit && !stroke;
            // Vertex jobs: decoding, texgen, lighting, the transform and packing run on the
            // worker core (and on whoever waits) from a snapshot of this epoch's vertex state;
            // this thread only stashes the bytes and checks the reuse cache. A snapshot costs
            // ~5 KiB of copying, so an epoch only goes out once it has more than a few vertices
            // (a UI quad per state change stays here; a model's run of strips goes). Strokes
            // are expanded right away and stay here too.
            constexpr std::uint32_t kInlineVerts = 24;
            static std::uint32_t s_snap_id = 0;
            static std::uint64_t s_snap_epoch = ~0ull;
            static std::uint64_t s_count_epoch = ~0ull;
            static std::uint32_t s_epoch_verts = 0;
            if (s_count_epoch != g_reuse_epoch) {
                s_count_epoch = g_reuse_epoch;
                s_epoch_verts = 0;
            }
            s_epoch_verts += vtx_count;
            if (vert_count == 0) {
                vertex_batch_begin(this);  // vertices are numbered from 0 again
            }
            const unsigned ntc = pack_ntc(state.num_tex_gens);
            const std::uint32_t pstride = pack_stride(ntc, glit);
            bool worker = false;
            if (!stroke && packets && vtx_count != 0 && vertex_worker_active() &&
                (s_snap_epoch == g_reuse_epoch || s_epoch_verts > kInlineVerts)) {
                if (s_snap_epoch != g_reuse_epoch) {
                    s_snap_id = vertex_worker_snapshot(state, prepared, su_scale, su_any, s_decoder, epoch_glit);
                    s_snap_epoch = g_reuse_epoch;
                }
                worker = true;
            }
            bool decoded_ok = true;
            const std::uint8_t* seq = data + pos;
            const std::uint8_t* const prim_end = data + pos + total;
            // Display-list vertex cache (static64, dl_cache.hpp): a DL primitive seen in an earlier
            // frame skips decoding (and the reuse hashing: all its vertices are processed from the
            // cached model-space data, which is cheaper than finding the repeats).
            DlCacheRef dl{};
            DlCacheResult dl_mode = kDlNone;
            if (g_dl_cache_on && dl_depth > 0 && dl_depth < 8 && !stroke && packets && vtx_count >= 3u &&
                s_decoder.compactable() && dl_cache_active() && !capture_armed() && !capture_active()) {
                const std::uint32_t guest =
                    g_dl_guest[dl_depth] + static_cast<std::uint32_t>((data + pos) - g_dl_host[dl_depth]);
                dl_mode = dl_cache_lookup(guest, s_decoder.signature(), vtx_count, hash_bytes64(data + pos, total), &dl);
                if (dl_mode == kDlHit && dl.check) {
                    // Spot check that the arrays did not change under the cached copy.
                    DecodedVertex t;
                    const std::uint8_t* nx = nullptr;
                    CachedVertex c{};
                    const bool ok = s_decoder.decode(data + pos, prim_end, t, &nx);
                    if (ok) {
                        VertexDecoder::store(t, c);
                    }
                    if (!ok || std::memcmp(&c, dl.data, 48) != 0) {
                        dl_mode = dl_cache_refill(&dl);
                    }
                }
            }
            const bool use_reuse = reuse && dl_mode == kDlNone;
            // One pass per vertex: a vertex whose packet was already processed in this epoch
            // is remapped to it (the renderer's indices point there); any other is decoded,
            // texgen'd, lit, transformed and packed - here, or by a vertex job.
            const std::uint8_t* stash = (worker && dl_mode != kDlHit) ? vertex_worker_stash(data + pos, total) : nullptr;
            if (worker && dl_mode != kDlHit && stash == nullptr) {
                worker = false;  // a primitive larger than the stash: do it here
            }
            std::uint8_t* const arena = vertex_batch_arena();
            const std::uint32_t pack_off = stroke ? 0u : vertex_batch_alloc(vert_count, vtx_count, ntc, glit);
            if (dl_mode != kDlNone) {
                for (std::uint32_t i = 0; i < vtx_count; ++i) {
                    vertex_batch_set_remap(vert_count + i, -1);
                }
                if (!worker) {
                    for (std::uint32_t i = 0; i < vtx_count; ++i) {
                        DecodedVertex& dv = dst[i];
                        if (dl_mode == kDlHit) {
                            s_decoder.restore(dl.data[i], dv);
                        } else {
                            if (!s_decoder.decode(data + pos + i * stride, prim_end, dv, &seq)) {
                                decoded_ok = false;
                                break;
                            }
                            VertexDecoder::store(dv, dl.data[i]);
                        }
                        if (glit) {
                            process_vertex_gpu_lit(state, su_scale, su_any, dv);
                        } else {
                            process_vertex(state, prepared, su_scale, su_any, dv);
                        }
                        pack_vertex(dv, ntc, arena + pack_off + i * pstride, glit);
                    }
                }
            } else {
            for (std::uint32_t i = 0; i < vtx_count; ++i) {
                DecodedVertex& dv = dst[i];
                const std::uint32_t vidx = vert_count + i;  // == &dv - verts unless stroke
                const std::uint8_t* packet = packets ? data + pos + i * stride : seq;
                VertexReuse* cached = nullptr;
                std::uint32_t h = 0;
                if (use_reuse) {
                    h = vertex_packet_hash(packet, stride);
                    cached = &g_reuse[h & (kReuseSlots - 1)];
                    if (cached->epoch == g_reuse_epoch && cached->hash == h &&
                        std::memcmp(cached->bytes, packet, stride) == 0) {
                        vertex_batch_set_remap(vidx, static_cast<std::int32_t>(cached->index));
                        if (!worker && s_snap_epoch != g_reuse_epoch) {
                            // Inline epoch: the source is complete, so keep verts whole too (a
                            // job's source may still be in flight: resolve_vertex_copies).
                            dv = verts[cached->index];
                        }
                        ++reused;
                        continue;
                    }
                }
                if (!stroke) {
                    vertex_batch_set_remap(vidx, -1);
                }
                if (!worker) {
                    if (!s_decoder.decode(packet, prim_end, dv, &seq)) {
                        decoded_ok = false;
                        break;
                    }
                    if (glit) {
                        process_vertex_gpu_lit(state, su_scale, su_any, dv);
                    } else {
                        process_vertex(state, prepared, su_scale, su_any, dv);
                    }
                    if (!stroke) {
                        pack_vertex(dv, ntc, arena + pack_off + i * pstride, glit);
                    }
                }
                if (cached) {
                    cached->epoch = g_reuse_epoch;
                    cached->hash = h;
                    cached->index = vidx;
                    std::memcpy(cached->bytes, packet, stride);
                }
            }
            }
            if (!decoded_ok) {
                // Cache entries may name vertices of this dropped primitive; retire them.
                ++g_reuse_epoch;
                if (dl_mode == kDlFill) {
                    dl_cache_drop(dl);
                }
                if (!stroke) {
                    vertex_batch_unalloc(vert_count, vtx_count, ntc, glit);
                }
                note_err(10u, cmd, cmd_off);
                pos += total;
                continue;
            }
            if (worker) {
                vertex_worker_push(verts, vert_count, vtx_count, s_snap_id, dl_mode == kDlHit ? nullptr : stash, stride,
                                   dl_mode == kDlHit ? dl.data : nullptr, dl_mode == kDlFill ? dl.data : nullptr);
                perf::count(perf::kWorkerVertices, vtx_count);
            }
            perf::count(perf::kVertices, vtx_count);
            perf::count(perf::kReusedVertices, reused);
            std::uint32_t emitted = vtx_count;
            if (stroke) {
                float vp_w = 640.0f;
                float vp_h = 528.0f;
                if (state.has_render_vp && state.render_vp[2] > 1.0f && state.render_vp[3] > 1.0f) {
                    vp_w = state.render_vp[2];
                    vp_h = state.render_vp[3];
                }
                const float width = static_cast<float>(prim == Points ? state.point_size : state.line_width);
                emitted = expand_strokes(prim, s_stroke_src, vtx_count, width / 12.0f, vp_w, vp_h, verts + vert_count,
                                         kMaxDecodedVerts - vert_count);
                prim = Triangles;
                const std::uint32_t off = vertex_batch_alloc(vert_count, emitted, ntc);
                for (std::uint32_t i = 0; i < emitted; ++i) {
                    vertex_batch_set_remap(vert_count + i, -1);
                    pack_vertex(verts[vert_count + i], ntc, arena + off + i * pstride);
                }
            }
            // A run of primitives without intervening state writes shares its
            // complete material/raster snapshot. Only geometry differs. Avoid
            // re-hashing TEV and resolving eight textures/TLUTs for every strip.
            if (g_reuse_enabled && draw_count != 0 && g_draw_extra_owner == this && g_draw_snapshot_epoch == g_reuse_epoch) {
                DrawCall& draw = draws[draw_count];
                draw = draws[draw_count - 1];
                draw.prim = prim;
                draw.fmt = fmt;
                draw.first_vertex = vert_count;
                draw.vertex_count = emitted;
                g_draw_extra_slot[draw_count] = g_draw_extra_slot[draw_count - 1];
                g_draw_key[draw_count] = g_draw_key[draw_count - 1];
                g_draw_glit[draw_count] = glit ? 1 : 0;
                perf::count(perf::kReusedDrawStates);
                ++draw_count;
                vert_count += emitted;
                pos += total;
                continue;
            }
            const TevOp tev = classify_tev(state);
            bool uses_tex = false;
            const unsigned nst = state.num_tev_stages == 0 ? 1u : static_cast<unsigned>(state.num_tev_stages);
            for (unsigned st = 0; st < nst && st < kTevStages; ++st) {
                // Without a texgen a stage's sample is a constant, not a texture read.
                if (tev_stage_can_sample(state, state.tev[st])) {
                    uses_tex = true;
                    break;
                }
            }
            DrawCall draw{};
            draw.prim = prim;
            draw.fmt = fmt;
            draw.first_vertex = vert_count;
            draw.vertex_count = emitted;
            draw.tex0 = uses_tex;
            draw.tev = tev;
            draw.blend = state.blend_mode == BlendMix || state.blend_mode == BlendSubtract;
            draw.blend_mode = state.blend_mode;
            draw.logic_op = state.logic_op;
            draw.blend_src = state.blend_src;
            draw.blend_dst = state.blend_dst;
            // Not hashed per draw any more: nothing linked reads tev_key (the TEV stages and
            // colours are in the DrawCall itself, which is what the GL path keys on).
            draw.tev_key = 0;
            draw.num_tev_stages = state.num_tev_stages == 0 ? 1 : state.num_tev_stages;
            draw.num_tex_gens = state.num_tex_gens;
            draw.num_chans = state.num_chans;
            std::memcpy(draw.tev_stages, state.tev, sizeof(draw.tev_stages));
            std::memcpy(draw.tev_color, state.tev_color, sizeof(draw.tev_color));
            std::memcpy(draw.tev_reg, state.tev_reg, sizeof(draw.tev_reg));
            std::memcpy(draw.konst, state.konst, sizeof(draw.konst));
            std::memcpy(draw.tev_swap, state.tev_swap, sizeof(draw.tev_swap));
            draw.fog = state.fog;
            draw.alpha_ref0 = state.alpha_ref0;
            draw.alpha_ref1 = state.alpha_ref1;
            draw.alpha_comp0 = state.alpha_comp0;
            draw.alpha_comp1 = state.alpha_comp1;
            draw.alpha_op = state.alpha_op;
            draw.dst_alpha_enable = state.dst_alpha_enable;
            draw.dst_alpha = state.dst_alpha;
            draw.pixel_fmt = state.pixel_fmt;
            draw.zcomploc = state.zcomploc;
            g_draw_extra_owner = this;
            snapshot_draw_extra(state, g_draw_extra[draw_count]);
            if (epoch_glit) {
                fill_gpu_lit_params(state, prepared, g_draw_extra[draw_count].lit);
            } else {
                g_draw_extra[draw_count].lit = GpuLitParams{};
            }
            g_draw_glit[draw_count] = glit ? 1 : 0;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
            // The submission merger compares snapshots bytewise. Uninitialised
            // padding used to split otherwise identical materials into GL calls.
            __builtin_clear_padding(&draw);
            __builtin_clear_padding(&g_draw_extra[draw_count]);
#endif
            g_draw_extra_slot[draw_count] = static_cast<std::uint16_t>(draw_count);
            g_draw_key[draw_count] = draw_key_of(draw, g_draw_extra[draw_count]);
            g_draw_snapshot_epoch = g_reuse_epoch;
            draws[draw_count++] = draw;
            vert_count += emitted;
            pos += total;
            continue;
        }

        // Unknown opcode. The hardware would hang; skipping one byte and carrying on at
        // least keeps the rest of the frame (Aurora command_processor.cpp:564-588 does the
        // same), with a cap so a stream of garbage does not spin.
        note_err(11u, cmd, cmd_off);
        if (++unknown_ops > 64u) {
            break;
        }
    }
    if (dl_depth == 0) {
        vertex_worker_flush();  // let the worker start on this call's tail of vertices
    }
    return error_count == 0;
}

bool write_efb_copy_escape(Fifo& f, const TexCopySetup& setup, bool clear, std::uint32_t dest_guest,
                           const std::uint8_t* dest_host) {
    const std::uint8_t flags = static_cast<std::uint8_t>((clear ? 1u : 0u) | (setup.mipmap ? 2u : 0u));
    return f.write_u8(kLoadAurora) && f.write_u16(kAuroraEfbCopyTex) && f.write_u16(setup.src_x) &&
           f.write_u16(setup.src_y) && f.write_u16(setup.src_w) && f.write_u16(setup.src_h) &&
           f.write_u16(setup.dst_w) && f.write_u16(setup.dst_h) && f.write_u32(setup.format) && f.write_u8(flags) &&
           f.write_u32(dest_guest) &&
           f.write_u64(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(dest_host)));
}

void set_gpu_lighting(bool on) {
    g_gpu_lighting = on;
}

bool gpu_lighting_enabled() {
    return g_gpu_lighting;
}

std::uint64_t draw_merge_key(const Processor& processor, std::uint32_t index) {
    if (g_draw_extra_owner != &processor || index >= processor.draw_count || index >= kMaxDraws) {
        return 0;
    }
    return g_draw_key[index];
}

void set_dl_cache(bool on) {
    g_dl_cache_on = on;
}

bool draw_gpu_lit(const Processor& processor, std::uint32_t index) {
    if (g_draw_extra_owner != &processor || index >= processor.draw_count || index >= kMaxDraws) {
        return false;
    }
    return g_draw_glit[index] != 0;
}

const DrawExtra* draw_extra(const Processor& processor, std::uint32_t index) {
    if (g_draw_extra_owner != &processor || index >= processor.draw_count || index >= kMaxDraws) {
        return nullptr;
    }
    return &g_draw_extra[g_draw_extra_slot[index]];
}

GuestFetchFailure last_guest_fetch_failure() {
    return g_fetch_fail;
}

void set_guest_resolver(GuestResolver resolver) {
    g_guest_resolver = resolver;
}

void set_direct_resolver(DirectResolver resolver) {
    g_direct_resolver = resolver;
}

void set_window_resolver(WindowResolver resolver) {
    g_window_resolver = resolver;
}

const std::uint8_t* guest_window(std::uint32_t addr, std::uint32_t* base, std::uint32_t* size) {
    return g_window_resolver != nullptr ? g_window_resolver(addr, base, size) : nullptr;
}

void set_vertex_reuse(bool enabled) { g_reuse_enabled = enabled; ++g_reuse_epoch; }

void set_skip_draws(bool skip) { g_skip_draws = skip; }
bool skip_draws() { return g_skip_draws; }
std::uint32_t skipped_primitives() { return g_skipped_prims; }

void set_batch_flush(BatchFlushFn fn) {
    g_batch_flush = fn;
}

std::uint32_t batches_flushed() {
    return g_batches_flushed;
}

std::uint32_t process_prefix(Processor& processor, const std::uint8_t* data, std::uint32_t size, bool big_endian) {
    g_prefix_mode = true;
    g_prefix_stop = size;
    processor.process(data, size, big_endian);
    g_prefix_mode = false;
    return g_prefix_stop;
}

void set_fifo_full_handler(FifoFullFn fn) {
    g_fifo_full = fn;
}

const std::uint8_t* guest_fetch(std::uint32_t addr, std::uint32_t size) {
    // Level 7 of the resolver's staging buffers: display lists use 0-4 and indexed XF loads
    // 4-7, but both copy out immediately, as the vertex decoder does.
    return g_guest_resolver != nullptr ? g_guest_resolver(addr, size, 7u) : nullptr;
}

std::uint32_t efb_clear_color() {
    return g_copy_clear_rgba;
}

namespace {
int g_disp_src[4] = {0, 0, 0, 0};
bool g_disp_src_set = false;
}  // namespace

void set_display_copy_source(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    g_disp_src[0] = x;
    g_disp_src[1] = y;
    g_disp_src[2] = w;
    g_disp_src[3] = h;
    g_disp_src_set = true;
}

bool display_copy_source(int* x, int* y, int* w, int* h) {
    *x = g_disp_src[0];
    *y = g_disp_src[1];
    *w = g_disp_src[2];
    *h = g_disp_src[3];
    return g_disp_src_set;
}

unsigned efb_copy_events(const EfbCopyEvent** out) {
    if (out != nullptr) {
        *out = g_efb_events;
    }
    return g_efb_event_count;
}

void xf_load_stats(std::uint32_t* pn, std::uint32_t* tex, std::uint32_t* ident, std::uint32_t* nrm,
                   std::uint32_t* dtt, std::uint32_t* light, std::uint32_t* reg, std::uint32_t* drop,
                   std::uint32_t* first_tex_addr, std::uint32_t* first_tex_count) {
    *pn = g_xf_pn;
    *tex = g_xf_tex;
    *ident = g_xf_ident;
    *nrm = g_xf_nrm;
    *dtt = g_xf_dtt;
    *light = g_xf_light;
    *reg = g_xf_reg;
    *drop = g_xf_drop;
    *first_tex_addr = g_xf_tex_addr;
    *first_tex_count = g_xf_tex_count;
}

void last_fifo_error(std::uint32_t* site, std::uint32_t* cmd, std::uint32_t* offset, std::uint32_t* size,
                     std::uint32_t* depth) {
    if (site != nullptr) *site = g_err_site;
    if (cmd != nullptr) *cmd = g_err_cmd;
    if (offset != nullptr) *offset = g_err_offset;
    if (size != nullptr) *size = g_err_size;
    if (depth != nullptr) *depth = g_err_depth;
}

} // namespace mkwv::gx
