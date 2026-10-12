#include "mkwv/gx/submit.hpp"
#include "mkwv/gx/capture.hpp"
#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/efb_copy.hpp"
#include "mkwv/gx/gpu_mesh.hpp"
#include "mkwv/gx/perf.hpp"
#include "mkwv/gx/vertex_worker.hpp"
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/tev.hpp"
#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/demo.hpp"
#include "mkwv/gx/texture.hpp"

#include <vitaGL.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The GX -> vitaGL renderer. Structure follows Aurora (lib/gx/gx.cpp, shader.cpp,
// lib/gfx/texture*.cpp): every draw is evaluated by a Cg program generated from its TEV
// state; the fixed-function GL_COMBINE path remains only as a fallback for a program that
// fails to compile.

namespace mkwv::gx {
namespace {

constexpr std::uint32_t kConvertedCapacity = 8u * 1024u * 1024u;
constexpr unsigned kTexCache = 256;
constexpr int kDisplayW = 960;
constexpr int kDisplayH = 544;

struct SubmitDebug {
    std::uint32_t draws = 0;
    std::uint32_t batches = 0;
    std::uint32_t nidx0 = 0;
    std::uint32_t issued = 0;
    std::uint32_t skipped = 0;
    std::uint32_t rasnull = 0;
    std::uint32_t comb = 0;
    std::uint32_t cfail = 0;
    std::uint32_t prog = 0;       // draws issued through a compiled TEV program
    std::uint32_t progfail = 0;   // draws that fell back to fixed function
    std::uint32_t progmake = 0;   // programs compiled this frame
    std::uint32_t progcount = 0;  // distinct programs cached
    std::uint32_t progevict = 0;  // programs evicted from the cache this frame
    int proglast = 0;             // last compile/link failure, for the log
    std::uint32_t efbcopy = 0;    // EFB -> texture copies performed this frame
    std::uint32_t efbhit = 0;     // texture binds served from a captured copy
    std::uint32_t rasterchg = 0;  // draws whose viewport/scissor/depth/cull differed from the last
    std::uint32_t logicop = 0;    // draws blended through a logic-op emulation
    std::uint32_t dstalpha = 0;   // blended draws given a second, constant-alpha pass (GXSetDstAlpha)
    std::uint32_t texdecode = 0;  // texture conversions this frame
    std::uint32_t texrevalid = 0; // cached textures re-hashed after GXInvalidateTexAll
    int tex_convert = -1;
    int tex_bind = -1;
    std::uint16_t fmt_bind[16]{};  // texture binds per GX format this frame (cache hits included)
    std::uint16_t fmt_fail[16]{};  // conversions that failed per GX format
    std::uint32_t tex_w = 0, tex_h = 0, tex_bytes = 0;
    int depth_test = -1, depth_write = -1, depth_func = -1, cull = -1;
    int gl_err = 0;
    std::uint32_t gathers = 0;    // merged runs whose vertices were not contiguous in the arena
};

SubmitDebug g_dbg;
bool g_frame_open = false;  // a batch of this frame has already been submitted

// Cumulative EFB-copy counters. The SUBMIT line is one frame, and MKW takes its dialog
// snapshot once, so a per-frame count would almost never be caught non-zero.
std::uint32_t g_efb_total_done = 0;
std::uint32_t g_efb_total_front = 0;
std::uint32_t g_efb_total_hits = 0;
std::uint32_t g_efb_total_orphan = 0;  // copies performed from a drain with no draws

// EFB -> render target mapping, refreshed by set_frame_mapping every frame.
float g_fb_scale = 1.0f;
int g_fb_base_x = 0;
int g_fb_base_y = 0;
int g_fb_dh = kDisplayH;

// The EFB. GX renders into a 640x528 embedded framebuffer that persists between frames and
// is only ever shown through a display copy of part of it. MKW relies on that: it renders
// its LightTexture images below the displayed area (EFB rows 456-528), copies them into
// textures and clears them, so they never appear on screen. Drawing straight into the back
// buffer letterboxed the whole 528-row extent, which showed those scratch images and shrank
// the picture. So GX draws into this render target, at a fixed scale that makes a 456-row
// EFB exactly 544 display rows, and present_efb shows only the display copy's source rect.
// If the FBO cannot be created, everything falls back to the old back-buffer path.
// EFB pixels per GX pixel: 544/456 fills the panel's 544 lines. set_efb_scale lowers it on
// real hardware to trade sharpness for fill rate (markers efb_low / efb_lower in main.cpp).
float g_efb_scale = 544.0f / 456.0f;
constexpr int kEfbTargetW = 768;  // >= 640 * g_efb_scale, tile aligned
constexpr int kEfbTargetH = 640;  // >= 528 * g_efb_scale, tile aligned
GLuint g_efb_fbo = 0;
// A copy's destination stays bound until the next real draw. Rebinding the EFB after every
// copy makes vitaGL end and begin the EFB scene twice per copy.
bool g_efb_draw_bound = true;
GLuint g_efb_color = 0;
GLuint g_efb_depth = 0;

// GPU vertex ring (static64): the vertex jobs pack into GPU-mapped memory; draws from it point
// a VBO at the run's first vertex (vglBufferData only sets the buffer's pointer) so vitaGL
// neither copies the vertices nor scans the indices for the highest one.
GLuint g_ring_vbo = 0;
const std::uint8_t* g_ring_base = nullptr;
std::uint32_t g_ring_bytes = 0;
GLuint g_array_bound = 0;

void use_array_buffer(GLuint b) {
    if (b != g_array_bound) {
        glBindBuffer(GL_ARRAY_BUFFER, b);
        g_array_bound = b;
    }
}

bool in_ring(const std::uint8_t* p) {
    return g_ring_vbo != 0 && p >= g_ring_base && p < g_ring_base + g_ring_bytes;
}
bool g_efb_target = false;          // rendering into the EFB target (not the back buffer)
bool g_efb_clear_pending = false;   // the new frame's clear, deferred past its leading copies
std::uint32_t g_efb_clear_rgba = 0;
int g_rt_w = kDisplayW;             // size of whatever GX draws into
int g_rt_h = kDisplayH;
std::uint32_t g_present_src[4] = {0, 0, 0, 0};  // last presented EFB rect, for the log

char g_dbg_text[1280];

std::uint8_t* converted_buffer() {
    static std::uint8_t* buffer = static_cast<std::uint8_t*>(std::malloc(kConvertedCapacity));
    return buffer;
}

// GX blend factors. In the source slot codes 2/3 mean DSTCLR/INVDSTCLR; in the destination
// slot they mean SRCCLR/INVSRCCLR (Aurora gx.cpp:1379-1396). With an EFB format that has no
// alpha, DSTALPHA reads as one (Aurora gx.cpp:1408-1417).
GLenum gl_blend_factor(BlendFactor f, bool is_src, bool dst_has_alpha) {
    switch (f) {
    case BlZero:
        return GL_ZERO;
    case BlOne:
        return GL_ONE;
    case BlSrcClr:
        return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case BlInvSrcClr:
        return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case BlSrcAlpha:
        return GL_SRC_ALPHA;
    case BlInvSrcAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case BlDstAlpha:
        return dst_has_alpha ? GL_DST_ALPHA : GL_ONE;
    case BlInvDstAlpha:
        return dst_has_alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ZERO;
    default:
        return GL_ONE;
    }
}

GLenum gl_compare(Compare c) {
    switch (c) {
    case Never:
        return GL_NEVER;
    case Less:
        return GL_LESS;
    case Equal:
        return GL_EQUAL;
    case LEqual:
        return GL_LEQUAL;
    case Greater:
        return GL_GREATER;
    case NEqual:
        return GL_NOTEQUAL;
    case GEqual:
        return GL_GEQUAL;
    case Always:
    default:
        return GL_ALWAYS;
    }
}

GLenum gl_wrap(unsigned wrap) {
    switch (wrap) {
    case WrapClamp:
        return GL_CLAMP_TO_EDGE;
    case WrapMirror:
        return GL_MIRRORED_REPEAT;
    case WrapRepeat:
    default:
        return GL_REPEAT;
    }
}

GLenum gl_min_filter(unsigned filter, bool have_mips) {
    switch (filter) {
    case TexNear:
        return GL_NEAREST;
    case TexNearMipNear:
        return have_mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
    case TexLinMipNear:
        return have_mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
    case TexNearMipLin:
        return have_mips ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST;
    case TexLinMipLin:
        return have_mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    case TexLinear:
    default:
        return GL_LINEAR;
    }
}

bool pixel_fmt_has_alpha(std::uint8_t pixel_fmt) {
    return pixel_fmt == 1u;  // GX_PF_RGBA6_Z24
}

void apply_cull(CullMode mode) {
    switch (mode) {
    case CullFront:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        break;
    case CullBack:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        break;
    case CullAll:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT_AND_BACK);
        break;
    case CullNone:
    default:
        glDisable(GL_CULL_FACE);
        break;
    }
}

// The EFB -> screen letterbox is a property of the frame, not of any one draw: it comes
// from the frame's full EFB extent (the union of every draw's viewport, which is the
// render mode's EFB in practice).
float g_fb_efb_w = 640.0f;
float g_fb_efb_h = 480.0f;

void set_frame_mapping(float efb_w, float efb_h) {
    if (efb_w < 2.0f || efb_h < 2.0f) {
        efb_w = 640.0f;
        efb_h = 480.0f;
    }
    if (g_efb_target) {
        // Fixed: the EFB target is the EFB, not a letterbox of it.
        g_fb_efb_w = efb_w;
        g_fb_efb_h = efb_h;
        g_fb_scale = g_efb_scale;
        g_fb_base_x = 0;
        g_fb_base_y = 0;
        g_fb_dh = kEfbTargetH;
        return;
    }
    const float scale = efb_w / efb_h > static_cast<float>(kDisplayW) / static_cast<float>(kDisplayH)
                            ? static_cast<float>(kDisplayW) / efb_w
                            : static_cast<float>(kDisplayH) / efb_h;
    const int dw = static_cast<int>(efb_w * scale + 0.5f);
    const int dh = static_cast<int>(efb_h * scale + 0.5f);
    g_fb_efb_w = efb_w;
    g_fb_efb_h = efb_h;
    g_fb_scale = scale;
    g_fb_base_x = (kDisplayW - dw) / 2;
    g_fb_base_y = (kDisplayH - dh) / 2;
    g_fb_dh = dh;
}

template <typename S>
void viewport_extent(const S& st, float* right, float* bottom) {
    if (st.has_render_vp && st.render_vp[2] > 1.0f && st.render_vp[3] > 1.0f) {
        *right = st.render_vp[0] + st.render_vp[2];
        *bottom = st.render_vp[1] + st.render_vp[3];
    } else {
        *right = 640.0f;
        *bottom = 480.0f;
    }
}

// An EFB rect (top-left origin) as a GL rect (bottom-left origin) inside the letterbox.
void efb_to_gl(float x, float y, float w, float h, int* gx, int* gy, int* gw, int* gh) {
    *gx = g_fb_base_x + static_cast<int>(x * g_fb_scale + 0.5f);
    *gw = static_cast<int>(w * g_fb_scale + 0.5f);
    *gh = static_cast<int>(h * g_fb_scale + 0.5f);
    *gy = g_fb_base_y + g_fb_dh - static_cast<int>((y + h) * g_fb_scale + 0.5f);
}

// The current draw's GX viewport depth range, for the fog depth.
float g_depth_near = 0.0f;
float g_depth_far = 1.0f;

template <typename S>
void apply_viewport(const S& state) {
    float vx = 0.0f, vy = 0.0f, vw = g_fb_efb_w, vh = g_fb_efb_h;
    float zn = 0.0f, zf = 1.0f;
    if (state.has_render_vp && state.render_vp[2] > 1.0f && state.render_vp[3] > 1.0f) {
        vx = state.render_vp[0];
        vy = state.render_vp[1];
        vw = state.render_vp[2];
        vh = state.render_vp[3];
        zn = state.render_vp[4];
        zf = state.render_vp[5];
    }
    // GXSetScissorBoxOffset moves the screen-to-EFB mapping: an EFB pixel is the screen
    // position minus the offset, for the viewport and the scissor alike (Aurora gx.cpp:903).
    vx -= static_cast<float>(state.scissor_off_x);
    vy -= static_cast<float>(state.scissor_off_y);
    int gx = 0, gy = 0, gw = 0, gh = 0;
    efb_to_gl(vx, vy, vw, vh, &gx, &gy, &gw, &gh);
    glViewport(gx, gy, gw > 0 ? gw : 1, gh > 0 ? gh : 1);
    // GX maps clip depth into [near, far] of the viewport (GXSetViewport's last two args).
    if (!(zn >= 0.0f && zn <= 1.0f)) {
        zn = 0.0f;
    }
    if (!(zf >= 0.0f && zf <= 1.0f)) {
        zf = 1.0f;
    }
    glDepthRangef(zn, zf);
    g_depth_near = zn;
    g_depth_far = zf;
    if (state.has_render_scissor || state.has_scissor) {
        const float sx = state.has_render_scissor ? static_cast<float>(state.render_scissor[0])
                                                  : static_cast<float>(state.scissor_x);
        const float sy = state.has_render_scissor ? static_cast<float>(state.render_scissor[1])
                                                  : static_cast<float>(state.scissor_y);
        const float sw = state.has_render_scissor ? static_cast<float>(state.render_scissor[2])
                                                  : static_cast<float>(state.scissor_w);
        const float sh = state.has_render_scissor ? static_cast<float>(state.render_scissor[3])
                                                  : static_cast<float>(state.scissor_h);
        int cx = 0, cy = 0, cw = 0, ch = 0;
        efb_to_gl(sx - static_cast<float>(state.scissor_off_x), sy - static_cast<float>(state.scissor_off_y), sw, sh,
                  &cx, &cy, &cw, &ch);
        glEnable(GL_SCISSOR_TEST);
        glScissor(cx, cy, cw > 0 ? cw : 1, ch > 0 ? ch : 1);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

// ---------------------------------------------------------------------------
// Textures produced by GXCopyTex.
//
// Aurora registers a copy under its destination and matches a later texobj on the same
// address, the same logical size and a compatible format (gx.cpp:1245-1261, gx.hpp:515-542).
// The address is compared in physical form, so a cached (0x80...) and an uncached (0xC0...)
// mapping of the same buffer match, and the host pointer is kept as a fallback key.
struct EfbTexture {
    const std::uint8_t* dest = nullptr;
    std::uint32_t phys = 0;
    GLuint id = 0;
    GLuint fbo = 0;
    std::uint32_t w = 0, h = 0;
    std::uint32_t format = 0;
    std::uint32_t stamp = 0;
    bool fbo_bound = false;  // unused since static62 (copy FBOs are pooled by size)
};

// Framebuffers the copies render into, pooled by size (static62). vitaGL gives every FBO its
// own GXM render target (8 scenes a frame) and its own depth/stencil buffer, created at the
// FBO's first scene and kept until the attachment changes size. One FBO per copy slot meant up
// to 48 of each: on a real Vita the menus' copies stalled the renderer for seconds creating them
// (static61: copy=5.9 s in one window during the title -> single-player load) and the race load
// then crashed inside sceGxm starting a scene. Re-attaching a texture of the same size to a pooled
// FBO only re-initialises its colour surface.
struct CopyFbo {
    GLuint fbo = 0;
    GLuint tex = 0;  // the texture currently attached
    std::uint32_t w = 0, h = 0;
    std::uint32_t stamp = 0;
};
constexpr unsigned kCopyFbos = 12;
CopyFbo g_copy_fbos[kCopyFbos];
std::uint32_t g_copy_fbo_stamp = 0;
std::uint32_t g_copy_fbo_resizes = 0;  // pool misses that re-sized an FBO (a new render target)
// A race frame makes 15-18 copies to distinct buffers: with 16 slots the LRU recycled a slot
// every frame, and a recycled slot of another size means a new texture, FBO attachment and
// render target (vitaGL) - per copy, per frame. Slots are only allocated when used.
constexpr unsigned kMaxEfbTextures = 48;
EfbTexture g_efb_tex[kMaxEfbTextures];
std::uint32_t g_efb_stamp = 0;
std::uint32_t g_copy_clear_failures = 0;  // masked copy clears that could not be drawn (SUBMIT cclrfail)
std::uint32_t g_model_copy_phys = 0;
unsigned g_model_copy_reports = 0;
unsigned g_model_bind_reports = 0;

// The texture format a copy format is sampled as (Aurora gx.hpp:515-542).
std::uint32_t copy_sample_format(std::uint32_t copy_fmt) {
    switch (copy_fmt) {
    case 0x20u:  // R4
        return TexI4;
    case 0x27u:  // A8
    case 0x28u:  // R8
    case 0x29u:  // G8
    case 0x2Au:  // B8
    case 0x11u:  // Z8
    case 0x39u:  // Z8M
    case 0x3Au:  // Z8L
        return TexI8;
    case 0x22u:  // RA4
        return TexIA4;
    case 0x23u:  // RA8
    case 0x2Bu:  // RG8
    case 0x2Cu:  // GB8
    case 0x13u:  // Z16
    case 0x3Cu:  // Z16L
        return TexIA8;
    case 0x16u:  // Z24X8
        return TexRGBA8;
    case 0x30u:  // Z4
        return TexI4;
    default:
        return copy_fmt & 0xFu;
    }
}

bool efb_format_compatible(std::uint32_t copy_fmt, std::uint32_t tex_fmt) {
    const std::uint32_t want = copy_sample_format(copy_fmt);
    const std::uint32_t have = sampled_format(tex_fmt) & 0xFu;
    if (want == have) {
        return true;
    }
    // A copy may be read back through a palette (Aurora resolve_dynamic_palette_texture):
    // an 8-bit copy as C8, a 4-bit one as C4.
    return (want == TexI8 && have == TexCI8) || (want == TexI4 && have == TexCI4) ||
           (want == TexIA8 && have == TexCI14X2);
}

const EfbTexture* efb_texture_for(const TexObj& tex) {
    const std::uint32_t phys = tex.phys != 0 ? (tex.phys & 0x3FFFFFFFu) : 0u;
    for (unsigned i = 0; i < kMaxEfbTextures; ++i) {
        const EfbTexture& e = g_efb_tex[i];
        if (e.id == 0) {
            continue;
        }
        const bool same_addr = (phys != 0 && e.phys == phys) || (tex.data != nullptr && e.dest == tex.data);
        if (!same_addr) {
            continue;
        }
        // A freed and reused copy buffer must not keep serving a stale copy: the logical size
        // and format have to agree too (Aurora gx.cpp:1259).
        if ((tex.width != 0 && tex.width != e.w) || (tex.height != 0 && tex.height != e.h)) {
            continue;
        }
        if (!efb_format_compatible(e.format, tex.format)) {
            continue;
        }
        return &e;
    }
    return nullptr;
}

EfbTexture& efb_slot_for(std::uint32_t phys, const std::uint8_t* dest) {
    unsigned oldest = 0;
    for (unsigned i = 0; i < kMaxEfbTextures; ++i) {
        if (g_efb_tex[i].id != 0 && (g_efb_tex[i].phys == phys || g_efb_tex[i].dest == dest)) {
            return g_efb_tex[i];
        }
        if (g_efb_tex[i].id == 0) {
            return g_efb_tex[i];
        }
        if (g_efb_tex[i].stamp < g_efb_tex[oldest].stamp) {
            oldest = i;
        }
    }
    return g_efb_tex[oldest];
}

// EFB colour -> copy texel, per Aurora gfx/tex_copy_conv.cpp:56-339. Intensity is the
// BT.601 luma GX computes (0.257R + 0.504G + 0.098B + 16/255); 4-bit formats quantise.
void convert_copy_texel(std::uint32_t fmt, bool efb_has_alpha, const std::uint8_t in[4], std::uint8_t out[4]) {
    const float r = in[0] / 255.0f, g = in[1] / 255.0f, b = in[2] / 255.0f;
    const float a = efb_has_alpha ? in[3] / 255.0f : 1.0f;
    float y = 0.257f * r + 0.504f * g + 0.098f * b + 16.0f / 255.0f;
    y = y > 1.0f ? 1.0f : y;
    auto q4 = [](float v) { return std::floor(v * 16.0f) / 15.0f > 1.0f ? 1.0f : std::floor(v * 16.0f) / 15.0f; };
    auto b8 = [](float v) {
        const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<std::uint8_t>(c * 255.0f + 0.5f);
    };
    float o[4] = {r, g, b, a};
    switch (fmt) {
    case 0x0u:  // I4
        o[0] = o[1] = o[2] = o[3] = q4(y);
        break;
    case 0x1u:  // I8
        o[0] = o[1] = o[2] = o[3] = y;
        break;
    case 0x2u:  // IA4
        o[0] = o[1] = o[2] = q4(y);
        o[3] = q4(a);
        break;
    case 0x3u:  // IA8
        o[0] = o[1] = o[2] = y;
        o[3] = a;
        break;
    case 0x4u:  // RGB565
        o[3] = 1.0f;
        break;
    case 0x20u:  // R4
        o[0] = o[1] = o[2] = o[3] = q4(r);
        break;
    case 0x22u:  // RA4
        o[0] = o[1] = o[2] = q4(r);
        o[3] = q4(a);
        break;
    case 0x23u:  // RA8
        o[0] = o[1] = o[2] = r;
        o[3] = a;
        break;
    case 0x27u:  // A8
        o[0] = o[1] = o[2] = o[3] = a;
        break;
    case 0x28u:  // R8
        o[0] = o[1] = o[2] = o[3] = r;
        break;
    case 0x29u:  // G8
        o[0] = o[1] = o[2] = o[3] = g;
        break;
    case 0x2Au:  // B8
        o[0] = o[1] = o[2] = o[3] = b;
        break;
    case 0x2Bu:  // RG8
        o[0] = o[1] = o[2] = r;
        o[3] = g;
        break;
    case 0x2Cu:  // GB8
        o[0] = o[1] = o[2] = g;
        o[3] = b;
        break;
    default:  // RGBA8, RGB5A3 and the depth formats (no depth readback here)
        break;
    }
    for (unsigned k = 0; k < 4u; ++k) {
        out[k] = b8(o[k]);
    }
}


void clear_efb_now(std::uint32_t rgba) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    // The clear colour's own alpha, as GX clears it (it used to be forced to 1.0). Only an RGBA6
    // EFB ever reads it back: blends and copies treat the other pixel formats' alpha as 1.
    glClearColor(static_cast<float>(rgba & 0xFFu) / 255.0f, static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
                 static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f, static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Keep colour copies on the GPU. In particular, do not turn an EFB texture into
// CPU bytes and upload it again: Vita3K can keep the rendered surface GPU-resident.
// These two formats use the same RGBA values as the existing CPU conversion.
GLuint prepare_copy_slot(EfbTexture& slot, std::uint32_t tw, std::uint32_t th);
void invalidate_gl_state_cache();

bool copy_efb_color_gpu(const EfbCopyEvent& e, int gx, int gy, int gw, int gh) {
    if (!g_efb_target || (e.format != TexRGB5A3 && e.format != TexRGBA8)) {
        return false;
    }
    const std::uint32_t phys = e.dest_guest & 0x3FFFFFFFu;
    EfbTexture& slot = efb_slot_for(phys, e.dest_host);
    const std::uint32_t tw = e.dst_w > 1024u ? 1024u : e.dst_w;
    const std::uint32_t th = e.dst_h > 1024u ? 1024u : e.dst_h;
    const GLuint dst_fbo = prepare_copy_slot(slot, tw, th);
    if (dst_fbo == 0) {
        return false;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // vitaGL's blit takes top-left source coordinates. Its FBO output is flipped,
    // so reverse the destination Y endpoints to produce an ordinary top-down GX
    // texture, matching the decoded textures consumed by our TEV shaders.
    glBlitNamedFramebuffer(g_efb_fbo, dst_fbo, gx, g_rt_h - gy - gh, gx + gw, g_rt_h - gy,
                          0, th, tw, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    g_efb_draw_bound = false;
    invalidate_gl_state_cache();
    slot.dest = e.dest_host;
    slot.phys = phys;
    slot.w = tw;
    slot.h = th;
    slot.format = e.format;
    slot.stamp = ++g_efb_stamp;
    if (e.format == TexRGB5A3 && tw == 256 && th == 256) {
        g_model_copy_phys = phys;
        if (g_model_copy_reports++ < 4) {
            std::printf("mkwv: model EFB GPU copy dest=%08X size=%ux%u\n", e.dest_guest, tw, th);
            std::fflush(stdout);
        }
    }
    return true;
}

GLuint compile_cg(GLenum kind, const char* src);

// Copy formats other than RGBA8/RGB5A3 (intensity, single channels, the depth formats) used to
// be read back to the CPU: glReadPixels ends the GXM scene *and waits for the GPU to finish
// everything queued so far*, then the CPU resampled and converted every texel and uploaded the
// result. MKW's race post-processing does several of these a frame. This draws the conversion
// on the GPU instead - the same colour matrix convert_copy_texel applies - straight into the
// copy's texture. The quad reproduces exactly the geometry of vitaGL's blit, which
// copy_efb_color_gpu already relies on: NDC y=-1 samples v=(gy+gh)/H, y=+1 samples v=gy/H.
struct CopyConvert {
    float m[16];    // row-major: out.r = dot(row0, c) ...
    float bias[4];
    bool quant4 = false;
};

bool depth_copy_format(std::uint32_t fmt) {
    return fmt == 0x11u || fmt == 0x13u || fmt == 0x16u ||
           fmt == 0x30u || fmt == 0x39u || fmt == 0x3Au || fmt == 0x3Cu;
}

CopyConvert copy_convert_for(std::uint32_t fmt) {
    CopyConvert cv{};
    const float Y[4] = {0.257f, 0.504f, 0.098f, 0.0f};
    const float R[4] = {1, 0, 0, 0}, G[4] = {0, 1, 0, 0}, B[4] = {0, 0, 1, 0}, A[4] = {0, 0, 0, 1};
    const float Z[4] = {0, 0, 0, 0};
    auto rows = [&](const float* r, const float* g, const float* b, const float* a) {
        for (unsigned i = 0; i < 4u; ++i) {
            cv.m[0 * 4 + i] = r[i];
            cv.m[1 * 4 + i] = g[i];
            cv.m[2 * 4 + i] = b[i];
            cv.m[3 * 4 + i] = a[i];
        }
    };
    const float yb = 16.0f / 255.0f;
    switch (fmt) {
    case 0x0u:  // I4
        rows(Y, Y, Y, Y);
        cv.bias[0] = cv.bias[1] = cv.bias[2] = cv.bias[3] = yb;
        cv.quant4 = true;
        break;
    case 0x1u:  // I8
        rows(Y, Y, Y, Y);
        cv.bias[0] = cv.bias[1] = cv.bias[2] = cv.bias[3] = yb;
        break;
    case 0x2u:  // IA4
        rows(Y, Y, Y, A);
        cv.bias[0] = cv.bias[1] = cv.bias[2] = yb;
        cv.quant4 = true;
        break;
    case 0x3u:  // IA8
        rows(Y, Y, Y, A);
        cv.bias[0] = cv.bias[1] = cv.bias[2] = yb;
        break;
    case 0x4u:  // RGB565
        rows(R, G, B, Z);
        cv.bias[3] = 1.0f;
        break;
    case 0x30u:  // Z4: high nibble of the high depth byte
    case 0x20u:  // R4
        rows(R, R, R, R);
        cv.quant4 = true;
        break;
    case 0x22u:  // RA4
        rows(R, R, R, A);
        cv.quant4 = true;
        break;
    case 0x23u:  // RA8
        rows(R, R, R, A);
        break;
    case 0x27u:  // A8
        rows(A, A, A, A);
        break;
    case 0x11u:  // Z8
    case 0x28u:  // R8
        rows(R, R, R, R);
        break;
    case 0x39u:  // Z8M
    case 0x29u:  // G8
        rows(G, G, G, G);
        break;
    case 0x3Au:  // Z8L
    case 0x2Au:  // B8
        rows(B, B, B, B);
        break;
    case 0x2Bu:  // RG8
        rows(R, R, R, G);
        break;
    case 0x2Cu:  // GB8
        rows(G, G, G, B);
        break;
    case 0x13u:  // Z16: high depth byte, opaque alpha (depth RA8 copy)
        rows(R, R, R, A);
        break;
    case 0x3Cu:  // Z16L: low two depth bytes, sampled as IA8
        rows(G, G, G, B);
        break;
    default:  // RGBA8, RGB5A3, Z24X8 (RGB contains high/middle/low depth bytes)
        rows(R, G, B, A);
        break;
    }
    return cv;
}

// Makes sure the slot's texture exists at tw x th and is attached to a pooled FBO of that size.
// Returns that FBO, or 0 if it is unusable.
GLuint prepare_copy_slot(EfbTexture& slot, std::uint32_t tw, std::uint32_t th) {
    const bool allocate = slot.id == 0 || slot.w != tw || slot.h != th;
    if (slot.id == 0) glGenTextures(1, &slot.id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, slot.id);
    if (allocate) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        slot.w = tw;
        slot.h = th;
        // A re-allocated texture has new storage: any FBO still attached to it is stale.
        for (CopyFbo& c : g_copy_fbos) {
            if (c.tex == slot.id) {
                c.tex = 0;
            }
        }
    }
    CopyFbo* pick = nullptr;
    for (CopyFbo& c : g_copy_fbos) {
        if (c.fbo != 0 && c.w == tw && c.h == th) {
            pick = &c;
            break;
        }
    }
    if (pick == nullptr) {
        // No FBO of this size: take an unused one, else the least recently used (its render
        // target and depth buffer are replaced at its next scene).
        for (CopyFbo& c : g_copy_fbos) {
            if (c.fbo == 0) {
                pick = &c;
                break;
            }
            if (pick == nullptr || c.stamp < pick->stamp) {
                pick = &c;
            }
        }
        if (pick->fbo == 0) {
            glGenFramebuffers(1, &pick->fbo);
        } else {
            ++g_copy_fbo_resizes;
        }
        pick->w = tw;
        pick->h = th;
        pick->tex = 0;
    }
    pick->stamp = ++g_copy_fbo_stamp;
    if (pick->tex != slot.id) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pick->fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.id, 0);
        const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::printf("mkwv: GPU EFB copy incomplete status=%X\n", static_cast<unsigned>(status));
            pick->tex = 0;
            return 0;
        }
        pick->tex = slot.id;
    }
    return pick->fbo;
}

bool copy_efb_convert_gpu(const EfbCopyEvent& e, int gx, int gy, int gw, int gh) {
    if (!g_efb_target) {
        return false;
    }
    static GLuint program = 0;
    static bool program_failed = false;
    static GLint a_pos = -1, a_uv = -1, u_tex = -1, u_m = -1, u_bias = -1, u_q = -1, u_a1 = -1, u_depth = -1;
    if (program == 0 && !program_failed) {
        const GLuint vs = compile_cg(GL_CG_VERTEX_SHADER_EXT,
            "void main(float2 aPosition, float2 aTexcoord0, float4 out vPosition : POSITION,"
            " float2 out vUv : TEXCOORD0) { vPosition = float4(aPosition, 0.0, 1.0); vUv = aTexcoord0; }");
        const GLuint fs = compile_cg(GL_CG_FRAGMENT_SHADER_EXT,
            "float4 main(float2 vUv : TEXCOORD0, uniform sampler2D uEfb, uniform float4 uRows[4],"
            " uniform float4 uBias, uniform float uQuant, uniform float uAlphaOne, uniform float uDepthCopy) : COLOR {"
            " float4 c = tex2D(uEfb, vUv);"
            " if (uDepthCopy > 0.5) {"
            " float z = min(floor(saturate(c.r) * 16777215.0 + 0.5), 16777215.0);"
            " float hi = floor(z / 65536.0); float mid = floor(z / 256.0) - hi * 256.0;"
            " c = float4(hi, mid, z - floor(z / 256.0) * 256.0, 255.0) / 255.0; }"
            " c.a = lerp(c.a, 1.0, uAlphaOne);"
            " float4 o = saturate(float4(dot(uRows[0], c), dot(uRows[1], c), dot(uRows[2], c), dot(uRows[3], c)) + uBias);"
            " float4 q = min(floor(o * 16.0) / 15.0, 1.0);"
            " return lerp(o, q, uQuant); }");
        if (vs != 0 && fs != 0) {
            program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            glLinkProgram(program);
            GLint ok = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (ok == 0) {
                glDeleteProgram(program);
                program = 0;
            } else {
                a_pos = glGetAttribLocation(program, "aPosition");
                a_uv = glGetAttribLocation(program, "aTexcoord0");
                u_tex = glGetUniformLocation(program, "uEfb");
                u_m = glGetUniformLocation(program, "uRows");
                u_bias = glGetUniformLocation(program, "uBias");
                u_q = glGetUniformLocation(program, "uQuant");
                u_a1 = glGetUniformLocation(program, "uAlphaOne");
                u_depth = glGetUniformLocation(program, "uDepthCopy");
            }
        }
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        if (program == 0 || a_pos < 0 || a_uv < 0) {
            program_failed = true;
            std::printf("mkwv: EFB copy conversion shader failed; falling back to CPU readback\n");
            return false;
        }
    }
    if (program == 0) {
        return false;
    }
    const std::uint32_t phys = e.dest_guest & 0x3FFFFFFFu;
    EfbTexture& slot = efb_slot_for(phys, e.dest_host);
    const std::uint32_t tw = e.dst_w > 1024u ? 1024u : e.dst_w;
    const std::uint32_t th = e.dst_h > 1024u ? 1024u : e.dst_h;
    const GLuint dst_fbo = prepare_copy_slot(slot, tw, th);
    if (dst_fbo == 0) {
        return false;
    }
    const float rw = static_cast<float>(g_rt_w), rh = static_cast<float>(g_rt_h);
    const float u0 = static_cast<float>(gx) / rw, u1 = static_cast<float>(gx + gw) / rw;
    const float vb = static_cast<float>(gy + gh) / rh, vt = static_cast<float>(gy) / rh;
    const GLfloat verts[16] = {-1.0f, -1.0f, u0, vb, 1.0f, -1.0f, u1, vb, 1.0f, 1.0f, u1, vt, -1.0f, 1.0f, u0, vt};
    static const GLushort quad[6] = {0, 1, 2, 0, 2, 3};
    const CopyConvert cv = copy_convert_for(e.format);

    GLuint source = g_efb_color;
    const bool copy_depth = depth_copy_format(e.format);
    if (copy_depth) {
        // Alias the source depth surface before switching targets. vitaGL stores
        // it at scene end; the conversion draw then reads that completed surface.
        // This texture owns no depth allocation and is never used as an FBO target.
        static GLuint depth_source = 0;
        if (!depth_source) glGenTextures(1, &depth_source);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, depth_source);
        vglTexImageDepthBuffer(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        source = depth_source;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
    glViewport(0, 0, static_cast<GLsizei>(tw), static_cast<GLsizei>(th));
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    if (u_depth >= 0) glUniform1f(u_depth, copy_depth ? 1.0f : 0.0f);
    if (u_tex >= 0) glUniform1i(u_tex, 0);
    if (u_m >= 0) glUniform4fv(u_m, 4, cv.m);  // four rows, one float4 each
    if (u_bias >= 0) glUniform4fv(u_bias, 1, cv.bias);
    if (u_q >= 0) glUniform1f(u_q, cv.quant4 ? 1.0f : 0.0f);
    if (u_a1 >= 0) glUniform1f(u_a1, pixel_fmt_has_alpha(e.pixel_fmt) ? 0.0f : 1.0f);
    use_array_buffer(0);
    glEnableVertexAttribArray(static_cast<GLuint>(a_pos));
    glEnableVertexAttribArray(static_cast<GLuint>(a_uv));
    glVertexAttribPointer(static_cast<GLuint>(a_pos), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
    glVertexAttribPointer(static_cast<GLuint>(a_uv), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, quad);
    glDisableVertexAttribArray(static_cast<GLuint>(a_uv));
    glDisableVertexAttribArray(static_cast<GLuint>(a_pos));
    glUseProgram(0);
    g_efb_draw_bound = false;
    invalidate_gl_state_cache();
    slot.dest = e.dest_host;
    slot.phys = phys;
    slot.format = e.format;
    slot.stamp = ++g_efb_stamp;
    return true;
}

// Effects level (set_effects_level): 2 = every copy, 1 = copies other than plain colour
// snapshots (RGBA8/RGB5A3/RGB565 - models, portraits, light textures) are skipped. A skipped
// copy leaves a transparent-black texture behind, so an additive effect sampling it (bloom)
// simply adds nothing instead of sampling whatever is in guest memory.
int g_effects_level = 2;

bool copy_is_effect(std::uint32_t fmt) {
    return fmt != TexRGBA8 && fmt != TexRGB5A3 && fmt != 0x4u;
}

void skip_efb_copy(const EfbCopyEvent& e) {
    const std::uint32_t phys = e.dest_guest & 0x3FFFFFFFu;
    EfbTexture& slot = efb_slot_for(phys, e.dest_host);
    const std::uint32_t tw = e.dst_w > 1024u ? 1024u : e.dst_w;
    const std::uint32_t th = e.dst_h > 1024u ? 1024u : e.dst_h;
    if (slot.id == 0 || slot.w != tw || slot.h != th || slot.phys != phys) {
        if (slot.id == 0) glGenTextures(1, &slot.id);
        static std::uint8_t zeros[256 * 256 * 4];
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, slot.id);
        // Small is enough: the texture is uniform, and a sampled texture's size is its own.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw > 256u ? 256 : static_cast<GLsizei>(tw),
                     th > 256u ? 256 : static_cast<GLsizei>(th), 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros);
        for (CopyFbo& c : g_copy_fbos) {
            if (c.tex == slot.id) {
                c.tex = 0;
            }
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        slot.w = tw;
        slot.h = th;
        slot.fbo_bound = false;
    }
    slot.dest = e.dest_host;
    slot.phys = phys;
    slot.format = e.format;
    slot.stamp = ++g_efb_stamp;
    invalidate_gl_state_cache();
}

void clear_copy_channels(const EfbCopyEvent& e, int gx, int gy, int gw, int gh);

// Copy clears deferred across a run of consecutive copies. Each clear draws into the EFB
// target, and every switch back to it from a copy's own target restarts the EFB's GXM scene
// (reloading its colour and depth). A run of copies - MKW's light textures, its post-effect
// chain - therefore clears once at the end instead of bouncing between targets per copy. A
// copy whose source overlaps a pending clear flushes the clears first, so it still sees them.
struct PendingClear {
    EfbCopyEvent e;
    int x, y, w, h;
};
constexpr unsigned kMaxPendingClears = kMaxEfbCopyEvents;
PendingClear g_pending_clears[kMaxPendingClears];
unsigned g_pending_clear_count = 0;

void flush_copy_clears() {
    for (unsigned i = 0; i < g_pending_clear_count; ++i) {
        const PendingClear& c = g_pending_clears[i];
        clear_copy_channels(c.e, c.x, c.y, c.w, c.h);
    }
    g_pending_clear_count = 0;
}

void queue_copy_clear(const EfbCopyEvent& e, int gx, int gy, int gw, int gh, bool defer) {
    if (!e.clear_color && !e.clear_alpha && !e.clear_depth) {
        return;
    }
    if (!defer || g_pending_clear_count >= kMaxPendingClears) {
        flush_copy_clears();
        clear_copy_channels(e, gx, gy, gw, gh);
        return;
    }
    g_pending_clears[g_pending_clear_count++] = PendingClear{e, gx, gy, gw, gh};
}

bool overlaps_pending_clear(int gx, int gy, int gw, int gh) {
    for (unsigned i = 0; i < g_pending_clear_count; ++i) {
        const PendingClear& c = g_pending_clears[i];
        if (gx < c.x + c.w && c.x < gx + gw && gy < c.y + c.h && c.y < gy + gh) {
            return true;
        }
    }
    return false;
}

void perform_efb_copy_impl(const EfbCopyEvent& e, bool defer);

void perform_efb_copy(const EfbCopyEvent& e) {
    perform_efb_copy_impl(e, false);
}

void perform_efb_copy_deferred(const EfbCopyEvent& e) {
    perform_efb_copy_impl(e, true);
}

// Temporary race diagnostic: retain batch checkpoints on the GPU and show them
// as a contact sheet. macOS Vita3K cannot resolve these surfaces into guest RAM.
struct RaceProbeSlot { GLuint texture = 0, fbo = 0; };
RaceProbeSlot g_race_probe[8];
unsigned g_race_probe_count = 0;
bool g_race_probe_sheet = false;
constexpr int kProbeW = 256, kProbeH = 224;

void capture_race_probe() {
    if (capture_batch_count() == 1) g_race_probe_count = 0;
    if (g_race_probe_count == 8) return;
    auto& slot = g_race_probe[g_race_probe_count];
    if (!slot.fbo) {
        glGenTextures(1, &slot.texture);
        glBindTexture(GL_TEXTURE_2D, slot.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kProbeW, kProbeH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &slot.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
            invalidate_gl_state_cache();
            return;
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitNamedFramebuffer(g_efb_fbo, slot.fbo, 0, 0, g_rt_w, g_rt_h,
                          0, 0, kProbeW, kProbeH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    std::printf("mkwv: race GPU probe tile=%u batch_end (GPU-only)\n", g_race_probe_count++);
    invalidate_gl_state_cache();
}

// Use an explicit same-format transfer: regular RGBA glReadPixels only copies
// backing RAM, while an RGB transfer requires a conversion Vita3K rejects.
// On Vita3K this still requires working surface synchronization; zero pixels
// alone do not prove the GPU rendered black.
void capture_gpu_checkpoint(const char* label) {
    if (!capture_active() || !g_efb_target) return;
    if (std::strstr(capture_directory(), "capture_auto_race")) {
        if (g_race_probe_sheet && std::strcmp(label, "batch_end") == 0) capture_race_probe();
        return;
    }
    static auto* rgba = static_cast<std::uint8_t*>(vglForceAlloc(kEfbTargetW * kEfbTargetH * 4));
    if (!rgba) {
        std::printf("mkwv: GPU checkpoint %s allocation failed\n", label);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    // vglReadPixels does not end the active scene itself.
    glFinish();
    vglReadPixels(0, 0, g_rt_w, g_rt_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    unsigned nonzero = 0;
    const unsigned pixels = static_cast<unsigned>(g_rt_w * g_rt_h);
    for (unsigned i = 0; i < pixels; ++i)
        nonzero += (rgba[i * 4] | rgba[i * 4 + 1] | rgba[i * 4 + 2]) != 0;
    std::printf("mkwv: GPU checkpoint %s nonzero=%u/%u transfer=RGBA-mapped\n", label, nonzero, pixels);
    capture_gpu_image(label, rgba, g_rt_w, g_rt_h);
    invalidate_gl_state_cache();
}

void perform_efb_copy_impl(const EfbCopyEvent& e, bool defer) {
    if (e.format == TexRGB5A3 && e.src_w == 256 && e.src_h == 256) {
        static unsigned model_events = 0;
        if (model_events++ < 4) {
            std::printf("mkwv: model EFB event src=%d,%d %dx%d dst=%ux%u dest=%08X host=%p rt=%dx%d scale=%.3f\n",
                        e.src_x, e.src_y, e.src_w, e.src_h, e.dst_w, e.dst_h, e.dest_guest,
                        static_cast<const void*>(e.dest_host), g_rt_w, g_rt_h, static_cast<double>(g_fb_scale));
        }
    }
    if (e.src_w <= 0 || e.src_h <= 0 || e.dst_w == 0 || e.dst_h == 0 || e.dest_host == nullptr) {
        return;
    }
    // EFB rect -> framebuffer rect. GL's origin is bottom-left, the EFB's top-left.
    int gx = 0, gy = 0, gw = 0, gh = 0;
    efb_to_gl(static_cast<float>(e.src_x), static_cast<float>(e.src_y), static_cast<float>(e.src_w),
              static_cast<float>(e.src_h), &gx, &gy, &gw, &gh);
    if (gx < 0) { gw += gx; gx = 0; }
    if (gy < 0) { gh += gy; gy = 0; }
    if (gx + gw > g_rt_w) { gw = g_rt_w - gx; }
    if (gy + gh > g_rt_h) { gh = g_rt_h - gy; }
    if (gw <= 0 || gh <= 0) {
        return;
    }
    const bool efb_alpha = pixel_fmt_has_alpha(e.pixel_fmt);
    perf::Scope timing(perf::kCopy);
    if (overlaps_pending_clear(gx, gy, gw, gh)) {
        flush_copy_clears();  // this copy must see the earlier copies' clears
    }
    if (capture_active()) {
        char label[80];
        std::snprintf(label, sizeof(label), "before_d%u_fmt%u_%08X", e.draw_index, e.format, e.dest_guest);
        capture_gpu_checkpoint(label);
    }
    if (g_efb_target && g_effects_level < 2 && copy_is_effect(e.format)) {
        skip_efb_copy(e);
        perf::count(perf::kSkippedCopies);
        ++g_dbg.efbcopy;
        queue_copy_clear(e, gx, gy, gw, gh, defer);
        return;
    }
    const bool on_gpu = copy_efb_color_gpu(e, gx, gy, gw, gh) || copy_efb_convert_gpu(e, gx, gy, gw, gh);
    perf::count(on_gpu ? perf::kGpuCopies : perf::kCpuCopies);
    if (!on_gpu) {
        // Which buffer holds the EFB image this copy observes. With the EFB target that is
        // always the target itself: it persists, and a new frame's clear waits until the
        // frame's leading copies are done. Without it every frame is cleared, submitted and
        // swapped, so a copy recorded before any draw of its frame - including one alone in a
        // drain with no draws at all - is looking at what the previous frame drew, and that is
        // now the FRONT buffer.
        const bool from_front = !g_efb_target && e.draw_index == 0 && !g_frame_open;
        if (!g_efb_target) {
            glReadBuffer(from_front ? GL_FRONT : GL_BACK);
        }
        // vitaGL's readback writes its result at the rect's own (x, y) offset inside a buffer
        // only width x height large (sceGxmTransferCopy's destination offset), so any rect not
        // at the origin came back as unrelated memory. Read from the origin instead and crop
        // the desired columns from the wider image. The row offset depends on vitaGL's
        // FBO versus default-framebuffer readback ordering below.
        static std::uint8_t readback_full[(kDisplayW > kEfbTargetW ? kDisplayW : kEfbTargetW) *
                                          (kDisplayH > kEfbTargetH ? kDisplayH : kEfbTargetH) * 4];
        const int full_w = gx + gw;
        glReadPixels(0, 0, full_w, gy + gh, GL_RGBA, GL_UNSIGNED_BYTE, readback_full);
        if (!g_efb_target) {
            glReadBuffer(GL_BACK);
        }
        // vitaGL stores an FBO readback top-down. The requested GL rectangle ends at
        // gy+gh, so the EFB rect starts at row zero of that readback. The default
        // framebuffer readback is bottom-up, where its first row is GL y=0 and the
        // desired rect instead starts at row gy.
        const std::size_t first_row = g_efb_target ? 0u : static_cast<std::size_t>(gy);
        const std::uint8_t* readback = readback_full + (first_row * full_w + gx) * 4u;
        if (from_front) {
            ++g_efb_total_front;
        }

        // Resample to the destination size (the guest already halved it for a mipmapped copy),
        // then convert to the copy format. vitaGL's FBO glReadPixels fast path writes rows
        // top-down (framebuffers.c), while its default-framebuffer path writes bottom-up.
        static std::uint8_t texels[1024 * 1024 * 4];
        const std::uint32_t tw = e.dst_w > 1024u ? 1024u : e.dst_w;
        const std::uint32_t th = e.dst_h > 1024u ? 1024u : e.dst_h;
        for (std::uint32_t ty = 0; ty < th; ++ty) {
            const int y0 = static_cast<int>(static_cast<std::uint64_t>(ty) * gh / th);
            int y1 = static_cast<int>(static_cast<std::uint64_t>(ty + 1u) * gh / th);
            if (y1 <= y0) y1 = y0 + 1;
            for (std::uint32_t tx = 0; tx < tw; ++tx) {
                const int x0 = static_cast<int>(static_cast<std::uint64_t>(tx) * gw / tw);
                int x1 = static_cast<int>(static_cast<std::uint64_t>(tx + 1u) * gw / tw);
                if (x1 <= x0) x1 = x0 + 1;
                std::uint32_t acc[4] = {0, 0, 0, 0};
                std::uint32_t n = 0;
                for (int yy = y0; yy < y1 && yy < gh; ++yy) {
                    const int row = g_efb_target ? yy : gh - 1 - yy;
                    for (int xx = x0; xx < x1 && xx < gw; ++xx) {
                        const std::uint8_t* px = readback + (static_cast<std::size_t>(row) * full_w + xx) * 4u;
                        acc[0] += px[0];
                        acc[1] += px[1];
                        acc[2] += px[2];
                        acc[3] += px[3];
                        ++n;
                    }
                }
                if (n == 0) n = 1;
                const std::uint8_t avg[4] = {static_cast<std::uint8_t>(acc[0] / n), static_cast<std::uint8_t>(acc[1] / n),
                                             static_cast<std::uint8_t>(acc[2] / n), static_cast<std::uint8_t>(acc[3] / n)};
                convert_copy_texel(e.format, efb_alpha, avg, texels + (static_cast<std::size_t>(ty) * tw + tx) * 4u);
            }
        }

        const std::uint32_t phys = e.dest_guest & 0x3FFFFFFFu;
        if (e.format == TexRGB5A3 && e.src_w == 256 && e.src_h == 256 && e.dst_w == 256 && e.dst_h == 256) {
            g_model_copy_phys = phys;
            if (g_model_copy_reports++ < 4) {
                std::uint32_t lit = 0;
                for (std::uint32_t i = 0; i < tw * th; ++i) {
                    const std::uint8_t* px = texels + static_cast<std::size_t>(i) * 4u;
                    lit += (px[0] | px[1] | px[2]) > 32u;
                }
                std::printf("mkwv: model EFB copy src=%d,%d %dx%d dest=%08X lit=%u/%u\n", e.src_x, e.src_y,
                            e.src_w, e.src_h, e.dest_guest, lit, tw * th);
            }
        }
        EfbTexture& slot = efb_slot_for(phys, e.dest_host);
        if (slot.id == 0) {
            glGenTextures(1, &slot.id);
        }
        slot.dest = e.dest_host;
        slot.phys = phys;
        slot.w = tw;
        slot.h = th;
        slot.format = e.format;
        slot.stamp = ++g_efb_stamp;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, slot.id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(tw), static_cast<GLsizei>(th), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, texels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        invalidate_gl_state_cache();
    }
    ++g_dbg.efbcopy;
    ++g_efb_total_done;

    queue_copy_clear(e, gx, gy, gw, gh, defer);
}

template <typename S>
void apply_depth(const S& state) {
    if (state.depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_compare(state.depth_func));
    } else if (state.depth_write) {
        // GL stops writing depth when the test is off; GX does not (Aurora gx.cpp:1609-1613).
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(state.depth_write ? GL_TRUE : GL_FALSE);
}

template <typename S>
void apply_color_mask(const S& state, std::uint8_t pixel_fmt) {
    const GLboolean c = state.color_update ? GL_TRUE : GL_FALSE;
    // Only an RGBA6 EFB stores alpha (Aurora gx.cpp:1562-1568); otherwise the framebuffer's
    // alpha stays at its cleared 1.0, which is what a later copy or DSTALPHA blend reads.
    const GLboolean a = (state.alpha_update && pixel_fmt_has_alpha(pixel_fmt)) ? GL_TRUE : GL_FALSE;
    glColorMask(c, c, c, a);
}

// Blend state per Aurora gx.cpp:1379-1537. vitaGL/GXM has no glLogicOp, so the logic ops
// map to the blend equations Aurora uses; NOOP is exact through the colour mask.
// Returns false when the draw must not touch colour at all.
// GL state the draw path re-sends constantly, cached so only changes reach vitaGL. Anything
// that touches this state behind the cache's back (EFB copies, clears, the present blit) calls
// invalidate_gl_state_cache().
struct GlStateCache {
    int blend_on = -1;
    GLenum blend_eq = 0;
    GLenum blend_src = 0, blend_dst = 0;
    GLuint program = 0xFFFFFFFFu;  // unknown
    bool attribs_known = false;
    std::uint32_t attribs = 0;     // enabled generic vertex attribute arrays
    int ffp_arrays = -1;           // FFP client arrays enabled (1), disabled (0), unknown (-1)
};
GlStateCache g_gl_cache;
std::uint32_t g_tex_generation = 1;  // bumped whenever a GL texture's contents may have changed

void invalidate_gl_state_cache() {
    g_gl_cache = GlStateCache{};
    ++g_tex_generation;
}

void c_blend_on(bool on) {
    const int v = on ? 1 : 0;
    if (g_gl_cache.blend_on != v) {
        g_gl_cache.blend_on = v;
        if (on) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }
    }
}

void c_blend_eq(GLenum eq) {
    if (g_gl_cache.blend_eq != eq) {
        g_gl_cache.blend_eq = eq;
        glBlendEquation(eq);
    }
}

void c_blend_func(GLenum src, GLenum dst) {
    if (g_gl_cache.blend_src != src || g_gl_cache.blend_dst != dst) {
        g_gl_cache.blend_src = src;
        g_gl_cache.blend_dst = dst;
        glBlendFunc(src, dst);
    }
}

void c_use_program(GLuint prog) {
    if (g_gl_cache.program != prog) {
        g_gl_cache.program = prog;
        glUseProgram(prog);
    }
}

void c_attribs(std::uint32_t want) {
    if (!g_gl_cache.attribs_known) {
        for (GLuint i = 0; i < 16u; ++i) {
            glDisableVertexAttribArray(i);
        }
        g_gl_cache.attribs = 0;
        g_gl_cache.attribs_known = true;
    }
    std::uint32_t diff = want ^ g_gl_cache.attribs;
    while (diff != 0) {
        const unsigned i = static_cast<unsigned>(__builtin_ctz(diff));
        diff &= diff - 1u;
        if ((want >> i) & 1u) {
            glEnableVertexAttribArray(i);
        } else {
            glDisableVertexAttribArray(i);
        }
    }
    g_gl_cache.attribs = want;
}

void c_ffp_arrays(bool on) {
    const int v = on ? 1 : 0;
    if (g_gl_cache.ffp_arrays == v) {
        return;
    }
    g_gl_cache.ffp_arrays = v;
    if (on) {
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    } else {
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
}

bool apply_blend(const DrawCall& draw, bool* color_masked) {
    const bool dst_alpha = pixel_fmt_has_alpha(draw.pixel_fmt);
    *color_masked = false;
    c_blend_eq(GL_FUNC_ADD);
    switch (draw.blend_mode) {
    case BlendSubtract:
        // GX ignores the factors in subtract mode: dst - src.
        c_blend_on(true);
        c_blend_eq(GL_FUNC_REVERSE_SUBTRACT);
        c_blend_func(GL_ONE, GL_ONE);
        return true;
    case BlendMix:
        c_blend_on(true);
        c_blend_func(gl_blend_factor(draw.blend_src, true, dst_alpha), gl_blend_factor(draw.blend_dst, false, dst_alpha));
        return true;
    case BlendLogic:
        ++g_dbg.logicop;
        switch (draw.logic_op) {
        case LoClear:
            c_blend_on(true);
            c_blend_func(GL_ZERO, GL_ZERO);
            return true;
        case LoNoop:
            c_blend_on(false);
            *color_masked = true;  // the draw writes depth only
            return true;
        case LoAndInverted:
            c_blend_on(true);
            c_blend_func(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
            return true;
        case LoOr:
            c_blend_on(true);
            c_blend_eq(GL_MAX);
            c_blend_func(GL_ONE, GL_ONE);
            return true;
        case LoSet:
            c_blend_on(true);
            c_blend_func(GL_ONE, GL_ONE);
            return true;
        default:
            c_blend_on(false);
            return true;
        }
    case BlendNone:
    default:
        c_blend_on(false);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Texture cache (Aurora gx.cpp:356-375, 1084-1117). Keyed on everything that changes the
// decoded pixels. GXInvalidateTexAll moves the decoder's epoch on; an entry from an older
// epoch is re-validated against a hash of its source bytes and only re-decoded if they
// changed, so a game that invalidates every frame does not pay a full re-decode.
struct TexCacheEntry {
    const void* src = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    std::uint32_t format = 0;
    unsigned levels = 1;
    const void* tlut_src = nullptr;
    std::uint32_t tlut_version = 0;
    std::uint32_t tlut_format = 0;
    std::uint32_t epoch = 0;
    std::uint32_t hash = 0;
    std::uint32_t last_used = 0;
    unsigned gl_levels = 1;  // levels present on the GL texture (1, or a generated chain)
    GLuint id = 0;
};

TexCacheEntry g_tex_cache[kTexCache];
std::uint32_t g_tex_clock = 0;

bool is_pow2(std::uint32_t v) {
    return v != 0 && (v & (v - 1u)) == 0;
}

// FNV-1a over the first 256 bytes and 64 samples spread over the rest: a cheap change
// detector for re-validation, not a content address.
std::uint32_t sample_hash(const std::uint8_t* p, std::uint32_t bytes) {
    std::uint32_t h = 2166136261u;
    auto mix = [&](std::uint8_t b) {
        h ^= b;
        h *= 16777619u;
    };
    const std::uint32_t head = bytes < 256u ? bytes : 256u;
    for (std::uint32_t i = 0; i < head; ++i) {
        mix(p[i]);
    }
    if (bytes > 256u) {
        const std::uint32_t step = (bytes - 256u) / 64u + 1u;
        for (std::uint32_t off = 256u; off + 4u <= bytes; off += step) {
            mix(p[off]);
            mix(p[off + 1u]);
            mix(p[off + 2u]);
            mix(p[off + 3u]);
        }
    }
    mix(static_cast<std::uint8_t>(bytes));
    mix(static_cast<std::uint8_t>(bytes >> 8));
    mix(static_cast<std::uint8_t>(bytes >> 16));
    return h;
}

std::uint32_t source_bytes(const TexObj& tex, unsigned levels) {
    const std::uint32_t fmt = sampled_format(tex.format);
    std::uint32_t w = tex.width, h = tex.height, total = 0;
    for (unsigned l = 0; l < levels; ++l) {
        total += tex_buffer_size(w, h, fmt);
        w = w > 1 ? w / 2u : 1u;
        h = h > 1 ? h / 2u : 1u;
    }
    return total;
}

// Sampler state is per bind, not per texture: two texobjs can share an image with different
// wrap or filter modes (Aurora builds a sampler per bind, gx.cpp:2126-2157).
void apply_sampler(const TexObj& tex, bool have_mips) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(gl_wrap(tex.wrap_s)));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(gl_wrap(tex.wrap_t)));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(gl_min_filter(tex.min_filter, have_mips)));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tex.mag_filter == TexNear ? GL_NEAREST : GL_LINEAR);
}

// Uploads (or finds) the GL texture for a texobj and leaves it bound to the active unit with
// this bind's sampler state. Returns the GL id, 0 on failure.
GLuint bind_texture(const TexObj& tex, const TlutObj* tlut) {
    if (tex.data == nullptr || tex.width == 0 || tex.height == 0) {
        return 0;
    }
    const EfbTexture* copied = efb_texture_for(tex);
    const bool model_bind = g_model_copy_phys != 0 &&
                            ((tex.phys & 0x3FFFFFFFu) == g_model_copy_phys ||
                             (copied != nullptr && copied->phys == g_model_copy_phys));
    if (model_bind && g_model_bind_reports++ < 4) {
        std::printf("mkwv: model EFB bind phys=%08X size=%ux%u fmt=%u hit=%d\n", tex.phys,
                    tex.width, tex.height, tex.format, copied != nullptr ? 1 : 0);
    }
    if (copied != nullptr) {
        ++g_dbg.efbhit;
        ++g_efb_total_hits;
        glBindTexture(GL_TEXTURE_2D, copied->id);
        TexObj clamp = tex;
        apply_sampler(clamp, false);
        return copied->id;
    }
    const std::uint32_t fmt = sampled_format(tex.format);
    const bool paletted = fmt == TexCI4 || fmt == TexCI8 || fmt == TexCI14X2;
    const void* tlut_src = (paletted && tlut != nullptr) ? tlut->data : nullptr;
    const std::uint32_t tlut_version = (paletted && tlut != nullptr) ? tlut->version : 0u;
    const std::uint32_t tlut_format = (paletted && tlut != nullptr) ? tlut->format : 0u;
    const unsigned fmt_slot = fmt & 0xFu;
    const unsigned levels = texture_level_count(tex);
    ++g_dbg.fmt_bind[fmt_slot];
    ++g_tex_clock;
    TexCacheEntry* hit = nullptr;
    for (unsigned i = 0; i < kTexCache; ++i) {
        TexCacheEntry& e = g_tex_cache[i];
        if (e.id != 0 && e.src == tex.data && e.width == tex.width && e.height == tex.height && e.format == fmt &&
            e.levels == levels && e.tlut_src == tlut_src && e.tlut_version == tlut_version &&
            e.tlut_format == tlut_format) {
            hit = &e;
            break;
        }
    }
    const std::uint32_t epoch = tex.version;
    if (hit != nullptr) {
        if (hit->epoch == epoch) {
            hit->last_used = g_tex_clock;
            glBindTexture(GL_TEXTURE_2D, hit->id);
            apply_sampler(tex, hit->gl_levels > 1);
            return hit->id;
        }
        // Invalidated since: same bytes -> keep the conversion.
        ++g_dbg.texrevalid;
        const std::uint32_t h = sample_hash(static_cast<const std::uint8_t*>(tex.data), source_bytes(tex, levels));
        if (h == hit->hash) {
            hit->epoch = epoch;
            hit->last_used = g_tex_clock;
            glBindTexture(GL_TEXTURE_2D, hit->id);
            apply_sampler(tex, hit->gl_levels > 1);
            return hit->id;
        }
    }
    // Whatever happens below changes what some GL texture holds (or evicts one): no unit's
    // memo of "this texobj is already bound" can be trusted afterwards.
    ++g_tex_generation;
    perf::count(perf::kTexDecodes);
    std::uint8_t* converted = converted_buffer();
    ConvertedTexture host{};
    const bool ok = converted != nullptr && convert_gx_texture(tex, converted, kConvertedCapacity, &host, tlut);
    if (!ok || host.data == nullptr || host.layout != HostRgba8) {
        g_dbg.tex_convert = 0;
        ++g_dbg.fmt_fail[fmt_slot];
        return 0;
    }
    ++g_dbg.texdecode;
    g_dbg.tex_convert = 1;
    g_dbg.tex_w = host.width;
    g_dbg.tex_h = host.height;
    g_dbg.tex_bytes = host.bytes;

    TexCacheEntry* e = hit;
    if (e == nullptr) {
        // Least recently used slot.
        e = &g_tex_cache[0];
        for (unsigned i = 0; i < kTexCache; ++i) {
            if (g_tex_cache[i].id == 0) {
                e = &g_tex_cache[i];
                break;
            }
            if (g_tex_cache[i].last_used < e->last_used) {
                e = &g_tex_cache[i];
            }
        }
    }
    if (e->id == 0) {
        glGenTextures(1, &e->id);
        if (e->id == 0) {
            return 0;
        }
    }
    glBindTexture(GL_TEXTURE_2D, e->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(host.width), static_cast<GLsizei>(host.height), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, host.data);
    // Mip levels: vitaGL builds the chain on the GPU. GX images carry their own levels, but
    // they are box-filtered reductions in practice, and generating them avoids depending on
    // per-level uploads.
    unsigned gl_levels = 1;
    if (host.mip_count > 1 && is_pow2(host.width) && is_pow2(host.height)) {
        glGenerateMipmap(GL_TEXTURE_2D);
        gl_levels = host.mip_count;
    }
    e->src = tex.data;
    e->width = tex.width;
    e->height = tex.height;
    e->format = fmt;
    e->levels = levels;
    e->tlut_src = tlut_src;
    e->tlut_version = tlut_version;
    e->tlut_format = tlut_format;
    e->epoch = epoch;
    e->hash = sample_hash(static_cast<const std::uint8_t*>(tex.data), source_bytes(tex, levels));
    e->last_used = g_tex_clock;
    e->gl_levels = gl_levels;
    apply_sampler(tex, gl_levels > 1);
    return e->id;
}

// A draw's texmap and TLUT, from its own snapshot. Falls back to processor.state only
// when there is no snapshot (a hand-built DrawCall).
const TexObj& texmap_of(const Processor& processor, const DrawExtra* x, unsigned t) {
    return x != nullptr ? x->tex[t] : processor.state.texmaps[t];
}

const TlutObj* tlut_of(const Processor& processor, const DrawExtra* x, unsigned t) {
    if (x != nullptr) {
        return x->tlut_valid[t] ? &x->tlut[t] : nullptr;
    }
    const TexObj& tex = processor.state.texmaps[t];
    if (tex.tlut < kMaxTluts && processor.state.tluts[tex.tlut].data != nullptr) {
        return &processor.state.tluts[tex.tlut];
    }
    return nullptr;
}

// Kept for the tests: the old entry point, binding to the active unit.
GLuint upload_texture(const TexObj& tex, const TlutObj* tlut) {
    return bind_texture(tex, tlut);
}

// Batch vertex buffer. Every vertex of a batch is packed once, interleaved, with only as many
// texture coordinate sets as the batch's draws use: position (4 floats), COLOR0, COLOR1
// (4 bytes each), then `ntc` x (s, t, q). vitaGL copies every vertex it draws, so the
// stride matters - a fixed layout with all eight texcoord sets was 128 bytes a vertex.
struct VertexLayout {
    unsigned ntc = 1;
    unsigned stride = 24u + 12u;
    bool lit = false;       // GPU-lit: eye position and normal follow the texcoords
    unsigned off_eye = 0;
};
constexpr unsigned kVertOffColor0 = 16u;
constexpr unsigned kVertOffColor1 = 20u;
constexpr unsigned kVertOffTex = 24u;
VertexLayout g_vlayout;
alignas(16) std::uint8_t g_vbuf[kMaxDecodedVerts * (24u + 12u * kMaxTexGens + kPackLitBytes)];

VertexLayout layout_for(unsigned ntc, bool lit = false) {
    VertexLayout l;
    l.ntc = ntc == 0 ? 1u : (ntc > kMaxTexGens ? kMaxTexGens : ntc);
    l.stride = kVertOffTex + 12u * l.ntc;
    l.lit = lit;
    l.off_eye = l.stride;
    if (lit) {
        l.stride += kPackLitBytes;
    }
    return l;
}

void pack_vertices(const Processor& processor, std::uint32_t first, std::uint32_t count, std::uint8_t* out,
                   const VertexLayout& l) {
    for (std::uint32_t v = 0; v < count; ++v) {
        const DecodedVertex& src = processor.verts[first + v];
        std::uint8_t* o = out + static_cast<std::size_t>(v) * l.stride;
        const float pos[4] = {src.x, src.y, src.z, src.w};
        std::memcpy(o, pos, 16);
        // DecodedVertex colours are r | g<<8 | b<<16 | a<<24: already RGBA byte order in memory
        // on a little-endian host.
        std::memcpy(o + kVertOffColor0, &src.color, 4);
        std::memcpy(o + kVertOffColor1, &src.color1, 4);
        std::memcpy(o + kVertOffTex, src.tc, 12u * l.ntc);
        if (l.lit) {
            const float extra[6] = {src.bx, src.by, src.bz, src.tx, src.ty, src.tz};
            std::memcpy(o + l.off_eye, extra, sizeof(extra));
        }
    }
}

void unpack_color(std::uint32_t color, std::uint8_t* rgba) {
    rgba[0] = static_cast<std::uint8_t>(color);
    rgba[1] = static_cast<std::uint8_t>(color >> 8);
    rgba[2] = static_cast<std::uint8_t>(color >> 16);
    rgba[3] = static_cast<std::uint8_t>(color >> 24);
}

// ---------------------------------------------------------------------------
// GX TEV stage -> GL_COMBINE
//
// VitaGL compiles GL_COMBINE into a real fragment shader
// (third_party/vitaGL/source/shaders/tex_env.h) and we never define
// DISABLE_TEXTURE_COMBINER, so the whole combiner is ours to use. GX computes
//
//     out = d + lerp(a, b, c)     where lerp(a, b, c) = a*(1-c) + b*c
//
// and GL_INTERPOLATE computes Arg0*Arg2 + Arg1*(1-Arg2) - the same expression with
// Arg0 = b, Arg1 = a, Arg2 = c.
//
// The part GL_MODULATE/GL_REPLACE could never express is that GX's colour and alpha
// have *independent* argument sets. GL_REPLACE forces alpha = texture alpha, so a
// quad whose GX alpha comes from the rasterised colour - which is GX_COLOR_NULL, i.e.
// zero, on nearly every draw this game emits - was painted fully opaque instead of
// not at all. GL_COMBINE has separate GL_COMBINE_RGB and GL_COMBINE_ALPHA, so the two
// are programmed apart, the way GX means them.
//
// Anything this cannot express exactly falls back to the old env-mode path. It never
// approximates silently: cfail= in the SUBMIT line counts every fallback.

struct CombArg {
    GLint src = 0;
    GLint op = 0;
    bool ok = false;
};

struct CombinePlan {
    GLint func = GL_REPLACE;
    CombArg arg[3];
    unsigned nargs = 0;
    bool needs_texture = false;
    bool needs_konst = false;
    bool ok = false;
};

GLint invert_operand(GLint op) {
    switch (op) {
        case GL_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
        case GL_ONE_MINUS_SRC_COLOR: return GL_SRC_COLOR;
        case GL_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        default: return GL_SRC_ALPHA;
    }
}

// CPREV/APREV, the TEV colour registers and CC_HALF have no single-unit GL source, so
// they are left unmapped and take the draw down the fallback path.
CombArg decode_color_arg(std::uint8_t a) {
    switch (a) {
        case CcTexC: return {GL_TEXTURE, GL_SRC_COLOR, true};
        case CcTexA: return {GL_TEXTURE, GL_SRC_ALPHA, true};
        case CcRasC: return {GL_PRIMARY_COLOR, GL_SRC_COLOR, true};
        case CcRasA: return {GL_PRIMARY_COLOR, GL_SRC_ALPHA, true};
        case CcKonst: return {GL_CONSTANT, GL_SRC_COLOR, true};
        default: return {};
    }
}

CombArg decode_alpha_arg(std::uint8_t a) {
    switch (a) {
        case CaTexA: return {GL_TEXTURE, GL_SRC_ALPHA, true};
        case CaRasA: return {GL_PRIMARY_COLOR, GL_SRC_ALPHA, true};
        case CaKonst: return {GL_CONSTANT, GL_SRC_ALPHA, true};
        default: return {};
    }
}

// zero/one are the ZERO and ONE selector values for whichever of the two arg
// namespaces this is; alpha has no ONE, so it passes an unused sentinel.
CombinePlan plan_combine(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint8_t op,
                         std::uint8_t bias, bool clamp, std::uint8_t zero, std::uint8_t one,
                         CombArg (*decode)(std::uint8_t)) {
    CombinePlan plan;
    if (bias != 0 || !clamp || (op != TevAdd && op != TevSub)) {
        return plan;
    }
    auto take = [&](const CombArg& ca) {
        if (!ca.ok) {
            return false;
        }
        if (ca.src == GL_TEXTURE) {
            plan.needs_texture = true;
        }
        if (ca.src == GL_CONSTANT) {
            plan.needs_konst = true;
        }
        plan.arg[plan.nargs++] = ca;
        return true;
    };

    // lerp(a, b, c) collapses to a single term whenever c is a constant end point or
    // the two ends agree. Those are the cases GL's two-argument functions cover.
    bool single = false;
    std::uint8_t term = zero;
    if (c == zero) {
        single = true;
        term = a;
    } else if (c == one && one != 0xFFu) {
        single = true;
        term = b;
    } else if (a == b) {
        single = true;
        term = a;
    } else if (a == zero && b == zero) {
        single = true;
        term = zero;
    }

    if (single) {
        const bool term_zero = term == zero;
        const bool d_zero = d == zero;
        if (term_zero && d_zero) {
            return plan;  // constant zero - no GL source produces it
        }
        if (term_zero) {
            plan.func = GL_REPLACE;
            plan.ok = take(decode(d));
            return plan;
        }
        if (d_zero) {
            if (op == TevSub) {
                return plan;  // negative output, nothing to clamp it against
            }
            plan.func = GL_REPLACE;
            plan.ok = take(decode(term));
            return plan;
        }
        plan.func = op == TevSub ? GL_SUBTRACT : GL_ADD;
        plan.ok = take(decode(d)) && take(decode(term));
        return plan;
    }

    // A real interpolation. GX would add d on top; GL has no fourth argument, so a
    // non-zero d is a fallback.
    if (d != zero || op != TevAdd) {
        return plan;
    }
    if (a == zero) {
        plan.func = GL_MODULATE;  // out = b*c
        plan.ok = take(decode(b)) && take(decode(c));
        return plan;
    }
    if (b == zero) {
        const CombArg ac = decode(a);
        CombArg cc = decode(c);
        if (!ac.ok || !cc.ok) {
            return plan;
        }
        cc.op = invert_operand(cc.op);  // out = a*(1-c)
        plan.func = GL_MODULATE;
        plan.ok = take(ac) && take(cc);
        return plan;
    }
    plan.func = GL_INTERPOLATE;  // Arg0*Arg2 + Arg1*(1-Arg2)
    plan.ok = take(decode(b)) && take(decode(a)) && take(decode(c));
    return plan;
}

GLint shift_to_scale(std::uint8_t shift) {
    switch (shift) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 4;
        default: return 0;  // GX's divide-by-two; vitaGL only accepts 1/2/4
    }
}

// The stage that actually produces the frame-buffer colour. Everything MKW emits here
// is a PASSCLR dummy in stage 0 followed by the real material in stage 1, so the last
// stage is the one to program - but only when it does not read CPREV/APREV, because
// then the earlier stages genuinely feed it and one GL unit is not enough.
unsigned effective_stage(const DrawCall& draw) {
    unsigned n = draw.num_tev_stages == 0 ? 1u : static_cast<unsigned>(draw.num_tev_stages);
    if (n > kTevStages) {
        n = kTevStages;
    }
    return n - 1u;
}

bool stage_reads_prev(const TevStage& s) {
    const std::uint8_t ca[4] = {s.color_a, s.color_b, s.color_c, s.color_d};
    const std::uint8_t aa[4] = {s.alpha_a, s.alpha_b, s.alpha_c, s.alpha_d};
    for (unsigned i = 0; i < 4u; ++i) {
        if (ca[i] == CcCPrev || ca[i] == CcAPrev || aa[i] == CaAPrev) {
            return true;
        }
    }
    return false;
}

const GLenum kSrcRgb[3] = {GL_SRC0_RGB, GL_SRC1_RGB, GL_SRC2_RGB};
const GLenum kOpRgb[3] = {GL_OPERAND0_RGB, GL_OPERAND1_RGB, GL_OPERAND2_RGB};
const GLenum kSrcAlpha[3] = {GL_SRC0_ALPHA, GL_SRC1_ALPHA, GL_SRC2_ALPHA};
const GLenum kOpAlpha[3] = {GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA, GL_OPERAND2_ALPHA};

// Programs the unit and reports whether it took the job. A false return leaves the
// unit untouched, so the caller's env-mode fallback is still valid.
bool apply_tev_combine(const DrawCall& draw, bool have_texture) {
    const unsigned si = effective_stage(draw);
    const TevStage& s = draw.tev_stages[si];
    if (si != 0 && stage_reads_prev(s)) {
        return false;
    }
    const GLint rgb_scale = shift_to_scale(s.color_shift);
    const GLint a_scale = shift_to_scale(s.alpha_shift);
    if (rgb_scale == 0 || a_scale == 0) {
        return false;
    }
    const CombinePlan cp = plan_combine(s.color_a, s.color_b, s.color_c, s.color_d, s.color_op, s.color_bias,
                                        s.color_clamp, CcZero, CcOne, decode_color_arg);
    const CombinePlan ap = plan_combine(s.alpha_a, s.alpha_b, s.alpha_c, s.alpha_d, s.alpha_op, s.alpha_bias,
                                        s.alpha_clamp, CaZero, 0xFFu, decode_alpha_arg);
    if (!cp.ok || !ap.ok) {
        return false;
    }
    // The combiner only runs on an enabled texture unit, so a stage that samples nothing
    // has no business here - its output is the vertex colour and the fallback path draws
    // that correctly.
    //
    // The stage's own tex_enabled is deliberately NOT a condition. MKW's materials set
    // GX_TEXMAP_NULL on the stage that names TEXC (measured: notex=247 of 262 draws on
    // the effective stage), and refusing those sent every game draw back to the env-mode
    // path - comb=0 cfail=245. have_texture already means this draw bound a texture, and
    // the env-mode path it would fall back to samples that same texture anyway, so taking
    // the stage here cannot be worse for colour and is the whole point for alpha.
    const bool wants_texture = cp.needs_texture || ap.needs_texture;
    if (!have_texture || !wants_texture) {
        return false;
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, cp.func);
    for (unsigned i = 0; i < cp.nargs && i < 3u; ++i) {
        glTexEnvi(GL_TEXTURE_ENV, static_cast<GLenum>(kSrcRgb[i]), cp.arg[i].src);
        glTexEnvi(GL_TEXTURE_ENV, static_cast<GLenum>(kOpRgb[i]), cp.arg[i].op);
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, ap.func);
    for (unsigned i = 0; i < ap.nargs && i < 3u; ++i) {
        glTexEnvi(GL_TEXTURE_ENV, static_cast<GLenum>(kSrcAlpha[i]), ap.arg[i].src);
        glTexEnvi(GL_TEXTURE_ENV, static_cast<GLenum>(kOpAlpha[i]), ap.arg[i].op);
    }
    // The scales go through glTexEnvfv deliberately. VitaGL's glTexEnvi writes
    // tex_unit->rgb_scale / a_scale but never calls flag_dirty_frag_unif for them
    // (ffp.c:3019, 3027), so a scale set that way is stored and never uploaded; only
    // the fv/xv paths flag the uniform. glTexEnvf is not a way out either - it just
    // casts to GLint and calls glTexEnvi.
    GLfloat scale_rgb = static_cast<GLfloat>(rgb_scale);
    GLfloat scale_a = static_cast<GLfloat>(a_scale);
    glTexEnvfv(GL_TEXTURE_ENV, GL_RGB_SCALE, &scale_rgb);
    glTexEnvfv(GL_TEXTURE_ENV, GL_ALPHA_SCALE, &scale_a);
    if (cp.needs_konst || ap.needs_konst) {
        // kcsel/kasel also encode constant fractions and per-channel selects; only the
        // four whole konst registers are mapped here, and the rest never reach this
        // point because decode_*_arg refuses anything but CcKonst/CaKonst.
        const unsigned kc = s.kcsel >= 12u && s.kcsel <= 15u ? s.kcsel - 12u : 0u;
        const std::uint32_t k = draw.konst[kc & 3u];
        GLfloat env[4] = {static_cast<GLfloat>(k & 0xFFu) / 255.0f,
                          static_cast<GLfloat>((k >> 8) & 0xFFu) / 255.0f,
                          static_cast<GLfloat>((k >> 16) & 0xFFu) / 255.0f,
                          static_cast<GLfloat>((k >> 24) & 0xFFu) / 255.0f};
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env);
    }
    return true;
}

const TexObj* draw_tex(const Processor& processor, const DrawCall& draw, const DrawExtra* x, unsigned* map_out) {
    if (!draw.tex0) {
        return nullptr;
    }
    unsigned map = 0;
    const unsigned si = effective_stage(draw);
    if (draw.tev_stages[si].texmap < kMaxTexMaps) {
        map = draw.tev_stages[si].texmap;
    } else if (draw.tev_stages[0].texmap < kMaxTexMaps) {
        map = draw.tev_stages[0].texmap;
    }
    const TexObj& tex = texmap_of(processor, x, map);
    if (tex.data != nullptr) {
        *map_out = map;
        return &tex;
    }
    *map_out = 0;
    return &texmap_of(processor, x, 0);
}

// ---------------------------------------------------------------------------
// Compiled TEV programs.
//
// VitaGL takes Cg directly: glCreateShader(GL_CG_*_SHADER_EXT) leaves is_glsl false, so
// glCompileShader hands the source to SceShaccCg with no GLSL translation, and VitaGL's
// own .gxp disk cache means a program compiled once survives across runs. glUseProgram(0)
// restores fixed function.
//
// The key is hash_tev_program - structure only. Register values ride in as uniforms.

constexpr unsigned kMaxPrograms = 448;  // vitaGL links up to 1024; GPU-lit variants (static63) add programs

struct TevProgram {
    std::uint64_t key = 0;
    GLuint prog = 0;
    bool used = false;
    bool failed = false;
    std::uint32_t last_used = 0;
    GLint a_pos = -1, a_col0 = -1, a_col1 = -1;
    GLint a_tc[kEmitTexCoordSets]{};
    GLint u_tex[kMaxTexMaps]{};
    GLint u_konst = -1, u_tevreg = -1, u_ind = -1, u_fogcol = -1, u_fogparam = -1, u_depthrange = -1;
    GLint u_indscale = -1, u_texsize = -1, u_dstalpha = -1;
    ShaderNeeds needs;
    // Last values sent for each uniform (vitaGL keeps uniforms per program), so an unchanged
    // value is not re-sent. Index: 0 konst, 1 tevreg, 2 fogcol, 3 fogparam, 4 depthrange,
    // 5 dstalpha, 6 ind, 7 indscale, 8 texsize.
    bool uvalid[9]{};
    GLfloat ukonst[16]{}, utevreg[16]{}, ufogcol[4]{}, ufogparam[4]{}, udepth[2]{}, udst[2]{};
    GLfloat uind[24]{}, uindscale[16]{}, utexsize[32]{};
    bool samplers_set = false;
    GLuint mesh_prog = 0;
    bool mesh_failed = false;
    GLint ma_pos = -1, ma_nrm = -1, ma_uv = -1, ma_uv1 = -1, ma_col = -1;
    GLint mu_pn = -1, mu_proj = -1, mu_nrm = -1, mu_tex = -1, mu_col1 = -1, mu_lit = -1, mu_src = -1;
    GLint mu_mat = -1, mu_amb = -1, mu_lpos = -1, mu_ldir = -1, mu_lcol = -1, mu_cos = -1, mu_dist = -1;
    GLint mm_konst = -1, mm_tevreg = -1, mm_fogc = -1, mm_fogp = -1, mm_depth = -1, mm_dst = -1;
    GLint mm_ind = -1, mm_indscale = -1, mm_texsize = -1;
    GLint mm_tex[kMaxTexMaps]{};
    // GPU-lit programs (emit_tev_vertex_cg_lit): extra attributes and the lighting uniforms.
    bool lit = false;
    GLint a_eye = -1, a_enrm = -1;
    GLint u_mat[2] = {-1, -1}, u_amb[2] = {-1, -1};
    GLint u_lp[kMaxLights]{}, u_ld[kMaxLights]{}, u_ls[kMaxLights]{}, u_lc[kMaxLights]{}, u_la[kMaxLights]{},
        u_lk[kMaxLights]{};
    bool ulit_valid = false;
    GpuLitParams ulit;
};

// The lighting structure a GPU-lit vertex program bakes in (values are uniforms).
std::uint64_t lit_structure_key(const GpuLitParams& p) {
    std::uint64_t h = 0x6C69746C69747631ull;  // "litlitv1"
    for (unsigned i = 0; i < 4u; ++i) {
        const std::uint64_t v = static_cast<std::uint64_t>(p.lit[i]) | static_cast<std::uint64_t>(p.mat_vtx[i]) << 8 |
                                static_cast<std::uint64_t>(p.amb_vtx[i]) << 16 |
                                static_cast<std::uint64_t>(p.diff_fn[i]) << 24 |
                                static_cast<std::uint64_t>(p.attn_fn[i]) << 32 |
                                static_cast<std::uint64_t>(p.light_mask[i]) << 40;
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return h;
}

// Set by the submit loop around draw_run: the lighting of the draw being issued, when its
// vertices were packed for GPU lighting.
const GpuLitParams* g_cur_lit = nullptr;
unsigned g_lit_program_failures = 0;

TevProgram g_programs[kMaxPrograms];
unsigned g_program_count = 0;
std::uint32_t g_program_clock = 0;
bool g_shader_path = true;
bool g_merge_draws = true;

// The per-draw TEV structure, as a State the emitter and the hash can read. The DrawCall
// snapshot is the authority: processor.state is the end-of-frame state.
const State& tev_state_for(const Processor& processor, const DrawCall& draw, const DrawExtra* x) {
    static State scratch;
    scratch.num_tev_stages = draw.num_tev_stages;
    scratch.num_tex_gens = draw.num_tex_gens;
    for (unsigned i = 0; i < kTevStages; ++i) {
        scratch.tev[i] = draw.tev_stages[i];
    }
    for (unsigned t = 0; t < kMaxTexGens; ++t) {
        scratch.texgens[t].type = x != nullptr ? x->tg_type[t] : processor.state.texgens[t].type;
    }
    std::memcpy(scratch.tev_swap, draw.tev_swap, sizeof(scratch.tev_swap));
    std::memcpy(scratch.ind, x != nullptr ? x->ind : processor.state.ind, sizeof(scratch.ind));
    scratch.num_ind_stages = x != nullptr ? x->num_ind_stages : processor.state.num_ind_stages;
    scratch.alpha_comp0 = draw.alpha_comp0;
    scratch.alpha_comp1 = draw.alpha_comp1;
    scratch.alpha_ref0 = draw.alpha_ref0;
    scratch.alpha_ref1 = draw.alpha_ref1;
    scratch.alpha_op = draw.alpha_op;
    scratch.fog = draw.fog;
    // Destination alpha only matters when alpha reaches the EFB at all (Aurora
    // effective_dst_alpha): an RGBA6 EFB with alpha updates on.
    {
        const bool alpha_update = x != nullptr ? x->alpha_update : processor.state.alpha_update;
        scratch.dst_alpha_enable = draw.dst_alpha_enable && alpha_update && pixel_fmt_has_alpha(draw.pixel_fmt);
    }
    for (unsigned i = 0; i < 4u; ++i) {
        scratch.tev_color[i] = draw.tev_color[i];
        scratch.konst[i] = draw.konst[i];
    }
    return scratch;
}

GLuint compile_cg(GLenum kind, const char* src) {
    const GLuint sh = glCreateShader(kind);
    if (sh == 0) {
        return 0;
    }
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// vitaGL's glClear uses an unmasked fragment program. Use a normal draw for
// colour/alpha clears so an alpha-only GX copy cannot erase the race colour.
void clear_copy_channels(const EfbCopyEvent& e, int gx, int gy, int gw, int gh) {
    if (!e.clear_color && !e.clear_alpha && !e.clear_depth) return;
    glEnable(GL_SCISSOR_TEST);
    glScissor(gx, gy, gw, gh);
    if (e.clear_depth) {
        glDepthMask(GL_TRUE);
        glClearDepthf(static_cast<float>(e.clear_z) / 16777215.0f);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
    if (e.clear_color || e.clear_alpha) {
        static GLuint program = 0;
        static GLint position = -1, color = -1;
        if (!program) {
            // GL_CG_*_SHADER_EXT: vitaGL runs plain GL_VERTEX_SHADER / GL_FRAGMENT_SHADER source
            // through its GLSL translator (custom_shaders.c glCreateShader sets is_glsl), so this Cg
            // never compiled and every colour/alpha copy clear was silently skipped. In a race that
            // left the EFB alpha at the frame clear's 1.0 under the course (whose draws do not write
            // alpha), and the shadow-volume composite (a full-screen black quad blended by DSTALPHA)
            // then blacked out everything but the objects, which write alpha 0.
            const GLuint vs = compile_cg(GL_CG_VERTEX_SHADER_EXT,
                "void main(float2 aPosition, float4 out vPosition : POSITION) {"
                " vPosition = float4(aPosition, 0.0, 1.0); }");
            const GLuint fs = compile_cg(GL_CG_FRAGMENT_SHADER_EXT,
                "float4 main(uniform float4 uColor) : COLOR { return uColor; }");
            if (vs && fs) {
                program = glCreateProgram();
                glAttachShader(program, vs);
                glAttachShader(program, fs);
                glLinkProgram(program);
                GLint ok = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &ok);
                if (!ok) { glDeleteProgram(program); program = 0; }
                else {
                    position = glGetAttribLocation(program, "aPosition");
                    color = glGetUniformLocation(program, "uColor");
                }
            }
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
        }
        if (program && position >= 0) {
            static const GLfloat vertices[] = {-1,-1, 1,-1, 1,1, -1,1};
            static const GLushort indices[] = {0,1,2, 0,2,3};
            glViewport(0, 0, g_rt_w, g_rt_h);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);
            glDisable(GL_ALPHA_TEST);
            glColorMask(e.clear_color, e.clear_color, e.clear_color, e.clear_alpha);
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
            glUseProgram(program);
            glUniform4f(color, (e.clear_rgba & 255u) / 255.0f,
                ((e.clear_rgba >> 8) & 255u) / 255.0f,
                ((e.clear_rgba >> 16) & 255u) / 255.0f,
                ((e.clear_rgba >> 24) & 255u) / 255.0f);
            use_array_buffer(0);
            glEnableVertexAttribArray(position);
            glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
            glDisableVertexAttribArray(position);
            glUseProgram(0);
        } else {
            static bool reported = false;
            if (!reported) { std::printf("mkwv: masked EFB clear shader failed\n"); reported = true; }
            ++g_copy_clear_failures;
        }
        invalidate_gl_state_cache();
    }
    glDisable(GL_SCISSOR_TEST);
}

TevProgram* find_or_build_program(const State& st, const GpuLitParams* lit = nullptr) {
    std::uint64_t key = hash_tev_program(st);
    if (lit != nullptr) {
        key ^= lit_structure_key(*lit) * 0x100000001B3ull;
    }
    ++g_program_clock;
    // Direct-mapped front for the linear table (hundreds of programs, a lookup per GL draw).
    static TevProgram* s_front[256] = {};
    TevProgram*& front = s_front[(key ^ (key >> 29)) & 255u];
    if (front != nullptr && front->used && front->key == key) {
        front->last_used = g_program_clock;
        return front->failed ? nullptr : front;
    }
    for (unsigned i = 0; i < g_program_count; ++i) {
        if (g_programs[i].used && g_programs[i].key == key) {
            g_programs[i].last_used = g_program_clock;
            front = &g_programs[i];
            return g_programs[i].failed ? nullptr : &g_programs[i];
        }
    }
    unsigned slot_index = g_program_count;
    if (g_program_count >= kMaxPrograms) {
        // Evict the least recently used program.
        slot_index = 0;
        for (unsigned i = 1; i < kMaxPrograms; ++i) {
            if (g_programs[i].last_used < g_programs[slot_index].last_used) {
                slot_index = i;
            }
        }
        if (g_programs[slot_index].prog != 0) {
            glDeleteProgram(g_programs[slot_index].prog);
        }
        if (g_programs[slot_index].mesh_prog != 0) {
            glDeleteProgram(g_programs[slot_index].mesh_prog);
        }
        ++g_dbg.progevict;
    } else {
        ++g_program_count;
    }
    TevProgram& slot = g_programs[slot_index];
    slot = TevProgram{};
    slot.key = key;
    slot.used = true;
    slot.failed = true;  // cleared only once everything below succeeds
    slot.last_used = g_program_clock;
    slot.needs = analyze_tev(st);
    ++g_dbg.progmake;

    static char vsrc[32768];
    static char fsrc[65536];
    std::size_t nv = 0, nf = 0;
    const bool vs_ok = lit != nullptr ? emit_tev_vertex_cg_lit(slot.needs, *lit, vsrc, sizeof(vsrc), &nv)
                                      : emit_tev_vertex_cg(slot.needs, vsrc, sizeof(vsrc), &nv);
    if (!vs_ok || !emit_tev_fragment_cg(st, fsrc, sizeof(fsrc), &nf)) {
        g_dbg.proglast = 1;  // the emitter refused or overflowed its buffer
        return nullptr;
    }
    const GLuint vs = compile_cg(GL_CG_VERTEX_SHADER_EXT, vsrc);
    if (vs == 0) {
        g_dbg.proglast = 2;
        return nullptr;
    }
    const GLuint fs = compile_cg(GL_CG_FRAGMENT_SHADER_EXT, fsrc);
    if (fs == 0) {
        glDeleteShader(vs);
        g_dbg.proglast = 3;
        return nullptr;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (linked == 0) {
        glDeleteProgram(prog);
        g_dbg.proglast = 4;
        return nullptr;
    }
    slot.prog = prog;
    slot.a_pos = glGetAttribLocation(prog, "aPosition");
    slot.a_col0 = slot.needs.color0 ? glGetAttribLocation(prog, "aColor0") : -1;
    slot.a_col1 = slot.needs.color1 ? glGetAttribLocation(prog, "aColor1") : -1;
    char name[24];
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        slot.a_tc[c] = -1;
        if (slot.needs.coord_used[c]) {
            std::snprintf(name, sizeof(name), "aTexcoord%u", c);
            slot.a_tc[c] = glGetAttribLocation(prog, name);
        }
    }
    if (slot.a_pos < 0) {
        glDeleteProgram(prog);
        slot.prog = 0;
        g_dbg.proglast = 5;
        return nullptr;
    }
    for (unsigned t = 0; t < kMaxTexMaps; ++t) {
        slot.u_tex[t] = -1;
        if (slot.needs.tex_used[t]) {
            std::snprintf(name, sizeof(name), "uTex%u", t);
            slot.u_tex[t] = glGetUniformLocation(prog, name);
        }
    }
    slot.u_konst = slot.needs.konst ? glGetUniformLocation(prog, "uKonst") : -1;
    slot.u_tevreg = glGetUniformLocation(prog, "uTevReg");
    slot.u_ind = slot.needs.indirect ? glGetUniformLocation(prog, "uIndMtx") : -1;
    slot.u_indscale = slot.needs.indirect ? glGetUniformLocation(prog, "uIndCoordScale") : -1;
    slot.u_texsize = slot.needs.indirect ? glGetUniformLocation(prog, "uTexSize") : -1;
    slot.u_fogcol = slot.needs.fog ? glGetUniformLocation(prog, "uFogColor") : -1;
    slot.u_fogparam = slot.needs.fog ? glGetUniformLocation(prog, "uFogParam") : -1;
    slot.u_depthrange = slot.needs.fog ? glGetUniformLocation(prog, "uDepthRange") : -1;
    slot.u_dstalpha = slot.needs.dst_alpha ? glGetUniformLocation(prog, "uDstAlpha") : -1;
    if (lit != nullptr) {
        slot.lit = true;
        slot.a_eye = glGetAttribLocation(prog, "aEyePos");
        slot.a_enrm = glGetAttribLocation(prog, "aEyeNrm");
        for (unsigned c = 0; c < 2u; ++c) {
            std::snprintf(name, sizeof(name), "uMat%u", c);
            slot.u_mat[c] = glGetUniformLocation(prog, name);
            std::snprintf(name, sizeof(name), "uAmb%u", c);
            slot.u_amb[c] = glGetUniformLocation(prog, name);
        }
        for (unsigned li = 0; li < kMaxLights; ++li) {
            GLint* dst[6] = {slot.u_lp, slot.u_ld, slot.u_ls, slot.u_lc, slot.u_la, slot.u_lk};
            static const char* kPre[6] = {"uLP", "uLD", "uLS", "uLC", "uLA", "uLK"};
            for (unsigned k = 0; k < 6u; ++k) {
                std::snprintf(name, sizeof(name), "%s%u", kPre[k], li);
                dst[k][li] = glGetUniformLocation(prog, name);
            }
        }
    }
    slot.failed = false;
    return &slot;
}

void unpack_to_float4(std::uint32_t c, GLfloat* out) {
    out[0] = static_cast<GLfloat>(c & 0xFFu) / 255.0f;
    out[1] = static_cast<GLfloat>((c >> 8) & 0xFFu) / 255.0f;
    out[2] = static_cast<GLfloat>((c >> 16) & 0xFFu) / 255.0f;
    out[3] = static_cast<GLfloat>((c >> 24) & 0xFFu) / 255.0f;
}

// Binds every texmap the program samples: texmap N on texture unit N. A unit that already
// holds the same texobj (same image, format, TLUT and sampler state, nothing re-uploaded since)
// is left alone - consecutive draws of one material skip the cache lookup and every GL call.
struct UnitMemo {
    bool valid = false;
    TexObj tex{};
    const void* tlut_data = nullptr;
    std::uint32_t tlut_version = 0;
    std::uint32_t tlut_format = 0;
    std::uint32_t generation = 0;
};
UnitMemo g_unit_memo[kMaxTexMaps];

bool bind_program_textures(const Processor& processor, const TevProgram& p, const DrawExtra* x) {
    bool all_bound = true;
    bool touched = false;
    for (unsigned t = 0; t < kMaxTexMaps; ++t) {
        if (!p.needs.tex_used[t]) {
            continue;
        }
        const TexObj& tex = texmap_of(processor, x, t);
        if (tex.data == nullptr) {
            all_bound = false;
            continue;
        }
        const TlutObj* tlut = tlut_of(processor, x, t);
        UnitMemo& memo = g_unit_memo[t];
        if (memo.valid && memo.generation == g_tex_generation && std::memcmp(&memo.tex, &tex, sizeof(TexObj)) == 0 &&
            memo.tlut_data == (tlut != nullptr ? tlut->data : nullptr) &&
            memo.tlut_version == (tlut != nullptr ? tlut->version : 0u) &&
            memo.tlut_format == (tlut != nullptr ? tlut->format : 0u)) {
            continue;
        }
        glActiveTexture(GL_TEXTURE0 + t);
        touched = true;
        const std::uint32_t gen_before = g_tex_generation;
        const GLuint id = bind_texture(tex, tlut);
        if (id == 0) {
            memo.valid = false;
            all_bound = false;
            continue;
        }
        // bind_texture may itself have uploaded (bumping the generation); the memo records the
        // state after this bind either way.
        (void)gen_before;
        memo.valid = true;
        memo.tex = tex;
        memo.tlut_data = tlut != nullptr ? tlut->data : nullptr;
        memo.tlut_version = tlut != nullptr ? tlut->version : 0u;
        memo.tlut_format = tlut != nullptr ? tlut->format : 0u;
        memo.generation = g_tex_generation;
    }
    if (touched) {
        glActiveTexture(GL_TEXTURE0);
    }
    return all_bound;
}

void set_uniform4fv_cached(TevProgram& p, unsigned slot, GLint loc, GLsizei count, const GLfloat* v, GLfloat* cache) {
    const std::size_t bytes = static_cast<std::size_t>(count) * 4u * sizeof(GLfloat);
    if (p.uvalid[slot] && std::memcmp(cache, v, bytes) == 0) {
        return;
    }
    std::memcpy(cache, v, bytes);
    p.uvalid[slot] = true;
    glUniform4fv(loc, count, v);
}

void set_uniform2f_cached(TevProgram& p, unsigned slot, GLint loc, GLfloat a, GLfloat b, GLfloat* cache) {
    if (p.uvalid[slot] && cache[0] == a && cache[1] == b) {
        return;
    }
    cache[0] = a;
    cache[1] = b;
    p.uvalid[slot] = true;
    glUniform2f(loc, a, b);
}

// Draws `nidx` indices (relative to `vbase`, laid out as g_vlayout) with the draw's compiled
// TEV program. Returns false to mean "fall back" (no program for this structure).
bool draw_with_program(const Processor& processor, const DrawCall& draw, const DrawExtra* x, const std::uint16_t* idx,
                       std::uint32_t nidx, const std::uint8_t* vbase, GLfloat dst_alpha_mix = 1.0f) {
    if (!g_shader_path) {
        return false;
    }
    const State& st = tev_state_for(processor, draw, x);
    const GpuLitParams* lit = g_cur_lit;
    TevProgram* pp = find_or_build_program(st, lit);
    if (pp == nullptr) {
        if (lit != nullptr) {
            // A GPU-lit vertex program that does not build: later state epochs go back to the
            // CPU lighting (this draw's vertices are already packed raw).
            if (g_lit_program_failures++ == 0) {
                std::printf("mkwv: GPU lighting program failed (step %d); lighting back on the CPU\n", g_dbg.proglast);
            }
            set_gpu_lighting(false);
        }
        return false;
    }
    TevProgram& p = *pp;
    c_use_program(p.prog);
    if (lit != nullptr && (!p.ulit_valid || std::memcmp(&p.ulit, lit, sizeof(GpuLitParams)) != 0)) {
        for (unsigned c = 0; c < 2u; ++c) {
            if (p.u_mat[c] >= 0) glUniform4fv(p.u_mat[c], 1, lit->mat[c]);
            if (p.u_amb[c] >= 0) glUniform4fv(p.u_amb[c], 1, lit->amb[c]);
        }
        for (unsigned li = 0; li < kMaxLights; ++li) {
            if (p.u_lp[li] >= 0) glUniform4fv(p.u_lp[li], 1, lit->pos[li]);
            if (p.u_ld[li] >= 0) glUniform4fv(p.u_ld[li], 1, lit->dir[li]);
            if (p.u_ls[li] >= 0) glUniform4fv(p.u_ls[li], 1, lit->spec[li]);
            if (p.u_lc[li] >= 0) glUniform4fv(p.u_lc[li], 1, lit->col[li]);
            if (p.u_la[li] >= 0) glUniform4fv(p.u_la[li], 1, lit->cosa[li]);
            if (p.u_lk[li] >= 0) glUniform4fv(p.u_lk[li], 1, lit->dista[li]);
        }
        p.ulit = *lit;
        p.ulit_valid = true;
    }
    if (!p.samplers_set) {
        for (unsigned t = 0; t < kMaxTexMaps; ++t) {
            if (p.u_tex[t] >= 0) {
                glUniform1i(p.u_tex[t], static_cast<GLint>(t));
            }
        }
        p.samplers_set = true;
    }
    if (!bind_program_textures(processor, p, x)) {
        return false;
    }
    GLfloat tmp[32];
    if (p.u_konst >= 0) {
        for (unsigned i = 0; i < 4u; ++i) {
            unpack_to_float4(draw.konst[i], tmp + i * 4u);
        }
        set_uniform4fv_cached(p, 0, p.u_konst, 4, tmp, p.ukonst);
    }
    if (p.u_tevreg >= 0) {
        // GX_TEVPREV, GX_TEVREG0/1/2 at full signed 11-bit precision.
        for (unsigned i = 0; i < 4u; ++i) {
            for (unsigned c = 0; c < 4u; ++c) {
                tmp[i * 4u + c] = static_cast<GLfloat>(draw.tev_reg[i][c]) / 255.0f;
            }
        }
        set_uniform4fv_cached(p, 1, p.u_tevreg, 4, tmp, p.utevreg);
    }
    if (p.u_fogcol >= 0) {
        unpack_to_float4(draw.fog.color, tmp);
        set_uniform4fv_cached(p, 2, p.u_fogcol, 1, tmp, p.ufogcol);
    }
    if (p.u_fogparam >= 0) {
        // A as the unshifted partial float, B's 24-bit magnitude, C, B's shift (Aurora
        // shader_info.cpp fog_uniform).
        const std::uint32_t a_bits = ((draw.fog.raw0 >> 19) & 1u) << 31 | ((draw.fog.raw0 >> 11) & 0xFFu) << 23 |
                                     (draw.fog.raw0 & 0x7FFu) << 12;
        float a_raw = 0.0f;
        std::memcpy(&a_raw, &a_bits, sizeof(a_raw));
        tmp[0] = a_raw;
        tmp[1] = static_cast<GLfloat>(draw.fog.raw1 & 0xFFFFFFu);
        tmp[2] = draw.fog.c;
        tmp[3] = static_cast<GLfloat>(draw.fog.raw2 & 0x1Fu);
        set_uniform4fv_cached(p, 3, p.u_fogparam, 1, tmp, p.ufogparam);
    }
    if (p.u_depthrange >= 0) {
        set_uniform2f_cached(p, 4, p.u_depthrange, g_depth_near, g_depth_far, p.udepth);
    }
    if (p.u_dstalpha >= 0) {
        set_uniform2f_cached(p, 5, p.u_dstalpha, dst_alpha_mix, static_cast<GLfloat>(draw.dst_alpha) / 255.0f, p.udst);
    }
    if (p.u_ind >= 0) {
        // Two float4 per matrix: row 0 with 2^scale_exp in .w, then row 1.
        GLfloat m[24];
        for (unsigned k = 0; k < kMaxIndMtx; ++k) {
            const IndMatrix& im = x != nullptr ? x->ind_mtx[k] : processor.state.ind_mtx[k];
            m[k * 8u + 0u] = im.m[0];
            m[k * 8u + 1u] = im.m[1];
            m[k * 8u + 2u] = im.m[2];
            m[k * 8u + 3u] = std::ldexp(1.0f, im.scale_exp);
            m[k * 8u + 4u] = im.m[3];
            m[k * 8u + 5u] = im.m[4];
            m[k * 8u + 6u] = im.m[5];
            m[k * 8u + 7u] = 0.0f;
        }
        set_uniform4fv_cached(p, 6, p.u_ind, 6, m, p.uind);
    }
    if (p.u_indscale >= 0) {
        GLfloat sc[16];
        for (unsigned k = 0; k < kMaxIndStages; ++k) {
            const IndStage& is = x != nullptr ? x->ind[k] : processor.state.ind[k];
            sc[k * 4u + 0u] = std::ldexp(1.0f, -static_cast<int>(is.scale_s));
            sc[k * 4u + 1u] = std::ldexp(1.0f, -static_cast<int>(is.scale_t));
            sc[k * 4u + 2u] = 0.0f;
            sc[k * 4u + 3u] = 0.0f;
        }
        set_uniform4fv_cached(p, 7, p.u_indscale, 4, sc, p.uindscale);
    }
    if (p.u_texsize >= 0) {
        GLfloat ts[32];
        for (unsigned t = 0; t < kMaxTexMaps; ++t) {
            const TexObj& tm = texmap_of(processor, x, t);
            const float w = tm.width != 0 ? static_cast<float>(tm.width) : 1.0f;
            const float h = tm.height != 0 ? static_cast<float>(tm.height) : 1.0f;
            ts[t * 4u + 0u] = w;
            ts[t * 4u + 1u] = h;
            ts[t * 4u + 2u] = 1.0f / w;
            ts[t * 4u + 3u] = 1.0f / h;
        }
        set_uniform4fv_cached(p, 8, p.u_texsize, 8, ts, p.utexsize);
    }
    // The fixed-function client-state arrays and generic attribute arrays are different
    // paths in VitaGL; the FFP ones stay off while programs draw.
    c_ffp_arrays(false);
    const VertexLayout& l = g_vlayout;
    const GLsizei stride = static_cast<GLsizei>(l.stride);
    if (in_ring(vbase)) {
        // Attribute pointers become offsets into the buffer, which starts at this run's base.
        use_array_buffer(g_ring_vbo);
        vglBufferData(GL_ARRAY_BUFFER, vbase);
    } else {
        use_array_buffer(0);
    }
    // Buffer offsets (from the run's base) or client pointers.
    const std::uintptr_t vb = g_array_bound != 0 ? 0u : reinterpret_cast<std::uintptr_t>(vbase);
    auto at = [vb](std::uint32_t off) { return reinterpret_cast<const void*>(vb + off); };
    std::uint32_t want = 1u << static_cast<unsigned>(p.a_pos);
    glVertexAttribPointer(static_cast<GLuint>(p.a_pos), 4, GL_FLOAT, GL_FALSE, stride, at(0));
    if (p.a_col0 >= 0) {
        want |= 1u << static_cast<unsigned>(p.a_col0);
        glVertexAttribPointer(static_cast<GLuint>(p.a_col0), 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(kVertOffColor0));
    }
    if (p.a_col1 >= 0) {
        want |= 1u << static_cast<unsigned>(p.a_col1);
        glVertexAttribPointer(static_cast<GLuint>(p.a_col1), 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(kVertOffColor1));
    }
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        // A coordinate set beyond what the batch packed stays disabled: the attribute then reads
        // its constant value instead of running off the end of the vertex.
        if (p.a_tc[c] >= 0 && c < l.ntc) {
            want |= 1u << static_cast<unsigned>(p.a_tc[c]);
            glVertexAttribPointer(static_cast<GLuint>(p.a_tc[c]), 3, GL_FLOAT, GL_FALSE, stride,
                                  at(kVertOffTex + 12u * c));
        }
    }
    if (p.lit && l.lit) {
        if (p.a_eye >= 0) {
            want |= 1u << static_cast<unsigned>(p.a_eye);
            glVertexAttribPointer(static_cast<GLuint>(p.a_eye), 3, GL_FLOAT, GL_FALSE, stride, at(l.off_eye));
        }
        if (p.a_enrm >= 0) {
            want |= 1u << static_cast<unsigned>(p.a_enrm);
            glVertexAttribPointer(static_cast<GLuint>(p.a_enrm), 3, GL_FLOAT, GL_FALSE, stride, at(l.off_eye + 12u));
        }
    }
    c_attribs(want);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(nidx), GL_UNSIGNED_SHORT, idx);
    perf::count(perf::kGlDraws);
    return true;
}

// Fixed-function fallback: one GL_COMBINE unit, one texture, the single alpha compare GL has.
void apply_alpha_test(const DrawCall& draw) {
    constexpr std::uint8_t kAopAnd = 0;
    const bool c0_always = draw.alpha_comp0 == Always || (draw.alpha_comp0 == GEqual && draw.alpha_ref0 == 0) ||
                           (draw.alpha_comp0 == LEqual && draw.alpha_ref0 == 255);
    const bool c1_always = draw.alpha_comp1 == Always || (draw.alpha_comp1 == GEqual && draw.alpha_ref1 == 0) ||
                           (draw.alpha_comp1 == LEqual && draw.alpha_ref1 == 255);
    if (draw.alpha_op == kAopAnd && !c0_always && c1_always) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(gl_compare(draw.alpha_comp0), static_cast<GLfloat>(draw.alpha_ref0) / 255.0f);
        return;
    }
    if (draw.alpha_op == kAopAnd && c0_always && !c1_always) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(gl_compare(draw.alpha_comp1), static_cast<GLfloat>(draw.alpha_ref1) / 255.0f);
        return;
    }
    glDisable(GL_ALPHA_TEST);
}

// Draws one run: a draw's state with `nidx` indices relative to `vbase`.
bool draw_run(const Processor& processor, const DrawCall& draw, const DrawExtra* x, const std::uint16_t* idx,
              std::uint32_t nidx, const std::uint8_t* vbase) {
    if (nidx == 0) {
        return false;
    }
    bool no_color = false;
    apply_blend(draw, &no_color);
    const DrawExtra* raster = x;
    if (no_color) {
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    }
    bool drew = false;
    // GXSetDstAlpha with blending: the colour has to blend with the TEV alpha while the EFB
    // receives the constant. GL has no constant blend factor on vitaGL, so that is two passes:
    // colour (alpha masked), then alpha alone with the constant and no blending.
    const State& tev_st = tev_state_for(processor, draw, x);
    const bool dst_two_pass = !no_color && tev_st.dst_alpha_enable &&
                              (draw.blend_mode == BlendMix || draw.blend_mode == BlendSubtract);
    if (dst_two_pass) {
        const bool cu = raster != nullptr ? raster->color_update : processor.state.color_update;
        const GLboolean c = cu ? GL_TRUE : GL_FALSE;
        glColorMask(c, c, c, GL_FALSE);
    }
    if (draw_with_program(processor, draw, x, idx, nidx, vbase, dst_two_pass ? 0.0f : 1.0f)) {
        ++g_dbg.prog;
        drew = true;
        if (dst_two_pass) {
            ++g_dbg.dstalpha;
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            c_blend_on(false);
            glDepthMask(GL_FALSE);
            const bool depth_test = raster != nullptr ? raster->depth_test : processor.state.depth_test;
            if (depth_test) {
                glDepthFunc(GL_EQUAL);
            }
            draw_with_program(processor, draw, x, idx, nidx, vbase, 1.0f);
            bool unused = false;
            apply_blend(draw, &unused);
            if (raster != nullptr) {
                apply_depth(*raster);
            } else {
                apply_depth(processor.state);
            }
        }
    } else {
        ++g_dbg.progfail;
        c_use_program(0);
        apply_alpha_test(draw);
        const bool passclr_only = draw.tev == TevPassClr;
        unsigned tex_map = 0;
        const TexObj* tex = passclr_only ? nullptr : draw_tex(processor, draw, x, &tex_map);
        glActiveTexture(GL_TEXTURE0);
        g_unit_memo[0].valid = false;
        if (tex != nullptr && tex->data != nullptr) {
            const GLuint id = bind_texture(*tex, tlut_of(processor, x, tex_map));
            g_dbg.tex_bind = id != 0 ? 1 : 0;
            if (id != 0) {
                glEnable(GL_TEXTURE_2D);
                if (apply_tev_combine(draw, true)) {
                    ++g_dbg.comb;
                } else {
                    ++g_dbg.cfail;
                    GLint env = GL_REPLACE;
                    if (draw.tev == TevModulate) {
                        env = GL_MODULATE;
                    } else if (draw.tev == TevDecal) {
                        env = GL_DECAL;
                    }
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env);
                }
            } else {
                glDisable(GL_TEXTURE_2D);
            }
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        c_attribs(0);
        c_ffp_arrays(true);
        use_array_buffer(0);
        const GLsizei stride = static_cast<GLsizei>(g_vlayout.stride);
        glVertexPointer(4, GL_FLOAT, stride, vbase);
        glColorPointer(4, GL_UNSIGNED_BYTE, stride, vbase + kVertOffColor0);
        glTexCoordPointer(2, GL_FLOAT, stride, vbase + kVertOffTex);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(nidx), GL_UNSIGNED_SHORT, idx);
        perf::count(perf::kGlDraws);
        drew = true;
    }
    if (no_color || dst_two_pass) {
        if (raster != nullptr) {
            apply_color_mask(*raster, draw.pixel_fmt);
        } else {
            apply_color_mask(processor.state, draw.pixel_fmt);
        }
    }
    return drew;
}

// One draw on its own: packs its vertices and indices and draws them (tests, and anything
// outside the batched loop).
bool draw_call(const Processor& processor, const DrawCall& draw, const DrawExtra* x) {
    static std::uint16_t idx[kMaxDecodedVerts * 3];
    const std::uint32_t nidx = expand_indices(draw.prim, draw.vertex_count, idx, kMaxDecodedVerts * 3);
    if (nidx == 0) {
        return false;
    }
    g_vlayout = layout_for(draw.num_tex_gens);
    pack_vertices(processor, draw.first_vertex, draw.vertex_count, g_vbuf, g_vlayout);
    return draw_run(processor, draw, x, idx, nidx, g_vbuf);
}

// Whether draw b can be drawn in the same GL call as draw a: everything but which vertices
// and what primitive must match. The decoder fills whole structs field by field, and the
// compared ranges are laid out without padding surprises the memcmp would trip on: at worst
// padding differs and two mergeable draws are simply drawn separately.
bool draws_mergeable(const DrawCall& a, const DrawCall& b, const DrawExtra* xa, const DrawExtra* xb,
                     std::uint64_t ka = 0, std::uint64_t kb = 0) {
    // Draws sharing a snapshot slot come from one run of primitives with no state write in
    // between: the decoder copied the DrawCall and changed only its vertices and primitive.
    if (xa != nullptr && xa == xb) {
        return true;
    }
    // static64: the decoder hashed exactly what the memcmps below compare (draw_merge_key).
    // ~2 KiB of mostly cache-missing compares per draw was most of `sub: idx`.
    if (ka != 0 && kb != 0 && xa != nullptr && xb != nullptr) {
        return ka == kb;
    }
    DrawCall bb = b;
    bb.prim = a.prim;
    bb.fmt = a.fmt;
    bb.first_vertex = a.first_vertex;
    bb.vertex_count = a.vertex_count;
    if (std::memcmp(&a, &bb, sizeof(DrawCall)) != 0) {
        return false;
    }
    if ((xa == nullptr) != (xb == nullptr)) {
        return false;
    }
    // Everything in DrawExtra up to the diagnostics-only tail (tg0_mtx, tg0_rows).
    return xa == nullptr || std::memcmp(xa, xb, offsetof(DrawExtra, tg0_mtx)) == 0;
}

} // namespace

// Raster state that can change between draws. Only re-sent to GL when it does, since a
// UI frame is hundreds of draws that almost all share one setup.
struct RasterKey {
    float vp[6];
    std::int32_t sc[4];
    std::uint8_t has_vp, has_sc;
    std::uint8_t depth_test, depth_write, depth_func, cull, color_update, alpha_update, pixel_fmt;
    std::int32_t off[2];
};

template <typename S>
RasterKey raster_key(const S& st, std::uint8_t pixel_fmt) {
    RasterKey k{};
    k.has_vp = st.has_render_vp ? 1 : 0;
    for (unsigned i = 0; i < 6u; ++i) {
        k.vp[i] = st.render_vp[i];
    }
    k.has_sc = static_cast<std::uint8_t>((st.has_render_scissor ? 1 : 0) | (st.has_scissor ? 2 : 0));
    if (st.has_render_scissor) {
        for (unsigned i = 0; i < 4u; ++i) {
            k.sc[i] = st.render_scissor[i];
        }
    } else {
        k.sc[0] = st.scissor_x;
        k.sc[1] = st.scissor_y;
        k.sc[2] = st.scissor_w;
        k.sc[3] = st.scissor_h;
    }
    k.depth_test = st.depth_test ? 1 : 0;
    k.depth_write = st.depth_write ? 1 : 0;
    k.depth_func = static_cast<std::uint8_t>(st.depth_func);
    k.cull = static_cast<std::uint8_t>(st.cull_mode);
    k.color_update = st.color_update ? 1 : 0;
    k.alpha_update = st.alpha_update ? 1 : 0;
    k.pixel_fmt = pixel_fmt;
    k.off[0] = st.scissor_off_x;
    k.off[1] = st.scissor_off_y;
    return k;
}

template <typename S>
void apply_raster(const S& st, std::uint8_t pixel_fmt) {
    apply_viewport(st);
    apply_depth(st);
    apply_cull(st.cull_mode);
    apply_color_mask(st, pixel_fmt);
}

// Object-space mesh vertex shader. Same varyings as the passthrough vertex shader for this
// TEV program, so it links against that program's fragment shader. Positions are model space;
// the PN and projection uniforms are this frame's matrices.
bool emit_mesh_vs(const ShaderNeeds& needs, char* out, std::size_t cap) {
    struct App {
        char* p;
        char* end;
        bool ok = true;
        void add(const char* s) {
            if (!ok || s == nullptr) {
                return;
            }
            const std::size_t n = std::strlen(s);
            if (static_cast<std::size_t>(end - p) <= n) {
                ok = false;
                return;
            }
            std::memcpy(p, s, n);
            p += n;
        }
    };
    App w{out, out + cap - 1};
    w.add("void main(\n"
          "    float3 aPosition,\n"
          "    float3 aNormal,\n"
          "    float2 aUv,\n");
    if (needs.coord_used[1]) {
        w.add("    float2 aUv1,\n");
    }
    w.add("    float4 aColor0,\n"
          "    uniform float4 uPn[3],\n"
          "    uniform float4 uProj[4],\n"
          "    uniform float3 uNrm[3],\n");
    w.add(needs.coord_used[1] ? "    uniform float4 uTex[4],\n" : "    uniform float4 uTex[2],\n");
    if (needs.color1) {
        w.add("    uniform float4 uCol1,\n");
    }
    w.add(
          "    uniform float4 uLit,\n"
          "    uniform float4 uSrc,\n"
          "    uniform float4 uMat,\n"
          "    uniform float4 uAmb,\n"
          "    uniform float3 uLPos[4],\n"
          "    uniform float3 uLDir[4],\n"
          "    uniform float4 uLCol[4],\n"
          "    uniform float3 uCos[4],\n"
          "    uniform float3 uDist[4],\n"
          "    float4 out vPosition : POSITION");
    if (needs.color0) {
        w.add(",\n    float4 out vColor0 : COLOR0");
    }
    if (needs.color1) {
        w.add(",\n    float4 out vColor1 : COLOR1");
    }
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (!needs.coord_used[c]) {
            continue;
        }
        char line[64];
        std::snprintf(line, sizeof(line), ",\n    float%u out vTexcoord%u : TEXCOORD%u", needs.coord_proj[c] ? 3u : 2u, c, c);
        w.add(line);
    }
    if (needs.fog) {
        char line[48];
        std::snprintf(line, sizeof(line), ",\n    float2 out vZW : TEXCOORD%u", needs.fog_slot);
        w.add(line);
    }
    w.add(")\n{\n"
          "    float4 obj = float4(aPosition, 1.0);\n"
          "    float3 mv = float3(dot(uPn[0], obj), dot(uPn[1], obj), dot(uPn[2], obj));\n"
          "    float4 eye = float4(mv, 1.0);\n"
          "    float cx = dot(uProj[0], eye);\n"
          "    float cy = dot(uProj[1], eye);\n"
          "    float cz = dot(uProj[2], eye);\n"
          "    float cw = dot(uProj[3], eye);\n"
          "    vPosition = float4(cx, cy, 2.0 * cz + cw, cw);\n"
          "    float4 st = float4(aUv.x, aUv.y, 1.0, 1.0);\n"
          "    float2 uv = float2(dot(uTex[0], st), dot(uTex[1], st));\n");
    if (needs.coord_used[1]) {
        w.add("    float4 st1 = float4(aUv1.x, aUv1.y, 1.0, 1.0);\n"
              "    float2 uv1 = float2(dot(uTex[2], st1), dot(uTex[3], st1));\n");
    }
    w.add(
          "    float4 vtx = aColor0 * 255.0;\n"
          "    float4 src = lerp(uMat, vtx, float4(uSrc.x, uSrc.x, uSrc.x, uSrc.z));\n"
          "    float4 col = src;\n"
          "    if (uLit.x > 0.5) {\n"
          "        float3 nrm = float3(dot(uNrm[0], aNormal), dot(uNrm[1], aNormal), dot(uNrm[2], aNormal));\n"
          "        float nl = length(nrm);\n"
          "        nrm = nl > 1.0e-20 ? nrm / nl : nrm;\n"
          "        float3 acc = lerp(uAmb.xyz, vtx.xyz, uSrc.y);\n");
    for (unsigned i = 0; i < 4; ++i) {
        char block[1600];
        std::snprintf(block, sizeof(block),
                      "        if (uLit.w > %u.5) {\n"
                      "            float3 L = uLPos[%u] - mv;\n"
                      "            float dist2 = dot(L, L);\n"
                      "            float dist = sqrt(dist2);\n"
                      "            float3 ldir = dist > 0.0 ? L / dist : nrm;\n"
                      "            float attn = 1.0;\n"
                      "            if (uLit.y > 0.5) {\n"
                      "                float cosine = max(dot(ldir, uLDir[%u]), 0.0);\n"
                      "                float cosAttn = uCos[%u].x + uCos[%u].y * cosine + uCos[%u].z * cosine * cosine;\n"
                      "                float distAttn = uDist[%u].x + uDist[%u].y * dist + uDist[%u].z * dist2;\n"
                      "                attn = distAttn != 0.0 ? max(cosAttn / distAttn, 0.0) : 0.0;\n"
                      "            }\n"
                      "            float diff = 1.0;\n"
                      "            if (uLit.z > 1.5) diff = max(dot(ldir, nrm), 0.0);\n"
                      "            else if (uLit.z > 0.5) diff = dot(ldir, nrm);\n"
                      "            acc += floor(attn * diff * uLCol[%u].xyz + 0.5);\n"
                      "        }\n",
                      i, i, i, i, i, i, i, i, i, i);
        w.add(block);
    }
    w.add("        acc = clamp(acc, 0.0, 255.0);\n"
          "        float3 scaled = acc + floor(acc * (1.0 / 128.0));\n"
          "        col.xyz = floor(src.xyz * scaled * (1.0 / 256.0));\n"
          "    }\n"
          "    col = saturate(col * (1.0 / 255.0));\n");
    if (needs.color0) {
        w.add("    vColor0 = col;\n");
    }
    if (needs.color1) {
        w.add("    vColor1 = uCol1;\n");
    }
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (!needs.coord_used[c]) {
            continue;
        }
        char line[80];
        if (c == 0) {
            std::snprintf(line, sizeof(line), "    vTexcoord0 = %s;\n", needs.coord_proj[0] ? "float3(uv, 1.0)" : "uv");
        } else if (c == 1) {
            std::snprintf(line, sizeof(line), "    vTexcoord1 = %s;\n", needs.coord_proj[1] ? "float3(uv1, 1.0)" : "uv1");
        } else if (needs.coord_proj[c]) {
            std::snprintf(line, sizeof(line), "    vTexcoord%u = float3(0.0, 0.0, 1.0);\n", c);
        } else {
            std::snprintf(line, sizeof(line), "    vTexcoord%u = float2(0.0, 0.0);\n", c);
        }
        w.add(line);
    }
    if (needs.fog) {
        w.add("    vZW = float2(2.0 * cz + cw, cw);\n");
    }
    w.add("}\n");
    if (w.ok) {
        *w.p = '\0';
    }
    return w.ok;
}

bool ensure_mesh_program(TevProgram& p, const State& st) {
    if (p.mesh_prog != 0) {
        return true;
    }
    if (p.mesh_failed) {
        return false;
    }
    static char vsrc[16384];
    static char fsrc[65536];
    std::size_t nf = 0;
    if (!emit_mesh_vs(p.needs, vsrc, sizeof(vsrc)) || !emit_tev_fragment_cg(st, fsrc, sizeof(fsrc), &nf)) {
        p.mesh_failed = true;
        return false;
    }
    const GLuint vs = compile_cg(GL_CG_VERTEX_SHADER_EXT, vsrc);
    const GLuint fs = compile_cg(GL_CG_FRAGMENT_SHADER_EXT, fsrc);
    if (vs == 0 || fs == 0) {
        if (vs != 0) {
            glDeleteShader(vs);
        }
        if (fs != 0) {
            glDeleteShader(fs);
        }
        p.mesh_failed = true;
        static bool logged = false;
        if (!logged) {
            std::printf("mkwv: gpu mesh shader compile failed\n");
            logged = true;
        }
        return false;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (linked == 0) {
        glDeleteProgram(prog);
        p.mesh_failed = true;
        static bool logged = false;
        if (!logged) {
            std::printf("mkwv: gpu mesh shader link failed\n");
            logged = true;
        }
        return false;
    }
    p.mesh_prog = prog;
    p.ma_pos = glGetAttribLocation(prog, "aPosition");
    p.ma_nrm = glGetAttribLocation(prog, "aNormal");
    p.ma_uv = glGetAttribLocation(prog, "aUv");
    p.ma_uv1 = glGetAttribLocation(prog, "aUv1");
    p.ma_col = glGetAttribLocation(prog, "aColor0");
    p.mu_pn = glGetUniformLocation(prog, "uPn");
    p.mu_proj = glGetUniformLocation(prog, "uProj");
    p.mu_nrm = glGetUniformLocation(prog, "uNrm");
    p.mu_tex = glGetUniformLocation(prog, "uTex");
    p.mu_col1 = glGetUniformLocation(prog, "uCol1");
    p.mu_lit = glGetUniformLocation(prog, "uLit");
    p.mu_src = glGetUniformLocation(prog, "uSrc");
    p.mu_mat = glGetUniformLocation(prog, "uMat");
    p.mu_amb = glGetUniformLocation(prog, "uAmb");
    p.mu_lpos = glGetUniformLocation(prog, "uLPos");
    p.mu_ldir = glGetUniformLocation(prog, "uLDir");
    p.mu_lcol = glGetUniformLocation(prog, "uLCol");
    p.mu_cos = glGetUniformLocation(prog, "uCos");
    p.mu_dist = glGetUniformLocation(prog, "uDist");
    p.mm_konst = p.needs.konst ? glGetUniformLocation(prog, "uKonst") : -1;
    p.mm_tevreg = glGetUniformLocation(prog, "uTevReg");
    p.mm_fogc = p.needs.fog ? glGetUniformLocation(prog, "uFogColor") : -1;
    p.mm_fogp = p.needs.fog ? glGetUniformLocation(prog, "uFogParam") : -1;
    p.mm_depth = p.needs.fog ? glGetUniformLocation(prog, "uDepthRange") : -1;
    p.mm_dst = p.needs.dst_alpha ? glGetUniformLocation(prog, "uDstAlpha") : -1;
    p.mm_ind = p.needs.indirect ? glGetUniformLocation(prog, "uIndMtx") : -1;
    p.mm_indscale = p.needs.indirect ? glGetUniformLocation(prog, "uIndCoordScale") : -1;
    p.mm_texsize = p.needs.indirect ? glGetUniformLocation(prog, "uTexSize") : -1;
    c_use_program(prog);
    for (unsigned t = 0; t < kMaxTexMaps; ++t) {
        p.mm_tex[t] = -1;
        if (p.needs.tex_used[t]) {
            char name[16];
            std::snprintf(name, sizeof(name), "uTex%u", t);
            p.mm_tex[t] = glGetUniformLocation(prog, name);
            if (p.mm_tex[t] >= 0) {
                glUniform1i(p.mm_tex[t], static_cast<GLint>(t));
            }
        }
    }
    if (p.ma_pos < 0) {
        glDeleteProgram(prog);
        p.mesh_prog = 0;
        p.mesh_failed = true;
        return false;
    }
    return true;
}

void bytes255(std::uint32_t c, GLfloat* o) {
    o[0] = static_cast<GLfloat>(c & 255u);
    o[1] = static_cast<GLfloat>((c >> 8) & 255u);
    o[2] = static_cast<GLfloat>((c >> 16) & 255u);
    o[3] = static_cast<GLfloat>((c >> 24) & 255u);
}

bool draw_mesh_program(const Processor& processor, const DrawCall& draw, const DrawExtra* x, const GpuMeshView& mesh,
                       GLfloat dst_alpha_mix) {
    const State& st = tev_state_for(processor, draw, x);
    TevProgram* pp = find_or_build_program(st);
    if (pp == nullptr || !ensure_mesh_program(*pp, st)) {
        return false;
    }
    TevProgram& p = *pp;
    bind_program_textures(processor, p, x);
    c_use_program(p.mesh_prog);
    if (p.mu_pn >= 0) {
        glUniform4fv(p.mu_pn, 3, mesh.pn);
    }
    if (p.mu_proj >= 0) {
        glUniform4fv(p.mu_proj, 4, &mesh.proj[0][0]);
    }
    if (p.mu_nrm >= 0) {
        glUniform3fv(p.mu_nrm, 3, mesh.nrm_mtx);
    }
    if (p.mu_tex >= 0) {
        glUniform4fv(p.mu_tex, p.needs.coord_used[1] ? 4 : 2, mesh.tex_rows);
    }
    if (p.mu_col1 >= 0) {
        glUniform4fv(p.mu_col1, 1, mesh.col1);
    }
    if (p.mu_lit >= 0) {
        glUniform4f(p.mu_lit, mesh.lit ? 1.0f : 0.0f, mesh.spot ? 1.0f : 0.0f, static_cast<GLfloat>(mesh.diff),
                    static_cast<GLfloat>(mesh.nlights));
    }
    if (p.mu_src >= 0) {
        glUniform4f(p.mu_src, mesh.mat_vtx ? 1.0f : 0.0f, mesh.amb_vtx ? 1.0f : 0.0f, mesh.a_mat_vtx ? 1.0f : 0.0f, 0.0f);
    }
    GLfloat tmp[32];
    if (p.mu_mat >= 0) {
        bytes255(mesh.mat_color, tmp);
        glUniform4fv(p.mu_mat, 1, tmp);
    }
    if (p.mu_amb >= 0) {
        bytes255(mesh.amb_color, tmp);
        glUniform4fv(p.mu_amb, 1, tmp);
    }
    if (p.mu_lpos >= 0) {
        glUniform3fv(p.mu_lpos, 4, &mesh.light_pos[0][0]);
    }
    if (p.mu_ldir >= 0) {
        glUniform3fv(p.mu_ldir, 4, &mesh.light_dir[0][0]);
    }
    if (p.mu_lcol >= 0) {
        glUniform4fv(p.mu_lcol, 4, &mesh.light_col[0][0]);
    }
    if (p.mu_cos >= 0) {
        glUniform3fv(p.mu_cos, 4, &mesh.cos_att[0][0]);
    }
    if (p.mu_dist >= 0) {
        glUniform3fv(p.mu_dist, 4, &mesh.dist_att[0][0]);
    }
    if (p.mm_konst >= 0) {
        for (unsigned i = 0; i < 4u; ++i) {
            unpack_to_float4(draw.konst[i], tmp + i * 4u);
        }
        glUniform4fv(p.mm_konst, 4, tmp);
    }
    if (p.mm_tevreg >= 0) {
        for (unsigned i = 0; i < 4u; ++i) {
            for (unsigned c = 0; c < 4u; ++c) {
                tmp[i * 4u + c] = static_cast<GLfloat>(draw.tev_reg[i][c]) / 255.0f;
            }
        }
        glUniform4fv(p.mm_tevreg, 4, tmp);
    }
    if (p.mm_fogc >= 0) {
        unpack_to_float4(draw.fog.color, tmp);
        glUniform4fv(p.mm_fogc, 1, tmp);
    }
    if (p.mm_fogp >= 0) {
        const std::uint32_t a_bits = ((draw.fog.raw0 >> 19) & 1u) << 31 | ((draw.fog.raw0 >> 11) & 0xFFu) << 23 |
                                     (draw.fog.raw0 & 0x7FFu) << 12;
        float a_raw = 0.0f;
        std::memcpy(&a_raw, &a_bits, sizeof(a_raw));
        tmp[0] = a_raw;
        tmp[1] = static_cast<GLfloat>(draw.fog.raw1 & 0xFFFFFFu);
        tmp[2] = draw.fog.c;
        tmp[3] = static_cast<GLfloat>(draw.fog.raw2 & 0x1Fu);
        glUniform4fv(p.mm_fogp, 1, tmp);
    }
    if (p.mm_depth >= 0) {
        glUniform2f(p.mm_depth, g_depth_near, g_depth_far);
    }
    if (p.mm_dst >= 0) {
        glUniform2f(p.mm_dst, dst_alpha_mix, static_cast<GLfloat>(draw.dst_alpha) / 255.0f);
    }
    if (p.mm_ind >= 0) {
        GLfloat m[24];
        for (unsigned k = 0; k < kMaxIndMtx; ++k) {
            const IndMatrix& im = x != nullptr ? x->ind_mtx[k] : processor.state.ind_mtx[k];
            m[k * 8u + 0u] = im.m[0];
            m[k * 8u + 1u] = im.m[1];
            m[k * 8u + 2u] = im.m[2];
            m[k * 8u + 3u] = std::ldexp(1.0f, im.scale_exp);
            m[k * 8u + 4u] = im.m[3];
            m[k * 8u + 5u] = im.m[4];
            m[k * 8u + 6u] = im.m[5];
            m[k * 8u + 7u] = 0.0f;
        }
        glUniform4fv(p.mm_ind, 6, m);
    }
    if (p.mm_indscale >= 0) {
        GLfloat sc[16];
        for (unsigned k = 0; k < kMaxIndStages; ++k) {
            const IndStage& is = x != nullptr ? x->ind[k] : processor.state.ind[k];
            sc[k * 4u + 0u] = std::ldexp(1.0f, -static_cast<int>(is.scale_s));
            sc[k * 4u + 1u] = std::ldexp(1.0f, -static_cast<int>(is.scale_t));
            sc[k * 4u + 2u] = 0.0f;
            sc[k * 4u + 3u] = 0.0f;
        }
        glUniform4fv(p.mm_indscale, 4, sc);
    }
    if (p.mm_texsize >= 0) {
        GLfloat ts[32];
        for (unsigned t = 0; t < kMaxTexMaps; ++t) {
            const TexObj& tm = texmap_of(processor, x, t);
            const float tw = tm.width != 0 ? static_cast<float>(tm.width) : 1.0f;
            const float th = tm.height != 0 ? static_cast<float>(tm.height) : 1.0f;
            ts[t * 4u + 0u] = tw;
            ts[t * 4u + 1u] = th;
            ts[t * 4u + 2u] = 1.0f / tw;
            ts[t * 4u + 3u] = 1.0f / th;
        }
        glUniform4fv(p.mm_texsize, 8, ts);
    }
    c_ffp_arrays(false);
    use_array_buffer(0);
    const GLsizei stride = static_cast<GLsizei>(mesh.stride);
    std::uint32_t want = 0;
    if (p.ma_pos >= 0) {
        want |= 1u << static_cast<unsigned>(p.ma_pos);
        glVertexAttribPointer(static_cast<GLuint>(p.ma_pos), 3, GL_FLOAT, GL_FALSE, stride, mesh.pos);
    }
    if (p.ma_nrm >= 0) {
        want |= 1u << static_cast<unsigned>(p.ma_nrm);
        glVertexAttribPointer(static_cast<GLuint>(p.ma_nrm), 3, GL_FLOAT, GL_FALSE, stride, mesh.nrm);
    }
    if (p.ma_uv >= 0) {
        want |= 1u << static_cast<unsigned>(p.ma_uv);
        glVertexAttribPointer(static_cast<GLuint>(p.ma_uv), 2, GL_FLOAT, GL_FALSE, stride, mesh.uv);
    }
    if (p.ma_uv1 >= 0) {
        want |= 1u << static_cast<unsigned>(p.ma_uv1);
        glVertexAttribPointer(static_cast<GLuint>(p.ma_uv1), 2, GL_FLOAT, GL_FALSE, stride, mesh.uv1);
    }
    if (p.ma_col >= 0) {
        want |= 1u << static_cast<unsigned>(p.ma_col);
        glVertexAttribPointer(static_cast<GLuint>(p.ma_col), 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, mesh.color);
    }
    c_attribs(want);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.index_count), GL_UNSIGNED_SHORT, mesh.indices);
    perf::count(perf::kGlDraws);
    return true;
}

bool draw_mesh_run(const Processor& processor, const DrawCall& draw, const DrawExtra* x, const GpuMeshView& mesh) {
    if (mesh.index_count == 0 || mesh.indices == nullptr || mesh.pos == nullptr) {
        return false;
    }
    bool no_color = false;
    apply_blend(draw, &no_color);
    if (no_color) {
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    }
    const State& tev_st = tev_state_for(processor, draw, x);
    const bool dst_two_pass = !no_color && tev_st.dst_alpha_enable &&
                              (draw.blend_mode == BlendMix || draw.blend_mode == BlendSubtract);
    if (dst_two_pass) {
        const bool cu = x != nullptr ? x->color_update : processor.state.color_update;
        const GLboolean c = cu ? GL_TRUE : GL_FALSE;
        glColorMask(c, c, c, GL_FALSE);
    }
    if (!draw_mesh_program(processor, draw, x, mesh, dst_two_pass ? 0.0f : 1.0f)) {
        ++g_dbg.progfail;
        return false;
    }
    ++g_dbg.prog;
    if (dst_two_pass) {
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        c_blend_on(false);
        glDepthMask(GL_FALSE);
        const bool depth_test = x != nullptr ? x->depth_test : processor.state.depth_test;
        if (depth_test) {
            glDepthFunc(GL_EQUAL);
        }
        draw_mesh_program(processor, draw, x, mesh, 1.0f);
        bool unused = false;
        apply_blend(draw, &unused);
        if (x != nullptr) {
            apply_depth(*x);
        } else {
            apply_depth(processor.state);
        }
    }
    if (no_color || dst_two_pass) {
        if (x != nullptr) {
            apply_color_mask(*x, draw.pixel_fmt);
        } else {
            apply_color_mask(processor.state, draw.pixel_fmt);
        }
    }
    return true;
}

// One batch of a frame: the draws the Processor holds right now, with the EFB copies among
// them. The first batch of a frame fixes the letterbox; later batches (a Processor that
// filled up mid-frame) reuse it.
bool submit_processor(const Processor& processor) {
    const bool first_batch = !g_frame_open;
    if (first_batch) {
        g_dbg = {};
    }
    // A capture starts at a frame boundary and then takes every batch of that frame.
    if (capture_active() || (capture_armed() && first_batch)) {
        capture_batch(processor);
    }
    ++g_dbg.batches;
    g_dbg.draws += processor.draw_count;
    g_dbg.depth_test = processor.state.depth_test ? 1 : 0;
    g_dbg.depth_write = processor.state.depth_write ? 1 : 0;
    g_dbg.depth_func = static_cast<int>(processor.state.depth_func);
    g_dbg.cull = static_cast<int>(processor.state.cull_mode);

    if (first_batch) {
        // The frame's EFB extent: the union of every draw's viewport.
        float efb_w = 0.0f, efb_h = 0.0f;
        for (std::uint32_t d = 0; d < processor.draw_count; ++d) {
            const DrawExtra* x = draw_extra(processor, d);
            float r = 0.0f, b = 0.0f;
            if (x != nullptr) {
                viewport_extent(*x, &r, &b);
            } else {
                viewport_extent(processor.state, &r, &b);
            }
            efb_w = r > efb_w ? r : efb_w;
            efb_h = b > efb_h ? b : efb_h;
        }
        if (processor.draw_count == 0) {
            viewport_extent(processor.state, &efb_w, &efb_h);
        }
        set_frame_mapping(efb_w, efb_h);
    }

    const std::uint8_t pf0 = processor.draw_count != 0 ? processor.draws[0].pixel_fmt : processor.state.pixel_fmt;
    apply_raster(processor.state, pf0);
    glFrontFace(GL_CW);
    RasterKey applied = raster_key(processor.state, pf0);

    // The decoder (and the vertex jobs) already packed the batch: per-vertex offsets into
    // its arena, and a remap for vertices that equal an earlier one. Only a batch that did not
    // come from Processor::process (tests) is packed here, whole, with as many texcoord sets
    // as its draws use.
    PackedBatch packed{};
    const bool prepacked = packed_batch(processor, &packed);
    if (!prepacked) {
        unsigned ntc = 1;
        for (std::uint32_t d = 0; d < processor.draw_count; ++d) {
            if (processor.draws[d].num_tex_gens > ntc) {
                ntc = processor.draws[d].num_tex_gens;
            }
        }
        g_vlayout = layout_for(ntc);
        pack_vertices(processor, 0, processor.vert_count, g_vbuf, g_vlayout);
    }
    static std::uint16_t run_idx[kMaxDecodedVerts * 3];
    static std::uint16_t draw_idx[kMaxDecodedVerts * 3];

    const EfbCopyEvent* copies = nullptr;
    const unsigned copy_count = efb_copy_events(&copies);
    unsigned next_copy = 0;
    if (g_efb_clear_pending) {
        // The frame's leading copies observe what the previous frame left in the EFB, so
        // they go before the new frame's clear.
        bool copied = false;
        while (next_copy < copy_count && copies[next_copy].draw_index == 0) {
            perform_efb_copy_deferred(copies[next_copy++]);
            copied = true;
        }
        flush_copy_clears();
        clear_efb_now(g_efb_clear_rgba);
        g_efb_clear_pending = false;
        (void)copied;
        apply_raster(processor.state, pf0);  // the clear touched scissor and masks
    }
    perf::count(perf::kDraws, processor.draw_count);
    for (std::uint32_t d = 0; d < processor.draw_count;) {
        const DrawCall& draw = processor.draws[d];
        // A copy recorded before draw d observes draws [0, d) - GX's own ordering.
        bool copied = false;
        while (next_copy < copy_count && copies[next_copy].draw_index <= d) {
            perform_efb_copy_deferred(copies[next_copy++]);
            copied = true;
        }
        flush_copy_clears();
        if (!g_efb_draw_bound) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
            g_efb_draw_bound = true;
            copied = true;
        }
        const DrawExtra* x = draw_extra(processor, d);
        const RasterKey want = x != nullptr ? raster_key(*x, draw.pixel_fmt) : raster_key(processor.state, draw.pixel_fmt);
        // A copy's clear touches scissor, masks and depth, so re-apply after one.
        if (copied || std::memcmp(&want, &applied, sizeof(want)) != 0) {
            if (x != nullptr) {
                apply_raster(*x, draw.pixel_fmt);
            } else {
                apply_raster(processor.state, draw.pixel_fmt);
            }
            applied = want;
            ++g_dbg.rasterchg;
        }
        if (draw.vertex_count == 0) {
            ++g_dbg.skipped;
            ++d;
            continue;
        }
        if (const GpuMeshView* mesh = gpu_mesh_view(d)) {
            if (draw_mesh_run(processor, draw, x, *mesh)) {
                ++g_dbg.issued;
            } else {
                ++g_dbg.skipped;
            }
            ++d;
            continue;
        }
        if (d == 0 && first_batch) {
            g_dbg.nidx0 = expanded_index_count(draw.prim, draw.vertex_count);
        }
        const bool glit = draw_gpu_lit(processor, d) && x != nullptr && x->lit.on != 0;
        const std::uint64_t index_t0 = perf::now_us();
        // Extend a run over the following draws that differ only in their vertices: one
        // glDrawElements instead of one per GXBegin. A copy between two draws ends the run.
        std::uint32_t e = d + 1;
        std::uint32_t vmin = draw.first_vertex;
        std::uint32_t vmax = draw.first_vertex + draw.vertex_count;
        while (g_merge_draws && e < processor.draw_count) {
            if (next_copy < copy_count && copies[next_copy].draw_index <= e) {
                break;
            }
            const DrawCall& n = processor.draws[e];
            if (n.vertex_count == 0 || gpu_mesh_view(e) != nullptr || draw_gpu_lit(processor, e) != draw_gpu_lit(processor, d) ||
                !draws_mergeable(draw, n, x, draw_extra(processor, e), draw_merge_key(processor, d),
                                 draw_merge_key(processor, e))) {
                break;
            }
            const std::uint32_t nmin = n.first_vertex < vmin ? n.first_vertex : vmin;
            const std::uint32_t end = n.first_vertex + n.vertex_count;
            const std::uint32_t nmax = end > vmax ? end : vmax;
            if (nmax - nmin > 0x10000u) {
                break;
            }
            vmin = nmin;
            vmax = nmax;
            ++e;
        }
        std::uint32_t nidx = 0;
        const std::uint8_t* vbase = nullptr;
        if (prepacked) {
            // Batch-global indices through the remap, then relative to the lowest one.
            std::uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (std::uint32_t k = d; k < e; ++k) {
                const DrawCall& dk = processor.draws[k];
                const std::uint32_t n = expand_indices(dk.prim, dk.vertex_count, draw_idx, kMaxDecodedVerts * 3);
                if (n > kMaxDecodedVerts * 3 - nidx) {
                    e = k;  // cannot happen: a batch's indices always fit
                    break;
                }
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::uint32_t g = dk.first_vertex + draw_idx[i];
                    const std::int32_t r = packed.remap[g];
                    if (r >= 0) {
                        g = static_cast<std::uint32_t>(r);
                    }
                    lo = g < lo ? g : lo;
                    hi = g > hi ? g : hi;
                    run_idx[nidx + i] = static_cast<std::uint16_t>(g);
                }
                nidx += n;
            }
            g_vlayout = layout_for(draw.num_tex_gens, glit);
            if (nidx != 0) {
                for (std::uint32_t i = 0; i < nidx; ++i) {
                    run_idx[i] = static_cast<std::uint16_t>(run_idx[i] - lo);
                }
                // Every vertex from lo to hi must sit in one stretch of the arena at this
                // layout's stride; it always does (one state, one stride), but if not, pack.
                if (packed.segment[lo] == packed.segment[hi] &&
                    packed.offset[hi] - packed.offset[lo] == (hi - lo) * g_vlayout.stride) {
                    vbase = packed.arena + packed.offset[lo];
                } else {
                    // Gather the referenced packed vertices (Processor::verts is not kept up to
                    // date by the vertex jobs any more; see vertex_worker_set_mirror). Everything
                    // a run references has the run's layout.
                    ++g_dbg.gathers;
                    const std::uint32_t st = g_vlayout.stride;
                    for (std::uint32_t i = 0; i < nidx; ++i) {
                        const std::uint32_t v = lo + run_idx[i];
                        std::memcpy(g_vbuf + static_cast<std::size_t>(run_idx[i]) * st, packed.arena + packed.offset[v], st);
                    }
                    vbase = g_vbuf;
                }
            }
        } else {
            for (std::uint32_t k = d; k < e; ++k) {
                const DrawCall& dk = processor.draws[k];
                const std::uint32_t n = expand_indices(dk.prim, dk.vertex_count, draw_idx, kMaxDecodedVerts * 3);
                if (n > kMaxDecodedVerts * 3 - nidx) {
                    e = k;  // cannot happen: a batch's indices always fit
                    break;
                }
                const std::uint16_t base = static_cast<std::uint16_t>(dk.first_vertex - vmin);
                for (std::uint32_t i = 0; i < n; ++i) {
                    run_idx[nidx + i] = static_cast<std::uint16_t>(draw_idx[i] + base);
                }
                nidx += n;
            }
            vbase = g_vbuf + static_cast<std::size_t>(vmin) * g_vlayout.stride;
        }
        if (e == d) {
            e = d + 1;
        }
        const std::uint64_t draw_t0 = perf::now_us();
        perf::add(perf::kSubmitIndex, draw_t0 - index_t0);
        g_cur_lit = glit ? &x->lit : nullptr;
        const bool drew = nidx != 0 && draw_run(processor, draw, x, run_idx, nidx, vbase);
        g_cur_lit = nullptr;
        perf::add(perf::kSubmitDraw, perf::now_us() - draw_t0);
        if (drew) {
            g_dbg.issued += e - d;
        } else {
            g_dbg.skipped += e - d;
        }
        d = e;
    }
    while (next_copy < copy_count) {
        perform_efb_copy_deferred(copies[next_copy++]);
    }
    flush_copy_clears();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);  // the backend's clear must reach every channel
    glDepthMask(GL_TRUE);
    capture_gpu_checkpoint("batch_end");
    g_frame_open = true;
    g_dbg.progcount = g_program_count;
    g_dbg.gl_err = static_cast<int>(glGetError());
    return g_dbg.issued != 0;
}

bool efb_target_init() {
    if (g_efb_target) {
        return true;
    }
    glGenTextures(1, &g_efb_color);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kEfbTargetW, kEfbTargetH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &g_efb_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_efb_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kEfbTargetW, kEfbTargetH);
    glGenFramebuffers(1, &g_efb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_efb_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_efb_depth);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (g_efb_fbo == 0 || g_efb_color == 0 || status != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("mkwv: EFB target unavailable (status=0x%X), drawing to the back buffer\n",
                    static_cast<unsigned>(status));
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    g_efb_target = true;
    g_rt_w = kEfbTargetW;
    g_rt_h = kEfbTargetH;
    clear_efb_now(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::printf("mkwv: EFB target %dx%d (scale %.3f)\n", kEfbTargetW, kEfbTargetH, static_cast<double>(g_efb_scale));
    return true;
}

bool efb_target_active() { return g_efb_target; }

void begin_efb_frame(std::uint32_t clear_rgba) {
    if (!g_efb_target) {
        clear_efb_now(clear_rgba);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    g_efb_clear_pending = true;
    g_efb_clear_rgba = clear_rgba;
}

bool present_efb() {
    if (!g_efb_target) {
        return false;
    }
    if (g_efb_clear_pending) {
        // A frame with nothing submitted still starts from a cleared EFB.
        clear_efb_now(g_efb_clear_rgba);
        g_efb_clear_pending = false;
    }
    // What the guest's display copy would take: GXSetDispCopySrc's rect, or failing that
    // the frame's EFB extent capped to a display's worth of lines.
    int ix = 0, iy = 0, iw = 0, ih = 0;
    const bool have_src = display_copy_source(&ix, &iy, &iw, &ih);
    std::uint32_t sx = ix > 0 ? static_cast<std::uint32_t>(ix) : 0u;
    std::uint32_t sy = iy > 0 ? static_cast<std::uint32_t>(iy) : 0u;
    std::uint32_t sw = iw > 0 ? static_cast<std::uint32_t>(iw) : 0u;
    std::uint32_t sh = ih > 0 ? static_cast<std::uint32_t>(ih) : 0u;
    if (!have_src || sw < 2u || sh < 2u) {
        sx = 0;
        sy = 0;
        sw = static_cast<std::uint32_t>(g_fb_efb_w);
        sh = static_cast<std::uint32_t>(g_fb_efb_h > 480.0f ? 480.0f : g_fb_efb_h);
    }
    if (sx + sw > 640u) {
        sw = sx < 640u ? 640u - sx : 1u;
    }
    if (sy + sh > 528u) {
        sh = sy < 528u ? 528u - sy : 1u;
    }
    g_present_src[0] = sx;
    g_present_src[1] = sy;
    g_present_src[2] = sw;
    g_present_src[3] = sh;
    // The EFB stays 608x456. Widescreen (SCGetAspectRatio is already 1) squeezes a wider
    // canvas into those pixels. A widescreen TV stretches that rect to 16:9; fitting it
    // with square pixels puts the side bars back. The Vita panel is that stretch, so the
    // same pixels cover 960x544 and the render size does not change.
    const int dx = 0;
    const int dy = 0;
    const int dw = kDisplayW;
    const int dh = kDisplayH;
    static bool logged_present = false;
    if (!logged_present) {
        std::printf("mkwv: present %ux%u stretched to %dx%d\n", sw, sh, dw, dh);
        logged_present = true;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glViewport(0, 0, kDisplayW, kDisplayH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // vitaGL's blit takes both rects with a top-left origin (it converts the destination
    // with 1 - 2y/h and samples a flipped FBO at (h - y)/h), which is the EFB's own
    // orientation, so neither rect is flipped here.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    glBlitFramebuffer(static_cast<GLint>(static_cast<float>(sx) * g_efb_scale + 0.5f),
                      static_cast<GLint>(static_cast<float>(sy) * g_efb_scale + 0.5f),
                      static_cast<GLint>(static_cast<float>(sx + sw) * g_efb_scale + 0.5f),
                      static_cast<GLint>(static_cast<float>(sy + sh) * g_efb_scale + 0.5f), dx, dy, dx + dw, dy + dh,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    // Diagnostic build: eight retained batch images in reading order; bottom
    // right is the live final output. No RAM readbacks or stalls for this sheet.
    if (g_race_probe_count) {
        glClear(GL_COLOR_BUFFER_BIT);
        for (unsigned i = 0; i < g_race_probe_count; ++i) {
            const int x = (i % 3) * 320 + 56, y = (i / 3) * 181;
            glBlitNamedFramebuffer(g_race_probe[i].fbo, 0, 0, 0, kProbeW, kProbeH,
                                  x, y, x + 207, y + 181, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        glBlitNamedFramebuffer(g_efb_fbo, 0, 0, 0, g_rt_w, g_rt_h,
                              696, 362, 903, 543, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return true;
}

void end_submit_frame() {
    g_frame_open = false;
    if (capture_frame_end()) {
        std::printf("mkwv: %s\n", capture_last_summary());
    }
}

void render_demo(float time_seconds) {
    static Fifo fifo;
    static Processor processor;
    if (!build_demo_frame(fifo, time_seconds)) {
        return;
    }
    processor.reset();
    if (!processor.process(fifo.data(), fifo.size(), true)) {
        return;
    }
    submit_processor(processor);
    end_submit_frame();
}

// A drain that carries EFB copies but no draws. It happens whenever the guest issues a copy
// after the GXDrawDone that ended the scene's drain - the snapshot MKW takes when a dialog
// opens. Nothing is cleared or presented; the copies read the EFB target (or, without one,
// the front buffer), which holds the image the scene's drain just presented.
void submit_efb_copies_only(const Processor& processor) {
    const EfbCopyEvent* copies = nullptr;
    const unsigned n = efb_copy_events(&copies);
    if (n == 0) {
        return;
    }
    if (g_efb_target) {
        // The EFB target still holds the frame just presented (its clear waits for the next
        // frame), which is exactly what this copy observes.
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    }
    for (unsigned i = 0; i < n; ++i) {
        perform_efb_copy_deferred(copies[i]);
        ++g_efb_total_orphan;
    }
    flush_copy_clears();
    (void)processor;
}

void set_efb_scale(float scale) {
    g_efb_scale = scale < 0.5f ? 0.5f : (scale > 544.0f / 456.0f ? 544.0f / 456.0f : scale);
}

float efb_scale() { return g_efb_scale; }

void set_shader_path_enabled(bool enabled) { g_shader_path = enabled; }

void set_draw_merging(bool enabled) { g_merge_draws = enabled; }

void set_vertex_ring_buffer(unsigned vbo, const std::uint8_t* base, std::uint32_t bytes) {
    g_ring_vbo = vbo;
    g_ring_base = base;
    g_ring_bytes = bytes;
}

void set_effects_level(int level) { g_effects_level = level < 0 ? 0 : (level > 2 ? 2 : level); }

void set_race_probe_sheet(bool enabled) { g_race_probe_sheet = enabled; }

bool shader_path_enabled() { return g_shader_path; }

bool frame_submit_open() { return g_frame_open; }

unsigned copy_fbos_in_use() {
    unsigned n = 0;
    for (const CopyFbo& c : g_copy_fbos) {
        n += c.fbo != 0 ? 1u : 0u;
    }
    return n;
}

const char* last_submit_debug() {
    // " fmt:binds/fails" for every GX texture format bound this frame (0=I4 1=I8 2=IA4
    // 3=IA8 4=RGB565 5=RGB5A3 6=RGBA8 8=CI4 9=CI8 10=CI14X2 14=CMPR).
    char fmt_text[160];
    int fn = 0;
    fmt_text[0] = '\0';
    for (unsigned f = 0; f < 16u && fn >= 0 && fn < static_cast<int>(sizeof(fmt_text)) - 16; ++f) {
        if (g_dbg.fmt_bind[f] != 0 || g_dbg.fmt_fail[f] != 0) {
            fn += std::snprintf(fmt_text + fn, sizeof(fmt_text) - static_cast<std::size_t>(fn), " %u:%u/%u", f,
                                static_cast<unsigned>(g_dbg.fmt_bind[f]), static_cast<unsigned>(g_dbg.fmt_fail[f]));
        }
    }
    std::snprintf(g_dbg_text, sizeof(g_dbg_text),
                  "mkwv: SUBMIT gl draws=%u batches=%u nidx0=%u issued=%u skip=%u comb=%u cfail=%u | prog=%u pfail=%u "
                  "pmake=%u pcount=%u pevict=%u plast=%d efb=%u/%u efbtot(done=%u front=%u orphan=%u hits=%u) rchg=%u "
                  "efbdim=%.0fx%.0f rt=%s disp=%u,%u %ux%u logic=%u dsta=%u | tex dec=%u reval=%u conv=%d bind=%d %ux%u bytes=%u | "
                  "depth=%d/%d fn=%d cull=%d | glerr=%d cclrfail=%u cfbo=%u/%u | ring=%s gather=%u | tfmt%s\n",
                  g_dbg.draws, g_dbg.batches, g_dbg.nidx0, g_dbg.issued, g_dbg.skipped, g_dbg.comb, g_dbg.cfail,
                  g_dbg.prog, g_dbg.progfail, g_dbg.progmake, g_dbg.progcount, g_dbg.progevict, g_dbg.proglast,
                  g_dbg.efbcopy, g_dbg.efbhit, g_efb_total_done, g_efb_total_front, g_efb_total_orphan,
                  g_efb_total_hits, g_dbg.rasterchg, static_cast<double>(g_fb_efb_w), static_cast<double>(g_fb_efb_h),
                  g_efb_target ? "efb" : "back", g_present_src[0], g_present_src[1], g_present_src[2], g_present_src[3],
                  g_dbg.logicop, g_dbg.dstalpha, g_dbg.texdecode, g_dbg.texrevalid, g_dbg.tex_convert, g_dbg.tex_bind, g_dbg.tex_w,
                  g_dbg.tex_h, g_dbg.tex_bytes, g_dbg.depth_test, g_dbg.depth_write, g_dbg.depth_func, g_dbg.cull,
                  g_dbg.gl_err, g_copy_clear_failures, copy_fbos_in_use(), g_copy_fbo_resizes,
                  g_ring_vbo != 0 ? "on" : "off", g_dbg.gathers, fmt_text);
    return g_dbg_text;
}

} // namespace mkwv::gx
