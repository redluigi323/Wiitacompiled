#pragma once

#include "mkwv/gx/fifo.hpp"
#include "mkwv/gx/draw_state.hpp"

#include <cstdint>

namespace mkwv::gx {

std::uint8_t comp_type_size(Attr attr, CompType type);
std::uint8_t comp_cnt_count(Attr attr, CompCnt cnt);

bool decode_vertices(const State& state, VtxFmt fmt, const std::uint8_t* data, std::uint32_t bytes, bool big_endian,
                     DecodedVertex* out, std::uint16_t count);

// A decoded vertex as the display-list vertex cache keeps it (dl_cache.hpp): the model-space
// attributes a "compactable" layout can carry - position, normal, two texture coordinates, two
// colours and the position-matrix index. 52 bytes; the first 48 are the vertex data.
struct CachedVertex {
    float x, y, z;
    float nx, ny, nz;
    float tc[2][2];
    std::uint32_t color, color1;
    std::uint8_t pn_mtx;
    std::uint8_t pad[3];
};
static_assert(sizeof(CachedVertex) == 52, "CachedVertex is 52 bytes");

// One vertex layout (VCD, VAT, array bindings, byte order) prepared once and then applied
// per vertex: what decode_vertices does, split so a caller can decode only some vertices.
// It holds copies of the array bindings, so it stays valid when the state moves on (the
// vertex worker decodes from a copy taken with the epoch's snapshot). Array element
// contents are read from memory when decoding.
class VertexDecoder {
public:
    static constexpr unsigned kMaxEntries = AttrTex7 - AttrPnMtxIdx + 1;
    // Per-thread cache of the guest memory window each array was last read from.
    struct Windows {
        const std::uint8_t* win[kMaxEntries];
        std::uint32_t base[kMaxEntries];
        std::uint32_t size[kMaxEntries];
        void clear() {
            for (unsigned i = 0; i < kMaxEntries; ++i) win[i] = nullptr;
        }
    };

    // Returns the FIFO bytes one vertex takes.
    std::uint32_t prepare(const State& state, VtxFmt fmt, bool big_endian);
    // Decodes the vertex at p (at most end - p bytes); *next is the byte after it.
    // allow_staging: an element the window cannot serve may go through the guest resolver's
    // staging buffers, which only the decoding thread may use.
    bool decode(const std::uint8_t* p, const std::uint8_t* end, DecodedVertex& out, const std::uint8_t** next,
                Windows& windows, bool allow_staging) const;
    bool decode(const std::uint8_t* p, const std::uint8_t* end, DecodedVertex& out, const std::uint8_t** next) {
        return decode(p, end, out, next, windows_, true);
    }
    std::uint32_t vertex_bytes() const { return vertex_bytes_; }
    // A hash of everything that decides what a packet decodes to apart from the array
    // contents: the layout, byte order and array bindings.
    std::uint64_t signature() const { return sig_; }
    // Whether a decoded vertex of this layout fits a CachedVertex without loss (no NBT, no
    // texture-matrix indices, texture coordinates 0 and 1 only).
    bool compactable() const { return compactable_; }
    // The compact form of a vertex decoded with this layout, and back (restore starts from the
    // current defaults, so the current matrix index still applies when there is no PNMTXIDX).
    static void store(const DecodedVertex& v, CachedVertex& c);
    void restore(const CachedVertex& c, DecodedVertex& v) const;
    // Only fields consumed by fast_xf_pack, for non-mirrored compact cache hits.
    void restore_for_pack(const CachedVertex& c, DecodedVertex& v) const;

    struct Entry {
        std::uint8_t kind;
        std::uint8_t desc;         // AttrType
        std::uint8_t type;         // CompType
        std::uint8_t ncomp;        // components read from the element (pos 2/3, nrm 3/9, tex 1/2)
        std::uint8_t index_bytes;  // 1 or 2 (index8/16), per index
        std::uint8_t slot;         // texture coordinate / matrix slot
        bool be;                   // byte order of the element
        float scale;               // dequantisation (exact power of two)
        std::uint32_t comp_bytes;
        std::uint32_t nbytes;      // element bytes (direct: FIFO bytes)
        AttrArray array;           // the binding, copied
    };

private:
    Entry entries_[kMaxEntries];
    unsigned count_ = 0;
    std::uint32_t vertex_bytes_ = 0;
    std::uint64_t sig_ = 0;
    bool compactable_ = false;
    bool has_pn_idx_ = false;
    bool big_endian_ = true;
    DecodedVertex base_{};
    Windows windows_{};
};

// The GL vertex layout the renderer draws from: clip position (4 floats), COLOR0, COLOR1
// (4 bytes each, RGBA), then `ntc` texture coordinates (s, t, q). ntc is clamped to 1..8.
inline constexpr unsigned kPackOffColor0 = 16u;
inline constexpr unsigned kPackOffColor1 = 20u;
inline constexpr unsigned kPackOffTex = 24u;
inline unsigned pack_ntc(unsigned num_tex_gens) {
    return num_tex_gens == 0 ? 1u : (num_tex_gens > kMaxTexGens ? kMaxTexGens : num_tex_gens);
}
// GPU-lit vertices (draw_state.hpp GpuLitParams) add the eye-space position and the
// (unnormalised) eye-space normal after the texture coordinates: 6 floats. Their COLOR0/COLOR1
// slots hold the raw vertex colours the lighting reads, not lit colours.
inline constexpr unsigned kPackLitBytes = 24u;
inline unsigned pack_stride(unsigned ntc, bool lit = false) {
    return kPackOffTex + 12u * ntc + (lit ? kPackLitBytes : 0u);
}
// A GPU-lit vertex keeps its eye position in (bx, by, bz) and its eye normal in (tx, ty, tz)
// after processing (the binormal/tangent are only texgen sources, used by then).
void pack_vertex(const DecodedVertex& v, unsigned ntc, std::uint8_t* out, bool lit = false);

std::uint32_t expanded_index_count(Primitive prim, std::uint32_t vtx_count);
std::uint32_t expand_indices(Primitive prim, std::uint32_t vtx_count, std::uint16_t* out, std::uint32_t cap);
// Lines/points -> triangles in clip space. half_px is half the GX width in EFB pixels
// (width/12, GX widths being in sixths of a pixel); vp_w/vp_h the viewport in EFB pixels.
std::uint32_t expand_strokes(Primitive prim, const DecodedVertex* in, std::uint32_t count, float half_px, float vp_w,
                             float vp_h, DecodedVertex* out, std::uint32_t cap);

// Model space -> GL clip space (x, y, z, w); see decode.cpp.
void transform_vertex(const State& state, const DecodedVertex& in, float clip[4]);
// Texgen from `raw` (the decoded vertex) into v.tc.
void apply_texgen(const State& state, const DecodedVertex& raw, DecodedVertex& v);
void apply_texgen(const State& state, DecodedVertex& v);
void apply_lighting(const State& state, DecodedVertex& v);

// Valid until the next GX state change. Geometry remains per vertex.
struct PreparedLighting {
    float direction[kMaxLights][3];
    float specular_distance[kMaxLights][3];
    // The lighting plan: per channel control (COLOR0, COLOR1, ALPHA0, ALPHA1, as State::chans)
    // whether it is lit, where its material and ambient come from, and its lights in order.
    struct Half {
        std::uint8_t lit, mat_vtx, amb_vtx, diff_fn, attn_fn, count;
        std::uint8_t light[kMaxLights];
    };
    Half half[4];
    float color[kMaxLights][4];              // each light's colour bytes (r, g, b, a) as floats
    std::uint8_t geometry_light[kMaxLights];  // lights some lit channel uses
    std::uint8_t geometry_count;
    bool any_lit;
};
void prepare_lighting(const State& state, PreparedLighting& out);
// Whether this state's lighting can run in the vertex shader (anything lit with colour
// channels on), and its parameters for the shader.
bool gpu_lit_eligible(const State& state, const PreparedLighting& prepared);
void fill_gpu_lit_params(const State& state, const PreparedLighting& prepared, GpuLitParams& out);
// The vertex shader's lighting, in C++ (host tests check it against the CPU path; the Cg the
// renderer emits is a transliteration). vtx0/vtx1 are the raw colours a GPU-lit vertex packs.
void gpu_lit_reference(const GpuLitParams& p, const float eye[3], const float nrm[3], std::uint32_t vtx0,
                       std::uint32_t vtx1, std::uint32_t* color0, std::uint32_t* color1);
void light_and_transform(const State& state, const PreparedLighting& prepared, DecodedVertex& v);

// The parts of State the per-vertex path (texgen, lighting, transform) reads, with the same
// member names, plus the per-epoch values derived from it. The vertex worker processes
// vertices from one of these while the render thread goes on decoding. ~4 KiB.
struct VertexSnapshot {
    AttrType vtx_desc[AttrCount];
    float pn_mtx[kMaxPnMtx][12];
    float nrm_mtx[kMaxPnMtx][9];
    float proj[4][4];
    std::uint32_t ambient[2];
    std::uint32_t chan_mat[2];
    TexGen texgens[kMaxTexGens];
    Light lights[kMaxLights];
    std::uint8_t num_chans;
    std::uint8_t num_tex_gens;
    bool dual_tex;
    bool su_any;
    float su_scale[kMaxTexGens][2];
    PreparedLighting prepared;
    bool gpu_lit;           // lighting runs on the GPU: pack raw colours, eye position, normal
    VertexDecoder decoder;  // the epoch's vertex layout (the worker decodes too)
    float xf_mem[kXfMemSize];
    float xf_dtt[kXfDttSize];
};
void capture_vertex_snapshot(const State& state, const PreparedLighting& prepared, const float (*su_scale)[2],
                             bool su_any, VertexSnapshot& out);
// Texgen (from the raw vertex), the SU coordinate scale, lighting and the transform to clip
// space. Identical results from the live state and from a snapshot of it.
void process_vertex(const State& state, const PreparedLighting& prepared, const float (*su_scale)[2], bool su_any,
                    DecodedVertex& v);
void process_vertex(const VertexSnapshot& snap, DecodedVertex& v);
// The same without the lighting: raw vertex colours (COLOR0/COLOR1 as the lighting would read
// them), eye position into (bx, by, bz), eye normal into (tx, ty, tz). For GPU-lit draws.
void process_vertex_gpu_lit(const State& state, const float (*su_scale)[2], bool su_any, DecodedVertex& v);
// One job's vertices, in this translation unit so decode, texgen, lighting, the transform
// and packing inline into a single loop. `remap[index] >= 0` skips a vertex (it repeats an
// earlier one). Returns how many vertices failed to decode.
std::uint32_t process_snapshot_vertices(const VertexSnapshot& snap, DecodedVertex* verts, const std::uint8_t* bytes,
                                       std::uint32_t first, std::uint32_t count, std::uint32_t stride,
                                       const std::int32_t* remap, std::uint8_t* arena, const std::uint32_t* offset);
// The same with the display-list cache (static64): `cached` (a cache hit) supplies the decoded
// vertices instead of `bytes`; `fill` (a cache miss being filled) receives them as they are
// decoded. Either is indexed by job vertex (0 .. count). `verts` may be null: the processed
// DecodedVertex then stays on the stack (only captures and the tests read Processor::verts).
struct VertexJob {
    const VertexSnapshot* snap = nullptr;
    DecodedVertex* verts = nullptr;
    const std::uint8_t* bytes = nullptr;
    const CachedVertex* cached = nullptr;
    CachedVertex* fill = nullptr;
    std::uint32_t first = 0, count = 0, stride = 0;
    const std::int32_t* remap = nullptr;
    std::uint8_t* arena = nullptr;
    const std::uint32_t* offset = nullptr;
};
std::uint32_t process_vertex_job(const VertexJob& job);

// Guest memory for vertex-array elements (fifo.cpp, through the runtime's resolver). The
// returned bytes are only valid until the next call.
const std::uint8_t* guest_fetch(std::uint32_t addr, std::uint32_t size);
// A contiguous window of guest memory containing `addr` (host pointer for guest *base, *size
// bytes) when the runtime installed a window resolver, else nullptr. See efb_copy.hpp.
const std::uint8_t* guest_window(std::uint32_t addr, std::uint32_t* base, std::uint32_t* size);

} // namespace mkwv::gx
