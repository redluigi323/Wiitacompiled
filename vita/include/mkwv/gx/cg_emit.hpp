#pragma once

#include "mkwv/gx/state.hpp"
#include "mkwv/gx/draw_state.hpp"

#include <cstddef>

namespace mkwv::gx {

// Texcoord sets a decoded vertex carries: all eight GX texgens.
inline constexpr unsigned kEmitTexCoordSets = kMaxTexGens;

// What one TEV configuration needs from the shader pair. Computed once and handed to both
// emitters so their varying sets cannot drift apart.
struct ShaderNeeds {
    unsigned stages = 1;
    unsigned num_tex_gens = 0;
    bool tex_used[kMaxTexMaps]{};
    bool coord_used[kEmitTexCoordSets]{};
    bool coord_proj[kEmitTexCoordSets]{};  // projective (3x4) texgen: divide by q per pixel
    bool color0 = false;                   // rasterised COLOR0A0 is read
    bool color1 = false;                   // rasterised COLOR1A1 is read
    bool konst = false;
    bool tevreg = false;
    bool indirect = false;
    bool alpha_test = false;
    bool fog = false;
    bool dst_alpha = false;  // GXSetDstAlpha: the written alpha may be replaced by a constant
    unsigned fog_slot = 8;   // TEXCOORDn carrying clip z/w for fog: the first set no texcoord uses
};

ShaderNeeds analyze_tev(const State& state);

// The effective texcoord a TEV stage samples with: GX falls back to texcoord 0 when the
// order names a coordinate beyond the configured texgen count (Aurora tev_effective_texcoord).
// -1 when there are no texgens at all.
int tev_effective_texcoord(const State& state, std::uint8_t texcoord);

// Whether a stage samples a texture at all: it needs a texgen and a texture map.
bool tev_stage_can_sample(const State& state, const TevStage& stage);

// Vertex program. Positions arrive in GL clip space - the CPU decoder has already
// transformed, lit and texgen'd every vertex - so this passes attributes through.
bool emit_tev_vertex_cg(const ShaderNeeds& needs, char* out, std::size_t cap, std::size_t* written);

// Vertex program for a GPU-lit draw (draw_state.hpp GpuLitParams): the same outputs, but
// COLOR0/COLOR1 are the GX lighting equation evaluated here from the raw vertex colours
// (aColor0/aColor1), the eye position (aEyePos) and eye normal (aEyeNrm). The lighting
// structure (which halves are lit, their sources, functions and lights) is baked in; the
// values are uniforms uMat0/1, uAmb0/1 and per light n uLPn (position), uLDn (direction),
// uLSn (specular distance vector), uLCn (colour bytes), uLAn (angle k), uLKn (distance k).
// decode.cpp gpu_lit_reference is the same arithmetic in C++.
bool emit_tev_vertex_cg_lit(const ShaderNeeds& needs, const GpuLitParams& lit, char* out, std::size_t cap,
                            std::size_t* written);

// Vertex program for the GPU transform (draw_state.hpp GpuXfParams): vertices are DL cache
// CachedVertex records in model space (aPosition, aNormal, aTc0, aTc1, aColor0, aColor1, aMtx);
// the matrices, texgen and colour selection are uniforms (uPn, uNm, uProj, uXf, per used
// texcoord c uTgS/M/P/F/K c), and the lighting is the GPU-lit program's.
bool emit_tev_vertex_cg_xf(const ShaderNeeds& needs, const GpuLitParams& lit, char* out, std::size_t cap,
                           std::size_t* written);
// Fragment program: evaluates the programmed TEV stages the way GX does (Aurora
// lib/gx/shader.cpp). Register values (konst, TEV colour registers, fog) arrive as uniforms;
// structure - including the alpha-compare references, which Aurora also bakes - is in the
// source. See hash_tev_program.
bool emit_tev_fragment_cg(const State& state, char* out, std::size_t cap, std::size_t* written);

// Exact (default) or fast TEV arithmetic in the fragment programs (cg_emit.cpp). Set before
// the first program is built: programs are cached by TEV state, not by this switch.
void set_tev_exact(bool exact);
bool tev_exact();

// One-stage classified combiners when we only have a TevOp, not full BP state.
bool emit_tev_op_fragment_cg(TevOp op, char* out, std::size_t cap, std::size_t* written);

} // namespace mkwv::gx
