#include "mkwv/gx/gpu_mesh.hpp"

#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/perf.hpp"
#include "mkwv/gx/types.hpp"

#include <cstring>
#include <memory>
#include <vector>

namespace mkwv::gx {
namespace {

bool g_active = false;
std::uint32_t g_generation = 1;

struct Mesh {
    std::uint64_t index_key = 0;
    std::uint64_t content_key = 0;
    std::unique_ptr<std::uint8_t[]> verts;
    std::unique_ptr<std::uint16_t[]> indices;
    std::uint32_t nverts = 0;
    std::uint32_t nidx = 0;
    std::uint32_t vcap = 0;
    std::uint32_t icap = 0;
    std::uint8_t mtx_slot = 0;
    bool used = false;
};

constexpr unsigned kSlots = 384;
constexpr std::uint32_t kMaxRunVerts = 65536;
constexpr std::uint32_t kMaxRunIdx = 196608;
constexpr unsigned kFpCap = 24;
constexpr std::uint32_t kFpBytes = 512u * 1024u;
constexpr unsigned kMaxPrims = 512;

Mesh g_meshes[kSlots];

struct Fp {
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    std::uint32_t gen = 0;
    std::uint64_t hash = 0;
    bool used = false;
};
Fp g_fp[kFpCap];

struct Vert {
    float px, py, pz;
    float nx, ny, nz;
    float u0, v0;
    float u1, v1;
    std::uint8_t r, g, b, a;
};
static_assert(sizeof(Vert) == 44, "mesh vertex stride");

struct RejectCount {
    std::uint32_t vary = 0, tex = 0, gen = 0, chan = 0, light = 0, decode = 0;
};
RejectCount g_rej;

enum Why : int { kOk = 0, kTex, kGen, kChan, kLight };

enum Build : int { kBuildFail = 0, kBuildOk = 1, kBuildVary = 2 };

GpuMeshView g_pending{};
bool g_have_pending = false;
std::uint8_t g_draw_on[1024];
GpuMeshView g_draw_view[1024];

std::uint16_t read_u16(const std::uint8_t* p, bool be) {
    const unsigned lo = p[0];
    const unsigned hi = p[1];
    return static_cast<std::uint16_t>(be ? ((lo << 8) | hi) : (lo | (hi << 8)));
}

std::uint64_t fnv_add(std::uint64_t h, const std::uint8_t* p, std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}

const std::uint8_t* array_bytes(const AttrArray& a, std::uint32_t* n) {
    if (a.data != nullptr && a.size != 0) {
        *n = a.size < kFpBytes ? a.size : kFpBytes;
        return a.data;
    }
    if (a.guest_base == 0) {
        *n = 0;
        return nullptr;
    }
    std::uint32_t base = 0, size = 0;
    const std::uint8_t* win = guest_window(a.guest_base, &base, &size);
    if (win == nullptr || a.guest_base < base || a.guest_base - base >= size) {
        *n = 0;
        return nullptr;
    }
    const std::uint32_t off = a.guest_base - base;
    std::uint32_t avail = size - off;
    if (a.size != 0 && a.size < avail) {
        avail = a.size;
    }
    if (avail > kFpBytes) {
        avail = kFpBytes;
    }
    *n = avail;
    return win + off;
}

std::uint64_t fingerprint(const AttrArray& a) {
    if (a.data == nullptr && a.guest_base == 0) {
        return 0;
    }
    const std::uint32_t base = a.data != nullptr ? static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a.data))
                                                 : a.guest_base;
    for (Fp& e : g_fp) {
        if (e.used && e.base == base && e.size == a.size && e.gen == g_generation) {
            return e.hash;
        }
    }
    std::uint32_t n = 0;
    const std::uint8_t* p = array_bytes(a, &n);
    const std::uint64_t h = (p != nullptr ? fnv_add(14695981039346656037ull, p, n) : 0x1234ull) ^ a.stride;
    for (Fp& e : g_fp) {
        if (!e.used || e.gen != g_generation) {
            e.used = true;
            e.base = base;
            e.size = a.size;
            e.gen = g_generation;
            e.hash = h;
            return h;
        }
    }
    g_fp[0] = Fp{base, a.size, g_generation, h, true};
    return h;
}

Why classify(const State& state) {
    for (int a = AttrTex0MtxIdx; a <= AttrTex7MtxIdx; ++a) {
        if (state.vtx_desc[a] != AttrNone) {
            return kTex;
        }
    }
    if (state.vtx_desc[AttrPos] == AttrNone) {
        return kGen;
    }
    if (state.vtx_desc[AttrClr1] != AttrNone) {
        return kChan;
    }
    for (int a = AttrTex2; a <= AttrTex7; ++a) {
        if (state.vtx_desc[a] != AttrNone) {
            return kGen;
        }
    }
    if (state.num_tex_gens > 2) {
        return kGen;
    }
    if (state.num_chans > 2) {
        return kChan;
    }
    for (unsigned i = 0; i < state.num_tex_gens; ++i) {
        const TexGen& g = state.texgens[i];
        // Gen i reads texcoord i. A 2x4 matrix, optional dual-tex post matrix, no normalize.
        if (g.type != TgMtx2x4 || g.normalize || g.src != static_cast<TexGenSrc>(TgTex0 + i)) {
            return kGen;
        }
    }
    PreparedLighting lit{};
    prepare_lighting(state, lit);
    if (lit.half[1].lit) {
        return kChan;
    }
    if (lit.half[2].lit || lit.half[3].lit) {
        return kLight;
    }
    if (lit.any_lit) {
        const PreparedLighting::Half& h = lit.half[0];
        if (h.attn_fn == AfSpec || h.count > 4 || h.diff_fn > DfClamp) {
            return kLight;
        }
    }
    return kOk;
}

void note_why(Why why) {
    switch (why) {
    case kTex: ++g_rej.tex; break;
    case kGen: ++g_rej.gen; break;
    case kChan: ++g_rej.chan; break;
    case kLight: ++g_rej.light; break;
    case kOk: break;
    }
}

// 2x4 tex matrix, then the dual-tex post matrix with z forced to 1, as texgen_impl does.
void tex_rows_for(const State& state, const TexGen& gen, float rows[8]) {
    if (gen.mtx != kTexMtxIdentity && static_cast<unsigned>(gen.mtx) * 4u + 8u <= kXfMemSize) {
        std::memcpy(rows, state.xf_mem + static_cast<unsigned>(gen.mtx) * 4u, 8u * sizeof(float));
    } else {
        const float ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        std::memcpy(rows, ident, sizeof(ident));
    }
    if (state.dual_tex && gen.post_mtx != kPtTexMtxIdentity && gen.post_mtx >= kPtTexMtx0) {
        const unsigned slot = static_cast<unsigned>(gen.post_mtx - kPtTexMtx0) * 4u;
        if (slot + 12u <= kXfDttSize) {
            const float* post = state.xf_dtt + slot;
            float r0[4], r1[4];
            std::memcpy(r0, rows, sizeof(r0));
            std::memcpy(r1, rows + 4, sizeof(r1));
            for (unsigned row = 0; row < 2u; ++row) {
                const float* pr = post + row * 4u;
                for (unsigned k = 0; k < 4u; ++k) {
                    rows[row * 4u + k] = pr[0] * r0[k] + pr[1] * r1[k];
                }
                rows[row * 4u + 2u] += pr[2];
                rows[row * 4u + 3u] += pr[3];
            }
        }
    }
}

void snapshot_xf(const State& state, GpuMeshView& v, unsigned pn_slot) {
    const unsigned pn = pn_slot < kMaxPnMtx ? pn_slot : 0;
    std::memcpy(v.pn, state.pn_mtx[pn], sizeof(v.pn));
    std::memcpy(v.nrm_mtx, state.nrm_mtx[pn], sizeof(v.nrm_mtx));
    std::memcpy(v.proj, state.proj, sizeof(v.proj));
    const float ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    for (unsigned g = 0; g < 2u; ++g) {
        float* rows = v.tex_rows + g * 8u;
        if (g < state.num_tex_gens) {
            tex_rows_for(state, state.texgens[g], rows);
        } else {
            std::memcpy(rows, ident, sizeof(ident));
        }
        if (g < state.num_tex_gens && state.su_manual[g]) {
            const unsigned nst = state.num_tev_stages == 0 ? 1u : static_cast<unsigned>(state.num_tev_stages);
            for (unsigned st = 0; st < nst && st < kTevStages; ++st) {
                const TevStage& ts = state.tev[st];
                if (!tev_stage_can_sample(state, ts) || tev_effective_texcoord(state, ts.texcoord) != static_cast<int>(g)) {
                    continue;
                }
                const TexObj& tex = state.texmaps[ts.texmap & 7u];
                if (tex.width != 0 && tex.height != 0) {
                    const float su_s = static_cast<float>(state.su_size_s[g]) / static_cast<float>(tex.width);
                    const float su_t = static_cast<float>(state.su_size_t[g]) / static_cast<float>(tex.height);
                    for (unsigned i = 0; i < 4; ++i) {
                        rows[i] *= su_s;
                    }
                    for (unsigned i = 4; i < 8; ++i) {
                        rows[i] *= su_t;
                    }
                }
                break;
            }
        }
    }
    PreparedLighting prep{};
    prepare_lighting(state, prep);
    v.lit = prep.any_lit;
    v.spot = prep.any_lit && prep.half[0].attn_fn == AfSpot;
    v.diff = prep.any_lit ? prep.half[0].diff_fn : static_cast<std::uint8_t>(DfNone);
    v.nlights = prep.any_lit ? prep.half[0].count : 0;
    if (v.nlights > 4) {
        v.nlights = 4;
    }
    v.mat_vtx = prep.half[0].mat_vtx;
    v.amb_vtx = prep.half[0].amb_vtx;
    v.a_mat_vtx = prep.half[2].mat_vtx;
    v.mat_color = state.chan_mat[0];
    v.amb_color = state.ambient[0];
    const std::uint32_t c1_rgb = prep.half[1].mat_vtx ? 0x00FFFFFFu : state.chan_mat[1];
    const std::uint32_t c1_a = prep.half[3].mat_vtx ? 0xFF000000u : state.chan_mat[1];
    const std::uint32_t c1 = (c1_rgb & 0x00FFFFFFu) | (c1_a & 0xFF000000u);
    v.col1[0] = static_cast<float>(c1 & 255u) / 255.0f;
    v.col1[1] = static_cast<float>((c1 >> 8) & 255u) / 255.0f;
    v.col1[2] = static_cast<float>((c1 >> 16) & 255u) / 255.0f;
    v.col1[3] = static_cast<float>((c1 >> 24) & 255u) / 255.0f;
    for (unsigned i = 0; i < v.nlights; ++i) {
        const unsigned li = prep.half[0].light[i];
        const Light& L = state.lights[li];
        std::memcpy(v.light_pos[i], L.pos, sizeof(L.pos));
        std::memcpy(v.light_dir[i], prep.direction[li], sizeof(float) * 3);
        for (unsigned c = 0; c < 4; ++c) {
            v.light_col[i][c] = prep.color[li][c];
        }
        std::memcpy(v.cos_att[i], L.cos_att, sizeof(L.cos_att));
        std::memcpy(v.dist_att[i], L.dist_att, sizeof(L.dist_att));
    }
}

Mesh* slot_for(std::uint64_t index_key, bool* found) {
    Mesh* empty = nullptr;
    for (Mesh& m : g_meshes) {
        if (m.used && m.index_key == index_key) {
            *found = true;
            return &m;
        }
        if (!m.used && empty == nullptr) {
            empty = &m;
        }
    }
    *found = false;
    if (empty != nullptr) {
        return empty;
    }
    // Table full: reuse slot 0. Its bytes are replaced below.
    return &g_meshes[0];
}

int decode_run(const State& state, const std::uint8_t* data, const std::uint32_t* at, const std::uint16_t* nv,
               const std::uint8_t* fmt, const std::uint8_t* prim, unsigned nprim, bool be, Mesh& mesh) {
    std::vector<Vert> verts;
    std::vector<std::uint16_t> indices;
    verts.reserve(2048);
    VertexDecoder dec[8];
    bool prepped[8] = {};
    bool have_slot = false;
    static std::uint16_t local[kMaxRunIdx];
    for (unsigned p = 0; p < nprim; ++p) {
        const unsigned fi = fmt[p];
        if (fi >= 8) {
            return kBuildFail;
        }
        if (!prepped[fi]) {
            dec[fi].prepare(state, static_cast<VtxFmt>(fi), be);
            prepped[fi] = true;
        }
        const std::uint32_t stride = dec[fi].vertex_bytes();
        if (stride == 0) {
            return kBuildFail;
        }
        const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
        const std::uint8_t* packet = data + at[p];
        const std::uint8_t* end = packet + static_cast<std::uint32_t>(nv[p]) * stride;
        VertexDecoder::Windows windows;
        windows.clear();
        for (std::uint16_t i = 0; i < nv[p]; ++i) {
            DecodedVertex dv{};
            const std::uint8_t* next = nullptr;
            if (!dec[fi].decode(packet + static_cast<std::uint32_t>(i) * stride, end, dv, &next, windows, false)) {
                return kBuildFail;
            }
            if (!have_slot) {
                mesh.mtx_slot = dv.pn_mtx;
                have_slot = true;
            } else if (dv.pn_mtx != mesh.mtx_slot) {
                return kBuildVary;
            }
            Vert v{};
            v.px = dv.x;
            v.py = dv.y;
            v.pz = dv.z;
            v.nx = dv.nx;
            v.ny = dv.ny;
            v.nz = dv.nz;
            v.u0 = dv.tc[0][0];
            v.v0 = dv.tc[0][1];
            v.u1 = dv.tc[1][0];
            v.v1 = dv.tc[1][1];
            v.r = static_cast<std::uint8_t>(dv.color);
            v.g = static_cast<std::uint8_t>(dv.color >> 8);
            v.b = static_cast<std::uint8_t>(dv.color >> 16);
            v.a = static_cast<std::uint8_t>(dv.color >> 24);
            verts.push_back(v);
            if (verts.size() > kMaxRunVerts) {
                return kBuildFail;
            }
        }
        const std::uint32_t nidx = expand_indices(static_cast<Primitive>(prim[p]), nv[p], local, kMaxRunIdx);
        if (nidx == 0 && nv[p] != 0) {
            return kBuildFail;
        }
        if (indices.size() + nidx > kMaxRunIdx) {
            return kBuildFail;
        }
        for (std::uint32_t i = 0; i < nidx; ++i) {
            indices.push_back(static_cast<std::uint16_t>(base + local[i]));
        }
    }
    if (verts.size() > mesh.vcap) {
        mesh.verts.reset(new std::uint8_t[verts.size() * sizeof(Vert)]);
        mesh.vcap = static_cast<std::uint32_t>(verts.size());
    }
    if (indices.size() > mesh.icap) {
        mesh.indices.reset(new std::uint16_t[indices.size()]);
        mesh.icap = static_cast<std::uint32_t>(indices.size());
    }
    if (!verts.empty()) {
        std::memcpy(mesh.verts.get(), verts.data(), verts.size() * sizeof(Vert));
    }
    if (!indices.empty()) {
        std::memcpy(mesh.indices.get(), indices.data(), indices.size() * sizeof(std::uint16_t));
    }
    mesh.nverts = static_cast<std::uint32_t>(verts.size());
    mesh.nidx = static_cast<std::uint32_t>(indices.size());
    return mesh.nidx != 0 ? kBuildOk : kBuildFail;
}

void view_from(const Mesh& mesh, const State& state) {
    g_pending = GpuMeshView{};
    const Vert* v = reinterpret_cast<const Vert*>(mesh.verts.get());
    g_pending.pos = &v->px;
    g_pending.nrm = &v->nx;
    g_pending.uv = &v->u0;
    g_pending.uv1 = &v->u1;
    g_pending.color = &v->r;
    g_pending.indices = mesh.indices.get();
    g_pending.stride = sizeof(Vert);
    g_pending.index_count = mesh.nidx;
    // No per-vertex index: the slot is XF MATINDEX_A and can change without touching the
    // vertex bytes, so it is read every draw. A present index is in the cache key.
    const unsigned slot = state.vtx_desc[AttrPnMtxIdx] != AttrNone ? mesh.mtx_slot : state.current_pn_mtx;
    snapshot_xf(state, g_pending, slot);
    g_have_pending = true;
}

}  // namespace

void gpu_mesh_set_active(bool on) { g_active = on; }
bool gpu_mesh_active() { return g_active; }

void gpu_mesh_invalidate() {
    ++g_generation;
    if (g_generation == 0) {
        g_generation = 1;
    }
}

void gpu_mesh_begin_batch() {
    std::memset(g_draw_on, 0, sizeof(g_draw_on));
    g_have_pending = false;
}

bool gpu_mesh_try(const State& state, const std::uint8_t* data, std::uint32_t cmd_off, std::uint32_t size, bool big_endian,
                  std::uint32_t* resume, std::uint32_t* index_count) {
    if (!g_active || cmd_off >= size) {
        return false;
    }
    const Why why = classify(state);
    if (why != kOk) {
        note_why(why);
        return false;
    }
    std::uint32_t at[kMaxPrims];
    std::uint16_t nv[kMaxPrims];
    std::uint8_t fmt[kMaxPrims];
    std::uint8_t prim[kMaxPrims];
    unsigned nprim = 0;
    std::uint64_t index_key = 14695981039346656037ull;
    std::uint32_t p = cmd_off;
    VertexDecoder stride_dec[8];
    bool stride_ok[8] = {};
    std::uint32_t stride_of[8] = {};
    while (p + 3 <= size && nprim < kMaxPrims) {
        const std::uint8_t cmd = data[p];
        if (cmd < 0x80 || cmd > 0xBF) {
            break;
        }
        const std::uint16_t count = read_u16(data + p + 1, big_endian);
        const unsigned fi = cmd & 7u;
        if (!stride_ok[fi]) {
            stride_of[fi] = stride_dec[fi].prepare(state, static_cast<VtxFmt>(fi), big_endian);
            stride_ok[fi] = true;
        }
        const std::uint32_t stride = stride_of[fi];
        const std::uint32_t total = static_cast<std::uint32_t>(count) * stride;
        if (stride == 0 || p + 3u + total > size) {
            break;
        }
        const Primitive kind = static_cast<Primitive>(cmd & 0xF8);
        if (kind == Lines || kind == LineStrip || kind == Points || count == 0) {
            if (nprim == 0) {
                return false;
            }
            break;
        }
        at[nprim] = p + 3;
        nv[nprim] = count;
        fmt[nprim] = static_cast<std::uint8_t>(fi);
        prim[nprim] = static_cast<std::uint8_t>(kind);
        index_key = fnv_add(index_key, data + p, 3u + total);
        ++nprim;
        p += 3u + total;
    }
    if (nprim == 0) {
        return false;
    }
    std::uint64_t content = 0x6D657368ull;
    content = mix(content, fingerprint(state.arrays[AttrPos]));
    if (state.vtx_desc[AttrNrm] != AttrNone) {
        content = mix(content, fingerprint(state.arrays[AttrNrm]));
    }
    if (state.vtx_desc[AttrTex0] != AttrNone) {
        content = mix(content, fingerprint(state.arrays[AttrTex0]));
    }
    if (state.vtx_desc[AttrTex1] != AttrNone) {
        content = mix(content, fingerprint(state.arrays[AttrTex1]));
    }
    if (state.vtx_desc[AttrClr0] != AttrNone) {
        content = mix(content, fingerprint(state.arrays[AttrClr0]));
    }
    bool found = false;
    Mesh* mesh = slot_for(index_key, &found);
    if (!found || mesh->content_key != content) {
        mesh->used = true;
        mesh->index_key = index_key;
        const int built = decode_run(state, data, at, nv, fmt, prim, nprim, big_endian, *mesh);
        if (built != kBuildOk) {
            mesh->used = false;
            mesh->index_key = 0;
            if (built == kBuildVary) {
                ++g_rej.vary;
            } else {
                ++g_rej.decode;
            }
            return false;
        }
        mesh->content_key = content;
        perf::count(perf::kGpuMeshRebuilds);
    } else {
        perf::count(perf::kGpuMeshHits);
    }
    perf::count(perf::kGpuMeshVerts, mesh->nverts);
    view_from(*mesh, state);
    *resume = p;
    *index_count = mesh->nidx;
    return true;
}

void gpu_mesh_bind_draw(std::uint32_t draw_index) {
    if (!g_have_pending || draw_index >= 1024) {
        return;
    }
    g_draw_on[draw_index] = 1;
    g_draw_view[draw_index] = g_pending;
    g_have_pending = false;
}

const GpuMeshView* gpu_mesh_view(std::uint32_t draw_index) {
    if (draw_index >= 1024 || g_draw_on[draw_index] == 0) {
        return nullptr;
    }
    return &g_draw_view[draw_index];
}

void gpu_mesh_reject_take(GpuMeshReject* out) {
    if (out == nullptr) {
        return;
    }
    out->vary = g_rej.vary;
    out->tex = g_rej.tex;
    out->gen = g_rej.gen;
    out->chan = g_rej.chan;
    out->light = g_rej.light;
    out->decode = g_rej.decode;
    g_rej = RejectCount{};
}

}  // namespace mkwv::gx
