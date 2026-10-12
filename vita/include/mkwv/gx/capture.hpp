#pragma once

// Frame capture: writes one frame, exactly as the renderer receives it, to a directory so it
// can be replayed and inspected off-device (tools/gx_replay.cpp).
//
// What is written is the decoder's *output*: every draw's snapshot (TEV, blend, depth, raster,
// texture objects), the transformed vertices, each texture decoded to RGBA8, and the frame's
// EFB copies. A replay of that on the host with a reference TEV answers the question that
// screenshots cannot: is a wrong-looking frame wrong in what the decoder hands the renderer,
// or in how the renderer (shader, GL, emulator) draws it.
//
// Text for the state (readable and independent of struct layout), binary for vertices and
// texels. Not part of fifo.hpp or anything the translated shards include.

#include <cstdint>

namespace mkwv::gx {

struct Processor;

// Arms a capture of the next frame into `dir` (created if missing). The next batch that
// reaches the renderer starts it; capture_frame_end() finishes it.
void capture_arm(const char* dir);
bool capture_armed();
bool capture_active();
const char* capture_directory();
unsigned capture_batch_count();

// Called for every batch the renderer submits while armed/active.
void capture_batch(const Processor& processor);

// Called when the renderer finishes the frame. Returns true when a capture was completed.
bool capture_frame_end();

// GPU colour checkpoints, only while a one-frame capture is active. Rows are top-down.
void capture_gpu_image(const char* label, const std::uint8_t* rgba, unsigned width, unsigned height);

// The last completed capture's summary line, for the log.
const char* capture_last_summary();

} // namespace mkwv::gx
