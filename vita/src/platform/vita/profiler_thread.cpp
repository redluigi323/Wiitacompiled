#include "mkwv/platform/profiler.hpp"

#include "mkwv/runtime/guest_prof.hpp"

#include <psp2/kernel/threadmgr.h>

#include <cstdio>

namespace mkwv::platform {
namespace {

int profiler_main(SceSize, void*) {
    for (;;) {
        // 500 us nominal; the kernel's timer granularity makes it somewhat longer, which only
        // changes the sample count, not the proportions.
        sceKernelDelayThread(500);
        runtime::prof::sample();
    }
    return 0;
}

} // namespace

bool start_guest_profiler() {
    // Core 1 (the render thread's). The sampler is a few hundred instructions every half
    // millisecond; core 2 is the vertex worker and is the frame's limiter.
    const SceUID tid = sceKernelCreateThread("mkwv_prof", profiler_main, 0x10000100 - 10, 16 * 1024, 0,
                                             SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (tid < 0) {
        std::printf("mkwv: profiler thread create failed 0x%08X\n", static_cast<unsigned>(tid));
        return false;
    }
    sceKernelStartThread(tid, 0, nullptr);
    return true;
}

} // namespace mkwv::platform
