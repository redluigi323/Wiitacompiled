#include "mkwv/gx/state.hpp"

#include <cmath>
#include <cstring>

namespace mkwv::gx {
namespace {

void set_identity_4x4(float m[4][4]) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0][0] = 1.0f;
    m[1][1] = 1.0f;
    m[2][2] = 1.0f;
    m[3][3] = 1.0f;
}

} // namespace

std::uint32_t bp_bits(std::uint32_t value, unsigned width, unsigned shift) {
    if (width == 0 || width >= 32) {
        return (width == 0) ? 0u : (value >> shift);
    }
    return (value >> shift) & ((1u << width) - 1u);
}

void identity_3x4(float m[12]) {
    std::memset(m, 0, sizeof(float) * 12);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
}

void look_at_3x4(float m[12], float eye_x, float eye_y, float eye_z, float center_x, float center_y, float center_z,
                 float up_x, float up_y, float up_z) {
    float fx = center_x - eye_x;
    float fy = center_y - eye_y;
    float fz = center_z - eye_z;
    const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (flen < 1.0e-8f) {
        identity_3x4(m);
        return;
    }
    fx /= flen;
    fy /= flen;
    fz /= flen;

    float sx = fy * up_z - fz * up_y;
    float sy = fz * up_x - fx * up_z;
    float sz = fx * up_y - fy * up_x;
    const float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (slen < 1.0e-8f) {
        identity_3x4(m);
        return;
    }
    sx /= slen;
    sy /= slen;
    sz /= slen;

    const float ux = sy * fz - sz * fy;
    const float uy = sz * fx - sx * fz;
    const float uz = sx * fy - sy * fx;

    m[0] = sx;
    m[1] = sy;
    m[2] = sz;
    m[3] = -(sx * eye_x + sy * eye_y + sz * eye_z);
    m[4] = ux;
    m[5] = uy;
    m[6] = uz;
    m[7] = -(ux * eye_x + uy * eye_y + uz * eye_z);
    m[8] = -fx;
    m[9] = -fy;
    m[10] = -fz;
    m[11] = fx * eye_x + fy * eye_y + fz * eye_z;
}

void rotate_y_3x4(float m[12], float radians) {
    identity_3x4(m);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    m[0] = c;
    m[2] = s;
    m[8] = -s;
    m[10] = c;
}

void mul_3x4(float out[12], const float a[12], const float b[12]) {
    float tmp[12];
    for (int r = 0; r < 3; ++r) {
        tmp[r * 4 + 0] = a[r * 4 + 0] * b[0] + a[r * 4 + 1] * b[4] + a[r * 4 + 2] * b[8];
        tmp[r * 4 + 1] = a[r * 4 + 0] * b[1] + a[r * 4 + 1] * b[5] + a[r * 4 + 2] * b[9];
        tmp[r * 4 + 2] = a[r * 4 + 0] * b[2] + a[r * 4 + 1] * b[6] + a[r * 4 + 2] * b[10];
        tmp[r * 4 + 3] = a[r * 4 + 0] * b[3] + a[r * 4 + 1] * b[7] + a[r * 4 + 2] * b[11] + a[r * 4 + 3];
    }
    std::memcpy(out, tmp, sizeof(tmp));
}

void perspective_raw(float raw[6], float fov_y_deg, float aspect, float near_z, float far_z) {
    const float cot = 1.0f / std::tan(fov_y_deg * 0.5f * 3.14159265358979323846f / 180.0f);
    raw[0] = cot / aspect;
    raw[1] = 0.0f;
    raw[2] = cot;
    raw[3] = 0.0f;
    // GX clip space, not GXM's: the near plane maps to z = -1 and the far plane to z = 0.
    // This used to be written in GXM's 0..1 range, which made the demo render correctly while
    // every guest projection - which is a real GX matrix - landed outside GXM's clip volume.
    raw[4] = -near_z / (far_z - near_z);
    raw[5] = -(far_z * near_z) / (far_z - near_z);
}

void State::reset() {
    std::memset(this, 0, sizeof(*this));
    for (unsigned i = 0; i < kMaxPnMtx; ++i) {
        identity_pn_mtx(i);
        std::memcpy(xf_mem + i * 12u, pn_mtx[i], sizeof(float) * 12u);
        nrm_mtx[i][0] = 1.0f;
        nrm_mtx[i][4] = 1.0f;
        nrm_mtx[i][8] = 1.0f;
    }
    identity_3x4(xf_mem + static_cast<unsigned>(kTexMtxIdentity) * 4u);
    identity_3x4(xf_dtt + static_cast<unsigned>(kPtTexMtxIdentity - kPtTexMtx0) * 4u);
    // GX's power-on swap tables (Aurora gx.hpp): RGBA, RRRA, GGGA, BBBA.
    for (unsigned t = 0; t < 4u; ++t) {
        tev_swap[t][0] = static_cast<std::uint8_t>(t == 0 ? 0 : t - 1u);
        tev_swap[t][1] = static_cast<std::uint8_t>(t == 0 ? 1 : t - 1u);
        tev_swap[t][2] = static_cast<std::uint8_t>(t == 0 ? 2 : t - 1u);
        tev_swap[t][3] = 3;
    }
    for (auto& fmt : vtx_fmts) {
        fmt.attrs[AttrPos].cnt = PosXYZ;
        fmt.attrs[AttrPos].type = F32;
        fmt.attrs[AttrPos].frac = 0;
        fmt.attrs[AttrNrm].cnt = NrmXYZ;
        fmt.attrs[AttrNrm].type = F32;
        fmt.attrs[AttrClr0].cnt = ClrRGBA;
        fmt.attrs[AttrClr0].type = RGBA8;
        fmt.attrs[AttrClr1].cnt = ClrRGBA;
        fmt.attrs[AttrClr1].type = RGBA8;
        for (int t = AttrTex0; t <= AttrTex7; ++t) {
            fmt.attrs[t].cnt = TexST;
            fmt.attrs[t].type = F32;
        }
    }
    xf_proj_raw[0] = 1.0f;
    xf_proj_raw[2] = 1.0f;
    xf_proj_raw[4] = 1.0f;
    proj_type = Orthographic;
    apply_projection();
    material = pack_rgba(255, 255, 255, 255);
    chan_mat[0] = material;
    chan_mat[1] = material;
    num_tev_stages = 1;
    num_chans = 1;
    for (auto& gen : texgens) {
        gen = TexGen{};
    }
    for (auto& id : tex_mtx_id) {
        id = kTexMtxIdentity;
    }
    for (auto& chan : chans) {
        chan = ChanCtrl{};
    }
    for (auto& light : lights) {
        light = Light{};
    }
    depth_func = Always;
    cull_mode = CullNone;
    color_update = true;
    alpha_update = true;
    blend_mode = BlendNone;
    blend_src = BlSrcAlpha;
    blend_dst = BlInvSrcAlpha;
    logic_op = LoCopy;
    line_width = 6;
    point_size = 6;
    num_ind_stages = 0;
    for (auto& mtx : ind_mtx) {
        mtx = IndMatrix{};
    }
    for (auto& stage : ind) {
        stage = IndStage{};
    }
    efb = EfbCopy{};
    alpha_comp0 = Always;
    alpha_comp1 = Always;
    alpha_ref0 = 0;
    alpha_ref1 = 0;
    alpha_op = 0;
    tev_color[0] = 0;
    tev_color[1] = 0;
    tev_color[2] = 0;
    konst[0] = pack_rgba(255, 255, 255, 255);
    konst[1] = pack_rgba(255, 255, 255, 255);
    konst[2] = pack_rgba(255, 255, 255, 255);
    konst[3] = pack_rgba(255, 255, 255, 255);
    bp[0xFE] = 0x00FFFFFFu;
    dual_tex = true;
    zcomploc = true;
    tex_epoch = 1;
    clear_z = 0xFFFFFFu;
    for (unsigned i = 0; i < 4u; ++i) {
        for (unsigned c = 0; c < 4u; ++c) {
            tev_reg[i][c] = 0;
        }
    }
    for (auto& tm : texmaps) {
        tm.tlut_tmem = 0xFFFFu;
    }
    fog = FogState{};
    for (auto& s : tev) {
        s = TevStage{};
    }
}

void State::identity_pn_mtx(unsigned index) {
    if (index >= kMaxPnMtx) {
        return;
    }
    identity_3x4(pn_mtx[index]);
}

void State::apply_projection() {
    set_identity_4x4(proj);
    proj[0][0] = xf_proj_raw[0];
    proj[1][1] = xf_proj_raw[2];
    proj[2][2] = xf_proj_raw[4];
    proj[2][3] = xf_proj_raw[5];
    if (proj_type == Orthographic) {
        proj[0][3] = xf_proj_raw[1];
        proj[1][3] = xf_proj_raw[3];
        proj[3][3] = 1.0f;
        proj[3][2] = 0.0f;
    } else {
        proj[0][2] = xf_proj_raw[1];
        proj[1][2] = xf_proj_raw[3];
        proj[3][2] = -1.0f;
        proj[3][3] = 0.0f;
    }
}

} // namespace mkwv::gx
