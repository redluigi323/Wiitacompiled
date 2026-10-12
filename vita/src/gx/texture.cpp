#include "mkwv/gx/texture.hpp"

#include <cstring>

namespace mkwv::gx {
namespace {

std::uint8_t expand_bits(std::uint8_t n, unsigned bits) {
    if (bits == 0 || bits >= 8) {
        return n;
    }
    if (bits == 3) {
        return static_cast<std::uint8_t>((n << 5) | (n << 2) | (n >> 1));
    }
    return static_cast<std::uint8_t>((n << (8u - bits)) | (n >> ((bits * 2u) - 8u)));
}

std::uint16_t load_be_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

void store_le_u16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

std::uint8_t reverse_dxt_indices(std::uint8_t bits) {
    return static_cast<std::uint8_t>(((bits & 0x03u) << 6) | ((bits & 0x0Cu) << 2) | ((bits & 0x30u) >> 2) |
                                     ((bits & 0xC0u) >> 6));
}

void write_rgba(std::uint8_t* dst, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint8_t r, std::uint8_t g,
                std::uint8_t b, std::uint8_t a) {
    std::uint8_t* p = dst + ((static_cast<std::size_t>(y) * width + x) * 4u);
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

void rgb565_to_rgba(std::uint16_t c, std::uint8_t* rgba, std::uint8_t alpha) {
    rgba[0] = expand_bits(static_cast<std::uint8_t>((c >> 11) & 0x1Fu), 5);
    rgba[1] = expand_bits(static_cast<std::uint8_t>((c >> 5) & 0x3Fu), 6);
    rgba[2] = expand_bits(static_cast<std::uint8_t>(c & 0x1Fu), 5);
    rgba[3] = alpha;
}

void decode_dxt1_block(const std::uint8_t* src, std::uint8_t colors[4][4]) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(src[0] | (src[1] << 8));
    const std::uint16_t c1 = static_cast<std::uint16_t>(src[2] | (src[3] << 8));
    rgb565_to_rgba(c0, colors[0], 255);
    rgb565_to_rgba(c1, colors[1], 255);
    if (c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            colors[2][i] = static_cast<std::uint8_t>(((colors[0][i] * 2u) + colors[1][i]) / 3u);
            colors[3][i] = static_cast<std::uint8_t>((colors[0][i] + (colors[1][i] * 2u)) / 3u);
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            colors[2][i] = static_cast<std::uint8_t>((colors[0][i] + colors[1][i]) / 2u);
            colors[3][i] = colors[2][i];
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }
}

void write_cmpr_block(std::uint8_t* dst, std::uint16_t c0, std::uint16_t c1) {
    dst[0] = static_cast<std::uint8_t>(c0 >> 8);
    dst[1] = static_cast<std::uint8_t>(c0);
    dst[2] = static_cast<std::uint8_t>(c1 >> 8);
    dst[3] = static_cast<std::uint8_t>(c1);
    dst[4] = 0;
    dst[5] = 0;
    dst[6] = 0;
    dst[7] = 0;
}

void rgb5a3_to_rgba(std::uint16_t texel, std::uint8_t* rgba) {
    if ((texel & 0x8000u) != 0) {
        rgba[0] = expand_bits(static_cast<std::uint8_t>((texel >> 10) & 0x1Fu), 5);
        rgba[1] = expand_bits(static_cast<std::uint8_t>((texel >> 5) & 0x1Fu), 5);
        rgba[2] = expand_bits(static_cast<std::uint8_t>(texel & 0x1Fu), 5);
        rgba[3] = 255;
    } else {
        rgba[0] = expand_bits(static_cast<std::uint8_t>((texel >> 8) & 0x0Fu), 4);
        rgba[1] = expand_bits(static_cast<std::uint8_t>((texel >> 4) & 0x0Fu), 4);
        rgba[2] = expand_bits(static_cast<std::uint8_t>(texel & 0x0Fu), 4);
        rgba[3] = expand_bits(static_cast<std::uint8_t>((texel >> 12) & 0x07u), 3);
    }
}

bool remain_src(std::uint32_t used, std::uint32_t src_bytes, std::uint32_t need) {
    return used <= src_bytes && (src_bytes - used) >= need;
}

bool lookup_tlut(const TlutObj* tlut, std::uint16_t index, std::uint8_t rgba[4]) {
    if (!tlut || !tlut->data || tlut->entries == 0) {
        return false;
    }
    if (index >= tlut->entries) {
        // Aurora (convert_texture_palette) and hardware-accurate decoders give an index past
        // the loaded palette transparent black. Failing here threw away the whole texture.
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        return true;
    }
    const std::uint16_t texel = load_be_u16(tlut->data + static_cast<std::size_t>(index) * 2u);
    if (tlut->format == TlutIa8) {
        // IA8 stores alpha in the first byte and intensity in the second (Aurora's
        // TextureDecoderIA8 reads the u16 without a byteswap: a = in & 0xff, i = in >> 8).
        rgba[0] = static_cast<std::uint8_t>(texel);
        rgba[1] = rgba[0];
        rgba[2] = rgba[0];
        rgba[3] = static_cast<std::uint8_t>(texel >> 8);
        return true;
    }
    if (tlut->format == TlutRgb565) {
        rgb565_to_rgba(texel, rgba, 255);
        return true;
    }
    rgb5a3_to_rgba(texel, rgba);
    return true;
}

} // namespace

std::uint32_t tex_buffer_size(std::uint32_t width, std::uint32_t height, std::uint32_t format) {
    if (width == 0 || height == 0) {
        return 0;
    }
    if ((format & kTexPc) != 0) {
        const std::uint32_t bpp = ((format & 0xFu) == TexI8) ? 1u : 4u;
        return width * height * bpp;
    }
    unsigned shift_x = 0;
    unsigned shift_y = 0;
    std::uint32_t tile_bytes = 32;
    switch (format) {
    case TexI4:
    case TexCI4:
    case TexCMPR:
        shift_x = 3;
        shift_y = 3;
        break;
    case TexI8:
    case TexIA4:
    case TexCI8:
        shift_x = 3;
        shift_y = 2;
        break;
    case TexIA8:
    case TexRGB565:
    case TexRGB5A3:
    case TexCI14X2:
        shift_x = 2;
        shift_y = 2;
        break;
    case TexRGBA8:
        shift_x = 2;
        shift_y = 2;
        tile_bytes = 64;
        break;
    default:
        return 0;
    }
    const std::uint32_t tile_x = (width + (1u << shift_x) - 1u) >> shift_x;
    const std::uint32_t tile_y = (height + (1u << shift_y) - 1u) >> shift_y;
    return tile_bytes * tile_x * tile_y;
}

std::uint32_t dxt1_buffer_size(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return 0;
    }
    return ((width + 3u) / 4u) * ((height + 3u) / 4u) * 8u;
}

bool cmpr_to_rgba8(const std::uint8_t* src, std::uint32_t src_bytes, std::uint32_t width, std::uint32_t height,
                   std::uint8_t* dst, std::uint32_t dst_bytes) {
    if (!src || !dst || width == 0 || height == 0) {
        return false;
    }
    const std::uint32_t need_src = tex_buffer_size(width, height, TexCMPR);
    const std::uint32_t need_dst = width * height * 4u;
    if (src_bytes < need_src || dst_bytes < need_dst) {
        return false;
    }
    const std::uint8_t* p = src;
    for (std::uint32_t yy = 0; yy < height; yy += 8) {
        for (std::uint32_t xx = 0; xx < width; xx += 8) {
            for (std::uint32_t yb = 0; yb < 8; yb += 4) {
                for (std::uint32_t xb = 0; xb < 8; xb += 4) {
                    const std::uint16_t c0 = load_be_u16(p);
                    const std::uint16_t c1 = load_be_u16(p + 2);
                    std::uint8_t table[4][4];
                    rgb565_to_rgba(c0, table[0], 255);
                    rgb565_to_rgba(c1, table[1], 255);
                    if (c0 > c1) {
                        // GX's gradient is 5/8 + 3/8 (Aurora S3TCBlend), not DXT1's thirds.
                        for (int i = 0; i < 3; ++i) {
                            table[2][i] = static_cast<std::uint8_t>((table[1][i] * 3u + table[0][i] * 5u) >> 3);
                            table[3][i] = static_cast<std::uint8_t>((table[0][i] * 3u + table[1][i] * 5u) >> 3);
                        }
                        table[2][3] = 255;
                        table[3][3] = 255;
                    } else {
                        for (int i = 0; i < 3; ++i) {
                            table[2][i] = static_cast<std::uint8_t>((table[0][i] + table[1][i]) >> 1);
                            table[3][i] = table[2][i];
                        }
                        table[2][3] = 255;
                        table[3][3] = 0;
                    }
                    for (std::uint32_t y = 0; y < 4; ++y) {
                        std::uint8_t bits = p[4 + y];
                        for (std::uint32_t x = 0; x < 4; ++x) {
                            const std::uint32_t px = xx + xb + x;
                            const std::uint32_t py = yy + yb + y;
                            if (px < width && py < height) {
                                const std::uint8_t* c = table[(bits >> 6) & 3u];
                                write_rgba(dst, px, py, width, c[0], c[1], c[2], c[3]);
                            }
                            bits = static_cast<std::uint8_t>(bits << 2);
                        }
                    }
                    p += 8;
                }
            }
        }
    }
    return true;
}

bool cmpr_to_dxt1(const std::uint8_t* src, std::uint32_t src_bytes, std::uint32_t width, std::uint32_t height,
                  std::uint8_t* dst, std::uint32_t dst_bytes) {
    if (!src || !dst || width == 0 || height == 0) {
        return false;
    }
    const std::uint32_t need_src = tex_buffer_size(width, height, TexCMPR);
    const std::uint32_t need_dst = dxt1_buffer_size(width, height);
    if (src_bytes < need_src || dst_bytes < need_dst) {
        return false;
    }
    const std::uint32_t blocks_x = (width + 3u) / 4u;
    const std::uint8_t* p = src;
    for (std::uint32_t yy = 0; yy < height; yy += 8) {
        for (std::uint32_t xx = 0; xx < width; xx += 8) {
            for (std::uint32_t yb = 0; yb < 8; yb += 4) {
                for (std::uint32_t xb = 0; xb < 8; xb += 4) {
                    const std::uint32_t px = xx + xb;
                    const std::uint32_t py = yy + yb;
                    if (px < width && py < height) {
                        const std::uint32_t bx = px / 4u;
                        const std::uint32_t by = py / 4u;
                        std::uint8_t* out = dst + (by * blocks_x + bx) * 8u;
                        out[0] = p[1];
                        out[1] = p[0];
                        out[2] = p[3];
                        out[3] = p[2];
                        out[4] = reverse_dxt_indices(p[4]);
                        out[5] = reverse_dxt_indices(p[5]);
                        out[6] = reverse_dxt_indices(p[6]);
                        out[7] = reverse_dxt_indices(p[7]);
                    }
                    p += 8;
                }
            }
        }
    }
    return true;
}

bool dxt1_to_rgba8(const std::uint8_t* src, std::uint32_t src_bytes, std::uint32_t width, std::uint32_t height,
                   std::uint8_t* dst, std::uint32_t dst_bytes) {
    if (!src || !dst || width == 0 || height == 0) {
        return false;
    }
    const std::uint32_t need_src = dxt1_buffer_size(width, height);
    const std::uint32_t need_dst = width * height * 4u;
    if (src_bytes < need_src || dst_bytes < need_dst) {
        return false;
    }
    const std::uint32_t blocks_x = (width + 3u) / 4u;
    const std::uint32_t blocks_y = (height + 3u) / 4u;
    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            const std::uint8_t* block = src + (by * blocks_x + bx) * 8u;
            std::uint8_t colors[4][4];
            decode_dxt1_block(block, colors);
            for (std::uint32_t y = 0; y < 4; ++y) {
                std::uint8_t bits = block[4 + y];
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const std::uint32_t px = bx * 4u + x;
                    const std::uint32_t py = by * 4u + y;
                    if (px < width && py < height) {
                        const std::uint8_t* c = colors[bits & 3u];
                        write_rgba(dst, px, py, width, c[0], c[1], c[2], c[3]);
                    }
                    bits = static_cast<std::uint8_t>(bits >> 2);
                }
            }
        }
    }
    return true;
}

bool tiled_to_rgba8(const std::uint8_t* src, std::uint32_t src_bytes, std::uint32_t width, std::uint32_t height,
                    std::uint32_t format, std::uint8_t* dst, std::uint32_t dst_bytes, const TlutObj* tlut) {
    if (!src || !dst || width == 0 || height == 0) {
        return false;
    }
    const std::uint32_t need_src = tex_buffer_size(width, height, format);
    const std::uint32_t need_dst = width * height * 4u;
    if (need_src == 0 || src_bytes < need_src || dst_bytes < need_dst) {
        return false;
    }
    if ((format & kTexPc) != 0) {
        if ((format & 0xFu) == TexRGBA8) {
            std::memcpy(dst, src, need_dst);
            return true;
        }
        return false;
    }

    std::uint32_t used = 0;
    if (format == TexRGBA8) {
        for (std::uint32_t yy = 0; yy < height; yy += 4) {
            for (std::uint32_t xx = 0; xx < width; xx += 4) {
                if (!remain_src(used, src_bytes, 64)) {
                    return false;
                }
                const std::uint8_t* ar = src + used;
                const std::uint8_t* gb = src + used + 32;
                for (std::uint32_t y = 0; y < 4; ++y) {
                    for (std::uint32_t x = 0; x < 4; ++x) {
                        const std::uint32_t px = xx + x;
                        const std::uint32_t py = yy + y;
                        if (px < width && py < height) {
                            const std::uint32_t i = (y * 4u + x) * 2u;
                            write_rgba(dst, px, py, width, ar[i + 1], gb[i], gb[i + 1], ar[i]);
                        }
                    }
                }
                used += 64;
            }
        }
        return true;
    }

    unsigned bw = 4;
    unsigned bh = 4;
    if (format == TexI4 || format == TexCI4) {
        bw = 8;
        bh = 8;
    } else if (format == TexI8 || format == TexIA4 || format == TexCI8) {
        bw = 8;
        bh = 4;
    }

    for (std::uint32_t yy = 0; yy < height; yy += bh) {
        for (std::uint32_t xx = 0; xx < width; xx += bw) {
            for (std::uint32_t y = 0; y < bh; ++y) {
                for (std::uint32_t x = 0; x < bw; ++x) {
                    const std::uint32_t px = xx + x;
                    const std::uint32_t py = yy + y;
                    std::uint8_t rgba[4] = {0, 0, 0, 255};
                    if (format == TexI4 || format == TexCI4) {
                        if (!remain_src(used, src_bytes, 1)) {
                            return false;
                        }
                        const std::uint8_t nibble = (x & 1u) ? (src[used] & 0x0Fu) : (src[used] >> 4);
                        if ((x & 1u) != 0) {
                            ++used;
                        }
                        if (format == TexCI4) {
                            if (!lookup_tlut(tlut, nibble, rgba)) {
                                return false;
                            }
                        } else {
                            const std::uint8_t i = expand_bits(nibble, 4);
                            rgba[0] = i;
                            rgba[1] = i;
                            rgba[2] = i;
                            rgba[3] = i;
                        }
                    } else if (format == TexI8 || format == TexCI8) {
                        if (!remain_src(used, src_bytes, 1)) {
                            return false;
                        }
                        const std::uint8_t i = src[used++];
                        if (format == TexCI8) {
                            if (!lookup_tlut(tlut, i, rgba)) {
                                return false;
                            }
                        } else {
                            rgba[0] = i;
                            rgba[1] = i;
                            rgba[2] = i;
                            rgba[3] = i;
                        }
                    } else if (format == TexIA4) {
                        if (!remain_src(used, src_bytes, 1)) {
                            return false;
                        }
                        const std::uint8_t texel = src[used++];
                        const std::uint8_t i = expand_bits(static_cast<std::uint8_t>(texel & 0x0Fu), 4);
                        const std::uint8_t a = expand_bits(static_cast<std::uint8_t>(texel >> 4), 4);
                        rgba[0] = i;
                        rgba[1] = i;
                        rgba[2] = i;
                        rgba[3] = a;
                    } else if (format == TexIA8) {
                        if (!remain_src(used, src_bytes, 2)) {
                            return false;
                        }
                        const std::uint16_t texel = load_be_u16(src + used);
                        used += 2;
                        // First byte alpha, second byte intensity (see lookup_tlut).
                        rgba[0] = static_cast<std::uint8_t>(texel);
                        rgba[1] = rgba[0];
                        rgba[2] = rgba[0];
                        rgba[3] = static_cast<std::uint8_t>(texel >> 8);
                    } else if (format == TexCI14X2) {
                        if (!remain_src(used, src_bytes, 2)) {
                            return false;
                        }
                        const std::uint16_t index = static_cast<std::uint16_t>(load_be_u16(src + used) & 0x3FFFu);
                        used += 2;
                        if (!lookup_tlut(tlut, index, rgba)) {
                            return false;
                        }
                    } else if (format == TexRGB565) {
                        if (!remain_src(used, src_bytes, 2)) {
                            return false;
                        }
                        rgb565_to_rgba(load_be_u16(src + used), rgba, 255);
                        used += 2;
                    } else if (format == TexRGB5A3) {
                        if (!remain_src(used, src_bytes, 2)) {
                            return false;
                        }
                        rgb5a3_to_rgba(load_be_u16(src + used), rgba);
                        used += 2;
                    } else {
                        return false;
                    }
                    if (px < width && py < height) {
                        write_rgba(dst, px, py, width, rgba[0], rgba[1], rgba[2], rgba[3]);
                    }
                }
            }
        }
    }
    return true;
}

// Depth formats are sampled through their colour twins (Aurora texture_convert.cpp):
// Z8 as I8, Z16 as IA8, Z24X8 as RGBA8.
std::uint32_t sampled_format(std::uint32_t format) {
    switch (format) {
    case 0x11u:
        return TexI8;
    case 0x13u:
        return TexIA8;
    case 0x16u:
        return TexRGBA8;
    default:
        return format;
    }
}

unsigned texture_level_count(const TexObj& obj) {
    if (!obj.mips) {
        return 1;
    }
    unsigned full = 1;
    for (std::uint32_t d = obj.width > obj.height ? obj.width : obj.height; d > 1u; d >>= 1) {
        ++full;
    }
    // max_lod is in 1/16ths (TX_SETMODE1); Aurora: min(max_lod + 1, full chain).
    const unsigned by_lod = static_cast<unsigned>(obj.max_lod) / 16u + 1u;
    const unsigned n = obj.max_lod != 0 ? (by_lod < full ? by_lod : full) : full;
    return n == 0 ? 1u : n;
}

bool convert_gx_texture(const TexObj& obj, std::uint8_t* dst, std::uint32_t dst_bytes, ConvertedTexture* out,
                        const TlutObj* tlut) {
    if (!out || !dst || !obj.data || obj.width == 0 || obj.height == 0) {
        return false;
    }
    const std::uint32_t format = sampled_format(obj.format);
    const unsigned max_levels = texture_level_count(obj);
    std::uint32_t width = obj.width;
    std::uint32_t height = obj.height;
    const std::uint8_t* src = obj.data;
    std::uint8_t* dst_p = dst;
    std::uint32_t total = 0;
    unsigned levels = 0;
    while (levels < max_levels) {
        const std::uint32_t src_bytes = tex_buffer_size(width, height, format);
        if (src_bytes == 0) {
            return levels != 0 ? true : false;
        }
        const std::uint32_t out_bytes = width * height * 4u;
        if (total + out_bytes > dst_bytes) {
            break;
        }
        const bool ok = format == TexCMPR
                            ? cmpr_to_rgba8(src, src_bytes, width, height, dst_p, dst_bytes - total)
                            : tiled_to_rgba8(src, src_bytes, width, height, format, dst_p, dst_bytes - total, tlut);
        if (!ok) {
            if (levels == 0) {
                return false;
            }
            break;
        }
        dst_p += out_bytes;
        total += out_bytes;
        src += src_bytes;
        ++levels;
        if (width == 1 && height == 1) {
            break;
        }
        width = width > 1 ? width / 2u : 1u;
        height = height > 1 ? height / 2u : 1u;
    }
    out->data = dst;
    out->width = obj.width;
    out->height = obj.height;
    out->bytes = total;
    out->layout = HostRgba8;
    out->mip_count = levels == 0 ? 1u : levels;
    return levels != 0;
}

std::uint16_t pack_rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void encode_cmpr_solid(std::uint8_t* dst, std::uint32_t width, std::uint32_t height, std::uint16_t rgb565) {
    if (!dst) {
        return;
    }
    for (std::uint32_t yy = 0; yy < height; yy += 8) {
        for (std::uint32_t xx = 0; xx < width; xx += 8) {
            for (int i = 0; i < 4; ++i) {
                write_cmpr_block(dst, rgb565, rgb565);
                dst += 8;
            }
        }
    }
}

void encode_cmpr_asphalt(std::uint8_t* dst, std::uint32_t dim) {
    if (!dst || dim < 8) {
        return;
    }
    for (std::uint32_t y = 0; y < dim; y += 8) {
        for (std::uint32_t x = 0; x < dim; x += 8) {
            const bool cell = ((x >> 3) ^ (y >> 3)) & 1u;
            for (std::uint32_t yb = 0; yb < 8; yb += 4) {
                for (std::uint32_t xb = 0; xb < 8; xb += 4) {
                    std::uint8_t r = static_cast<std::uint8_t>(cell ? 52u : 38u);
                    std::uint8_t g = static_cast<std::uint8_t>(cell ? 56u : 42u);
                    std::uint8_t b = static_cast<std::uint8_t>(cell ? 70u : 58u);
                    if (((y + yb) & 15u) == 0u) {
                        r = 210;
                        g = 196;
                        b = 72;
                    }
                    const std::uint16_t c = pack_rgb565(r, g, b);
                    write_cmpr_block(dst, c, c);
                    dst += 8;
                }
            }
        }
    }
}

} // namespace mkwv::gx
