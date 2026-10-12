#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/demo.hpp"
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/fifo.hpp"
#include "mkwv/gx/efb_copy.hpp"
#include "mkwv/gx/tev.hpp"
#include "mkwv/gx/texture.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <cstring>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "gx_fifo: %s\n", message);
    return 1;
}

bool near(float a, float b, float eps = 1.0e-4f) {
    return std::fabs(a - b) <= eps;
}

void write_be_f32(std::uint8_t* p, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    p[0] = static_cast<std::uint8_t>(bits >> 24);
    p[1] = static_cast<std::uint8_t>(bits >> 16);
    p[2] = static_cast<std::uint8_t>(bits >> 8);
    p[3] = static_cast<std::uint8_t>(bits);
}

void pack_pos_clr(std::uint8_t* out, float x, float y, float z, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                  std::uint8_t a) {
    write_be_f32(out, x);
    write_be_f32(out + 4, y);
    write_be_f32(out + 8, z);
    out[12] = r;
    out[13] = g;
    out[14] = b;
    out[15] = a;
}

void pack_pos_clr_uv2(std::uint8_t* out, float x, float y, float z, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                      std::uint8_t a, float u0, float v0, float u1, float v1) {
    pack_pos_clr(out, x, y, z, r, g, b, a);
    write_be_f32(out + 16, u0);
    write_be_f32(out + 20, v0);
    write_be_f32(out + 24, u1);
    write_be_f32(out + 28, v1);
}

} // namespace

int main() {
    using namespace mkwv::gx;

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        configure_pos_clr0_direct(fifo, VtxFmt0);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("vcd/vat process");
        }
        if (proc.state.vtx_desc[AttrPos] != AttrDirect || proc.state.vtx_desc[AttrClr0] != AttrDirect) {
            return fail("vcd direct");
        }
        if (proc.state.vtx_fmts[0].attrs[AttrPos].type != F32 ||
            proc.state.vtx_fmts[0].attrs[AttrClr0].type != RGBA8) {
            return fail("vat fmt0");
        }
        if (vertex_stride(proc.state, VtxFmt0) != 16) {
            return fail("stride pos+clr0");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        configure_pos_clr0_tex0_direct(fifo, VtxFmt0);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("tex0 vat process");
        }
        if (proc.state.vtx_desc[AttrTex0] != AttrDirect || vertex_stride(proc.state, VtxFmt0) != 24) {
            return fail("stride pos+clr0+tex0");
        }
        std::uint8_t vert[24];
        pack_pos_clr(vert, 0.0f, 1.0f, 2.0f, 9, 8, 7, 6);
        write_be_f32(vert + 16, 0.25f);
        write_be_f32(vert + 20, 0.75f);
        // A stage only samples through a texgen (one TEX0 -> identity texgen here).
        fifo.set_gen_mode(1, 1, 1);
        fifo.set_tex_coord_gen(0, TgMtx2x4, TgTex0, kTexMtxIdentity);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        auto draw_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& draw = *draw_owner;
        draw.reset();
        if (!draw.process(fifo.data(), fifo.size(), true) || draw.vert_count != 1) {
            return fail("tex0 draw process");
        }
        if (!near(draw.verts[0].tc[0][0], 0.25f) || !near(draw.verts[0].tc[0][1], 0.75f) || !draw.draws[0].tex0) {
            return fail("tex0 uv");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        configure_pos_clr0_direct(fifo, VtxFmt0);
        std::uint8_t verts[48];
        pack_pos_clr(verts, 1.0f, 2.0f, 3.0f, 10, 20, 30, 40);
        pack_pos_clr(verts + 16, -1.0f, 0.0f, 0.5f, 1, 2, 3, 255);
        pack_pos_clr(verts + 32, 0.0f, -4.0f, 8.0f, 255, 0, 128, 64);
        fifo.draw(Triangles, VtxFmt0, 3, verts, sizeof(verts));
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("triangle process");
        }
        if (proc.draw_count != 1 || proc.vert_count != 3) {
            return fail("triangle counts");
        }
        // process() leaves GL clip space: identity matrices and the reset orthographic
        // projection keep x/y, and GX's [-w, 0] depth maps to GL's [-w, w] as z' = 2z + w.
        if (!near(proc.verts[0].x, 1.0f) || !near(proc.verts[0].y, 2.0f) || !near(proc.verts[0].z, 7.0f) ||
            !near(proc.verts[0].w, 1.0f)) {
            return fail("triangle pos0");
        }
        if (proc.verts[0].color != pack_rgba(10, 20, 30, 40) || proc.verts[2].color != pack_rgba(255, 0, 128, 64)) {
            return fail("triangle color");
        }
        std::uint16_t idx[8];
        if (expand_indices(Triangles, 3, idx, 8) != 3 || idx[0] != 0 || idx[2] != 2) {
            return fail("triangle expand");
        }
    }

    {
        std::uint16_t idx[16];
        if (expand_indices(Quads, 4, idx, 16) != 6 || idx[0] != 0 || idx[2] != 2 || idx[3] != 2 || idx[4] != 3 ||
            idx[5] != 0) {
            return fail("quad expand");
        }
        if (expand_indices(TriangleStrip, 4, idx, 16) != 6 || idx[0] != 0 || idx[1] != 1 || idx[2] != 2 || idx[3] != 2 ||
            idx[4] != 1 || idx[5] != 3) {
            return fail("strip expand");
        }
        if (expand_indices(TriangleFan, 4, idx, 16) != 6 || idx[3] != 0 || idx[4] != 2 || idx[5] != 3) {
            return fail("fan expand");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        float mtx[12];
        identity_3x4(mtx);
        mtx[3] = 5.0f;
        mtx[7] = -2.0f;
        fifo.load_xf_f32(0x0000, mtx, 12);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("xf pos mtx");
        }
        if (!near(proc.state.pn_mtx[0][3], 5.0f) || !near(proc.state.pn_mtx[0][7], -2.0f)) {
            return fail("xf pos mtx values");
        }
        DecodedVertex v{};
        v.x = 1.0f;
        v.y = 1.0f;
        v.z = 1.0f;
        float ndc[4];
        transform_vertex(proc.state, v, ndc);
        if (!near(ndc[0], 6.0f) || !near(ndc[1], -1.0f) || !near(ndc[2], 3.0f) || !near(ndc[3], 1.0f)) {
            return fail("translated transform");
        }
    }

    {
        State state;
        state.reset();
        perspective_raw(state.xf_proj_raw, 90.0f, 1.0f, 1.0f, 2.0f);
        state.proj_type = Perspective;
        state.apply_projection();
        DecodedVertex v{};
        v.z = -1.0f;
        float ndc[4];
        transform_vertex(state, v, ndc);
        // GX clip z runs -w (near) .. 0 (far); transform_vertex hands GL z' = 2z + w, so after
        // the divide near -> -1 and far -> +1, and glDepthRangef does the rest.
        if (!near(ndc[2] / ndc[3], -1.0f, 1.0e-3f)) {
            return fail("perspective near z");
        }
        v.z = -2.0f;
        transform_vertex(state, v, ndc);
        if (!near(ndc[2] / ndc[3], 1.0f, 1.0e-3f)) {
            return fail("perspective far z");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.load_bp(0x40, 1u | (static_cast<std::uint32_t>(LEqual) << 1) | (1u << 4));
        fifo.load_bp(0xC0, 0x00000Fu);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("bp process");
        }
        if (!proc.state.depth_test || !proc.state.depth_write || proc.state.depth_func != LEqual) {
            return fail("zmode");
        }
        if (proc.state.tev[0].color_d != 0xF) {
            return fail("tev color d");
        }
        if (classify_tev(proc.state) != TevOther) {
            return fail("tev other with d=zero");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_tev_op(0, TevModulate);
        fifo.set_tev_op(0, TevReplace);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("tev op process");
        }
        if (proc.state.tev[0].color_d != CcTexC || classify_tev(proc.state) != TevReplace) {
            return fail("tev replace");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_tev_op(0, TevModulate);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("tev modulate process");
        }
        if (proc.state.tev[0].color_a != CcZero || proc.state.tev[0].color_b != CcTexC ||
            proc.state.tev[0].color_c != CcRasC || proc.state.tev[0].color_d != CcZero) {
            return fail("tev modulate args");
        }
        if (!proc.state.tev[0].color_clamp || classify_tev(proc.state) != TevModulate) {
            return fail("tev modulate classify");
        }
        const std::uint32_t ras = pack_rgba(128, 64, 255, 255);
        const std::uint32_t tex = pack_rgba(255, 255, 0, 255);
        const std::uint32_t out = eval_tev_color(proc.state, ras, tex);
        if ((out & 0xFF) < 120 || (out & 0xFF) > 136) {
            return fail("tev modulate red");
        }
        if (((out >> 8) & 0xFF) < 60 || ((out >> 8) & 0xFF) > 70) {
            return fail("tev modulate green");
        }
        if (((out >> 16) & 0xFF) != 0) {
            return fail("tev modulate blue");
        }
    }

    {
        State state;
        state.reset();
        if (classify_tev(state) != TevReplace) {
            return fail("default tev replace");
        }
        const std::uint32_t out = eval_tev_color(state, pack_rgba(1, 2, 3, 4), pack_rgba(9, 8, 7, 6));
        if (out != pack_rgba(9, 8, 7, 6)) {
            return fail("tev replace eval");
        }
        state.tev[0].color_shift = 1;
        const std::uint32_t doubled = eval_tev_color(state, pack_rgba(0, 0, 0, 0), pack_rgba(80, 40, 20, 10));
        if (doubled != pack_rgba(160, 80, 40, 10)) {
            return fail("tev shift eval");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_tev_kcolor(0, 10, 20, 30, 40);
        fifo.set_tev_kcolor_sel(0, 0x0C);
        fifo.set_tev_op(0, TevReplace);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("konst process");
        }
        if (proc.state.konst[0] != pack_rgba(10, 20, 30, 40) || proc.state.tev[0].kcsel != 0x0C) {
            return fail("konst load");
        }
        proc.state.tev[0].color_d = CcKonst;
        proc.state.tev[0].alpha_d = CaKonst;
        proc.state.tev[0].kasel = 0x1C;
        const std::uint32_t out = eval_tev_color(proc.state, 0, 0);
        if (out != pack_rgba(10, 20, 30, 40)) {
            return fail("konst eval");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_gen_mode(1, 1, 2);
        fifo.set_tev_op(0, TevPassClr);
        fifo.set_tev_op(1, TevModulate);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("tev two stage process");
        }
        if (proc.state.num_tev_stages != 2 || classify_tev(proc.state) != TevModulate) {
            return fail("tev two stage classify");
        }
        const std::uint32_t ras = pack_rgba(128, 64, 255, 255);
        const std::uint32_t tex = pack_rgba(255, 255, 0, 255);
        const std::uint32_t out = eval_tev_color(proc.state, ras, tex);
        if ((out & 0xFF) < 120 || (out & 0xFF) > 136) {
            return fail("tev two stage red");
        }
        if (((out >> 8) & 0xFF) < 60 || ((out >> 8) & 0xFF) > 70) {
            return fail("tev two stage green");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        const float mtx[8] = {0.25f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, -0.25f, 3.0f};
        fifo.set_gen_mode(1, 1, 1);
        fifo.set_tev_op(0, TevPassClr);
        fifo.load_tex_mtx(kTexMtx0, mtx, 8);
        fifo.set_tex_coord_gen(0, TgMtx2x4, TgPos, kTexMtx0);
        configure_pos_clr0_direct(fifo, VtxFmt0);
        std::uint8_t vert[16];
        pack_pos_clr(vert, -16.0f, 0.0f, 12.0f, 1, 2, 3, 4);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("texgen process");
        }
        if (proc.state.texgens[0].src != TgPos || proc.state.texgens[0].mtx != kTexMtx0 ||
            proc.state.num_tex_gens != 1) {
            return fail("texgen state");
        }
        if (!near(proc.verts[0].tc[0][0], 0.0f) || !near(proc.verts[0].tc[0][1], 0.0f)) {
            return fail("texgen pos uv");
        }
    }

    {
        State state;
        state.reset();
        state.num_chans = 1;
        state.chans[0].lighting = true;
        state.chans[0].mat_src = SrcVtx;
        state.chans[0].amb_src = SrcReg;
        state.chans[0].diff_fn = DfClamp;
        state.chans[0].light_mask = 1;
        state.ambient[0] = pack_rgba(0, 0, 0, 255);
        state.lights[0].color = pack_rgba(255, 255, 255, 255);
        state.lights[0].pos[2] = 100.0f;
        // The vertex colour is the material only when the vertex format carries one;
        // without CLR0 GX lights a white material.
        state.vtx_desc[AttrClr0] = AttrDirect;
        DecodedVertex lit{};
        lit.nz = 1.0f;
        lit.color = pack_rgba(128, 64, 32, 255);
        apply_lighting(state, lit);
        if ((lit.color & 0xFF) < 120 || (lit.color & 0xFF) > 136) {
            return fail("lighting facing red");
        }
        state.lights[0].pos[0] = 0.0f;
        state.lights[0].pos[1] = 100.0f;
        state.lights[0].pos[2] = 0.0f;
        DecodedVertex dark{};
        dark.nz = 1.0f;
        dark.color = pack_rgba(128, 64, 32, 255);
        apply_lighting(state, dark);
        if ((dark.color & 0xFF) > 8) {
            return fail("lighting side red");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_chan_amb_color(0, 16, 32, 48);
        fifo.set_chan_ctrl(0, true, SrcReg, SrcVtx, 0x01, DfClamp, AfNone);
        fifo.load_light(0, 255, 0, 0, 255, 0.0f, 0.0f, 50.0f);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("chan xf process");
        }
        if (!proc.state.chans[0].lighting || proc.state.chans[0].diff_fn != DfClamp ||
            (proc.state.chans[0].light_mask & 1u) == 0) {
            return fail("chan ctrl");
        }
        if (proc.state.ambient[0] != pack_rgba(16, 32, 48, 255)) {
            return fail("chan amb");
        }
        if ((proc.state.lights[0].color & 0xFF) != 255 || !near(proc.state.lights[0].pos[2], 50.0f)) {
            return fail("light obj");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_blend_mode(BlendMix, BlSrcAlpha, BlInvSrcAlpha);
        fifo.set_alpha_compare(GEqual, 16, 0, Always, 255);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("pe process");
        }
        if (proc.state.blend_mode != BlendMix || proc.state.blend_src != BlSrcAlpha ||
            proc.state.blend_dst != BlInvSrcAlpha || !proc.state.color_update || !proc.state.alpha_update) {
            return fail("pe blend");
        }
        if (proc.state.alpha_comp0 != GEqual || proc.state.alpha_ref0 != 16 || proc.state.alpha_comp1 != Always ||
            proc.state.alpha_ref1 != 255) {
            return fail("pe alpha compare");
        }
        fifo.reset();
        proc.reset();
        fifo.set_blend_mode(BlendNone, BlOne, BlZero);
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.state.blend_mode != BlendNone) {
            return fail("pe blend off");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        configure_pos_clr0_direct(fifo, VtxFmt0);
        fifo.draw(Triangles, VtxFmt0, 3, nullptr, 0);
        if (proc.process(fifo.data(), fifo.size(), true)) {
            return fail("truncated draw should fail");
        }
        if (proc.error_count == 0) {
            return fail("truncated draw error count");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        std::uint8_t pos[36];
        std::uint8_t clr[12];
        write_be_f32(pos, 1.0f);
        write_be_f32(pos + 4, 0.0f);
        write_be_f32(pos + 8, 0.0f);
        write_be_f32(pos + 12, 0.0f);
        write_be_f32(pos + 16, 2.0f);
        write_be_f32(pos + 20, 0.0f);
        write_be_f32(pos + 24, 0.0f);
        write_be_f32(pos + 28, 0.0f);
        write_be_f32(pos + 32, 3.0f);
        clr[0] = 10;
        clr[1] = 20;
        clr[2] = 30;
        clr[3] = 40;
        clr[4] = 50;
        clr[5] = 60;
        clr[6] = 70;
        clr[7] = 80;
        clr[8] = 90;
        clr[9] = 100;
        clr[10] = 110;
        clr[11] = 120;
        configure_pos_index16_clr0_index8(fifo, VtxFmt0);
        fifo.set_array(AttrPos, pos, sizeof(pos), 12, false);
        fifo.set_array(AttrClr0, clr, sizeof(clr), 4, false);
        const std::uint8_t idx[] = {0x00, 0x00, 0, 0x00, 0x02, 2, 0x00, 0x01, 1};
        fifo.draw(Triangles, VtxFmt0, 3, idx, 9);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("indexed process");
        }
        if (vertex_stride(proc.state, VtxFmt0) != 3) {
            return fail("indexed stride");
        }
        if (!near(proc.verts[0].x, 1.0f) || !near(proc.verts[1].z, 7.0f) || !near(proc.verts[2].y, 2.0f)) {
            return fail("indexed gather pos");
        }
        if (proc.verts[0].color != pack_rgba(10, 20, 30, 40) || proc.verts[1].color != pack_rgba(90, 100, 110, 120) ||
            proc.verts[2].color != pack_rgba(50, 60, 70, 80)) {
            return fail("indexed gather color");
        }
        if (proc.state.arrays[AttrPos].stride != 12 || proc.state.arrays[AttrClr0].stride != 4) {
            return fail("array stride");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        configure_pos_index16_clr0_index8(fifo, VtxFmt0);
        const std::uint8_t idx[] = {0x00, 0x00, 0, 0x00, 0x01, 1, 0x00, 0x02, 2};
        fifo.draw(Triangles, VtxFmt0, 3, idx, 9);
        if (proc.process(fifo.data(), fifo.size(), true)) {
            return fail("indexed without array should fail");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        if (!build_demo_frame(fifo, 0.25f) || fifo.size() == 0) {
            return fail("demo fifo");
        }
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("demo process");
        }
        if (proc.draw_count != 6 || proc.vert_count != 33) {
            std::fprintf(stderr, "gx_fifo: demo draws=%u verts=%u\n", proc.draw_count, proc.vert_count);
            return fail("demo draw/vert counts");
        }
        if (proc.draws[0].prim != Quads || proc.draws[2].prim != TriangleStrip || proc.draws[3].prim != Triangles ||
            proc.draws[4].prim != TriangleFan || proc.draws[5].prim != Triangles) {
            return fail("demo primitives");
        }
        if (!proc.draws[0].tex0 || !proc.draws[1].tex0 || proc.draws[2].tex0 || proc.draws[5].tex0) {
            return fail("demo tex0 flags");
        }
        if (proc.draws[0].tev != TevModulate || proc.draws[1].tev != TevModulate) {
            return fail("demo tev modulate");
        }
        // tev_key is no longer hashed per draw (nothing linked reads it); the stages are.
        if (std::memcmp(proc.draws[0].tev_stages, proc.draws[1].tev_stages, sizeof(proc.draws[0].tev_stages)) != 0 ||
            proc.draws[0].num_tev_stages != 2) {
            return fail("demo tev key");
        }
        if (proc.verts[0].color != pack_rgba(255, 250, 240, 255) ||
            proc.verts[2].color != pack_rgba(186, 190, 204, 255)) {
            return fail("demo tev vertex color");
        }
        if (!proc.draws[4].blend || proc.draws[4].blend_src != BlSrcAlpha || proc.draws[4].blend_dst != BlInvSrcAlpha) {
            return fail("demo coin blend");
        }
        if (proc.draws[5].blend) {
            return fail("demo triangle opaque");
        }
        if (proc.state.vtx_desc[AttrPos] != AttrIndex16 || proc.state.vtx_desc[AttrClr0] != AttrIndex8 ||
            proc.state.vtx_desc[AttrNrm] != AttrIndex8) {
            return fail("demo indexed vcd");
        }
        if (!proc.state.chans[0].lighting || (proc.state.chans[0].light_mask & 1u) == 0) {
            return fail("demo lighting");
        }
        if (proc.state.proj_type != Perspective) {
            return fail("demo projection");
        }
        if (!near(proc.verts[0].tc[0][0], 0.0f) || !near(proc.verts[1].tc[0][0], 8.0f) || !near(proc.verts[1].tc[0][1], 0.0f) ||
            !near(proc.verts[2].tc[0][1], 7.5f)) {
            return fail("demo ground uv");
        }
        const DrawCall& tri = proc.draws[5];
        const DecodedVertex& tip = proc.verts[tri.first_vertex];
        // Clip space: divide by w for NDC.
        const float tx = tip.x / tip.w;
        const float ty = tip.y / tip.w;
        if (tip.w <= 0.0f || tx < -1.5f || tx > 1.5f || ty < -1.5f || ty > 1.5f) {
            return fail("demo triangle ndc");
        }
        if (!near(tip.nx, 0.0f) || !near(tip.ny, 0.0f) || !near(tip.nz, 1.0f)) {
            return fail("demo triangle nrm");
        }
        if (tip.color == pack_rgba(0x59, 0x51, 0xB8, 255)) {
            return fail("demo triangle lit");
        }
        const TexObj& tex0 = proc.state.texmaps[0];
        if (tex0.data == nullptr || tex0.width != 64 || tex0.height != 64 || tex0.format != TexCMPR) {
            return fail("demo texobj");
        }
        if (tex0.wrap_s != WrapRepeat || tex0.wrap_t != WrapRepeat) {
            return fail("demo tex wrap");
        }
        ConvertedTexture host{};
        // Every format, CMPR included, is decoded to RGBA8 (Aurora's texture_convert).
        static std::uint8_t rgba[64 * 64 * 4];
        if (!convert_gx_texture(tex0, rgba, sizeof(rgba), &host) || host.layout != HostRgba8 ||
            host.bytes != sizeof(rgba)) {
            return fail("demo cmpr to rgba8");
        }
    }

    {
        if (tex_buffer_size(8, 8, TexCMPR) != 32 || tex_buffer_size(4, 4, TexRGB565) != 32 ||
            dxt1_buffer_size(8, 8) != 32) {
            return fail("tex buffer size");
        }
        std::uint8_t cmpr[32];
        encode_cmpr_solid(cmpr, 8, 8, pack_rgb565(255, 0, 0));
        std::uint8_t rgba[8 * 8 * 4];
        if (!cmpr_to_rgba8(cmpr, sizeof(cmpr), 8, 8, rgba, sizeof(rgba))) {
            return fail("cmpr rgba");
        }
        if (rgba[0] < 240 || rgba[1] != 0 || rgba[2] != 0 || rgba[3] != 255) {
            std::fprintf(stderr, "gx_fifo: cmpr rgba %u %u %u %u\n", rgba[0], rgba[1], rgba[2], rgba[3]);
            return fail("cmpr red");
        }
        std::uint8_t dxt[32];
        std::uint8_t round[8 * 8 * 4];
        if (!cmpr_to_dxt1(cmpr, sizeof(cmpr), 8, 8, dxt, sizeof(dxt)) ||
            !dxt1_to_rgba8(dxt, sizeof(dxt), 8, 8, round, sizeof(round))) {
            return fail("cmpr dxt1 roundtrip");
        }
        if (round[0] < 240 || round[1] != 0 || round[2] != 0 || round[3] != 255) {
            return fail("dxt1 red");
        }
        std::uint8_t mixed[32]{};
        encode_cmpr_solid(mixed, 8, 8, pack_rgb565(255, 0, 0));
        mixed[2] = 0;
        mixed[3] = 0;
        mixed[4] = 0x40;
        std::uint8_t mixed_rgba[8 * 8 * 4];
        std::uint8_t mixed_dxt[32];
        std::uint8_t mixed_round[8 * 8 * 4];
        if (!cmpr_to_rgba8(mixed, sizeof(mixed), 8, 8, mixed_rgba, sizeof(mixed_rgba)) ||
            !cmpr_to_dxt1(mixed, sizeof(mixed), 8, 8, mixed_dxt, sizeof(mixed_dxt)) ||
            !dxt1_to_rgba8(mixed_dxt, sizeof(mixed_dxt), 8, 8, mixed_round, sizeof(mixed_round))) {
            return fail("cmpr mixed convert");
        }
        if (mixed_rgba[0] != 0 || mixed_rgba[1] != 0 || mixed_rgba[2] != 0 || mixed_round[0] != 0) {
            return fail("cmpr mixed index");
        }
        if (mixed_rgba[4] < 240 || mixed_round[4] < 240) {
            return fail("cmpr mixed neighbor");
        }
    }

    {
        std::uint8_t tiled[32];
        for (int i = 0; i < 16; ++i) {
            const std::uint16_t c = pack_rgb565(0, 255, 0);
            tiled[i * 2] = static_cast<std::uint8_t>(c >> 8);
            tiled[i * 2 + 1] = static_cast<std::uint8_t>(c);
        }
        std::uint8_t rgba[4 * 4 * 4];
        if (!tiled_to_rgba8(tiled, sizeof(tiled), 4, 4, TexRGB565, rgba, sizeof(rgba))) {
            return fail("rgb565 tiled");
        }
        if (rgba[0] != 0 || rgba[1] < 240 || rgba[2] != 0) {
            return fail("rgb565 green");
        }
    }

    {
        std::uint8_t image[32]{};
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.load_texobj(2, image, 8, 8, TexRGB5A3, WrapMirror, WrapClamp, false, 7, 3);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("texobj process");
        }
        const TexObj& slot = proc.state.texmaps[2];
        if (slot.data != image || slot.width != 8 || slot.height != 8 || slot.format != TexRGB5A3) {
            return fail("texobj slot");
        }
        if (slot.id != 7 || slot.version != 3 || slot.wrap_s != WrapMirror || slot.wrap_t != WrapClamp) {
            return fail("texobj meta");
        }
        if (proc.state.texmaps[0].data != nullptr) {
            return fail("texobj other map");
        }
    }

    {
        std::uint8_t ia4[32];
        std::memset(ia4, 0x0F, sizeof(ia4));
        std::uint8_t rgba[8 * 4 * 4];
        if (!tiled_to_rgba8(ia4, sizeof(ia4), 8, 4, TexIA4, rgba, sizeof(rgba))) {
            return fail("ia4 tiled");
        }
        if (rgba[0] < 240 || rgba[1] < 240 || rgba[2] < 240 || rgba[3] != 0) {
            return fail("ia4 white a0");
        }
    }

    {
        std::uint8_t ci4[32]{};
        std::uint8_t pal[32]{};
        pal[0] = 0x83;
        pal[1] = 0xE0;
        TlutObj tlut{};
        tlut.data = pal;
        tlut.format = TlutRgb5a3;
        tlut.entries = 16;
        std::uint8_t rgba[8 * 8 * 4];
        if (!tiled_to_rgba8(ci4, sizeof(ci4), 8, 8, TexCI4, rgba, sizeof(rgba), &tlut)) {
            return fail("ci4 tiled");
        }
        if (rgba[0] != 0 || rgba[1] < 240 || rgba[2] != 0 || rgba[3] != 255) {
            return fail("ci4 tlut green");
        }
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.load_tlut(3, pal, TlutRgb5a3, 16, 11, 2);
        fifo.load_texobj(0, ci4, 8, 8, TexCI4, WrapClamp, WrapClamp, false, 4, 1, 3);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("tlut process");
        }
        if (proc.state.tluts[3].data != pal || proc.state.tluts[3].entries != 16 ||
            proc.state.tluts[3].format != TlutRgb5a3 || proc.state.tluts[3].id != 11) {
            return fail("tlut slot");
        }
        if (proc.state.texmaps[0].format != TexCI4 || proc.state.texmaps[0].tlut != 3) {
            return fail("texobj tlut idx");
        }
        ConvertedTexture host{};
        std::uint8_t converted[8 * 8 * 4];
        if (!convert_gx_texture(proc.state.texmaps[0], converted, sizeof(converted), &host, &proc.state.tluts[3]) ||
            host.layout != HostRgba8 || converted[1] < 240) {
            return fail("ci4 convert tlut");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        const float post[12] = {2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        fifo.set_gen_mode(1, 1, 1);
        fifo.set_tev_op(0, TevPassClr);
        fifo.load_tex_mtx(kPtTexMtx0, post, 12);
        fifo.set_tex_coord_gen(0, TgMtx2x4, TgTex0, kTexMtxIdentity, false, kPtTexMtx0);
        configure_pos_clr0_tex0_direct(fifo, VtxFmt0);
        std::uint8_t vert[24];
        pack_pos_clr(vert, 0.0f, 0.0f, 0.0f, 1, 2, 3, 4);
        write_be_f32(vert + 16, 0.25f);
        write_be_f32(vert + 20, 0.5f);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("post texgen process");
        }
        if (proc.state.texgens[0].post_mtx != kPtTexMtx0) {
            return fail("post texgen state");
        }
        if (!near(proc.verts[0].tc[0][0], 0.5f) || !near(proc.verts[0].tc[0][1], 0.5f)) {
            return fail("post texgen uv");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_tev_swap_table(0, 2, 1, 0, 3);
        fifo.set_tev_op(0, TevPassClr);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("swap process");
        }
        if (proc.state.tev_swap[0][0] != 2 || proc.state.tev_swap[0][1] != 1 || proc.state.tev_swap[0][2] != 0 ||
            proc.state.tev_swap[0][3] != 3) {
            return fail("swap table");
        }
        const std::uint32_t out = eval_tev_color(proc.state, pack_rgba(255, 0, 0, 128), 0);
        if (out != pack_rgba(0, 0, 255, 128)) {
            return fail("swap eval bgra");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        fifo.nop();
        fifo.load_cp(0x30, 3);
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.state.current_pn_mtx != 1) {
            return fail("pnmtx idx / nop");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        configure_pos_clr0_tex0_tex1_direct(fifo, VtxFmt0);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("tex1 vat process");
        }
        if (proc.state.vtx_desc[AttrTex1] != AttrDirect || vertex_stride(proc.state, VtxFmt0) != 32) {
            return fail("stride pos+clr0+tex0+tex1");
        }
        std::uint8_t vert[32];
        pack_pos_clr_uv2(vert, 0.0f, 1.0f, 2.0f, 9, 8, 7, 6, 0.25f, 0.75f, 0.5f, 0.125f);
        fifo.set_gen_mode(2, 1, 1);
        fifo.set_tex_coord_gen(0, TgMtx2x4, TgTex0, kTexMtxIdentity);
        fifo.set_tex_coord_gen(1, TgMtx2x4, TgTex1, kTexMtxIdentity);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        auto draw_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& draw = *draw_owner;
        draw.reset();
        if (!draw.process(fifo.data(), fifo.size(), true) || draw.vert_count != 1) {
            return fail("tex1 draw process");
        }
        if (!near(draw.verts[0].tc[0][0], 0.25f) || !near(draw.verts[0].tc[0][1], 0.75f) || !near(draw.verts[0].tc[1][0], 0.5f) ||
            !near(draw.verts[0].tc[1][1], 0.125f) || !draw.draws[0].tex0) {
            return fail("tex1 uv");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_gen_mode(2, 1, 1);
        fifo.set_tex_coord_gen(0, TgMtx2x4, TgTex0, kTexMtxIdentity);
        fifo.set_tex_coord_gen(1, TgMtx2x4, TgTex1, kTexMtxIdentity);
        configure_pos_clr0_tex0_tex1_direct(fifo, VtxFmt0);
        std::uint8_t vert[32];
        pack_pos_clr_uv2(vert, 0.0f, 0.0f, 0.0f, 1, 2, 3, 4, 0.2f, 0.4f, 0.6f, 0.8f);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("dual texgen process");
        }
        if (proc.state.num_tex_gens != 2 || proc.state.texgens[1].src != TgTex1) {
            return fail("dual texgen state");
        }
        if (!near(proc.verts[0].tc[0][0], 0.2f) || !near(proc.verts[0].tc[0][1], 0.4f) || !near(proc.verts[0].tc[1][0], 0.6f) ||
            !near(proc.verts[0].tc[1][1], 0.8f)) {
            return fail("dual texgen uv");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_gen_mode(2, 1, 2);
        fifo.set_tev_order(0, 0, 0, kRasColor0);
        fifo.set_tev_order(1, 1, 1, kRasColor0);
        fifo.set_tev_op(0, TevReplace);
        fifo.set_tev_op(1, TevModulate);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("tev order process");
        }
        if (!proc.state.tev[0].tex_enabled || proc.state.tev[0].texmap != 0 || proc.state.tev[0].texcoord != 0) {
            return fail("tev order stage0");
        }
        if (!proc.state.tev[1].tex_enabled || proc.state.tev[1].texmap != 1 || proc.state.tev[1].texcoord != 1) {
            return fail("tev order stage1");
        }
        const std::uint32_t maps[2] = {pack_rgba(255, 255, 255, 255), pack_rgba(128, 64, 0, 255)};
        const std::uint32_t out = eval_tev_color(proc.state, pack_rgba(255, 255, 255, 255), 0, maps, 2);
        if ((out & 0xFF) < 120 || (out & 0xFF) > 136) {
            return fail("dual texmap red");
        }
        if (((out >> 8) & 0xFF) < 60 || ((out >> 8) & 0xFF) > 70) {
            return fail("dual texmap green");
        }
        if (((out >> 16) & 0xFF) != 0) {
            return fail("dual texmap blue");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_tev_order(0, 0, kTexMapNull, kRasColor1);
        fifo.set_tev_op(0, TevPassClr);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("tev order null process");
        }
        if (proc.state.tev[0].tex_enabled || proc.state.tev[0].texmap != kTexMapNull ||
            proc.state.tev[0].ras_chan != kRasColor1) {
            return fail("tev order null/ras1");
        }
        const std::uint32_t maps[1] = {pack_rgba(9, 8, 7, 6)};
        const std::uint32_t out =
            eval_tev_color(proc.state, pack_rgba(1, 2, 3, 4), pack_rgba(10, 20, 30, 40), maps, 1);
        if (out != pack_rgba(10, 20, 30, 40)) {
            return fail("ras1 eval");
        }
        fifo.reset();
        fifo.set_tev_order(0, 0, kTexMapNull, kRasZero);
        fifo.set_tev_op(0, TevPassClr);
        auto zero_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& zero = *zero_owner;
        zero.reset();
        if (!zero.process(fifo.data(), fifo.size(), true) || zero.state.tev[0].ras_chan != kRasZero) {
            return fail("tev order ras zero");
        }
        if (eval_tev_color(zero.state, pack_rgba(1, 2, 3, 4), pack_rgba(9, 8, 7, 6)) != 0) {
            return fail("ras zero eval");
        }
    }

    {
        State state;
        state.reset();
        state.tev[0].color_a = CcTexC;
        state.tev[0].color_b = CcRasC;
        state.tev[0].color_c = CcOne;
        state.tev[0].color_d = CcZero;
        state.tev[0].color_op = TevCompR8Gt;
        const std::uint32_t gt =
            eval_tev_color(state, pack_rgba(10, 0, 0, 255), pack_rgba(200, 0, 0, 255));
        if (gt != pack_rgba(255, 255, 255, 255)) {
            return fail("tev r8 gt");
        }
        const std::uint32_t ngt =
            eval_tev_color(state, pack_rgba(200, 0, 0, 255), pack_rgba(10, 0, 0, 255));
        if (ngt != pack_rgba(0, 0, 0, 255)) {
            return fail("tev r8 gt false");
        }
        state.tev[0].color_op = TevCompRgb8Gt;
        const std::uint32_t rgb =
            eval_tev_color(state, pack_rgba(10, 200, 10, 255), pack_rgba(200, 10, 200, 255));
        if (rgb != pack_rgba(255, 0, 255, 255)) {
            return fail("tev rgb8 gt");
        }
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_tev_op(0, TevReplace);
        fifo.set_tev_color_op(0, TevCompR8Gt);
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.state.tev[0].color_op != TevCompR8Gt) {
            return fail("tev compare op decode");
        }
        fifo.reset();
        fifo.set_tev_op(0, TevReplace);
        fifo.set_tev_color_op(0, TevCompRgb8Eq);
        auto eq_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& eq = *eq_owner;
        eq.reset();
        if (!eq.process(fifo.data(), fifo.size(), true) || eq.state.tev[0].color_op != TevCompRgb8Eq) {
            return fail("tev compare eq decode");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_fog(FogOrthoLin, 0.0f, 1.0f, 0.0f, 1.0f, 255, 0, 0);
        configure_pos_clr0_direct(fifo, VtxFmt0);
        std::uint8_t vert[16];
        pack_pos_clr(vert, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("fog process");
        }
        if (proc.state.fog.type != FogOrthoLin || proc.state.fog.color != pack_rgba(255, 0, 0, 255)) {
            return fail("fog state");
        }
        if (!near(eval_fog_factor(proc.state, 0.0f), 0.0f) || !near(eval_fog_factor(proc.state, 1.0f), 1.0f) ||
            !near(eval_fog_factor(proc.state, 0.5f), 0.5f, 0.02f)) {
            return fail("fog factor");
        }
        const std::uint32_t mixed = mix_fog(proc.state, pack_rgba(255, 255, 255, 128), 0.5f);
        if ((mixed & 0xFF) != 255 || ((mixed >> 8) & 0xFF) < 120 || ((mixed >> 8) & 0xFF) > 136 ||
            ((mixed >> 24) & 0xFF) != 128) {
            return fail("fog mix");
        }
        if (proc.draw_count != 1 || proc.draws[0].fog.type != FogOrthoLin) {
            return fail("fog draw snapshot");
        }
        State fogged = proc.state;
        State clear_fog = proc.state;
        clear_fog.fog = FogState{};
        if (hash_tev(fogged) == hash_tev(clear_fog)) {
            return fail("fog hash");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_viewport(10.0f, 20.0f, 100.0f, 50.0f, 0.0f, 1.0f);
        fifo.set_scissor(4, 8, 32, 16);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("viewport/scissor process");
        }
        if (!proc.state.has_render_vp || !near(proc.state.render_vp[0], 10.0f) ||
            !near(proc.state.render_vp[1], 20.0f) || !near(proc.state.render_vp[2], 100.0f) ||
            !near(proc.state.render_vp[3], 50.0f) || !near(proc.state.render_vp[4], 0.0f) ||
            !near(proc.state.render_vp[5], 1.0f)) {
            return fail("aurora viewport");
        }
        if (!proc.state.has_render_scissor || proc.state.render_scissor[0] != 4 ||
            proc.state.render_scissor[1] != 8 || proc.state.render_scissor[2] != 32 ||
            proc.state.render_scissor[3] != 16) {
            return fail("aurora scissor");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        const std::int32_t x = 10;
        const std::int32_t y = 20;
        const std::int32_t w = 100;
        const std::int32_t h = 50;
        fifo.load_bp(0x20, static_cast<std::uint32_t>(y + 342) | (static_cast<std::uint32_t>(x + 342) << 12));
        fifo.load_bp(0x21, static_cast<std::uint32_t>(y + h - 1 + 342) |
                               (static_cast<std::uint32_t>(x + w - 1 + 342) << 12));
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("bp scissor process");
        }
        if (!proc.state.has_scissor || proc.state.scissor_x != x || proc.state.scissor_y != y ||
            proc.state.scissor_w != w || proc.state.scissor_h != h) {
            return fail("bp scissor");
        }
    }

    {
        auto inner_owner = std::make_unique<Fifo>();
        Fifo& inner = *inner_owner;
        inner.set_line_width(10, 12);
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        proc.map_range(0x80001000u, inner.data(), inner.size());
        fifo.call_dl(0x1000, inner.size());
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.skipped_dl != 0) {
            return fail("call dl");
        }
        if (proc.state.line_width != 10 || proc.state.point_size != 12) {
            return fail("call dl line width");
        }
        auto rec_owner = std::make_unique<Fifo>();
        Fifo& rec = *rec_owner;
        rec.call_dl(0x2000, 9);
        auto nest_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& nest = *nest_owner;
        nest.reset();
        nest.map_range(0x80002000u, rec.data(), rec.size());
        if (!nest.process(rec.data(), rec.size(), true) || nest.skipped_dl == 0) {
            return fail("nested call dl skip");
        }
    }

    {
        float mtx[12] = {1.0f, 0.0f, 0.0f, 5.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_xf_array(0, mtx, sizeof(mtx), 48);
        fifo.load_indx(kLoadIndxA, 0, 12, 0);
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.skipped_indexed_xf != 0) {
            return fail("indexed xf");
        }
        if (!near(proc.state.pn_mtx[0][3], 5.0f) || !near(proc.state.xf_mem[3], 5.0f)) {
            return fail("indexed xf pos mtx");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        configure_pos_clr0_direct(fifo, VtxFmt0);
        fifo.set_line_width(6, 6);
        std::uint8_t verts[32];
        pack_pos_clr(verts, -0.5f, 0.0f, 0.0f, 255, 255, 0, 255);
        pack_pos_clr(verts + 16, 0.5f, 0.0f, 0.0f, 255, 255, 0, 255);
        fifo.draw(Lines, VtxFmt0, 2, verts, sizeof(verts));
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.draw_count != 1 || proc.vert_count != 6) {
            return fail("line expand");
        }
        if (proc.draws[0].prim != Triangles || proc.draws[0].vertex_count != 6) {
            return fail("line prim");
        }
        bool raised = false;
        bool lowered = false;
        for (std::uint32_t i = 0; i < 6; ++i) {
            if (proc.verts[i].y > 0.0f) {
                raised = true;
            }
            if (proc.verts[i].y < 0.0f) {
                lowered = true;
            }
        }
        if (!raised || !lowered) {
            return fail("line thickness");
        }
        fifo.reset();
        proc.reset();
        configure_pos_clr0_direct(fifo, VtxFmt0);
        fifo.set_line_width(6, 24);
        std::uint8_t pt[16];
        pack_pos_clr(pt, 0.0f, 0.0f, 0.0f, 255, 0, 0, 255);
        fifo.draw(Points, VtxFmt0, 1, pt, sizeof(pt));
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.vert_count != 6) {
            return fail("point expand");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.load_cp(0x50, (static_cast<std::uint32_t>(AttrDirect) << 9) | (static_cast<std::uint32_t>(AttrDirect) << 11));
        fifo.load_cp(0x60, 0);
        const std::uint32_t vat_a = (static_cast<std::uint32_t>(PosXYZ) << 0) | (static_cast<std::uint32_t>(F32) << 1) |
                                    (1u << 9) | (static_cast<std::uint32_t>(F32) << 10);
        fifo.load_cp(0x70, vat_a);
        fifo.set_gen_mode(1, 1, 1);
        fifo.set_tex_coord_gen(0, TgMtx2x4, TgBinrm, kTexMtxIdentity);
        std::uint8_t vert[48];
        write_be_f32(vert, 0.0f);
        write_be_f32(vert + 4, 0.0f);
        write_be_f32(vert + 8, 0.0f);
        write_be_f32(vert + 12, 0.0f);
        write_be_f32(vert + 16, 0.0f);
        write_be_f32(vert + 20, 1.0f);
        write_be_f32(vert + 24, 0.25f);
        write_be_f32(vert + 28, 0.50f);
        write_be_f32(vert + 32, 0.0f);
        write_be_f32(vert + 36, 0.0f);
        write_be_f32(vert + 40, 1.0f);
        write_be_f32(vert + 44, 0.0f);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.vert_count != 1) {
            return fail("nbt process");
        }
        if (!near(proc.verts[0].nx, 0.0f) || !near(proc.verts[0].nz, 1.0f) || !near(proc.verts[0].bx, 0.25f) ||
            !near(proc.verts[0].by, 0.50f) || !near(proc.verts[0].ty, 1.0f)) {
            return fail("nbt decode");
        }
        if (!near(proc.verts[0].tc[0][0], 0.25f) || !near(proc.verts[0].tc[0][1], 0.50f)) {
            return fail("nbt texgen binrm");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_blend_mode(BlendSubtract, BlOne, BlOne);
        configure_pos_clr0_direct(fifo, VtxFmt0);
        std::uint8_t vert[16];
        pack_pos_clr(vert, 0.0f, 0.0f, 0.0f, 255, 0, 0, 255);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.draws[0].blend_mode != BlendSubtract ||
            !proc.draws[0].blend) {
            return fail("subtract blend");
        }
        fifo.reset();
        auto logic_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& logic = *logic_owner;
        logic.reset();
        fifo.set_blend_mode(BlendLogic, BlOne, BlZero, LoXor);
        configure_pos_clr0_direct(fifo, VtxFmt0);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        if (!logic.process(fifo.data(), fifo.size(), true) || logic.draws[0].blend_mode != BlendLogic ||
            logic.draws[0].logic_op != LoXor || logic.draws[0].blend) {
            return fail("logic op");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.set_gen_mode(1, 1, 1, 1);
        fifo.set_ind_order(0, 0, 1);
        const float m[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        fifo.set_ind_mtx(0, m, 0);
        fifo.set_tev_indirect(0, 0, IndFmt8, 0, 1, IndWrapOff, IndWrapOff, false);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("indirect process");
        }
        if (proc.state.num_ind_stages != 1 || proc.state.ind[0].texmap != 1 || proc.state.tev[0].ind_mtx != 1) {
            return fail("indirect state");
        }
        float s = 0.0f;
        float t = 0.0f;
        eval_indirect_st(proc.state, 0, pack_rgba(64, 0, 0, 255), 0.25f, 0.5f, &s, &t);
        if (!near(s, 0.5f, 0.02f) || !near(t, 0.5f, 0.02f)) {
            return fail("indirect st");
        }
        proc.state.tev[0].ind_wrap_s = IndWrap0;
        eval_indirect_st(proc.state, 0, pack_rgba(0, 0, 0, 255), 0.25f, 0.5f, &s, &t);
        if (!near(s, 0.0f) || !near(t, 0.5f)) {
            return fail("indirect wrap0");
        }
        State hashed = proc.state;
        hashed.tev[0].ind_mtx = kIndMtxOff;
        if (hash_tev(proc.state) == hash_tev(hashed)) {
            return fail("indirect hash");
        }
    }

    {
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.load_bp(0x49, 10u | (20u << 10));
        fifo.load_bp(0x4A, 99u | (49u << 10));
        fifo.load_bp(0x52, (6u << 3) | (1u << 9));
        fifo.trigger_efb_copy(true);
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.efb_copy_count != 1) {
            return fail("efb copy count");
        }
        if (proc.state.efb.x != 10 || proc.state.efb.y != 20 || proc.state.efb.w != 100 || proc.state.efb.h != 50 ||
            !proc.state.efb.to_xfb || !proc.state.efb.half_scale || !proc.state.efb.pending) {
            return fail("efb copy state");
        }
    }

    {
        std::uint8_t src[128];
        std::memset(src, 0, sizeof(src));
        encode_cmpr_solid(src, 8, 8, pack_rgb565(255, 0, 0));
        encode_cmpr_solid(src + 32, 4, 4, pack_rgb565(0, 255, 0));
        encode_cmpr_solid(src + 64, 2, 2, pack_rgb565(0, 0, 255));
        encode_cmpr_solid(src + 96, 1, 1, pack_rgb565(255, 255, 255));
        TexObj obj{};
        obj.data = src;
        obj.width = 8;
        obj.height = 8;
        obj.format = TexCMPR;
        obj.mips = true;
        ConvertedTexture host{};
        std::uint8_t dst[(64 + 16 + 4 + 1) * 4];
        if (!convert_gx_texture(obj, dst, sizeof(dst), &host) || host.mip_count != 4 || host.layout != HostRgba8 ||
            host.bytes != sizeof(dst)) {
            return fail("mip convert");
        }
        // Level 1 (4x4 green) follows level 0's 8x8 texels.
        if (dst[64 * 4 + 1] < 240 || dst[64 * 4] != 0) {
            return fail("mip level 1 texels");
        }
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.load_texobj(0, src, 8, 8, TexCMPR, WrapRepeat, WrapRepeat, true, 1, 1);
        fifo.load_bp(0x84, 0x08u | (0x40u << 8));
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.state.texmaps[0].min_filter != TexLinMipLin ||
            proc.state.texmaps[0].min_lod != 8 || proc.state.texmaps[0].max_lod != 0x40) {
            return fail("tex lod");
        }
    }

    {
        // IA8 is alpha in the first byte, intensity in the second (Aurora's TextureDecoderIA8,
        // Dolphin). One 4x4 block, every texel (a=0x40, i=0xC0).
        std::uint8_t src[32];
        for (unsigned i = 0; i < 16; ++i) {
            src[i * 2] = 0x40;
            src[i * 2 + 1] = 0xC0;
        }
        std::uint8_t dst[64];
        if (!tiled_to_rgba8(src, sizeof(src), 4, 4, TexIA8, dst, sizeof(dst)) || dst[0] != 0xC0 || dst[1] != 0xC0 ||
            dst[2] != 0xC0 || dst[3] != 0x40) {
            return fail("ia8 byte order");
        }
        // Same order for an IA8 palette; an index past the palette is transparent, not a
        // failure of the whole texture.
        std::uint8_t pal[4] = {0x80, 0x10, 0xFF, 0x20};  // entry0 a=0x80 i=0x10, entry1 a=0xFF i=0x20
        TlutObj tlut{};
        tlut.data = pal;
        tlut.format = TlutIa8;
        tlut.entries = 2;
        std::uint8_t ci8[32];
        std::memset(ci8, 0, sizeof(ci8));
        ci8[1] = 1;
        ci8[2] = 7;
        std::uint8_t out[8 * 4 * 4];
        if (!tiled_to_rgba8(ci8, sizeof(ci8), 8, 4, TexCI8, out, sizeof(out), &tlut)) {
            return fail("ci8 out-of-range index failed the texture");
        }
        if (out[0] != 0x10 || out[3] != 0x80 || out[4] != 0x20 || out[7] != 0xFF || out[8] != 0 || out[11] != 0) {
            return fail("ci8 ia8 palette");
        }
    }

    {
        // The per-frame reset keeps the register shadows. Stage 0's konst selector is set in
        // "frame 1"; "frame 2" sets only stage 1's, which shares BP 0xF6 with stage 0. A full
        // reset between them would re-zero stage 0's field in the shadow.
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        fifo.reset();
        fifo.set_tev_kcolor_sel(0, 0x0C);
        if (!proc.process(fifo.data(), fifo.size(), true)) {
            return fail("ksel frame 1");
        }
        fifo_reset_buffer_only(fifo);
        if (fifo.size() != 0) {
            return fail("buffer-only reset left bytes");
        }
        fifo.set_tev_kcolor_sel(1, 0x0D);
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.state.tev[0].kcsel != 0x0C ||
            proc.state.tev[1].kcsel != 0x0D) {
            return fail("ksel shadow lost across the per-frame reset");
        }
    }

    {
        // GXSetTexCoordScaleManually: coordinate 0 scaled by a 16x8 SU size while it samples a
        // 64x32 texture -> the normalised coordinate is (s * 16/64, t * 8/32).
        static std::uint8_t texels[64 * 32 * 4];
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        auto proc_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& proc = *proc_owner;
        proc.reset();
        configure_pos_clr0_tex0_direct(fifo, VtxFmt0);
        fifo.set_gen_mode(1, 1, 1);
        fifo.set_tex_coord_gen(0, TgMtx2x4, TgTex0, kTexMtxIdentity);
        fifo.load_texobj(0, texels, 64, 32, TexRGBA8);
        fifo.load_bp(0x30, (16u - 1u) | (1u << 20));
        fifo.load_bp(0x31, (8u - 1u) | (1u << 20));
        std::uint8_t vert[24];
        pack_pos_clr(vert, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255);
        write_be_f32(vert + 16, 1.0f);
        write_be_f32(vert + 20, 2.0f);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        if (!proc.process(fifo.data(), fifo.size(), true) || proc.vert_count != 1) {
            return fail("manual su process");
        }
        if (!near(proc.verts[0].tc[0][0], 0.25f) || !near(proc.verts[0].tc[0][1], 0.5f)) {
            std::fprintf(stderr, "gx_fifo: manual su tc=%f,%f\n", proc.verts[0].tc[0][0], proc.verts[0].tc[0][1]);
            return fail("manual texcoord scale");
        }
        // Without the marker (automatic scale) the coordinate is untouched.
        fifo.reset();
        fifo.load_bp(0x30, 0);
        fifo.load_bp(0x31, 0);
        fifo.draw(Triangles, VtxFmt0, 1, vert, sizeof(vert));
        proc.vert_count = 0;
        proc.draw_count = 0;
        if (!proc.process(fifo.data(), fifo.size(), true) || !near(proc.verts[0].tc[0][0], 1.0f) ||
            !near(proc.verts[0].tc[0][1], 2.0f)) {
            return fail("automatic texcoord scale must be identity");
        }
    }

    {
        // A frame larger than the FIFO buffer: with the full-buffer handler installed, the
        // complete commands are decoded as the buffer fills and nothing is lost. Before, every
        // write past 64 KiB was dropped for the rest of the frame.
        static Processor* s_proc = nullptr;
        static std::uint32_t s_flushed_draws = 0;
        static std::uint32_t s_flushed_verts = 0;
        s_flushed_draws = 0;
        s_flushed_verts = 0;
        set_batch_flush([](Processor& p) {
            s_flushed_draws += p.draw_count;
            s_flushed_verts += p.vert_count;
        });
        auto proc_owner = std::make_unique<Processor>();
        Processor& proc = *proc_owner;
        proc.reset();
        s_proc = &proc;
        set_fifo_full_handler([](Fifo&, const std::uint8_t* data, std::uint32_t size) -> std::uint32_t {
            return process_prefix(*s_proc, data, size, true);
        });
        auto fifo_owner = std::make_unique<Fifo>();
        Fifo& fifo = *fifo_owner;
        fifo.reset();
        configure_pos_clr0_direct(fifo, VtxFmt0);
        std::uint8_t tri[48];
        pack_pos_clr(tri, 0.0f, 0.0f, 0.0f, 1, 2, 3, 4);
        pack_pos_clr(tri + 16, 1.0f, 0.0f, 0.0f, 1, 2, 3, 4);
        pack_pos_clr(tri + 32, 0.0f, 1.0f, 0.0f, 1, 2, 3, 4);
        // 900 x (51-byte draw + 5-byte BP) is ~50 KiB per 900; 2000 of them is ~110 KiB.
        unsigned written = 0;
        for (unsigned i = 0; i < 2000; ++i) {
            if (!fifo.load_bp(0xF3, i & 0xFFu) || !fifo.draw(Triangles, VtxFmt0, 3, tri, sizeof(tri))) {
                break;
            }
            ++written;
        }
        set_fifo_full_handler(nullptr);
        if (written != 2000 || fifo.overflow()) {
            std::fprintf(stderr, "gx_fifo: wrote %u of 2000 draws, overflow=%d\n", written, fifo.overflow() ? 1 : 0);
            return fail("a frame larger than the FIFO buffer must not overflow");
        }
        const bool tail_ok = proc.process(fifo.data(), fifo.size(), true);
        set_batch_flush(nullptr);
        if (!tail_ok) {
            return fail("the tail left in the FIFO must decode");
        }
        if (s_flushed_draws + proc.draw_count != 2000 || s_flushed_verts + proc.vert_count != 6000) {
            std::fprintf(stderr, "gx_fifo: draws=%u+%u verts=%u+%u\n", s_flushed_draws, proc.draw_count,
                         s_flushed_verts, proc.vert_count);
            return fail("every draw of an oversized frame must reach the processor");
        }
    }

    {
        // Race post-processing copies frequently request clear=true with colour
        // writes disabled. Keep each copy's masks despite subsequent BP changes.
        Fifo fifo;
        Processor proc;
        proc.reset();
        TexCopySetup setup;
        setup.src_w = setup.dst_w = 4;
        setup.src_h = setup.dst_h = 4;
        std::uint8_t dest[64]{};
        fifo.load_bp(0x43, 1); // RGBA6: alpha exists
        fifo.load_bp(0x41, 1u << 4); // alpha only
        fifo.load_bp(0x40, 0); // no depth writes
        fifo.load_bp(0x51, 0x123456);
        write_efb_copy_escape(fifo, setup, true, 0x80001000, dest);
        fifo.load_bp(0x41, 1u << 3); // colour only
        fifo.load_bp(0x40, 1u << 4);
        write_efb_copy_escape(fifo, setup, true, 0x80002000, dest);
        fifo.load_bp(0x43, 0); // RGB8: alpha must be preserved
        fifo.load_bp(0x41, 1u << 4);
        write_efb_copy_escape(fifo, setup, true, 0x80003000, dest);
        write_efb_copy_escape(fifo, setup, false, 0x80004000, dest);
        if (!proc.process(fifo.data(), fifo.size(), true)) return fail("copy mask stream failed");
        const EfbCopyEvent* copies = nullptr;
        if (efb_copy_events(&copies) != 4 || copies[0].clear_color || !copies[0].clear_alpha ||
            copies[0].clear_depth || copies[0].clear_z != 0x123456 ||
            !copies[1].clear_color || copies[1].clear_alpha || !copies[1].clear_depth ||
            copies[2].clear_alpha || copies[3].clear_color || copies[3].clear_alpha || copies[3].clear_depth)
            return fail("copy must freeze channel masks, pixel format and depth at its FIFO position");
    }

    return 0;
}
