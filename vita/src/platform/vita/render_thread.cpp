#include "mkwv/platform/render_thread.hpp"

#include "mkwv/gx/backend.hpp"
#include "mkwv/gx/fifo.hpp"
#include "mkwv/gx/perf.hpp"
#include "mkwv/gx/submit.hpp"
#include "mkwv/gx/vertex_worker.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/hle_audio.hpp"
#include "mkwv/runtime/guest_prof.hpp"

#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mkwv::platform {
namespace {

// Single producer (guest thread), single consumer (render thread). Each slot holds one FIFO
// buffer's worth of command bytes. Eight slots is two to four race frames of headroom.
constexpr unsigned kSlots = 8;

enum SlotKind : std::uint8_t { kChunk = 0, kPresent = 1 };

struct Slot {
    std::uint32_t seq;
    std::uint32_t size;
    std::uint8_t kind;
    std::uint8_t data[gx::kFifoCapacity];
};

Slot g_slots[kSlots];
std::uint32_t g_write = 0;  // guest thread only
std::uint32_t g_read = 0;   // render thread only
std::uint32_t g_next_seq = 0;
std::atomic<std::uint32_t> g_done_seq{0};

SceUID g_work = -1;   // published slots
SceUID g_space = -1;  // free slots
SceUID g_done = -1;   // signalled after every processed slot
SceUID g_init = -1;   // render thread -> main: backend initialised

gx::Backend* g_backend = nullptr;
// How far the guest may run ahead of the renderer (RenderSync): kSyncFrame waits at every
// present until the renderer has caught up (GXDrawDone); kSyncAheadOne only at each retrace,
// for everything presented before the previous retrace - so the renderer draws frame N while
// the game computes frame N+1, and never more than one frame behind; kSyncNone never waits.
int g_sync = kSyncAheadOne;
std::uint32_t g_last_present_seq = 0;  // guest thread
std::uint32_t g_boundary_seq = 0;      // guest thread: last present of the previous frame
std::atomic<int> g_init_ok{0};

// The guest thread waits for the render thread a lot (a full queue, or everything handed
// over at a frame end). Meanwhile its core runs vertex jobs, checking the semaphore between
// them and at least every 200 us.
void wait_helping_loop(SceUID sema, bool guest_thread);

void wait_helping(SceUID sema, bool guest_thread) {
    if (guest_thread) {
        // Only the guest thread tags its activity (the profiler samples the guest).
        runtime::prof::ActivityScope activity(runtime::prof::kRenderWait);
        wait_helping_loop(sema, true);
    } else {
        wait_helping_loop(sema, false);
    }
}

void wait_helping_loop(SceUID sema, bool guest_thread) {
    for (;;) {
        if (sceKernelPollSema(sema, 1) == 0) {
            return;
        }
        if (gx::vertex_worker_help()) {
            continue;
        }
        // The guest waiting for the render thread at a present is where audio blocks fall due
        // (guest code: the guest thread only).
        if (guest_thread) {
            runtime::audio_poll_waiting();
        }
        SceUInt timeout = 200;
        if (sceKernelWaitSema(sema, 1, &timeout) == 0) {
            return;
        }
    }
}

std::uint32_t push(SlotKind kind, const std::uint8_t* data, std::uint32_t size) {
    if (size > gx::kFifoCapacity) {
        size = gx::kFifoCapacity;  // cannot happen: the guest FIFO is this size
    }
    {
        perf::Scope waiting(perf::kRenderWait);
        wait_helping(g_space, true);
    }
    Slot& slot = g_slots[g_write % kSlots];
    slot.kind = kind;
    slot.size = size;
    slot.seq = ++g_next_seq;
    if (size != 0 && data != nullptr) {
        runtime::prof::ActivityScope activity(runtime::prof::kHandoff);
        std::memcpy(slot.data, data, size);
    }
    ++g_write;
    sceKernelSignalSema(g_work, 1);
    return slot.seq;
}

void offload_chunk(const std::uint8_t* data, std::uint32_t size) {
    push(kChunk, data, size);
}

void wait_done(std::uint32_t seq) {
    if (static_cast<std::int32_t>(g_done_seq.load(std::memory_order_acquire) - seq) >= 0) {
        return;
    }
    perf::Scope waiting(perf::kRenderWait);
    while (static_cast<std::int32_t>(g_done_seq.load(std::memory_order_acquire) - seq) < 0) {
        wait_helping(g_done, true);
    }
}

void offload_present(const std::uint8_t* data, std::uint32_t size) {
    const std::uint32_t seq = push(kPresent, data, size);
    g_last_present_seq = seq;
    if (g_sync == kSyncFrame) {
        // GXDrawDone semantics: everything handed over so far has been decoded and submitted.
        wait_done(seq);
    }
}

void offload_frame_boundary() {
    if (g_sync != kSyncAheadOne) {
        return;
    }
    if (g_boundary_seq != 0) {
        wait_done(g_boundary_seq);
    }
    g_boundary_seq = g_last_present_seq;
}

const runtime::RenderOffload g_hooks{&offload_chunk, &offload_present, &offload_frame_boundary};

// Vertex transform used to be pinned to core 2, which is already full. The worker now
// floats on cores 0 and 1, below the game and render threads, and blocks on g_wsem when
// it runs out of work. While those two are busy the render thread runs jobs itself.
SceUID g_wsem = -1;
bool g_single_worker = false;
bool g_workers_low = false;

void worker_wait() {
    sceKernelWaitSema(g_wsem, 1, nullptr);
}

void worker_wake() {
    sceKernelSignalSema(g_wsem, 1);
}

void worker_relax() {
    // No unclaimed work: remaining jobs already belong to workers. Yield this high-priority
    // renderer's core instead of burning it until those lower-priority workers finish.
    sceKernelDelayThread(50);
}

int worker_main(SceSize, void*) {
    gx::vertex_worker_main();
    return 0;
}

bool start_vertex_worker() {
    g_wsem = sceKernelCreateSema("mkwv_vw_wake", 0, 0, 0x7FFFFFFF, nullptr);
    if (g_wsem < 0) {
        return false;
    }
    // Below the game thread (core 0), the render thread (core 1) and the sound thread (core 2),
    // so this only takes their idle slices. static62: core 2 again - on a real Vita the sound
    // thread leaves about a third of it idle, and it now sits above this worker there.
    const int affinity = SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2;
    // static63: two workers. One thread can only ever use one core; the game thread leaves
    // core 0 idle while it waits for the renderer and the sound thread leaves half of core 2.
    const int workers = g_single_worker ? 1 : 2;
    SceUID tids[2] = {-1, -1};
    for (int k = 0; k < workers; ++k) {
        // static65: above the sound thread (+8), still below the game and render threads. static64's
        // log had the render thread waiting ~10-16 ms a frame for jobs the starved workers had not
        // run (w0/w1 got ~10 ms of CPU each); audio has ~270 ms queued and tolerates the delay.
        // workers_low puts them back below the sound thread.
        tids[k] = sceKernelCreateThread(k == 0 ? "mkwv_vertex" : "mkwv_vertex2", worker_main,
                                        g_workers_low ? 0x10000100 + 16 : 0x10000100 + 6,
                                        256 * 1024, 0, affinity, nullptr);
        if (tids[k] < 0) {
            std::printf("mkwv: vertex worker %d create failed 0x%08X\n", k, static_cast<unsigned>(tids[k]));
            if (k == 0) {
                return false;
            }
            break;
        }
        sceKernelChangeThreadCpuAffinityMask(tids[k], affinity);
    }
    // Drain waits help with unclaimed jobs, then yield while other consumers finish.
    gx::vertex_worker_install(gx::VertexWorkerHooks{&worker_wait, &worker_wake, &worker_relax});
    for (int k = 0; k < workers; ++k) {
        if (tids[k] >= 0) {
            sceKernelStartThread(tids[k], 0, nullptr);
            perf::register_thread(k == 0 ? "w0" : "w1", tids[k]);
        }
    }
    // Only captures (and the host tests) read Processor::verts; the jobs skip writing it.
    gx::vertex_worker_set_mirror(false);
    return true;
}

int render_main(SceSize, void*) {
    const bool ok = g_backend != nullptr && g_backend->initialize();
    g_init_ok.store(ok ? 1 : 0, std::memory_order_release);
    gx::set_deferred_swap(ok);
    sceKernelSignalSema(g_init, 1);
    if (!ok) {
        return 0;
    }
    for (;;) {
        wait_helping(g_work, false);
        Slot& slot = g_slots[g_read % kSlots];
        const bool present = slot.kind == kPresent;
        {
            perf::Scope busy(perf::kRender);
            if (present) {
                runtime::render_consume_present(slot.data, slot.size);
            } else {
                runtime::render_consume_chunk(slot.data, slot.size);
            }
        }
        g_done_seq.store(slot.seq, std::memory_order_release);
        ++g_read;
        sceKernelSignalSema(g_space, 1);
        sceKernelSignalSema(g_done, 1);
        if (present) {
            // The guest is already running again; the vsync wait overlaps it.
            perf::Scope busy(perf::kRender);
            gx::complete_deferred_swap();
        }
    }
    return 0;
}

} // namespace

void set_vertex_workers_low(bool low) {
    g_workers_low = low;
}

void set_single_vertex_worker(bool single) {
    g_single_worker = single;
}

int start_render_thread(gx::Backend* backend, int sync_mode, bool vertex_worker) {
    g_backend = backend;
    g_sync = sync_mode;
    g_work = sceKernelCreateSema("mkwv_rt_work", 0, 0, static_cast<int>(kSlots), nullptr);
    g_space = sceKernelCreateSema("mkwv_rt_space", 0, static_cast<int>(kSlots), static_cast<int>(kSlots), nullptr);
    g_done = sceKernelCreateSema("mkwv_rt_done", 0, 0, 0x7FFFFFFF, nullptr);
    g_init = sceKernelCreateSema("mkwv_rt_init", 0, 0, 1, nullptr);
    if (g_work < 0 || g_space < 0 || g_done < 0 || g_init < 0) {
        std::printf("mkwv: render thread semaphores failed\n");
        return -1;
    }
    // Guest on core 0, renderer on core 1. The stack is generous: the decoder and submitter
    // keep their big buffers static, but vitaGL and the shader compiler recurse.
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    const SceUID tid = sceKernelCreateThread("mkwv_render", render_main, 0x10000100, 1024 * 1024, 0,
                                             SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (tid < 0) {
        std::printf("mkwv: render thread create failed 0x%08X\n", static_cast<unsigned>(tid));
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0);
        return -1;
    }
    sceKernelStartThread(tid, 0, nullptr);
    perf::register_thread("game", sceKernelGetThreadId());
    perf::register_thread("rend", tid);
    sceKernelWaitSema(g_init, 1, nullptr);
    if (g_init_ok.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    runtime::set_render_offload(&g_hooks);
    if (vertex_worker) {
        std::printf("mkwv: vertex worker %s\n", start_vertex_worker() ? (g_single_worker ? "on (1 thread, cores 0-2)" : "on (2 threads, cores 0-2)") : "unavailable");
    }
    return 1;
}

} // namespace mkwv::platform
