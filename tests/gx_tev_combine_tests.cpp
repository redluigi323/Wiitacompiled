// Proves the GX TEV stage -> GL_COMBINE mapping in vita/src/gx/gl_submit.cpp.
//
// The renderer cannot be exercised on the host, so this TU includes gl_submit.cpp
// directly (tests/stub_gl/vitaGL.h stands in for the real header) and supplies
// recording implementations of the GL entry points. Each case then evaluates two
// things from the same inputs - GX's own expression, d + a*(1-c) + b*c, and whatever
// the mapper actually programmed into the texture unit - and requires them to agree.
//
// A mapping bug here costs a full Vita rebuild and a run to notice, which is the
// entire reason this file exists.

#include <vitaGL.h>
#include "mkwv/gx/dl_cache.hpp"
#include <vector>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <chrono>

namespace {

struct EnvRecord {
    GLint mode = 0;
    GLint comb_rgb = 0;
    GLint comb_alpha = 0;
    GLint src_rgb[3]{};
    GLint op_rgb[3]{};
    GLint src_alpha[3]{};
    GLint op_alpha[3]{};
    float rgb_scale = 1.0f;
    float alpha_scale = 1.0f;
    float env_color[4]{};
};

EnvRecord g_env;
unsigned g_depth_aliases = 0;
unsigned g_finishes = 0, g_mapped_reads = 0;

} // namespace

extern "C" {
void glAlphaFunc(GLenum, GLclampf) {}
void glBindTexture(GLenum, GLuint) {}
void glBlendEquation(GLenum) {}
void glDepthRangef(GLfloat, GLfloat) {}
void glClearDepthf(GLclampf) {}
void glGenerateMipmap(GLenum) {}
void vglTexImageDepthBuffer(GLenum) { ++g_depth_aliases; }
void glBlendFunc(GLenum, GLenum) {}
void glColorPointer(GLint, GLenum, GLsizei, const GLvoid*) {}
void glCullFace(GLenum) {}
void glDeleteTextures(GLsizei, const GLuint*) {}
void glDepthFunc(GLenum) {}
void glDepthMask(GLboolean) {}
void glDisable(GLenum) {}
void glDisableClientState(GLenum) {}
void glDrawElements(GLenum, GLsizei, GLenum, const GLvoid*);
void glEnable(GLenum) {}
void glEnableClientState(GLenum) {}
void glFrontFace(GLenum) {}
void glGenTextures(GLsizei n, GLuint* out) {
    static GLuint next = 1;
    for (GLsizei i = 0; i < n; ++i) {
        out[i] = next++;
    }
}
GLenum glGetError(void) { return 0; /* GL_NO_ERROR */ }
void glScissor(GLint, GLint, GLsizei, GLsizei) {}
void glTexCoordPointer(GLint, GLenum, GLsizei, const GLvoid*) {}
void glTexEnvf(GLenum, GLenum, GLfloat) {}
void glTexEnvfv(GLenum, GLenum pname, GLfloat* param) {
    if (pname == GL_TEXTURE_ENV_COLOR) {
        for (int i = 0; i < 4; ++i) {
            g_env.env_color[i] = param[i];
        }
    } else if (pname == GL_RGB_SCALE) {
        g_env.rgb_scale = param[0];
    } else if (pname == GL_ALPHA_SCALE) {
        g_env.alpha_scale = param[0];
    }
}
void glTexEnvi(GLenum, GLenum pname, GLint param) {
    switch (pname) {
        case GL_TEXTURE_ENV_MODE: g_env.mode = param; break;
        case GL_COMBINE_RGB: g_env.comb_rgb = param; break;
        case GL_COMBINE_ALPHA: g_env.comb_alpha = param; break;
        case GL_SRC0_RGB: g_env.src_rgb[0] = param; break;
        case GL_SRC1_RGB: g_env.src_rgb[1] = param; break;
        case GL_SRC2_RGB: g_env.src_rgb[2] = param; break;
        case GL_OPERAND0_RGB: g_env.op_rgb[0] = param; break;
        case GL_OPERAND1_RGB: g_env.op_rgb[1] = param; break;
        case GL_OPERAND2_RGB: g_env.op_rgb[2] = param; break;
        case GL_SRC0_ALPHA: g_env.src_alpha[0] = param; break;
        case GL_SRC1_ALPHA: g_env.src_alpha[1] = param; break;
        case GL_SRC2_ALPHA: g_env.src_alpha[2] = param; break;
        case GL_OPERAND0_ALPHA: g_env.op_alpha[0] = param; break;
        case GL_OPERAND1_ALPHA: g_env.op_alpha[1] = param; break;
        case GL_OPERAND2_ALPHA: g_env.op_alpha[2] = param; break;
        default: break;
    }
}
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
void glTexParameteri(GLenum, GLenum, GLint) {}
void glVertexPointer(GLint, GLenum, GLsizei, const GLvoid*) {}
void glViewport(GLint, GLint, GLsizei, GLsizei) {}
}

// ---- recording stand-ins for the GLES2 shader path ------------------------
// The program cache in gl_submit.cpp can be exercised on the host with a GL that hands
// back valid handles and remembers what it was told. That is enough to check the parts
// most likely to be wrong: that the emitted pair compiles as two shaders, that the
// attribute and uniform names the cache looks up are the names the emitter actually
// wrote, that the draw is issued, and - the one that matters for frame time - that a
// second draw with the same TEV structure reuses the program instead of recompiling.
struct GlRecord {
    int shaders_created = 0;
    int glsl_shaders = 0;  // created as GLSL (plain GL_*_SHADER): vitaGL translates those
    int programs_created = 0;
    int compiles = 0;
    int links = 0;
    int draws = 0;
    int last_vertex_shader = 0;
    char vsrc[16384]{};
    char fsrc[16384]{};
    bool uniform_set[16]{};
    int attrib_lookups_failed = 0;
    int uniform_lookups_failed = 0;
    GLuint bound_program = 0;
};

GlRecord g_gl;

// EFB-copy recording: the fake framebuffer encodes each pixel's GL row (bottom-up) in red
// and its column in green, so the flip and the resample can be read straight off the
// uploaded texture.
struct CopyRecord {
    int reads = 0;
    GLenum read_format = 0;
    int read_x = 0, read_y = 0, read_w = 0, read_h = 0;
    int uploads = 0;
    int up_w = 0, up_h = 0;
    std::uint8_t up_px[64 * 64 * 4]{};
    int clears = 0;
    float clear_rgba[4]{};
    GLenum read_buffer = 0x0405;          // GL_BACK
    GLenum read_buffer_at_read = 0x0405;  // which buffer the last glReadPixels actually read
};
CopyRecord g_copy;
void g_fbo_read_hook();

extern "C" {
void glReadBuffer(GLenum mode) { g_copy.read_buffer = mode; }
void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean) {}
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum, GLvoid* data) {
    ++g_copy.reads;
    g_copy.read_format = format;
    g_copy.read_buffer_at_read = g_copy.read_buffer;
    g_fbo_read_hook();
    g_copy.read_x = x;
    g_copy.read_y = y;
    g_copy.read_w = w;
    g_copy.read_h = h;
    std::uint8_t* out = static_cast<std::uint8_t*>(data);
    for (GLsizei row = 0; row < h; ++row) {
        for (GLsizei col = 0; col < w; ++col) {
            std::uint8_t* px = out + (static_cast<std::size_t>(row) * w + col) * (format == GL_RGB ? 3u : 4u);
            px[0] = static_cast<std::uint8_t>(row * 10);  // bottom-up row
            px[1] = static_cast<std::uint8_t>(col * 10);
            px[2] = 40;
            if (format != GL_RGB) px[3] = 200;
        }
    }
}
void glFinish(void) { ++g_finishes; }
void glBindBuffer(GLenum, GLuint) {}
void glGenBuffers(GLsizei n, GLuint* out) { for (GLsizei i = 0; i < n; ++i) out[i] = 0; }
const void* g_vbo_ptr = nullptr;
void vglBufferData(GLenum, const GLvoid* p) { g_vbo_ptr = p; }
void* vglForceAlloc(uint32_t size) { return std::malloc(size); }
void vglReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid* data) {
    if (g_finishes) ++g_mapped_reads;
    glReadPixels(x, y, w, h, format, type, data);
}
void glTexImage2D(GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum, GLenum, const GLvoid* px) {
    ++g_copy.uploads;
    g_copy.up_w = w;
    g_copy.up_h = h;
    if (px != nullptr && w <= 64 && h <= 64) {
        std::memcpy(g_copy.up_px, px, static_cast<std::size_t>(w) * h * 4u);
    }
}
GLboolean glIsEnabled(GLenum) { return 0; }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    g_copy.clear_rgba[0] = r;
    g_copy.clear_rgba[1] = g;
    g_copy.clear_rgba[2] = b;
    g_copy.clear_rgba[3] = a;
}
void glClear(GLbitfield) { ++g_copy.clears; }
}

// Framebuffer objects: records what the EFB target set-up and present asked for.
struct FboRecord {
    GLenum status = 0x8CD5;  // GL_FRAMEBUFFER_COMPLETE
    GLuint next_id = 100;
    GLuint draw_fb = 0;
    GLuint read_fb = 0;
    int blits = 0;
    GLint src[4]{};
    GLint dst[4]{};
    GLuint blit_read_fb = 0;
    GLuint blit_draw_fb = 0;
    GLuint read_fb_at_read = 0;  // which framebuffer the last glReadPixels read
};
FboRecord g_fbo;
void g_fbo_read_hook() { g_fbo.read_fb_at_read = g_fbo.read_fb; }

extern "C" {
void glGenFramebuffers(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; ++i) out[i] = g_fbo.next_id++;
}
void glBindFramebuffer(GLenum target, GLuint fb) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) g_fbo.draw_fb = fb;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) g_fbo.read_fb = fb;
}
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {}
void glGenRenderbuffers(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; ++i) out[i] = g_fbo.next_id++;
}
void glBindRenderbuffer(GLenum, GLuint) {}
void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei) {}
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}
GLenum glCheckFramebufferStatus(GLenum) { return g_fbo.status; }
void glBlitNamedFramebuffer(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                            GLenum);
void glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat*) {}
void glUniform1f(GLint, GLfloat) {}
void glBlitNamedFramebuffer(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                            GLenum) {
    ++g_fbo.blits;
}
void glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                       GLbitfield, GLenum) {
    ++g_fbo.blits;
    g_fbo.src[0] = sx0; g_fbo.src[1] = sy0; g_fbo.src[2] = sx1; g_fbo.src[3] = sy1;
    g_fbo.dst[0] = dx0; g_fbo.dst[1] = dy0; g_fbo.dst[2] = dx1; g_fbo.dst[3] = dy1;
    g_fbo.blit_read_fb = g_fbo.read_fb;
    g_fbo.blit_draw_fb = g_fbo.draw_fb;
}
}

// A fake guest address space for the decoder's resolver: one buffer at 0x80500000.
std::uint8_t g_fake_guest[4096];
constexpr std::uint32_t kFakeGuestBase = 0x80500000u;
int g_resolver_calls = 0;
const std::uint8_t* fake_resolver(std::uint32_t addr, std::uint32_t size, unsigned) {
    ++g_resolver_calls;
    if (addr < kFakeGuestBase || addr + size > kFakeGuestBase + sizeof(g_fake_guest)) {
        return nullptr;
    }
    return g_fake_guest + (addr - kFakeGuestBase);
}

extern "C" {
void glDrawElements(GLenum, GLsizei, GLenum, const GLvoid*) { ++g_gl.draws; }
void glActiveTexture(GLenum) {}
GLuint glCreateShader(GLenum kind) {
    ++g_gl.shaders_created;
    if (kind == GL_VERTEX_SHADER || kind == GL_FRAGMENT_SHADER) ++g_gl.glsl_shaders;
    g_gl.last_vertex_shader = (kind == GL_CG_VERTEX_SHADER_EXT || kind == GL_VERTEX_SHADER) ? 1 : 0;
    return static_cast<GLuint>(g_gl.shaders_created);
}
void glShaderSource(GLuint, GLsizei count, const GLchar* const* src, const GLint*) {
    if (count < 1 || src == nullptr || src[0] == nullptr) {
        return;
    }
    char* dst = g_gl.last_vertex_shader ? g_gl.vsrc : g_gl.fsrc;
    const std::size_t cap = sizeof(g_gl.vsrc) - 1;
    std::size_t n = std::strlen(src[0]);
    if (n > cap) {
        n = cap;
    }
    std::memcpy(dst, src[0], n);
    dst[n] = '\0';
}
void glCompileShader(GLuint) { ++g_gl.compiles; }
void glGetShaderiv(GLuint, GLenum pname, GLint* out) {
    if (out) {
        *out = (pname == GL_COMPILE_STATUS) ? 1 : 0;
    }
}
void glDeleteShader(GLuint) {}
GLuint glCreateProgram(void) {
    ++g_gl.programs_created;
    return static_cast<GLuint>(100 + g_gl.programs_created);
}
void glAttachShader(GLuint, GLuint) {}
void glLinkProgram(GLuint) { ++g_gl.links; }
void glGetProgramiv(GLuint, GLenum pname, GLint* out) {
    if (out) {
        *out = (pname == GL_LINK_STATUS) ? 1 : 0;
    }
}
void glDeleteProgram(GLuint) {}
void glUseProgram(GLuint p) { g_gl.bound_program = p; }
// Resolve a name only if the emitted vertex source actually declares it. A cache that
// looks up a name the emitter never wrote should therefore be caught here, not on device.
GLint glGetAttribLocation(GLuint, const GLchar* name) {
    if (name == nullptr || std::strstr(g_gl.vsrc, name) == nullptr) {
        ++g_gl.attrib_lookups_failed;
        return -1;
    }
    if (std::strcmp(name, "aPosition") == 0) return 0;
    if (std::strcmp(name, "aColor") == 0) return 1;
    if (std::strcmp(name, "aTexcoord0") == 0) return 2;
    if (std::strcmp(name, "aTexcoord1") == 0) return 3;
    return -1;
}
GLint glGetUniformLocation(GLuint, const GLchar* name) {
    if (name == nullptr || std::strstr(g_gl.fsrc, name) == nullptr) {
        ++g_gl.uniform_lookups_failed;
        return -1;
    }
    static int next = 0;
    return next++ % 16;
}
void glUniform1i(GLint loc, GLint) {
    if (loc >= 0 && loc < 16) g_gl.uniform_set[loc] = true;
}
void glUniform2f(GLint loc, GLfloat, GLfloat) {
    if (loc >= 0 && loc < 16) g_gl.uniform_set[loc] = true;
}
void glUniform4f(GLint loc, GLfloat, GLfloat, GLfloat, GLfloat) {
    if (loc >= 0 && loc < 16) g_gl.uniform_set[loc] = true;
}
void glUniform4fv(GLint loc, GLsizei, const GLfloat*) {
    if (loc >= 0 && loc < 16) g_gl.uniform_set[loc] = true;
}
void glUniform3fv(GLint loc, GLsizei, const GLfloat*) {
    if (loc >= 0 && loc < 16) g_gl.uniform_set[loc] = true;
}
void glEnableVertexAttribArray(GLuint) {}
void glDisableVertexAttribArray(GLuint) {}
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {}
}

#include "../vita/src/gx/gl_submit.cpp"
#include "mkwv/gx/efb_copy.hpp"
#include "mkwv/gx/draw_state.hpp"

namespace {

using namespace mkwv::gx;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// The sampled values every case is evaluated against. Deliberately asymmetric so a
// swapped argument cannot pass by coincidence, and the alpha values differ from the
// colour values so an alpha combiner reading a colour source is caught.
struct Inputs {
    float tex[4];
    float ras[4];
    float konst[4];  // unused by the reference; konst lives on the draw, see kKonst
};

// Konst is a property of the draw, not of the sampled inputs: the mapper reads
// DrawCall::konst[kcsel - GX_TEV_KCSEL_K0] and pushes it into GL_TEXTURE_ENV_COLOR, so
// the test has to put a real value on the draw and check that exact value comes back.
// Packed the way the decoder packs it: r, g, b, a from low byte up.
constexpr std::uint32_t kKonst = 0xD98C5926u;
const float kKonstRef[4] = {
    static_cast<float>(kKonst & 0xFFu) / 255.0f,
    static_cast<float>((kKonst >> 8) & 0xFFu) / 255.0f,
    static_cast<float>((kKonst >> 16) & 0xFFu) / 255.0f,
    static_cast<float>((kKonst >> 24) & 0xFFu) / 255.0f,
};

const Inputs kInputs[] = {
    {{0.25f, 0.50f, 0.75f, 0.10f}, {0.90f, 0.20f, 0.40f, 0.60f}, {0.15f, 0.35f, 0.55f, 0.85f}},
    {{1.00f, 0.00f, 0.50f, 0.00f}, {0.30f, 0.70f, 0.10f, 0.00f}, {0.20f, 0.20f, 0.20f, 1.00f}},
    {{0.60f, 0.60f, 0.60f, 1.00f}, {1.00f, 1.00f, 1.00f, 1.00f}, {0.00f, 0.50f, 1.00f, 0.25f}},
};

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// --- GX reference -----------------------------------------------------------
float gx_color_arg(std::uint8_t arg, const Inputs& in, int ch) {
    switch (arg) {
        case CcTexC: return in.tex[ch];
        case CcTexA: return in.tex[3];
        case CcRasC: return in.ras[ch];
        case CcRasA: return in.ras[3];
        case CcKonst: return kKonstRef[ch];
        case CcOne: return 1.0f;
        case CcHalf: return 0.5f;
        case CcZero: return 0.0f;
        default: std::abort();
    }
}

float gx_alpha_arg(std::uint8_t arg, const Inputs& in) {
    switch (arg) {
        case CaTexA: return in.tex[3];
        case CaRasA: return in.ras[3];
        case CaKonst: return kKonstRef[3];
        case CaZero: return 0.0f;
        default: std::abort();
    }
}

float gx_eval(float a, float b, float c, float d, std::uint8_t op, float scale) {
    const float lerp = a * (1.0f - c) + b * c;
    const float out = (op == TevSub) ? (d - lerp) : (d + lerp);
    return clamp01(out * scale);
}

// --- what the mapper actually programmed ------------------------------------
float gl_source(GLint src, GLint op, const Inputs& in, int ch) {
    const float* v = nullptr;
    switch (src) {
        case GL_TEXTURE: v = in.tex; break;
        case GL_PRIMARY_COLOR: v = in.ras; break;
        case GL_CONSTANT: v = g_env.env_color; break;
        default: std::abort();
    }
    switch (op) {
        case GL_SRC_COLOR: return v[ch];
        case GL_ONE_MINUS_SRC_COLOR: return 1.0f - v[ch];
        case GL_SRC_ALPHA: return v[3];
        case GL_ONE_MINUS_SRC_ALPHA: return 1.0f - v[3];
        default: std::abort();
    }
}

float gl_eval(GLint func, const GLint* src, const GLint* op, const Inputs& in, int ch, float scale) {
    const float a0 = gl_source(src[0], op[0], in, ch);
    float out = a0;
    if (func == GL_MODULATE) {
        out = a0 * gl_source(src[1], op[1], in, ch);
    } else if (func == GL_ADD) {
        out = a0 + gl_source(src[1], op[1], in, ch);
    } else if (func == GL_SUBTRACT) {
        out = a0 - gl_source(src[1], op[1], in, ch);
    } else if (func == GL_INTERPOLATE) {
        const float a1 = gl_source(src[1], op[1], in, ch);
        const float a2 = gl_source(src[2], op[2], in, ch);
        out = a0 * a2 + a1 * (1.0f - a2);
    }
    return clamp01(out * scale);
}

DrawCall make_draw(const TevStage& s, unsigned stages) {
    DrawCall d;
    d.tex0 = true;
    d.konst[0] = kKonst;
    d.num_tev_stages = static_cast<std::uint8_t>(stages);
    for (unsigned i = 0; i < stages && i < kTevStages; ++i) {
        d.tev_stages[i] = s;
    }
    return d;
}

TevStage plain_stage() {
    TevStage s;
    s.color_a = CcZero;
    s.color_b = CcZero;
    s.color_c = CcZero;
    s.color_d = CcZero;
    s.alpha_a = CaZero;
    s.alpha_b = CaZero;
    s.alpha_c = CaZero;
    s.alpha_d = CaZero;
    s.color_op = TevAdd;
    s.alpha_op = TevAdd;
    s.color_bias = 0;
    s.alpha_bias = 0;
    s.color_shift = 0;
    s.alpha_shift = 0;
    s.color_clamp = true;
    s.alpha_clamp = true;
    s.tex_enabled = true;
    s.ras_chan = kRasColor0;
    s.kcsel = 12;  // GX_TEV_KCSEL_K0
    s.kasel = 28;  // GX_TEV_KASEL_K0_A
    return s;
}

// Requires the mapper to accept the stage and to reproduce GX's arithmetic exactly on
// every input triple, in every channel.
void expect_exact(const char* name, const TevStage& s, unsigned stages = 1) {
    g_env = EnvRecord{};
    const DrawCall d = make_draw(s, stages);
    if (!apply_tev_combine(d, true)) {
        std::printf("FAIL: %s - mapper refused a stage it should express\n", name);
        ++g_failures;
        return;
    }
    if (g_env.mode != GL_COMBINE) {
        std::printf("FAIL: %s - env mode is not GL_COMBINE\n", name);
        ++g_failures;
        return;
    }
    for (const Inputs& in : kInputs) {
        for (int ch = 0; ch < 3; ++ch) {
            const float want = gx_eval(gx_color_arg(s.color_a, in, ch), gx_color_arg(s.color_b, in, ch),
                                       gx_color_arg(s.color_c, in, ch), gx_color_arg(s.color_d, in, ch), s.color_op,
                                       g_env.rgb_scale);
            const float got = gl_eval(g_env.comb_rgb, g_env.src_rgb, g_env.op_rgb, in, ch, g_env.rgb_scale);
            if (std::fabs(want - got) > 1.0e-5f) {
                std::printf("FAIL: %s - rgb ch%d want %.5f got %.5f\n", name, ch, want, got);
                ++g_failures;
            }
        }
        const float want_a = gx_eval(gx_alpha_arg(s.alpha_a, in), gx_alpha_arg(s.alpha_b, in),
                                     gx_alpha_arg(s.alpha_c, in), gx_alpha_arg(s.alpha_d, in), s.alpha_op,
                                     g_env.alpha_scale);
        const float got_a = gl_eval(g_env.comb_alpha, g_env.src_alpha, g_env.op_alpha, in, 3, g_env.alpha_scale);
        if (std::fabs(want_a - got_a) > 1.0e-5f) {
            std::printf("FAIL: %s - alpha want %.5f got %.5f\n", name, want_a, got_a);
            ++g_failures;
        }
    }
}

void expect_fallback(const char* name, const TevStage& s, unsigned stages = 1) {
    g_env = EnvRecord{};
    const DrawCall d = make_draw(s, stages);
    if (apply_tev_combine(d, true)) {
        std::printf("FAIL: %s - mapper claimed a stage it cannot express\n", name);
        ++g_failures;
    }
}

} // namespace

int main() {
    // The material this whole exercise is about: colour is the texture, alpha is the
    // rasterised alpha. GL_REPLACE forced alpha = texture alpha and painted these
    // opaque; the combiner has to keep the two apart.
    {
        TevStage s = plain_stage();
        s.color_d = CcTexC;
        s.alpha_d = CaRasA;
        expect_exact("replace-colour, ras alpha", s, 2);

        // And prove the point directly: zero rasterised alpha must survive as zero.
        g_env = EnvRecord{};
        const DrawCall d = make_draw(s, 2);
        check(apply_tev_combine(d, true), "replace/ras-alpha must map");
        const Inputs opaque_tex{{1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 0.0f}, {0, 0, 0, 0}};
        const float a = gl_eval(g_env.comb_alpha, g_env.src_alpha, g_env.op_alpha, opaque_tex, 3, g_env.alpha_scale);
        check(a == 0.0f, "an opaque texture with zero ras alpha must come out transparent");
    }
    {
        TevStage s = plain_stage();  // out = tex * ras
        s.color_b = CcTexC;
        s.color_c = CcRasC;
        s.alpha_b = CaTexA;
        s.alpha_c = CaRasA;
        expect_exact("modulate", s);
    }
    {
        TevStage s = plain_stage();  // out = ras*(1-texA) + tex*texA
        s.color_a = CcRasC;
        s.color_b = CcTexC;
        s.color_c = CcTexA;
        s.alpha_d = CaRasA;
        expect_exact("decal", s);
    }
    {
        TevStage s = plain_stage();  // out = tex * (1 - rasA), the b == ZERO branch
        s.color_a = CcTexC;
        s.color_c = CcRasA;
        s.alpha_d = CaTexA;
        expect_exact("one-minus modulate", s);
    }
    {
        TevStage s = plain_stage();  // c == ZERO collapses the lerp to a
        s.color_a = CcTexC;
        s.color_b = CcRasC;
        s.alpha_d = CaTexA;
        expect_exact("c=zero collapses to a", s);
    }
    {
        TevStage s = plain_stage();  // c == ONE collapses the lerp to b
        s.color_a = CcTexC;
        s.color_b = CcRasC;
        s.color_c = CcOne;
        s.alpha_d = CaTexA;
        expect_exact("c=one collapses to b", s);
    }
    {
        TevStage s = plain_stage();  // d + a, with the lerp collapsed
        s.color_a = CcTexC;
        s.color_d = CcRasC;
        s.alpha_a = CaTexA;
        s.alpha_d = CaRasA;
        expect_exact("add", s);
    }
    {
        TevStage s = plain_stage();  // konst through GL_CONSTANT
        s.color_b = CcTexC;
        s.color_c = CcKonst;
        s.alpha_b = CaTexA;
        s.alpha_c = CaKonst;
        expect_exact("konst modulate", s);
    }
    {
        TevStage s = plain_stage();  // 2x shift
        s.color_b = CcTexC;
        s.color_c = CcRasC;
        s.color_shift = 1;
        s.alpha_d = CaTexA;
        expect_exact("rgb scale 2x", s);
    }

    // Things the mapper must hand back rather than approximate.
    {
        TevStage s = plain_stage();  // PASSCLR: samples nothing, so the unit is wrong for it
        s.color_d = CcRasC;
        s.alpha_d = CaRasA;
        expect_fallback("passclr samples nothing", s);
    }
    {
        TevStage s = plain_stage();  // a real lerp with a non-zero d has no GL form
        s.color_a = CcRasC;
        s.color_b = CcTexC;
        s.color_c = CcTexA;
        s.color_d = CcKonst;
        s.alpha_d = CaTexA;
        expect_fallback("lerp plus d", s);
    }
    {
        TevStage s = plain_stage();  // GX's divide-by-two; vitaGL only accepts 1/2/4
        s.color_d = CcTexC;
        s.color_shift = 3;
        s.alpha_d = CaRasA;
        expect_fallback("shift divide-by-two", s);
    }
    {
        TevStage s = plain_stage();  // bias is not expressible
        s.color_d = CcTexC;
        s.color_bias = 1;
        s.alpha_d = CaRasA;
        expect_fallback("bias", s);
    }
    {
        TevStage s = plain_stage();  // reading CPREV means earlier stages actually matter
        s.color_d = CcTexC;
        s.color_a = CcCPrev;
        s.color_c = CcRasC;
        s.alpha_d = CaRasA;
        expect_fallback("reads cprev", s, 2);
    }
    {
        // The stage names TEXC while its own order says GX_TEXMAP_NULL. MKW does this on
        // nearly every draw, and a bound texture is still what the fallback path would
        // sample, so the mapper takes it rather than handing back 245 draws a frame.
        TevStage s = plain_stage();
        s.color_d = CcTexC;
        s.alpha_d = CaRasA;
        s.tex_enabled = false;
        expect_exact("tex order disabled but stage samples", s, 2);
    }
    {
        TevStage s = plain_stage();  // constant zero has no GL source
        s.alpha_d = CaRasA;
        expect_fallback("constant zero colour", s);
    }


    // ---- the program cache, end to end -----------------------------------
    // A fake GL cannot tell us the shader is correct, but it can tell us the wiring is:
    // that the pair compiles as two shaders, that every name the cache looks up is a name
    // the emitter wrote, that the draw is issued, and that identical structure reuses the
    // program. The last one is the whole point of hash_tev_program - a miss here means a
    // shader compile per draw, which on this hardware is not survivable.
    {
        static Processor pr;
        pr.reset();
        pr.state.texmaps[0].data = reinterpret_cast<const std::uint8_t*>(&kInputs[0]);
        pr.state.texmaps[0].width = 4;
        pr.state.texmaps[0].height = 4;
        pr.state.texmaps[0].format = TexRGBA8PC;
        pr.vert_count = 4;
        for (unsigned v = 0; v < 4u; ++v) {
            pr.verts[v] = DecodedVertex{};
        }

        TevStage st = plain_stage();
        st.color_a = CcZero;
        st.color_b = CcTexC;
        st.color_c = CcRasC;
        st.color_d = CcZero;
        st.alpha_b = CaTexA;
        st.alpha_c = CaRasA;
        DrawCall d = make_draw(st, 1);
        d.prim = Triangles;
        d.first_vertex = 0;
        d.vertex_count = 3;

        static std::uint16_t idx[8] = {0, 1, 2};
        static std::uint8_t pv[8 * 128]{};  // vertices in g_vlayout (zeroed)

        const int programs_before = g_gl.programs_created;
        const int draws_before = g_gl.draws;
        if (!draw_with_program(pr, d, nullptr, idx, 3, pv)) {
            std::printf("FAIL: program path declined a plain modulate draw (plast=%d)\n", g_dbg.proglast);
            ++g_failures;
        } else {
            if (g_gl.programs_created != programs_before + 1) {
                check(false, "the first draw of a new structure must build one program");
            }
            if (g_gl.compiles < 2) {
                check(false, "a vertex and a fragment shader must both be compiled");
            }
            if (g_gl.attrib_lookups_failed != 0) {
                std::printf("FAIL: the cache looked up %d attribute name(s) the emitter never wrote\n",
                            g_gl.attrib_lookups_failed);
                ++g_failures;
            }
            if (g_gl.uniform_lookups_failed != 0) {
                std::printf("FAIL: the cache looked up %d uniform name(s) the emitter never wrote\n",
                            g_gl.uniform_lookups_failed);
                ++g_failures;
            }
            if (g_gl.draws != draws_before + 1) {
                check(false, "the draw must actually be issued");
            }
            check(g_gl.bound_program != 0, "the program stays bound between draws (the state cache)");

            // Same structure, different colours: must reuse.
            int built = g_gl.programs_created;
            DrawCall d2 = d;
            d2.konst[0] = 0x01020304u;
            d2.tev_color[0] = 0xFFEEDDCCu;
            if (!draw_with_program(pr, d2, nullptr, idx, 3, pv)) {
                check(false, "the second draw should have reused the program");
            }
            if (g_gl.programs_created != built) {
                std::printf("FAIL: a colour-only change compiled a second program\n");
                ++g_failures;
            }

            // GXSetDstAlpha on an RGBA6 EFB: one pass unblended; blended, a second
            // constant-alpha pass (vitaGL has no constant blend factor).
            {
                pr.state.alpha_update = true;
                pr.state.color_update = true;
                DrawCall da = d;
                da.dst_alpha_enable = true;
                da.dst_alpha = 0x80;
                da.pixel_fmt = 1;  // GX_PF_RGBA6_Z24
                const int lookups_failed = g_gl.uniform_lookups_failed;
                int before = g_gl.draws;
                check(draw_call(pr, da, nullptr), "dst-alpha draw");
                check(g_gl.draws == before + 1, "an unblended dst-alpha draw is one pass");
                da.blend_mode = BlendMix;
                da.blend_src = BlSrcAlpha;
                da.blend_dst = BlInvSrcAlpha;
                before = g_gl.draws;
                check(draw_call(pr, da, nullptr), "blended dst-alpha draw");
                check(g_gl.draws == before + 2, "a blended dst-alpha draw needs the constant-alpha pass");
                check(g_gl.uniform_lookups_failed == lookups_failed, "uDstAlpha must exist in the program");
                da.pixel_fmt = 0;  // RGB8: no alpha in the EFB, nothing to replace
                before = g_gl.draws;
                check(draw_call(pr, da, nullptr), "rgb8 dst-alpha draw");
                check(g_gl.draws == before + 1, "without EFB alpha dst-alpha is inert");
                built = g_gl.programs_created;
            }

            // Different structure: must build a new one.
            DrawCall d3 = d;
            d3.tev_stages[0].color_c = CcKonst;
            d3.tev_stages[0].kcsel = 0x0Cu;
            if (!draw_with_program(pr, d3, nullptr, idx, 3, pv)) {
                check(false, "a konst material should still compile");
            }
            if (g_gl.programs_created != built + 1) {
                std::printf("FAIL: a structural change did not build a new program\n");
                ++g_failures;
            }

            // An indirect material: the three new uniforms must be names the emitter wrote.
            const int lookups_failed_before = g_gl.uniform_lookups_failed;
            pr.state.num_ind_stages = 1;
            pr.state.ind[0].texmap = 0;
            pr.state.ind[0].texcoord = 0;
            DrawCall d4 = d;
            d4.tev_stages[0].ind_mtx = 1;
            d4.tev_stages[0].ind_stage = 0;
            d4.tev_stages[0].ind_wrap_s = IndWrap128;
            if (!draw_with_program(pr, d4, nullptr, idx, 3, pv)) {
                std::printf("FAIL: an indirect material did not go through the program path (plast=%d)\n",
                            g_dbg.proglast);
                ++g_failures;
            }
            if (g_gl.uniform_lookups_failed != lookups_failed_before) {
                std::printf("FAIL: the indirect uniforms looked up are not the names the emitter wrote\n");
                ++g_failures;
            }
            pr.state.num_ind_stages = 0;
        }
    }


    // ---- EFB -> texture copies --------------------------------------------
    {
        // 1. The escape the native writes round-trips through the decoder with its fields
        //    and its position in the stream. Uses the real writer, the one the native calls.
        static Fifo f;
        f.reset();
        static Processor pr2;
        pr2.reset();
        static std::uint8_t guest_dest[16 * 16 * 4];
        TexCopySetup setup;
        setup.src_x = 8;
        setup.src_y = 12;
        setup.src_w = 4;
        setup.src_h = 4;
        setup.dst_w = 4;
        setup.dst_h = 4;
        setup.format = TexRGBA8;
        check(write_efb_copy_escape(f, setup, true, 0x80ABC000u, guest_dest), "escape must write");
        f.load_bp(0x4F, (0x7Fu << 8) | 0x11u);  // copy clear A=0x7F R=0x11
        f.load_bp(0x50, (0x22u << 8) | 0x33u);  // G=0x22 B=0x33
        check(write_efb_copy_escape(f, setup, false, 0x80ABC000u, guest_dest), "second escape must write");
        check(pr2.process(f.data(), f.size(), true), "a stream carrying copy escapes must decode");
        const EfbCopyEvent* ev = nullptr;
        const unsigned n_ev = efb_copy_events(&ev);
        if (n_ev != 2) {
            std::printf("FAIL: expected 2 copy events, got %u\n", n_ev);
            ++g_failures;
        } else {
            check(ev[0].src_x == 8 && ev[0].src_y == 12 && ev[0].src_w == 4 && ev[0].src_h == 4,
                  "copy source rect round-trip");
            check(ev[0].dst_w == 4 && ev[0].dst_h == 4 && ev[0].format == TexRGBA8, "copy destination round-trip");
            check(ev[0].clear && !ev[1].clear, "copy clear flag round-trip");
            check(ev[0].dest_guest == 0x80ABC000u && ev[0].dest_host == guest_dest, "copy destination address");
            check(ev[0].draw_index == 0, "a copy before any draw observes no draws");
            // The copy clear colour is sampled when the copy is recorded, not later.
            check(ev[1].clear_rgba == (0x11u | (0x22u << 8) | (0x33u << 16) | (0x7Fu << 24)),
                  "copy clear colour from BP 0x4F/0x50");
        }

        // 2. Performing a copy: GL reads bottom-up, GX textures are top-down, so the top
        //    texel row must hold the framebuffer's *highest* GL row.
        g_fb_scale = 1.0f;
        g_fb_base_x = 0;
        g_fb_base_y = 0;
        g_fb_dh = 16;
        EfbCopyEvent e;
        e.src_x = 0;
        e.src_y = 0;
        e.src_w = 4;
        e.src_h = 4;
        e.dst_w = 4;
        e.dst_h = 4;
        e.format = TexRGBA8;
        e.dest_host = guest_dest;
        const int uploads_before = g_copy.uploads;
        perform_efb_copy(e);
        check(g_copy.uploads == uploads_before + 1, "a copy must upload a texture");
        // EFB rows 0..3 from the top of a 16-high frame are GL rows 12..15. vitaGL puts a
        // readback at the rect's own offset inside a rect-sized buffer, so the read starts
        // at the origin and covers them: 4 wide, 16 high.
        check(g_copy.read_x == 0 && g_copy.read_y == 0 && g_copy.read_h == 16 && g_copy.read_w == 4,
              "the readback must start at the origin and reach the EFB rect");
        check(g_copy.up_w == 4 && g_copy.up_h == 4, "destination size");
        // top texel row <- GL row 15 (value 150); bottom texel row <- GL row 12 (120).
        check(g_copy.up_px[0] == 150, "top texel row must come from the top of the framebuffer rect");
        check(g_copy.up_px[(3 * 4) * 4] == 120, "bottom texel row must come from the bottom");
        check(g_copy.up_px[1] == 0 && g_copy.up_px[3 * 4 + 1] == 30, "columns stay left-to-right");

        // 3. A later texobj pointing at the destination samples the captured copy.
        TexObj t;
        t.data = guest_dest;
        t.width = 4;
        t.height = 4;
        t.format = TexRGBA8;
        const std::uint32_t hits_before = g_dbg.efbhit;
        const GLuint id = upload_texture(t, nullptr);
        check(id != 0 && efb_texture_for(t) != nullptr && id == efb_texture_for(t)->id,
              "upload_texture must return the captured copy");
        check(g_dbg.efbhit == hits_before + 1, "the efb hit counter must move");

        // 4. A half-size (mipmapped) intensity copy: box filter plus luma, alpha = luma for I8.
        EfbCopyEvent h = e;
        h.dst_w = 2;
        h.dst_h = 2;
        h.format = 1;  // GX_TF_I8
        perform_efb_copy(h);
        check(g_copy.up_w == 2 && g_copy.up_h == 2, "half-size destination");
        const std::uint8_t* p0 = g_copy.up_px;
        check(p0[0] == p0[1] && p0[1] == p0[2] && p0[2] == p0[3], "I8 copy must be pure intensity, alpha included");

        // 5. The clear flag clears the copied rect to the copy clear colour afterwards.
        //    Channels follow the copy's masks: depth through glClear, colour/alpha through a
        //    masked full-rect draw (vitaGL's glClear ignores the colour mask).
        EfbCopyEvent c = e;
        c.clear = true;
        c.clear_color = true;
        c.clear_alpha = true;
        c.clear_depth = true;
        c.clear_rgba = 0xFF000080u;  // r = 0x80, a = 0xFF
        const int clears_before = g_copy.clears;
        const int draws_before_clear = g_gl.draws;
        perform_efb_copy(c);
        check(g_copy.clears == clears_before + 1, "GXCopyTex(..., GX_TRUE) must clear depth");
        check(g_gl.draws == draws_before_clear + 1, "the colour/alpha clear is one masked draw");
        // vitaGL treats GL_VERTEX_SHADER/GL_FRAGMENT_SHADER source as GLSL and translates it:
        // Cg given that way never compiles on the Vita (the clear was silently skipped).
        check(g_gl.glsl_shaders == 0, "every renderer shader is created as Cg (GL_CG_*_SHADER_EXT)");
        check(std::strstr(g_gl.vsrc, "vPosition") != nullptr, "masked clear vertex program is the Cg one");
    }


    // ---- display lists and indexed matrix loads through the guest resolver --
    // Before the resolver, Processor::resolve only knew map_range, which nothing calls,
    // so every CALL_DL and every indexed XF load was dropped.
    {
        set_guest_resolver(&fake_resolver);

        // A display list that sets GX_TEVREG1 (BP 0xE4/0xE5). Written with a scratch Fifo so
        // its bytes are exactly what the guest would have built with GXBeginDisplayList.
        static Fifo dl;
        dl.reset();
        dl.set_tev_color(2, 0x12, 0x34, 0x56, 0x78);
        check(dl.size() <= sizeof(g_fake_guest) / 2, "display list fits the fake guest buffer");
        std::memcpy(g_fake_guest, dl.data(), dl.size());

        static Fifo f;
        f.reset();
        static Processor pr3;
        pr3.reset();
        f.call_dl(kFakeGuestBase, dl.size());
        check(pr3.process(f.data(), f.size(), true), "a stream calling a display list must decode");
        check(pr3.skipped_dl == 0, "the display list must not be skipped");
        check(pr3.state.tev_color[2] == pack_rgba(0x12, 0x34, 0x56, 0x78),
              "the display list's register write must take effect");

        // An indexed position-matrix load: GXSetArray points CP 0xAC at the array (as a
        // physical address) and 0xBC holds the stride; LOAD_INDX_A copies 12 words to XF.
        const std::uint32_t arr_off = 2048;
        for (unsigned i = 0; i < 12u; ++i) {
            const float v = static_cast<float>(i) + 0.5f;
            std::uint32_t bits;
            std::memcpy(&bits, &v, 4);
            std::uint8_t* w = g_fake_guest + arr_off + 48u + i * 4u;  // element index 1, stride 48
            w[0] = static_cast<std::uint8_t>(bits >> 24);
            w[1] = static_cast<std::uint8_t>(bits >> 16);
            w[2] = static_cast<std::uint8_t>(bits >> 8);
            w[3] = static_cast<std::uint8_t>(bits);
        }
        f.reset();
        pr3.reset();
        f.load_cp(0xAC, (kFakeGuestBase + arr_off) & 0x7FFFFFFFu);  // physical, as GXSetArray writes it
        f.load_cp(0xBC, 48);
        f.load_indx(kLoadIndxA, 1, 12, 0);  // element 1 -> XF 0 (PNMTX0)
        check(pr3.process(f.data(), f.size(), true), "indexed load stream must decode");
        check(pr3.skipped_indexed_xf == 0, "the indexed matrix load must not be skipped");
        check(pr3.state.xf_mem[0] == 0.5f && pr3.state.xf_mem[11] == 11.5f,
              "the indexed matrix must land in XF memory, big-endian decoded");

        set_guest_resolver(nullptr);
        check(g_resolver_calls >= 2, "both paths must have gone through the resolver");
    }


    // ---- which buffer a copy reads, and copy-only drains ------------------
    // Every drain here is cleared, submitted and swapped, so a copy recorded before any
    // draw of its drain is looking at what the previous drain presented: the front buffer.
    {
        static std::uint8_t dest2[64];
        EfbCopyEvent first;
        first.src_w = 4;
        first.src_h = 4;
        first.dst_w = 4;
        first.dst_h = 4;
        first.format = TexRGBA8;
        first.dest_host = dest2;
        first.draw_index = 0;
        perform_efb_copy(first);
        check(g_copy.read_buffer_at_read == GL_FRONT, "a copy before any draw of its drain must read the front buffer");
        check(g_copy.read_buffer == GL_BACK, "the read buffer must be restored to GL_BACK afterwards");

        EfbCopyEvent later = first;
        later.draw_index = 3;
        perform_efb_copy(later);
        check(g_copy.read_buffer_at_read == GL_BACK, "a copy after this drain's draws must read the back buffer");

        // A drain with a copy and no draws: performed, nothing drawn or cleared.
        static Fifo f;
        f.reset();
        static Processor pr4;
        pr4.reset();
        TexCopySetup setup;
        setup.src_w = 4;
        setup.src_h = 4;
        setup.dst_w = 4;
        setup.dst_h = 4;
        setup.format = TexRGBA8;
        write_efb_copy_escape(f, setup, false, 0x80500000u, dest2);
        check(pr4.process(f.data(), f.size(), true), "copy-only stream decodes");
        check(pr4.draw_count == 0, "the stream has no draws");
        const int reads_before = g_copy.reads;
        const int draws_before = g_gl.draws;
        const int clears_before = g_copy.clears;
        submit_efb_copies_only(pr4);
        check(g_copy.reads == reads_before + 1, "the orphan copy must be performed");
        check(g_copy.read_buffer_at_read == GL_FRONT, "the orphan copy must read the front buffer");
        check(g_gl.draws == draws_before && g_copy.clears == clears_before, "a copy-only drain must not draw or clear");
        TexObj orphan;
        orphan.data = dest2;
        orphan.width = 4;
        orphan.height = 4;
        orphan.format = TexRGBA8;
        check(efb_texture_for(orphan) != nullptr, "the orphan copy must be sampleable afterwards");
    }


    // ---- draw merging -------------------------------------------------------
    // Consecutive draws that differ only in their vertices go out as one glDrawElements; a
    // state change (here a different texture) starts a new one.
    {
        static Fifo f;
        f.reset();
        static Processor pm;
        pm.reset();
        static std::uint8_t tex_a[4 * 4 * 4];
        static std::uint8_t tex_b[4 * 4 * 4];
        f.set_gen_mode(1, 1, 1);
        f.set_tev_op(0, TevModulate);
        f.set_tev_order(0, 0, 0, kRasColor0);
        f.set_tex_coord_gen(0, TgMtx2x4, TgTex0, kTexMtxIdentity);
        configure_pos_clr0_tex0_direct(f, VtxFmt0);
        std::uint8_t vtx[4 * 24]{};
        f.set_viewport(0.0f, 0.0f, 608.0f, 456.0f, 0.0f, 1.0f);
        f.load_texobj(0, tex_a, 4, 4, TexRGBA8);
        f.draw(Triangles, VtxFmt0, 3, vtx, 3 * 24);
        f.draw(TriangleStrip, VtxFmt0, 4, vtx, 4 * 24);
        f.draw(Triangles, VtxFmt0, 3, vtx, 3 * 24);
        f.load_texobj(0, tex_b, 4, 4, TexRGBA8);
        f.draw(Triangles, VtxFmt0, 3, vtx, 3 * 24);
        check(pm.process(f.data(), f.size(), true), "merge stream decodes");
        check(pm.draw_count == 4, "four draws decoded");
        const int before = g_gl.draws;
        submit_processor(pm);
        end_submit_frame();
        check(g_gl.draws == before + 2, "three same-state draws merge into one GL draw, the fourth is separate");
        set_draw_merging(false);
        const int before_off = g_gl.draws;
        submit_processor(pm);
        end_submit_frame();
        set_draw_merging(true);
        check(g_gl.draws == before_off + 4, "with merging off every draw is its own GL draw");
    }

    // ---- every draw keeps its own texture and raster state ----------------
    // The renderer used to read textures, viewport and depth from processor.state - the
    // state after the whole frame decoded - so every draw sampled the frame's last texture.
    {
        static Fifo f;
        f.reset();
        static Processor pr5;
        pr5.reset();
        static std::uint8_t tex_a[4 * 4 * 4];
        static std::uint8_t tex_b[4 * 4 * 4];
        f.set_gen_mode(1, 1, 1);
        f.set_tev_op(0, TevModulate);
        f.set_tev_order(0, 0, 0, kRasColor0);
        f.set_tex_coord_gen(0, TgMtx2x4, TgTex0, kTexMtxIdentity);
        configure_pos_clr0_tex0_direct(f, VtxFmt0);
        // One vertex: pos f32x3, rgba8, st f32x2 = 24 bytes.
        std::uint8_t vtx[3 * 24]{};
        f.set_viewport(0.0f, 0.0f, 608.0f, 456.0f, 0.0f, 1.0f);
        f.load_texobj(0, tex_a, 4, 4, TexRGBA8);
        f.draw(Triangles, VtxFmt0, 3, vtx, sizeof(vtx));
        f.set_viewport(100.0f, 50.0f, 200.0f, 100.0f, 0.0f, 1.0f);  // a sub-viewport
        f.load_texobj(0, tex_b, 4, 4, TexRGBA8);
        f.draw(Triangles, VtxFmt0, 3, vtx, sizeof(vtx));
        check(pr5.process(f.data(), f.size(), true), "two-draw stream decodes");
        check(pr5.draw_count == 2, "two draws decoded");
        const DrawExtra* x0 = draw_extra(pr5, 0);
        const DrawExtra* x1 = draw_extra(pr5, 1);
        check(x0 != nullptr && x1 != nullptr, "both draws must have a snapshot");
        if (x0 != nullptr && x1 != nullptr) {
            check(x0->tex[0].data == tex_a, "draw 0 must keep the texture it was drawn with");
            check(x1->tex[0].data == tex_b, "draw 1 must see its own texture");
            check(pr5.state.texmaps[0].data == tex_b, "(end-of-frame state holds only the last one)");
            check(x0->render_vp[2] == 608.0f && x1->render_vp[2] == 200.0f, "each draw keeps its viewport");

            // A sub-viewport lands inside the letterbox, at its own size.
            set_frame_mapping(608.0f, 456.0f);
            int gx = 0, gy = 0, gw = 0, gh = 0;
            efb_to_gl(x1->render_vp[0], x1->render_vp[1], x1->render_vp[2], x1->render_vp[3], &gx, &gy, &gw, &gh);
            const float sc = g_fb_scale;
            check(gw == static_cast<int>(200.0f * sc + 0.5f) && gh == static_cast<int>(100.0f * sc + 0.5f),
                  "a sub-viewport must keep its size, not be scaled to fill the screen");
            // GX y is top-down: the rect's GL bottom is (456 - 50 - 100) EFB rows above the base.
            check(gy == g_fb_base_y + g_fb_dh - static_cast<int>(150.0f * sc + 0.5f),
                  "a sub-viewport must be placed with GX's top-left origin");
        }
        static Processor other;
        other.reset();
        check(draw_extra(other, 0) == nullptr, "a snapshot belongs to the Processor that filled it");
    }

    {
        // CMPR used to convert to DXT1 and then be refused by upload_texture (only RGBA8
        // uploads existed), so every CMPR texture drew unbound. It now decodes to RGBA8.
        static std::uint8_t cmpr[32];
        encode_cmpr_solid(cmpr, 8, 8, pack_rgb565(255, 0, 0));
        TexObj t;
        t.data = cmpr;
        t.width = 8;
        t.height = 8;
        t.format = TexCMPR;
        const int uploads_before = g_copy.uploads;
        const GLuint id = upload_texture(t, nullptr);
        check(id != 0, "a CMPR texture must upload");
        check(g_copy.uploads == uploads_before + 1 && g_copy.up_w == 8 && g_copy.up_h == 8, "CMPR upload size");
        check(g_copy.up_px[0] == 255 && g_copy.up_px[1] == 0 && g_copy.up_px[2] == 0 && g_copy.up_px[3] == 255,
              "CMPR decodes to RGBA8");
        // The same bytes read as another format are a different texture, not a cache hit.
        TexObj t3 = t;
        t3.width = 8;
        t3.height = 8;
        t3.format = TexI4;
        const GLuint id3 = upload_texture(t3, nullptr);
        check(id3 != 0 && id3 != id, "texture cache must key on format");
    }
    // ---- the EFB render target -------------------------------------------
    // Everything above ran without it (the back-buffer fallback). A failed FBO keeps that.
    {
        g_fbo.status = 0;  // incomplete
        check(!efb_target_init() && !efb_target_active(), "an incomplete FBO must leave the back-buffer path");
        check(g_fbo.draw_fb == 0, "a failed EFB target must not stay bound");
        check(!present_efb(), "no target, no present");

        g_fbo.status = GL_FRAMEBUFFER_COMPLETE;
        check(efb_target_init() && efb_target_active(), "a complete FBO becomes the EFB target");
        const GLuint target = g_fbo.next_id - 1;  // the framebuffer is generated last
        check(g_fbo.draw_fb == 0, "init leaves the default framebuffer bound");

        // The mapping is fixed: a 456-row EFB is 544 target rows, whatever the frame's extent.
        set_frame_mapping(608.0f, 528.0f);
        int gx = 0, gy = 0, gw = 0, gh = 0;
        efb_to_gl(0.0f, 0.0f, 608.0f, 456.0f, &gx, &gy, &gw, &gh);
        check(gx == 0 && gw == 725 && gh == 544 && gy == kEfbTargetH - 544,
              "EFB rows map to the target at 544/456 with the EFB's top at the target's top");

        // A frame: bound, and the clear waits for the leading copies.
        const int clears_before = g_copy.clears;
        begin_efb_frame(0x00000000u);
        check(g_fbo.draw_fb == target && g_fbo.read_fb == target, "a frame draws into the EFB target");
        check(g_copy.clears == clears_before, "the clear is deferred past the frame's leading copies");

        // A copy reads the target, never the front buffer, even before any draw.
        static std::uint8_t dest3[64];
        EfbCopyEvent lead;
        lead.src_x = 0;
        lead.src_y = 500;  // a LightTexture scratch rect below the displayed area
        lead.src_w = 4;
        lead.src_h = 4;
        lead.dst_w = 4;
        lead.dst_h = 4;
        lead.format = TexRGBA8;
        lead.dest_host = dest3;
        lead.draw_index = 0;
        g_copy.read_buffer = GL_BACK;
        const int reads_before_gpu = g_copy.reads;
        const int blits_before = g_fbo.blits;
        perform_efb_copy(lead);
        check(g_copy.reads == reads_before_gpu, "with the EFB target an RGBA8 copy never reads back to the CPU");
        check(g_fbo.blits == blits_before + 1, "an RGBA8 copy is a GPU blit from the EFB target");
        // Other formats are converted by a draw on the GPU, also without a readback.
        EfbCopyEvent i8 = lead;
        i8.format = 0x1u;  // I8
        const int draws_before_i8 = g_gl.draws;
        perform_efb_copy(i8);
        check(g_copy.reads == reads_before_gpu, "an I8 copy never reads back to the CPU either");
        check(g_gl.draws == draws_before_i8 + 1, "an I8 copy is one conversion draw");
        // static51: the EFB is bound again lazily, before the next draw, not after each copy.
        check(g_fbo.draw_fb == target || !g_efb_draw_bound, "the EFB target is bound again after a copy (or before the next draw)");
        // Race depth snapshots must read the depth surface, not scene RGB.
        const unsigned depth_before = g_depth_aliases;
        EfbCopyEvent zcopy = lead;
        zcopy.format = 0x16u;
        const int draws_before_z = g_gl.draws;
        perform_efb_copy(zcopy);
        check(g_depth_aliases == depth_before + 1, "Z24X8 aliases the EFB depth surface");
        check(g_gl.draws == draws_before_z + 1 && g_copy.reads == reads_before_gpu,
              "depth bytes are encoded by a GPU draw without CPU readback");
        check((g_fbo.draw_fb == target && g_fbo.read_fb == target) || !g_efb_draw_bound,
              "depth copy restores EFB bindings (or leaves them to the next draw)");
        perform_efb_copy(i8);
        check(g_depth_aliases == depth_before + 1, "colour copies do not alias the depth surface");
        const CopyConvert high = copy_convert_for(0x11u), middle = copy_convert_for(0x39u);
        const CopyConvert low = copy_convert_for(0x3Au), z24 = copy_convert_for(0x16u);
        for (unsigned row = 0; row < 4; ++row) {
            check(high.m[row*4] == 1 && middle.m[row*4+1] == 1 && low.m[row*4+2] == 1,
                  "depth byte selection matches high/middle/low I8 storage");
            check(z24.m[row*4+row] == 1, "Z24X8 keeps RGB depth bytes and opaque alpha");
        }
        // With effects cut, a post-processing copy is skipped entirely (no draw, no read).
        set_effects_level(1);
        const int draws_before_skip = g_gl.draws;
        perform_efb_copy(i8);
        check(g_gl.draws == draws_before_skip && g_copy.reads == reads_before_gpu, "fx_low skips an I8 copy");
        set_effects_level(2);

        // Present: the display copy's source rect, fitted to the screen, from the target.
        set_display_copy_source(0, 0, 608, 456);
        const int blits_before_present = g_fbo.blits;
        check(present_efb(), "present with the target");
        check(g_copy.clears > clears_before, "an unsubmitted frame is still cleared before present");
        check(g_fbo.blits == blits_before_present + 1 && g_fbo.blit_read_fb == target && g_fbo.blit_draw_fb == 0,
              "present blits the target to the default framebuffer");
        check(g_fbo.src[0] == 0 && g_fbo.src[1] == 0 && g_fbo.src[2] == 725 && g_fbo.src[3] == 544,
              "present reads exactly the display copy rect (top-left origin), not the scratch rows");
        // static55: widescreen, the display copy is stretched over the whole panel.
        check(g_fbo.dst[0] == 0 && g_fbo.dst[1] == 0 && g_fbo.dst[2] == 960 && g_fbo.dst[3] == 544,
              "the displayed rect fills the panel");
    }

    {
        const auto dir = std::filesystem::temp_directory_path() /
            ("mkwv-gpu-readback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(std::filesystem::create_directory(dir), "create temporary capture directory");
        static Processor empty;
        empty.reset();
        capture_arm(dir.c_str());
        capture_batch(empty);
        const int before = g_copy.reads;
        capture_gpu_checkpoint("test");
        check(g_copy.reads == before + 1 && g_copy.read_format == GL_RGBA && g_mapped_reads == 1 && g_finishes == 1,
              "GPU checkpoint finishes the scene then uses a mapped same-format transfer");
        capture_frame_end();
        const auto image = std::filesystem::path(dir) / "gpu_b0_test.ppm";
        check(std::filesystem::file_size(image) > static_cast<unsigned>(g_rt_w * g_rt_h * 3),
              "GPU checkpoint writes a full RGB image with a PPM header");
        std::filesystem::remove_all(dir);
    }

    {
        const auto dir = std::filesystem::temp_directory_path() /
            ("capture_auto_race-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        static Processor empty;
        empty.reset();
        // Off by default: the race capture keeps no probes and the game stays on screen.
        capture_arm(dir.c_str());
        capture_batch(empty);
        capture_gpu_checkpoint("batch_end");
        check(g_race_probe_count == 0, "race contact sheet is off without the race_sheet marker");
        capture_frame_end();
        set_race_probe_sheet(true);
        capture_arm(dir.c_str());
        const int reads = g_copy.reads;
        const int blits = g_fbo.blits;
        for (unsigned i = 0; i < 7; ++i) {
            capture_batch(empty);
            capture_gpu_checkpoint("before_d0");
            capture_gpu_checkpoint("batch_end");
        }
        check(g_copy.reads == reads && g_race_probe_count == 7 && g_fbo.blits == blits + 7,
              "race checkpoints stay on the GPU, one texture per batch");
        check(g_fbo.draw_fb == g_efb_fbo, "race probe restores EFB binding");
        capture_frame_end();
        const int before_present = g_fbo.blits;
        present_efb();
        check(g_fbo.blits == before_present + 9, "sheet presents seven checkpoints and live output");
        check(g_copy.reads == reads, "contact sheet never reads guest framebuffer RAM");
        g_race_probe_count = 0;
        set_race_probe_sheet(false);
        std::filesystem::remove_all(dir);
    }

    // ---- GPU transform (static70): DL cache hits drawn from the cache's GPU copy ----
    {
        static Fifo dl;
        dl.reset();
        auto strip = [](Fifo& fifo, float x0) {
            std::uint8_t vtx[4 * 24];
            for (unsigned v = 0; v < 4; ++v) {
                const float p[3] = {x0 + static_cast<float>(v & 1), static_cast<float>(v >> 1), -2.0f};
                const float t[2] = {0.5f * static_cast<float>(v & 1), 0.5f * static_cast<float>(v >> 1)};
                std::uint8_t* o = vtx + v * 24;
                for (unsigned k = 0; k < 3; ++k) {
                    std::uint32_t bits;
                    std::memcpy(&bits, &p[k], 4);
                    o[k * 4 + 0] = static_cast<std::uint8_t>(bits >> 24);
                    o[k * 4 + 1] = static_cast<std::uint8_t>(bits >> 16);
                    o[k * 4 + 2] = static_cast<std::uint8_t>(bits >> 8);
                    o[k * 4 + 3] = static_cast<std::uint8_t>(bits);
                }
                o[12] = 0xFF; o[13] = 0x80; o[14] = 0x40; o[15] = 0xFF;
                for (unsigned k = 0; k < 2; ++k) {
                    std::uint32_t bits;
                    std::memcpy(&bits, &t[k], 4);
                    o[16 + k * 4 + 0] = static_cast<std::uint8_t>(bits >> 24);
                    o[16 + k * 4 + 1] = static_cast<std::uint8_t>(bits >> 16);
                    o[16 + k * 4 + 2] = static_cast<std::uint8_t>(bits >> 8);
                    o[16 + k * 4 + 3] = static_cast<std::uint8_t>(bits);
                }
            }
            fifo.draw(TriangleStrip, VtxFmt0, 4, vtx, sizeof(vtx));
        };
        strip(dl, 0.0f);
        strip(dl, 2.0f);
        strip(dl, 4.0f);
        const float m[12] = {1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, -1.0f};
        dl.load_xf_f32(0, m, 12);  // a new matrix: a second epoch
        strip(dl, 6.0f);
        constexpr std::uint32_t kXfDl = 0x80600000u;

        static Fifo f;
        f.reset();
        static std::uint8_t tex[4 * 4 * 4];
        f.set_gen_mode(1, 1, 1);
        f.set_tev_op(0, TevModulate);
        f.set_tev_order(0, 0, 0, kRasColor0);
        f.set_tex_coord_gen(0, TgMtx2x4, TgTex0, kTexMtxIdentity);
        configure_pos_clr0_tex0_direct(f, VtxFmt0);
        f.set_viewport(0.0f, 0.0f, 608.0f, 456.0f, 0.0f, 1.0f);
        f.load_texobj(0, tex, 4, 4, TexRGBA8);
        f.set_chan_ctrl(0, false, SrcReg, SrcVtx, 0x00, DfClamp, AfNone);
        f.call_dl(kXfDl, dl.size());

        static std::vector<std::uint8_t> pool(6u * 1024u * 1024u);
        static std::vector<std::uint8_t> gpu(6u * 1024u * 1024u);
        set_gpu_lighting(true);
        set_gpu_xf(true);
        dl_cache_install(pool.data(), static_cast<std::uint32_t>(pool.size()));
        dl_cache_install_gpu(gpu.data(), nullptr);
        set_gpu_xf_buffer(7, gpu.data(), static_cast<std::uint32_t>(gpu.size()));
        static Processor px;
        unsigned xf_draws[2] = {0, 0};
        int gl_draws[2] = {0, 0};
        for (int frame = 0; frame < 2; ++frame) {
            px.reset();
            px.map_range(kXfDl, dl.data(), dl.size());
            check(px.process(f.data(), f.size(), true), "transform stream decodes");
            vertex_worker_drain();
            for (unsigned d = 0; d < px.draw_count; ++d) {
                GpuXfDraw xd{};
                xf_draws[frame] += draw_gpu_xf(px, d, &xd) ? 1u : 0u;
            }
            const int before = g_gl.draws;
            g_vbo_ptr = nullptr;
            submit_processor(px);
            end_submit_frame();
            gl_draws[frame] = g_gl.draws - before;
            if (frame == 1) {
                const auto* vp = static_cast<const std::uint8_t*>(g_vbo_ptr);
                check(vp != nullptr && vp >= gpu.data() && vp < gpu.data() + gpu.size(),
                      "transform draws point the VBO into the GPU copy");
                check(std::strstr(g_gl.vsrc, "uPn[") != nullptr, "the transform vertex program was built");
            }
        }
        check(px.draw_count == 4, "four DL primitives decoded");
        check(xf_draws[0] == 0, "the filling frame draws through the vertex jobs");
        check(xf_draws[1] == 4, "every hit draws with the GPU transform");
        check(gl_draws[1] == 2, "one GL draw per epoch: three merged strips, then the one after the matrix load");
        std::printf("gpu transform submit: %d GL draws (fill frame %d)\n", gl_draws[1], gl_draws[0]);
        set_gpu_xf_buffer(0, nullptr, 0);
        dl_cache_install(nullptr, 0);
        set_gpu_xf(false);
        set_gpu_lighting(false);
    }

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    // A silently skipped test is worse than no test, so say what the cache actually did.
    if (g_copy.uploads < 3 || g_copy.reads < 3 || g_copy.clears < 1) {
        std::printf("FAIL: the EFB-copy cases did not run (reads=%d uploads=%d clears=%d)\n", g_copy.reads,
                    g_copy.uploads, g_copy.clears);
        return 1;
    }
    if (g_gl.programs_created < 3 || g_gl.draws < 4) {
        std::printf("FAIL: the program-cache case did not run (programs=%d draws=%d)\n",
                    g_gl.programs_created, g_gl.draws);
        return 1;
    }
    std::printf("gx_tev_combine_tests: all cases pass (%d programs built, %d compiles, %d draws issued, "
                "%d EFB copies)\n",
                g_gl.programs_created, g_gl.compiles, g_gl.draws, g_copy.uploads);
    return 0;
}
