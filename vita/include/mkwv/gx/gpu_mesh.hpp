#pragma once

// Cached object-space meshes. A run of primitive commands between state changes is hashed
// with the vertex arrays it reads. The first time, those vertices are decoded once (no
// lighting, no transform) into a buffer. Every later frame with the same indices and the
// same array bytes draws that buffer with the current matrices on the GPU.
// A position-matrix index is cached when it is constant for the run. A run that changes
// index (skinning) stays on the CPU path.
//
// Off unless gpu_mesh_set_active(true). Host tests leave it off, so their vertices stay on
// the CPU path. Not included by translated shards.

#include "mkwv/gx/state.hpp"

#include <cstdint>

namespace mkwv::gx {

struct Processor;

void gpu_mesh_set_active(bool on);
bool gpu_mesh_active();
// GXInvalidateVtxCache: array bytes may have changed.
void gpu_mesh_invalidate();
// Draw indices restart (a new Processor::process, or a mid-frame batch flush).
void gpu_mesh_begin_batch();

struct GpuMeshView {
    const float* pos;       // xyz, stride floats
    const float* nrm;       // xyz
    const float* uv;        // st of texcoord 0
    const float* uv1;       // st of texcoord 1
    const std::uint8_t* color;  // RGBA
    const std::uint16_t* indices;
    std::uint32_t stride;   // bytes
    std::uint32_t index_count;
    float pn[12];
    float nrm_mtx[9];
    float proj[4][4];
    float tex_rows[16];    // two texgens, two rows each
    float col1[4];          // unlit COLOR1, 0..1
    bool lit;
    bool spot;              // attenuation: spot, otherwise none
    std::uint8_t diff;      // DiffuseFn
    std::uint8_t nlights;
    std::uint8_t mat_vtx;   // 1 = material colour is the vertex colour
    std::uint8_t amb_vtx;
    std::uint8_t a_mat_vtx;  // alpha material source: 1 = vertex alpha
    std::uint32_t mat_color;
    std::uint32_t amb_color;
    float light_pos[4][3];
    float light_dir[4][3];
    float light_col[4][4];  // colour bytes, 0..255
    float cos_att[4][3];
    float dist_att[4][3];
};

// If the primitive run starting at `cmd_off` can be served from the cache (building it on
// a miss), skips that run and returns where processing resumes. The draw itself is pushed
// by the caller, which then binds it with gpu_mesh_bind_draw.
bool gpu_mesh_try(const State& state, const std::uint8_t* data, std::uint32_t cmd_off, std::uint32_t size,
                  bool big_endian, std::uint32_t* resume, std::uint32_t* index_count);
void gpu_mesh_bind_draw(std::uint32_t draw_index);
const GpuMeshView* gpu_mesh_view(std::uint32_t draw_index);

// Why runs stayed on the CPU, cleared each time it is read. Per frame of the last window.
struct GpuMeshReject {
    std::uint32_t vary;    // position-matrix index changes inside the run (skinned)
    std::uint32_t tex;     // a per-vertex texture-matrix index
    std::uint32_t gen;     // texgen this path does not evaluate
    std::uint32_t chan;    // a second lit colour channel, or a second vertex colour
    std::uint32_t light;   // specular, lit alpha, or more than four lights
    std::uint32_t decode;  // the run could not be decoded into the cache
};
void gpu_mesh_reject_take(GpuMeshReject* out);

}  // namespace mkwv::gx
