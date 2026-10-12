#include "mkwv/gx/demo.hpp"
#include "mkwv/gx/texture.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace mkwv::gx {
namespace {

struct PackedVert {
    float x, y, z;
    std::uint8_t r, g, b, a;
    float s = 0.0f;
    float t = 0.0f;
    bool uv = false;
};

void pack(PackedVert* out, float x, float y, float z, std::uint8_t r, std::uint8_t g, std::uint8_t b,
          std::uint8_t a = 255) {
    out->x = x;
    out->y = y;
    out->z = z;
    out->r = r;
    out->g = g;
    out->b = b;
    out->a = a;
    out->s = 0.0f;
    out->t = 0.0f;
    out->uv = false;
}

void pack_uv(PackedVert* out, float x, float y, float z, float s, float t, std::uint8_t r, std::uint8_t g,
             std::uint8_t b, std::uint8_t a = 255) {
    pack(out, x, y, z, r, g, b, a);
    out->s = s;
    out->t = t;
    out->uv = true;
}

void write_be_f32(std::uint8_t*& p, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    *p++ = static_cast<std::uint8_t>(bits >> 24);
    *p++ = static_cast<std::uint8_t>(bits >> 16);
    *p++ = static_cast<std::uint8_t>(bits >> 8);
    *p++ = static_cast<std::uint8_t>(bits);
}

bool emit_draw(Fifo& fifo, Primitive prim, const PackedVert* verts, std::uint16_t count) {
    std::uint8_t bytes[24 * 16];
    if (verts == nullptr || count == 0 || count > 16) {
        return false;
    }
    const bool uv = verts[0].uv;
    std::uint8_t* p = bytes;
    for (std::uint16_t i = 0; i < count; ++i) {
        const PackedVert& v = verts[i];
        write_be_f32(p, v.x);
        write_be_f32(p, v.y);
        write_be_f32(p, v.z);
        *p++ = v.r;
        *p++ = v.g;
        *p++ = v.b;
        *p++ = v.a;
        if (uv) {
            write_be_f32(p, v.s);
            write_be_f32(p, v.t);
        }
    }
    return fifo.draw(prim, VtxFmt0, count, bytes, static_cast<std::uint32_t>(p - bytes));
}

struct IndexedTri {
    std::uint8_t pos[36]{};
    std::uint8_t nrm[12]{};
    std::uint8_t clr[12]{};
};

struct DemoTextures {
    std::uint8_t asphalt[2048]{};
};

IndexedTri& demo_indexed_tri() {
    static IndexedTri tri;
    static bool ready = false;
    if (!ready) {
        std::uint8_t* p = tri.pos;
        write_be_f32(p, 0.0f);
        write_be_f32(p, 2.6f);
        write_be_f32(p, 0.0f);
        write_be_f32(p, -2.1f);
        write_be_f32(p, 0.2f);
        write_be_f32(p, 0.8f);
        write_be_f32(p, 2.1f);
        write_be_f32(p, 0.2f);
        write_be_f32(p, 0.8f);
        std::uint8_t* n = tri.nrm;
        write_be_f32(n, 0.0f);
        write_be_f32(n, 0.0f);
        write_be_f32(n, 1.0f);
        tri.clr[0] = 0x59;
        tri.clr[1] = 0x51;
        tri.clr[2] = 0xB8;
        tri.clr[3] = 255;
        tri.clr[4] = 0xF2;
        tri.clr[5] = 0xC7;
        tri.clr[6] = 0x2E;
        tri.clr[7] = 255;
        tri.clr[8] = 0x32;
        tri.clr[9] = 0xD9;
        tri.clr[10] = 0x8C;
        tri.clr[11] = 255;
        ready = true;
    }
    return tri;
}

DemoTextures& demo_textures() {
    static DemoTextures textures;
    static bool ready = false;
    if (!ready) {
        encode_cmpr_asphalt(textures.asphalt, 64);
        ready = true;
    }
    return textures;
}

} // namespace

bool build_demo_frame(Fifo& fifo, float time_seconds) {
    fifo.reset();
    fifo.load_bp(0xFE, 0x00FFFFFFu);
    fifo.set_gen_mode(1, 1, 2);
    fifo.load_bp(0x40, 1u | (static_cast<std::uint32_t>(LEqual) << 1) | (1u << 4));
    fifo.load_bp(0x41, 1u << 3);
    fifo.set_tev_op(0, TevPassClr);
    fifo.set_tev_op(1, TevModulate);

    float view[12];
    look_at_3x4(view, 0.0f, 5.8f, 16.5f, 0.0f, 0.4f, -1.5f, 0.0f, 1.0f, 0.0f);
    fifo.load_xf_f32(0x0000, view, 12);

    float proj[6];
    perspective_raw(proj, 48.0f, 960.0f / 544.0f, 0.6f, 80.0f);
    fifo.load_xf_f32(0x1020, proj, 6);
    const std::uint32_t persp = Perspective;
    fifo.load_xf(0x1026, &persp, 1);

    DemoTextures& textures = demo_textures();
    fifo.load_texobj(0, textures.asphalt, 64, 64, TexCMPR, WrapRepeat, WrapRepeat, false, 1, 1);

    const float ground_mtx[8] = {0.25f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, -0.25f, 3.0f};
    fifo.load_tex_mtx(kTexMtx0, ground_mtx, 8);
    fifo.set_tex_coord_gen(0, TgMtx2x4, TgPos, kTexMtx0);

    configure_pos_clr0_direct(fifo, VtxFmt0);
    PackedVert ground[4];
    pack(&ground[0], -16.0f, 0.0f, 12.0f, 255, 250, 240);
    pack(&ground[1], 16.0f, 0.0f, 12.0f, 255, 250, 240);
    pack(&ground[2], 16.0f, 0.0f, -18.0f, 186, 190, 204);
    pack(&ground[3], -16.0f, 0.0f, -18.0f, 186, 190, 204);
    if (!emit_draw(fifo, Quads, ground, 4)) {
        return false;
    }

    fifo.set_tex_coord_gen(0, TgMtx2x4, TgTex0, kTexMtxIdentity);
    configure_pos_clr0_tex0_direct(fifo, VtxFmt0);
    PackedVert track[4];
    pack_uv(&track[0], -4.4f, 0.02f, 10.0f, 0.0f, 0.0f, 235, 235, 240);
    pack_uv(&track[1], 4.4f, 0.02f, 10.0f, 2.2f, 0.0f, 235, 235, 240);
    pack_uv(&track[2], 3.2f, 0.02f, -14.0f, 2.2f, 6.0f, 200, 200, 210);
    pack_uv(&track[3], -3.2f, 0.02f, -14.0f, 0.0f, 6.0f, 200, 200, 210);
    if (!emit_draw(fifo, Quads, track, 4)) {
        return false;
    }

    fifo.set_gen_mode(0, 1, 1);
    fifo.set_tev_op(0, TevPassClr);
    configure_pos_clr0_direct(fifo, VtxFmt0);

    PackedVert curb[6];
    pack(&curb[0], -4.6f, 0.0f, 6.0f, 196, 48, 52);
    pack(&curb[1], -4.2f, 0.0f, 6.0f, 236, 236, 240);
    pack(&curb[2], -4.6f, 0.55f, 2.0f, 196, 48, 52);
    pack(&curb[3], -4.2f, 0.55f, 2.0f, 236, 236, 240);
    pack(&curb[4], -4.4f, 0.0f, -2.0f, 196, 48, 52);
    pack(&curb[5], -4.0f, 0.0f, -2.0f, 236, 236, 240);
    if (!emit_draw(fifo, TriangleStrip, curb, 6)) {
        return false;
    }

    fifo.set_line_width(18, 18);
    PackedVert finish[2];
    pack(&finish[0], -4.4f, 0.08f, 8.2f, 255, 236, 72);
    pack(&finish[1], 4.4f, 0.08f, 8.2f, 255, 236, 72);
    if (!emit_draw(fifo, Lines, finish, 2)) {
        return false;
    }

    fifo.set_blend_mode(BlendMix, BlSrcAlpha, BlInvSrcAlpha);

    PackedVert coin[10];
    const float cx = 3.4f;
    const float cz = 2.5f;
    const float cy = 0.05f;
    const float radius = 1.15f;
    pack(&coin[0], cx, cy, cz, 255, 210, 64, 140);
    for (int i = 0; i < 9; ++i) {
        const float a = static_cast<float>(i) * 6.28318530718f / 8.0f;
        const bool alt = (i & 1) != 0;
        pack(&coin[i + 1], cx + radius * std::cos(a), cy, cz + radius * std::sin(a), 255,
             static_cast<std::uint8_t>(alt ? 210 : 186), static_cast<std::uint8_t>(alt ? 96 : 48), 140);
    }
    if (!emit_draw(fifo, TriangleFan, coin, 10)) {
        return false;
    }

    fifo.set_blend_mode(BlendNone, BlSrcAlpha, BlInvSrcAlpha);

    fifo.set_chan_amb_color(0, 48, 52, 72);
    fifo.set_chan_mat_color(0, 255, 255, 255);
    fifo.set_chan_ctrl(0, true, SrcReg, SrcVtx, 0x01, DfClamp, AfNone);
    fifo.load_light(0, 200, 210, 255, 255, 6.0f, 14.0f, 10.0f, 0.3f, 0.8f, 0.5f);

    float rot[12];
    float model[12];
    rotate_y_3x4(rot, time_seconds * 1.35f);
    mul_3x4(model, view, rot);
    fifo.load_xf_f32(0x0000, model, 12);

    IndexedTri& tri = demo_indexed_tri();
    configure_pos_index16_nrm_index8_clr0_index8(fifo, VtxFmt0);
    fifo.set_array(AttrPos, tri.pos, sizeof(tri.pos), 12, false);
    fifo.set_array(AttrNrm, tri.nrm, sizeof(tri.nrm), 12, false);
    fifo.set_array(AttrClr0, tri.clr, sizeof(tri.clr), 4, false);
    const std::uint8_t idx[] = {0x00, 0x00, 0, 0, 0x00, 0x01, 0, 1, 0x00, 0x02, 0, 2};
    return fifo.draw(Triangles, VtxFmt0, 3, idx, 12) && !fifo.overflow();
}

} // namespace mkwv::gx
