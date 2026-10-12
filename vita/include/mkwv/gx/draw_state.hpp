#pragma once

// Per-draw raster and texture state.
//
// DrawCall snapshots the TEV, blend, alpha test and fog at the moment each draw is
// decoded - but the renderer also needs the textures, TLUTs, indirect setup, viewport,
// scissor, depth and cull, and it was reading all of those from processor.state, which
// is the state *after the whole frame decoded*. So every draw in a frame sampled whatever
// texture the frame loaded last, and every draw used the frame's final viewport and depth
// mode. That is why a dialog's backdrop never found its captured EFB copy, and why the
// dialog itself came out as a flat slab.
//
// DrawCall lives in mkwv/gx/fifo.hpp, which reaches every translated shard, so growing it
// would force a full shard rebuild. The snapshot is a side table in fifo.cpp instead,
// indexed by draw, owned by the Processor that filled it. This header is included only
// by .cpp files.

#include "mkwv/gx/state.hpp"

#include <cstdint>

namespace mkwv::gx {

struct Processor;

// Lighting evaluated in the vertex shader (static63, "GPU lighting"). The vertex path then only
// decodes, texgens and transforms; each vertex carries its eye-space position and normal and its
// raw vertex colours, and the GX lighting equation runs per vertex on the GPU with these
// parameters as uniforms. Everything a lit channel reads, captured with the draw's state.
struct GpuLitParams {
    std::uint8_t on = 0;              // this state's lighting runs on the GPU
    std::uint8_t lit[4]{};            // COLOR0, COLOR1, ALPHA0, ALPHA1 (State::chans order)
    std::uint8_t mat_vtx[4]{};
    std::uint8_t amb_vtx[4]{};
    std::uint8_t diff_fn[4]{};
    std::uint8_t attn_fn[4]{};
    std::uint8_t light_mask[4]{};     // lights each half uses (bit per light)
    std::uint8_t pad[3]{};
    float mat[2][4]{};                // material colours, bytes 0..255 (r, g, b, a)
    float amb[2][4]{};                // ambient colours, bytes 0..255
    float pos[kMaxLights][4]{};       // eye-space light position
    float dir[kMaxLights][4]{};       // normalised light direction (spot / specular)
    float spec[kMaxLights][4]{};      // normalised distance-attenuation vector (specular)
    float col[kMaxLights][4]{};       // light colour bytes 0..255
    float cosa[kMaxLights][4]{};      // angle attenuation k0, k1, k2
    float dista[kMaxLights][4]{};     // distance attenuation k0, k1, k2
};

// Whether lighting may move to the GPU at all (main.cpp; marker cpu_lighting turns it off).
// Host tests leave it off, so their vertices stay on the CPU lighting path.
void set_gpu_lighting(bool on);
bool gpu_lighting_enabled();
// Whether draw `index` of `processor`'s batch was packed for GPU lighting (its vertices carry
// eye position/normal and raw colours; its DrawExtra's `lit` holds the parameters).
// A hash of everything the submission merger compares for draw `index` (0 when unknown).
// Draws with equal keys and equal GPU-lit flags go into one GL call.
std::uint64_t draw_merge_key(const Processor& processor, std::uint32_t index);
// The display-list vertex cache (dl_cache.hpp); on by default, no_dl_cache turns it off.
void set_dl_cache(bool on);
bool draw_gpu_lit(const Processor& processor, std::uint32_t index);

struct DrawExtra {
    TexObj tex[kMaxTexMaps];
    TlutObj tlut[kMaxTexMaps];  // each texmap's TLUT, resolved when the draw was decoded
    bool tlut_valid[kMaxTexMaps];
    IndStage ind[kMaxIndStages];
    IndMatrix ind_mtx[kMaxIndMtx];
    std::uint8_t num_ind_stages;

    // Same names as State, so the apply_* helpers work on either.
    float render_vp[6];
    std::int32_t render_scissor[4];
    std::int32_t scissor_x, scissor_y, scissor_w, scissor_h;
    std::int32_t scissor_off_x, scissor_off_y;  // GXSetScissorBoxOffset (BP 0x59)
    bool has_render_vp, has_render_scissor, has_scissor;
    bool depth_test, depth_write;
    Compare depth_func;
    CullMode cull_mode;
    bool color_update, alpha_update;

    // Texgen types, which decide whether a texcoord is projective (divided per pixel).
    TexGenType tg_type[kMaxTexGens];

    // Lighting parameters when the draw's lighting runs on the GPU (else all zero).
    GpuLitParams lit;

    // Diagnostics only: texcoord 0's matrix id and the first two rows of that matrix as the
    // decoder applied it, for the DRAW dump.
    std::uint8_t tg0_mtx;
    float tg0_rows[8];
};

class Fifo;

// Empties the Fifo's command buffer between frames without touching its register shadows.
// Several GX registers pack more than one setting (TEV orders and konst selectors pair two
// stages, the colour/alpha combiner registers hold inputs and ops together, MATINDEX holds the
// position and four texture matrices), so each setter patches its own field in a shadow and
// writes the whole register. Fifo::reset() re-initialised those shadows every frame while
// the decoder's State - like the real GXData - persisted, so the first partial update of a
// frame zeroed the fields that frame did not happen to set again. Fifo::reset() is still the
// full power-on reset; this is the per-frame one. (A free function so fifo.hpp, which every
// translated shard includes, does not change.)
void fifo_reset_buffer_only(Fifo& fifo);

// Called by Processor::process when its vertex or draw capacity fills mid-frame: render the
// batch decoded so far (draws, their snapshots and the EFB copies among them), after which the
// Processor starts a new batch and keeps decoding. Installed by the renderer.
using BatchFlushFn = void (*)(Processor& processor);
void set_batch_flush(BatchFlushFn fn);
// Toggle transformed-vertex reuse (diagnostic A/B switch).
void set_vertex_reuse(bool enabled);
// Frame skipping: while set, Processor::process keeps decoding state but drops primitives
// (counted in skipped_primitives(), reset by end_frame()), so a skipped frame costs the
// command parse only. EFB copies are still recorded (they then copy the last rendered frame).
void set_skip_draws(bool skip);
bool skip_draws();
std::uint32_t skipped_primitives();
// Batches flushed since the Processor's last end_frame(): a frame whose draws all went out
// mid-decode ends with draw_count == 0 and still has to be presented.
std::uint32_t batches_flushed();

// Decodes the complete commands at the start of `data` and returns how many bytes they
// took; a command cut off by the end of the buffer is left for later rather than treated as
// an error.
std::uint32_t process_prefix(Processor& processor, const std::uint8_t* data, std::uint32_t size, bool big_endian);

// Called by a Fifo whose buffer is full: decode what it can (normally process_prefix into the
// live Processor) and return the number of leading bytes consumed, which the Fifo then drops.
// Returning 0 lets the write fail as before.
using FifoFullFn = std::uint32_t (*)(Fifo& fifo, const std::uint8_t* data, std::uint32_t size);
void set_fifo_full_handler(FifoFullFn fn);

// The snapshot for draw `index` of `processor`, or nullptr when that Processor did not
// fill the table (a hand-built DrawCall in a test, or another Processor decoded since).
const DrawExtra* draw_extra(const Processor& processor, std::uint32_t index);

} // namespace mkwv::gx
