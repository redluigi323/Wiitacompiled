// End-to-end check of the GX HLE layer against the FIFO decoder: guest-style GX calls go
// through the HLE natives (which emit the SDK's raw BP/CP/XF register writes), the FIFO is
// drained into a Processor, and the decoded state must describe the same draw the calls
// asked for. This is the register contract between vita/src/runtime/hle_gx.cpp and
// vita/src/gx/fifo.cpp - either side drifting shows up here, not on hardware.
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/fifo.hpp"
#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/runtime.hpp"
#include "memory.h"

#include <cstdio>
#include <cstring>

namespace {

using mkwv::runtime::CpuContext;
using mkwv::runtime::invoke_guest;

int fail(const char* message) {
    std::fprintf(stderr, "gx_hle_pipeline: %s\n", message);
    return 1;
}

// Guest addresses for this test's objects, well clear of the low globals.
constexpr std::uint32_t kTexObj = 0x80600000u;
constexpr std::uint32_t kTexObjCI = 0x80600040u;
constexpr std::uint32_t kTlutObj = 0x80600080u;
constexpr std::uint32_t kColorArg = 0x806000C0u;
constexpr std::uint32_t kStack = 0x80600F00u;
constexpr std::uint32_t kImage = 0x80610000u;    // 8x8 RGBA8
constexpr std::uint32_t kImageCI = 0x80611000u;  // 8x8 CI8
constexpr std::uint32_t kPalette = 0x80612000u;  // 256 RGB565 entries

bool call(CpuContext& cpu, std::uint32_t addr, std::initializer_list<std::uint32_t> args) {
    unsigned r = 3;
    for (std::uint32_t a : args) {
        cpu.gpr[r++] = a;
    }
    return invoke_guest(addr, &cpu);
}

void fifo_u8(std::uint8_t v) { Memory::Write8(0xCC008000u, v); }
void fifo_u32(std::uint32_t v) { Memory::Write32(0xCC008000u, v); }
void fifo_f32(float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    fifo_u32(bits);
}

// SDK enum values (GXEnum.h).
constexpr std::uint32_t GX_VA_POS = 9, GX_VA_CLR0 = 11, GX_VA_TEX0 = 13;
constexpr std::uint32_t GX_DIRECT = 1;
constexpr std::uint32_t GX_POS_XYZ = 1, GX_CLR_RGBA = 1, GX_TEX_ST = 1;
constexpr std::uint32_t GX_F32 = 4, GX_RGBA8 = 5;
constexpr std::uint32_t GX_TF_RGBA8 = 6, GX_TF_C8 = 9, GX_TL_RGB565 = 1;
constexpr std::uint32_t GX_CLAMP = 0, GX_REPEAT = 1;
constexpr std::uint32_t GX_TG_MTX2x4 = 1, GX_TG_TEX0 = 4, GX_IDENTITY = 60, GX_PTIDENTITY = 125;
constexpr std::uint32_t GX_TEXCOORD0 = 0, GX_TEXMAP0 = 0, GX_TEXMAP1 = 1, GX_COLOR0A0 = 4;
constexpr std::uint32_t GX_TEVSTAGE0 = 0, GX_MODULATE = 0;
constexpr std::uint32_t GX_QUADS = 0x80, GX_VTXFMT0 = 0;
constexpr std::uint32_t GX_TLUT0 = 0;

// Addresses of the natives (hle_native.cpp g_natives).
constexpr std::uint32_t kGXInit = 0x8016B850u;
constexpr std::uint32_t kClearVtxDesc = 0x8016DC34u;
constexpr std::uint32_t kSetVtxDesc = 0x8016D3A4u;
constexpr std::uint32_t kSetVtxAttrFmt = 0x8016DC68u;
constexpr std::uint32_t kSetNumTexGens = 0x8016E5A4u;
constexpr std::uint32_t kSetTexCoordGen2 = 0x8016E37Cu;
constexpr std::uint32_t kSetNumChans = 0x8017054Cu;
constexpr std::uint32_t kSetNumTevStages = 0x801722A8u;
constexpr std::uint32_t kSetTevOrder = 0x8017214Cu;
constexpr std::uint32_t kSetTevOp = 0x80171C4Cu;
constexpr std::uint32_t kSetTevColor = 0x80171E10u;
constexpr std::uint32_t kInitTexObj = 0x801707F8u;
constexpr std::uint32_t kInitTexObjCI = 0x80170A04u;
constexpr std::uint32_t kLoadTexObj = 0x80170F2Cu;
constexpr std::uint32_t kInitTlutObj = 0x80170F80u;
constexpr std::uint32_t kLoadTlut = 0x80170FA8u;
constexpr std::uint32_t kBegin = 0x8016F0F0u;
constexpr std::uint32_t kSetProjection = 0x8017301Cu;
constexpr std::uint32_t kLoadPosMtxImm = 0x8017310Cu;
constexpr std::uint32_t kSetCurrentMtx = 0x80173214u;
constexpr std::uint32_t kSetViewport = 0x801733B4u;
constexpr std::uint32_t kBeginDisplayList = 0x80172E00u;
constexpr std::uint32_t kEndDisplayList = 0x80172EB4u;
constexpr std::uint32_t kCallDisplayList = 0x80172F64u;

constexpr std::uint32_t kProjMtx = 0x80601000u;   // Mtx44
constexpr std::uint32_t kPosMtx = 0x80601100u;    // Mtx
constexpr std::uint32_t kDlBuf = 0x80620000u;     // 32-byte aligned display list

void write_be_f32(std::uint32_t addr, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    Memory::Write32(addr, bits);
}

void set_f(CpuContext& cpu, unsigned reg, double v) { cpu.fpr[reg].d = v; }

} // namespace

int main() {
    using namespace mkwv::gx;
    mkwv::runtime::Runtime runtime;
    if (!runtime.initialize("/tmp/mkwv-hle-pipeline-overlay")) {
        return fail("runtime");
    }
    CpuContext& cpu = runtime.cpu();
    cpu.gpr[1] = kStack;

    // Texture data: an 8x8 RGBA8 image (AR/GB tiled; content only matters for the pointer)
    // and an 8x8 CI8 image with a 565 palette.
    for (std::uint32_t i = 0; i < 8u * 8u * 4u; ++i) {
        Memory::Write8(kImage + i, static_cast<std::uint8_t>(i));
        Memory::Write8(kImageCI + i, static_cast<std::uint8_t>(i & 0xFFu));
    }
    for (std::uint32_t i = 0; i < 256u; ++i) {
        Memory::Write16(kPalette + i * 2u, static_cast<std::uint16_t>(0xF800u));  // red
    }

    if (!call(cpu, kGXInit, {0x80700000u, 0x40000u})) {
        return fail("GXInit");
    }
    // GXInit's own register stream must decode cleanly into the same processor the
    // draw below uses, as it does on hardware.
    if (!runtime.fifo().drain(runtime.processor())) {
        return fail("the GXInit stream must decode");
    }
    runtime.fifo().reset();

    bool ok = true;
    ok &= call(cpu, kClearVtxDesc, {});
    ok &= call(cpu, kSetVtxDesc, {GX_VA_POS, GX_DIRECT});
    ok &= call(cpu, kSetVtxDesc, {GX_VA_CLR0, GX_DIRECT});
    ok &= call(cpu, kSetVtxDesc, {GX_VA_TEX0, GX_DIRECT});
    ok &= call(cpu, kSetVtxAttrFmt, {GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0});
    ok &= call(cpu, kSetVtxAttrFmt, {GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0});
    ok &= call(cpu, kSetVtxAttrFmt, {GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0});
    ok &= call(cpu, kSetNumChans, {1});
    ok &= call(cpu, kSetNumTexGens, {1});
    ok &= call(cpu, kSetTexCoordGen2, {GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, 0, GX_PTIDENTITY});
    ok &= call(cpu, kSetNumTevStages, {1});
    ok &= call(cpu, kSetTevOrder, {GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0});
    ok &= call(cpu, kSetTevOp, {GX_TEVSTAGE0, GX_MODULATE});
    // GXColor by value arrives as a pointer under CodeWarrior's EABI.
    Memory::Write32(kColorArg, 0x11223344u);
    ok &= call(cpu, kSetTevColor, {1, kColorArg});
    ok &= call(cpu, kInitTexObj, {kTexObj, kImage, 8, 8, GX_TF_RGBA8, GX_CLAMP, GX_REPEAT, 0});
    ok &= call(cpu, kLoadTexObj, {kTexObj, GX_TEXMAP0});

    // A paletted texture on map 1 through TLUT 0.
    ok &= call(cpu, kInitTlutObj, {kTlutObj, kPalette, GX_TL_RGB565, 256});
    ok &= call(cpu, kLoadTlut, {kTlutObj, GX_TLUT0});
    Memory::Write32(kStack + 8u, GX_TLUT0);  // 9th argument (tlut_name) on the stack
    ok &= call(cpu, kInitTexObjCI, {kTexObjCI, kImageCI, 8, 8, GX_TF_C8, GX_REPEAT, GX_REPEAT, 0});
    ok &= call(cpu, kLoadTexObj, {kTexObjCI, GX_TEXMAP1});
    if (!ok) {
        return fail("a GX native was not dispatched");
    }

    ok &= call(cpu, kBegin, {GX_QUADS, GX_VTXFMT0, 4});
    const float xy[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (unsigned i = 0; i < 4; ++i) {
        fifo_f32(xy[i][0]);
        fifo_f32(xy[i][1]);
        fifo_f32(-0.5f);
        fifo_u32(0xFF8040C0u);
        fifo_f32(xy[i][0] * 0.5f);
        fifo_f32(xy[i][1] * 0.25f);
    }
    if (!ok) {
        return fail("GXBegin");
    }

    Processor& proc = runtime.processor();
    if (!runtime.fifo().drain(proc)) {
        std::fprintf(stderr, "gx_hle_pipeline: fifo bytes=%u\n", runtime.fifo().size());
        return fail("the HLE stream must decode");
    }
    const State& st = proc.state;
    if (proc.draw_count != 1 || proc.vert_count != 4) {
        std::fprintf(stderr, "gx_hle_pipeline: draws=%u verts=%u\n", proc.draw_count, proc.vert_count);
        return fail("one quad of four vertices");
    }
    if (st.vtx_desc[AttrPos] != AttrDirect || st.vtx_desc[AttrClr0] != AttrDirect ||
        st.vtx_desc[AttrTex0] != AttrDirect || vertex_stride(st, VtxFmt0) != 24) {
        return fail("vertex descriptor / format");
    }
    if (st.num_tex_gens != 1 || st.num_chans != 1 || st.num_tev_stages != 1) {
        return fail("gen mode counts");
    }
    if (st.texgens[0].src != TgTex0 || st.texgens[0].type != TgMtx2x4) {
        return fail("texgen 0");
    }
    if (st.tev[0].texmap != 0 || st.tev[0].texcoord != 0 || !st.tev[0].tex_enabled ||
        st.tev[0].ras_chan != kRasColor0) {
        return fail("tev order");
    }
    if (st.tev[0].color_b != CcTexC || st.tev[0].color_c != CcRasC || st.tev[0].alpha_b != CaTexA ||
        st.tev[0].alpha_c != CaRasA) {
        return fail("GX_MODULATE combiner inputs");
    }
    // GX_TEVREG0 is register slot 1.
    if (st.tev_color[1] != pack_rgba(0x11, 0x22, 0x33, 0x44)) {
        std::fprintf(stderr, "gx_hle_pipeline: tevreg0=%08X\n", st.tev_color[1]);
        return fail("GXSetTevColor(GX_TEVREG0) via the colour pointer");
    }

    const TexObj& t0 = st.texmaps[0];
    if (t0.width != 8 || t0.height != 8 || t0.format != TexRGBA8 || t0.wrap_s != WrapClamp || t0.wrap_t != WrapRepeat) {
        std::fprintf(stderr, "gx_hle_pipeline: map0 %ux%u fmt=%u wrap=%u/%u\n", t0.width, t0.height, t0.format, t0.wrap_s,
                     t0.wrap_t);
        return fail("texmap 0 registers");
    }
    if (t0.data != Memory::GetPointer(kImage, 1)) {
        return fail("texmap 0 image address must resolve to the guest image");
    }
    const TexObj& t1 = st.texmaps[1];
    if (t1.width != 8 || t1.height != 8 || t1.format != TexCI8 || t1.data != Memory::GetPointer(kImageCI, 1)) {
        return fail("texmap 1 (CI8)");
    }
    if (t1.tlut_tmem == 0xFFFFu) {
        return fail("the CI texmap must carry its TLUT's TMEM address");
    }
    // The palette was copied into TMEM by the TLUT load; the draw's snapshot resolves the
    // CI map's palette from there.
    const DrawExtra* x = draw_extra(proc, 0);
    if (x == nullptr || !x->tlut_valid[1] || x->tlut[1].data == nullptr) {
        return fail("the CI texmap must resolve a palette");
    }
    if (x->tlut[1].format != TlutRgb565 || x->tlut[1].entries != 256) {
        std::fprintf(stderr, "gx_hle_pipeline: palette fmt=%u entries=%u\n", x->tlut[1].format, x->tlut[1].entries);
        return fail("palette format / size");
    }
    const std::uint8_t* p = x->tlut[1].data;
    if (p[0] != 0xF8 || p[1] != 0x00 || p[510] != 0xF8) {
        std::fprintf(stderr, "gx_hle_pipeline: palette[0]=%02X%02X\n", p[0], p[1]);
        return fail("palette bytes");
    }
    if (x->tex[0].data != Memory::GetPointer(kImage, 1)) {
        return fail("the draw snapshot carries texmap 0");
    }

    const DecodedVertex& v1 = proc.verts[1];
    if (v1.tc[0][0] != 0.5f || v1.tc[0][1] != 0.0f) {
        std::fprintf(stderr, "gx_hle_pipeline: v1 tc=%f,%f\n", v1.tc[0][0], v1.tc[0][1]);
        return fail("texcoords through the identity texgen");
    }
    if (proc.draws[0].tex0 == false) {
        return fail("the draw samples a texture");
    }

    // ---- transform: C_MTXOrtho(0, 480, 0, 640, 0, 1) + identity modelview, recorded into
    // a display list and replayed with GXCallDisplayList ---------------------------------
    {
        const float t = 0.0f, b = 480.0f, l = 0.0f, r = 640.0f, n = 0.0f, f = 1.0f;
        float proj[16]{};
        proj[0] = 2.0f / (r - l);
        proj[3] = -(r + l) / (r - l);
        proj[5] = 2.0f / (t - b);
        proj[7] = -(t + b) / (t - b);
        proj[10] = -1.0f / (f - n);
        proj[11] = -f / (f - n);
        proj[15] = 1.0f;
        for (unsigned i = 0; i < 16; ++i) {
            write_be_f32(kProjMtx + i * 4u, proj[i]);
        }
        const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (unsigned i = 0; i < 12; ++i) {
            write_be_f32(kPosMtx + i * 4u, ident[i]);
        }
        runtime.fifo().reset();
        proc.reset();
        ok = call(cpu, kSetProjection, {kProjMtx, 1});
        ok &= call(cpu, kLoadPosMtxImm, {kPosMtx, 0});
        ok &= call(cpu, kSetCurrentMtx, {0});
        set_f(cpu, 1, 0.0);
        set_f(cpu, 2, 0.0);
        set_f(cpu, 3, 640.0);
        set_f(cpu, 4, 480.0);
        set_f(cpu, 5, 0.0);
        set_f(cpu, 6, 1.0);
        ok &= call(cpu, kSetViewport, {});
        ok &= call(cpu, kSetNumTexGens, {0});
        ok &= call(cpu, kClearVtxDesc, {});
        ok &= call(cpu, kSetVtxDesc, {GX_VA_POS, GX_DIRECT});
        ok &= call(cpu, kSetVtxDesc, {GX_VA_CLR0, GX_DIRECT});
        ok &= call(cpu, kBeginDisplayList, {kDlBuf, 0x1000});
        ok &= call(cpu, kBegin, {GX_QUADS, GX_VTXFMT0, 4});
        const float px[4][2] = {{0, 0}, {640, 0}, {640, 480}, {320, 240}};
        for (unsigned i = 0; i < 4; ++i) {
            fifo_f32(px[i][0]);
            fifo_f32(px[i][1]);
            fifo_f32(0.0f);
            fifo_u32(0xFFFFFFFFu);
        }
        ok &= call(cpu, kEndDisplayList, {});
        const std::uint32_t dl_bytes = cpu.gpr[3];
        if (!ok || dl_bytes == 0 || (dl_bytes & 31u) != 0) {
            std::fprintf(stderr, "gx_hle_pipeline: dl bytes=%u\n", dl_bytes);
            return fail("display list recording");
        }
        ok &= call(cpu, kCallDisplayList, {kDlBuf, dl_bytes});
        if (!ok || !runtime.fifo().drain(proc)) {
            return fail("display list replay decode");
        }
        if (proc.draw_count != 1 || proc.vert_count != 4 || proc.skipped_dl != 0) {
            std::fprintf(stderr, "gx_hle_pipeline: dl draws=%u verts=%u skipped=%u\n", proc.draw_count, proc.vert_count,
                         proc.skipped_dl);
            return fail("the display list's quad must be decoded through CALL_DL");
        }
        auto near = [](float a, float e) { return a > e - 1.0e-4f && a < e + 1.0e-4f; };
        const DecodedVertex& a = proc.verts[0];
        const DecodedVertex& c = proc.verts[3];
        // Top-left -> (-1, +1); centre -> (0, 0). GX clip z for z=0 is -f/(f-n) = -1 (w=1),
        // and GL z' = 2z + w = -1.
        if (!near(a.x, -1.0f) || !near(a.y, 1.0f) || !near(a.w, 1.0f) || !near(c.x, 0.0f) || !near(c.y, 0.0f) ||
            !near(c.z, -1.0f)) {
            std::fprintf(stderr, "gx_hle_pipeline: v0=(%f %f %f %f) v3=(%f %f %f %f)\n", a.x, a.y, a.z, a.w, c.x, c.y,
                         c.z, c.w);
            return fail("orthographic transform through the HLE projection and matrix loads");
        }
        const State& s2 = proc.state;
        if (!s2.has_render_vp || !near(s2.render_vp[2], 640.0f) || !near(s2.render_vp[3], 480.0f)) {
            std::fprintf(stderr, "gx_hle_pipeline: vp=%f %f %f %f\n", s2.render_vp[0], s2.render_vp[1], s2.render_vp[2],
                         s2.render_vp[3]);
            return fail("GXSetViewport must reach the decoder");
        }
    }

    // Render offload: the guest thread only hands bytes over; the render side decodes them,
    // including a command split across a full-FIFO chunk boundary. Hooked synchronously here,
    // it must present exactly the draws the synchronous path would.
    {
        static unsigned presents = 0;
        static unsigned presented_draws = 0;
        static unsigned chunks = 0;
        runtime.set_present([](mkwv::gx::Processor& p) {
            ++presents;
            presented_draws += p.draw_count;
        });
        static const mkwv::runtime::RenderOffload hooks{
            [](const std::uint8_t* d, std::uint32_t n) {
                ++chunks;
                mkwv::runtime::render_consume_chunk(d, n);
            },
            [](const std::uint8_t* d, std::uint32_t n) { mkwv::runtime::render_consume_present(d, n); }};
        runtime.fifo().reset();
        runtime.processor().end_frame();
        mkwv::runtime::set_render_offload(&hooks);
        // The vertex format the display-list case above left: position f32 x3 + RGBA8 = 16
        // bytes. 1000 quads of 4 vertices plus a 3-byte header: ~67 KiB, more than one FIFO.
        constexpr unsigned kQuads = 1000;
        bool ok = true;
        for (unsigned q = 0; q < kQuads && ok; ++q) {
            ok &= call(cpu, kBegin, {GX_QUADS, GX_VTXFMT0, 4});
            for (unsigned i = 0; i < 4; ++i) {
                fifo_f32(static_cast<float>(i));
                fifo_f32(static_cast<float>(q % 480u));
                fifo_f32(0.0f);
                fifo_u32(0xFFFFFFFFu);
            }
        }
        mkwv::runtime::hle_present_frame();
        mkwv::runtime::set_render_offload(nullptr);
        if (!ok || chunks == 0 || presents != 1 || presented_draws != kQuads) {
            std::fprintf(stderr, "gx_hle_pipeline: offload chunks=%u presents=%u draws=%u\n", chunks, presents,
                         presented_draws);
            return fail("the render offload must decode a frame spanning several FIFO buffers");
        }
        if (runtime.fifo().size() != 0) {
            return fail("the guest FIFO must be empty after an offloaded present");
        }
    }

    std::printf("gx_hle_pipeline_tests: all cases pass\n");
    return 0;
}
