#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/fifo.hpp"
#include "mkwv/gx/tev.hpp"

#include <cstdio>
#include <memory>
#include <cstring>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "gx_cg: %s\n", message);
    return 1;
}

bool contains(const char* hay, const char* needle) {
    return hay && needle && std::strstr(hay, needle) != nullptr;
}

} // namespace

int main() {
    using namespace mkwv::gx;
    static char buf[32768];
    std::size_t n = 0;
    {
        State vs;
        vs.reset();
        vs.num_tev_stages = 2;
        vs.tev[0].color_d = CcTexC;
        vs.tev[0].texcoord = 0;
        vs.tev[1].color_d = CcTexC;
        vs.tev[1].texmap = 1;
        vs.tev[1].texcoord = 1;
        vs.tev[1].color_a = CcRasC;
        vs.num_tex_gens = 2;
        vs.num_chans = 1;
        const ShaderNeeds needs = analyze_tev(vs);
        if (!emit_tev_vertex_cg(needs, buf, sizeof(buf), &n) || n < 32 || !contains(buf, "aTexcoord0") ||
            !contains(buf, "aTexcoord1") || !contains(buf, "vTexcoord1") || !contains(buf, "aColor0")) {
            return fail("vertex cg");
        }
        // The CPU already produced GL clip space (w included), so the vertex program must
        // pass the position through untouched.
        if (!contains(buf, "vPosition = aPosition;") || contains(buf, "mul(")) {
            return fail("vertex program must not apply a matrix");
        }
    }

    State state;
    state.reset();
    state.num_tev_stages = 2;
    state.num_tex_gens = 1;
    state.tev[0].color_a = CcZero;
    state.tev[0].color_b = CcZero;
    state.tev[0].color_c = CcZero;
    state.tev[0].color_d = CcRasC;
    state.tev[0].alpha_d = CaRasA;
    state.tev[1].color_a = CcZero;
    state.tev[1].color_b = CcTexC;
    state.tev[1].color_c = CcCPrev;
    state.tev[1].color_d = CcZero;
    state.tev[1].alpha_b = CaTexA;
    state.tev[1].alpha_c = CaAPrev;
    if (!emit_tev_fragment_cg(state, buf, sizeof(buf), &n) || !contains(buf, "tex2D") ||
        !contains(buf, "tex_s.rgb") || !contains(buf, "prev.rgb") || !contains(buf, "tex2D(uTex0, tc0)")) {
        return fail("two stage fragment cg");
    }
    if (classify_tev(state) != TevModulate) {
        return fail("two stage still modulate");
    }

    if (!emit_tev_op_fragment_cg(TevModulate, buf, sizeof(buf), &n) || !contains(buf, "ras_s.rgb") ||
        !contains(buf, "tex_s.rgb")) {
        return fail("modulate op cg");
    }
    if (!emit_tev_op_fragment_cg(TevReplace, buf, sizeof(buf), &n) || !contains(buf, "tex_s.rgb")) {
        return fail("replace op cg");
    }

    State one_stage = state;
    one_stage.num_tev_stages = 1;
    one_stage.tev[0].color_a = CcZero;
    one_stage.tev[0].color_b = CcTexC;
    one_stage.tev[0].color_c = CcRasC;
    one_stage.tev[0].color_d = CcZero;
    const std::uint64_t two = hash_tev(state);
    const std::uint64_t one = hash_tev(one_stage);
    if (two == 0 || two == one) {
        return fail("tev hash");
    }
    if (hash_tev(state) != two) {
        return fail("tev hash stable");
    }

    State shifted = one_stage;
    shifted.tev[0].color_shift = 1;
    if (!emit_tev_fragment_cg(shifted, buf, sizeof(buf), &n) || !contains(buf, ") * 2.0 +")) {
        return fail("shift cg");
    }
    State alpha = one_stage;
    alpha.alpha_comp0 = Greater;
    alpha.alpha_ref0 = 128;
    alpha.alpha_comp1 = Always;
    if (!emit_tev_fragment_cg(alpha, buf, sizeof(buf), &n) || !contains(buf, "clip(")) {
        return fail("alpha test cg");
    }
    if (hash_tev(alpha) == hash_tev(one_stage)) {
        return fail("alpha hash");
    }
    State kon;
    kon.reset();
    kon.tev[0].color_d = CcKonst;
    kon.tev[0].kcsel = 0x0C;
    kon.konst[0] = pack_rgba(255, 0, 0, 255);
    kon.num_tex_gens = 1;
    // kcsel 0x0C is "konst register 0, RGB". The register's contents are a uniform now,
    // so the source must name the uniform and must NOT contain the resolved colour.
    if (!emit_tev_fragment_cg(kon, buf, sizeof(buf), &n) || !contains(buf, "uKonst[0].rgb") ||
        contains(buf, "float3(1.000000, 0.000000, 0.000000)")) {
        return fail("konst cg");
    }
    State swapped;
    swapped.reset();
    swapped.tev[0].color_d = CcRasC;
    swapped.tev_swap[0][0] = 2;
    swapped.tev_swap[0][1] = 1;
    swapped.tev_swap[0][2] = 0;
    swapped.tev_swap[0][3] = 3;
    if (!emit_tev_fragment_cg(swapped, buf, sizeof(buf), &n) || !contains(buf, "float4 ras_s = rast0.bgra;")) {
        return fail("swap cg");
    }
    if (hash_tev(swapped) == hash_tev(kon)) {
        return fail("swap hash");
    }
    State dual;
    dual.reset();
    dual.tev[0].color_d = CcTexC;
    dual.tev[0].texmap = 1;
    dual.num_tex_gens = 2;
    // texmap and texcoord are independent in GX, and the emitter now honours both rather
    // than assuming map N is sampled at coord N.
    if (!emit_tev_fragment_cg(dual, buf, sizeof(buf), &n) || !contains(buf, "tex2D(uTex1, tc0)") ||
        !contains(buf, "sampler2D uTex1") || !contains(buf, "vTexcoord0")) {
        return fail("dual tex cg");
    }
    dual.tev[0].texcoord = 1;
    if (!emit_tev_fragment_cg(dual, buf, sizeof(buf), &n) ||
        !contains(buf, "tex2D(uTex1, tc1)") || !contains(buf, "vTexcoord1 : TEXCOORD1")) {
        return fail("dual tex coord routing");
    }
    State tex0 = dual;
    tex0.tev[0].texmap = 0;
    if (hash_tev(dual) == hash_tev(tex0)) {
        return fail("texmap hash");
    }
    State cmp;
    cmp.reset();
    cmp.tev[0].color_a = CcTexC;
    cmp.tev[0].color_b = CcRasC;
    cmp.tev[0].color_c = CcOne;
    cmp.tev[0].color_d = CcZero;
    cmp.tev[0].color_op = TevCompRgb8Gt;
    // GX compares 8-bit integer channel values: A and B are byte-space locals already.
    if (!emit_tev_fragment_cg(cmp, buf, sizeof(buf), &n) || !contains(buf, "(A_oc > B_oc)") ||
        !contains(buf, "float3 A_oc = round(tex_s.rgb * 255.0)")) {
        return fail("compare cg");
    }
    State fogged;
    fogged.reset();
    fogged.fog.type = FogOrthoLin;
    fogged.fog.a = 1.0f;
    fogged.fog.c = 0.0f;
    fogged.fog.color = pack_rgba(255, 0, 0, 255);
    if (!emit_tev_fragment_cg(fogged, buf, sizeof(buf), &n) || !contains(buf, "lerp(prev.rgb") ||
        !contains(buf, "saturate(")) {
        return fail("fog cg");
    }
    State clear_fog = fogged;
    clear_fog.fog = FogState{};
    if (hash_tev(fogged) == hash_tev(clear_fog)) {
        return fail("fog hash");
    }
    State ras0;
    ras0.reset();
    ras0.tev[0].ras_chan = kRasZero;
    if (!emit_tev_fragment_cg(ras0, buf, sizeof(buf), &n) ||
        !contains(buf, "float4 ras_s = float4(0.0, 0.0, 0.0, 0.0);")) {
        return fail("ras zero cg");
    }
    State ind;
    ind.reset();
    ind.tev[0].color_d = CcTexC;
    ind.tev[0].ind_mtx = 1;
    ind.num_tex_gens = 1;
    ind.num_ind_stages = 1;
    if (!emit_tev_fragment_cg(ind, buf, sizeof(buf), &n) || !contains(buf, "ind_off") ||
        !contains(buf, "uIndMtx[0]") || !contains(buf, "ind_c")) {
        return fail("indirect cg");
    }
    State no_ind = ind;
    no_ind.tev[0].ind_mtx = kIndMtxOff;
    if (hash_tev(ind) == hash_tev(no_ind)) {
        return fail("indirect hash");
    }

    // ---- the keystone invariant -------------------------------------------
    // Colour values are uniforms, so changing one must leave both the emitted source and
    // the program key untouched. If this ever regresses, every palette change becomes a
    // shader compile on hardware where compilation costs tens of milliseconds.
    {
        State a;
        a.reset();
        a.num_tev_stages = 1;
        a.num_tex_gens = 1;
        a.tev[0].color_a = CcZero;
        a.tev[0].color_b = CcTexC;
        a.tev[0].color_c = CcKonst;
        a.tev[0].color_d = CcC0;
        a.tev[0].kcsel = 0x0Du;  // K1 RGB
        a.tev[0].alpha_d = CaKonst;
        a.tev[0].kasel = 0x1Bu;
        a.alpha_comp0 = Greater;
        a.alpha_ref0 = 0x40;
        a.fog.type = FogPerspLin;
        a.konst[1] = 0x11223344u;
        a.tev_color[0] = 0x55667788u;
        a.fog.color = 0x00AABBCCu;
        a.alpha_ref1 = 0x10;

        char src_a[8192];
        std::size_t na = 0;
        if (!emit_tev_fragment_cg(a, src_a, sizeof(src_a), &na)) {
            return fail("keystone emit a");
        }
        State b = a;
        b.konst[1] = 0xF0E0D0C0u;
        b.tev_color[0] = 0x01020304u;
        b.fog.color = 0x00112233u;
        char src_b[8192];
        std::size_t nb = 0;
        if (!emit_tev_fragment_cg(b, src_b, sizeof(src_b), &nb)) {
            return fail("keystone emit b");
        }
        if (na != nb || std::strcmp(src_a, src_b) != 0) {
            std::fprintf(stderr, "gx_cg: colour-only change altered the shader source\n");
            return 1;
        }
        if (hash_tev_program(a) != hash_tev_program(b)) {
            return fail("colour-only change altered the program key");
        }
        if (hash_tev(a) == hash_tev(b)) {
            return fail("hash_tev should still see colour values");
        }
        // No literal colour may survive in the source.
        if (contains(src_a, "0.333333") || contains(src_a, "uKonst[1].rgb") == false) {
            return fail("konst did not resolve to a uniform");
        }
        if (!contains(src_a, "uFogColor.rgb") || !contains(src_a, "uTevReg[0]")) {
            return fail("a value that should be a uniform was baked in");
        }
        // Alpha-compare references are baked, as Aurora does (it folds constant compares),
        // so they are part of the program key.
        State r = a;
        r.alpha_ref0 = 0xF0;
        if (hash_tev_program(a) == hash_tev_program(r)) {
            return fail("alpha reference must change the program key");
        }
        char src_r[8192];
        std::size_t nr = 0;
        if (!emit_tev_fragment_cg(r, src_r, sizeof(src_r), &nr) || !contains(src_r, "(acmp > 240.0)")) {
            return fail("alpha reference must be baked into the compare");
        }
        // ...but a structural change must change the key.
        State c = a;
        c.tev[0].kcsel = 0x0Eu;  // K2 RGB - a different selector, same register contents
        if (hash_tev_program(a) == hash_tev_program(c)) {
            return fail("konst selector is structural and must change the key");
        }
        char src_c[8192];
        std::size_t nc = 0;
        if (!emit_tev_fragment_cg(c, src_c, sizeof(src_c), &nc) || !contains(src_c, "uKonst[2].rgb")) {
            return fail("konst selector did not reach the source");
        }
    }

    // KCSEL's constant range is structural too: 0..7 are the 8ths, not register reads.
    {
        State k;
        k.reset();
        k.num_tev_stages = 1;
        k.tev[0].color_d = CcKonst;
        k.tev[0].kcsel = 1u;  // 7/8
        char src[8192];
        std::size_t n2 = 0;
        if (!emit_tev_fragment_cg(k, src, sizeof(src), &n2) || !contains(src, "0.875")) {
            return fail("kcsel 7/8 constant");
        }
        if (contains(src, "uKonst")) {
            return fail("a constant kcsel must not declare the konst uniform");
        }
        k.tev[0].kcsel = 0x12u;  // konst register 2, red channel, splatted
        if (!emit_tev_fragment_cg(k, src, sizeof(src), &n2) || !contains(src, "uKonst[2].rrr")) {
            return fail("kcsel per-channel splat");
        }
    }

    // Only the samplers a program actually uses get declared.
    {
        State t;
        t.reset();
        t.num_tev_stages = 2;
        t.tev[0].color_d = CcTexC;
        t.tev[0].texmap = 0;
        t.tev[0].texcoord = 0;
        t.tev[1].color_d = CcTexC;
        t.tev[1].texmap = 3;
        t.tev[1].texcoord = 2;
        t.num_tex_gens = 2;
        char src[8192];
        std::size_t n2 = 0;
        if (!emit_tev_fragment_cg(t, src, sizeof(src), &n2)) {
            return fail("multi sampler emit");
        }
        if (!contains(src, "uniform sampler2D uTex0") || !contains(src, "uniform sampler2D uTex3")) {
            return fail("used samplers must be declared");
        }
        if (contains(src, "uTex1") || contains(src, "uTex2") || contains(src, "uTex7")) {
            return fail("unused samplers must not be declared");
        }
        // Only two texgens are configured, so texcoord 2 falls back to coordinate 0 (GX's
        // effective-texcoord rule) rather than declaring a varying nothing writes.
        if (contains(src, "vTexcoord2")) {
            return fail("texcoord 2 is beyond the texgen count and must not be declared");
        }
        if (!contains(src, "tex2D(uTex3, tc0)")) {
            return fail("texcoord beyond the texgen count must fall back to coordinate 0");
        }
        t.num_tex_gens = 3;
        if (!emit_tev_fragment_cg(t, src, sizeof(src), &n2) || !contains(src, "tex2D(uTex3, tc2)") ||
            !contains(src, "vTexcoord2 : TEXCOORD2")) {
            return fail("a configured texgen 2 must be sampled");
        }
    }
    // ---- the vertex and fragment programs must agree on every varying -----
    // A varying the fragment program declares and the vertex program never writes is a
    // link failure at best. Both are generated from one ShaderNeeds for exactly this
    // reason, so the pairing is asserted rather than assumed.
    {
        struct Case {
            const char* name;
            std::uint8_t map0, coord0, map1, coord1;
            unsigned stages;
            bool fog;
        };
        const Case cases[] = {
            {"one stage coord0", 0, 0, 0, 0, 1, false},
            {"two coords", 0, 0, 1, 1, 2, false},
            {"coord1 only", 1, 1, 1, 1, 1, false},
            {"texgen beyond what a vertex carries", 0, 5, 1, 7, 2, false},
            {"with fog", 0, 0, 1, 1, 2, true},
        };
        for (const Case& c : cases) {
            State st;
            st.reset();
            st.num_tev_stages = static_cast<std::uint8_t>(c.stages);
            st.num_tex_gens = 3;
            st.tev[0].color_d = CcTexC;
            st.tev[0].texmap = c.map0;
            st.tev[0].texcoord = c.coord0;
            if (c.stages > 1) {
                st.tev[1].color_d = CcTexC;
                st.tev[1].texmap = c.map1;
                st.tev[1].texcoord = c.coord1;
            }
            if (c.fog) {
                st.fog.type = FogPerspLin;
            }
            const ShaderNeeds needs = analyze_tev(st);
            char vsrc[8192];
            char fsrc[8192];
            std::size_t nv = 0;
            std::size_t nf = 0;
            if (!emit_tev_vertex_cg(needs, vsrc, sizeof(vsrc), &nv) ||
                !emit_tev_fragment_cg(st, fsrc, sizeof(fsrc), &nf)) {
                std::fprintf(stderr, "gx_cg: emit failed for %s\n", c.name);
                return 1;
            }
            for (unsigned i = 0; i < 8u; ++i) {
                char decl[32];
                std::snprintf(decl, sizeof(decl), "vTexcoord%u", i);
                const bool in_vs = contains(vsrc, decl);
                const bool in_fs = contains(fsrc, decl);
                if (in_vs != in_fs) {
                    std::fprintf(stderr, "gx_cg: %s - %s in vertex=%d fragment=%d\n", c.name, decl,
                                 in_vs ? 1 : 0, in_fs ? 1 : 0);
                    return 1;
                }
                // Nothing beyond what a decoded vertex carries may ever be declared.
                if (i >= kEmitTexCoordSets && in_fs) {
                    std::fprintf(stderr, "gx_cg: %s declared %s with no vertex data behind it\n", c.name, decl);
                    return 1;
                }
            }
            if (contains(vsrc, "vZW") != contains(fsrc, "vZW")) {
                std::fprintf(stderr, "gx_cg: %s - vZW disagrees between the two programs\n", c.name);
                return 1;
            }
        }
    }
    // ---- the TEV colour registers are indexed the way GX indexes them -----
    // GX_TEVPREV is register 0 and GX_TEVREG0 is register 1, so GX_CC_C0 must read slot 1.
    // Reading slot 0 for C0 - which this emitter did - shifts every register by one and
    // silently mis-colours any multi-stage material, which is most of them.
    {
        State r;
        r.reset();
        r.num_tev_stages = 1;
        r.tev[0].color_a = CcC0;
        r.tev[0].color_b = CcC1;
        r.tev[0].color_c = CcC2;
        r.tev[0].color_d = CcCPrev;
        char src[8192];
        std::size_t n2 = 0;
        if (!emit_tev_fragment_cg(r, src, sizeof(src), &n2)) {
            return fail("tev register emit");
        }
        if (!contains(src, "float4 prev = uTevReg[0];")) {
            return fail("prev must be seeded from GX_TEVPREV, not a constant");
        }
        if (!contains(src, "float4 reg0 = uTevReg[1];") || !contains(src, "float4 reg1 = uTevReg[2];") ||
            !contains(src, "float4 reg2 = uTevReg[3];")) {
            return fail("GX_TEVREG0/1/2 must be uniform slots 1/2/3");
        }
        if (!contains(src, "uniform float4 uTevReg[4]")) {
            return fail("all four registers must be declared");
        }
    }

    // GX_TEVREG2 lives in BP register 0xE6/0xE7. It used to be decoded and discarded, so
    // a material setting it saw no change at all.
    {
        Fifo f;
        f.reset();
        auto pr_owner = std::make_unique<Processor>();  // ~3 MiB: never on the stack
        Processor& pr = *pr_owner;
        pr.reset();
        f.set_tev_color(3, 0x11, 0x22, 0x33, 0x44);
        if (!pr.process(f.data(), f.size(), true)) {
            return fail("tev reg 2 fifo");
        }
        if (pr.state.tev_color[3] == 0) {
            return fail("GX_TEVREG2 was decoded and dropped");
        }
    }


    // ---- the GX compare-op family -----------------------------------------
    {
        struct Op {
            std::uint8_t op;
            const char* colour_shape;
        };
        const Op ops[] = {
            {TevCompR8Gt, "(A_oc.r > B_oc.r)"},
            {TevCompR8Eq, "(A_oc.r == B_oc.r)"},
            {TevCompGr16Gt, "(dot(A_oc.rg, float2(1.0, 256.0)) > "},
            {TevCompGr16Eq, "(dot(A_oc.rg, float2(1.0, 256.0)) == "},
            {TevCompBgr24Gt, "float3(1.0, 256.0, 65536.0)) > "},
            {TevCompBgr24Eq, "float3(1.0, 256.0, 65536.0)) == "},
            {TevCompRgb8Gt, "(A_oc > B_oc)"},
            {TevCompRgb8Eq, "(A_oc == B_oc)"},
        };
        for (const Op& o : ops) {
            State c;
            c.reset();
            c.num_tev_stages = 1;
            c.num_tex_gens = 1;
            c.tev[0].color_a = CcTexC;
            c.tev[0].color_b = CcRasC;
            c.tev[0].color_c = CcOne;
            c.tev[0].color_d = CcZero;
            c.tev[0].color_op = o.op;
            c.tev[0].alpha_a = CaTexA;
            c.tev[0].alpha_b = CaRasA;
            c.tev[0].alpha_c = CaZero;
            c.tev[0].alpha_d = CaZero;
            c.tev[0].alpha_op = o.op;
            char src[8192];
            std::size_t n2 = 0;
            if (!emit_tev_fragment_cg(c, src, sizeof(src), &n2) || !contains(src, o.colour_shape)) {
                std::fprintf(stderr, "gx_cg: compare op %u colour shape missing\n", o.op);
                return 1;
            }
            // Only the RGB8 slot (the A8 compare) reads the alpha inputs in the alpha combiner;
            // the other three compare the colour combiner's A and B.
            const bool a8 = (o.op & ~1u) == TevCompRgb8Gt;
            if (a8 != (contains(src, "(A_oa > B_oa)") || contains(src, "(A_oa == B_oa)"))) {
                std::fprintf(stderr, "gx_cg: compare op %u alpha combiner read the wrong inputs\n", o.op);
                return 1;
            }
        }
    }

    // Unclamped output is still a signed 11-bit register.
    {
        State u;
        u.reset();
        u.num_tev_stages = 1;
        u.tev[0].color_d = CcTexC;
        u.tev[0].color_clamp = false;
        char src[8192];
        std::size_t n2 = 0;
        if (!emit_tev_fragment_cg(u, src, sizeof(src), &n2) ||
            !contains(src, "oc = clamp(oc, -1024.0 / 255.0, 1023.0 / 255.0);")) {
            return fail("unclamped output must clamp to the signed register range");
        }
    }

    // ---- indirect texturing -----------------------------------------------
    {
        State base;
        base.reset();
        base.num_tev_stages = 1;
        base.num_ind_stages = 1;
        base.num_tex_gens = 2;
        base.ind[0].texmap = 2;
        base.ind[0].texcoord = 1;
        base.tev[0].color_d = CcTexC;
        base.tev[0].texmap = 0;
        base.tev[0].texcoord = 0;
        base.tev[0].ind_stage = 0;
        base.tev[0].ind_mtx = 2;  // GX_ITM_1
        base.tev[0].ind_fmt = IndFmt8;
        base.tev[0].ind_bias = 3;  // S and T
        char src[8192];
        std::size_t n2 = 0;
        if (!emit_tev_fragment_cg(base, src, sizeof(src), &n2)) {
            return fail("indirect emit");
        }
        if (!contains(src, "tex2D(uTex2, tc1 * uIndCoordScale[0].xy).abg")) {
            return fail("indirect lookup must use the indirect stage's own map, coord and scale");
        }
        if (!contains(src, "ind_c += float3(-128.0, -128.0, 0.0);")) {
            return fail("8-bit indirect bias on S and T");
        }
        if (!contains(src, "dot(uIndMtx[2].xyz, ind_c), dot(uIndMtx[3].xyz, ind_c)) * uIndMtx[2].w")) {
            return fail("GX_ITM_1 must use matrix rows 2/3 and its scale exponent");
        }
        if (!contains(src, "tex2D(uTex0, tcs * uTexSize[0].zw)")) {
            return fail("the offset texel coordinate must be normalised by the sampled map's size");
        }

        State dyn = base;
        dyn.tev[0].ind_mtx = 9;  // GX_ITM_T0
        if (!emit_tev_fragment_cg(dyn, src, sizeof(src), &n2) || !contains(src, "base_tc * (ind_c.y / 256.0)")) {
            return fail("dynamic T matrix");
        }

        State wrap = base;
        wrap.tev[0].ind_mtx = kIndMtxOff;
        wrap.tev[0].ind_wrap_s = IndWrap64;
        wrap.tev[0].ind_wrap_t = IndWrap0;
        wrap.tev[0].ind_add_prev = true;
        if (!emit_tev_fragment_cg(wrap, src, sizeof(src), &n2)) {
            return fail("wrap emit");
        }
        if (!contains(src, "base_tc.x = base_tc.x - 64.0 * floor(base_tc.x / 64.0);") ||
            !contains(src, "base_tc.y = 0.0;") || !contains(src, "tcs += tc_prev;")) {
            return fail("wrap modes and add_prev");
        }

        // The matrices are uniforms: changing one must not change the program.
        State m2 = base;
        m2.ind_mtx[1].m[0] = 0.75f;
        m2.ind_mtx[1].scale_exp = 3;
        if (hash_tev_program(base) != hash_tev_program(m2)) {
            return fail("an indirect matrix value must not change the program key");
        }
        // The indirect order is structural: it picks the sampler.
        State o2 = base;
        o2.ind[0].texmap = 5;
        if (hash_tev_program(base) == hash_tev_program(o2)) {
            return fail("the indirect order must change the program key");
        }
    }

    // ---- every uniform a program reads must be declared ---------------------
    // The konst selectors keep whatever the previous material set (GXInit leaves KCSEL at
    // K3_A), so a stage that never reads KONST still carries a register selector. The
    // emitter used to write `kc = uKonst[3].aaa` for every stage and declare uKonst only when
    // some stage read KONST - every such program failed to compile on the Vita and fell back
    // to fixed function. Fuzz the TEV and check each uName used has a declaration.
    {
        unsigned seed = 12345;
        auto rnd = [&seed]() {
            seed = seed * 1103515245u + 12345u;
            return (seed >> 16) & 0x7FFFu;
        };
        static char src[65536];
        for (int iter = 0; iter < 300; ++iter) {
            State st;
            st.reset();
            st.num_tev_stages = static_cast<std::uint8_t>(1 + rnd() % 16);
            st.num_tex_gens = static_cast<std::uint8_t>(rnd() % 9);
            st.num_ind_stages = static_cast<std::uint8_t>(rnd() % 5);
            for (unsigned k = 0; k < 16; ++k) {
                TevStage& t = st.tev[k];
                t.color_a = rnd() % 16; t.color_b = rnd() % 16; t.color_c = rnd() % 16; t.color_d = rnd() % 16;
                t.alpha_a = rnd() % 8; t.alpha_b = rnd() % 8; t.alpha_c = rnd() % 8; t.alpha_d = rnd() % 8;
                t.kcsel = rnd() % 32; t.kasel = rnd() % 32;
                t.texmap = (rnd() % 8 == 0) ? 0xFF : rnd() % 8;
                t.texcoord = rnd() % 8;
                t.ras_chan = (rnd() % 4 == 0) ? kRasZero : rnd() % 2;
                t.ind_mtx = (rnd() % 3 == 0) ? 1 + rnd() % 3 : 0;
                t.ind_stage = rnd() % 4;
                const std::uint8_t ops[] = {0, 1, 8, 9, 12, 14, 15};
                t.color_op = ops[rnd() % 7];
                t.alpha_op = ops[rnd() % 7];
            }
            const std::uint8_t fogs[] = {0, 2, 4, 10};
            st.fog.type = fogs[rnd() % 4];
            st.dst_alpha_enable = rnd() % 3 == 0;
            st.alpha_comp0 = static_cast<Compare>(rnd() % 8);
            std::size_t n2 = 0;
            if (!emit_tev_fragment_cg(st, src, sizeof(src), &n2)) {
                return fail("fuzz emit");
            }
            const char* names[] = {"uKonst", "uTevReg", "uIndMtx", "uIndCoordScale", "uTexSize", "uFogColor",
                                   "uFogParam", "uDepthRange", "uDstAlpha", "uTex0", "uTex1", "uTex2", "uTex3",
                                   "uTex4", "uTex5", "uTex6", "uTex7"};
            for (const char* name : names) {
                char used[32];
                char decl[48];
                std::snprintf(used, sizeof(used), "%s", name);
                std::snprintf(decl, sizeof(decl), " %s", name);
                const bool is_used = contains(src, used);
                bool declared = false;
                for (const char* p = std::strstr(src, "uniform "); p != nullptr; p = std::strstr(p + 1, "uniform ")) {
                    const char* eol = std::strchr(p, '\n');
                    const char* hit = std::strstr(p, decl);
                    if (hit != nullptr && (eol == nullptr || hit < eol)) {
                        declared = true;
                        break;
                    }
                }
                if (is_used && !declared) {
                    std::fprintf(stderr, "gx_cg: fuzz %d uses %s without declaring it\n", iter, name);
                    return 1;
                }
            }
        }
    }

    std::printf("gx_cg_tests: all cases pass\n");
    return 0;
}
