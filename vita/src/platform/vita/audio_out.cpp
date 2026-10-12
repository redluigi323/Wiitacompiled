#include "mkwv/platform/audio_out.hpp"

#include "mkwv/gx/perf.hpp"
#include "mkwv/runtime/hle_audio.hpp"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mkwv::platform {
namespace {

// The AI blocks (3 ms, 96 frames at 32 kHz, from the guest thread) go into this ring; a
// high-priority thread plays it through a BGM port (the one port type that takes 32 kHz)
// in 256-frame grains. The guest delivers blocks in bursts - one burst per poll, and a slow
// frame is a long gap between polls - so playback starts (and restarts after running dry)
// only once a cushion has built up.
constexpr std::uint32_t kRate = 32000;
constexpr std::uint32_t kGrain = 256;               // frames per sceAudioOutOutput (8 ms)
constexpr std::uint32_t kRingFrames = 1u << 15;     // ~1 s
constexpr std::uint32_t kStartFrames = 3072;        // ~96 ms cushion before (re)starting
constexpr std::uint32_t kMaxQueuedFrames = 16000;   // ~0.5 s: beyond this, drop (catch up)

std::int16_t g_ring[kRingFrames * 2];
std::atomic<std::uint32_t> g_write{0};  // frames written (guest thread)
std::atomic<std::uint32_t> g_read{0};   // frames consumed (audio thread)
std::atomic<std::uint32_t> g_underruns{0};
std::atomic<std::uint32_t> g_dropped{0};
int g_port = -1;

void sink(const std::int16_t* lr, std::uint32_t frames) {
    const std::uint32_t w = g_write.load(std::memory_order_relaxed);
    const std::uint32_t r = g_read.load(std::memory_order_acquire);
    if (w - r + frames > kMaxQueuedFrames) {
        g_dropped.fetch_add(frames, std::memory_order_relaxed);
        return;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint32_t slot = (w + i) & (kRingFrames - 1u);
        g_ring[slot * 2] = lr[i * 2];
        g_ring[slot * 2 + 1] = lr[i * 2 + 1];
    }
    g_write.store(w + frames, std::memory_order_release);
}

int audio_main(SceSize, void*) {
    static std::int16_t grain[kGrain * 2];
    bool playing = false;
    for (;;) {
        const std::uint32_t r = g_read.load(std::memory_order_relaxed);
        const std::uint32_t avail = g_write.load(std::memory_order_acquire) - r;
        if (!playing && avail >= kStartFrames) {
            playing = true;
        }
        if (playing && avail >= kGrain) {
            for (std::uint32_t i = 0; i < kGrain; ++i) {
                const std::uint32_t slot = (r + i) & (kRingFrames - 1u);
                grain[i * 2] = g_ring[slot * 2];
                grain[i * 2 + 1] = g_ring[slot * 2 + 1];
            }
            g_read.store(r + kGrain, std::memory_order_release);
        } else {
            if (playing) {
                g_underruns.fetch_add(1, std::memory_order_relaxed);
                playing = false;
            }
            std::memset(grain, 0, sizeof(grain));
        }
        sceAudioOutOutput(g_port, grain);  // blocks until the previous grain has played
    }
    return 0;
}

} // namespace

bool start_audio_out() {
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, kGrain, kRate, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) {
        std::printf("mkwv: audio out port failed 0x%08X\n", static_cast<unsigned>(g_port));
        return false;
    }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(g_port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                         vol);
    // Above the vertex worker, on its core: this thread copies one grain and then blocks in
    // sceAudioOutOutput, so the preemption is a short wake every 8 ms. Affinity 0 used to let
    // it land on the game thread.
    const SceUID tid = sceKernelCreateThread("mkwv_audio", audio_main, 0x10000100 - 20, 64 * 1024, 0,
                                             SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (tid < 0) {
        std::printf("mkwv: audio thread create failed 0x%08X\n", static_cast<unsigned>(tid));
        return false;
    }
    sceKernelStartThread(tid, 0, nullptr);
    perf::register_thread("aout", tid);
    runtime::set_audio_sink(&sink);
    return true;
}

void audio_out_stats(std::uint32_t* queued_ms, std::uint32_t* underruns, std::uint32_t* dropped_ms) {
    if (queued_ms != nullptr) {
        *queued_ms = (g_write.load() - g_read.load()) * 1000u / kRate;
    }
    if (underruns != nullptr) {
        *underruns = g_underruns.load();
    }
    if (dropped_ms != nullptr) {
        *dropped_ms = g_dropped.load() * 1000u / kRate;
    }
}

} // namespace mkwv::platform
