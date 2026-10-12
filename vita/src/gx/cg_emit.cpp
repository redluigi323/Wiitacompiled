#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/tev.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mkwv::gx {
namespace {

// Exact TEV (GX's 8-bit integer arithmetic, Aurora's formulas): every input is rounded and
// wrapped to a byte and every combiner floors. That is ~50 ALU instructions a stage per pixel,
// and a real Vita's GPU is ALU-bound on it (static59 menu: 50 ms of swap for 162 draws). The
// fast form is the same combiner in plain float: inputs that can leave [0,1] (PREV and the
// colour registers) are saturated instead of wrapped, and nothing is rounded. Off-by-one
// differences in the low bit are expected; a register holding an out-of-range value wraps
// differently. set_tev_exact(true) (marker exact_tev) restores the exact form.
bool g_tev_exact = true;

struct Writer {
    char* cur = nullptr;
    char* end = nullptr;
    bool ok = true;

    void append(const char* text) {
        if (!ok || text == nullptr) {
            return;
        }
        const std::size_t len = std::strlen(text);
        if (cur + len >= end) {
            ok = false;
            return;
        }
        std::memcpy(cur, text, len);
        cur += len;
    }

    void line(const char* fmt, ...) {
        if (!ok) {
            return;
        }
        char buf[512];
        va_list args;
        va_start(args, fmt);
        const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n < 0) {
            ok = false;
            return;
        }
        append(buf);
        append("\n");
    }
};

// KCSEL / KASEL resolve to an *expression*, not a value. The selector itself is
// structural - it says "the 6/8 constant" or "konst register 2's green channel" - so it
// belongs in the shader source, while the register contents are a uniform. Baking the
// resolved bytes in (which is what this emitter used to do) made the source depend on
// colour values, so every palette change would have been a fresh shader compile.
void konst_rgb_expr(std::uint8_t sel, char* buf, std::size_t cap) {
    static constexpr char kCh[] = "rgba";
    if (sel <= 7u) {
        const float v = static_cast<float>(8u - sel) / 8.0f;
        std::snprintf(buf, cap, "float3(%f, %f, %f)", v, v, v);
        return;
    }
    if (sel >= 0x0Cu && sel <= 0x0Fu) {
        std::snprintf(buf, cap, "uKonst[%u].rgb", static_cast<unsigned>(sel - 0x0Cu));
        return;
    }
    if (sel >= 0x10u && sel <= 0x1Fu) {
        const unsigned id = (sel - 0x10u) & 3u;
        const unsigned ch = (sel - 0x10u) / 4u;
        const char c = kCh[ch & 3u];
        std::snprintf(buf, cap, "uKonst[%u].%c%c%c", id, c, c, c);
        return;
    }
    std::snprintf(buf, cap, "float3(1.0, 1.0, 1.0)");
}

void konst_a_expr(std::uint8_t sel, char* buf, std::size_t cap) {
    static constexpr char kCh[] = "rgba";
    if (sel <= 7u) {
        std::snprintf(buf, cap, "%f", static_cast<float>(8u - sel) / 8.0f);
        return;
    }
    if (sel >= 0x10u && sel <= 0x1Fu) {
        const unsigned id = (sel - 0x10u) & 3u;
        const unsigned ch = (sel - 0x10u) / 4u;
        std::snprintf(buf, cap, "uKonst[%u].%c", id, kCh[ch & 3u]);
        return;
    }
    std::snprintf(buf, cap, "1.0");
}


struct ParamList {
    char text[4096]{};
    std::size_t len = 0;
    bool ok = true;

    void add(const char* fmt, ...) {
        if (!ok) {
            return;
        }
        char buf[192];
        va_list args;
        va_start(args, fmt);
        const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n < 0) {
            ok = false;
            return;
        }
        const char* sep = (len == 0) ? "    " : ",\n    ";
        const std::size_t need = std::strlen(sep) + std::strlen(buf);
        if (len + need + 1 >= sizeof(text)) {
            ok = false;
            return;
        }
        std::memcpy(text + len, sep, std::strlen(sep));
        len += std::strlen(sep);
        std::memcpy(text + len, buf, std::strlen(buf));
        len += std::strlen(buf);
        text[len] = '\0';
    }
};

bool uses_indirect(const TevStage& s) {
    return s.ind_mtx != kIndMtxOff || s.ind_wrap_s != IndWrapOff || s.ind_wrap_t != IndWrapOff || s.ind_add_prev;
}

// A colour-combiner input as an expression over the per-stage locals.
const char* color_arg(std::uint8_t arg) {
    switch (arg) {
    case CcCPrev:
        return "prev.rgb";
    case CcAPrev:
        return "prev.aaa";
    case CcC0:
        return "reg0.rgb";
    case CcA0:
        return "reg0.aaa";
    case CcC1:
        return "reg1.rgb";
    case CcA1:
        return "reg1.aaa";
    case CcC2:
        return "reg2.rgb";
    case CcA2:
        return "reg2.aaa";
    case CcTexC:
        return "tex_s.rgb";
    case CcTexA:
        return "tex_s.aaa";
    case CcRasC:
        return "ras_s.rgb";
    case CcRasA:
        return "ras_s.aaa";
    case CcOne:
        return "float3(1.0, 1.0, 1.0)";
    case CcHalf:
        return "float3(0.5, 0.5, 0.5)";
    case CcKonst:
        return "kc";
    default:
        return "float3(0.0, 0.0, 0.0)";
    }
}

const char* alpha_arg(std::uint8_t arg) {
    switch (arg) {
    case CaAPrev:
        return "prev.a";
    case CaA0:
        return "reg0.a";
    case CaA1:
        return "reg1.a";
    case CaA2:
        return "reg2.a";
    case CaTexA:
        return "tex_s.a";
    case CaRasA:
        return "ras_s.a";
    case CaKonst:
        return "ka";
    default:
        return "0.0";
    }
}

// GX writes a stage's output into PREV or one of the three colour registers. Those
// registers start at their uniform values and are then written by earlier stages, so
// they are mutable locals seeded from the uniform, not the uniform itself.
const char* dest_name(std::uint8_t dest) {
    switch (dest) {
    case 1:
        return "reg0";
    case 2:
        return "reg1";
    case 3:
        return "reg2";
    default:
        return "prev";
    }
}

void emit_swap4(Writer& w, const char* dst, const char* src, const std::uint8_t table[4]) {
    static constexpr char kCh[] = "rgba";
    w.line("        float4 %s = %s.%c%c%c%c;", dst, src, kCh[table[0] & 3u], kCh[table[1] & 3u], kCh[table[2] & 3u],
           kCh[table[3] & 3u]);
}

// The regular combiner, in GX's own 8-bit arithmetic (Aurora tev_regular_i32):
//   inputs a/b/c wrap to a byte, c' = c + (c >> 7),
//   lerp = ((a << 8) + (b - a) * c') * scale + (sub ? 127 : 128)  [no rounding for /2]
//   out  = (d + bias) * scale +/- (lerp >> 8), then >> 1 for DIVIDE_2.
// Everything is in float here - the values stay far inside float's exact-integer range.
void emit_regular(Writer& w, const char* out, const char* type, std::uint8_t op, std::uint8_t bias,
                  std::uint8_t shift) {
    const bool sub = op == TevSub;
    const float bias_v = bias == 1 ? 128.0f : (bias == 2 ? -128.0f : 0.0f);
    const float mul = shift == 1 ? 2.0f : (shift == 2 ? 4.0f : 1.0f);
    const float rnd = shift == 3 ? 0.0f : (sub ? 127.0f : 128.0f);
    w.line("        %s cp_%s = C_%s + floor(C_%s / 128.0);", type, out, out, out);
    w.line("        %s lp_%s = (A_%s * 256.0 + (B_%s - A_%s) * cp_%s) * %.1f + %.1f;", type, out, out, out, out, out,
           mul, rnd);
    w.line("        %s r_%s = (D_%s + %.1f) * %.1f %s floor(lp_%s / 256.0);", type, out, out, bias_v, mul,
           sub ? "-" : "+", out);
    if (shift == 3) {
        w.line("        r_%s = floor(r_%s * 0.5);", out, out);
    }
    w.line("        %s %s = r_%s / 255.0;", type, out, out);
}

// The comparison ops (Aurora tev_op): on 8-bit values; GR16 and BGR24 pack two or three
// channels into one integer before comparing. `A`/`B` are byte-space expressions.
void compare_cond(std::uint8_t op, const char* A, const char* B, char* out, std::size_t cap) {
    const bool eq = (op & 1u) != 0;
    const char* rel = eq ? "==" : ">";
    switch (op & ~1u) {
    case TevCompR8Gt:
        std::snprintf(out, cap, "(%s.r %s %s.r)", A, rel, B);
        return;
    case TevCompGr16Gt:
        std::snprintf(out, cap, "(dot(%s.rg, float2(1.0, 256.0)) %s dot(%s.rg, float2(1.0, 256.0)))", A, rel, B);
        return;
    case TevCompBgr24Gt:
        std::snprintf(out, cap, "(dot(%s.rgb, float3(1.0, 256.0, 65536.0)) %s dot(%s.rgb, float3(1.0, 256.0, 65536.0)))",
                      A, rel, B);
        return;
    default:
        std::snprintf(out, cap, "(%s %s %s)", A, rel, B);
        return;
    }
}

// Alpha compare with Aurora's constant folding (shader.cpp:523-557): -1 not constant,
// 0 always false, 1 always true.
int alpha_cmp_const(Compare comp, std::uint8_t ref) {
    switch (comp) {
    case Never:
        return 0;
    case Less:
        return ref == 0 ? 0 : -1;
    case LEqual:
        return ref == 255 ? 1 : -1;
    case GEqual:
        return ref == 0 ? 1 : -1;
    case Greater:
        return ref == 255 ? 0 : -1;
    case Equal:
    case NEqual:
        return -1;
    case Always:
    default:
        return 1;
    }
}

void alpha_cmp_expr(Compare comp, std::uint8_t ref, char* out, std::size_t cap) {
    const char* rel = "==";
    switch (comp) {
    case Less:
        rel = "<";
        break;
    case LEqual:
        rel = "<=";
        break;
    case Equal:
        rel = "==";
        break;
    case NEqual:
        rel = "!=";
        break;
    case GEqual:
        rel = ">=";
        break;
    case Greater:
        rel = ">";
        break;
    default:
        break;
    }
    std::snprintf(out, cap, "(acmp %s %u.0)", rel, static_cast<unsigned>(ref));
}

// Fast combiner inputs: only PREV and the colour registers can hold values outside [0, 1].
bool color_arg_in_range(std::uint8_t arg) {
    return !(arg == CcCPrev || arg == CcAPrev || arg == CcC0 || arg == CcA0 || arg == CcC1 || arg == CcA1 ||
             arg == CcC2 || arg == CcA2);
}

bool alpha_arg_in_range(std::uint8_t arg) {
    return !(arg == CaAPrev || arg == CaA0 || arg == CaA1 || arg == CaA2);
}

// GX compare ops in float: bytes compare as values half a step apart.
void fast_compare_cond(std::uint8_t op, const char* A, const char* B, bool scalar, char* out, std::size_t cap) {
    const bool eq = (op & 1u) != 0;
    char a[96], b[96];
    switch (op & ~1u) {
    case TevCompR8Gt:
        std::snprintf(a, sizeof(a), "%s.r", A);
        std::snprintf(b, sizeof(b), "%s.r", B);
        break;
    case TevCompGr16Gt:
        std::snprintf(a, sizeof(a), "dot(round(%s.rg * 255.0), float2(1.0, 256.0))", A);
        std::snprintf(b, sizeof(b), "dot(round(%s.rg * 255.0), float2(1.0, 256.0))", B);
        std::snprintf(out, cap, eq ? "(%s == %s)" : "(%s > %s)", a, b);
        return;
    case TevCompBgr24Gt:
        std::snprintf(a, sizeof(a), "dot(round(%s.rgb * 255.0), float3(1.0, 256.0, 65536.0))", A);
        std::snprintf(b, sizeof(b), "dot(round(%s.rgb * 255.0), float3(1.0, 256.0, 65536.0))", B);
        std::snprintf(out, cap, eq ? "(%s == %s)" : "(%s > %s)", a, b);
        return;
    default:  // RGB8 per channel (colour) / A8 (alpha)
        std::snprintf(a, sizeof(a), "%s", A);
        std::snprintf(b, sizeof(b), "%s", B);
        break;
    }
    (void)scalar;
    if (eq) {
        std::snprintf(out, cap, "(abs(%s - %s) < (0.5 / 255.0))", a, b);
    } else {
        std::snprintf(out, cap, "((%s - %s) > (0.5 / 255.0))", a, b);
    }
}

// One fast combiner (colour when `vec`, else alpha). `names` are the four argument
// expressions a..d, `in_range` whether each is known to stay inside [0, 1].
void emit_fast_combiner(Writer& w, bool vec, const char* out, const char* const names[4], const bool in_range[4],
                        std::uint8_t op, std::uint8_t bias, std::uint8_t shift, bool clamp) {
    const char* type = vec ? "float3" : "float";
    const char* zero = vec ? "float3(0.0, 0.0, 0.0)" : "0.0";
    static const char* kSuffix[4] = {"a", "b", "c", "d"};
    for (unsigned k = 0; k < 3u; ++k) {
        w.line(in_range[k] ? "        %s %s_%s = %s;" : "        %s %s_%s = saturate(%s);", type, out, kSuffix[k],
               names[k]);
    }
    w.line("        %s %s_d = %s;", type, out, names[3]);
    if (op >= TevCompR8Gt) {
        char a[16], b[16], cond[512];
        std::snprintf(a, sizeof(a), "%s_a", out);
        std::snprintf(b, sizeof(b), "%s_b", out);
        if (!vec && (op & ~1u) != TevCompRgb8Gt) {
            // R8/GR16/BGR24 in the alpha combiner compare the colour combiner's A and B.
            std::snprintf(a, sizeof(a), "oc_a3");
            std::snprintf(b, sizeof(b), "oc_b3");
        } else if (vec && (op & ~1u) != TevCompRgb8Gt) {
            std::snprintf(a, sizeof(a), "oc_a3");
            std::snprintf(b, sizeof(b), "oc_b3");
        }
        fast_compare_cond(op, a, b, !vec, cond, sizeof(cond));
        w.line("        %s %s = %s_d + (%s ? %s_c : %s);", type, out, out, cond, out, zero);
    } else {
        const char* bias_v = bias == 1 ? " + (128.0 / 255.0)" : (bias == 2 ? " - (128.0 / 255.0)" : "");
        const char* scale = shift == 1 ? " * 2.0" : (shift == 2 ? " * 4.0" : (shift == 3 ? " * 0.5" : ""));
        w.line("        %s %s = ((%s_d%s) %s lerp(%s_a, %s_b, %s_c))%s;", type, out, out, bias_v,
               op == TevSub ? "-" : "+", out, out, out, scale);
    }
    w.line(clamp ? "        %s = saturate(%s);" : "        %s = clamp(%s, -1024.0 / 255.0, 1023.0 / 255.0);", out, out);
}

void finish(Writer& w, char* out, std::size_t* written) {
    if (w.ok && w.cur < w.end) {
        *w.cur = '\0';
    }
    if (written) {
        *written = w.ok ? static_cast<std::size_t>(w.cur - out) : 0;
    }
}

} // namespace

int tev_effective_texcoord(const State& state, std::uint8_t texcoord) {
    if (state.num_tex_gens == 0) {
        return -1;
    }
    return texcoord < state.num_tex_gens && texcoord < kEmitTexCoordSets ? static_cast<int>(texcoord) : 0;
}

bool tev_stage_can_sample(const State& state, const TevStage& stage) {
    return tev_effective_texcoord(state, stage.texcoord) >= 0 && stage.tex_enabled && stage.texmap < kMaxTexMaps;
}

ShaderNeeds analyze_tev(const State& state) {
    ShaderNeeds n;
    n.stages = state.num_tev_stages == 0 ? 1u : static_cast<unsigned>(state.num_tev_stages);
    if (n.stages > kTevStages) {
        n.stages = kTevStages;
    }
    n.num_tex_gens = state.num_tex_gens > kMaxTexGens ? kMaxTexGens : state.num_tex_gens;
    auto use_coord = [&](int c) {
        if (c >= 0 && c < static_cast<int>(kEmitTexCoordSets)) {
            n.coord_used[c] = true;
            n.coord_proj[c] = state.texgens[c].type == TgMtx3x4;
        }
    };
    for (unsigned i = 0; i < n.stages; ++i) {
        const TevStage& s = state.tev[i];
        const std::uint8_t ca[4] = {s.color_a, s.color_b, s.color_c, s.color_d};
        const std::uint8_t aa[4] = {s.alpha_a, s.alpha_b, s.alpha_c, s.alpha_d};
        bool reads_ras = false;
        for (unsigned k = 0; k < 4u; ++k) {
            reads_ras = reads_ras || ca[k] == CcRasC || ca[k] == CcRasA || aa[k] == CaRasA;
        }
        if (reads_ras) {
            if (s.ras_chan == kRasColor0) {
                n.color0 = true;
            } else if (s.ras_chan == kRasColor1) {
                n.color1 = true;
            }
        }
        if (tev_stage_can_sample(state, s)) {
            n.tex_used[s.texmap] = true;
            use_coord(tev_effective_texcoord(state, s.texcoord));
        }
        if (uses_indirect(s) && s.ind_stage < state.num_ind_stages) {
            n.indirect = true;
            const IndStage& is = state.ind[s.ind_stage & 3u];
            if (is.texmap < kMaxTexMaps && state.num_tex_gens != 0) {
                n.tex_used[is.texmap] = true;
                use_coord(tev_effective_texcoord(state, is.texcoord));
            }
        } else if (s.ind_add_prev || s.ras_chan == kRasAlphaBump || s.ras_chan == kRasAlphaBumpN) {
            n.indirect = n.indirect || s.ind_add_prev;
        }
        const bool kc_reads_reg = s.kcsel >= 0x0Cu && s.kcsel <= 0x1Fu;
        const bool ka_reads_reg = s.kasel >= 0x10u && s.kasel <= 0x1Fu;
        for (unsigned k = 0; k < 4u; ++k) {
            if ((ca[k] == CcKonst && kc_reads_reg) || (aa[k] == CaKonst && ka_reads_reg)) {
                n.konst = true;
            }
        }
    }
    n.tevreg = true;  // uTevReg[0] seeds prev on every program
    const int c0 = alpha_cmp_const(state.alpha_comp0, state.alpha_ref0);
    const int c1 = alpha_cmp_const(state.alpha_comp1, state.alpha_ref1);
    // Only a combination that can actually discard needs the test.
    n.alpha_test = !(state.alpha_op == 0 && c0 == 1 && c1 == 1) && !(state.alpha_op == 1 && (c0 == 1 || c1 == 1)) &&
                   !(c0 != -1 && c1 != -1 && [&] {
                       const bool a = c0 == 1, b = c1 == 1;
                       switch (state.alpha_op) {
                       case 0:
                           return a && b;
                       case 1:
                           return a || b;
                       case 2:
                           return a != b;
                       default:
                           return a == b;
                       }
                   }());
    n.fog = state.fog.type != FogNone;
    // Fog's z/w rides in the first free TEXCOORD. Most programs sample one or two coordinates,
    // so this stays inside TEXCOORD0-7, which every Cg profile accepts; 8 only when all eight
    // texcoords are in use.
    n.fog_slot = kEmitTexCoordSets;
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (!n.coord_used[c]) {
            n.fog_slot = c;
            break;
        }
    }
    n.dst_alpha = state.dst_alpha_enable;
    return n;
}

bool emit_tev_vertex_cg(const ShaderNeeds& needs, char* out, std::size_t cap, std::size_t* written) {
    if (!out || cap < 8) {
        return false;
    }
    ParamList params;
    params.add("float4 aPosition");
    if (needs.color0) {
        params.add("float4 aColor0");
    }
    if (needs.color1) {
        params.add("float4 aColor1");
    }
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (needs.coord_used[c]) {
            params.add("float3 aTexcoord%u", c);
        }
    }
    params.add("float4 out vPosition : POSITION");
    if (needs.color0) {
        params.add("float4 out vColor0 : COLOR0");
    }
    if (needs.color1) {
        params.add("float4 out vColor1 : COLOR1");
    }
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (needs.coord_used[c]) {
            params.add("float%u out vTexcoord%u : TEXCOORD%u", needs.coord_proj[c] ? 3u : 2u, c, c);
        }
    }
    if (needs.fog) {
        params.add("float2 out vZW : TEXCOORD%u", needs.fog_slot);
    }
    if (!params.ok) {
        return false;
    }
    Writer w{out, out + cap - 1, true};
    w.append("void main(\n");
    w.append(params.text);
    w.append(")\n{\n");
    // No matrix: the decoder has already produced GL clip-space positions, w included, so the
    // GPU's perspective divide and near-plane clipping do the rest.
    w.append("    vPosition = aPosition;\n");
    if (needs.color0) {
        w.append("    vColor0 = aColor0;\n");
    }
    if (needs.color1) {
        w.append("    vColor1 = aColor1;\n");
    }
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (needs.coord_used[c]) {
            w.line(needs.coord_proj[c] ? "    vTexcoord%u = aTexcoord%u;" : "    vTexcoord%u = aTexcoord%u.xy;", c, c);
        }
    }
    if (needs.fog) {
        w.append("    vZW = aPosition.zw;\n");
    }
    w.append("}\n");
    finish(w, out, written);
    return w.ok;
}

namespace {

// One lighting half: out = `oname` (float3 for colour, float for alpha), from vertex colour
// `vtx` (a float4 in bytes), channel `chan`'s material/ambient uniforms, components `comp`.
void emit_lit_half(Writer& w, const GpuLitParams& lit, unsigned h, unsigned chan, const char* vtx, const char* type,
                   const char* comp, const char* oname) {
    w.line("    {");
    w.line("        %s m = %s.%s;", type, lit.mat_vtx[h] ? vtx : (chan == 0 ? "uMat0" : "uMat1"), comp);
    if (!lit.lit[h]) {
        w.line("        %s = m;", oname);
        w.line("    }");
        return;
    }
    w.line("        %s l = %s.%s;", type, lit.amb_vtx[h] ? vtx : (chan == 0 ? "uAmb0" : "uAmb1"), comp);
    for (unsigned li = 0; li < kMaxLights; ++li) {
        if (!(lit.light_mask[h] & (1u << li))) {
            continue;
        }
        w.line("        {");
        w.line("            float attn = 1.0;");
        if (lit.attn_fn[h] == AfSpot) {
            w.line("            float cs = max(0.0, dot(d%u, uLD%u.xyz));", li, li);
            w.line("            float ca = uLA%u.x + uLA%u.y * cs + uLA%u.z * cs * cs;", li, li, li);
            w.line("            float da = uLK%u.x + uLK%u.y * dist%u + uLK%u.z * dist%u * dist%u;", li, li, li, li, li, li);
            w.line("            attn = da != 0.0 ? max(0.0, ca / da) : 0.0;");
        } else if (lit.attn_fn[h] == AfSpec) {
            w.line("            attn = dot(n, d%u) >= 0.0 ? max(0.0, dot(n, uLD%u.xyz)) : 0.0;", li, li);
            w.line("            float ca = uLA%u.x + uLA%u.y * attn + uLA%u.z * attn * attn;", li, li, li);
            const char* k = lit.diff_fn[h] != DfNone ? "uLS" : "uLK";
            w.line("            float da = %s%u.x + %s%u.y * attn + %s%u.z * attn * attn;", k, li, k, li, k, li);
            w.line("            attn = da != 0.0 ? max(0.0, ca / da) : (ca > 0.0 ? 1.0 : 0.0);");
        }
        if (lit.diff_fn[h] == DfSign) {
            w.line("            float diff = dot(d%u, n);", li);
        } else if (lit.diff_fn[h] == DfClamp) {
            w.line("            float diff = max(0.0, dot(d%u, n));", li);
        } else {
            w.line("            float diff = 1.0;");
        }
        w.line("            %s t = (attn * diff) * uLC%u.%s;", type, li, comp);
        w.line("            l += sign(t) * floor(abs(t) + 0.5);");
        w.line("        }");
    }
    w.line("        l = clamp(l, 0.0, 255.0);");
    w.line("        %s = floor(m * (l + floor(l / 128.0)) / 256.0);", oname);
    w.line("    }");
}

// The GX lighting of a GPU-lit vertex program: COLOR0A0 / COLOR1A1 from the eye position and
// (unnormalised) eye normal and the raw vertex colours (0..1), named by the caller.
void emit_lighting_body(Writer& w, const GpuLitParams& lit, unsigned used, bool c0, bool c1, const char* eye,
                        const char* enrm, const char* col0, const char* col1) {
    if (used != 0) {
        w.line("    float3 n = %s;", enrm);
        w.append("    float nn = dot(n, n);\n");
        w.append("    if (nn >= 1.0e-20) n = n * rsqrt(nn);\n");
        for (unsigned li = 0; li < kMaxLights; ++li) {
            if (!(used & (1u << li))) continue;
            w.line("    float3 d%u = uLP%u.xyz - %s;", li, li, eye);
            w.line("    float q%u = dot(d%u, d%u);", li, li, li);
            w.line("    float dist%u = 0.0;", li);
            w.line("    if (q%u > 1.0e-20) { float r%u = rsqrt(q%u); dist%u = q%u * r%u; d%u = d%u * r%u; } else { d%u = n; }",
                   li, li, li, li, li, li, li, li, li, li);
        }
    }
    if (c0) {
        w.line("    float4 v0 = %s * 255.0;", col0);
        w.append("    float3 o0; float a0;\n");
        emit_lit_half(w, lit, 0, 0, "v0", "float3", "rgb", "o0");
        emit_lit_half(w, lit, 2, 0, "v0", "float", "a", "a0");
        w.append("    vColor0 = float4(o0, a0) / 255.0;\n");
    }
    if (c1) {
        w.line("    float4 v1 = %s * 255.0;", col1);
        w.append("    float3 o1; float a1;\n");
        emit_lit_half(w, lit, 1, 1, "v1", "float3", "rgb", "o1");
        emit_lit_half(w, lit, 3, 1, "v1", "float", "a", "a1");
        w.append("    vColor1 = float4(o1, a1) / 255.0;\n");
    }
}

// The lighting uniforms a GPU-lit program declares.
void add_lighting_params(ParamList& params, unsigned used, bool c0, bool c1) {
    if (c0) {
        params.add("uniform float4 uMat0");
        params.add("uniform float4 uAmb0");
    }
    if (c1) {
        params.add("uniform float4 uMat1");
        params.add("uniform float4 uAmb1");
    }
    for (unsigned li = 0; li < kMaxLights; ++li) {
        if (!(used & (1u << li))) continue;
        params.add("uniform float4 uLP%u", li);
        params.add("uniform float4 uLD%u", li);
        params.add("uniform float4 uLS%u", li);
        params.add("uniform float4 uLC%u", li);
        params.add("uniform float4 uLA%u", li);
        params.add("uniform float4 uLK%u", li);
    }
}

} // namespace

bool emit_tev_vertex_cg_lit(const ShaderNeeds& needs, const GpuLitParams& lit, char* out, std::size_t cap,
                            std::size_t* written) {
    if (!out || cap < 8) {
        return false;
    }
    // Halves emitted: COLOR0A0 when the fragment reads COLOR0, COLOR1A1 when it reads COLOR1.
    const bool c0 = needs.color0;
    const bool c1 = needs.color1;
    unsigned used = 0;
    if (c0) used |= lit.light_mask[0] | lit.light_mask[2];
    if (c1) used |= lit.light_mask[1] | lit.light_mask[3];
    ParamList params;
    params.add("float4 aPosition");
    if (c0) params.add("float4 aColor0");
    if (c1) params.add("float4 aColor1");
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (needs.coord_used[c]) {
            params.add("float3 aTexcoord%u", c);
        }
    }
    if (used != 0) {
        params.add("float3 aEyePos");
        params.add("float3 aEyeNrm");
    }
    add_lighting_params(params, used, c0, c1);
    params.add("float4 out vPosition : POSITION");
    if (c0) params.add("float4 out vColor0 : COLOR0");
    if (c1) params.add("float4 out vColor1 : COLOR1");
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (needs.coord_used[c]) {
            params.add("float%u out vTexcoord%u : TEXCOORD%u", needs.coord_proj[c] ? 3u : 2u, c, c);
        }
    }
    if (needs.fog) {
        params.add("float2 out vZW : TEXCOORD%u", needs.fog_slot);
    }
    if (!params.ok) {
        return false;
    }
    Writer w{out, out + cap - 1, true};
    w.append("void main(\n");
    w.append(params.text);
    w.append(")\n{\n");
    w.append("    vPosition = aPosition;\n");
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (needs.coord_used[c]) {
            w.line(needs.coord_proj[c] ? "    vTexcoord%u = aTexcoord%u;" : "    vTexcoord%u = aTexcoord%u.xy;", c, c);
        }
    }
    if (needs.fog) {
        w.append("    vZW = aPosition.zw;\n");
    }
    emit_lighting_body(w, lit, used, c0, c1, "aEyePos", "aEyeNrm", "aColor0", "aColor1");
    w.append("}\n");
    finish(w, out, written);
    return w.ok;
}

bool emit_tev_vertex_cg_xf(const ShaderNeeds& needs, const GpuLitParams& lit, char* out, std::size_t cap,
                           std::size_t* written) {
    if (!out || cap < 8) {
        return false;
    }
    const bool c0 = needs.color0;
    const bool c1 = needs.color1;
    unsigned used = 0;
    if (c0) used |= lit.light_mask[0] | lit.light_mask[2];
    if (c1) used |= lit.light_mask[1] | lit.light_mask[3];
    bool any_tc = false;
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        any_tc = any_tc || needs.coord_used[c];
    }
    ParamList params;
    params.add("float3 aPosition");
    params.add("float3 aNormal");
    if (any_tc) {
        params.add("float2 aTc0");
        params.add("float2 aTc1");
    }
    if (c0 || c1) {
        params.add("float4 aColor0");
        params.add("float4 aColor1");
    }
    params.add("float4 aMtx");
    params.add("uniform float4 uPn[%u]", kMaxPnMtx * 3u);
    params.add("uniform float4 uNm[%u]", kMaxPnMtx * 3u);
    params.add("uniform float4 uProj[4]");
    params.add("uniform float4 uXf");
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (!needs.coord_used[c]) continue;
        params.add("uniform float4 uTgS%u", c);
        params.add("uniform float4 uTgM%u[3]", c);
        params.add("uniform float4 uTgP%u[3]", c);
        params.add("uniform float4 uTgF%u", c);
        params.add("uniform float4 uTgK%u", c);
    }
    add_lighting_params(params, used, c0, c1);
    params.add("float4 out vPosition : POSITION");
    if (c0) params.add("float4 out vColor0 : COLOR0");
    if (c1) params.add("float4 out vColor1 : COLOR1");
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (needs.coord_used[c]) {
            params.add("float%u out vTexcoord%u : TEXCOORD%u", needs.coord_proj[c] ? 3u : 2u, c, c);
        }
    }
    if (needs.fog) {
        params.add("float2 out vZW : TEXCOORD%u", needs.fog_slot);
    }
    if (!params.ok) {
        return false;
    }
    Writer w{out, out + cap - 1, true};
    w.append("void main(\n");
    w.append(params.text);
    w.append(")\n{\n");
    // decode.cpp gpu_xf_reference is this, in C++.
    w.append("    float mi = uXf.y > 0.5 ? aMtx.x : uXf.x;\n");
    w.line("    if (mi > %u.5) mi = 0.0;", kMaxPnMtx - 1u);
    w.append("    int r = (int)(mi + 0.25) * 3;\n");
    w.append("    float4 obj = float4(aPosition, 1.0);\n");
    w.append("    float3 aEyePos = float3(dot(uPn[r], obj), dot(uPn[r + 1], obj), dot(uPn[r + 2], obj));\n");
    w.append("    float4 eye = float4(aEyePos, 1.0);\n");
    w.append("    float cx = dot(uProj[0], eye);\n");
    w.append("    float cy = dot(uProj[1], eye);\n");
    w.append("    float cz = dot(uProj[2], eye);\n");
    w.append("    float cw = dot(uProj[3], eye);\n");
    w.append("    vPosition = float4(cx, cy, 2.0 * cz + cw, cw);\n");
    if (needs.fog) {
        w.append("    vZW = float2(2.0 * cz + cw, cw);\n");
    }
    if (used != 0) {
        w.append("    float4 nobj = float4(aNormal, 0.0);\n");
        w.append("    float3 aEyeNrm = float3(dot(uNm[r], nobj), dot(uNm[r + 1], nobj), dot(uNm[r + 2], nobj));\n");
    }
    if (c0 || c1) {
        // The colours the lighting reads: COLOR0 if the layout has it, else COLOR1, else white;
        // COLOR1 only when both are present.
        w.append("    float4 white = float4(1.0, 1.0, 1.0, 1.0);\n");
        w.append("    float4 raw0 = uXf.z > 0.5 ? aColor0 : (uXf.w > 0.5 ? aColor1 : white);\n");
        w.append("    float4 raw1 = (uXf.z > 0.5 && uXf.w > 0.5) ? aColor1 : white;\n");
    }
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (!needs.coord_used[c]) continue;
        w.line("    {");
        w.line("        float4 s = float4(aPosition, 1.0) * uTgS%u.x + float4(aNormal, 1.0) * uTgS%u.y +", c, c);
        w.line("                   float4(aTc0, 1.0, 1.0) * uTgS%u.z + float4(aTc1, 1.0, 1.0) * uTgS%u.w;", c, c);
        w.line("        if (uTgF%u.x > 0.5) s.z = 1.0;", c);
        w.line("        float3 t = float3(dot(uTgM%u[0], s), dot(uTgM%u[1], s), dot(uTgM%u[2], s));", c, c, c);
        w.line("        if (uTgF%u.y > 0.5) { float l = dot(t, t); if (l >= 1.0e-20) t = t * rsqrt(l); }", c);
        w.line("        float4 t4 = float4(t, 1.0);");
        w.line("        float3 o = float3(dot(uTgP%u[0], t4), dot(uTgP%u[1], t4), dot(uTgP%u[2], t4));", c, c, c);
        w.line("        if (uTgF%u.z > 0.5 && o.z == 0.0) o.xy = clamp(o.xy * 0.5, -1.0, 1.0);", c);
        w.line("        o.xy = o.xy * uTgK%u.xy;", c);
        if (needs.coord_proj[c]) {
            w.line("        vTexcoord%u = float3(o.xy, uTgF%u.w > 0.5 ? o.z : 1.0);", c, c);
        } else {
            w.line("        vTexcoord%u = o.xy;", c);
        }
        w.line("    }");
    }
    emit_lighting_body(w, lit, used, c0, c1, "aEyePos", "aEyeNrm", "raw0", "raw1");
    w.append("}\n");
    finish(w, out, written);
    return w.ok;
}

bool emit_tev_fragment_cg(const State& state, char* out, std::size_t cap, std::size_t* written) {
    if (!out || cap < 8) {
        return false;
    }
    const ShaderNeeds needs = analyze_tev(state);
    const unsigned stages = needs.stages;

    ParamList params;
    if (needs.color0) {
        params.add("float4 vColor0 : COLOR0");
    }
    if (needs.color1) {
        params.add("float4 vColor1 : COLOR1");
    }
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (needs.coord_used[c]) {
            params.add("float%u vTexcoord%u : TEXCOORD%u", needs.coord_proj[c] ? 3u : 2u, c, c);
        }
    }
    if (needs.fog) {
        params.add("float2 vZW : TEXCOORD%u", needs.fog_slot);
    }
    for (unsigned t = 0; t < kMaxTexMaps; ++t) {
        if (needs.tex_used[t]) {
            params.add("uniform sampler2D uTex%u", t);
        }
    }
    if (needs.konst) {
        params.add("uniform float4 uKonst[4]");
    }
    params.add("uniform float4 uTevReg[4]");
    if (needs.indirect) {
        params.add("uniform float4 uIndMtx[6]");
        params.add("uniform float4 uIndCoordScale[4]");
        params.add("uniform float4 uTexSize[8]");
    }
    if (needs.fog) {
        params.add("uniform float4 uFogColor");
        // x = A (unshifted partial float), y = B magnitude, z = C, w = B shift
        params.add("uniform float4 uFogParam");
        params.add("uniform float2 uDepthRange");
    }
    if (needs.dst_alpha) {
        // x = 1 to write the constant, 0 to keep the TEV alpha; y = the constant.
        params.add("uniform float2 uDstAlpha");
    }
    if (!params.ok) {
        return false;
    }

    Writer w{out, out + cap - 1, true};
    w.append("float4 main(\n");
    w.append(params.text);
    w.append(") : COLOR\n{\n");
    for (unsigned c = 0; c < kEmitTexCoordSets; ++c) {
        if (!needs.coord_used[c]) {
            continue;
        }
        if (needs.coord_proj[c]) {
            w.line("    float2 tc%u = vTexcoord%u.z != 0.0 ? vTexcoord%u.xy / vTexcoord%u.z : vTexcoord%u.xy;", c, c, c,
                   c, c);
        } else {
            w.line("    float2 tc%u = vTexcoord%u;", c, c);
        }
    }
    w.append(needs.color0 ? "    float4 rast0 = vColor0;\n" : "    float4 rast0 = float4(0.0, 0.0, 0.0, 0.0);\n");
    w.append(needs.color1 ? "    float4 rast1 = vColor1;\n" : "    float4 rast1 = float4(0.0, 0.0, 0.0, 0.0);\n");
    w.append("    float4 prev = uTevReg[0];\n");
    w.append("    float4 reg0 = uTevReg[1];\n    float4 reg1 = uTevReg[2];\n    float4 reg2 = uTevReg[3];\n");
    if (needs.indirect) {
        w.append("    float2 tc_prev = float2(0.0, 0.0);\n");
    }
    // What a stage reads as TEXC/TEXA when it samples nothing (Aurora shader.cpp:43-49).
    const char* no_tex = needs.num_tex_gens == 0 ? "float4(0.0, 0.0, 0.0, 0.0)" : "float4(1.0, 1.0, 1.0, 1.0)";
    for (unsigned i = 0; i < stages && i < kTevStages; ++i) {
        const TevStage& s = state.tev[i];
        w.line("    {");
        const bool can_sample = tev_stage_can_sample(state, s);
        const int coord = tev_effective_texcoord(state, s.texcoord);
        const bool ind = uses_indirect(s) && s.ind_stage < state.num_ind_stages;
        bool have_bump = false;
        // --- texture ---
        if (can_sample && (ind || (needs.indirect && s.ind_add_prev))) {
            // Indirect texture coordinate path, after Aurora (shader.cpp:1312-1455), in float:
            // the result is the same texel coordinate up to fixed-point rounding.
            const unsigned map = s.texmap;
            w.line("        float2 base_tc = tc%d * uTexSize[%u].xy;", coord, map);
            w.line("        float3 ind_c = float3(0.0, 0.0, 0.0);");
            if (ind) {
                const unsigned istage = s.ind_stage & 3u;
                const IndStage& is = state.ind[istage];
                const int icoord = tev_effective_texcoord(state, is.texcoord);
                if (is.texmap < kMaxTexMaps && icoord >= 0) {
                    // Indirect texel components arrive as alpha, blue, green -> s, t, u.
                    w.line("        ind_c = floor(tex2D(uTex%u, tc%d * uIndCoordScale[%u].xy).abg * 255.0 + 0.5);",
                           static_cast<unsigned>(is.texmap), icoord, istage);
                    have_bump = true;
                }
                static const float kFmtDiv[4] = {1.0f, 8.0f, 16.0f, 32.0f};  // ITF_8/5/4/3
                const float div = kFmtDiv[s.ind_fmt & 3u];
                w.line("        float3 ind_raw = ind_c;");
                if (div != 1.0f) {
                    w.line("        ind_c = floor(ind_c / %.1f);", div);
                }
                if (s.ind_bias != 0) {
                    const float bias = (s.ind_fmt == IndFmt8) ? -128.0f : 1.0f;
                    w.line("        ind_c += float3(%.1f, %.1f, %.1f);", (s.ind_bias & 1u) ? bias : 0.0f,
                           (s.ind_bias & 2u) ? bias : 0.0f, (s.ind_bias & 4u) ? bias : 0.0f);
                }
                const unsigned mtx = s.ind_mtx;
                if (mtx >= 1u && mtx <= 3u) {
                    const unsigned k = mtx - 1u;
                    w.line("        float2 ind_off = float2(dot(uIndMtx[%u].xyz, ind_c), dot(uIndMtx[%u].xyz, ind_c)) * "
                           "uIndMtx[%u].w;",
                           k * 2u, k * 2u + 1u, k * 2u);
                } else if (mtx >= 5u && mtx <= 7u) {
                    w.line("        float2 ind_off = base_tc * (ind_c.x / 256.0) * uIndMtx[%u].w;", (mtx - 5u) * 2u);
                } else if (mtx >= 9u && mtx <= 11u) {
                    w.line("        float2 ind_off = base_tc * (ind_c.y / 256.0) * uIndMtx[%u].w;", (mtx - 9u) * 2u);
                } else {
                    w.line("        float2 ind_off = float2(0.0, 0.0);");
                }
            } else {
                w.line("        float3 ind_raw = ind_c;");
                w.line("        float2 ind_off = float2(0.0, 0.0);");
            }
            static const float kWrap[7] = {0.0f, 256.0f, 128.0f, 64.0f, 32.0f, 16.0f, -1.0f};
            const char* comp[2] = {"x", "y"};
            const std::uint8_t wraps[2] = {s.ind_wrap_s, s.ind_wrap_t};
            for (unsigned k = 0; k < 2u; ++k) {
                const float wv = wraps[k] < 7u ? kWrap[wraps[k]] : 0.0f;
                if (wv < 0.0f) {
                    w.line("        base_tc.%s = 0.0;", comp[k]);
                } else if (wv > 0.0f) {
                    w.line("        base_tc.%s = base_tc.%s - %.1f * floor(base_tc.%s / %.1f);", comp[k], comp[k], wv,
                           comp[k], wv);
                }
            }
            w.line("        float2 tcs = base_tc + ind_off;");
            if (s.ind_add_prev) {
                w.line("        tcs += tc_prev;");
            }
            w.line("        tc_prev = tcs;");
            w.line("        float4 tex_src = tex2D(uTex%u, tcs * uTexSize[%u].zw);", map, map);
            emit_swap4(w, "tex_s", "tex_src", state.tev_swap[s.tex_swap & 3u]);
            if (have_bump && (s.ras_chan == kRasAlphaBump || s.ras_chan == kRasAlphaBumpN) && s.ind_alpha_sel != 0) {
                static const char* kSel[4] = {"x", "x", "y", "z"};
                static const float kMask[4] = {248.0f, 224.0f, 240.0f, 248.0f};  // ITF_8/5/4/3 (Aurora)
                const float m = kMask[s.ind_fmt & 3u];
                w.line("        float bump = (ind_raw.%s - fmod(ind_raw.%s, %.1f)) / 255.0;", kSel[s.ind_alpha_sel & 3u],
                       kSel[s.ind_alpha_sel & 3u], 256.0f - m);
            }
        } else if (can_sample) {
            w.line("        float4 tex_src = tex2D(uTex%u, tc%d);", static_cast<unsigned>(s.texmap), coord);
            emit_swap4(w, "tex_s", "tex_src", state.tev_swap[s.tex_swap & 3u]);
            if (needs.indirect && s.texmap < kMaxTexMaps) {
                // A plain stage still sets the coordinate a following add-prev stage builds on.
                w.line("        tc_prev = tc%d * uTexSize[%u].xy;", coord, static_cast<unsigned>(s.texmap));
            }
        } else {
            w.line("        float4 tex_s = %s;", no_tex);
        }
        // --- rasterised colour ---
        if (s.ras_chan == kRasColor0 || s.ras_chan == kRasColor1) {
            emit_swap4(w, "ras_s", s.ras_chan == kRasColor0 ? "rast0" : "rast1", state.tev_swap[s.ras_swap & 3u]);
        } else if ((s.ras_chan == kRasAlphaBump || s.ras_chan == kRasAlphaBumpN) && have_bump &&
                   s.ind_alpha_sel != 0) {
            w.line(s.ras_chan == kRasAlphaBumpN ? "        float bn = bump * (255.0 / 248.0); float4 ras_s = float4(bn, bn, bn, bn);"
                                                : "        float4 ras_s = float4(bump, bump, bump, bump);");
        } else {
            w.line("        float4 ras_s = float4(0.0, 0.0, 0.0, 0.0);");
        }
        // --- konst: only when this stage reads it. The selector registers keep whatever the
        // last material set, so an unused kcsel/kasel routinely names a konst register this
        // program does not declare (uKonst is declared only when some stage reads KONST).
        {
            const bool reads_kc = s.color_a == CcKonst || s.color_b == CcKonst || s.color_c == CcKonst ||
                                  s.color_d == CcKonst;
            const bool reads_ka = s.alpha_a == CaKonst || s.alpha_b == CaKonst || s.alpha_c == CaKonst ||
                                  s.alpha_d == CaKonst;
            char kexpr[64];
            if (reads_kc) {
                konst_rgb_expr(s.kcsel, kexpr, sizeof(kexpr));
                w.line("        float3 kc = %s;", kexpr);
            }
            if (reads_ka) {
                konst_a_expr(s.kasel, kexpr, sizeof(kexpr));
                w.line("        float ka = %s;", kexpr);
            }
        }
        if (!g_tev_exact) {
            const char* cn[4] = {color_arg(s.color_a), color_arg(s.color_b), color_arg(s.color_c),
                                 color_arg(s.color_d)};
            const bool cr[4] = {color_arg_in_range(s.color_a), color_arg_in_range(s.color_b),
                                color_arg_in_range(s.color_c), true};
            const bool packed_c = s.color_op >= TevCompR8Gt && (s.color_op & ~1u) != TevCompRgb8Gt;
            const bool packed_a = s.alpha_op >= TevCompR8Gt && (s.alpha_op & ~1u) != TevCompRgb8Gt;
            if (packed_c || packed_a) {
                // R8/GR16/BGR24 compare the colour inputs A and B as packed bytes.
                w.line("        float3 oc_a3 = saturate(%s);", cn[0]);
                w.line("        float3 oc_b3 = saturate(%s);", cn[1]);
            }
            emit_fast_combiner(w, true, "oc", cn, cr, s.color_op, s.color_bias, s.color_shift, s.color_clamp);
            const char* an[4] = {alpha_arg(s.alpha_a), alpha_arg(s.alpha_b), alpha_arg(s.alpha_c),
                                 alpha_arg(s.alpha_d)};
            const bool ar[4] = {alpha_arg_in_range(s.alpha_a), alpha_arg_in_range(s.alpha_b),
                                alpha_arg_in_range(s.alpha_c), true};
            emit_fast_combiner(w, false, "oa", an, ar, s.alpha_op, s.alpha_bias, s.alpha_shift, s.alpha_clamp);
        } else {
            // --- colour combiner. Inputs a/b/c wrap to a byte; d does not. ---
            w.line("        float3 A_oc = round(%s * 255.0); A_oc -= floor(A_oc / 256.0) * 256.0;", color_arg(s.color_a));
            w.line("        float3 B_oc = round(%s * 255.0); B_oc -= floor(B_oc / 256.0) * 256.0;", color_arg(s.color_b));
            w.line("        float3 C_oc = round(%s * 255.0); C_oc -= floor(C_oc / 256.0) * 256.0;", color_arg(s.color_c));
            w.line("        float3 D_oc = round(%s * 255.0);", color_arg(s.color_d));
            if (s.color_op >= TevCompR8Gt) {
                char cond[256];
                compare_cond(s.color_op, "A_oc", "B_oc", cond, sizeof(cond));
                w.line("        float3 oc = (D_oc + (%s ? C_oc : float3(0.0, 0.0, 0.0))) / 255.0;", cond);
            } else {
                emit_regular(w, "oc", "float3", s.color_op, s.color_bias, s.color_shift);
            }
            w.line(s.color_clamp ? "        oc = saturate(oc);" : "        oc = clamp(oc, -1024.0 / 255.0, 1023.0 / 255.0);");
            // --- alpha combiner ---
            w.line("        float A_oa = round(%s * 255.0); A_oa -= floor(A_oa / 256.0) * 256.0;", alpha_arg(s.alpha_a));
            w.line("        float B_oa = round(%s * 255.0); B_oa -= floor(B_oa / 256.0) * 256.0;", alpha_arg(s.alpha_b));
            w.line("        float C_oa = round(%s * 255.0); C_oa -= floor(C_oa / 256.0) * 256.0;", alpha_arg(s.alpha_c));
            w.line("        float D_oa = round(%s * 255.0);", alpha_arg(s.alpha_d));
            if (s.alpha_op >= TevCompR8Gt) {
                // R8/GR16/BGR24 in the alpha combiner compare the *colour* combiner's A and B
                // inputs; only the RGB8 slot (the A8 compare) looks at the alpha inputs.
                char cond[256];
                const bool colour_inputs = (s.alpha_op & ~1u) != TevCompRgb8Gt;
                compare_cond(s.alpha_op, colour_inputs ? "A_oc" : "A_oa", colour_inputs ? "B_oc" : "B_oa", cond,
                             sizeof(cond));
                w.line("        float oa = (D_oa + (%s ? C_oa : 0.0)) / 255.0;", cond);
            } else {
                emit_regular(w, "oa", "float", s.alpha_op, s.alpha_bias, s.alpha_shift);
            }
            w.line(s.alpha_clamp ? "        oa = saturate(oa);" : "        oa = clamp(oa, -1024.0 / 255.0, 1023.0 / 255.0);");
        }
        w.line("        %s.rgb = oc;", dest_name(s.color_dest));
        w.line("        %s.a = oa;", dest_name(s.alpha_dest));
        w.line("    }");
    }
    // The last stage's destination is the pixel colour even when it is a register
    // (Aurora shader.cpp:1063-1071).
    {
        const TevStage& last = state.tev[(stages == 0 ? 1u : stages) - 1u];
        if (last.color_dest != 0) {
            w.line("    prev.rgb = %s.rgb;", dest_name(last.color_dest));
        }
        if (last.alpha_dest != 0) {
            w.line("    prev.a = %s.a;", dest_name(last.alpha_dest));
        }
    }
    if (needs.fog) {
        // GX fog on the window depth (Aurora shader.cpp:1477-1588). The varying carries GL
        // clip z/w; the viewport's depth range maps it to GX's 0 (near) .. 1 (far).
        w.line("    float fog_ndc = vZW.x / vZW.y;");
        w.line("    float fog_d = saturate(uDepthRange.x + (fog_ndc * 0.5 + 0.5) * (uDepthRange.y - uDepthRange.x));");
        w.line("    float fog_zc = floor(fog_d * 16777216.0 + 0.5);");
        if ((state.fog.type & 8u) == 0) {
            w.line("    float fog_ze = (uFogParam.x * 16777216.0) / (uFogParam.y - floor(fog_zc / exp2(uFogParam.w)));");
        } else {
            w.line("    float fog_ze = uFogParam.x * fog_zc / 16777216.0;");
        }
        w.line("    float fog_f = saturate(fog_ze - uFogParam.z);");
        switch (state.fog.type & 7u) {
        case 4:  // EXP
            w.line("    float fog_z = 1.0 - exp2(-8.0 * fog_f);");
            break;
        case 5:  // EXP2
            w.line("    float fog_z = 1.0 - exp2(-8.0 * fog_f * fog_f);");
            break;
        case 6:  // REVEXP
            w.line("    float fog_z = exp2(-8.0 * (1.0 - fog_f));");
            break;
        case 7:  // REVEXP2
            w.line("    float fog_ri = 1.0 - fog_f;");
            w.line("    float fog_z = exp2(-8.0 * fog_ri * fog_ri);");
            break;
        default:  // linear (1, 2, 3)
            w.line("    float fog_z = fog_f;");
            break;
        }
        w.line("    prev.rgb = lerp(prev.rgb, uFogColor.rgb, saturate(fog_z));");
    }
    // The final value wraps to a byte like every TEV register write (Aurora tev_overflow).
    // The fast form leaves it to the render target's clamp.
    if (g_tev_exact) {
        w.line("    float4 pb = round(prev * 255.0);");
        w.line("    prev = (pb - floor(pb / 256.0) * 256.0) / 255.0;");
    }
    if (needs.alpha_test) {
        const int c0 = alpha_cmp_const(state.alpha_comp0, state.alpha_ref0);
        const int c1 = alpha_cmp_const(state.alpha_comp1, state.alpha_ref1);
        char e0[64], e1[64];
        if (c0 == -1) {
            alpha_cmp_expr(state.alpha_comp0, state.alpha_ref0, e0, sizeof(e0));
        } else {
            std::snprintf(e0, sizeof(e0), "%s", c0 == 1 ? "true" : "false");
        }
        if (c1 == -1) {
            alpha_cmp_expr(state.alpha_comp1, state.alpha_ref1, e1, sizeof(e1));
        } else {
            std::snprintf(e1, sizeof(e1), "%s", c1 == 1 ? "true" : "false");
        }
        w.line("    float acmp = round(saturate(prev.a) * 255.0);");
        static const char* kOp[4] = {"&&", "||", "!=", "=="};
        w.line("    bool apass = (%s %s %s);", e0, kOp[state.alpha_op & 3u], e1);
        w.line("    clip(apass ? 1.0 : -1.0);");
    }
    if (needs.dst_alpha) {
        // GXSetDstAlpha replaces the alpha written to the EFB, after the alpha test has used
        // the TEV's own (Aurora blends it in with a constant alpha factor).
        w.line("    prev.a = lerp(prev.a, uDstAlpha.y, uDstAlpha.x);");
    }
    w.append("    return prev;\n}\n");
    finish(w, out, written);
    return w.ok;
}

void set_tev_exact(bool exact) {
    g_tev_exact = exact;
}

bool tev_exact() {
    return g_tev_exact;
}

bool emit_tev_op_fragment_cg(TevOp op, char* out, std::size_t cap, std::size_t* written) {
    State state;
    state.reset();
    state.num_tev_stages = 1;
    state.num_tex_gens = 1;
    switch (op) {
    case TevModulate:
        state.tev[0].color_a = CcZero;
        state.tev[0].color_b = CcTexC;
        state.tev[0].color_c = CcRasC;
        state.tev[0].color_d = CcZero;
        state.tev[0].alpha_a = CaZero;
        state.tev[0].alpha_b = CaTexA;
        state.tev[0].alpha_c = CaRasA;
        state.tev[0].alpha_d = CaZero;
        break;
    case TevReplace:
        state.tev[0].color_a = CcZero;
        state.tev[0].color_b = CcZero;
        state.tev[0].color_c = CcZero;
        state.tev[0].color_d = CcTexC;
        state.tev[0].alpha_d = CaTexA;
        break;
    case TevPassClr:
        state.tev[0].color_d = CcRasC;
        state.tev[0].alpha_d = CaRasA;
        break;
    case TevDecal:
        state.tev[0].color_a = CcRasC;
        state.tev[0].color_b = CcTexC;
        state.tev[0].color_c = CcTexA;
        state.tev[0].color_d = CcZero;
        state.tev[0].alpha_d = CaRasA;
        break;
    case TevBlend:
        state.tev[0].color_a = CcRasC;
        state.tev[0].color_b = CcOne;
        state.tev[0].color_c = CcTexC;
        state.tev[0].color_d = CcZero;
        state.tev[0].alpha_b = CaTexA;
        state.tev[0].alpha_c = CaRasA;
        break;
    default:
        return false;
    }
    return emit_tev_fragment_cg(state, out, cap, written);
}

} // namespace mkwv::gx
