#include "mkwv/gx/submit.hpp"
#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/demo.hpp"
#include "mkwv/gx/texture.hpp"
#include "mkwv/gxm/device.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mkwv::gx {
namespace {

// Textures are expanded to RGBA8 before upload, so the scratch buffer has to hold width*height*4
// plus mips. The strap image alone is 832x456 = 1.45 MB expanded, which overflowed the old 1 MiB
// buffer; convert_gx_texture then returned false, nothing was ever bound, and the textured quad drew
// with no texture at all - silently, which is what made this cost so long to find. Allocated lazily
// off the heap rather than sitting in .bss.
constexpr std::uint32_t kConvertedCapacity = 8u * 1024u * 1024u;

// Opt in while diagnosing the device; these draws otherwise cover guest output.
constexpr bool kDrawDeviceProbes = false;

// Everything in submit_processor that can silently drop a draw is recorded here and printed by the
// watchdog, because each silent failure otherwise costs a full rebuild-and-run cycle to locate.
struct SubmitDebug {
    std::uint32_t draws = 0;
    std::uint32_t nidx0 = 0;
    int bucket0 = -1;
    std::uint32_t rep_v = 0, rep_i = 0, mod_v = 0, mod_i = 0, col_v = 0, alp_v = 0, tevk = 0;
    int early_out = 0;
    int tex_convert = -1;
    int tex_bind = -1;
    std::uint32_t tex_w = 0, tex_h = 0, tex_bytes = 0;
    int tex_layout = -1, tex_mips = 0;
    int r_replace = -1, r_modulate = -1, r_color = -1, r_alpha = -1, r_tev = -1;
    int depth_test = -1, depth_write = -1, depth_func = -1, cull = -1;
    int probe = -1;
    int probe2 = -1;
    int e_scene = 0, e_clear = 0, e_prog = 0, e_stream = 0, e_tex = 0, e_draw = 0;
    int e_vunif = 0, e_vunull = 0, e_pclear = 0, e_pres = 0, e_pnull = 0;
};

SubmitDebug g_dbg;
char g_dbg_text[512];

std::uint8_t* converted_buffer() {
    static std::uint8_t* buffer = static_cast<std::uint8_t*>(std::malloc(kConvertedCapacity));
    return buffer;
}

// GX clip space puts the near plane at z = -1 and the far plane at z = 0; sceGxm clips at
// 0 <= z <= w. Feeding GX's z through unchanged put every vertex in front of the camera outside
// the clip volume, so the guest's draws were accepted by GXM and produced no pixels. The demo was
// unaffected only because its projection had been hand-written in GXM's range.
inline float gx_z_to_gxm(float z) {
    return z + 1.0f;
}

struct MeshBucket {
    gxm::ColorVertex color[kMaxDecodedVerts];
    gxm::TexturedVertex tex[kMaxDecodedVerts];
    gxm::TevVertex tev[kMaxDecodedVerts];
    std::uint16_t idx[kMaxDecodedVerts * 3];
    std::uint32_t color_verts = 0;
    std::uint32_t tex_verts = 0;
    std::uint32_t tev_verts = 0;
    std::uint32_t indices = 0;
};

void clear_bucket(MeshBucket& bucket) {
    std::memset(&bucket, 0, sizeof(bucket));
}

bool append_tex(MeshBucket& bucket, const Processor& processor, const DrawCall& draw, const std::uint16_t* local,
                std::uint32_t nidx) {
    if (bucket.tex_verts + draw.vertex_count > kMaxDecodedVerts || bucket.indices + nidx > kMaxDecodedVerts * 3) {
        return false;
    }
    for (std::uint16_t v = 0; v < draw.vertex_count; ++v) {
        const DecodedVertex& src = processor.verts[draw.first_vertex + v];
        bucket.tex[bucket.tex_verts + v].x = src.x;
        bucket.tex[bucket.tex_verts + v].y = src.y;
        bucket.tex[bucket.tex_verts + v].z = gx_z_to_gxm(src.z);
        bucket.tex[bucket.tex_verts + v].u = src.u;
        bucket.tex[bucket.tex_verts + v].v = src.v;
        bucket.color[bucket.tex_verts + v].x = src.x;
        bucket.color[bucket.tex_verts + v].y = src.y;
        bucket.color[bucket.tex_verts + v].z = gx_z_to_gxm(src.z);
        bucket.color[bucket.tex_verts + v].color = src.color;
    }
    for (std::uint32_t i = 0; i < nidx; ++i) {
        bucket.idx[bucket.indices + i] = static_cast<std::uint16_t>(bucket.tex_verts + local[i]);
    }
    bucket.tex_verts += draw.vertex_count;
    bucket.color_verts = bucket.tex_verts;
    bucket.indices += nidx;
    return true;
}

bool append_tev(MeshBucket& bucket, const Processor& processor, const DrawCall& draw, const std::uint16_t* local,
                std::uint32_t nidx) {
    if (bucket.tev_verts + draw.vertex_count > kMaxDecodedVerts || bucket.indices + nidx > kMaxDecodedVerts * 3) {
        return false;
    }
    for (std::uint16_t v = 0; v < draw.vertex_count; ++v) {
        const DecodedVertex& src = processor.verts[draw.first_vertex + v];
        bucket.tev[bucket.tev_verts + v].x = src.x;
        bucket.tev[bucket.tev_verts + v].y = src.y;
        bucket.tev[bucket.tev_verts + v].z = gx_z_to_gxm(src.z);
        bucket.tev[bucket.tev_verts + v].color = src.color;
        bucket.tev[bucket.tev_verts + v].u = src.u;
        bucket.tev[bucket.tev_verts + v].v = src.v;
        bucket.tev[bucket.tev_verts + v].u1 = src.u1;
        bucket.tev[bucket.tev_verts + v].v1 = src.v1;
    }
    for (std::uint32_t i = 0; i < nidx; ++i) {
        bucket.idx[bucket.indices + i] = static_cast<std::uint16_t>(bucket.tev_verts + local[i]);
    }
    bucket.tev_verts += draw.vertex_count;
    bucket.indices += nidx;
    return true;
}

bool append_color(MeshBucket& bucket, const Processor& processor, const DrawCall& draw, const std::uint16_t* local,
                  std::uint32_t nidx) {
    if (bucket.color_verts + draw.vertex_count > kMaxDecodedVerts || bucket.indices + nidx > kMaxDecodedVerts * 3) {
        return false;
    }
    for (std::uint16_t v = 0; v < draw.vertex_count; ++v) {
        const DecodedVertex& src = processor.verts[draw.first_vertex + v];
        bucket.color[bucket.color_verts + v].x = src.x;
        bucket.color[bucket.color_verts + v].y = src.y;
        bucket.color[bucket.color_verts + v].z = gx_z_to_gxm(src.z);
        bucket.color[bucket.color_verts + v].color = src.color;
    }
    for (std::uint32_t i = 0; i < nidx; ++i) {
        bucket.idx[bucket.indices + i] = static_cast<std::uint16_t>(bucket.color_verts + local[i]);
    }
    bucket.color_verts += draw.vertex_count;
    bucket.indices += nidx;
    return true;
}

void snapshot_tev(State& out, const DrawCall& draw) {
    out.reset();
    out.num_tev_stages = draw.num_tev_stages == 0 ? 1 : draw.num_tev_stages;
    std::memcpy(out.tev, draw.tev_stages, sizeof(out.tev));
    std::memcpy(out.tev_color, draw.tev_color, sizeof(out.tev_color));
    std::memcpy(out.konst, draw.konst, sizeof(out.konst));
    std::memcpy(out.tev_swap, draw.tev_swap, sizeof(out.tev_swap));
    out.fog = draw.fog;
    out.alpha_ref0 = draw.alpha_ref0;
    out.alpha_ref1 = draw.alpha_ref1;
    out.alpha_comp0 = draw.alpha_comp0;
    out.alpha_comp1 = draw.alpha_comp1;
    out.alpha_op = draw.alpha_op;
}

bool warm_tev(gxm::Device* device, const Processor& processor) {
    if (!gxm::has_runtime_tev(device)) {
        return false;
    }
    char src[8192];
    bool any = false;
    for (std::uint32_t d = 0; d < processor.draw_count; ++d) {
        const DrawCall& draw = processor.draws[d];
        if (!draw.tex0 || draw.tev == TevPassClr || draw.vertex_count == 0) {
            continue;
        }
        if (gxm::has_tev_fragment(device, draw.tev_key)) {
            any = true;
            continue;
        }
        State snap;
        snapshot_tev(snap, draw);
        std::size_t n = 0;
        if (!emit_tev_fragment_cg(snap, src, sizeof(src), &n)) {
            continue;
        }
        if (gxm::ensure_tev_fragment(device, draw.tev_key, src)) {
            any = true;
        }
    }
    return any;
}

} // namespace

bool submit_processor(gxm::Device* device, const Processor& processor) {
    if (!device) {
        return false;
    }

    static MeshBucket replace;
    static MeshBucket modulate;
    static MeshBucket color;
    static MeshBucket alpha;
    static MeshBucket subtract;
    static MeshBucket tev;
    clear_bucket(replace);
    clear_bucket(modulate);
    clear_bucket(color);
    clear_bucket(alpha);
    clear_bucket(subtract);

    constexpr unsigned kTevBatches = 8;
    std::uint64_t tev_keys[kTevBatches]{};
    unsigned tev_key_count = 0;
    auto remember_tev = [&](std::uint64_t key) {
        for (unsigned i = 0; i < tev_key_count; ++i) {
            if (tev_keys[i] == key) {
                return;
            }
        }
        if (tev_key_count < kTevBatches) {
            tev_keys[tev_key_count++] = key;
        }
    };

    for (std::uint32_t d = 0; d < processor.draw_count; ++d) {
        const DrawCall& draw = processor.draws[d];
        if (draw.vertex_count == 0) {
            continue;
        }
        static std::uint16_t local[kMaxDecodedVerts * 3];
        const std::uint32_t nidx = expand_indices(draw.prim, draw.vertex_count, local, kMaxDecodedVerts * 3);
        if (nidx == 0) {
            continue;
        }
        if (d == 0) {
            g_dbg.nidx0 = nidx;
            g_dbg.bucket0 = draw.tex0 && gxm::has_tev_fragment(device, draw.tev_key) ? 0
                            : (draw.tex0 && draw.tev == TevModulate)                 ? 1
                            : (draw.tex0 && draw.tev != TevPassClr)                  ? 2
                            : (draw.blend_mode == BlendSubtract)                     ? 3
                            : draw.blend                                             ? 4
                                                                                     : 5;
        }
        if (draw.tex0 && gxm::has_tev_fragment(device, draw.tev_key)) {
            remember_tev(draw.tev_key);
        } else if (draw.tex0 && draw.tev == TevModulate) {
            if (!append_tex(modulate, processor, draw, local, nidx)) {
                break;
            }
        } else if (draw.tex0 && draw.tev != TevPassClr) {
            if (!append_tex(replace, processor, draw, local, nidx)) {
                break;
            }
        } else if (draw.blend_mode == BlendSubtract) {
            if (!append_color(subtract, processor, draw, local, nidx)) {
                break;
            }
        } else if (draw.blend) {
            if (!append_color(alpha, processor, draw, local, nidx)) {
                break;
            }
        } else if (!append_color(color, processor, draw, local, nidx)) {
            break;
        }
    }

    g_dbg.draws = processor.draw_count;
    g_dbg.depth_test = processor.state.depth_test ? 1 : 0;
    g_dbg.depth_write = processor.state.depth_write ? 1 : 0;
    g_dbg.depth_func = static_cast<int>(processor.state.depth_func);
    g_dbg.cull = static_cast<int>(processor.state.cull_mode);
    g_dbg.rep_v = replace.tex_verts;
    g_dbg.rep_i = replace.indices;
    g_dbg.mod_v = modulate.tex_verts;
    g_dbg.mod_i = modulate.indices;
    g_dbg.col_v = color.color_verts;
    g_dbg.alp_v = alpha.color_verts;
    g_dbg.tevk = tev_key_count;
    if (replace.tex_verts == 0 && modulate.tex_verts == 0 && tev_key_count == 0 && color.color_verts == 0 &&
        alpha.color_verts == 0 && subtract.color_verts == 0) {
        g_dbg.early_out = 1;
        return false;
    }
    g_dbg.early_out = 0;

    gxm::set_cull_mode(device, static_cast<unsigned>(processor.state.cull_mode));
    gxm::set_depth(device, processor.state.depth_test, processor.state.depth_write,
                   static_cast<unsigned>(processor.state.depth_func));
    if (processor.state.has_render_vp) {
        gxm::set_viewport(device, processor.state.render_vp[0], processor.state.render_vp[1], processor.state.render_vp[2],
                          processor.state.render_vp[3], processor.state.render_vp[4], processor.state.render_vp[5]);
    }
    if (processor.state.has_render_scissor) {
        gxm::set_scissor(device, processor.state.render_scissor[0], processor.state.render_scissor[1],
                         processor.state.render_scissor[2], processor.state.render_scissor[3]);
    } else if (processor.state.has_scissor) {
        gxm::set_scissor(device, processor.state.scissor_x, processor.state.scissor_y, processor.state.scissor_w,
                         processor.state.scissor_h);
    }
    bool ok = true;
    const TexObj& tex0 = processor.state.texmaps[0];
    const TexObj& tex1 = processor.state.texmaps[1];
    std::uint8_t* converted = converted_buffer();
    ConvertedTexture host{};
    const TlutObj* tlut = nullptr;
    if (tex0.tlut < kMaxTluts && processor.state.tluts[tex0.tlut].data != nullptr) {
        tlut = &processor.state.tluts[tex0.tlut];
    }
    const bool need_tex0 = replace.tex_verts != 0 || modulate.tex_verts != 0 || tev_key_count != 0;
    if (need_tex0 && tex0.data != nullptr) {
        if (converted != nullptr && convert_gx_texture(tex0, converted, kConvertedCapacity, &host, tlut)) {
            g_dbg.tex_convert = 1;
            g_dbg.tex_w = host.width;
            g_dbg.tex_h = host.height;
            g_dbg.tex_bytes = host.bytes;
            g_dbg.tex_layout = static_cast<int>(host.layout);
            g_dbg.tex_mips = static_cast<int>(host.mip_count);
            g_dbg.tex_bind = gxm::bind_fragment_texture(device, host.data, host.bytes, host.width, host.height,
                                                        static_cast<unsigned>(host.layout), tex0.wrap_s, tex0.wrap_t, 0,
                                                        tex0.min_filter, tex0.mag_filter, tex0.lod_bias,
                                                        host.mip_count)
                                   ? 1
                                   : 0;
        } else {
            g_dbg.tex_convert = 0;
            static bool logged = false;
            if (!logged) {
                std::printf("mkwv: tex0 convert fail %ux%u fmt=%u mips=%d need=%u cap=%u buf=%d\n", tex0.width,
                            tex0.height, tex0.format, tex0.mips ? 1 : 0, tex0.width * tex0.height * 4u,
                            kConvertedCapacity, converted != nullptr ? 1 : 0);
                std::fflush(stdout);
                logged = true;
            }
        }
    }
    if (tev_key_count != 0 && tex1.data != nullptr) {
        const TlutObj* tlut1 = nullptr;
        if (tex1.tlut < kMaxTluts && processor.state.tluts[tex1.tlut].data != nullptr) {
            tlut1 = &processor.state.tluts[tex1.tlut];
        }
        if (converted != nullptr && convert_gx_texture(tex1, converted, kConvertedCapacity, &host, tlut1)) {
            gxm::bind_fragment_texture(device, host.data, host.bytes, host.width, host.height,
                                       static_cast<unsigned>(host.layout), tex1.wrap_s, tex1.wrap_t, 1, tex1.min_filter,
                                       tex1.mag_filter, tex1.lod_bias, host.mip_count);
        }
    }
    if (replace.tex_verts != 0 && replace.indices != 0) {
        const bool r = gxm::draw_textured_mesh(device, replace.tex, replace.tex_verts, replace.idx, replace.indices);
        g_dbg.r_replace = r ? 1 : 0;
        ok = r && ok;
    }
    for (unsigned k = 0; k < tev_key_count; ++k) {
        clear_bucket(tev);
        for (std::uint32_t d = 0; d < processor.draw_count; ++d) {
            const DrawCall& draw = processor.draws[d];
            if (!draw.tex0 || draw.tev_key != tev_keys[k] || draw.vertex_count == 0) {
                continue;
            }
            static std::uint16_t local[kMaxDecodedVerts * 3];
            const std::uint32_t nidx = expand_indices(draw.prim, draw.vertex_count, local, kMaxDecodedVerts * 3);
            if (nidx == 0) {
                continue;
            }
            if (!append_tev(tev, processor, draw, local, nidx)) {
                break;
            }
        }
        if (tev.tev_verts != 0 && tev.indices != 0) {
            const bool r = gxm::draw_tev_mesh(device, tev.tev, tev.tev_verts, tev.idx, tev.indices, tev_keys[k]);
            g_dbg.r_tev = r ? 1 : 0;
            ok = r && ok;
        }
    }
    if (modulate.tex_verts != 0 && modulate.indices != 0) {
        const bool rm = gxm::draw_color_mesh(device, modulate.color, modulate.color_verts, modulate.idx,
                                             modulate.indices, gxm::kBlendOpaque);
        g_dbg.r_modulate = rm ? 1 : 0;
        ok = rm && ok;
        gxm::set_depth(device, processor.state.depth_test, false,
                       static_cast<unsigned>(processor.state.depth_func));
        ok = gxm::draw_textured_mesh(device, modulate.tex, modulate.tex_verts, modulate.idx, modulate.indices,
                                     gxm::kBlendModulate) &&
             ok;
        gxm::set_depth(device, processor.state.depth_test, processor.state.depth_write,
                       static_cast<unsigned>(processor.state.depth_func));
    }
    if (color.color_verts != 0 && color.indices != 0) {
        const bool r = gxm::draw_color_mesh(device, color.color, color.color_verts, color.idx, color.indices);
        g_dbg.r_color = r ? 1 : 0;
        ok = r && ok;
    }
    if (alpha.color_verts != 0 && alpha.indices != 0) {
        gxm::set_depth(device, processor.state.depth_test, false,
                       static_cast<unsigned>(processor.state.depth_func));
        ok = gxm::draw_color_mesh(device, alpha.color, alpha.color_verts, alpha.idx, alpha.indices, gxm::kBlendAlpha) &&
             ok;
        gxm::set_depth(device, processor.state.depth_test, processor.state.depth_write,
                       static_cast<unsigned>(processor.state.depth_func));
    }
    if (subtract.color_verts != 0 && subtract.indices != 0) {
        gxm::set_depth(device, processor.state.depth_test, false,
                       static_cast<unsigned>(processor.state.depth_func));
        ok = gxm::draw_color_mesh(device, subtract.color, subtract.color_verts, subtract.idx, subtract.indices,
                                  gxm::kBlendSubtract) &&
             ok;
        gxm::set_depth(device, processor.state.depth_test, processor.state.depth_write,
                       static_cast<unsigned>(processor.state.depth_func));
    }
    // TEMPORARY PROBE (remove once the strap image is visible): re-draw the frame's texture as a
    // hardcoded full-screen quad with depth and cull disabled. If this shows the image but the guest's
    // own quad does not, the texture, shader, viewport and present path are all fine and the fault is
    // in the geometry/state coming out of the GX decode. If it shows nothing either, the fault is
    // below submit_processor.
    if (kDrawDeviceProbes && need_tex0 && g_dbg.tex_bind == 1) {
        static const gxm::TexturedVertex kProbe[4] = {
            {-1.0f, 1.0f, 0.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, 0.0f, 1.0f, 0.0f},
            {1.0f, -1.0f, 0.0f, 1.0f, 1.0f},
            {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},
        };
        static const std::uint16_t kProbeIdx[6] = {0, 1, 2, 2, 3, 0};
        gxm::set_depth(device, false, false, 7);
        gxm::set_cull_mode(device, 0);
        g_dbg.probe = gxm::draw_textured_mesh(device, kProbe, 4, kProbeIdx, 6) ? 1 : 0;
    }

    // Second probe: the simplest possible mesh draw - a solid magenta quad through draw_color_mesh,
    // no texture involved, depth and cull off, left half of the screen. Magenta on screen means the
    // mesh path works and the fault is texture-specific; nothing means mesh draws never reach the
    // framebuffer at all, even though the clear triangle in begin_frame does.
    if (kDrawDeviceProbes) {
        static const gxm::ColorVertex kSolid[4] = {
            {-0.9f, 0.9f, 0.0f, 0xFFFF00FFu},
            {0.0f, 0.9f, 0.0f, 0xFFFF00FFu},
            {0.0f, -0.9f, 0.0f, 0xFFFF00FFu},
            {-0.9f, -0.9f, 0.0f, 0xFFFF00FFu},
        };
        static const std::uint16_t kSolidIdx[6] = {0, 1, 2, 2, 3, 0};
        gxm::set_depth(device, false, false, 7);
        gxm::set_cull_mode(device, 0);
        g_dbg.probe2 = gxm::draw_color_mesh(device, kSolid, 4, kSolidIdx, 6) ? 1 : 0;
    }
    if (kDrawDeviceProbes) {
        gxm::probe_clear_quad(device);
    }
    gxm::gxm_errors(device, &g_dbg.e_scene, &g_dbg.e_clear, &g_dbg.e_prog, &g_dbg.e_stream, &g_dbg.e_tex,
                    &g_dbg.e_draw);
    gxm::gxm_uniform_errors(device, &g_dbg.e_vunif, &g_dbg.e_vunull, &g_dbg.e_pclear, &g_dbg.e_pres,
                            &g_dbg.e_pnull);

    return ok;
}

void render_demo(gxm::Device* device, float time_seconds) {
    static Fifo fifo;
    static Processor processor;
    if (!build_demo_frame(fifo, time_seconds)) {
        return;
    }
    processor.reset();
    if (!processor.process(fifo.data(), fifo.size(), true)) {
        return;
    }
    warm_tev(device, processor);
    gxm::begin_frame(device, 0.05f, 0.07f, 0.16f, 1.0f);
    submit_processor(device, processor);
}

const char* last_submit_debug() {
    std::snprintf(g_dbg_text, sizeof(g_dbg_text),
                  "mkwv: SUBMIT draws=%u nidx0=%u bucket0=%d early=%d | rep=%u/%u mod=%u/%u col=%u alp=%u tevk=%u | "
                  "tex conv=%d bind=%d %ux%u bytes=%u layout=%d mips=%d | depth=%d/%d fn=%d cull=%d probe=%d/%d | "
                  "r: rep=%d mod=%d col=%d tev=%d | gxm: scene=%d clear=%d prog=%d stream=%d tex=%d draw=%d | "
                  "vunif=%d null=%d | pclear=%d pres=%d pnull=%d\n",
                  g_dbg.draws, g_dbg.nidx0, g_dbg.bucket0, g_dbg.early_out, g_dbg.rep_v, g_dbg.rep_i, g_dbg.mod_v,
                  g_dbg.mod_i, g_dbg.col_v, g_dbg.alp_v, g_dbg.tevk, g_dbg.tex_convert, g_dbg.tex_bind, g_dbg.tex_w,
                  g_dbg.tex_h, g_dbg.tex_bytes, g_dbg.tex_layout, g_dbg.tex_mips, g_dbg.depth_test,
                  g_dbg.depth_write, g_dbg.depth_func, g_dbg.cull, g_dbg.probe, g_dbg.probe2, g_dbg.r_replace, g_dbg.r_modulate,
                  g_dbg.r_color, g_dbg.r_tev, g_dbg.e_scene, g_dbg.e_clear, g_dbg.e_prog, g_dbg.e_stream,
                  g_dbg.e_tex, g_dbg.e_draw, g_dbg.e_vunif, g_dbg.e_vunull, g_dbg.e_pclear, g_dbg.e_pres,
                  g_dbg.e_pnull);
    return g_dbg_text;
}

} // namespace mkwv::gx
