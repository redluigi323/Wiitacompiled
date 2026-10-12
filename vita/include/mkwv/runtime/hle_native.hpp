#pragma once

#include "mkwv/runtime/cpu_context.hpp"

#include <cstdint>

namespace mkwv::gx {
struct Processor;
}

namespace mkwv::runtime {

class HleFifo;

using HlePresentFn = void (*)(gx::Processor&);

void register_hle_natives();
void unbind_hle_natives();
TranslatedFn find_hle_native(std::uint32_t address);
// How many translated functions outside the shards registered themselves (see
// mkwv_register_extra_native in hle_native.cpp); logged at boot.
unsigned extra_native_count();

void bind_hle_frame(HleFifo* fifo, gx::Processor* processor, HlePresentFn present);

// Render offload: with a render thread, the guest thread stops decoding. chunk() receives a
// full FIFO buffer's bytes (which may end mid-command), present() the frame's last bytes.
// Both copy the bytes before returning; present() also applies the sync policy (waiting for
// the render thread where the guest must not run ahead). The render thread feeds what it
// receives back through render_consume_chunk/render_consume_present, in order.
struct RenderOffload {
    void (*chunk)(const std::uint8_t* data, std::uint32_t size);
    void (*present)(const std::uint8_t* data, std::uint32_t size);
    // Once per guest frame (VIWaitForRetrace, after its present); may be null.
    void (*frame_boundary)();
};
// Called by VIWaitForRetrace: the renderer's per-frame pacing point (render_thread.hpp).
void hle_frame_boundary();
void set_render_offload(const RenderOffload* offload);
// Render-thread mode: hands the FIFO's bytes to the render thread as soon as they pass
// `threshold` bytes (called by the GX natives at command boundaries: GXBegin,
// GXCallDisplayList), so the render thread decodes a frame while the guest is still building
// it instead of all at once at the present. 0 turns it off (hand over only when the FIFO
// buffer fills and at the present).
void set_render_handoff_threshold(std::uint32_t threshold);
void hle_fifo_early_handoff();
// Draw one frame, then skip `frames` (0 = draw every frame). See finish_present.
void set_frameskip(unsigned frames);
void render_consume_chunk(const std::uint8_t* data, std::uint32_t size);
void render_consume_present(const std::uint8_t* data, std::uint32_t size);
void unbind_hle_frame();
void hle_present_frame();
const char* hle_take_draw_dump();
std::uint32_t hle_fifo_bytes(const std::uint8_t** out);
void hle_frame_stats(std::uint32_t* frames, std::uint32_t* draws, std::uint32_t* verts, std::uint32_t* fifo,
                     std::uint32_t* max_draws, std::uint32_t* skipped);

} // namespace mkwv::runtime

// Called from static initializers in the build-generated extra_translated.cpp.
extern "C" void mkwv_register_extra_native(std::uint32_t address, mkwv::runtime::TranslatedFn fn);
