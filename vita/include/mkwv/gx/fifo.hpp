#pragma once

#include "mkwv/gx/state.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mkwv::gx {

inline constexpr std::uint32_t kFifoCapacity = 64u * 1024u;
// Sized for a real game frame, not the triangle demo these started at. With kMaxDraws = 64 a
// menu frame hit the cap 13869 bytes into a 22877-byte FIFO, and Processor::process() then
// broke out and silently discarded the remaining 39% of every frame.
inline constexpr std::uint32_t kMaxDecodedVerts = 16384;
inline constexpr std::uint32_t kMaxDraws = 1024;
// When either limit is reached mid-frame the Processor hands the batch it has so far to the
// flush callback (see draw_state.hpp: set_batch_flush) and carries on decoding - a race frame
// is far larger than one batch. Without a callback it stops, as it always did.

struct DecodedVertex {
    // Model-space position while decoding; GL clip space (x, y, z, w) after process().
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 1.0f;
    float bx = 1.0f;
    float by = 0.0f;
    float bz = 0.0f;
    float tx = 0.0f;
    float ty = 1.0f;
    float tz = 0.0f;
    // Eight texture coordinates. Raw attribute values (s, t, 1) while decoding; texgen
    // outputs (s, t, q) afterwards - q is 1 except for projective (3x4) texgens.
    float tc[kMaxTexGens][3]{};
    // Raw vertex colours while decoding; lit channel colours COLOR0A0/COLOR1A1 afterwards.
    std::uint32_t color = pack_rgba(255, 255, 255, 255);
    std::uint32_t color1 = pack_rgba(255, 255, 255, 255);
    std::uint8_t pn_mtx = 0;
    std::uint8_t tex_mtx[kMaxTexGens]{};
};

struct DrawCall {
    Primitive prim = Triangles;
    VtxFmt fmt = VtxFmt0;
    std::uint32_t first_vertex = 0;
    std::uint32_t vertex_count = 0;
    bool tex0 = false;
    TevOp tev = TevReplace;
    bool blend = false;
    BlendMode blend_mode = BlendNone;
    BlendFactor blend_src = BlOne;
    BlendFactor blend_dst = BlZero;
    LogicOp logic_op = LoCopy;
    std::uint64_t tev_key = 0;
    std::uint8_t num_tev_stages = 1;
    std::uint8_t num_tex_gens = 0;
    std::uint8_t num_chans = 0;
    TevStage tev_stages[kTevStages]{};
    std::uint32_t tev_color[4]{};  // GX_TEVPREV, then GX_TEVREG0/1/2 (clamped to 0..255)
    std::int16_t tev_reg[4][4]{};  // the same, signed 11-bit, RGBA
    std::uint32_t konst[4]{};
    std::uint8_t tev_swap[4][4]{};
    std::uint8_t alpha_ref0 = 0;
    std::uint8_t alpha_ref1 = 0;
    Compare alpha_comp0 = Always;
    Compare alpha_comp1 = Always;
    std::uint8_t alpha_op = 0;
    FogState fog{};
    bool dst_alpha_enable = false;
    std::uint8_t dst_alpha = 0;
    std::uint8_t pixel_fmt = 0;
    bool zcomploc = true;
};

class Fifo {
public:
    void reset();
    // Inline: the guest writes every GX command through these, a word at a time. Only a
    // full (or overflowed) buffer takes the out-of-line write_bytes path.
    bool write_u8(std::uint8_t value) {
        if (__builtin_expect(!overflow_ && size_ < kFifoCapacity, 1)) {
            buffer_[size_++] = value;
            return true;
        }
        return write_bytes(&value, 1);
    }
    bool write_u16(std::uint16_t value) {
        const std::uint16_t be = __builtin_bswap16(value);
        if (__builtin_expect(!overflow_ && size_ + 2u <= kFifoCapacity, 1)) {
            std::memcpy(buffer_ + size_, &be, 2);
            size_ += 2u;
            return true;
        }
        return write_bytes(&be, 2);
    }
    bool write_u32(std::uint32_t value) {
        const std::uint32_t be = __builtin_bswap32(value);
        if (__builtin_expect(!overflow_ && size_ + 4u <= kFifoCapacity, 1)) {
            std::memcpy(buffer_ + size_, &be, 4);
            size_ += 4u;
            return true;
        }
        return write_bytes(&be, 4);
    }
    bool write_u64(std::uint64_t value);
    bool write_f32(float value) {
        std::uint32_t bits;
        std::memcpy(&bits, &value, 4);
        return write_u32(bits);
    }
    bool write_bytes(const void* data, std::uint32_t bytes);

    bool nop();
    bool load_cp(std::uint8_t addr, std::uint32_t value);
    bool load_bp(std::uint8_t reg, std::uint32_t value24);
    bool load_xf(std::uint16_t addr, const std::uint32_t* words, std::uint32_t count);
    bool load_xf_f32(std::uint16_t addr, const float* values, std::uint32_t count);
    bool set_array(Attr attr, const void* data, std::uint32_t size, std::uint8_t stride, bool little_endian);
    bool load_texobj(unsigned texmap, const void* data, std::uint32_t width, std::uint32_t height, std::uint32_t format,
                     unsigned wrap_s = WrapRepeat, unsigned wrap_t = WrapRepeat, bool mips = false,
                     std::uint32_t id = 1, std::uint32_t version = 1, std::uint32_t tlut = 0);
    bool load_tlut(unsigned idx, const void* data, std::uint32_t format, std::uint16_t entries, std::uint32_t id = 1,
                   std::uint32_t version = 1);
    bool set_tev_op(unsigned stage, TevOp mode);
    bool set_gen_mode(std::uint8_t tex_gens, std::uint8_t chans, std::uint8_t tev_stages, std::uint8_t n_ind = 0);
    bool set_tex_coord_gen(unsigned dst, TexGenType type, TexGenSrc src, std::uint8_t mtx, bool normalize = false,
                           std::uint8_t post_mtx = kPtTexMtxIdentity);
    bool load_tex_mtx(std::uint8_t id, const float* values, unsigned count);
    bool set_chan_ctrl(unsigned chan, bool lighting, ColorSrc amb_src, ColorSrc mat_src, std::uint8_t light_mask,
                       DiffuseFn diff_fn, AttnFn attn_fn);
    bool set_chan_amb_color(unsigned chan, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255);
    bool set_chan_mat_color(unsigned chan, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255);
    bool load_light(unsigned id, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a, float px, float py,
                    float pz, float nx = 0.0f, float ny = 0.0f, float nz = 1.0f);
    bool set_blend_mode(BlendMode mode, BlendFactor src, BlendFactor dst, LogicOp logic = LoCopy);
    bool set_alpha_compare(Compare comp0, std::uint8_t ref0, std::uint8_t op, Compare comp1, std::uint8_t ref1);
    bool set_tev_color(unsigned id, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255);
    bool set_tev_kcolor(unsigned id, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255);
    bool set_tev_kcolor_sel(unsigned stage, std::uint8_t sel);
    bool set_tev_kalpha_sel(unsigned stage, std::uint8_t sel);
    bool set_tev_swap_table(unsigned id, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);
    bool set_tev_order(unsigned stage, std::uint8_t texcoord, std::uint8_t texmap, std::uint8_t ras_chan = kRasColor0);
    bool set_tev_color_op(unsigned stage, std::uint8_t op, std::uint8_t bias = 0, std::uint8_t shift = 0, bool clamp = true,
                          std::uint8_t dest = 0);
    // The raw per-stage combiner inputs. set_tev_op only writes the five coarse GXSetTevOp
    // presets; GXSetTevColorIn / GXSetTevAlphaIn program the four arguments directly, which
    // is what every real material does. Both halves of a stage share one BP register, so
    // these preserve the op/bias/shift/dest bits through tev_color_reg_ / tev_alpha_reg_.
    bool set_tev_color_in(unsigned stage, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d);
    bool set_tev_alpha_in(unsigned stage, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d);
    bool set_tev_alpha_op(unsigned stage, std::uint8_t op, std::uint8_t bias = 0, std::uint8_t shift = 0,
                          bool clamp = true, std::uint8_t dest = 0);
    bool set_tev_swap_mode(unsigned stage, std::uint8_t ras_sel, std::uint8_t tex_sel);
    bool set_fog(std::uint8_t type, float start_z, float end_z, float near_z, float far_z, std::uint8_t r, std::uint8_t g,
                 std::uint8_t b);
    bool set_viewport(float left, float top, float width, float height, float near_z, float far_z);
    bool set_scissor(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
    bool set_xf_array(unsigned kind, const void* data, std::uint32_t size, std::uint8_t stride);
    bool load_indx(std::uint8_t opcode, std::uint16_t index, std::uint8_t count, std::uint16_t dst);
    bool call_dl(std::uint32_t addr, std::uint32_t bytes);
    bool set_line_width(std::uint8_t line_width, std::uint8_t point_size);
    bool set_ind_order(unsigned stage, std::uint8_t texcoord, std::uint8_t texmap);
    bool set_ind_mtx(unsigned id, const float m[6], std::int8_t scale_exp);
    // BP 0x25 holds stages 0 and 1, 0x26 holds 2 and 3, four bits per scale. The decoder
    // already read these; nothing could write them until now.
    bool set_ind_tex_coord_scale(unsigned stage, std::uint8_t scale_s, std::uint8_t scale_t);
    bool set_tev_indirect(unsigned stage, std::uint8_t ind_stage, std::uint8_t fmt, std::uint8_t bias, std::uint8_t mtx,
                          std::uint8_t wrap_s = IndWrapOff, std::uint8_t wrap_t = IndWrapOff, bool add_prev = false);
    bool trigger_efb_copy(bool to_xfb);
    bool draw(Primitive prim, VtxFmt fmt, std::uint16_t count, const void* vertices, std::uint32_t vertex_bytes);
    // Truncates the buffer back to `size` bytes (display-list recording rewinds with this).
    void truncate(std::uint32_t size) { if (size < size_) { size_ = size; } }

    const std::uint8_t* data() const { return buffer_; }
    std::uint32_t size() const { return size_; }
    bool overflow() const { return overflow_; }

private:
    std::uint8_t buffer_[kFifoCapacity]{};
    std::uint32_t size_ = 0;
    std::uint32_t mat_idx_a_ = 0;
    std::uint32_t mat_idx_b_ = 0;
    std::uint32_t tev_ksel_[8]{};
    std::uint32_t tev_tref_[8]{};
    std::uint32_t tev_color_reg_[kTevStages]{};
    std::uint32_t tev_alpha_reg_[kTevStages]{};
    std::uint32_t ind_scale_reg_[2]{};  // BP 0x25 and 0x26, two indirect stages each
    bool overflow_ = false;
};

struct Processor {
    State state{};
    // After process(), positions are GX clip/NDC using the XF state at each draw.
    DecodedVertex verts[kMaxDecodedVerts]{};
    DrawCall draws[kMaxDraws]{};
    std::uint32_t vert_count = 0;
    std::uint32_t draw_count = 0;
    std::uint32_t error_count = 0;
    std::uint32_t skipped_indexed_xf = 0;
    std::uint32_t skipped_dl = 0;
    std::uint32_t skipped_aurora = 0;
    std::uint32_t efb_copy_count = 0;

    struct MemMap {
        std::uint32_t addr = 0;
        std::uint32_t size = 0;
        const std::uint8_t* host = nullptr;
    };
    MemMap maps[kMaxMemMaps]{};
    std::uint32_t map_count = 0;
    int dl_depth = 0;

    void reset();
    void end_frame();
    void map_range(std::uint32_t addr, const void* host, std::uint32_t size);
    const std::uint8_t* resolve(std::uint32_t addr, std::uint32_t size) const;
    bool process(const std::uint8_t* data, std::uint32_t size, bool big_endian);
};

std::uint32_t vertex_stride(const State& state, VtxFmt fmt);

void configure_pos_clr0_direct(Fifo& fifo, VtxFmt fmt);
void configure_pos_clr0_tex0_direct(Fifo& fifo, VtxFmt fmt);
void configure_pos_clr0_tex0_tex1_direct(Fifo& fifo, VtxFmt fmt);
void configure_pos_index16_clr0_index8(Fifo& fifo, VtxFmt fmt);
void configure_pos_index16_nrm_index8_clr0_index8(Fifo& fifo, VtxFmt fmt);

} // namespace mkwv::gx
