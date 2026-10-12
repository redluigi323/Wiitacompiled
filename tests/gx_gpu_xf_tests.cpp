// GPU transform (static70): a display-list vertex drawn from the DL cache's GPU copy goes
// through the vertex shader's transform, texgen and lighting (emit_tev_vertex_cg_xf). Its C++
// form (gpu_xf_reference, then gpu_lit_reference for the colours) must give what the CPU vertex
// path (process_vertex) gives for the same cached vertex and state: positions and texture
// coordinates to float rounding, colours to within a step. Also checks eligibility and the
// emitted program's shape.
#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/dl_cache.hpp"
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/vertex_worker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace mkwv::gx;

namespace {

int byte_diff(std::uint32_t a, std::uint32_t b) {
    int worst = 0;
    for (unsigned k = 0; k < 4u; ++k) {
        const int d = std::abs(static_cast<int>((a >> (8u * k)) & 0xFFu) - static_cast<int>((b >> (8u * k)) & 0xFFu));
        worst = d > worst ? d : worst;
    }
    return worst;
}

bool close(float a, float b) {
    const float d = std::fabs(a - b);
    return d <= 1.0e-4f || d <= 1.0e-4f * std::max(std::fabs(a), std::fabs(b));
}

struct Rng {
    unsigned s;
    unsigned next() {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    float scalar() { return (static_cast<int>(next() % 2001) - 1000) / 113.0f; }
};

// A random state the GPU transform accepts (or, for some trials, one it must refuse).
void random_state(State& state, Rng& r, unsigned trial, bool* expect_ok) {
    state.reset();
    *expect_ok = true;
    state.num_chans = 1 + trial % 2;
    state.vtx_desc[AttrPos] = AttrDirect;
    state.vtx_fmts[0].attrs[AttrPos].cnt = PosXYZ;
    state.vtx_desc[AttrPnMtxIdx] = (trial & 4) ? AttrDirect : AttrNone;
    state.vtx_desc[AttrNrm] = (trial & 8) ? AttrDirect : AttrNone;
    state.vtx_fmts[0].attrs[AttrNrm].cnt = NrmXYZ;
    state.vtx_desc[AttrClr0] = (trial & 1) ? AttrDirect : AttrNone;
    state.vtx_desc[AttrClr1] = (trial & 2) ? AttrDirect : AttrNone;
    state.vtx_desc[AttrTex0] = (trial & 16) ? AttrDirect : AttrNone;
    state.vtx_desc[AttrTex1] = (trial & 32) ? AttrDirect : AttrNone;
    state.current_pn_mtx = static_cast<std::uint8_t>(r.next() % kMaxPnMtx);
    for (auto& ctrl : state.chans) {
        ctrl.lighting = r.next() % 4 != 0;
        ctrl.light_mask = r.next() >> 24;
        ctrl.mat_src = r.next() & 256 ? SrcReg : SrcVtx;
        ctrl.amb_src = r.next() & 256 ? SrcReg : SrcVtx;
        ctrl.diff_fn = static_cast<DiffuseFn>(r.next() % 3);
        ctrl.attn_fn = static_cast<AttnFn>(r.next() % 3);
    }
    for (auto& light : state.lights) {
        light.color = r.next();
        for (unsigned c = 0; c < 3; ++c) {
            light.pos[c] = r.scalar();
            light.dir[c] = r.scalar();
            light.cos_att[c] = r.scalar();
            light.dist_att[c] = 1.0f + static_cast<float>(r.next() % 100) / 100.0f;
        }
    }
    for (unsigned c = 0; c < 2; ++c) {
        state.chan_mat[c] = r.next();
        state.ambient[c] = r.next();
    }
    for (auto& m : state.pn_mtx) {
        for (auto& c : m) c = r.scalar();
    }
    for (auto& m : state.nrm_mtx) {
        for (auto& c : m) c = r.scalar();
    }
    for (auto& row : state.proj) {
        for (auto& c : row) c = r.scalar();
    }
    for (auto& c : state.xf_mem) c = r.scalar();
    for (auto& c : state.xf_dtt) c = r.scalar();
    state.dual_tex = (r.next() & 1) != 0;
    state.num_tex_gens = static_cast<std::uint8_t>(r.next() % (kMaxTexGens + 1));
    static const TexGenSrc kSrc[4] = {TgPos, TgNrm, TgTex0, TgTex1};
    for (unsigned i = 0; i < state.num_tex_gens; ++i) {
        TexGen& g = state.texgens[i];
        g.type = static_cast<TexGenType>(r.next() % 3);
        g.src = kSrc[r.next() % 4];
        const unsigned pick = r.next() % 8;
        g.mtx = pick == 0 ? kTexMtxIdentity : static_cast<std::uint8_t>(r.next() % 62);
        g.normalize = (r.next() & 1) != 0;
        const unsigned post = r.next() % 6;
        g.post_mtx = post == 0 ? kPtTexMtxIdentity : static_cast<std::uint8_t>(kPtTexMtx0 + r.next() % 61);
        g.form_ab11 = (r.next() % 5) == 0;
        if (post == 1) {
            // A post matrix whose q row is zero: the zero-q fold.
            const unsigned slot = static_cast<unsigned>(g.post_mtx - kPtTexMtx0) * 4u;
            if (slot + 12u <= kXfDttSize) {
                for (unsigned k = 8; k < 12; ++k) state.xf_dtt[slot + k] = 0.0f;
            }
        }
    }
    if (trial % 23 == 0 && state.num_tex_gens != 0) {
        state.texgens[0].src = TgColor0;  // not evaluated by the shader
        *expect_ok = false;
    }
    if (trial % 29 == 0) {
        state.num_chans = 0;  // no colour channels: stays on the CPU path
        *expect_ok = false;
    }
}

bool test_reference() {
    set_gpu_lighting(true);
    set_gpu_xf(true);
    Rng r{777};
    unsigned checked = 0, off_by_one = 0, bigger = 0, refused = 0;
    for (unsigned trial = 0; trial < 8000; ++trial) {
        State state;
        bool expect_ok = true;
        random_state(state, r, trial, &expect_ok);
        VertexDecoder dec;
        dec.prepare(state, VtxFmt0, true);
        if (!dec.compactable()) {
            std::fprintf(stderr, "gpu_xf: layout of trial %u not compactable\n", trial);
            return false;
        }
        const bool ok = gpu_xf_eligible(state, dec);
        if (ok != expect_ok) {
            std::fprintf(stderr, "gpu_xf: trial %u eligibility %d, expected %d\n", trial, ok, expect_ok);
            return false;
        }
        if (!ok) {
            ++refused;
            continue;
        }
        float su[kMaxTexGens][2];
        const bool su_any = (trial % 3) == 0;
        for (auto& c : su) {
            c[0] = su_any ? 0.25f + static_cast<float>(r.next() % 8) / 4.0f : 1.0f;
            c[1] = su_any ? 0.25f + static_cast<float>(r.next() % 8) / 4.0f : 1.0f;
        }
        CachedVertex cv{};
        cv.x = r.scalar(); cv.y = r.scalar(); cv.z = r.scalar();
        cv.nx = r.scalar(); cv.ny = r.scalar(); cv.nz = r.scalar();
        cv.tc[0][0] = r.scalar(); cv.tc[0][1] = r.scalar();
        cv.tc[1][0] = r.scalar(); cv.tc[1][1] = r.scalar();
        cv.color = r.next();
        cv.color1 = r.next();
        cv.pn_mtx = static_cast<std::uint8_t>(r.next() % (kMaxPnMtx + 2));  // out of range -> matrix 0

        // CPU: the cached vertex restored and processed as a vertex job would.
        DecodedVertex cpu;
        dec.restore(cv, cpu);
        PreparedLighting prepared;
        prepare_lighting(state, prepared);
        process_vertex(state, prepared, su, su_any, cpu);

        // GPU: the shader's arithmetic.
        static GpuXfParams xp;
        fill_gpu_xf_params(state, dec, su, su_any, xp);
        GpuLitParams lit;
        fill_gpu_lit_params(state, prepared, lit);
        float clip[4], tc[kMaxTexGens][3], eye[3], enrm[3];
        std::uint32_t raw0 = 0, raw1 = 0;
        gpu_xf_reference(xp, cv, clip, tc, &raw0, &raw1, eye, enrm);
        std::uint32_t c0 = 0, c1 = 0;
        gpu_lit_reference(lit, eye, enrm, raw0, raw1, &c0, &c1);

        const float cpu_clip[4] = {cpu.x, cpu.y, cpu.z, cpu.w};
        for (unsigned k = 0; k < 4; ++k) {
            if (!close(clip[k], cpu_clip[k])) {
                std::fprintf(stderr, "gpu_xf: trial %u clip[%u] %g vs CPU %g\n", trial, k, clip[k], cpu_clip[k]);
                return false;
            }
        }
        for (unsigned i = 0; i < state.num_tex_gens; ++i) {
            for (unsigned k = 0; k < 3; ++k) {
                if (!close(tc[i][k], cpu.tc[i][k])) {
                    std::fprintf(stderr, "gpu_xf: trial %u tc%u[%u] %g vs CPU %g (type %u src %u mtx %u post %u n %u ab11 %u)\n",
                                 trial, i, k, tc[i][k], cpu.tc[i][k], state.texgens[i].type, state.texgens[i].src,
                                 state.texgens[i].mtx, state.texgens[i].post_mtx, state.texgens[i].normalize,
                                 state.texgens[i].form_ab11);
                    return false;
                }
            }
        }
        // Raw texcoords past the texgen count (0 and 1) pass through, scaled.
        for (unsigned i = state.num_tex_gens; i < 2u; ++i) {
            for (unsigned k = 0; k < 2; ++k) {
                if (!close(tc[i][k], cpu.tc[i][k])) {
                    std::fprintf(stderr, "gpu_xf: trial %u raw tc%u[%u] %g vs CPU %g\n", trial, i, k, tc[i][k], cpu.tc[i][k]);
                    return false;
                }
            }
        }
        const int d = std::max(byte_diff(c0, cpu.color), byte_diff(c1, cpu.color1));
        ++checked;
        if (d == 1) {
            ++off_by_one;
        } else if (d > 1) {
            ++bigger;
            if (bigger <= 5) {
                std::fprintf(stderr, "gpu_xf: trial %u colours %08X/%08X vs CPU %08X/%08X\n", trial, c0, c1, cpu.color,
                             cpu.color1);
            }
        }
    }
    std::printf("gpu_xf: %u vertices, %u refused as expected, colours %u off by one, %u further\n", checked, refused,
                off_by_one, bigger);
    set_gpu_xf(false);
    set_gpu_lighting(false);
    return checked > 5000 && bigger <= checked / 200u;
}

bool test_emit() {
    ShaderNeeds needs;
    needs.num_tex_gens = 3;
    needs.coord_used[0] = true;
    needs.coord_used[2] = true;
    needs.coord_proj[2] = true;
    needs.color0 = true;
    needs.fog = true;
    needs.fog_slot = 3;
    GpuLitParams lit{};
    lit.on = 1;
    lit.lit[0] = 1;
    lit.light_mask[0] = 0x03;
    lit.diff_fn[0] = DfClamp;
    static char buf[32768];
    std::size_t n = 0;
    if (!emit_tev_vertex_cg_xf(needs, lit, buf, sizeof(buf), &n) || n == 0) {
        std::fprintf(stderr, "gpu_xf: emit failed\n");
        return false;
    }
    const char* want[] = {"uniform float4 uPn[30]", "uniform float4 uTgM0[3]", "uniform float4 uTgP2[3]",
                          "float3 out vTexcoord2 : TEXCOORD2", "float2 out vTexcoord0 : TEXCOORD0", "vZW",
                          "uniform float4 uLP1", "vColor0 ="};
    for (const char* w : want) {
        if (std::strstr(buf, w) == nullptr) {
            std::fprintf(stderr, "gpu_xf: program lacks '%s'\n%s\n", w, buf);
            return false;
        }
    }
    if (std::strstr(buf, "uTgS1") != nullptr || std::strstr(buf, "vColor1") != nullptr) {
        std::fprintf(stderr, "gpu_xf: program declares unused parts\n%s\n", buf);
        return false;
    }
    return true;
}

// ---- End to end: a frame decoded with the GPU transform must draw what the CPU path draws ----

float g_pos[64][3], g_nrm[64][3];
std::uint32_t g_col[64];
constexpr std::uint32_t kDlAddr = 0x80002000u;
unsigned g_syncs = 0;
void gpu_sync() { ++g_syncs; }

void fill_arrays() {
    for (unsigned i = 0; i < 64; ++i) {
        g_pos[i][0] = i * 0.31f - 9.0f;
        g_pos[i][1] = (i % 7) * -0.6f;
        g_pos[i][2] = -4.0f - i * 0.05f;
        g_nrm[i][0] = (i % 3) - 1.0f;
        g_nrm[i][1] = 0.5f;
        g_nrm[i][2] = (i % 2) ? 1.0f : -0.25f;
        g_col[i] = 0x80402010u + i * 0x01030507u;
    }
}

std::unique_ptr<Fifo> make_dl() {
    auto dl = std::make_unique<Fifo>();
    for (unsigned i = 0; i < 24; ++i) {
        if (i == 8) {
            const float m[12] = {1.0f, 0.2f, 0.0f, 0.5f, -0.2f, 1.0f, 0.1f, 1.0f, 0.0f, -0.1f, 1.0f, -6.0f};
            dl->load_xf_f32(0, m, 12);
        }
        if (i == 16) {
            dl->set_chan_ctrl(0, false, SrcReg, SrcVtx, 0x00, DfClamp, AfSpot);
            dl->set_tex_coord_gen(1, TgMtx3x4, TgNrm, 33, true, kPtTexMtx0 + 3);
        }
        std::uint8_t packet[10 * 4];
        const unsigned n = 3 + i % 8;
        for (unsigned v = 0; v < n; ++v) {
            const unsigned idx = (v * 5 + i * 3) % 64;
            packet[v * 4 + 0] = 0;
            packet[v * 4 + 1] = static_cast<std::uint8_t>(idx);
            packet[v * 4 + 2] = static_cast<std::uint8_t>(idx);
            packet[v * 4 + 3] = static_cast<std::uint8_t>(idx);
        }
        dl->draw(i % 3 == 0 ? Triangles : TriangleStrip, VtxFmt0, static_cast<std::uint16_t>(n), packet, n * 4u);
    }
    return dl;
}

std::unique_ptr<Fifo> make_frame(const Fifo& dl) {
    auto f = std::make_unique<Fifo>();
    configure_pos_index16_nrm_index8_clr0_index8(*f, VtxFmt0);
    f->set_array(AttrPos, g_pos, sizeof(g_pos), 12, true);
    f->set_array(AttrNrm, g_nrm, sizeof(g_nrm), 12, true);
    f->set_array(AttrClr0, g_col, sizeof(g_col), 4, true);
    f->set_gen_mode(2, 1, 1);
    f->set_tex_coord_gen(0, TgMtx2x4, TgPos, 30);
    f->set_tex_coord_gen(1, TgMtx3x4, TgNrm, 33);
    const float t0[8] = {0.1f, 0.0f, 0.02f, 0.5f, 0.0f, 0.1f, 0.03f, 0.25f};
    const float t1[12] = {0.5f, 0.1f, 0.0f, 0.0f, 0.0f, 0.5f, 0.1f, 0.0f, 0.2f, 0.0f, 0.4f, 1.0f};
    const float post[12] = {0.5f, 0.0f, 0.0f, 0.5f, 0.0f, -0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f};
    f->load_tex_mtx(30, t0, 8);
    f->load_tex_mtx(33, t1, 12);
    f->load_tex_mtx(kPtTexMtx0 + 3, post, 12);
    const float m[12] = {0.9f, 0.1f, 0.0f, 1.0f, -0.1f, 0.9f, 0.2f, 2.0f, 0.0f, -0.2f, 1.0f, -5.0f};
    f->load_xf_f32(0, m, 12);
    f->set_chan_mat_color(0, 200, 150, 100, 255);
    f->set_chan_amb_color(0, 20, 30, 40, 255);
    f->set_chan_ctrl(0, true, SrcReg, SrcVtx, 0x03, DfClamp, AfSpot);
    f->load_light(0, 200, 180, 160, 255, 1.0f, 4.0f, 3.0f);
    f->load_light(1, 90, 90, 255, 255, -3.0f, 2.0f, 1.0f);
    f->call_dl(kDlAddr, dl.size());
    const float m2[12] = {0.8f, 0.0f, 0.3f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, -0.3f, 0.0f, 0.8f, -7.0f};
    f->load_xf_f32(0, m2, 12);
    f->set_chan_ctrl(0, true, SrcReg, SrcVtx, 0x03, DfClamp, AfSpot);
    f->set_tex_coord_gen(1, TgMtx3x4, TgNrm, 33);
    f->call_dl(kDlAddr, dl.size());
    std::uint8_t packet[4 * 4];
    for (unsigned v = 0; v < 4; ++v) {
        packet[v * 4 + 0] = 0;
        packet[v * 4 + 1] = packet[v * 4 + 2] = packet[v * 4 + 3] = static_cast<std::uint8_t>(v + 1);
    }
    f->draw(TriangleStrip, VtxFmt0, 4, packet, sizeof(packet));
    return f;
}

// One drawn vertex: clip position, the two colours and the texcoords, final values.
struct Out {
    float clip[4];
    std::uint32_t c0, c1;
    float tc[2][3];
};

// The CPU path's vertices (GPU lighting off: the packed colours are the lit ones).
bool collect_cpu(const Processor& p, std::vector<Out>& out) {
    PackedBatch pb{};
    if (!packed_batch(p, &pb)) return false;
    for (unsigned d = 0; d < p.draw_count; ++d) {
        const DrawCall& dc = p.draws[d];
        for (unsigned k = 0; k < dc.vertex_count; ++k) {
            const unsigned v = dc.first_vertex + k;
            const unsigned src = pb.remap[v] >= 0 ? static_cast<unsigned>(pb.remap[v]) : v;
            const std::uint8_t* b = pb.arena + pb.offset[src];
            Out o{};
            std::memcpy(o.clip, b, 16);
            std::memcpy(&o.c0, b + kPackOffColor0, 4);
            std::memcpy(&o.c1, b + kPackOffColor1, 4);
            const unsigned ntc = pack_ntc(dc.num_tex_gens);
            std::memcpy(o.tc, b + kPackOffTex, 12u * std::min(ntc, 2u));
            out.push_back(o);
        }
    }
    return true;
}

// With the GPU transform: transform draws through the shader's arithmetic from the GPU copy.
bool collect_xf(const Processor& p, const CachedVertex* gpu, std::vector<Out>& out, unsigned* xf_draws) {
    PackedBatch pb{};
    if (!packed_batch(p, &pb)) return false;
    for (unsigned d = 0; d < p.draw_count; ++d) {
        const DrawCall& dc = p.draws[d];
        GpuXfDraw xd{};
        const bool xf = draw_gpu_xf(p, d, &xd);
        const DrawExtra* x = draw_extra(p, d);
        *xf_draws += xf ? 1u : 0u;
        for (unsigned k = 0; k < dc.vertex_count; ++k) {
            Out o{};
            if (xf) {
                float tc[kMaxTexGens][3], eye[3], en[3];
                std::uint32_t r0, r1;
                gpu_xf_reference(*xd.params, gpu[xd.first + k], o.clip, tc, &r0, &r1, eye, en);
                gpu_lit_reference(x->lit, eye, en, r0, r1, &o.c0, &o.c1);
                std::memcpy(o.tc, tc, sizeof(o.tc));
            } else {
                const unsigned v = dc.first_vertex + k;
                const unsigned src = pb.remap[v] >= 0 ? static_cast<unsigned>(pb.remap[v]) : v;
                const std::uint8_t* b = pb.arena + pb.offset[src];
                std::memcpy(o.clip, b, 16);
                const bool lit = draw_gpu_lit(p, d);
                const unsigned ntc = pack_ntc(dc.num_tex_gens);
                std::memcpy(o.tc, b + kPackOffTex, 12u * std::min(ntc, 2u));
                std::uint32_t r0, r1;
                std::memcpy(&r0, b + kPackOffColor0, 4);
                std::memcpy(&r1, b + kPackOffColor1, 4);
                if (lit) {
                    float eye[3], en[3];
                    std::memcpy(eye, b + kPackOffTex + 12u * ntc, 12);
                    std::memcpy(en, b + kPackOffTex + 12u * ntc + 12u, 12);
                    gpu_lit_reference(x->lit, eye, en, r0, r1, &o.c0, &o.c1);
                } else {
                    o.c0 = r0;
                    o.c1 = r1;
                }
            }
            out.push_back(o);
        }
    }
    return true;
}

bool same_out(const std::vector<Out>& a, const std::vector<Out>& b, unsigned ntc_check, const char* what) {
    if (a.size() != b.size()) {
        std::fprintf(stderr, "%s: %zu vertices vs %zu\n", what, a.size(), b.size());
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (unsigned k = 0; k < 4; ++k) {
            if (!close(a[i].clip[k], b[i].clip[k])) {
                std::fprintf(stderr, "%s: vertex %zu clip[%u] %g vs %g\n", what, i, k, a[i].clip[k], b[i].clip[k]);
                return false;
            }
        }
        for (unsigned t = 0; t < ntc_check; ++t) {
            for (unsigned k = 0; k < 3; ++k) {
                if (!close(a[i].tc[t][k], b[i].tc[t][k])) {
                    std::fprintf(stderr, "%s: vertex %zu tc%u[%u] %g vs %g\n", what, i, t, k, a[i].tc[t][k], b[i].tc[t][k]);
                    return false;
                }
            }
        }
        if (byte_diff(a[i].c0, b[i].c0) > 1 || byte_diff(a[i].c1, b[i].c1) > 1) {
            std::fprintf(stderr, "%s: vertex %zu colours %08X/%08X vs %08X/%08X\n", what, i, a[i].c0, a[i].c1, b[i].c0,
                         b[i].c1);
            return false;
        }
    }
    return true;
}

bool run(Processor& p, const Fifo& f, const Fifo& dl) {
    p.reset();
    p.map_range(kDlAddr, dl.data(), dl.size());
    if (!p.process(f.data(), f.size(), true)) return false;
    vertex_worker_drain();
    return true;
}

// The GPU copy's overwrite protection, on the cache API directly: steady content is copied once
// and never waits; content that churns through the ring waits at most once a frame, and only
// after the ring has wrapped.
bool test_gpu_copy_sync() {
    const std::uint32_t pool_bytes = 3u * 2u * kMaxDecodedVerts * sizeof(CachedVertex);  // the minimum
    std::vector<std::uint8_t> pool(pool_bytes);
    std::vector<CachedVertex> gpu(pool_bytes / sizeof(CachedVertex));
    const std::uint32_t cap = pool_bytes / sizeof(CachedVertex);
    dl_cache_install(pool.data(), pool_bytes);
    dl_cache_install_gpu(gpu.data(), &gpu_sync);
    g_syncs = 0;
    auto frame = [&](std::uint32_t base_addr, unsigned n, std::uint32_t count, bool* ok) {
        DlCacheRef refs[16];
        for (unsigned i = 0; i < n; ++i) {
            DlCacheRef r{};
            dl_cache_lookup(base_addr + i * 64u, 0x1234, count, 0x55 + i, &r);
            if (r.data != nullptr) {
                for (std::uint32_t k = 0; k < count; ++k) {
                    r.data[k].x = static_cast<float>(base_addr + i * 64u + k);
                }
            }
            refs[i] = r;
        }
        dl_cache_batch_begin();
        for (unsigned i = 0; i < n; ++i) {
            DlCacheRef r{};
            if (dl_cache_lookup(base_addr + i * 64u, 0x1234, count, 0x55 + i, &r) != kDlHit) {
                std::fprintf(stderr, "gpu copy: no hit\n");
                *ok = false;
                return;
            }
            const std::uint32_t first = dl_cache_gpu_vertices(r, count);
            if (first == ~0u || gpu[first].x != static_cast<float>(base_addr + i * 64u) ||
                gpu[first + count - 1u].x != static_cast<float>(base_addr + i * 64u + count - 1u)) {
                std::fprintf(stderr, "gpu copy: wrong vertices\n");
                *ok = false;
                return;
            }
        }
        (void)refs;
        dl_cache_batch_begin();
        dl_cache_frame_mark();
    };
    bool ok = true;
    // Steady: the same ten entries every frame.
    for (int f = 0; f < 10 && ok; ++f) {
        frame(0x80100000u, 10, 1000, &ok);
    }
    const unsigned steady_copied = dl_cache_take_gpu_copied();
    if (!ok || g_syncs != 0 || steady_copied != 10000u) {
        std::fprintf(stderr, "gpu copy steady: %u syncs, %u copied\n", g_syncs, steady_copied);
        return false;
    }
    // Churn: new content every frame, ~40% of the ring a frame.
    const std::uint32_t per = cap / 25u;
    unsigned before_wrap_syncs = 0, max_per_frame = 0, frames_with_sync = 0;
    std::uint64_t allocated = 10000;
    for (int f = 0; f < 30 && ok; ++f) {
        const unsigned s0 = g_syncs;
        frame(0x81000000u + static_cast<std::uint32_t>(f) * 0x10000u, 10, per, &ok);
        allocated += 10ull * per;
        const unsigned d = g_syncs - s0;
        if (allocated <= cap) before_wrap_syncs += d;
        max_per_frame = std::max(max_per_frame, d);
        frames_with_sync += d != 0 ? 1u : 0u;
    }
    std::printf("gpu copy: churn %u frames waited (at most %u a frame), %u before the first wrap\n", frames_with_sync,
                max_per_frame, before_wrap_syncs);
    dl_cache_install(nullptr, 0);
    return ok && before_wrap_syncs == 0 && max_per_frame <= 1u && frames_with_sync > 0;
}

bool test_end_to_end() {
    fill_arrays();
    auto dl = make_dl();
    auto frame = make_frame(*dl);
    auto p = std::make_unique<Processor>();

    // Reference: CPU lighting, no cache.
    set_gpu_lighting(false);
    set_gpu_xf(false);
    dl_cache_install(nullptr, 0);
    std::vector<Out> ref;
    if (!run(*p, *frame, *dl) || !collect_cpu(*p, ref) || ref.empty()) return false;

    std::vector<std::uint8_t> pool(6u * 1024u * 1024u);
    std::vector<CachedVertex> gpu(pool.size() / sizeof(CachedVertex));
    set_gpu_lighting(true);
    set_gpu_xf(true);
    dl_cache_install(pool.data(), static_cast<std::uint32_t>(pool.size()));
    dl_cache_install_gpu(gpu.data(), &gpu_sync);
    unsigned prims = 0;
    for (int f = 0; f < 6; ++f) {
        if (!run(*p, *frame, *dl)) return false;
        std::vector<Out> got;
        unsigned xf_draws = 0;
        if (!collect_xf(*p, gpu.data(), got, &xf_draws)) return false;
        char what[48];
        std::snprintf(what, sizeof(what), "gpu transform frame %d", f);
        if (!same_out(ref, got, 2, what)) return false;
        prims = p->draw_count;
        if (f == 0 && xf_draws != 0) {
            std::fprintf(stderr, "%s: %u transform draws on the filling frame\n", what, xf_draws);
            return false;
        }
        if (f >= 1 && xf_draws != prims - 1u) {
            // Every DL primitive (both calls), not the one straight in the FIFO.
            std::fprintf(stderr, "%s: %u of %u draws used the transform\n", what, xf_draws, prims);
            return false;
        }
        dl_cache_frame_mark();
    }
    const unsigned copied = dl_cache_take_gpu_copied();
    if (g_syncs != 0 || copied == 0) {
        std::fprintf(stderr, "gpu transform: %u syncs, %u vertices copied\n", g_syncs, copied);
        return false;
    }
    // Copied once each: later frames copy nothing.
    if (!run(*p, *frame, *dl) || dl_cache_take_gpu_copied() != 0) {
        std::fprintf(stderr, "gpu transform: steady frame copied again\n");
        return false;
    }
    std::printf("gpu_xf end to end: %u draws a frame, %u vertices copied once\n", prims, copied);
    dl_cache_install(nullptr, 0);
    dl_cache_install_gpu(nullptr, nullptr);
    set_gpu_xf(false);
    set_gpu_lighting(false);
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok = test_reference() && ok;
    ok = test_emit() && ok;
    ok = test_end_to_end() && ok;
    ok = test_gpu_copy_sync() && ok;
    std::printf("%s\n", ok ? "gx_gpu_xf_tests: OK" : "gx_gpu_xf_tests: FAILED");
    return ok ? 0 : 1;
}
