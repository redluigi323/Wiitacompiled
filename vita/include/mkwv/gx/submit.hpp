#pragma once

#include <cstdint>

#include "mkwv/gx/fifo.hpp"

namespace mkwv::gx {

// Renders the Processor's current batch - all of a frame, or the part of it decoded before
// the Processor filled up. Does not clear or present.
bool submit_processor(const Processor& processor);

// Ends the frame for the renderer: the next submit_processor starts a new frame (letterbox,
// debug counters, front-buffer semantics for early EFB copies).
void end_submit_frame();

// Whether a batch of the current frame has already been submitted.
bool frame_submit_open();

// Performs a drain's EFB copies without drawing, clearing or presenting - for a drain that
// carries copies but no draws.
void submit_efb_copies_only(const Processor& processor);

// The EFB render target (gl_submit.cpp). efb_target_init creates it once vitaGL is up;
// if that fails, GX keeps drawing into the back buffer as before.
bool efb_target_init();
bool efb_target_active();
// Starts a frame: binds the EFB target and schedules the new frame's clear (to the copy
// clear colour) for after the frame's leading EFB copies. Without the target, clears the
// back buffer at once.
void begin_efb_frame(std::uint32_t clear_rgba);
// Shows the display copy's source rect of the EFB target across the whole back buffer.
// The rect is anamorphic 16:9; stretching it is the widescreen present.
// The caller swaps. Returns false (and does nothing) without the target.
bool present_efb();

// gl_backend.cpp. With a render thread, the swap after a presented frame is deferred:
// present_processor only submits, and the render thread calls complete_deferred_swap() once
// it has released the guest. Returns whether a swap was pending.
void set_deferred_swap(bool deferred);
bool complete_deferred_swap();
// Whether vglSwapBuffers waits for vblank (on by default).
void set_vsync(bool on);

// One-line record of what the last submit_processor actually did; printed by the watchdog.
const char* last_submit_debug();

// Turns the compiled-TEV program path on or off. Off falls every draw back to the
// fixed-function GL_COMBINE path, which is the behaviour that shipped before this. Having
// the switch at runtime matters because a full rebuild here is expensive: main.cpp flips
// it from a marker file so the two paths can be compared from the same VPK.
void set_shader_path_enabled(bool enabled);
// EFB render scale (EFB-target pixels per GX pixel, 0.5 .. 544/456, default 544/456). Call
// before the backend initialises. Lower is fewer pixels to shade, upscaled at present.
void set_efb_scale(float scale);
float efb_scale();
// GPU vertex ring (static64): `vbo` is a GL buffer the renderer re-points (vglBufferData) at
// the run being drawn when its vertices lie in [base, base + bytes) - the ring the vertex jobs
// pack into (vertex_ring_install). vbo 0 turns it off.
void set_vertex_ring_buffer(unsigned vbo, const std::uint8_t* base, std::uint32_t bytes);
// Before the backend initialises: whether it allocates the GPU vertex ring (no_vbo_ring) and
// the display-list cache pool (no_dl_cache). Vita backend only.
void set_vertex_ring_enabled(bool on);
void set_dl_cache_memory(bool on);
// Consecutive draws with identical state go out as one glDrawElements (on by default).
void set_draw_merging(bool enabled);
// EFB-copy effects: 2 = all copies (default), 1 = skip post-processing copies (anything but
// plain colour snapshots), 0 = same as 1 for now.
void set_effects_level(int level);
// The race GPU contact sheet (static44 diagnostic): batch-end EFB images of the automatic race
// capture shown as a 3x3 grid instead of the game. Off unless the `race_sheet` marker is set.
void set_race_probe_sheet(bool enabled);
bool shader_path_enabled();

} // namespace mkwv::gx
