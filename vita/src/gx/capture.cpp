#include "mkwv/gx/capture.hpp"

#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/efb_copy.hpp"
#include "mkwv/gx/fifo.hpp"
#include "mkwv/gx/texture.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>

#if defined(__vita__)
#include <psp2/io/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace mkwv::gx {
namespace {

// Format of <dir>/frame.txt (one record per line, space separated, all integers decimal
// unless noted; floats with %.9g):
//
//   MKWVCAP 1
//   BATCH <index> <draws> <verts> <verts file>
//   COPY <draw_index> <src x y w h> <dst w h> <format> <clear> <clear_rgba hex> <dest_guest hex>
//   DRAW <batch> <index> <prim> <first_vertex> <vertex_count>
//   BLEND <mode> <src> <dst> <logic>
//   PE <depth_test> <depth_write> <depth_func> <cull> <color_update> <alpha_update> <pixel_fmt>
//      <dst_alpha_enable> <dst_alpha> <zcomploc>
//   RAST <has_vp> <vp x y w h near far> <has_rscissor> <rsc x y w h> <has_scissor> <sc x y w h>
//        <off x y>
//   GEN <num_tev_stages> <num_tex_gens> <num_chans> <num_ind_stages> <tg_type x8>
//   STAGE <n> <35 TevStage fields in declaration order>
//   IND <n> <texmap> <texcoord> <scale_s> <scale_t>
//   IMTX <n> <m0..m5> <scale_exp>
//   REGS <tev_reg 16 signed> <konst 4 hex> <swap 16>
//   ACMP <comp0> <ref0> <comp1> <ref1> <op>
//   FOG <type> <color hex> <a> <b> <c> <raw0 hex> <raw1 hex> <raw2 hex>
//   TEX <map> <w> <h> <format> <wrap_s> <wrap_t> <min> <mag> <mips> <min_lod> <max_lod> <lod_bias>
//       <phys hex> <tlut_tmem> <tlut_fmt> <tlut_entries> <file or ->
//   END
//
// <verts file> holds, per vertex: float x y z w (GL clip space), u32 color0, u32 color1
// (r | g<<8 | b<<16 | a<<24), float tc[8][3]. Texture files are raw RGBA8 of level 0.

constexpr unsigned kMaxTexFiles = 256;
constexpr std::uint32_t kScratchBytes = 1024u * 1024u * 4u * 2u;

struct TexKey {
    const std::uint8_t* data = nullptr;
    std::uint32_t w = 0, h = 0, fmt = 0;
    const std::uint8_t* tlut = nullptr;
    std::uint32_t tlut_fmt = 0, tlut_version = 0;
};

struct Session {
    bool armed = false;
    bool active = false;
    char dir[160]{};
    std::FILE* txt = nullptr;
    unsigned batch = 0;
    unsigned draws = 0;
    unsigned verts = 0;
    unsigned textures = 0;
    unsigned tex_fail = 0;
    TexKey keys[kMaxTexFiles];
    std::uint8_t* scratch = nullptr;
};

Session g_cap;
char g_summary[256] = "no capture";

void make_dir(const char* path) {
#if defined(__vita__)
    sceIoMkdir(path, 0777);
#else
    mkdir(path, 0755);
#endif
}

void make_dirs(const char* path) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s", path);
    for (char* p = buf; *p != '\0'; ++p) {
        if (*p == '/' && p != buf && p[-1] != ':') {
            *p = '\0';
            make_dir(buf);
            *p = '/';
        }
    }
    make_dir(buf);
}

// Index of the texture file for this texobj, writing it on first use. -1 when it cannot be
// decoded (no data, an unknown format, or an EFB copy the decoder only knows by address).
int texture_file(const TexObj& tex, const TlutObj* tlut) {
    if (tex.data == nullptr || tex.width == 0 || tex.height == 0 || tex.width > 1024 || tex.height > 1024) {
        return -1;
    }
    TexKey k;
    k.data = tex.data;
    k.w = tex.width;
    k.h = tex.height;
    k.fmt = tex.format;
    k.tlut = tlut != nullptr ? tlut->data : nullptr;
    k.tlut_fmt = tlut != nullptr ? tlut->format : 0u;
    k.tlut_version = tlut != nullptr ? tlut->version : 0u;
    for (unsigned i = 0; i < g_cap.textures; ++i) {
        const TexKey& e = g_cap.keys[i];
        if (e.data == k.data && e.w == k.w && e.h == k.h && e.fmt == k.fmt && e.tlut == k.tlut &&
            e.tlut_fmt == k.tlut_fmt && e.tlut_version == k.tlut_version) {
            return static_cast<int>(i);
        }
    }
    if (g_cap.textures >= kMaxTexFiles || g_cap.scratch == nullptr) {
        return -1;
    }
    TexObj one = tex;
    one.mips = false;  // level 0 is enough to look at
    ConvertedTexture out{};
    if (!convert_gx_texture(one, g_cap.scratch, kScratchBytes, &out, tlut) || out.data == nullptr) {
        ++g_cap.tex_fail;
        return -1;
    }
    const unsigned idx = g_cap.textures;
    char path[200];
    std::snprintf(path, sizeof(path), "%s/tex_%03u.rgba", g_cap.dir, idx);
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        ++g_cap.tex_fail;
        return -1;
    }
    std::fwrite(out.data, 1, static_cast<std::size_t>(out.width) * out.height * 4u, f);
    std::fclose(f);
    g_cap.keys[idx] = k;
    ++g_cap.textures;
    return static_cast<int>(idx);
}

void write_draw(const Processor& p, std::uint32_t i) {
    std::FILE* f = g_cap.txt;
    const DrawCall& d = p.draws[i];
    const DrawExtra* x = draw_extra(p, i);
    std::fprintf(f, "DRAW %u %u %u %u %u\n", g_cap.batch, i, static_cast<unsigned>(d.prim), d.first_vertex,
                 d.vertex_count);
    std::fprintf(f, "BLEND %u %u %u %u\n", static_cast<unsigned>(d.blend_mode), static_cast<unsigned>(d.blend_src),
                 static_cast<unsigned>(d.blend_dst), static_cast<unsigned>(d.logic_op));
    const State& s = p.state;
    const bool dt = x != nullptr ? x->depth_test : s.depth_test;
    const bool dw = x != nullptr ? x->depth_write : s.depth_write;
    const unsigned df = x != nullptr ? static_cast<unsigned>(x->depth_func) : static_cast<unsigned>(s.depth_func);
    const unsigned cull = x != nullptr ? static_cast<unsigned>(x->cull_mode) : static_cast<unsigned>(s.cull_mode);
    const bool cu = x != nullptr ? x->color_update : s.color_update;
    const bool au = x != nullptr ? x->alpha_update : s.alpha_update;
    std::fprintf(f, "PE %d %d %u %u %d %d %u %d %u %d\n", dt ? 1 : 0, dw ? 1 : 0, df, cull, cu ? 1 : 0, au ? 1 : 0,
                 static_cast<unsigned>(d.pixel_fmt), d.dst_alpha_enable ? 1 : 0, static_cast<unsigned>(d.dst_alpha),
                 d.zcomploc ? 1 : 0);
    if (x != nullptr) {
        std::fprintf(f, "RAST %d %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d %d %d %d %d %d %d %d %d %d\n",
                     x->has_render_vp ? 1 : 0, x->render_vp[0], x->render_vp[1], x->render_vp[2], x->render_vp[3],
                     x->render_vp[4], x->render_vp[5], x->has_render_scissor ? 1 : 0, x->render_scissor[0],
                     x->render_scissor[1], x->render_scissor[2], x->render_scissor[3], x->has_scissor ? 1 : 0,
                     x->scissor_x, x->scissor_y, x->scissor_w, x->scissor_h, x->scissor_off_x, x->scissor_off_y);
    }
    const unsigned nind = x != nullptr ? x->num_ind_stages : s.num_ind_stages;
    std::fprintf(f, "GEN %u %u %u %u", static_cast<unsigned>(d.num_tev_stages), static_cast<unsigned>(d.num_tex_gens),
                 static_cast<unsigned>(d.num_chans), nind);
    for (unsigned t = 0; t < kMaxTexGens; ++t) {
        std::fprintf(f, " %u", static_cast<unsigned>(x != nullptr ? x->tg_type[t] : s.texgens[t].type));
    }
    std::fputc('\n', f);
    const unsigned stages = d.num_tev_stages == 0 ? 1u : d.num_tev_stages;
    for (unsigned k = 0; k < stages && k < kTevStages; ++k) {
        const TevStage& t = d.tev_stages[k];
        std::fprintf(f,
                     "STAGE %u %u %u %u %u %u %u %u %u %u %u %u %u %d %u %u %u %u %d %u %u %u %u %u %u %u %d %u %u %u "
                     "%u %u %u %d %u %d\n",
                     k, t.color_a, t.color_b, t.color_c, t.color_d, t.alpha_a, t.alpha_b, t.alpha_c, t.alpha_d,
                     t.color_bias, t.color_op, t.color_shift, t.color_dest, t.color_clamp ? 1 : 0, t.alpha_bias,
                     t.alpha_op, t.alpha_shift, t.alpha_dest, t.alpha_clamp ? 1 : 0, t.kcsel, t.kasel, t.ras_swap,
                     t.tex_swap, t.texmap, t.texcoord, t.ras_chan, t.tex_enabled ? 1 : 0, t.ind_stage, t.ind_fmt,
                     t.ind_bias, t.ind_mtx, t.ind_wrap_s, t.ind_wrap_t, t.ind_add_prev ? 1 : 0, t.ind_alpha_sel,
                     t.ind_utc_lod ? 1 : 0);
    }
    for (unsigned k = 0; k < kMaxIndStages; ++k) {
        const IndStage& is = x != nullptr ? x->ind[k] : s.ind[k];
        std::fprintf(f, "IND %u %u %u %u %u\n", k, is.texmap, is.texcoord, is.scale_s, is.scale_t);
    }
    for (unsigned k = 0; k < kMaxIndMtx; ++k) {
        const IndMatrix& m = x != nullptr ? x->ind_mtx[k] : s.ind_mtx[k];
        std::fprintf(f, "IMTX %u %.9g %.9g %.9g %.9g %.9g %.9g %d\n", k, m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5],
                     static_cast<int>(m.scale_exp));
    }
    std::fprintf(f, "REGS");
    for (unsigned r = 0; r < 4; ++r) {
        for (unsigned c = 0; c < 4; ++c) {
            std::fprintf(f, " %d", static_cast<int>(d.tev_reg[r][c]));
        }
    }
    for (unsigned r = 0; r < 4; ++r) {
        std::fprintf(f, " %08X", d.konst[r]);
    }
    for (unsigned r = 0; r < 4; ++r) {
        for (unsigned c = 0; c < 4; ++c) {
            std::fprintf(f, " %u", static_cast<unsigned>(d.tev_swap[r][c]));
        }
    }
    std::fputc('\n', f);
    std::fprintf(f, "ACMP %u %u %u %u %u\n", static_cast<unsigned>(d.alpha_comp0), static_cast<unsigned>(d.alpha_ref0),
                 static_cast<unsigned>(d.alpha_comp1), static_cast<unsigned>(d.alpha_ref1),
                 static_cast<unsigned>(d.alpha_op));
    std::fprintf(f, "FOG %u %08X %.9g %.9g %.9g %08X %08X %08X\n", static_cast<unsigned>(d.fog.type), d.fog.color,
                 d.fog.a, d.fog.b, d.fog.c, d.fog.raw0, d.fog.raw1, d.fog.raw2);
    // Every texmap a stage (or an indirect stage) of this draw can read.
    bool used[kMaxTexMaps]{};
    for (unsigned k = 0; k < stages && k < kTevStages; ++k) {
        if (d.tev_stages[k].tex_enabled && d.tev_stages[k].texmap < kMaxTexMaps) {
            used[d.tev_stages[k].texmap] = true;
        }
    }
    for (unsigned k = 0; k < nind && k < kMaxIndStages; ++k) {
        const IndStage& is = x != nullptr ? x->ind[k] : s.ind[k];
        if (is.texmap < kMaxTexMaps) {
            used[is.texmap] = true;
        }
    }
    for (unsigned m = 0; m < kMaxTexMaps; ++m) {
        if (!used[m]) {
            continue;
        }
        const TexObj& t = x != nullptr ? x->tex[m] : s.texmaps[m];
        const TlutObj* tl = nullptr;
        if (x != nullptr && x->tlut_valid[m]) {
            tl = &x->tlut[m];
        }
        const int file = texture_file(t, tl);
        std::fprintf(f, "TEX %u %u %u %u %u %u %u %u %d %u %u %d %08X %u %u %u ", m, t.width, t.height, t.format,
                     static_cast<unsigned>(t.wrap_s), static_cast<unsigned>(t.wrap_t),
                     static_cast<unsigned>(t.min_filter), static_cast<unsigned>(t.mag_filter), t.mips ? 1 : 0,
                     static_cast<unsigned>(t.min_lod), static_cast<unsigned>(t.max_lod), static_cast<int>(t.lod_bias),
                     t.phys, static_cast<unsigned>(t.tlut_tmem), static_cast<unsigned>(tl != nullptr ? tl->format : 0u),
                     static_cast<unsigned>(tl != nullptr ? tl->entries : 0u));
        if (file >= 0) {
            std::fprintf(f, "tex_%03d.rgba\n", file);
        } else {
            std::fprintf(f, "-\n");
        }
    }
    std::fprintf(f, "END\n");
}

} // namespace

void capture_arm(const char* dir) {
    if (g_cap.active || dir == nullptr) {
        return;
    }
    std::snprintf(g_cap.dir, sizeof(g_cap.dir), "%s", dir);
    g_cap.armed = true;
}

bool capture_armed() {
    return g_cap.armed;
}

const char* capture_directory() { return g_cap.dir; }
unsigned capture_batch_count() { return g_cap.batch; }

bool capture_active() {
    return g_cap.active;
}

void capture_batch(const Processor& p) {
    if (!g_cap.armed && !g_cap.active) {
        return;
    }
    if (!g_cap.active) {
        make_dirs(g_cap.dir);
        char path[200];
        std::snprintf(path, sizeof(path), "%s/frame.txt", g_cap.dir);
        g_cap.txt = std::fopen(path, "w");
        if (g_cap.txt == nullptr) {
            g_cap.armed = false;
            std::snprintf(g_summary, sizeof(g_summary), "capture: cannot write %s (errno %d)", path, errno);
            return;
        }
        g_cap.scratch = new (std::nothrow) std::uint8_t[kScratchBytes];
        g_cap.active = true;
        g_cap.armed = false;
        g_cap.batch = 0;
        g_cap.draws = 0;
        g_cap.verts = 0;
        g_cap.textures = 0;
        g_cap.tex_fail = 0;
        std::fprintf(g_cap.txt, "MKWVCAP 1\n");
    }
    char vname[32];
    std::snprintf(vname, sizeof(vname), "verts_%u.bin", g_cap.batch);
    char vpath[200];
    std::snprintf(vpath, sizeof(vpath), "%s/%s", g_cap.dir, vname);
    if (std::FILE* vf = std::fopen(vpath, "wb")) {
        for (std::uint32_t v = 0; v < p.vert_count; ++v) {
            const DecodedVertex& dv = p.verts[v];
            const float pos[4] = {dv.x, dv.y, dv.z, dv.w};
            std::fwrite(pos, sizeof(float), 4, vf);
            std::fwrite(&dv.color, 4, 1, vf);
            std::fwrite(&dv.color1, 4, 1, vf);
            std::fwrite(dv.tc, sizeof(float), kMaxTexGens * 3u, vf);
        }
        std::fclose(vf);
    }
    std::fprintf(g_cap.txt, "BATCH %u %u %u %s\n", g_cap.batch, p.draw_count, p.vert_count, vname);
    const EfbCopyEvent* ev = nullptr;
    const unsigned nev = efb_copy_events(&ev);
    for (unsigned e = 0; e < nev; ++e) {
        std::fprintf(g_cap.txt, "COPY %u %d %d %d %d %u %u %u %d %08X %08X %u %u %u %08X %u\n", ev[e].draw_index, ev[e].src_x,
                     ev[e].src_y, ev[e].src_w, ev[e].src_h, ev[e].dst_w, ev[e].dst_h, ev[e].format, ev[e].clear ? 1 : 0,
                     ev[e].clear_rgba, ev[e].dest_guest, ev[e].clear_color, ev[e].clear_alpha, ev[e].clear_depth,
                     ev[e].clear_z, ev[e].pixel_fmt);
    }
    for (std::uint32_t i = 0; i < p.draw_count; ++i) {
        write_draw(p, i);
    }
    g_cap.draws += p.draw_count;
    g_cap.verts += p.vert_count;
    ++g_cap.batch;
    std::fflush(g_cap.txt);
}

bool capture_frame_end() {
    if (!g_cap.active) {
        return false;
    }
    std::fprintf(g_cap.txt, "FRAME_END %u %08X\n", g_cap.batch, efb_clear_color());
    std::fclose(g_cap.txt);
    g_cap.txt = nullptr;
    delete[] g_cap.scratch;
    g_cap.scratch = nullptr;
    g_cap.active = false;
    std::snprintf(g_summary, sizeof(g_summary), "capture: %s batches=%u draws=%u verts=%u textures=%u texfail=%u",
                  g_cap.dir, g_cap.batch, g_cap.draws, g_cap.verts, g_cap.textures, g_cap.tex_fail);
    return true;
}

void capture_gpu_image(const char* label, const std::uint8_t* rgba, unsigned width, unsigned height) {
    if (!g_cap.active || !rgba || !width || !height) return;
    char path[256];
    std::snprintf(path, sizeof(path), "%s/gpu_b%u_%s.ppm", g_cap.dir, g_cap.batch - 1, label);
    if (std::FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", width, height);
        std::uint8_t row[4096 * 3];
        if (width <= 4096) for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x)
                std::memcpy(row + x * 3, rgba + (y * width + x) * 4, 3);
            std::fwrite(row, 3, width, f);
        }
        std::fclose(f);
    }
}

const char* capture_last_summary() {
    return g_summary;
}

} // namespace mkwv::gx
