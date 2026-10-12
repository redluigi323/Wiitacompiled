#include "mkwv/platform/sound_thread.hpp"

#include "mkwv/gx/perf.hpp"
#include "mkwv/runtime/guest_sync.hpp"
#include "mkwv/runtime/hle_audio.hpp"
#include "mkwv/runtime/host_context.hpp"

#include "memory.h"

#include <psp2/kernel/threadmgr.h>

#include <cstdint>
#include <cstdio>

namespace mkwv::platform {
namespace {

SceUID g_wake = -1;
SceUID g_sound_tid = -1;
int g_base_priority = 0;
int g_raises = 0;
// Above the vertex worker (0x10000100), below the audio output thread (-20).
constexpr int kRaisedPriority = 0x10000100 - 15;

void raise_sound() {
    if (g_sound_tid < 0 || g_base_priority <= kRaisedPriority) {
        return;
    }
    if (g_raises++ == 0) {
        sceKernelChangeThreadPriority(g_sound_tid, kRaisedPriority);
    }
}

void restore_sound() {
    if (g_sound_tid < 0 || g_base_priority <= kRaisedPriority) {
        return;
    }
    if (g_raises > 0 && --g_raises == 0) {
        sceKernelChangeThreadPriority(g_sound_tid, g_base_priority);
    }
}

void signal() {
    if (g_wake >= 0) {
        sceKernelSignalSema(g_wake, 1);
    }
}

void wait(unsigned timeout_us) {
    if (g_wake < 0) {
        sceKernelDelayThread(timeout_us);
        return;
    }
    SceUInt timeout = timeout_us;
    sceKernelWaitSema(g_wake, 1, &timeout);
}

void sleep_us(unsigned us) {
    sceKernelDelayThread(us);
}

bool init_fibers() {
    static void* s_scheduler = nullptr;
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 || info.stack == nullptr ||
        static_cast<std::size_t>(info.stackSize) <= runtime::kFiberThreadReserve) {
        std::printf("mkwv: sound thread has no stack for fibers\n");
        return false;
    }
    // stack is the low address. This thread runs at the top; fibers take the low end, still
    // inside the region the kernel checks.
    const std::size_t fiber_bytes = static_cast<std::size_t>(info.stackSize) - runtime::kFiberThreadReserve;
    auto* fiber_top = static_cast<std::uint8_t*>(info.stack) + fiber_bytes;
    void* sp = nullptr;
    asm volatile("mov %0, sp" : "=r"(sp));
    if (sp <= fiber_top) {
        std::printf("mkwv: sound thread SP %p is inside the fiber region\n", sp);
        return false;
    }
    if (!runtime::HostContext::initialize_scheduler(&s_scheduler)) {
        return false;
    }
    runtime::HostContext::adopt_thread_stack(info.stack, fiber_bytes);
    std::printf("mkwv: sound fibers %p..%p inside thread stack %p+%#x\n", info.stack,
                static_cast<void*>(fiber_top), info.stack, info.stackSize);
    runtime::FiberOps ops{};
    ops.create = [](std::size_t bytes, void (*entry)(void*), void* arg) -> void* {
        return runtime::HostContext::create(bytes, entry, arg);
    };
    ops.switch_to = [](void* fiber) { runtime::HostContext::switch_to(fiber); };
    ops.main = s_scheduler;
    runtime::set_fiber_ops(ops);
    return true;
}

int sound_main(SceSize, void*) {
    runtime::audio_thread_main();
    return 0;
}

bool g_share = false;

bool start_audio_thread() {
    // Userland has three cores. The vertex worker on core 2 is the race's limiter, so the mix
    // stays on that core but below the worker and runs in its gaps (the output thread, which
    // blocks in the driver, stays above it). share puts the mix back above the renderer on
    // cores 1-2. Raising this thread while the game waits made it drain a late audio queue
    // with the sound mutex held, and the menu frame got slower.
    // static62: above the vertex worker (0x10000100 + 16), which may now use core 2's idle time,
    // below the game and render threads (0x10000100).
    const int priority = g_share ? 0x10000100 - 30 : 0x10000100 + 8;
    const int affinity = g_share ? (SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2)
                                 : SCE_KERNEL_CPU_MASK_USER_2;
    // The reserve stays under this thread. The rest is sliced up for the guest fibers; they
    // cannot have stacks of their own or the kernel kills the thread (0x10006).
    const SceSize stack_bytes = static_cast<SceSize>(
        runtime::kFiberThreadReserve + runtime::kMaxFibers * runtime::kFiberStackBytes);
    const SceUID tid = sceKernelCreateThread("mkwv_sound", sound_main, priority, stack_bytes, 0, affinity, nullptr);
    if (tid < 0) {
        std::printf("mkwv: sound thread create failed 0x%08X\n", static_cast<unsigned>(tid));
        return false;
    }
    g_sound_tid = tid;
    g_base_priority = priority;
    const bool started = sceKernelStartThread(tid, 0, nullptr) >= 0;
    if (started) {
        perf::register_thread("snd", tid);
    }
    return started;
}

} // namespace

void install_sound_thread(bool share_with_renderer) {
    g_share = share_with_renderer;
    g_wake = sceKernelCreateSema("mkwv_snd_wake", 0, 0, 1, nullptr);
    runtime::GuestThreadOps ops{};
    ops.signal = &signal;
    ops.wait = &wait;
    ops.sleep_us = &sleep_us;
    ops.init_fibers = &init_fibers;
    ops.start_audio_thread = &start_audio_thread;
    ops.raise_sound = &raise_sound;
    ops.restore_sound = &restore_sound;
    runtime::set_guest_thread_ops(ops);
    runtime::set_sound_thread_requested(true);
}

} // namespace mkwv::platform
