// AI / DSP boundary of the audio HLE, ported from upstream WiiCompiled
// (runtime/src/hle/audio/audio.cpp) onto this runtime.
//
// The game's AX library runs translated. What it expects from hardware is emulated here:
//   - the DSP running the AX ucode: mails, task callbacks and the command-list mix (ax_mix.cpp,
//     upstream's port of Dolphin's AX HLE);
//   - the AI DMA engine: every 3 ms block (96 stereo samples at 32 kHz) the DMA'd buffer goes to
//     the audio sink and the guest's AI DMA callback runs, which starts the next DSP mix and runs
//     __AXOutNewFrame (voice sync, the game's AX callbacks - nw4r's sound thread tick).
// Blocks are paced by wall-clock time, delivered at poll points on the guest thread
// (audio_poll): VIWaitForRetrace, the sleeps, and the waits for the render thread.

#include "mkwv/runtime/hle_audio.hpp"
#include "mkwv/runtime/guest_prof.hpp"
#include "mkwv/runtime/guest_sync.hpp"
#include "mkwv/runtime/hle_os.hpp"
#include "mkwv/runtime/compact_memory.hpp"

#include "ax_dsp.h"
#include "ax_internal.h"

#include "abi_bridge.h"
#include "memory.h"
#include "mkwv/gx/perf.hpp"
#include "mkwv/runtime/diagnostics.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace prof = mkwv::runtime::prof;

namespace {

constexpr std::uint32_t kSampleRate = 32000u;
constexpr std::uint32_t kChannels = 2u;
constexpr std::uint32_t kBytesPerSample = 2u;
constexpr std::uint32_t kAIInitializedAddr = 0x80386448u;
constexpr std::uint32_t kAICallbackBusyAddr = 0x8038644Cu;
constexpr std::uint32_t kAICallbackStackSwitchAddr = 0x8038647Cu;
constexpr std::uint32_t kAIDmaCallbackAddr = 0x80386480u;
// A long frame (a slow race frame, a load) leaves many blocks due at once; deliver up to
// ~0.2 s worth per poll and drop older backlog, so a stall cannot snowball.
constexpr int kMaxBlocksPerPoll = 64;
constexpr std::uint64_t kMaxPollDeltaUs = 250000u;
// nw4r::snd::SoundPlayer::mVolume.
constexpr std::uint32_t kSoundPlayerVolumeOffset = 0x2Cu;

struct AIDmaState {
    std::uint32_t start_addr = 0;
    std::uint32_t register_start = 0;
    std::uint32_t length = 0;
    std::uint32_t callback = 0;
    std::uint32_t bytes_left = 0;
    bool enabled = false;
    bool in_tick = false;
    std::uint64_t last_poll_us = 0;
    std::uint64_t accumulated_us = 0;  // wall time not yet turned into blocks
    std::uint32_t blocks = 0;
    std::uint32_t dropped_us = 0;
};

AIDmaState g_ai{};
mkwv::runtime::AudioSink g_sink = nullptr;
mkwv::runtime::AudioOutStats g_out_stats = nullptr;
bool g_audio_enabled = true;
bool g_in_audio_irq = false;

// The sound thread (guest_sync.hpp): the audio interrupt and nw4r's sound/task threads on
// their own host thread. Requested by main.cpp (marker no_snd_thread keeps them on the game
// thread); switched on by AIStartDMA, before nw4r creates its threads, so every fiber only ever
// runs on the audio thread (a fiber must not move between host threads: thread_local accesses
// may be cached across its switch points).
bool g_snd_thread_requested = false;
bool g_snd_thread_started = false;
CpuContext g_irq_ctx{};  // the audio interrupt's guest context (SDA registers, its own stack)
// The interrupt's guest stack: the top of the FST reserve in MEM2 (the FST is at most 64 KiB of
// the 2 MiB reserved).
constexpr std::uint32_t kIrqStackTop =
    mkwv::runtime::kDvdFstBase + mkwv::runtime::kDvdFstReserve - 0x100u;
std::atomic<std::uint64_t> g_snd_busy_us{0};
// Split of the sound thread's busy time: the audio interrupt's guest code (under the lock),
// the deferred DSP mix, and nw4r's threads (the fibers).
std::atomic<std::uint64_t> g_snd_irq_us{0};
std::atomic<std::uint64_t> g_snd_mix_us{0};
std::atomic<std::uint64_t> g_snd_fiber_us{0};
// The DSP mix runs after the audio interrupt, outside the guest lock (sound thread only).
// Marker mix_inline keeps it inside the interrupt, as before static60.
bool g_mix_deferred = false;
bool g_mix_inline = false;
// nw4r's aux effects (reverb). Off by default on the Vita; see AuxCallbackFunc below.
bool g_reverb = true;
// static65: percentage of AX's DSP-cycle budget left to the game (snd_voice_cap markers). AX
// itself (__AXSyncPBs) stops the lowest-priority voices when the running voices' estimated DSP
// cycles exceed __AXMaxDspCycles (SDA r13-26200, AXSetMaxDspCycles 0x80127FDC): the Wii's own
// voice culling, which saves both the mix and nw4r's per-voice work. 100 = untouched.
std::uint32_t g_voice_cap_pct = 100;
std::uint32_t g_voice_cap_written = 0;

void apply_voice_cap(CpuContext& cb) {
    if (g_voice_cap_pct >= 100u) {
        return;
    }
    const std::uint32_t addr = cb.gpr[13] - 26200u;
    const std::uint32_t v = Memory::Read32(addr);
    if (v != 0 && v != g_voice_cap_written) {
        g_voice_cap_written = static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) * g_voice_cap_pct / 100u);
        Memory::Write32(addr, g_voice_cap_written);
    }
}
// Start-up handshake: the audio thread reports its thread pointer, the game thread checks it
// differs from its own (guest_thread_slot() depends on it) and then lets it run (1) or not (2).
std::atomic<std::uintptr_t> g_audio_tp_candidate{0};
std::atomic<int> g_audio_ready{0};
std::atomic<int> g_audio_go{0};

void set_r3(CpuContext* ctx, std::uint32_t value) {
    if (ctx != nullptr) {
        ctx->gpr[3] = value;
    }
}

std::uint32_t arg(CpuContext* ctx, unsigned i) {
    return ctx != nullptr ? ctx->gpr[3 + i] : 0u;
}

// The DMA'd block: big-endian 16-bit, ordered right, left. The sink takes host-order left, right.
void push_block(std::uint32_t start, std::uint32_t length) {
    if (g_sink == nullptr || start == 0 || length < 4) {
        return;
    }
    static std::int16_t lr[2048];
    const std::uint32_t frames = length / 4u > 1024u ? 1024u : length / 4u;
    const std::uint8_t* src = Memory::GetPointer(start, frames * 4u);
    for (std::uint32_t i = 0; i < frames; ++i) {
        std::uint16_t right, left;
        if (src != nullptr) {
            right = static_cast<std::uint16_t>((src[i * 4] << 8) | src[i * 4 + 1]);
            left = static_cast<std::uint16_t>((src[i * 4 + 2] << 8) | src[i * 4 + 3]);
        } else {
            right = Memory::Read16(start + i * 4u);
            left = Memory::Read16(start + i * 4u + 2u);
        }
        lr[i * 2] = static_cast<std::int16_t>(left);
        lr[i * 2 + 1] = static_cast<std::int16_t>(right);
    }
    g_sink(lr, frames);
}

// Delivers the AI blocks that are due. Each block: the buffer the DMA just played goes to the
// sink, then the AI DMA callback runs (it points the DMA at the next buffer and starts the next
// DSP mix), then the DSP task callbacks it left pending, then the guest threads it woke.
void tick(CpuContext* ctx) {
    const bool threaded = mkwv::runtime::on_audio_thread();
    if (!g_ai.enabled || g_ai.in_tick || g_ai.start_addr == 0 || g_ai.length == 0 || ctx == nullptr ||
        (!threaded && !mkwv::runtime::os_interrupts_enabled())) {
        return;
    }
    const std::uint64_t now = mkwv::perf::now_us();
    if (g_ai.last_poll_us == 0) {
        g_ai.last_poll_us = now;
        return;
    }
    std::uint64_t delta = now - g_ai.last_poll_us;
    g_ai.last_poll_us = now;
    if (delta > kMaxPollDeltaUs) {
        g_ai.dropped_us += static_cast<std::uint32_t>(delta - kMaxPollDeltaUs);
        delta = kMaxPollDeltaUs;
    }
    g_ai.accumulated_us += delta;

    g_ai.in_tick = true;
    CpuContext cb = *ctx;  // the callbacks run as an interrupt would, below the caller's stack
    for (int n = 0; n < kMaxBlocksPerPoll; ++n) {
        const std::uint64_t block_us =
            static_cast<std::uint64_t>(g_ai.length) * 1000000u / (kSampleRate * kChannels * kBytesPerSample);
        if (block_us == 0 || g_ai.accumulated_us < block_us) {
            break;
        }
        g_ai.accumulated_us -= block_us;
        if (threaded) {
            // Between sound frames nothing of the guest is held here: a game thread waiting
            // for a free guest mutex gets it before the next frame's sound thread can.
            mkwv::runtime::audio_yield_to_game();
        }
        g_ai.bytes_left = 0;
        push_block(g_ai.start_addr, g_ai.length);
        ++g_ai.blocks;
        if (g_ai.callback != 0) {
            {
                prof::ActivityScope irq_activity(prof::kAudioIrq);
                // On the audio thread the interrupt holds the guest lock: the game thread's
                // critical sections wait for it, as interrupts would be masked on the Wii.
                mkwv::runtime::IrqLockScope irq_lock;
                const std::uint64_t irq_t0 = threaded ? mkwv::perf::now_us() : 0;
                AxDspHle::g_axGuestCtx = &cb;
                g_in_audio_irq = true;
                // The previous frame's mix (deferred) publishes its aux outputs before
                // __AXOutNewFrame's aux callbacks read them.
                AxDspHle::JoinMixWorker();
                apply_voice_cap(cb);
                Memory::Write32(kAICallbackBusyAddr, 1);
                InvokeIndirectCpu(g_ai.callback, &cb);
                Memory::Write32(kAICallbackBusyAddr, 0);
                if (!g_mix_deferred) {
                    AxDspHle::ServiceDeferredCallbacks();
                }
                g_in_audio_irq = false;
                AxDspHle::g_axGuestCtx = nullptr;
                if (threaded) {
                    g_snd_irq_us.fetch_add(mkwv::perf::now_us() - irq_t0, std::memory_order_relaxed);
                }
            }
            if (g_mix_deferred) {
                // The DSP's half: the command list the callback just started is mixed here,
                // without the guest lock, so the game thread's critical sections do not wait
                // for it. Its completion interrupt (the resume callback) follows under the lock.
                const std::uint64_t mix_t0 = mkwv::perf::now_us();
                {
                    prof::ActivityScope mix_activity(prof::kAudioMix);
                    AxDspHle::RunDeferredMix();
                }
                g_snd_mix_us.fetch_add(mkwv::perf::now_us() - mix_t0, std::memory_order_relaxed);
                prof::ActivityScope irq_activity(prof::kAudioIrq);
                mkwv::runtime::IrqLockScope irq_lock;
                AxDspHle::g_axGuestCtx = &cb;
                g_in_audio_irq = true;
                AxDspHle::ServiceDeferredCallbacks();
                g_in_audio_irq = false;
                AxDspHle::g_axGuestCtx = nullptr;
            }
            // Threads the frame woke (nw4r's sound thread) run now, before the interrupted
            // thread resumes - they have the higher priority.
            const std::uint64_t fib_t0 = threaded ? mkwv::perf::now_us() : 0;
            mkwv::runtime::schedule_guest_fibers();
            if (threaded) {
                g_snd_fiber_us.fetch_add(mkwv::perf::now_us() - fib_t0, std::memory_order_relaxed);
            }
        }
        g_ai.bytes_left = g_ai.length;
    }
    // Backlog beyond what one poll delivers is dropped rather than carried (audio skips
    // instead of the game falling further behind).
    const std::uint64_t cap = static_cast<std::uint64_t>(g_ai.length) * 2u * 1000000u /
                              (kSampleRate * kChannels * kBytesPerSample);
    if (g_ai.accumulated_us > cap) {
        g_ai.dropped_us += static_cast<std::uint32_t>(g_ai.accumulated_us - cap);
        g_ai.accumulated_us = cap;
    }
    g_ai.in_tick = false;
}

} // namespace

namespace mkwv::runtime {

void set_audio_sink(AudioSink sink) {
    g_sink = sink;
}

void set_audio_coef_rom(const char* path) {
    AxDspHle::SetCoefficientRomPath(path);
}

void audio_poll(CpuContext* ctx) {
    if (guest_threads_active()) {
        return;  // the audio thread keeps time itself
    }
    tick(ctx);
}

void set_sound_thread_requested(bool requested) {
    g_snd_thread_requested = requested;
}

bool sound_thread_running() {
    return g_snd_thread_started;
}

void audio_thread_main() {
    g_audio_tp_candidate.store(host_thread_pointer(), std::memory_order_relaxed);
    use_private_dispatch_cache();
    // Fibers have to be carved out of this thread's stack before the game thread returns from
    // AIStartDMA and creates nw4r's threads. The handshake below is what holds it.
    const GuestThreadOps& ops = guest_thread_ops();
    if (ops.init_fibers != nullptr && !ops.init_fibers()) {
        std::printf("mkwv: audio: sound thread could not become the fiber scheduler\n");
    }
    g_audio_ready.store(1, std::memory_order_release);
    while (g_audio_go.load(std::memory_order_acquire) == 0) {
        host_sleep_us(100);
    }
    if (g_audio_go.load() != 1) {
        return;  // refused: the game thread keeps the audio (see AIStartDMA)
    }
    g_mix_deferred = !g_mix_inline;
    AxDspHle::SetDeferredMix(g_mix_deferred);
    std::printf("mkwv: audio: sound thread running (interrupt stack 0x%08X, mix %s)\n", kIrqStackTop,
                g_mix_deferred ? "outside the lock" : "inside the interrupt");
    CpuContext& irq = g_irq_ctx;
    for (;;) {
        // Blocks fall due every 3 ms; a wake (a guest thread made ready by the game thread)
        // cuts the wait short.
        {
            prof::ActivityScope idle(prof::kSleep);
            if (ops.wait != nullptr) {
                ops.wait(1000);
            } else {
                host_sleep_us(1000);
            }
        }
        const std::uint64_t t0 = mkwv::perf::now_us();
        tick(&irq);
        const std::uint64_t t1 = mkwv::perf::now_us();
        schedule_guest_fibers();
        const std::uint64_t t2 = mkwv::perf::now_us();
        g_snd_fiber_us.fetch_add(t2 - t1, std::memory_order_relaxed);
        g_snd_busy_us.fetch_add(t2 - t0, std::memory_order_relaxed);
    }
}

bool audio_in_interrupt() {
    return g_in_audio_irq;
}

namespace {
CpuContext* g_wait_ctx = nullptr;
}

void audio_set_wait_ctx(CpuContext* ctx) {
    g_wait_ctx = ctx;
}

void audio_poll_waiting() {
    if (g_wait_ctx != nullptr && !guest_threads_active()) {
        tick(g_wait_ctx);
    }
}

void set_audio_enabled(bool enabled) {
    g_audio_enabled = enabled;
}

void set_audio_reverb(bool enabled) {
    g_reverb = enabled;
    AxDspHle::SetAuxSends(enabled);
}

bool audio_reverb_enabled() {
    return g_reverb;
}

void set_audio_voice_cap(unsigned percent) {
    g_voice_cap_pct = percent == 0 || percent > 100 ? 100u : percent;
}

void set_audio_mix_inline(bool inline_mix) {
    g_mix_inline = inline_mix;
}

void set_audio_out_stats(AudioOutStats fn) {
    g_out_stats = fn;
}

void audio_perf_text(char* out, std::size_t size) {
    static std::uint32_t s_blocks = 0;
    std::uint32_t queued = 0, underruns = 0, out_dropped = 0;
    if (g_out_stats != nullptr) {
        g_out_stats(&queued, &underruns, &out_dropped);
    }
    // snd: the sound thread's busy share of the window (audio interrupt + nw4r threads).
    static std::uint64_t s_busy = 0, s_t = 0, s_t_prev_for_split = 0;
    const std::uint64_t busy = g_snd_busy_us.load(std::memory_order_relaxed);
    const std::uint64_t now = mkwv::perf::now_us();
    const double snd = (s_t != 0 && now > s_t) ? 100.0 * static_cast<double>(busy - s_busy) / static_cast<double>(now - s_t) : 0.0;
    s_busy = busy;
    s_t = now;
    // irq / mix / fib: the same window split into the audio interrupt's guest code (under the
    // guest lock), the DSP mix, and nw4r's sound/task threads.
    static std::uint64_t s_irq = 0, s_mix = 0, s_fib = 0;
    const std::uint64_t irq = g_snd_irq_us.load(std::memory_order_relaxed);
    const std::uint64_t mix = g_snd_mix_us.load(std::memory_order_relaxed);
    const std::uint64_t fib = g_snd_fiber_us.load(std::memory_order_relaxed);
    const double span = (s_t_prev_for_split != 0 && now > s_t_prev_for_split) ? static_cast<double>(now - s_t_prev_for_split) : 0.0;
    auto pct = [&](std::uint64_t a, std::uint64_t b) { return span > 0.0 ? 100.0 * static_cast<double>(a - b) / span : 0.0; };
    std::uint32_t voices = 0, lists = 0;
    AxDspHle::TakeVoiceStats(&voices, &lists);
    std::snprintf(out, size, " | audio: blocks=%u q=%ums under=%u late=%ums drop=%ums snd=%.0f%% (irq=%.0f mix=%.0f fib=%.0f) vox=%.1f cap=%u%%",
                  g_ai.blocks - s_blocks, queued, underruns, g_ai.dropped_us / 1000u, out_dropped,
                  g_snd_thread_started ? snd : -1.0, pct(irq, s_irq), pct(mix, s_mix), pct(fib, s_fib),
                  lists != 0 ? static_cast<double>(voices) / lists : 0.0, g_voice_cap_pct);
    s_irq = irq;
    s_mix = mix;
    s_fib = fib;
    s_t_prev_for_split = now;
    s_blocks = g_ai.blocks;
}

void audio_stats(std::uint32_t* blocks, std::uint32_t* dropped_ms) {
    if (blocks != nullptr) {
        *blocks = g_ai.blocks;
    }
    if (dropped_ms != nullptr) {
        *dropped_ms = g_ai.dropped_us / 1000u;
    }
}

} // namespace mkwv::runtime

// ---- natives (addresses from the PAL main.dol; upstream audio.cpp overrides the same set) ----

extern "C" void AIClockInit_801a1138(CpuContext*) {}

extern "C" void OSInitAudioSystem_801a1358(CpuContext* ctx) {
    AxDspHle::g_axGuestCtx = ctx;
    AxDspHle::InitAram();
    AxDspHle::Init();
    AxDspHle::g_axGuestCtx = nullptr;
}

extern "C" void OSStopAudioSystem_801a1520(CpuContext*) {
    g_ai.enabled = false;
    g_ai.register_start = 0;
    g_ai.bytes_left = 0;
    g_ai.accumulated_us = 0;
    AxDspHle::Stop();
}

extern "C" void AIInit_801240b0(CpuContext* ctx) {
    const bool already = Memory::Read32(kAIInitializedAddr) == 1u;
    if (!already) {
        g_ai.callback = 0;
        Memory::Write32(kAIDmaCallbackAddr, 0);
        Memory::Write32(kAICallbackBusyAddr, 0);
        Memory::Write32(kAICallbackStackSwitchAddr, arg(ctx, 0));
        Memory::Write32(kAIInitializedAddr, 1);
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::printf("mkwv: audio: AIInit (HLE, 32 kHz)\n");
    }
}

extern "C" void AICheckInit_80124094(CpuContext* ctx) {
    set_r3(ctx, Memory::Read32(kAIInitializedAddr));
}

extern "C" void AIRegisterDMACallback_80123f88(CpuContext* ctx) {
    const std::uint32_t old = Memory::Read32(kAIDmaCallbackAddr);
    g_ai.callback = arg(ctx, 0);
    Memory::Write32(kAIDmaCallbackAddr, g_ai.callback);
    set_r3(ctx, old);
}

extern "C" void AIInitDMA_80123fcc(CpuContext* ctx) {
    g_ai.start_addr = arg(ctx, 0);
    g_ai.register_start = arg(ctx, 0) & 0x1fffffe0u;
    g_ai.length = arg(ctx, 1) & 0x000fffe0u;
    g_ai.bytes_left = g_ai.length;
}

extern "C" void AIStartDMA_80124048(CpuContext* ctx) {
    if (!g_audio_enabled) {
        return;
    }
    if (!g_ai.enabled) {
        g_ai.last_poll_us = 0;
        g_ai.accumulated_us = 0;
        std::printf("mkwv: audio: AI DMA started (%u-byte blocks)\n", g_ai.length);
    }
    g_ai.enabled = true;
    g_ai.bytes_left = g_ai.length;
    // The sound thread starts with the DMA - before nw4r creates its sound and task threads.
    if (g_snd_thread_requested && !g_snd_thread_started && ctx != nullptr &&
        mkwv::runtime::guest_thread_ops().start_audio_thread != nullptr) {
        g_irq_ctx = *ctx;  // SDA bases (r2, r13) and the rest of the caller's state
        g_irq_ctx.gpr[1] = kIrqStackTop & ~15u;
        Memory::Write32(g_irq_ctx.gpr[1], 0);  // back chain
        g_irq_ctx.lr = 0;
        if (mkwv::runtime::guest_thread_ops().start_audio_thread()) {
            // Handshake: the two host threads must be told apart by their thread pointer.
            const std::uintptr_t mine = mkwv::runtime::host_thread_pointer();
            for (int i = 0; i < 2000 && g_audio_ready.load(std::memory_order_acquire) == 0; ++i) {
                mkwv::runtime::host_sleep_us(100);
            }
            const std::uintptr_t theirs = g_audio_tp_candidate.load(std::memory_order_relaxed);
            if (g_audio_ready.load() != 0 && theirs != 0 && theirs != mine) {
                mkwv::runtime::set_audio_thread_pointer(theirs);
                mkwv::runtime::set_guest_threads_active(true, !mkwv::runtime::os_interrupts_enabled());
                g_snd_thread_started = true;
                g_audio_go.store(1, std::memory_order_release);
                mkwv::runtime::watchdog_log_line("mkwv: audio: sound thread on (audio interrupt + nw4r threads)\n");
            } else {
                g_audio_go.store(2, std::memory_order_release);
                char line[160];
                std::snprintf(line, sizeof(line),
                              "mkwv: audio: sound thread refused (thread pointers %08X/%08X); audio stays on the game "
                              "thread\n", static_cast<unsigned>(mine), static_cast<unsigned>(theirs));
                mkwv::runtime::watchdog_log_line(line);
            }
        } else {
            std::printf("mkwv: audio: sound thread failed to start; audio stays on the game thread\n");
        }
    }
}

extern "C" void AIGetDMABytesLeft_8012405c(CpuContext* ctx) { set_r3(ctx, g_ai.bytes_left); }
extern "C" void AIGetDMAStartAddr_8012406c(CpuContext* ctx) { set_r3(ctx, g_ai.register_start); }
extern "C" void AIGetDMALength_80124084(CpuContext* ctx) { set_r3(ctx, g_ai.length); }
// AIDFR^1: 0 for 32 kHz.
extern "C" void AIGetDSPSampleRate_8012409c(CpuContext* ctx) { set_r3(ctx, 0); }

extern "C" void AXOutInitDSP_801269bc(CpuContext* ctx) {
    AxDspHle::g_axGuestCtx = ctx;
    AxDspHle::InitForAXOut(ctx);
    AxDspHle::g_axGuestCtx = nullptr;
    std::printf("mkwv: audio: __AXOutInitDSP (native AX/DSP HLE)\n");
}

extern "C" void DSPInit_8015d444(CpuContext* ctx) {
    AxDspHle::g_axGuestCtx = ctx;
    AxDspHle::Init();
    AxDspHle::g_axGuestCtx = nullptr;
}
extern "C" void DSPCheckInit_8015d504(CpuContext* ctx) { set_r3(ctx, AxDspHle::CheckInit()); }
extern "C" void DSPAddTask_8015d50c(CpuContext* ctx) {
    CpuContext* saved = AxDspHle::g_axGuestCtx;
    if (saved == nullptr) AxDspHle::g_axGuestCtx = ctx;
    const std::uint32_t r = AxDspHle::AddTask(arg(ctx, 0));
    AxDspHle::g_axGuestCtx = saved;
    set_r3(ctx, r);
}
extern "C" void DSP_boot_task_8015dc60(CpuContext* ctx) { AxDspHle::AssertTask(arg(ctx, 0)); }
extern "C" void DSPSendMailToDSP_8015d430(CpuContext* ctx) {
    CpuContext* saved = AxDspHle::g_axGuestCtx;
    if (saved == nullptr) AxDspHle::g_axGuestCtx = ctx;
    prof::ActivityScope mix_activity(prof::kAudioMix);
    AxDspHle::SendMailToDSP(arg(ctx, 0));
    AxDspHle::g_axGuestCtx = saved;
}
extern "C" void DSPCheckMailToDSP_8015d3fc(CpuContext* ctx) { set_r3(ctx, AxDspHle::CheckMailToDSP()); }
extern "C" void DSPCheckMailFromDSP_8015d40c(CpuContext* ctx) { set_r3(ctx, AxDspHle::CheckMailFromDSP()); }
extern "C" void DSPReadMailFromDSP_8015d41c(CpuContext* ctx) { set_r3(ctx, AxDspHle::ReadMailFromDSP()); }
extern "C" void DSPAssertTask_8015d57c(CpuContext* ctx) { set_r3(ctx, AxDspHle::AssertTask(arg(ctx, 0))); }

// nw4r::snd::detail::AxManager::AuxCallbackFunc(void* data, void* context), the callback AX's
// __AXProcessAux calls (through a pointer, so this native takes it) with the aux bus buffers
// the DSP mixed this frame. nw4r runs the bus's effects (FxReverbHi/Std, translated) over
// them in place; the DSP mixes the result back next frame. Reverb off: return what nw4r
// returns for a bus with no effects, three zeroed buffers (96 samples of s32 each).
extern "C" void AxManager_AuxCallbackFunc_800895d0(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    if (g_reverb) {
        static void (*original)(CpuContext*) = nullptr;
        if (original == nullptr) {
            const RawDispatchRecord* record = FindStaticIndirectDispatchEntry(
                g_publishedStaticIndirectDispatchTable.load(std::memory_order_acquire), 0x800895D0u);
            original = record != nullptr ? record->entry : nullptr;
        }
        if (original != nullptr) {
            original(ctx);
            return;
        }
    }
    const std::uint32_t list = ctx->gpr[3];
    if (list < 0x80000000u) {
        return;
    }
    constexpr std::uint32_t kAuxBytes = 96u * 4u;
    for (std::uint32_t i = 0; i < 3u; ++i) {
        const std::uint32_t buf = Memory::Read32(list + i * 4u);
        if (buf < 0x80000000u) {
            continue;
        }
        if (std::uint8_t* host = Memory::GetPointer(buf, kAuxBytes)) {
            std::memset(host, 0, kAuxBytes);
        } else {
            for (std::uint32_t b = 0; b < kAuxBytes; b += 4u) {
                Memory::Write32(buf + b, 0);
            }
        }
    }
}

// nw4r::snd::SoundPlayer::SetVolume: clamp to [0, 1] (unordered - NaN - gives 1) and store.
extern "C" void SoundPlayerSetVolume_800a35e0(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        return;
    }
    const float v = static_cast<float>(ctx->fpr[1].d);
    float clamped = 1.0f;
    if (v <= 1.0f) {
        clamped = v < 0.0f ? 0.0f : v;
    }
    std::uint32_t bits;
    std::memcpy(&bits, &clamped, 4);
    Memory::Write32(ctx->gpr[3] + kSoundPlayerVolumeOffset, bits);
}
