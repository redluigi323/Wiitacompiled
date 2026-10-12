// Emits random GPU-transform cases as JSON (for tools/gpu_xf_check/run_cases.py): the Cg vertex program, its uniform values, vertices and
// the C++ reference outputs (gpu_xf_reference + gpu_lit_reference).
#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/draw_state.hpp"
#include <cstdio>
#include <string>
using namespace mkwv::gx;
static unsigned rs = 99;
static unsigned rnd() { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static float sc() { return ((int)(rnd() % 2001) - 1000) / 113.0f; }
static std::string esc(const char* s) { std::string o; for (; *s; ++s) { if (*s == '"') o += "\\\""; else if (*s == '\n') o += "\\n"; else if (*s=='\\') o += "\\\\"; else o += *s; } return o; }
static void arr(FILE* f, const char* name, const float* v, unsigned n, bool& first) {
    fprintf(f, "%s\"%s\":[", first ? "" : ",", name); first = false;
    for (unsigned i = 0; i < n; ++i) fprintf(f, "%s%.9g", i ? "," : "", v[i]); fprintf(f, "]");
}
int main(int argc, char** argv) {
    int ncases = argc > 1 ? atoi(argv[1]) : 60;
    set_gpu_lighting(true); set_gpu_xf(true);
    FILE* f = stdout; fprintf(f, "[");
    int written = 0;
    for (int k = 0; written < ncases; ++k) {
        State st; st.reset();
        st.num_chans = 1 + rnd() % 2;
        st.vtx_desc[AttrPos] = AttrDirect; st.vtx_fmts[0].attrs[AttrPos].cnt = PosXYZ;
        const bool skinned = rnd() & 1;
        st.vtx_desc[AttrPnMtxIdx] = skinned ? AttrDirect : AttrNone;
        st.vtx_desc[AttrNrm] = AttrDirect; st.vtx_fmts[0].attrs[AttrNrm].cnt = NrmXYZ;
        st.vtx_desc[AttrClr0] = (rnd() & 1) ? AttrDirect : AttrNone;
        st.vtx_desc[AttrClr1] = (rnd() & 1) ? AttrDirect : AttrNone;
        st.vtx_desc[AttrTex0] = AttrDirect; st.vtx_desc[AttrTex1] = AttrDirect;
        st.current_pn_mtx = rnd() % kMaxPnMtx;
        for (auto& c : st.chans) { c.lighting = rnd() % 4 != 0; c.light_mask = rnd() & 0xFF; c.mat_src = rnd() & 1 ? SrcReg : SrcVtx; c.amb_src = rnd() & 1 ? SrcReg : SrcVtx; c.diff_fn = (DiffuseFn)(rnd() % 3); c.attn_fn = (AttnFn)(rnd() % 3); }
        for (auto& l : st.lights) { l.color = rnd() * 2654435761u; for (int c = 0; c < 3; ++c) { l.pos[c] = sc(); l.dir[c] = sc(); l.cos_att[c] = sc(); l.dist_att[c] = 1.0f + (rnd() % 100) / 100.0f; } }
        for (int c = 0; c < 2; ++c) { st.chan_mat[c] = rnd() * 2654435761u; st.ambient[c] = rnd() * 40503u; }
        for (auto& m : st.pn_mtx) for (auto& c : m) c = sc();
        for (auto& m : st.nrm_mtx) for (auto& c : m) c = sc();
        for (auto& r : st.proj) for (auto& c : r) c = sc();
        for (auto& c : st.xf_mem) c = sc();
        for (auto& c : st.xf_dtt) c = sc();
        st.dual_tex = rnd() & 1;
        st.num_tex_gens = rnd() % 9;
        static const TexGenSrc kSrc[4] = {TgPos, TgNrm, TgTex0, TgTex1};
        for (unsigned i = 0; i < st.num_tex_gens; ++i) {
            TexGen& g = st.texgens[i]; g.type = (TexGenType)(rnd() % 3); g.src = kSrc[rnd() % 4];
            g.mtx = rnd() % 8 == 0 ? kTexMtxIdentity : rnd() % 62; g.normalize = rnd() & 1;
            const unsigned post = rnd() % 6; g.post_mtx = post == 0 ? kPtTexMtxIdentity : kPtTexMtx0 + rnd() % 61;
            g.form_ab11 = rnd() % 5 == 0;
            if (post == 1) { unsigned slot = (g.post_mtx - kPtTexMtx0) * 4u; if (slot + 12 <= kXfDttSize) for (unsigned q = 8; q < 12; ++q) st.xf_dtt[slot + q] = 0.0f; }
        }
        VertexDecoder dec; dec.prepare(st, VtxFmt0, true);
        if (!gpu_xf_eligible(st, dec)) continue;
        float su[kMaxTexGens][2]; const bool su_any = rnd() & 1;
        for (auto& c : su) { c[0] = su_any ? 0.25f + (rnd() % 8) / 4.0f : 1; c[1] = su_any ? 0.25f + (rnd() % 8) / 4.0f : 1; }
        PreparedLighting prep; prepare_lighting(st, prep);
        static GpuXfParams xp; fill_gpu_xf_params(st, dec, su, su_any, xp);
        GpuLitParams lit; fill_gpu_lit_params(st, prep, lit);
        ShaderNeeds needs; needs.num_tex_gens = st.num_tex_gens;
        for (unsigned c = 0; c < st.num_tex_gens; ++c) { needs.coord_used[c] = rnd() % 4 != 0; needs.coord_proj[c] = st.texgens[c].type == TgMtx3x4; }
        needs.color0 = rnd() % 4 != 0; needs.color1 = rnd() % 2; needs.fog = false;
        static char vs[32768]; size_t n;
        if (!emit_tev_vertex_cg_xf(needs, lit, skinned, vs, sizeof vs, &n)) { fprintf(stderr, "emit fail\n"); return 1; }
        fprintf(f, "%s{\"vs\":\"%s\",\"skinned\":%d,\"color0\":%d,\"color1\":%d,\"used\":[", written ? "," : "", esc(vs).c_str(), skinned, needs.color0, needs.color1);
        for (unsigned c = 0; c < 8; ++c) fprintf(f, "%s%d", c ? "," : "", needs.coord_used[c] ? (needs.coord_proj[c] ? 2 : 1) : 0);
        fprintf(f, "],\"u\":{");
        bool first = true;
        arr(f, "uPn", &xp.pn[0][0], xp.num_pn * 12u, first);
        arr(f, "uNm", &xp.nrm[0][0], xp.num_pn * 12u, first);
        arr(f, "uProj", &xp.proj[0][0], 16, first);
        arr(f, "uXf", xp.misc, 4, first);
        char nm[16];
        for (unsigned c = 0; c < 8; ++c) { if (!needs.coord_used[c]) continue;
            snprintf(nm, 16, "uTgS%u", c); arr(f, nm, xp.tg_src[c], 4, first);
            snprintf(nm, 16, "uTgM%u", c); arr(f, nm, &xp.tg_mtx[c][0][0], 12, first);
            snprintf(nm, 16, "uTgP%u", c); arr(f, nm, &xp.tg_post[c][0][0], 12, first);
            snprintf(nm, 16, "uTgF%u", c); arr(f, nm, xp.tg_flags[c], 4, first);
            snprintf(nm, 16, "uTgK%u", c); arr(f, nm, xp.tg_scale[c], 4, first); }
        for (unsigned c = 0; c < 2; ++c) { snprintf(nm, 16, "uMat%u", c); arr(f, nm, lit.mat[c], 4, first); snprintf(nm, 16, "uAmb%u", c); arr(f, nm, lit.amb[c], 4, first); }
        for (unsigned li = 0; li < kMaxLights; ++li) {
            snprintf(nm, 16, "uLP%u", li); arr(f, nm, lit.pos[li], 4, first); snprintf(nm, 16, "uLD%u", li); arr(f, nm, lit.dir[li], 4, first);
            snprintf(nm, 16, "uLS%u", li); arr(f, nm, lit.spec[li], 4, first); snprintf(nm, 16, "uLC%u", li); arr(f, nm, lit.col[li], 4, first);
            snprintf(nm, 16, "uLA%u", li); arr(f, nm, lit.cosa[li], 4, first); snprintf(nm, 16, "uLK%u", li); arr(f, nm, lit.dista[li], 4, first); }
        fprintf(f, "},\"v\":[");
        for (int vi = 0; vi < 16; ++vi) {
            CachedVertex cv{}; cv.x = sc(); cv.y = sc(); cv.z = sc(); cv.nx = sc(); cv.ny = sc(); cv.nz = sc();
            cv.tc[0][0] = sc(); cv.tc[0][1] = sc(); cv.tc[1][0] = sc(); cv.tc[1][1] = sc();
            cv.color = rnd() * 2654435761u; cv.color1 = rnd() * 40503u; cv.pn_mtx = rnd() % 12;
            float clip[4], tc[8][3], eye[3], en[3]; uint32_t r0, r1, c0, c1;
            gpu_xf_reference(xp, cv, clip, tc, &r0, &r1, eye, en);
            gpu_lit_reference(lit, eye, en, r0, r1, &c0, &c1);
            fprintf(f, "%s{\"a\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],\"c0\":%u,\"c1\":%u,\"m\":%u,\"clip\":[%.9g,%.9g,%.9g,%.9g],\"col\":[%u,%u],\"tc\":[",
                    vi ? "," : "", cv.x, cv.y, cv.z, cv.nx, cv.ny, cv.nz, cv.tc[0][0], cv.tc[0][1], cv.tc[1][0], cv.tc[1][1], cv.color, cv.color1, cv.pn_mtx,
                    clip[0], clip[1], clip[2], clip[3], c0, c1);
            for (int c = 0; c < 8; ++c) fprintf(f, "%s[%.9g,%.9g,%.9g]", c ? "," : "", tc[c][0], tc[c][1], tc[c][2]);
            fprintf(f, "]}");
        }
        fprintf(f, "]}");
        ++written;
    }
    fprintf(f, "]\n");
}
