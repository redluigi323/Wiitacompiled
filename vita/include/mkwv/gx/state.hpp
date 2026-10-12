#pragma once

#include "mkwv/gx/texture.hpp"
#include "mkwv/gx/types.hpp"

#include <cstdint>

namespace mkwv::gx {

struct AttrFormat {
    CompCnt cnt = PosXYZ;
    CompType type = F32;
    std::uint8_t frac = 0;
};

struct VtxFormat {
    AttrFormat attrs[AttrCount]{};
};

struct TevStage {
    std::uint8_t color_a = CcZero;
    std::uint8_t color_b = CcZero;
    std::uint8_t color_c = CcZero;
    std::uint8_t color_d = CcTexC;
    std::uint8_t alpha_a = CaZero;
    std::uint8_t alpha_b = CaZero;
    std::uint8_t alpha_c = CaZero;
    std::uint8_t alpha_d = CaTexA;
    std::uint8_t color_bias = 0;
    std::uint8_t color_op = 0;
    std::uint8_t color_shift = 0;
    std::uint8_t color_dest = 0;
    bool color_clamp = true;
    std::uint8_t alpha_bias = 0;
    std::uint8_t alpha_op = 0;
    std::uint8_t alpha_shift = 0;
    std::uint8_t alpha_dest = 0;
    bool alpha_clamp = true;
    std::uint8_t kcsel = 0;
    std::uint8_t kasel = 0;
    std::uint8_t ras_swap = 0;
    std::uint8_t tex_swap = 0;
    std::uint8_t texmap = 0;
    std::uint8_t texcoord = 0;
    std::uint8_t ras_chan = kRasColor0;
    bool tex_enabled = true;
    std::uint8_t ind_stage = 0;
    std::uint8_t ind_fmt = IndFmt8;
    std::uint8_t ind_bias = 0;
    std::uint8_t ind_mtx = kIndMtxOff;
    std::uint8_t ind_wrap_s = IndWrapOff;
    std::uint8_t ind_wrap_t = IndWrapOff;
    bool ind_add_prev = false;
    std::uint8_t ind_alpha_sel = 0;  // BP 0x10+ bits 7-8: which texcoord bits feed ALPHA_BUMP
    bool ind_utc_lod = false;        // bit 19: unmodified texcoord for LOD
};

struct IndStage {
    std::uint8_t texmap = 0;
    std::uint8_t texcoord = 0;
    std::uint8_t scale_s = 0;
    std::uint8_t scale_t = 0;
};

struct IndMatrix {
    float m[6]{};
    std::int8_t scale_exp = 0;
    std::uint8_t raw_scale = 17;
};

struct EfbCopy {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
    std::uint32_t format = TexRGBA8;
    bool to_xfb = false;
    bool half_scale = false;
    bool pending = false;
};

struct FogState {
    std::uint8_t type = FogNone;
    std::uint32_t color = 0;
    float a = 0.0f;
    float b = 0.5f;
    float c = 0.0f;
    std::uint32_t raw0 = 0;
    std::uint32_t raw1 = 0;
    std::uint32_t raw2 = 0;
};

struct TexGen {
    TexGenType type = TgMtx2x4;
    TexGenSrc src = TgTex0;
    std::uint8_t mtx = kTexMtxIdentity;
    std::uint8_t post_mtx = kPtTexMtxIdentity;
    bool normalize = false;
    bool form_ab11 = true;
};

struct ChanCtrl {
    bool lighting = false;
    ColorSrc mat_src = SrcVtx;
    ColorSrc amb_src = SrcReg;
    DiffuseFn diff_fn = DfNone;
    AttnFn attn_fn = AfNone;
    std::uint8_t light_mask = 0;
};

struct Light {
    std::uint32_t color = 0;
    float pos[3]{};
    float dir[3]{0.0f, 0.0f, 1.0f};
    float cos_att[3]{1.0f, 0.0f, 0.0f};
    float dist_att[3]{1.0f, 0.0f, 0.0f};
};

struct AttrArray {
    const std::uint8_t* data = nullptr;
    std::uint32_t size = 0;
    std::uint8_t stride = 0;
    bool le = false;
    // Guest vertex array programmed through CP 0xA0+ (GXSetArray): the effective address of
    // element 0. Elements are fetched from guest memory through the resolver when `data` is
    // null (no host-side escape array).
    std::uint32_t guest_base = 0;
};

// One TLUT resident in TMEM, loaded by BP 0x64/0x65 (GXLoadTlut). The palette bytes are copied
// into the decoder's TMEM image at load time, as the hardware does.
struct TmemTlut {
    std::uint16_t lines = 0;     // 16-entry lines loaded
    std::uint32_t version = 0;   // load sequence number, so a reload of the same slot is new data
};

struct State {
    AttrType vtx_desc[AttrCount]{};
    VtxFormat vtx_fmts[kVtxFmtCount]{};
    AttrArray arrays[AttrCount]{};
    AttrArray xf_arrays[kMaxXfArrays]{};
    TexObj texmaps[kMaxTexMaps]{};
    TlutObj tluts[kMaxTluts]{};
    float pn_mtx[kMaxPnMtx][12]{};
    float nrm_mtx[kMaxPnMtx][9]{};
    float xf_mem[kXfMemSize]{};
    float xf_dtt[kXfDttSize]{};
    float xf_proj_raw[6]{};
    float proj[4][4]{};
    float viewport[6]{};
    std::uint32_t bp[256]{};
    std::uint32_t material = pack_rgba(255, 255, 255, 255);
    std::uint32_t ambient[2]{};
    std::uint32_t chan_mat[2]{};
    TevStage tev[kTevStages]{};
    IndStage ind[kMaxIndStages]{};
    IndMatrix ind_mtx[kMaxIndMtx]{};
    // Four registers, indexed the way GX indexes them: 0 is GX_TEVPREV (the output
    // register, which GXSetTevColor can preset and GX_CC_CPREV reads), and 1/2/3 are
    // GX_TEVREG0/1/2. This was [3], so GX_TEVREG2 - BP register 0xE6/0xE7 - was decoded
    // and then thrown away, and everything downstream was reading one register too low.
    std::uint32_t tev_color[4]{};
    // The same four registers at full precision: GX TEV colour registers are signed 11-bit
    // (GXSetTevColorS10), which tev_color above clamps to 0..255.
    std::int16_t tev_reg[4][4]{};
    std::uint32_t konst[4]{};
    std::uint8_t tev_swap[4][4]{};
    TexGen texgens[kMaxTexGens]{};
    ChanCtrl chans[4]{};
    Light lights[kMaxLights]{};
    std::uint8_t tex_mtx_id[kMaxTexGens]{};
    std::uint32_t mat_idx_a = 0;
    std::uint32_t mat_idx_b = 0;
    std::uint8_t current_pn_mtx = 0;
    std::uint8_t num_tev_stages = 1;
    std::uint8_t num_chans = 1;
    std::uint8_t num_tex_gens = 0;
    std::uint8_t num_ind_stages = 0;
    std::uint8_t line_width = 6;
    std::uint8_t point_size = 6;
    EfbCopy efb{};
    ProjectionType proj_type = Orthographic;
    Compare depth_func = Always;
    CullMode cull_mode = CullNone;
    BlendMode blend_mode = BlendNone;
    BlendFactor blend_src = BlSrcAlpha;
    BlendFactor blend_dst = BlInvSrcAlpha;
    LogicOp logic_op = LoCopy;
    std::uint8_t alpha_ref0 = 0;
    std::uint8_t alpha_ref1 = 0;
    Compare alpha_comp0 = Always;
    Compare alpha_comp1 = Always;
    std::uint8_t alpha_op = 0;
    FogState fog{};
    float render_vp[6]{};
    std::int32_t render_scissor[4]{};
    std::int32_t scissor_x = 0;
    std::int32_t scissor_y = 0;
    std::int32_t scissor_w = 0;
    std::int32_t scissor_h = 0;
    bool has_render_vp = false;
    bool has_render_scissor = false;
    bool has_scissor = false;
    bool depth_test = false;
    bool depth_write = false;
    bool color_update = true;
    bool alpha_update = true;

    // XF 0x1012: dual texture transform (post matrices + normalize). RVL GXInit enables it.
    bool dual_tex = true;
    // BP 0x43 PE control and BP 0x42 destination alpha.
    std::uint8_t pixel_fmt = 0;  // 0 RGB8_Z24, 1 RGBA6_Z24, 2 RGB565_Z16, ...
    std::uint8_t z_fmt = 0;
    bool zcomploc = true;
    bool dst_alpha_enable = false;
    std::uint8_t dst_alpha = 0;
    bool dither = false;
    // SU texcoord scale registers (BP 0x30+2n/0x31+2n). `su_manual` is set only by the HLE's
    // GXSetTexCoordScaleManually (bit 20 marker); otherwise the scale is the texture size.
    bool su_manual[kMaxTexGens]{};
    std::uint16_t su_size_s[kMaxTexGens]{};
    std::uint16_t su_size_t[kMaxTexGens]{};
    // Texture-cache invalidation counter (BP 0x66, GXInvalidateTexAll). Stamped into each
    // TexObj's version so the renderer can re-validate cached conversions.
    std::uint32_t tex_epoch = 1;
    // TMEM TLUT slots, indexed by the 10-bit TMEM offset field (512-byte units).
    TmemTlut tmem_tlut[1024]{};
    std::uint32_t tlut_load_seq = 0;
    std::int32_t scissor_off_x = 0;
    std::int32_t scissor_off_y = 0;
    std::uint32_t clear_z = 0xFFFFFFu;
    std::uint8_t line_tex_offset = 0;
    std::uint8_t point_tex_offset = 0;

    void reset();
    void apply_projection();
    void identity_pn_mtx(unsigned index);
};

void identity_3x4(float m[12]);
void look_at_3x4(float m[12], float eye_x, float eye_y, float eye_z, float center_x, float center_y, float center_z,
                 float up_x, float up_y, float up_z);
void rotate_y_3x4(float m[12], float radians);
void mul_3x4(float out[12], const float a[12], const float b[12]);
void perspective_raw(float raw[6], float fov_y_deg, float aspect, float near_z, float far_z);

std::uint32_t bp_bits(std::uint32_t value, unsigned width, unsigned shift);

} // namespace mkwv::gx
