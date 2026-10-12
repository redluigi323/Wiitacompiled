#include "mkwv/gx/tev.hpp"

#include <cmath>
#include <cstring>

namespace mkwv::gx {
namespace {

std::uint8_t ch_r(std::uint32_t c) {
    return static_cast<std::uint8_t>(c);
}

std::uint8_t ch_g(std::uint32_t c) {
    return static_cast<std::uint8_t>(c >> 8);
}

std::uint8_t ch_b(std::uint32_t c) {
    return static_cast<std::uint8_t>(c >> 16);
}

std::uint8_t ch_a(std::uint32_t c) {
    return static_cast<std::uint8_t>(c >> 24);
}

std::uint8_t clamp8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

struct TevRegs {
    std::uint32_t prev = 0;
    std::uint32_t c0 = 0;
    std::uint32_t c1 = 0;
    std::uint32_t c2 = 0;
};

void write_rgb(TevRegs& regs, std::uint8_t dest, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::uint32_t& slot = dest == 1 ? regs.c0 : dest == 2 ? regs.c1 : dest == 3 ? regs.c2 : regs.prev;
    slot = pack_rgba(r, g, b, ch_a(slot));
}

void write_a(TevRegs& regs, std::uint8_t dest, std::uint8_t a) {
    std::uint32_t& slot = dest == 1 ? regs.c0 : dest == 2 ? regs.c1 : dest == 3 ? regs.c2 : regs.prev;
    slot = pack_rgba(ch_r(slot), ch_g(slot), ch_b(slot), a);
}

std::uint8_t select_rgb(std::uint8_t arg, std::uint32_t ras, std::uint32_t tex, const TevRegs& regs,
                       const std::uint32_t konst[4], std::uint8_t kcsel, int channel) {
    std::uint8_t rgb[3];
    auto from = [&](std::uint32_t c) {
        rgb[0] = ch_r(c);
        rgb[1] = ch_g(c);
        rgb[2] = ch_b(c);
        return rgb[channel];
    };
    switch (arg) {
    case CcCPrev:
        return from(regs.prev);
    case CcAPrev:
        return ch_a(regs.prev);
    case CcC0:
        return from(regs.c0);
    case CcA0:
        return ch_a(regs.c0);
    case CcC1:
        return from(regs.c1);
    case CcA1:
        return ch_a(regs.c1);
    case CcC2:
        return from(regs.c2);
    case CcA2:
        return ch_a(regs.c2);
    case CcTexC:
        return from(tex);
    case CcTexA:
        return ch_a(tex);
    case CcRasC:
        return from(ras);
    case CcRasA:
        return ch_a(ras);
    case CcOne:
        return 255;
    case CcHalf:
        return 128;
    case CcKonst:
        tev_konst_rgb(kcsel, konst, rgb);
        return rgb[channel];
    case CcZero:
    default:
        return 0;
    }
}

std::uint8_t select_a(std::uint8_t arg, std::uint32_t ras, std::uint32_t tex, const TevRegs& regs,
                     const std::uint32_t konst[4], std::uint8_t kasel) {
    switch (arg) {
    case CaAPrev:
        return ch_a(regs.prev);
    case CaA0:
        return ch_a(regs.c0);
    case CaA1:
        return ch_a(regs.c1);
    case CaA2:
        return ch_a(regs.c2);
    case CaTexA:
        return ch_a(tex);
    case CaRasA:
        return ch_a(ras);
    case CaKonst:
        return tev_konst_a(kasel, konst);
    case CaZero:
    default:
        return 0;
    }
}

std::uint32_t apply_swap(std::uint32_t color, const std::uint8_t table[4]) {
    const std::uint8_t ch[4] = {ch_r(color), ch_g(color), ch_b(color), ch_a(color)};
    return pack_rgba(ch[table[0] & 3u], ch[table[1] & 3u], ch[table[2] & 3u], ch[table[3] & 3u]);
}

std::uint8_t combine(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint8_t bias, std::uint8_t op,
                     std::uint8_t shift, bool clamp) {
    const int lerp = (static_cast<int>(a) * (255 - static_cast<int>(c)) + static_cast<int>(b) * static_cast<int>(c) +
                      127) /
                     255;
    int out = op == TevSub ? static_cast<int>(d) - lerp : static_cast<int>(d) + lerp;
    if (bias == 1) {
        out += 128;
    } else if (bias == 2) {
        out -= 128;
    }
    if (shift == 1) {
        out *= 2;
    } else if (shift == 2) {
        out *= 4;
    } else if (shift == 3) {
        out /= 2;
    }
    return clamp ? clamp8(out) : static_cast<std::uint8_t>(out & 0xFF);
}

bool compare_pack(std::uint8_t op, const std::uint8_t a[4], const std::uint8_t b[4]) {
    switch (op) {
    case TevCompR8Gt:
        return a[0] > b[0];
    case TevCompR8Eq:
        return a[0] == b[0];
    case TevCompGr16Gt:
        return (static_cast<int>(a[0]) + static_cast<int>(a[1]) * 256) > (static_cast<int>(b[0]) + static_cast<int>(b[1]) * 256);
    case TevCompGr16Eq:
        return (static_cast<int>(a[0]) + static_cast<int>(a[1]) * 256) == (static_cast<int>(b[0]) + static_cast<int>(b[1]) * 256);
    case TevCompBgr24Gt: {
        const int av = a[0] + a[1] * 256 + a[2] * 65536;
        const int bv = b[0] + b[1] * 256 + b[2] * 65536;
        return av > bv;
    }
    case TevCompBgr24Eq: {
        const int av = a[0] + a[1] * 256 + a[2] * 65536;
        const int bv = b[0] + b[1] * 256 + b[2] * 65536;
        return av == bv;
    }
    default:
        return false;
    }
}

void combine_rgb(std::uint8_t op, std::uint8_t bias, std::uint8_t shift, bool clamp, const std::uint8_t a[4],
                 const std::uint8_t b[4], const std::uint8_t c[4], const std::uint8_t d[4], std::uint8_t rgb[3]) {
    if (op < TevCompR8Gt) {
        for (int i = 0; i < 3; ++i) {
            rgb[i] = combine(a[i], b[i], c[i], d[i], bias, op, shift, clamp);
        }
        return;
    }
    if (op == TevCompRgb8Gt || op == TevCompRgb8Eq) {
        for (int i = 0; i < 3; ++i) {
            const bool cond = (op == TevCompRgb8Eq) ? (a[i] == b[i]) : (a[i] > b[i]);
            rgb[i] = clamp ? clamp8(static_cast<int>(d[i]) + (cond ? c[i] : 0))
                           : static_cast<std::uint8_t>((d[i] + (cond ? c[i] : 0)) & 0xFF);
        }
        return;
    }
    const bool cond = compare_pack(op, a, b);
    for (int i = 0; i < 3; ++i) {
        rgb[i] = clamp ? clamp8(static_cast<int>(d[i]) + (cond ? c[i] : 0))
                       : static_cast<std::uint8_t>((d[i] + (cond ? c[i] : 0)) & 0xFF);
    }
}

std::uint8_t combine_alpha(std::uint8_t op, std::uint8_t bias, std::uint8_t shift, bool clamp, const std::uint8_t a[4],
                           const std::uint8_t b[4], const std::uint8_t c[4], const std::uint8_t d[4]) {
    if (op < TevCompR8Gt) {
        return combine(a[3], b[3], c[3], d[3], bias, op, shift, clamp);
    }
    if (op == TevCompRgb8Gt || op == TevCompRgb8Eq) {
        const bool cond = (op == TevCompRgb8Eq) ? (a[3] == b[3]) : (a[3] > b[3]);
        return clamp ? clamp8(static_cast<int>(d[3]) + (cond ? c[3] : 0))
                     : static_cast<std::uint8_t>((d[3] + (cond ? c[3] : 0)) & 0xFF);
    }
    const bool cond = compare_pack(op, a, b);
    return clamp ? clamp8(static_cast<int>(d[3]) + (cond ? c[3] : 0))
                 : static_cast<std::uint8_t>((d[3] + (cond ? c[3] : 0)) & 0xFF);
}

TevOp classify_stage(const TevStage& s) {
    if (s.color_a == CcZero && s.color_b == CcTexC && (s.color_c == CcRasC || s.color_c == CcCPrev) &&
        s.color_d == CcZero) {
        return TevModulate;
    }
    if (s.color_a == CcRasC && s.color_b == CcTexC && s.color_c == CcTexA && s.color_d == CcZero) {
        return TevDecal;
    }
    if (s.color_a == CcRasC && s.color_b == CcOne && s.color_c == CcTexC && s.color_d == CcZero) {
        return TevBlend;
    }
    if (s.color_a == CcZero && s.color_b == CcZero && s.color_c == CcZero && s.color_d == CcTexC) {
        return TevReplace;
    }
    if (s.color_a == CcZero && s.color_b == CcZero && s.color_c == CcZero && s.color_d == CcRasC) {
        return TevPassClr;
    }
    return TevOther;
}

bool stage_passthrough(const TevStage& s) {
    return s.color_a == CcZero && s.color_b == CcZero && s.color_c == CcZero && s.color_d == CcCPrev;
}

} // namespace

TevOp classify_tev(const State& state) {
    const unsigned n = state.num_tev_stages == 0 ? 1u : static_cast<unsigned>(state.num_tev_stages);
    TevOp op = classify_stage(state.tev[0]);
    unsigned used = 1;
    if (n >= 2 && op == TevPassClr) {
        const TevOp second = classify_stage(state.tev[1]);
        if (second == TevModulate || second == TevReplace || second == TevDecal || second == TevBlend) {
            op = second;
            used = 2;
        }
    }
    for (unsigned i = used; i < n && i < kTevStages; ++i) {
        if (!stage_passthrough(state.tev[i]) && classify_stage(state.tev[i]) == TevOther) {
            break;
        }
    }
    return op;
}

std::uint64_t hash_tev(const State& state) {
    const unsigned n = state.num_tev_stages == 0 ? 1u : static_cast<unsigned>(state.num_tev_stages);
    std::uint64_t h = 14695981039346656037ull;
    auto mix = [&](std::uint8_t byte) {
        h ^= byte;
        h *= 1099511628211ull;
    };
    mix(static_cast<std::uint8_t>(n));
    for (unsigned i = 0; i < n && i < kTevStages; ++i) {
        const TevStage& s = state.tev[i];
        mix(s.color_a);
        mix(s.color_b);
        mix(s.color_c);
        mix(s.color_d);
        mix(s.alpha_a);
        mix(s.alpha_b);
        mix(s.alpha_c);
        mix(s.alpha_d);
        mix(s.color_bias);
        mix(s.color_op);
        mix(s.color_shift);
        mix(s.color_dest);
        mix(s.color_clamp ? 1 : 0);
        mix(s.alpha_bias);
        mix(s.alpha_op);
        mix(s.alpha_shift);
        mix(s.alpha_dest);
        mix(s.alpha_clamp ? 1 : 0);
        mix(s.kcsel);
        mix(s.kasel);
        mix(s.ras_swap);
        mix(s.tex_swap);
        mix(s.texmap);
        mix(s.texcoord);
        mix(s.ras_chan);
        mix(s.tex_enabled ? 1 : 0);
        mix(s.ind_stage);
        mix(s.ind_fmt);
        mix(s.ind_bias);
        mix(s.ind_mtx);
        mix(s.ind_wrap_s);
        mix(s.ind_wrap_t);
        mix(s.ind_add_prev ? 1 : 0);
        mix(s.ind_alpha_sel);
    }
    mix(state.num_ind_stages);
    // The texgen count decides the effective texcoord of every stage and what an untextured
    // stage reads as TEXC; a projective texgen changes the varying and adds a divide.
    mix(state.num_tex_gens);
    for (unsigned t = 0; t < kMaxTexGens; ++t) {
        mix(state.texgens[t].type == TgMtx3x4 ? 1 : 0);
    }
    for (unsigned m = 0; m < kMaxIndMtx; ++m) {
        mix(static_cast<std::uint8_t>(state.ind_mtx[m].scale_exp));
        mix(static_cast<std::uint8_t>(state.ind_mtx[m].raw_scale));
    }
    for (unsigned t = 0; t < 4; ++t) {
        mix(state.tev_swap[t][0]);
        mix(state.tev_swap[t][1]);
        mix(state.tev_swap[t][2]);
        mix(state.tev_swap[t][3]);
    }
    mix(state.alpha_comp0);
    mix(state.alpha_comp1);
    mix(state.alpha_ref0);
    mix(state.alpha_ref1);
    mix(state.alpha_op);
    for (unsigned i = 0; i < 4; ++i) {
        mix(static_cast<std::uint8_t>(state.tev_color[i]));
        mix(static_cast<std::uint8_t>(state.tev_color[i] >> 8));
        mix(static_cast<std::uint8_t>(state.tev_color[i] >> 16));
        mix(static_cast<std::uint8_t>(state.tev_color[i] >> 24));
    }
    for (unsigned i = 0; i < 4; ++i) {
        mix(static_cast<std::uint8_t>(state.konst[i]));
        mix(static_cast<std::uint8_t>(state.konst[i] >> 8));
        mix(static_cast<std::uint8_t>(state.konst[i] >> 16));
        mix(static_cast<std::uint8_t>(state.konst[i] >> 24));
    }
    mix(state.fog.type);
    mix(static_cast<std::uint8_t>(state.fog.color));
    mix(static_cast<std::uint8_t>(state.fog.color >> 8));
    mix(static_cast<std::uint8_t>(state.fog.color >> 16));
    return h;
}

// The *shader program* key. hash_tev mixes the colour values too - the TEV registers,
// konst, the alpha reference levels and the fog colour - because the old emitter baked
// them into the source as literals. Those are uniforms now, so they must not be part of
// the key: including them would compile a fresh program for every palette change, and
// shader compilation on this hardware is far too slow for that. Everything structural
// stays: stage count, every combiner argument and op, bias/shift/clamp/dest, the konst
// *selectors*, the swap tables, texmap/texcoord/ras routing, the indirect setup, which
// alpha comparisons are used, and whether fog is on.
// The program cache key: everything that shapes the generated shader, nothing that rides in
// as a uniform. Looked up for every GL draw, so it is built as one byte image and hashed a word
// at a time in two independent 32-bit lanes (a 64-bit key without 64-bit multiplies, which
// cost several instructions each on the Vita's 32-bit core).
std::uint64_t hash_tev_program(const State& state) {
    static_assert(sizeof(TevStage) == 35 && alignof(TevStage) == 1,
                  "TevStage is hashed as raw bytes: all fields single bytes, no padding. A new field must be one too.");
    const unsigned n = state.num_tev_stages == 0 ? 1u : static_cast<unsigned>(state.num_tev_stages);
    const unsigned stages = n < kTevStages ? n : kTevStages;
    std::uint8_t buf[kTevStages * sizeof(TevStage) + 64];
    std::size_t len = 0;
    buf[len++] = static_cast<std::uint8_t>(n);
    // TevStage is all single-byte fields (no padding), so its bytes are exactly its fields.
    std::memcpy(buf + len, state.tev, stages * sizeof(TevStage));
    len += stages * sizeof(TevStage);
    buf[len++] = state.num_ind_stages;
    // The texgen count decides the effective texcoord of every stage and what an untextured
    // stage reads as TEXC; a projective texgen changes the varying and adds a divide.
    buf[len++] = state.num_tex_gens;
    std::uint8_t proj = 0;
    for (unsigned t = 0; t < kMaxTexGens && t < 8u; ++t) {
        proj |= static_cast<std::uint8_t>((state.texgens[t].type == TgMtx3x4 ? 1u : 0u) << t);
    }
    buf[len++] = proj;
    // The indirect *order* is structural - it picks which sampler and which varying the
    // lookup reads. The matrices and their scale exponents are uniforms, not part of the key.
    for (unsigned k = 0; k < kMaxIndStages; ++k) {
        buf[len++] = state.ind[k].texmap;
        buf[len++] = state.ind[k].texcoord;
    }
    for (unsigned t = 0; t < 4; ++t) {
        for (unsigned c = 0; c < 4; ++c) {
            buf[len++] = state.tev_swap[t][c];
        }
    }
    // The alpha-compare references are part of the source (as in Aurora), so they key too.
    buf[len++] = static_cast<std::uint8_t>(state.alpha_comp0);
    buf[len++] = static_cast<std::uint8_t>(state.alpha_comp1);
    buf[len++] = state.alpha_op;
    buf[len++] = state.alpha_ref0;
    buf[len++] = state.alpha_ref1;
    buf[len++] = static_cast<std::uint8_t>(state.fog.type);
    buf[len++] = static_cast<std::uint8_t>(state.dst_alpha_enable ? 1u : 0u);
    while ((len & 3u) != 0) {
        buf[len++] = 0;
    }
    std::uint32_t h1 = 0x9E3779B9u ^ static_cast<std::uint32_t>(len);
    std::uint32_t h2 = 0x85EBCA6Bu + static_cast<std::uint32_t>(len);
    for (std::size_t i = 0; i < len; i += 4) {
        std::uint32_t k;
        std::memcpy(&k, buf + i, 4);
        std::uint32_t k1 = k * 0xCC9E2D51u;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= 0x1B873593u;
        h1 ^= k1;
        h1 = ((h1 << 13) | (h1 >> 19)) * 5u + 0xE6546B64u;
        std::uint32_t k2 = (k ^ 0x5BD1E995u) * 0x85EBCA77u;
        k2 = (k2 << 13) | (k2 >> 19);
        h2 = (h2 ^ k2) * 0xC2B2AE3Du;
        h2 ^= h2 >> 16;
    }
    h1 ^= h1 >> 16;
    h1 *= 0x85EBCA6Bu;
    h1 ^= h1 >> 13;
    h2 ^= h2 >> 15;
    h2 *= 0x27D4EB2Fu;
    h2 ^= h2 >> 13;
    return (static_cast<std::uint64_t>(h1) << 32) | h2;
}

std::uint32_t eval_tev_color(const State& state, std::uint32_t ras, std::uint32_t tex, std::uint32_t prev) {
    const std::uint32_t maps[1] = {tex};
    return eval_tev_color(state, ras, 0, maps, 1, prev);
}

std::uint32_t eval_tev_color(const State& state, std::uint32_t ras0, std::uint32_t ras1, const std::uint32_t* texmaps,
                             unsigned ntex, std::uint32_t prev) {
    const unsigned n = state.num_tev_stages == 0 ? 1u : static_cast<unsigned>(state.num_tev_stages);
    TevRegs regs;
    regs.prev = prev;
    regs.c0 = state.tev_color[0];
    regs.c1 = state.tev_color[1];
    regs.c2 = state.tev_color[2];
    for (unsigned stage = 0; stage < n && stage < kTevStages; ++stage) {
        const TevStage& s = state.tev[stage];
        std::uint32_t ras = ras0;
        if (s.ras_chan == kRasZero) {
            ras = 0;
        } else if (s.ras_chan == kRasColor1) {
            ras = ras1;
        }
        std::uint32_t tex = 0;
        if (s.tex_enabled) {
            const unsigned map = s.texmap;
            if (texmaps && ntex != 0) {
                tex = (map < ntex) ? texmaps[map] : texmaps[0];
            }
        }
        const std::uint32_t ras_s = apply_swap(ras, state.tev_swap[s.ras_swap & 3u]);
        const std::uint32_t tex_s = apply_swap(tex, state.tev_swap[s.tex_swap & 3u]);
        std::uint8_t a[4];
        std::uint8_t b[4];
        std::uint8_t c[4];
        std::uint8_t d[4];
        for (int i = 0; i < 3; ++i) {
            a[i] = select_rgb(s.color_a, ras_s, tex_s, regs, state.konst, s.kcsel, i);
            b[i] = select_rgb(s.color_b, ras_s, tex_s, regs, state.konst, s.kcsel, i);
            c[i] = select_rgb(s.color_c, ras_s, tex_s, regs, state.konst, s.kcsel, i);
            d[i] = select_rgb(s.color_d, ras_s, tex_s, regs, state.konst, s.kcsel, i);
        }
        a[3] = select_a(s.alpha_a, ras_s, tex_s, regs, state.konst, s.kasel);
        b[3] = select_a(s.alpha_b, ras_s, tex_s, regs, state.konst, s.kasel);
        c[3] = select_a(s.alpha_c, ras_s, tex_s, regs, state.konst, s.kasel);
        d[3] = select_a(s.alpha_d, ras_s, tex_s, regs, state.konst, s.kasel);
        std::uint8_t rgb[3];
        combine_rgb(s.color_op, s.color_bias, s.color_shift, s.color_clamp, a, b, c, d, rgb);
        const std::uint8_t alpha = combine_alpha(s.alpha_op, s.alpha_bias, s.alpha_shift, s.alpha_clamp, a, b, c, d);
        write_rgb(regs, s.color_dest, rgb[0], rgb[1], rgb[2]);
        write_a(regs, s.alpha_dest, alpha);
    }
    return regs.prev;
}

float eval_fog_factor(const State& state, float z) {
    if (state.fog.type == FogNone) {
        return 0.0f;
    }
    float ze = state.fog.a * z - state.fog.c;
    if ((state.fog.type & 8u) == 0 && state.fog.b != 0.0f) {
        const float denom = state.fog.b - z;
        ze = (denom != 0.0f) ? (state.fog.a / denom) - state.fog.c : 0.0f;
    }
    if (ze < 0.0f) {
        ze = 0.0f;
    } else if (ze > 1.0f) {
        ze = 1.0f;
    }
    switch (state.fog.type & 7u) {
    case 4:
        return 1.0f - std::exp2(-8.0f * ze);
    case 5:
        return 1.0f - std::exp2(-8.0f * ze * ze);
    case 6:
        return std::exp2(-8.0f * (1.0f - ze));
    case 7:
        return std::exp2(-8.0f * (1.0f - ze) * (1.0f - ze));
    default:
        return ze;
    }
}

std::uint32_t mix_fog(const State& state, std::uint32_t color, float z) {
    const float f = eval_fog_factor(state, z);
    if (f <= 0.0f) {
        return color;
    }
    auto lerp8 = [f](std::uint8_t src, std::uint8_t dst) {
        return static_cast<std::uint8_t>(src * (1.0f - f) + dst * f + 0.5f);
    };
    return pack_rgba(lerp8(ch_r(color), ch_r(state.fog.color)), lerp8(ch_g(color), ch_g(state.fog.color)),
                     lerp8(ch_b(color), ch_b(state.fog.color)), ch_a(color));
}

int ind_wrap_mask(std::uint8_t mode) {
    switch (mode) {
    case IndWrap256:
        return 255;
    case IndWrap128:
        return 127;
    case IndWrap64:
        return 63;
    case IndWrap32:
        return 31;
    case IndWrap16:
        return 15;
    case IndWrap0:
        return 0;
    default:
        return -1;
    }
}

float wrap_ind_coord(float uv, std::uint8_t mode) {
    const int mask = ind_wrap_mask(mode);
    if (mask < 0) {
        return uv;
    }
    const int pix = static_cast<int>(uv * 256.0f);
    return static_cast<float>(pix & mask) / 256.0f;
}

void eval_indirect_st(const State& state, unsigned stage, std::uint32_t ind_tex, float base_s, float base_t, float* s,
                      float* t) {
    if (s == nullptr || t == nullptr) {
        return;
    }
    *s = base_s;
    *t = base_t;
    if (stage >= kTevStages) {
        return;
    }
    const TevStage& ts = state.tev[stage];
    *s = wrap_ind_coord(base_s, ts.ind_wrap_s);
    *t = wrap_ind_coord(base_t, ts.ind_wrap_t);
    if (ts.ind_mtx == kIndMtxOff || ts.ind_mtx > 3) {
        return;
    }
    unsigned fmt_shift = 0;
    switch (ts.ind_fmt) {
    case IndFmt5:
        fmt_shift = 3;
        break;
    case IndFmt4:
        fmt_shift = 4;
        break;
    case IndFmt3:
        fmt_shift = 5;
        break;
    default:
        break;
    }
    int is = static_cast<int>(ch_r(ind_tex) >> fmt_shift);
    int it = static_cast<int>(ch_g(ind_tex) >> fmt_shift);
    int iu = static_cast<int>(ch_b(ind_tex) >> fmt_shift);
    const int bias = (ts.ind_fmt == IndFmt8) ? -128 : 1;
    if ((ts.ind_bias & 1u) != 0) {
        is += bias;
    }
    if ((ts.ind_bias & 2u) != 0) {
        it += bias;
    }
    if ((ts.ind_bias & 4u) != 0) {
        iu += bias;
    }
    const IndMatrix& mtx = state.ind_mtx[ts.ind_mtx - 1u];
    float os = mtx.m[0] * static_cast<float>(is) + mtx.m[1] * static_cast<float>(it) + mtx.m[2] * static_cast<float>(iu);
    float ot = mtx.m[3] * static_cast<float>(is) + mtx.m[4] * static_cast<float>(it) + mtx.m[5] * static_cast<float>(iu);
    const float scale = std::ldexp(1.0f, mtx.scale_exp);
    os *= scale;
    ot *= scale;
    *s += os / 256.0f;
    *t += ot / 256.0f;
}

std::uint8_t channel_of_konst(std::uint32_t color, unsigned ch) {
    return static_cast<std::uint8_t>(color >> (8u * ch));
}

void tev_konst_rgb(std::uint8_t sel, const std::uint32_t konst[4], std::uint8_t rgb[3]) {
    if (rgb == nullptr) {
        return;
    }
    if (sel <= 7) {
        const std::uint8_t v = static_cast<std::uint8_t>((255u * (8u - sel)) / 8u);
        rgb[0] = v;
        rgb[1] = v;
        rgb[2] = v;
        return;
    }
    if (sel >= 0x0C && sel <= 0x0F) {
        const std::uint32_t c = konst[sel - 0x0C];
        rgb[0] = channel_of_konst(c, 0);
        rgb[1] = channel_of_konst(c, 1);
        rgb[2] = channel_of_konst(c, 2);
        return;
    }
    if (sel >= 0x10 && sel <= 0x1F) {
        const unsigned id = (sel - 0x10u) & 3u;
        const unsigned ch = (sel - 0x10u) / 4u;
        const std::uint8_t v = channel_of_konst(konst[id], ch);
        rgb[0] = v;
        rgb[1] = v;
        rgb[2] = v;
        return;
    }
    rgb[0] = 255;
    rgb[1] = 255;
    rgb[2] = 255;
}

std::uint8_t tev_konst_a(std::uint8_t sel, const std::uint32_t konst[4]) {
    if (sel <= 7) {
        return static_cast<std::uint8_t>((255u * (8u - sel)) / 8u);
    }
    if (sel >= 0x10 && sel <= 0x1F) {
        const unsigned id = (sel - 0x10u) & 3u;
        const unsigned ch = (sel - 0x10u) / 4u;
        return channel_of_konst(konst[id], ch);
    }
    return 255;
}

} // namespace mkwv::gx
