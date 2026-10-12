// Replays a frame written by the on-device capture (vita/src/gx/capture.cpp) on the host.
//
//   mkwv_gx_replay <capture dir> [out dir] [--last N]
//
// Renders the captured draws with a software rasteriser and a reference TEV (the integer
// model Aurora uses: tev_regular_i32, byte-wrapped a/b/c, compare ops, final overflow,
// alpha compare), exact GX blending including the real logic ops, depth test, scissor and
// the frame's EFB copies. Writes:
//
//   replay.ppm       the frame as the reference sees it
//   report.txt       one line per draw: what it is, what it reads, what the reference did
//   prog_NNN.cg      the fragment program our emitter builds for each distinct TEV setup
//   tex_NNN.ppm      every captured texture (RGB; alpha as a separate tex_NNN_a.pgm)
//
// A frame that looks right here and wrong on the Vita is a renderer problem (shader, GL,
// emulator); one that looks wrong here too is a decoder/HLE problem.

#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/decode.hpp"
#include "mkwv/gx/state.hpp"
#include "mkwv/gx/tev.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace mkwv::gx;

namespace {

struct Vtx {
    float pos[4];
    std::uint32_t c0, c1;
    float tc[kMaxTexGens][3];
};

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;  // RGBA8
};

struct RTex {
    unsigned map = 0, w = 0, h = 0, fmt = 0, ws = 0, wt = 0, minf = 0, magf = 0;
    int mips = 0;
    std::uint32_t phys = 0;
    unsigned tlut_tmem = 0, tlut_fmt = 0, tlut_entries = 0;
    std::string file;
};

struct RDraw {
    unsigned batch = 0, index = 0, prim = 0, first = 0, count = 0;
    unsigned blend_mode = 0, blend_src = 0, blend_dst = 0, logic = 0;
    int depth_test = 0, depth_write = 0;
    unsigned depth_func = 0, cull = 0;
    int color_update = 1, alpha_update = 1;
    unsigned pixel_fmt = 0;
    int dst_alpha_en = 0;
    unsigned dst_alpha = 0;
    int zcomploc = 1;  // 1: depth compared before texturing (so alpha-killed pixels still write depth)
    int has_vp = 0;
    float vp[6]{};
    int has_rsc = 0, rsc[4]{};
    int has_sc = 0, sc[4]{};
    int off[2]{};
    unsigned num_chans = 0;
    State st;  // TEV structure and registers in the shape the emitter reads
    int tev_reg[4][4]{};
    std::vector<RTex> tex;
};

struct RCopy {
    unsigned batch = 0, draw_index = 0;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    unsigned dw = 0, dh = 0, fmt = 0;
    int clear = 0;
    unsigned clear_color = 1, clear_alpha = 1, clear_depth = 1;
    std::uint32_t clear_z = 0xFFFFFFu;
    std::uint32_t clear_rgba = 0, dest = 0;
};

struct Capture {
    std::vector<std::vector<Vtx>> batches;
    std::vector<RDraw> draws;
    std::vector<RCopy> copies;
    std::uint32_t clear_rgba = 0;
};

bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<std::size_t>(n) : 0u);
    const bool ok = n <= 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

bool load_capture(const std::string& dir, Capture& cap) {
    std::FILE* f = std::fopen((dir + "/frame.txt").c_str(), "r");
    if (!f) {
        std::fprintf(stderr, "cannot open %s/frame.txt\n", dir.c_str());
        return false;
    }
    char line[4096];
    RDraw* cur = nullptr;
    unsigned batch = 0;
    while (std::fgets(line, sizeof(line), f)) {
        std::istringstream in(line);
        std::string key;
        in >> key;
        if (key == "FRAME_END") {
            unsigned n = 0;
            std::string clr;
            in >> n >> clr;
            cap.clear_rgba = static_cast<std::uint32_t>(std::stoul(clr, nullptr, 16));
            break; // A shorter capture can leave an older file tail on Vita.
        }
        if (key == "BATCH") {
            unsigned draws = 0, verts = 0;
            std::string file;
            in >> batch >> draws >> verts >> file;
            std::vector<std::uint8_t> raw;
            read_file(dir + "/" + file, raw);
            std::vector<Vtx> v(raw.size() / sizeof(Vtx));
            if (!v.empty()) {
                std::memcpy(v.data(), raw.data(), v.size() * sizeof(Vtx));
            }
            if (cap.batches.size() <= batch) {
                cap.batches.resize(batch + 1);
            }
            cap.batches[batch] = std::move(v);
        } else if (key == "COPY") {
            RCopy c;
            c.batch = batch;
            std::string clr, dest;
            in >> c.draw_index >> c.sx >> c.sy >> c.sw >> c.sh >> c.dw >> c.dh >> c.fmt >> c.clear >> clr >> dest;
            c.clear_rgba = static_cast<std::uint32_t>(std::stoul(clr, nullptr, 16));
            c.dest = static_cast<std::uint32_t>(std::stoul(dest, nullptr, 16));
            // Optional fields added with masked copy clears; old captures retain
            // their old unconditional-clear semantics rather than inventing masks.
            std::string depth;
            if (in >> c.clear_color >> c.clear_alpha >> c.clear_depth >> depth)
                c.clear_z = static_cast<std::uint32_t>(std::stoul(depth, nullptr, 16));
            cap.copies.push_back(c);
        } else if (key == "DRAW") {
            cap.draws.emplace_back();
            cur = &cap.draws.back();
            cur->st.reset();
            in >> cur->batch >> cur->index >> cur->prim >> cur->first >> cur->count;
        } else if (cur == nullptr) {
            if (key == "FRAME_END") {
                unsigned n = 0;
                std::string clr;
                in >> n >> clr;
                cap.clear_rgba = static_cast<std::uint32_t>(std::stoul(clr, nullptr, 16));
            }
            continue;
        } else if (key == "BLEND") {
            in >> cur->blend_mode >> cur->blend_src >> cur->blend_dst >> cur->logic;
        } else if (key == "PE") {
            in >> cur->depth_test >> cur->depth_write >> cur->depth_func >> cur->cull >> cur->color_update >>
                cur->alpha_update >> cur->pixel_fmt >> cur->dst_alpha_en >> cur->dst_alpha >> cur->zcomploc;
        } else if (key == "RAST") {
            in >> cur->has_vp;
            for (float& v : cur->vp) in >> v;
            in >> cur->has_rsc;
            for (int& v : cur->rsc) in >> v;
            in >> cur->has_sc;
            for (int& v : cur->sc) in >> v;
            in >> cur->off[0] >> cur->off[1];
        } else if (key == "GEN") {
            unsigned ntev = 0, ntg = 0, nch = 0, nind = 0;
            in >> ntev >> ntg >> nch >> nind;
            cur->st.num_tev_stages = static_cast<std::uint8_t>(ntev);
            cur->st.num_tex_gens = static_cast<std::uint8_t>(ntg);
            cur->st.num_chans = static_cast<std::uint8_t>(nch);
            cur->num_chans = nch;
            cur->st.num_ind_stages = static_cast<std::uint8_t>(nind);
            for (unsigned t = 0; t < kMaxTexGens; ++t) {
                unsigned ty = 0;
                in >> ty;
                cur->st.texgens[t].type = static_cast<TexGenType>(ty);
            }
        } else if (key == "STAGE") {
            unsigned k = 0;
            in >> k;
            unsigned v[35];
            for (unsigned& x : v) in >> x;
            TevStage& t = cur->st.tev[k & 15u];
            unsigned i = 0;
            t.color_a = v[i++]; t.color_b = v[i++]; t.color_c = v[i++]; t.color_d = v[i++];
            t.alpha_a = v[i++]; t.alpha_b = v[i++]; t.alpha_c = v[i++]; t.alpha_d = v[i++];
            t.color_bias = v[i++]; t.color_op = v[i++]; t.color_shift = v[i++]; t.color_dest = v[i++];
            t.color_clamp = v[i++] != 0;
            t.alpha_bias = v[i++]; t.alpha_op = v[i++]; t.alpha_shift = v[i++]; t.alpha_dest = v[i++];
            t.alpha_clamp = v[i++] != 0;
            t.kcsel = v[i++]; t.kasel = v[i++]; t.ras_swap = v[i++]; t.tex_swap = v[i++];
            t.texmap = v[i++]; t.texcoord = v[i++]; t.ras_chan = v[i++]; t.tex_enabled = v[i++] != 0;
            t.ind_stage = v[i++]; t.ind_fmt = v[i++]; t.ind_bias = v[i++]; t.ind_mtx = v[i++];
            t.ind_wrap_s = v[i++]; t.ind_wrap_t = v[i++]; t.ind_add_prev = v[i++] != 0;
            t.ind_alpha_sel = v[i++]; t.ind_utc_lod = v[i++] != 0;
        } else if (key == "IND") {
            unsigned k = 0, a = 0, b = 0, c = 0, d = 0;
            in >> k >> a >> b >> c >> d;
            IndStage& is = cur->st.ind[k & 3u];
            is.texmap = a; is.texcoord = b; is.scale_s = c; is.scale_t = d;
        } else if (key == "IMTX") {
            unsigned k = 0;
            int e = 0;
            in >> k;
            IndMatrix& m = cur->st.ind_mtx[k % kMaxIndMtx];
            for (float& x : m.m) in >> x;
            in >> e;
            m.scale_exp = static_cast<std::int8_t>(e);
        } else if (key == "REGS") {
            for (auto& r : cur->tev_reg) {
                for (int& c : r) in >> c;
            }
            for (unsigned r = 0; r < 4; ++r) {
                std::string h;
                in >> h;
                cur->st.konst[r] = static_cast<std::uint32_t>(std::stoul(h, nullptr, 16));
            }
            for (auto& r : cur->st.tev_swap) {
                for (auto& c : r) {
                    unsigned x = 0;
                    in >> x;
                    c = static_cast<std::uint8_t>(x);
                }
            }
            for (unsigned r = 0; r < 4; ++r) {
                for (unsigned c = 0; c < 4; ++c) {
                    cur->st.tev_reg[r][c] = static_cast<std::int16_t>(cur->tev_reg[r][c]);
                }
                auto c8 = [](int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); };
                cur->st.tev_color[r] = pack_rgba(c8(cur->tev_reg[r][0]), c8(cur->tev_reg[r][1]), c8(cur->tev_reg[r][2]),
                                                 c8(cur->tev_reg[r][3]));
            }
        } else if (key == "ACMP") {
            unsigned a = 0, b = 0, c = 0, d = 0, e = 0;
            in >> a >> b >> c >> d >> e;
            cur->st.alpha_comp0 = static_cast<Compare>(a);
            cur->st.alpha_ref0 = b;
            cur->st.alpha_comp1 = static_cast<Compare>(c);
            cur->st.alpha_ref1 = d;
            cur->st.alpha_op = e;
        } else if (key == "FOG") {
            unsigned type = 0;
            std::string col, r0, r1, r2;
            in >> type >> col >> cur->st.fog.a >> cur->st.fog.b >> cur->st.fog.c >> r0 >> r1 >> r2;
            cur->st.fog.type = type;
            cur->st.fog.color = static_cast<std::uint32_t>(std::stoul(col, nullptr, 16));
            cur->st.fog.raw0 = static_cast<std::uint32_t>(std::stoul(r0, nullptr, 16));
            cur->st.fog.raw1 = static_cast<std::uint32_t>(std::stoul(r1, nullptr, 16));
            cur->st.fog.raw2 = static_cast<std::uint32_t>(std::stoul(r2, nullptr, 16));
        } else if (key == "TEX") {
            RTex t;
            int bias = 0;
            unsigned minl = 0, maxl = 0;
            std::string phys;
            in >> t.map >> t.w >> t.h >> t.fmt >> t.ws >> t.wt >> t.minf >> t.magf >> t.mips >> minl >> maxl >> bias >>
                phys >> t.tlut_tmem >> t.tlut_fmt >> t.tlut_entries >> t.file;
            t.phys = static_cast<std::uint32_t>(std::stoul(phys, nullptr, 16));
            cur->tex.push_back(t);
        } else if (key == "END") {
            cur = nullptr;
        }
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Reference TEV (Aurora shader.cpp, integer form).

int wrap8(int v) {
    return ((v % 256) + 256) % 256;
}

int regular(int a, int b, int c, int d, int bias, unsigned scale, bool sub) {
    int d_part = d + bias;
    int lerp_part = (a << 8) + (b - a) * (c + (c >> 7));
    if (scale == 1) {
        d_part <<= 1;
        lerp_part <<= 1;
    } else if (scale == 2) {
        d_part <<= 2;
        lerp_part <<= 2;
    }
    if (scale != 3) {
        lerp_part += sub ? 127 : 128;
    }
    const int lerp = lerp_part >> 8;  // arithmetic shift
    int result = sub ? d_part - lerp : d_part + lerp;
    if (scale == 3) {
        result >>= 1;
    }
    return result;
}

struct Rgba {
    int v[4];
};

Rgba swap4(const Rgba& in, const std::uint8_t table[4]) {
    return {{in.v[table[0] & 3], in.v[table[1] & 3], in.v[table[2] & 3], in.v[table[3] & 3]}};
}

int konst_c(const State& st, unsigned sel, int ch) {
    if (sel <= 7) {
        return static_cast<int>(std::lround((8.0 - sel) / 8.0 * 255.0));
    }
    auto k = [&](unsigned id, int c) { return static_cast<int>((st.konst[id & 3] >> (8 * c)) & 0xFF); };
    if (sel >= 0x0C && sel <= 0x0F) {
        return k(sel - 0x0C, ch);
    }
    if (sel >= 0x10 && sel <= 0x1F) {
        return k((sel - 0x10) & 3, static_cast<int>((sel - 0x10) / 4));
    }
    return 255;
}

int konst_a(const State& st, unsigned sel) {
    if (sel <= 7) {
        return static_cast<int>(std::lround((8.0 - sel) / 8.0 * 255.0));
    }
    if (sel >= 0x10 && sel <= 0x1F) {
        return static_cast<int>((st.konst[(sel - 0x10) & 3] >> (8 * ((sel - 0x10) / 4))) & 0xFF);
    }
    return 255;
}

bool acmp(Compare c, int a, int ref) {
    switch (c) {
    case Never: return false;
    case Less: return a < ref;
    case Equal: return a == ref;
    case LEqual: return a <= ref;
    case Greater: return a > ref;
    case NEqual: return a != ref;
    case GEqual: return a >= ref;
    default: return true;
    }
}

struct TexUnit {
    const Image* img = nullptr;
    unsigned ws = 0, wt = 0;
    bool linear = true;
};

int wrap_coord(int i, int n, unsigned mode) {
    if (mode == WrapRepeat) {
        return ((i % n) + n) % n;
    }
    if (mode == WrapMirror) {
        const int p = ((i % (2 * n)) + 2 * n) % (2 * n);
        return p < n ? p : 2 * n - 1 - p;
    }
    return std::clamp(i, 0, n - 1);
}

Rgba sample(const TexUnit& t, float u, float v) {
    if (t.img == nullptr || t.img->w == 0) {
        return {{255, 0, 255, 255}};  // missing texture: magenta, so it stands out
    }
    const Image& im = *t.img;
    auto texel = [&](int x, int y) {
        x = wrap_coord(x, im.w, t.ws);
        y = wrap_coord(y, im.h, t.wt);
        const std::uint8_t* p = &im.px[(static_cast<std::size_t>(y) * im.w + x) * 4];
        return Rgba{{p[0], p[1], p[2], p[3]}};
    };
    const float fx = u * im.w - 0.5f;
    const float fy = v * im.h - 0.5f;
    if (!t.linear) {
        return texel(static_cast<int>(std::floor(fx + 0.5f)), static_cast<int>(std::floor(fy + 0.5f)));
    }
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float ax = fx - x0, ay = fy - y0;
    const Rgba a = texel(x0, y0), b = texel(x0 + 1, y0), c = texel(x0, y0 + 1), d = texel(x0 + 1, y0 + 1);
    Rgba r;
    for (int i = 0; i < 4; ++i) {
        const float top = a.v[i] + (b.v[i] - a.v[i]) * ax;
        const float bot = c.v[i] + (d.v[i] - c.v[i]) * ax;
        r.v[i] = static_cast<int>(std::lround(top + (bot - top) * ay));
    }
    return r;
}

struct PixelIn {
    float rast[2][4];  // 0..1
    float tc[kMaxTexGens][3];
};

// Returns false when the alpha test discards. `out` is 0..255 per channel.
bool eval_tev(const RDraw& d, const TexUnit* units, const PixelIn& in, int out[4], unsigned* flags) {
    const State& st = d.st;
    int reg[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            reg[r][c] = d.tev_reg[r][c];
        }
    }
    const unsigned stages = st.num_tev_stages == 0 ? 1u : std::min<unsigned>(st.num_tev_stages, kTevStages);
    Rgba ras[2];
    for (int k = 0; k < 2; ++k) {
        for (int c = 0; c < 4; ++c) {
            ras[k].v[c] = static_cast<int>(std::lround(std::clamp(in.rast[k][c], 0.0f, 1.0f) * 255.0f));
        }
    }
    for (unsigned s = 0; s < stages; ++s) {
        const TevStage& t = st.tev[s];
        if (t.ind_mtx != kIndMtxOff || t.ind_add_prev || t.ind_wrap_s != IndWrapOff || t.ind_wrap_t != IndWrapOff) {
            *flags |= 1u;  // indirect: not modelled here, sampled without the offset
        }
        Rgba tex;
        const int coord = tev_effective_texcoord(st, t.texcoord);
        if (tev_stage_can_sample(st, t) && coord >= 0) {
            const float* tc = in.tc[coord];
            float u = tc[0], v = tc[1];
            if (st.texgens[coord].type == TgMtx3x4 && tc[2] != 0.0f) {
                u /= tc[2];
                v /= tc[2];
            }
            tex = sample(units[t.texmap & 7], u, v);
        } else {
            const int z = st.num_tex_gens == 0 ? 0 : 255;
            tex = {{z, z, z, z}};
        }
        const Rgba ts = swap4(tex, st.tev_swap[t.tex_swap & 3]);
        Rgba rs{{0, 0, 0, 0}};
        if (t.ras_chan == kRasColor0 || t.ras_chan == kRasColor1) {
            rs = swap4(ras[t.ras_chan], st.tev_swap[t.ras_swap & 3]);
        } else if (t.ras_chan == kRasAlphaBump || t.ras_chan == kRasAlphaBumpN) {
            *flags |= 1u;
        }
        auto carg = [&](unsigned a, int ch) -> int {
            switch (a) {
            case CcCPrev: return reg[0][ch];
            case CcAPrev: return reg[0][3];
            case CcC0: return reg[1][ch];
            case CcA0: return reg[1][3];
            case CcC1: return reg[2][ch];
            case CcA1: return reg[2][3];
            case CcC2: return reg[3][ch];
            case CcA2: return reg[3][3];
            case CcTexC: return ts.v[ch];
            case CcTexA: return ts.v[3];
            case CcRasC: return rs.v[ch];
            case CcRasA: return rs.v[3];
            case CcOne: return 255;
            case CcHalf: return 128;
            case CcKonst: return konst_c(st, t.kcsel, ch);
            default: return 0;
            }
        };
        auto aarg = [&](unsigned a) -> int {
            switch (a) {
            case CaAPrev: return reg[0][3];
            case CaA0: return reg[1][3];
            case CaA1: return reg[2][3];
            case CaA2: return reg[3][3];
            case CaTexA: return ts.v[3];
            case CaRasA: return rs.v[3];
            case CaKonst: return konst_a(st, t.kasel);
            default: return 0;
            }
        };
        int A[4], B[4], C[4], D[4];
        for (int ch = 0; ch < 3; ++ch) {
            A[ch] = wrap8(carg(t.color_a, ch));
            B[ch] = wrap8(carg(t.color_b, ch));
            C[ch] = wrap8(carg(t.color_c, ch));
            D[ch] = carg(t.color_d, ch);
        }
        A[3] = wrap8(aarg(t.alpha_a));
        B[3] = wrap8(aarg(t.alpha_b));
        C[3] = wrap8(aarg(t.alpha_c));
        D[3] = aarg(t.alpha_d);
        int oc[3];
        if (t.color_op >= TevCompR8Gt) {
            const bool eq = (t.color_op & 1) != 0;
            auto cmp = [&](long x, long y) { return eq ? x == y : x > y; };
            for (int ch = 0; ch < 3; ++ch) {
                bool pass = false;
                switch (t.color_op & ~1u) {
                case TevCompR8Gt: pass = cmp(A[0], B[0]); break;
                case TevCompGr16Gt: pass = cmp(A[0] + A[1] * 256L, B[0] + B[1] * 256L); break;
                case TevCompBgr24Gt: pass = cmp(A[0] + A[1] * 256L + A[2] * 65536L, B[0] + B[1] * 256L + B[2] * 65536L); break;
                default: pass = cmp(A[ch], B[ch]); break;
                }
                oc[ch] = D[ch] + (pass ? C[ch] : 0);
            }
        } else {
            const int bias = t.color_bias == 1 ? 128 : (t.color_bias == 2 ? -128 : 0);
            for (int ch = 0; ch < 3; ++ch) {
                oc[ch] = regular(A[ch], B[ch], C[ch], D[ch], bias, t.color_shift, t.color_op == TevSub);
            }
        }
        for (int& v : oc) {
            v = t.color_clamp ? std::clamp(v, 0, 255) : std::clamp(v, -1024, 1023);
        }
        int oa;
        if (t.alpha_op >= TevCompR8Gt) {
            const bool eq = (t.alpha_op & 1) != 0;
            auto cmp = [&](long x, long y) { return eq ? x == y : x > y; };
            bool pass = false;
            switch (t.alpha_op & ~1u) {
            case TevCompR8Gt: pass = cmp(A[0], B[0]); break;
            case TevCompGr16Gt: pass = cmp(A[0] + A[1] * 256L, B[0] + B[1] * 256L); break;
            case TevCompBgr24Gt: pass = cmp(A[0] + A[1] * 256L + A[2] * 65536L, B[0] + B[1] * 256L + B[2] * 65536L); break;
            default: pass = cmp(A[3], B[3]); break;
            }
            oa = D[3] + (pass ? C[3] : 0);
        } else {
            const int bias = t.alpha_bias == 1 ? 128 : (t.alpha_bias == 2 ? -128 : 0);
            oa = regular(A[3], B[3], C[3], D[3], bias, t.alpha_shift, t.alpha_op == TevSub);
        }
        oa = t.alpha_clamp ? std::clamp(oa, 0, 255) : std::clamp(oa, -1024, 1023);
        for (int ch = 0; ch < 3; ++ch) {
            reg[t.color_dest & 3][ch] = oc[ch];
        }
        reg[t.alpha_dest & 3][3] = oa;
    }
    const TevStage& last = st.tev[stages - 1];
    int prev[4] = {reg[last.color_dest & 3][0], reg[last.color_dest & 3][1], reg[last.color_dest & 3][2],
                   reg[last.alpha_dest & 3][3]};
    if (st.fog.type != FogNone) {
        *flags |= 2u;  // fog: not modelled here
    }
    for (int c = 0; c < 4; ++c) {
        out[c] = wrap8(prev[c]);
    }
    const bool p0 = acmp(st.alpha_comp0, out[3], st.alpha_ref0);
    const bool p1 = acmp(st.alpha_comp1, out[3], st.alpha_ref1);
    bool pass = true;
    switch (st.alpha_op & 3) {
    case 0: pass = p0 && p1; break;
    case 1: pass = p0 || p1; break;
    case 2: pass = p0 != p1; break;
    default: pass = p0 == p1; break;
    }
    return pass;
}

// ---------------------------------------------------------------------------------------------
// Framebuffer


// --- GPU-renderer model (MKWV_REPLAY_GPU=1) -------------------------------------------------
// Mirrors vita/src/gx/gl_submit.cpp: an EFB copy becomes a texture converted per copy format
// (intensity/channel matrices, depth bytes from the depth buffer), box filtered to the copy
// size, and a texture whose address, size and format match a copy samples that copy instead
// of guest RAM.
unsigned copy_sample_fmt(unsigned copy_fmt) {
    switch (copy_fmt) {
    case 0x20u: return 0u;                          // R4 -> I4
    case 0x27u: case 0x28u: case 0x29u: case 0x2Au:
    case 0x11u: case 0x39u: case 0x3Au: return 1u;   // -> I8
    case 0x22u: return 2u;                          // RA4 -> IA4
    case 0x23u: case 0x2Bu: case 0x2Cu: case 0x13u: case 0x3Cu: return 3u;  // -> IA8
    case 0x16u: return 6u;                          // Z24X8 -> RGBA8
    case 0x30u: return 0u;
    default: return copy_fmt & 0xFu;
    }
}

bool copy_is_depth(unsigned f) {
    return f == 0x11u || f == 0x13u || f == 0x16u || f == 0x30u || f == 0x39u || f == 0x3Au || f == 0x3Cu;
}

void convert_copy_px(unsigned fmt, const float in[4], float depth, std::uint8_t out[4]) {
    float r = in[0] / 255.0f, g = in[1] / 255.0f, b = in[2] / 255.0f, a = in[3] / 255.0f;
    if (copy_is_depth(fmt)) {
        const float z = std::min(std::floor(std::clamp(depth, 0.0f, 1.0f) * 16777215.0f + 0.5f), 16777215.0f);
        const float hi = std::floor(z / 65536.0f), mid = std::floor(z / 256.0f) - hi * 256.0f;
        const float lo = z - std::floor(z / 256.0f) * 256.0f;
        r = hi / 255.0f; g = mid / 255.0f; b = lo / 255.0f; a = 1.0f;
    }
    const float y = std::min(1.0f, 0.257f * r + 0.504f * g + 0.098f * b + 16.0f / 255.0f);
    float o[4] = {r, g, b, a};
    bool q4 = false;
    switch (fmt) {
    case 0x0u: o[0] = o[1] = o[2] = o[3] = y; q4 = true; break;
    case 0x1u: o[0] = o[1] = o[2] = o[3] = y; break;
    case 0x2u: o[0] = o[1] = o[2] = y; o[3] = a; q4 = true; break;
    case 0x3u: o[0] = o[1] = o[2] = y; o[3] = a; break;
    case 0x4u: o[3] = 1.0f; break;
    case 0x30u: case 0x20u: o[0] = o[1] = o[2] = o[3] = r; q4 = true; break;
    case 0x22u: o[0] = o[1] = o[2] = r; o[3] = a; q4 = true; break;
    case 0x23u: case 0x13u: o[0] = o[1] = o[2] = r; o[3] = a; break;
    case 0x27u: o[0] = o[1] = o[2] = o[3] = a; break;
    case 0x11u: case 0x28u: o[0] = o[1] = o[2] = o[3] = r; break;
    case 0x39u: case 0x29u: o[0] = o[1] = o[2] = o[3] = g; break;
    case 0x3Au: case 0x2Au: o[0] = o[1] = o[2] = o[3] = b; break;
    case 0x2Bu: o[0] = o[1] = o[2] = r; o[3] = g; break;
    case 0x2Cu: case 0x3Cu: o[0] = o[1] = o[2] = g; o[3] = b; break;
    default: break;
    }
    for (int k = 0; k < 4; ++k) {
        float v = std::clamp(o[k], 0.0f, 1.0f);
        if (q4) v = std::min(std::floor(v * 16.0f) / 15.0f, 1.0f);
        out[k] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
    }
}

struct GpuCopy {
    Image img;
    unsigned fmt = 0;
};

struct Efb {
    int w = 640, h = 528;
    std::vector<int> color;  // RGBA, 0..255
    std::vector<float> depth;
    void init(int W, int H, std::uint32_t clear) {
        w = W;
        h = H;
        color.assign(static_cast<std::size_t>(w) * h * 4, 0);
        depth.assign(static_cast<std::size_t>(w) * h, 1.0f);
        for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
            for (int c = 0; c < 4; ++c) {
                color[i * 4 + c] = static_cast<int>((clear >> (8 * c)) & 0xFF);
            }
        }
    }
};

int factor(unsigned f, bool is_src, const int src[4], const int dst[4], int ch) {
    switch (f) {
    case BlZero: return 0;
    case BlOne: return 255;
    case BlSrcClr: return is_src ? dst[ch] : src[ch];
    case BlInvSrcClr: return 255 - (is_src ? dst[ch] : src[ch]);
    case BlSrcAlpha: return src[3];
    case BlInvSrcAlpha: return 255 - src[3];
    case BlDstAlpha: return dst[3];
    default: return 255 - dst[3];
    }
}

int logic(unsigned op, int s, int d) {
    switch (op) {
    case LoClear: return 0;
    case LoAnd: return s & d;
    case LoAndReverse: return s & ~d & 255;
    case LoCopy: return s;
    case LoAndInverted: return ~s & d & 255;
    case LoNoop: return d;
    case LoXor: return s ^ d;
    case LoOr: return s | d;
    case LoNor: return ~(s | d) & 255;
    case LoEquiv: return ~(s ^ d) & 255;
    case LoInvert: return ~d & 255;
    case LoOrReverse: return (s | ~d) & 255;
    case LoCopyInverted: return ~s & 255;
    case LoOrInverted: return (~s | d) & 255;
    case LoNand: return ~(s & d) & 255;
    default: return 255;
    }
}

bool depth_pass(unsigned func, float z, float stored) {
    switch (func) {
    case Never: return false;
    case Less: return z < stored;
    case Equal: return z == stored;
    case LEqual: return z <= stored;
    case Greater: return z > stored;
    case NEqual: return z != stored;
    case GEqual: return z >= stored;
    default: return true;
    }
}

int g_trace_x = -1, g_trace_y = -1;  // MKWV_TRACE=x,y: log every write to that pixel
long g_cur_draw = 0;

struct Stats {
    unsigned pixels = 0, discarded = 0, depth_failed = 0, flags = 0;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
    unsigned tris = 0, clipped = 0;
};

void raster_draw(Efb& efb, const RDraw& d, const std::vector<Vtx>& verts, const TexUnit* units, Stats& stats) {
    std::vector<std::uint16_t> idx(d.count * 6 + 16);
    const std::uint32_t n = expand_indices(static_cast<Primitive>(d.prim == Lines || d.prim == LineStrip || d.prim == Points
                                                                      ? Triangles
                                                                      : d.prim),
                                           d.count, idx.data(), static_cast<std::uint32_t>(idx.size()));
    float vx = 0, vy = 0, vw = static_cast<float>(efb.w), vh = static_cast<float>(efb.h), zn = 0, zf = 1;
    if (d.has_vp && d.vp[2] > 1 && d.vp[3] > 1) {
        vx = d.vp[0] - d.off[0];
        vy = d.vp[1] - d.off[1];
        vw = d.vp[2];
        vh = d.vp[3];
        zn = d.vp[4];
        zf = d.vp[5];
    }
    int cx0 = 0, cy0 = 0, cx1 = efb.w, cy1 = efb.h;
    if (d.has_rsc) {
        cx0 = d.rsc[0]; cy0 = d.rsc[1]; cx1 = d.rsc[0] + d.rsc[2]; cy1 = d.rsc[1] + d.rsc[3];
    } else if (d.has_sc) {
        cx0 = d.sc[0] - d.off[0]; cy0 = d.sc[1] - d.off[1];
        cx1 = cx0 + d.sc[2]; cy1 = cy0 + d.sc[3];
    }
    cx0 = std::max(cx0, 0); cy0 = std::max(cy0, 0);
    cx1 = std::min(cx1, efb.w); cy1 = std::min(cy1, efb.h);
    for (std::uint32_t t = 0; t + 2 < n; t += 3) {
        const Vtx* v[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t vi = d.first + idx[t + k];
            if (vi >= verts.size()) {
                ok = false;
                break;
            }
            v[k] = &verts[vi];
            if (!(v[k]->pos[3] > 1e-6f)) {
                ok = false;
            }
        }
        ++stats.tris;
        if (!ok) {
            ++stats.clipped;
            continue;
        }
        float sx[3], sy[3], sz[3], iw[3];
        for (int k = 0; k < 3; ++k) {
            iw[k] = 1.0f / v[k]->pos[3];
            const float nx = v[k]->pos[0] * iw[k], ny = v[k]->pos[1] * iw[k], nz = v[k]->pos[2] * iw[k];
            sx[k] = vx + (nx * 0.5f + 0.5f) * vw;
            sy[k] = vy + (0.5f - ny * 0.5f) * vh;
            sz[k] = zn + (nz * 0.5f + 0.5f) * (zf - zn);
        }
        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (std::fabs(area) < 1e-9f) {
            continue;
        }
        const int x0 = std::max(cx0, static_cast<int>(std::floor(std::min({sx[0], sx[1], sx[2]}))));
        const int x1 = std::min(cx1 - 1, static_cast<int>(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
        const int y0 = std::max(cy0, static_cast<int>(std::floor(std::min({sy[0], sy[1], sy[2]}))));
        const int y1 = std::min(cy1 - 1, static_cast<int>(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                float b[3];
                b[0] = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                b[1] = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                b[2] = 1.0f - b[0] - b[1];
                if (b[0] < -1e-5f || b[1] < -1e-5f || b[2] < -1e-5f) {
                    continue;
                }
                const float z = b[0] * sz[0] + b[1] * sz[1] + b[2] * sz[2];
                float pw[3] = {b[0] * iw[0], b[1] * iw[1], b[2] * iw[2]};
                const float s = pw[0] + pw[1] + pw[2];
                for (float& p : pw) p /= s;
                PixelIn in{};
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t cc[2] = {v[k]->c0, v[k]->c1};
                    for (int ch = 0; ch < 2; ++ch) {
                        for (int c = 0; c < 4; ++c) {
                            in.rast[ch][c] += pw[k] * static_cast<float>((cc[ch] >> (8 * c)) & 0xFF) / 255.0f;
                        }
                    }
                    for (unsigned g = 0; g < kMaxTexGens; ++g) {
                        for (int c = 0; c < 3; ++c) {
                            in.tc[g][c] += pw[k] * v[k]->tc[g][c];
                        }
                    }
                }
                const std::size_t pi = static_cast<std::size_t>(y) * efb.w + x;
                const bool early = d.zcomploc != 0;
                if (early) {
                    if (d.depth_test && !depth_pass(d.depth_func, z, efb.depth[pi])) {
                        ++stats.depth_failed;
                        continue;
                    }
                    if (d.depth_write) {
                        efb.depth[pi] = z;
                    }
                }
                int src[4];
                if (!eval_tev(d, units, in, src, &stats.flags)) {
                    ++stats.discarded;
                    continue;
                }
                if (!early) {
                    if (d.depth_test && !depth_pass(d.depth_func, z, efb.depth[pi])) {
                        ++stats.depth_failed;
                        continue;
                    }
                    if (d.depth_write) {
                        efb.depth[pi] = z;
                    }
                }
                int* dst = &efb.color[pi * 4];
                int res[4];
                switch (d.blend_mode) {
                case BlendMix:
                    for (int c = 0; c < 4; ++c) {
                        const int fs = factor(d.blend_src, true, src, dst, c);
                        const int fd = factor(d.blend_dst, false, src, dst, c);
                        res[c] = std::min(255, (src[c] * fs + dst[c] * fd + 127) / 255);
                    }
                    break;
                case BlendSubtract:
                    for (int c = 0; c < 4; ++c) res[c] = std::max(0, dst[c] - src[c]);
                    break;
                case BlendLogic:
                    for (int c = 0; c < 4; ++c) res[c] = logic(d.logic, src[c], dst[c]);
                    break;
                default:
                    for (int c = 0; c < 4; ++c) res[c] = src[c];
                    break;
                }
                const bool has_alpha = d.pixel_fmt == 1;
                if (d.dst_alpha_en && has_alpha) {
                    res[3] = static_cast<int>(d.dst_alpha);
                }
                if (x == g_trace_x && y == g_trace_y) {
                    std::printf("trace D%ld src=%d,%d,%d,%d dst=%d,%d,%d,%d -> %d,%d,%d,%d rast0=%.3f,%.3f,%.3f,%.3f "
                                "tc0=%.4f,%.4f\n",
                                g_cur_draw, src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], res[0], res[1],
                                res[2], res[3], in.rast[0][0], in.rast[0][1], in.rast[0][2], in.rast[0][3], in.tc[0][0],
                                in.tc[0][1]);
                }
                if (d.color_update) {
                    dst[0] = res[0]; dst[1] = res[1]; dst[2] = res[2];
                }
                if (d.alpha_update && has_alpha) {
                    dst[3] = res[3];
                }
                ++stats.pixels;
                stats.minx = std::min(stats.minx, x); stats.miny = std::min(stats.miny, y);
                stats.maxx = std::max(stats.maxx, x); stats.maxy = std::max(stats.maxy, y);
            }
        }
    }
}

void write_ppm(const std::string& path, int w, int h, const std::vector<int>& rgba) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char p[3] = {static_cast<unsigned char>(std::clamp(rgba[i * 4], 0, 255)),
                                    static_cast<unsigned char>(std::clamp(rgba[i * 4 + 1], 0, 255)),
                                    static_cast<unsigned char>(std::clamp(rgba[i * 4 + 2], 0, 255))};
        std::fwrite(p, 1, 3, f);
    }
    std::fclose(f);
}

const char* cc_name(unsigned a) {
    static const char* n[16] = {"CPREV", "APREV", "C0", "A0", "C1", "A1", "C2", "A2",
                                "TEXC", "TEXA", "RASC", "RASA", "ONE", "HALF", "KONST", "ZERO"};
    return n[a & 15];
}
const char* ca_name(unsigned a) {
    static const char* n[8] = {"APREV", "A0", "A1", "A2", "TEXA", "RASA", "KONST", "ZERO"};
    return n[a & 7];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <capture dir> [out dir] [--last N]\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    std::string out = argc > 2 && argv[2][0] != '-' ? argv[2] : dir;
    long last = -1;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--last") == 0) last = std::strtol(argv[i + 1], nullptr, 10);
    }
    if (const char* t = std::getenv("MKWV_TRACE")) {
        std::sscanf(t, "%d,%d", &g_trace_x, &g_trace_y);
    }
    Capture cap;
    if (!load_capture(dir, cap)) return 1;

    // Textures.
    std::map<std::string, Image> images;
    for (const RDraw& d : cap.draws) {
        for (const RTex& t : d.tex) {
            if (t.file == "-" || images.count(t.file)) continue;
            Image im;
            std::vector<std::uint8_t> raw;
            if (read_file(dir + "/" + t.file, raw) && raw.size() >= static_cast<std::size_t>(t.w) * t.h * 4) {
                im.w = static_cast<int>(t.w);
                im.h = static_cast<int>(t.h);
                im.px.assign(raw.begin(), raw.begin() + static_cast<long>(t.w) * t.h * 4);
            }
            images[t.file] = im;
            std::vector<int> rgba(im.px.begin(), im.px.end());
            std::string base = t.file.substr(0, t.file.find('.'));
            write_ppm(out + "/" + base + ".ppm", im.w, im.h, rgba);
            std::vector<int> alpha(rgba.size());
            for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) alpha[i] = alpha[i + 1] = alpha[i + 2] = rgba[i + 3];
            write_ppm(out + "/" + base + "_alpha.ppm", im.w, im.h, alpha);
        }
    }

    // EFB extent: the union of the viewports, as the renderer does.
    int ew = 640, eh = 480;
    {
        float r = 0, b = 0;
        for (const RDraw& d : cap.draws) {
            if (d.has_vp && d.vp[2] > 1 && d.vp[3] > 1) {
                r = std::max(r, d.vp[0] + d.vp[2]);
                b = std::max(b, d.vp[1] + d.vp[3]);
            }
        }
        if (r > 1 && b > 1) {
            ew = static_cast<int>(std::ceil(r));
            eh = static_cast<int>(std::ceil(b));
        }
    }
    Efb efb;
    // MKWV_REPLAY_CLEAR=RRGGBBAA-style hex (r | g<<8 | b<<16 | a<<24) overrides the frame's clear,
    // e.g. to model the renderer's clear_efb_now (alpha 1).
    std::uint32_t clear_rgba = cap.clear_rgba;
    if (const char* ov = std::getenv("MKWV_REPLAY_CLEAR")) clear_rgba = static_cast<std::uint32_t>(std::strtoul(ov, nullptr, 16));
    efb.init(ew, eh, clear_rgba);
    std::map<std::uint32_t, Image> copies;
    const bool gpu_model = std::getenv("MKWV_REPLAY_GPU") != nullptr;
    std::map<std::uint32_t, GpuCopy> gpu_copies;

    std::FILE* rep = std::fopen((out + "/report.txt").c_str(), "w");
    std::map<std::uint64_t, int> programs;
    const long total = static_cast<long>(cap.draws.size());
    std::size_t copy_i = 0;
    for (long di = 0; di < total; ++di) {
        const RDraw& d = cap.draws[di];
        // EFB copies scheduled before this draw.
        while (copy_i < cap.copies.size() &&
               (cap.copies[copy_i].batch < d.batch ||
                (cap.copies[copy_i].batch == d.batch && cap.copies[copy_i].draw_index <= d.index))) {
            const RCopy& c = cap.copies[copy_i++];
            Image im;
            im.w = static_cast<int>(c.dw);
            im.h = static_cast<int>(c.dh);
            im.px.assign(static_cast<std::size_t>(im.w) * im.h * 4, 0);
            for (int y = 0; y < im.h; ++y) {
                for (int x = 0; x < im.w; ++x) {
                    const int sx = c.sx + x * std::max(1, c.sw) / std::max(1, im.w);
                    const int sy = c.sy + y * std::max(1, c.sh) / std::max(1, im.h);
                    if (sx < 0 || sy < 0 || sx >= efb.w || sy >= efb.h) continue;
                    for (int ch = 0; ch < 4; ++ch) {
                        im.px[(static_cast<std::size_t>(y) * im.w + x) * 4 + ch] =
                            static_cast<std::uint8_t>(efb.color[(static_cast<std::size_t>(sy) * efb.w + sx) * 4 + ch]);
                    }
                }
            }
            copies[c.dest & 0x1FFFFFFFu] = im;
            if (gpu_model) {
                GpuCopy gc;
                gc.fmt = c.fmt;
                gc.img.w = im.w;
                gc.img.h = im.h;
                gc.img.px.assign(im.px.size(), 0);
                for (int y = 0; y < im.h; ++y) {
                    const int y0 = c.sy + y * c.sh / im.h, y1 = std::max(y0 + 1, c.sy + (y + 1) * c.sh / im.h);
                    for (int x = 0; x < im.w; ++x) {
                        const int x0 = c.sx + x * c.sw / im.w, x1 = std::max(x0 + 1, c.sx + (x + 1) * c.sw / im.w);
                        float acc[4] = {0, 0, 0, 0};
                        float dacc = 0.0f;
                        int n = 0;
                        for (int yy = y0; yy < y1; ++yy) {
                            for (int xx = x0; xx < x1; ++xx) {
                                if (xx < 0 || yy < 0 || xx >= efb.w || yy >= efb.h) continue;
                                const std::size_t i = static_cast<std::size_t>(yy) * efb.w + xx;
                                for (int ch = 0; ch < 4; ++ch) acc[ch] += static_cast<float>(efb.color[i * 4 + ch]);
                                dacc += efb.depth[i];
                                ++n;
                            }
                        }
                        if (n == 0) n = 1;
                        for (float& v : acc) v /= static_cast<float>(n);
                        convert_copy_px(c.fmt, acc, dacc / static_cast<float>(n),
                                        &gc.img.px[(static_cast<std::size_t>(y) * im.w + x) * 4]);
                    }
                }
                gpu_copies[c.dest & 0x1FFFFFFFu] = gc;
            }
            std::fprintf(rep, "COPY before draw %u.%u src=%d,%d %dx%d dst=%ux%u fmt=%u clear=%d dest=%08X\n", c.batch,
                         c.draw_index, c.sx, c.sy, c.sw, c.sh, c.dw, c.dh, c.fmt, c.clear, c.dest);
            if (c.clear) {
                for (int y = std::max(0, c.sy); y < std::min(efb.h, c.sy + c.sh); ++y) {
                    for (int x = std::max(0, c.sx); x < std::min(efb.w, c.sx + c.sw); ++x) {
                        for (int ch = 0; ch < 4; ++ch) {
                            if (ch == 3 ? (c.clear_alpha && !std::getenv("MKWV_REPLAY_NO_ALPHA_CLEAR")) : c.clear_color)
                                efb.color[(static_cast<std::size_t>(y) * efb.w + x) * 4 + ch] =
                                    static_cast<int>((c.clear_rgba >> (8 * ch)) & 0xFF);
                        }
                        if (c.clear_depth) efb.depth[static_cast<std::size_t>(y) * efb.w + x] = c.clear_z / 16777215.0f;
                    }
                }
            }
        }
        if (last >= 0 && di >= last) break;
        TexUnit units[kMaxTexMaps];
        std::string texdesc;
        for (const RTex& t : d.tex) {
            TexUnit& u = units[t.map & 7];
            u.ws = t.ws;
            u.wt = t.wt;
            u.linear = t.magf != TexNear;
            auto it = images.find(t.file);
            auto git = gpu_model ? gpu_copies.find(t.phys & 0x1FFFFFFFu) : gpu_copies.end();
            if (git != gpu_copies.end() && static_cast<unsigned>(git->second.img.w) == t.w &&
                static_cast<unsigned>(git->second.img.h) == t.h &&
                (copy_sample_fmt(git->second.fmt) == (t.fmt & 0xFu) ||
                 (copy_sample_fmt(git->second.fmt) == 1u && t.fmt == 9u))) {
                u.img = &git->second.img;
            } else if (t.file != "-" && it != images.end() && it->second.w != 0) {
                u.img = &it->second;
            } else {
                auto cit = copies.find(t.phys & 0x1FFFFFFFu);
                if (cit != copies.end()) u.img = &cit->second;
            }
            char b[160];
            std::snprintf(b, sizeof(b), " map%u=%ux%u f%u w%u/%u %s%s", t.map, t.w, t.h, t.fmt, t.ws, t.wt,
                          t.file.c_str(), u.img == nullptr ? "(MISSING)" : "");
            texdesc += b;
        }
        Stats stats;
        g_cur_draw = di;
        const auto& verts = d.batch < cap.batches.size() ? cap.batches[d.batch] : cap.batches[0];
        raster_draw(efb, d, verts, units, stats);
        const std::uint64_t key = hash_tev_program(d.st);
        int prog = 0;
        auto pit = programs.find(key);
        if (pit == programs.end()) {
            prog = static_cast<int>(programs.size());
            programs[key] = prog;
            static char src[65536];
            std::size_t n = 0;
            if (emit_tev_fragment_cg(d.st, src, sizeof(src), &n)) {
                char path[64];
                std::snprintf(path, sizeof(path), "/prog_%03d.cg", prog);
                if (std::FILE* pf = std::fopen((out + path).c_str(), "w")) {
                    std::fwrite(src, 1, n, pf);
                    std::fclose(pf);
                }
            }
        } else {
            prog = pit->second;
        }
        std::fprintf(rep,
                     "D%ld b%u.%u prim=%02X n=%u px=%u bbox=%d,%d-%d,%d disc=%u zfail=%u tris=%u clip=%u "
                     "blend=%u(%u,%u,lo%u) z=%d%d f%u cu=%d au=%d pf=%u dsta=%d/%u stages=%u tg=%u ch=%u prog=%d%s%s%s\n",
                     di, d.batch, d.index, d.prim, d.count, stats.pixels, stats.minx, stats.miny, stats.maxx, stats.maxy,
                     stats.discarded, stats.depth_failed, stats.tris, stats.clipped, d.blend_mode, d.blend_src,
                     d.blend_dst, d.logic, d.depth_test, d.depth_write, d.depth_func, d.color_update, d.alpha_update,
                     d.pixel_fmt, d.dst_alpha_en, d.dst_alpha, d.st.num_tev_stages, d.st.num_tex_gens, d.num_chans, prog,
                     (stats.flags & 1u) ? " IND" : "", (stats.flags & 2u) ? " FOG" : "", texdesc.c_str());
        const unsigned stages = d.st.num_tev_stages == 0 ? 1u : d.st.num_tev_stages;
        for (unsigned s = 0; s < stages && s < kTevStages; ++s) {
            const TevStage& t = d.st.tev[s];
            std::fprintf(rep,
                         "    s%u map%u tc%u ras%u  C: a=%s b=%s c=%s d=%s op%u bias%u sh%u cl%d ->%u   A: a=%s b=%s "
                         "c=%s d=%s op%u bias%u sh%u cl%d ->%u  k=%02X/%02X swap r%u t%u\n",
                         s, t.texmap, t.texcoord, t.ras_chan, cc_name(t.color_a), cc_name(t.color_b), cc_name(t.color_c),
                         cc_name(t.color_d), t.color_op, t.color_bias, t.color_shift, t.color_clamp ? 1 : 0, t.color_dest,
                         ca_name(t.alpha_a), ca_name(t.alpha_b), ca_name(t.alpha_c), ca_name(t.alpha_d), t.alpha_op,
                         t.alpha_bias, t.alpha_shift, t.alpha_clamp ? 1 : 0, t.alpha_dest, t.kcsel, t.kasel, t.ras_swap,
                         t.tex_swap);
        }
        std::fprintf(rep, "    regs prev=%d,%d,%d,%d r0=%d,%d,%d,%d r1=%d,%d,%d,%d r2=%d,%d,%d,%d konst=%08X %08X %08X %08X "
                          "acmp=%u:%u %u %u:%u\n",
                     d.tev_reg[0][0], d.tev_reg[0][1], d.tev_reg[0][2], d.tev_reg[0][3], d.tev_reg[1][0], d.tev_reg[1][1],
                     d.tev_reg[1][2], d.tev_reg[1][3], d.tev_reg[2][0], d.tev_reg[2][1], d.tev_reg[2][2], d.tev_reg[2][3],
                     d.tev_reg[3][0], d.tev_reg[3][1], d.tev_reg[3][2], d.tev_reg[3][3], d.st.konst[0], d.st.konst[1],
                     d.st.konst[2], d.st.konst[3], d.st.alpha_comp0, d.st.alpha_ref0, d.st.alpha_op, d.st.alpha_comp1,
                     d.st.alpha_ref1);
        if (d.count > 0 && d.first < verts.size()) {
            const Vtx& v = verts[d.first];
            std::fprintf(rep, "    v0 pos=%.4g,%.4g,%.4g,%.4g c0=%08X c1=%08X tc0=%.4g,%.4g,%.4g tc1=%.4g,%.4g\n", v.pos[0],
                         v.pos[1], v.pos[2], v.pos[3], v.c0, v.c1, v.tc[0][0], v.tc[0][1], v.tc[0][2], v.tc[1][0],
                         v.tc[1][1]);
        }
    }
    std::fclose(rep);
    write_ppm(out + "/replay.ppm", efb.w, efb.h, efb.color);
    // The EFB alpha as a greyscale image (what DSTALPHA blends read).
    if (std::FILE* af = std::fopen((out + "/replay_alpha.pgm").c_str(), "wb")) {
        std::fprintf(af, "P5\n%d %d\n255\n", efb.w, efb.h);
        for (int i = 0; i < efb.w * efb.h; ++i) {
            const unsigned char a = static_cast<unsigned char>(std::clamp(efb.color[i * 4 + 3], 0, 255));
            std::fwrite(&a, 1, 1, af);
        }
        std::fclose(af);
    }
    std::printf("replayed %zu draws, %zu copies, %zu textures, %zu programs -> %s/replay.ppm (%dx%d)\n", cap.draws.size(),
                cap.copies.size(), images.size(), programs.size(), out.c_str(), efb.w, efb.h);
    return 0;
}
