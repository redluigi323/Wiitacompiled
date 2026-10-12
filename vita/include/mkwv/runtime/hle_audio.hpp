#pragma once

// Audio HLE (vita/src/runtime/audio): the AI DMA engine and the DSP running the AX ucode, and
// the cooperative guest threads audio needs (nw4r's sound thread and its stream-loading task
// thread). Not shard-visible.

#include "mkwv/runtime/cpu_context.hpp"

#include <cstddef>
#include <cstdint>

namespace mkwv::runtime {

// Receives each 3 ms AI block: `frames` stereo frames, host-order int16, left then right.
using AudioSink = void (*)(const std::int16_t* lr, std::uint32_t frames);
void set_audio_sink(AudioSink sink);
// Dolphin's free DSP coefficient ROM (dsp_coef.bin), used for polyphase resampling.
void set_audio_coef_rom(const char* path);
// Guest thread, at a point where an interrupt could be taken: delivers the AI blocks that are
// due by wall-clock time, running the guest's AI DMA callback for each.
void audio_poll(CpuContext* ctx);
bool audio_in_interrupt();
// While the guest thread waits inside a call that took `ctx` (VIWaitForRetrace's present), its
// waits may poll audio on that context: audio_poll_waiting() does, when one is set.
void audio_set_wait_ctx(CpuContext* ctx);
void audio_poll_waiting();
void audio_stats(std::uint32_t* blocks, std::uint32_t* dropped_ms);
// Off: the AI DMA never starts, so AX and nw4r's sound thread never run (as before audio).
void set_audio_enabled(bool enabled);
// Real-hardware budget switches (main.cpp markers):
//   reverb off (default on the Vita): nw4r's aux effect callback (AxManager::AuxCallbackFunc,
//     FxReverbHi/Std in translated code, 333 times a second) returns silence instead.
//   mix inline: the DSP mix runs inside the audio interrupt again (under the guest lock).
void set_audio_reverb(bool enabled);
bool audio_reverb_enabled();
void set_audio_mix_inline(bool inline_mix);
// Percentage of AX's DSP-cycle budget left to the game (100 = untouched). Lower makes AX drop
// its lowest-priority voices earlier (markers snd_voice_cap = 60, snd_voice_cap_low = 35).
void set_audio_voice_cap(unsigned percent);
// The platform output's state, for the PERF line.
using AudioOutStats = void (*)(std::uint32_t* queued_ms, std::uint32_t* underruns, std::uint32_t* dropped_ms);
void set_audio_out_stats(AudioOutStats fn);
// " | audio: ..." for the PERF line (blocks since the last call).
void audio_perf_text(char* out, std::size_t size);

// ---- Cooperative guest threads (hle_os.cpp) ----
// A handful of guest OSThreads run as fibers on the guest's host thread: they run when woken
// (a message sent to the queue they wait on, OSWakeupThread on their thread queue, a mutex they
// wait for being released) until they block again. Everything else keeps the inline model.
// Host stacks for nw4r's sound and task threads. On a Vita these have to be slices of the
// host thread's own stack: the kernel reports a stack overflow (stop reason 0x10006) as soon
// as SP leaves the region passed to sceKernelCreateThread, which a separately allocated fiber
// stack does. kFiberThreadReserve stays at the top for the host thread itself.
constexpr unsigned kMaxFibers = 4;
constexpr std::size_t kFiberStackBytes = 256u * 1024u;
constexpr std::size_t kFiberThreadReserve = 256u * 1024u;

struct FiberOps {
    void* (*create)(std::size_t stack_bytes, void (*entry)(void*), void* arg);
    void (*switch_to)(void* fiber);  // returns when something switches back to the caller
    void* main;                      // the guest thread's own context
};
void set_fiber_ops(const FiberOps& ops);
// Runs every fiber that is ready until each has blocked again. A no-op inside a fiber.
void schedule_guest_fibers();
bool any_fiber_ready();
// The sound thread (guest_sync.hpp): request it before the game starts; AIStartDMA starts it.
void set_sound_thread_requested(bool requested);
bool sound_thread_running();
// Body of the audio host thread (never returns).
void audio_thread_main();
// OSSleepThread from a fiber: blocks it on `queue` and returns true; false on the main thread.
bool fiber_sleep_on(std::uint32_t queue);
// The guest's OSDisableInterrupts state (audio blocks are only delivered with interrupts on).
bool os_interrupts_enabled();

} // namespace mkwv::runtime

extern "C" {
void AIClockInit_801a1138(CpuContext* ctx);
void AIGetDMABytesLeft_8012405c(CpuContext* ctx);
void AIGetDMAStartAddr_8012406c(CpuContext* ctx);
void AIGetDMALength_80124084(CpuContext* ctx);
void AIGetDSPSampleRate_8012409c(CpuContext* ctx);
void DSPCheckMailToDSP_8015d3fc(CpuContext* ctx);
void DSPCheckMailFromDSP_8015d40c(CpuContext* ctx);
void DSPReadMailFromDSP_8015d41c(CpuContext* ctx);
void DSPSendMailToDSP_8015d430(CpuContext* ctx);
void DSPInit_8015d444(CpuContext* ctx);
void DSPCheckInit_8015d504(CpuContext* ctx);
void DSPAddTask_8015d50c(CpuContext* ctx);
void DSPAssertTask_8015d57c(CpuContext* ctx);
void DSP_boot_task_8015dc60(CpuContext* ctx);
}
