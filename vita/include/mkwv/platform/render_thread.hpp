#pragma once

namespace mkwv::gx {
class Backend;
}

namespace mkwv::platform {

// The render thread: GX decode, GL submission and the swap on a second CPU core, fed the
// guest's command bytes through a queue. The guest thread keeps running game code while the
// previous chunk of the frame is decoded and drawn.
//
// sync_mode (RenderSync):
//   kSyncFrame    - at each present the guest waits until the render thread has decoded and
//                   submitted everything so far (not the swap): what GXDrawDone means on
//                   hardware. Frame time = game + whatever the renderer has left at the end.
//   kSyncAheadOne - (default) the wait moves to the retrace and covers only what was presented
//                   before the previous retrace: the renderer draws frame N while the game
//                   computes N+1. Frame time = max(game, renderer). Buffers the game rewrites
//                   every frame may be read one frame late (immediate-mode data is copied into
//                   the queue and is always exact).
//   kSyncNone     - never waits (up to a queue's worth of chunks ahead).
//
// Initialises `backend` on the render thread (all GL lives there). Returns 1 when running,
// 0 when the backend failed to initialise there (fatal, as it would be on the main thread),
// -1 when the thread could not be started at all (fall back to the synchronous path).
// vertex_worker: also start the vertex worker on cores 0 and 1, below the game and
// render threads (mkwv/gx/vertex_worker.hpp).
enum RenderSync { kSyncNone = 0, kSyncAheadOne = 1, kSyncFrame = 2 };
int start_render_thread(gx::Backend* backend, int sync_mode, bool vertex_worker);
// Before start_render_thread: one vertex worker thread instead of two (marker one_vertex_worker).
void set_single_vertex_worker(bool single);
// Vertex workers below the sound thread (the pre-static65 priority). Before start_render_thread.
void set_vertex_workers_low(bool low);

} // namespace mkwv::platform
