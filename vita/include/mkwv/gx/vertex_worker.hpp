#pragma once

// Vertex jobs: decoding, texgen, lighting, the transform and packing into the GL vertex
// layout, run on other CPU cores while the thread that runs Processor::process goes on
// parsing commands.
//
// Processor::process copies each primitive's vertex bytes (the FIFO bytes do not outlive
// the call) and queues a job: a run of vertices in Processor::verts, the snapshot of the
// vertex state they are processed with (VertexSnapshot, taken once per epoch) and where the
// packed output goes. A vertex found in the reuse cache is not processed at all: the batch's
// remap table names the earlier vertex it equals, and the renderer's indices point there.
//
// Consumers: the worker thread (vertex_worker_main, cores 0 and 1) and anyone who would otherwise
// wait - the decoding thread while it drains, the guest thread while it waits for the render
// thread (vertex_worker_help). Jobs are independent (copies are index remaps), so any
// consumer may take any job. Anything that reads the output (submission at a batch flush,
// the present) calls vertex_worker_drain() first.
//
// Batch output (the packed arena, per-vertex offsets and the remap table) is written for
// every batch Processor::process builds, with or without the worker; the renderer reads it
// through packed_batch(). Not shard-visible.

#include "mkwv/gx/decode.hpp"

#include <cstdint>

namespace mkwv::gx {

struct VertexWorkerHooks {
    void (*wait)();   // worker: block until wake() (a semaphore wait)
    void (*wake)();   // producer: release one wait()
    void (*relax)();  // either side, while spinning (yield / short delay); may be null
};

// Enables the worker. The thread calling vertex_worker_main() must already be running (or
// start soon: jobs queue up until it does).
void vertex_worker_install(const VertexWorkerHooks& hooks);
// Disables it again (drains first). Host tests; the Vita build never turns it off.
void vertex_worker_uninstall();
bool vertex_worker_active();
// The worker thread's body. Returns only after vertex_worker_quit().
void vertex_worker_main();
void vertex_worker_quit();
// Runs one queued job on the calling thread if there is one. Any thread.
bool vertex_worker_help();

// ---- Producer side (the thread running Processor::process) ----

// A snapshot of the current vertex state and layout; returns its id for vertex_worker_push.
std::uint32_t vertex_worker_snapshot(const State& state, const PreparedLighting& prepared,
                                     const float (*su_scale)[2], bool su_any, const VertexDecoder& decoder,
                                     bool gpu_lit = false);
// Copies `n` bytes of vertex data where the job can read them until the batch is drained.
const std::uint8_t* vertex_worker_stash(const std::uint8_t* src, std::uint32_t n);
// Queues verts[first .. first + count) (packets of `stride` bytes at `bytes`) for processing
// with snapshot `snap`. Vertices whose remap entry names another vertex are skipped.
// `cached` (a display-list cache hit) replaces `bytes` as the source of the decoded vertices;
// `fill` receives them as they are decoded (a miss being filled). Both are per job vertex.
void vertex_worker_push(DecodedVertex* verts, std::uint32_t first, std::uint32_t count, std::uint32_t snap,
                        const std::uint8_t* bytes, std::uint32_t stride, const CachedVertex* cached = nullptr,
                        CachedVertex* fill = nullptr);
// Whether jobs also write the processed DecodedVertex to Processor::verts (default on; the Vita
// renderer turns it off - only captures and the host tests read those, and at ~150 bytes a
// vertex the writes were a large share of the job's memory traffic).
void vertex_worker_set_mirror(bool on);
bool vertex_worker_mirror();
// Publishes queued work to the consumers (the end of a process() call).
void vertex_worker_flush();
// Waits (helping) until every queued vertex is done.
void vertex_worker_drain();

// ---- Batch output ----

// Starts a new batch for `owner` (its vertices are about to be numbered from 0 again).
void vertex_batch_begin(const Processor* owner);
// Allocates packed space for vertices [first, first + count), all with `ntc` texture
// coordinates, and records their offsets. Returns the arena offset of `first`.
std::uint32_t vertex_batch_alloc(std::uint32_t first, std::uint32_t count, unsigned ntc, bool lit = false);
// Undoes the last allocation (a primitive that failed to decode).
void vertex_batch_unalloc(std::uint32_t first, std::uint32_t count, unsigned ntc, bool lit = false);
std::uint8_t* vertex_batch_arena();
// Vertex `index` equals vertex `src` (an earlier one in this batch), or is its own (-1).
void vertex_batch_set_remap(std::uint32_t index, std::int32_t src);

struct PackedBatch {
    const std::uint8_t* arena;
    const std::uint32_t* offset;  // per vertex: its packed bytes in the arena
    const std::uint32_t* segment; // per vertex: equal for a contiguous run of equal stride
    const std::int32_t* remap;    // per vertex: the vertex it equals, or -1
};
// The current batch's output if it belongs to `processor` (drained by the caller), else false.
bool packed_batch(const Processor& processor, PackedBatch* out);
// Makes every remapped vertex in processor.verts a real copy (captures, the draw dump).
void resolve_vertex_copies(Processor& processor);

// ---- GPU vertex ring (static64) ----
// Installs `base` (GPU-readable memory). By default the cached static arena stages each
// batch and packed_batch uploads it; the direct mode packs into base. The cursor runs on across batches and frames and wraps; offsets stay relative
// to `base`. Overwriting bytes the GPU may still read (from the last three frames) calls
// `gpu_sync` first. base == nullptr goes back to the static arena.
void vertex_ring_install(std::uint8_t* base, std::uint32_t bytes, void (*gpu_sync)());
// Select before installation. Default: pack into the existing cached batch arena and
// upload once when packed_batch is requested after decoding. false restores direct stores.
void vertex_ring_set_staging(bool on);
bool vertex_ring_staging();
bool vertex_ring_active();
// Once per presented frame, on the decoding thread.
void vertex_ring_frame_mark();
// GPU waits forced by the ring since the last call; bytes packed so far this frame.
std::uint32_t vertex_ring_take_syncs();
std::uint32_t vertex_ring_frame_bytes();

} // namespace mkwv::gx
