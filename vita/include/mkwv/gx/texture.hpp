#pragma once

#include "mkwv/gx/types.hpp"

#include <cstdint>

namespace mkwv::gx {

struct TexObj {
    const std::uint8_t* data = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::uint32_t tlut = 0;
    std::uint32_t id = 0;
    std::uint32_t version = 0;
    std::uint8_t wrap_s = WrapClamp;
    std::uint8_t wrap_t = WrapClamp;
    std::uint8_t min_filter = TexLinear;
    std::uint8_t mag_filter = TexLinear;
    std::int8_t lod_bias = 0;
    std::uint8_t min_lod = 0;
    std::uint8_t max_lod = 0;
    bool mips = false;
    // Hardware texture path (BP image3/TLUT registers): the physical image address, and for
    // colour-indexed formats the TMEM TLUT the map was pointed at (BP 0x98+/0xB8+).
    std::uint32_t phys = 0;
    std::uint16_t tlut_tmem = 0xFFFFu;  // TMEM offset field (512-byte units); 0xFFFF = none
    std::uint8_t tlut_fmt = 0;
    std::uint8_t max_aniso = 0;
    bool edge_lod = false;
};

struct TlutObj {
    const std::uint8_t* data = nullptr;
    std::uint32_t format = TlutRgb5a3;
    std::uint16_t entries = 0;
    std::uint32_t id = 0;
    std::uint32_t version = 0;
};

enum HostTexLayout : std::uint8_t {
    HostRgba8 = 0,
    HostDxt1 = 1,
};

struct ConvertedTexture {
    const std::uint8_t* data = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bytes = 0;
    HostTexLayout layout = HostRgba8;
    unsigned mip_count = 1;
};

std::uint32_t tex_buffer_size(std::uint32_t width, std::uint32_t height, std::uint32_t format);
std::uint32_t dxt1_buffer_size(std::uint32_t width, std::uint32_t height);

bool cmpr_to_rgba8(const std::uint8_t* src, std::uint32_t src_bytes, std::uint32_t width, std::uint32_t height,
                   std::uint8_t* dst, std::uint32_t dst_bytes);
bool cmpr_to_dxt1(const std::uint8_t* src, std::uint32_t src_bytes, std::uint32_t width, std::uint32_t height,
                  std::uint8_t* dst, std::uint32_t dst_bytes);
bool dxt1_to_rgba8(const std::uint8_t* src, std::uint32_t src_bytes, std::uint32_t width, std::uint32_t height,
                   std::uint8_t* dst, std::uint32_t dst_bytes);
bool tiled_to_rgba8(const std::uint8_t* src, std::uint32_t src_bytes, std::uint32_t width, std::uint32_t height,
                    std::uint32_t format, std::uint8_t* dst, std::uint32_t dst_bytes, const TlutObj* tlut = nullptr);

// Level count GX would sample for this texobj: 1 without mips, else min(max_lod + 1, full chain).
unsigned texture_level_count(const TexObj& obj);
// The format a depth texture format is sampled as (Z8 -> I8, Z16 -> IA8, Z24X8 -> RGBA8).
std::uint32_t sampled_format(std::uint32_t format);

// Decodes every level to RGBA8 (CMPR included), level after level in `dst`.
bool convert_gx_texture(const TexObj& obj, std::uint8_t* dst, std::uint32_t dst_bytes, ConvertedTexture* out,
                        const TlutObj* tlut = nullptr);

std::uint16_t pack_rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b);
void encode_cmpr_solid(std::uint8_t* dst, std::uint32_t width, std::uint32_t height, std::uint16_t rgb565);
void encode_cmpr_asphalt(std::uint8_t* dst, std::uint32_t dim);

} // namespace mkwv::gx
