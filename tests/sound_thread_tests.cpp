// The sound thread (mkwv/runtime/guest_sync.hpp): with it on, the audio interrupt and a
// cooperative guest thread run on a second host thread, in parallel with the game thread.
// Stand-ins: the AI DMA callback (an "interrupt") bumps a guest counter inside the guest lock
// and posts a message; the sound thread (a fiber) receives it and bumps a second counter under
// a guest OSMutex. The game thread meanwhile does the same read-modify-writes inside
// OSDisableInterrupts and OSLockMutex. Every increment must survive, the fiber must run on the
// audio thread, and the game thread must never run it.
#include "mkwv/runtime/guest_sync.hpp"
#include "mkwv/runtime/hle_audio.hpp"
#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/runtime.hpp"
#include "memory.h"

#include <ucontext.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

using mkwv::runtime::CpuContext;
using mkwv::runtime::invoke_guest;

constexpr std::uint32_t kOSInitMessageQueue = 0x801A72FCu;
constexpr std::uint32_t kOSSendMessage = 0x801A735Cu;
constexpr std::uint32_t kOSReceiveMessage = 0x801A7424u;
constexpr std::uint32_t kOSCreateThread = 0x801A9E84u;
constexpr std::uint32_t kOSResumeThread = 0x801AA58Cu;
constexpr std::uint32_t kOSLockMutex = 0x801A7EE4u;
constexpr std::uint32_t kOSUnlockMutex = 0x801A7FC0u;
constexpr std::uint32_t kOSDisableInterrupts = 0x801A65ACu;
constexpr std::uint32_t kOSRestoreInterrupts = 0x801A65D4u;
constexpr std::uint32_t kAIInit = 0x801240B0u;
constexpr std::uint32_t kAIRegisterDMACallback = 0x80123F88u;
constexpr std::uint32_t kAIInitDMA = 0x80123FCCu;
constexpr std::uint32_t kAIStartDMA = 0x80124048u;
constexpr std::uint32_t kSoundThreadFunc = 0x800A49C0u;
constexpr std::uint32_t kCallback = 0x80001230u;

constexpr std::uint32_t kQueue = 0x80600000u;
constexpr std::uint32_t kQueueArray = 0x80600100u;
constexpr std::uint32_t kThread = 0x80601000u;
constexpr std::uint32_t kThreadStack = 0x80680000u;
constexpr std::uint32_t kMutex = 0x80603000u;
constexpr std::uint32_t kMutex2 = 0x80603100u;  // taken before kMutex by the fiber (X -> ST)
constexpr std::uint32_t kMutex2Counter = 0x80604008u;
constexpr std::uint32_t kIrqCounter = 0x80604000u;
constexpr std::uint32_t kMutexCounter = 0x80604004u;
constexpr std::uint32_t kMsgOut = 0x80604010u;
constexpr std::uint32_t kAiBuffer = 0x80610000u;
constexpr std::uint32_t kMainStack = 0x806F0000u;

std::atomic<int> g_callbacks{0};
std::atomic<int> g_fiber_rounds{0};
std::atomic<bool> g_fiber_on_audio{true};
std::atomic<bool> g_callback_on_audio{true};

bool call(CpuContext& cpu, std::uint32_t addr, std::initializer_list<std::uint32_t> args) {
    unsigned r = 3;
    for (std::uint32_t a : args) cpu.gpr[r++] = a;
    return invoke_guest(addr, &cpu);
}

void bump(std::uint32_t addr) {
    const std::uint32_t v = Memory::Read32(addr);
    // Widen the window a racing writer would need.
    for (volatile int i = 0; i < 50; ++i) {
    }
    Memory::Write32(addr, v + 1u);
}

// The AI DMA callback: interrupt context (the guest lock is held by the audio thread).
void ai_callback(CpuContext* ctx) {
    if (!mkwv::runtime::on_audio_thread()) g_callback_on_audio = false;
    ++g_callbacks;
    bump(kIrqCounter);
    call(*ctx, kOSSendMessage, {kQueue, 1, 0});
}

// The sound thread: one round per message.
void sound_thread(CpuContext* ctx) {
    CpuContext& cpu = *ctx;
    for (;;) {
        call(cpu, kOSReceiveMessage, {kQueue, kMsgOut, 1});
        if (!mkwv::runtime::on_audio_thread()) g_fiber_on_audio = false;
        call(cpu, kOSLockMutex, {kMutex2});
        bump(kMutex2Counter);
        call(cpu, kOSLockMutex, {kMutex});
        bump(kMutexCounter);
        call(cpu, kOSUnlockMutex, {kMutex});
        call(cpu, kOSUnlockMutex, {kMutex2});
        ++g_fiber_rounds;
    }
}

// ucontext fibers, used only on the thread that calls init_fibers.
ucontext_t g_sched_uc;
ucontext_t* g_current = &g_sched_uc;
struct HostFiber {
    ucontext_t uc;
    void (*entry)(void*);
    void* arg;
};
void trampoline(int lo, int hi) {
    const std::uintptr_t p =
        (static_cast<std::uintptr_t>(static_cast<std::uint32_t>(hi)) << 32) | static_cast<std::uint32_t>(lo);
    HostFiber* f = reinterpret_cast<HostFiber*>(p);
    f->entry(f->arg);
}
void* fiber_create(std::size_t bytes, void (*entry)(void*), void* arg) {
    HostFiber* f = new HostFiber{};
    f->entry = entry;
    f->arg = arg;
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = std::malloc(bytes);
    f->uc.uc_stack.ss_size = bytes;
    f->uc.uc_link = nullptr;
    const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(f);
    makecontext(&f->uc, reinterpret_cast<void (*)()>(&trampoline), 2, static_cast<int>(p & 0xFFFFFFFFu),
                static_cast<int>(p >> 32));
    return f;
}
void fiber_switch(void* target) {
    ucontext_t* to = target == &g_sched_uc ? &g_sched_uc : &static_cast<HostFiber*>(target)->uc;
    ucontext_t* from = g_current;
    if (to == from) return;
    g_current = to;
    swapcontext(from, to);
    g_current = from;
}

std::mutex g_m;
std::condition_variable g_cv;
bool g_signalled = false;
void sig() {
    {
        std::lock_guard<std::mutex> l(g_m);
        g_signalled = true;
    }
    g_cv.notify_one();
}
void wait(unsigned us) {
    std::unique_lock<std::mutex> l(g_m);
    g_cv.wait_for(l, std::chrono::microseconds(us), [] { return g_signalled; });
    g_signalled = false;
}
void sleep_us(unsigned us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}
bool init_fibers() {
    mkwv::runtime::FiberOps ops{};
    ops.create = &fiber_create;
    ops.switch_to = &fiber_switch;
    ops.main = &g_sched_uc;
    mkwv::runtime::set_fiber_ops(ops);
    return true;
}
bool start_audio_thread() {
    std::thread([] { mkwv::runtime::audio_thread_main(); }).detach();
    return true;
}

int fail(const char* what) {
    std::fprintf(stderr, "sound_thread: FAIL %s (callbacks=%d rounds=%d irq=%u mutex=%u)\n", what, g_callbacks.load(),
                 g_fiber_rounds.load(), Memory::Read32(kIrqCounter), Memory::Read32(kMutexCounter));
    std::fflush(stderr);
    std::_Exit(1);
}

} // namespace

int main() {
    mkwv::runtime::Runtime runtime;
    if (!runtime.initialize("/tmp/mkwv-sound-thread-overlay")) {
        return fail("runtime");
    }
    mkwv_register_extra_native(kCallback, &ai_callback);
    mkwv_register_extra_native(kSoundThreadFunc, &sound_thread);
    // Fibers are on from boot (main.cpp does this on the game thread); the audio thread takes
    // over as their scheduler when it starts.
    init_fibers();
    mkwv::runtime::GuestThreadOps ops{};
    ops.signal = &sig;
    ops.wait = &wait;
    ops.sleep_us = &sleep_us;
    ops.init_fibers = &init_fibers;
    ops.start_audio_thread = &start_audio_thread;
    mkwv::runtime::set_guest_thread_ops(ops);
    mkwv::runtime::set_sound_thread_requested(true);

    CpuContext& cpu = runtime.cpu();
    cpu.gpr[1] = kMainStack;
    Memory::Write32(0x800000E4u, 0x80605000u);  // __OSCurrentThread: the game thread
    Memory::Write32(kIrqCounter, 0);
    Memory::Write32(kMutexCounter, 0);
    Memory::Write32(kMutex2Counter, 0);
    call(cpu, kOSInitMessageQueue, {kQueue, kQueueArray, 64});
    call(cpu, kAIInit, {0});
    call(cpu, kAIRegisterDMACallback, {kCallback});
    call(cpu, kAIInitDMA, {kAiBuffer, 96u * 4u});
    call(cpu, kAIStartDMA, {});  // starts the sound thread
    if (!mkwv::runtime::sound_thread_running() || !mkwv::runtime::guest_threads_active()) {
        return fail("sound thread did not start");
    }
    // nw4r creates its thread after the DMA starts.
    call(cpu, kOSCreateThread, {kThread, kSoundThreadFunc, 0, kThreadStack, 0x4000, 4, 1});
    call(cpu, kOSResumeThread, {kThread});

    unsigned main_irq = 0, main_mutex = 0, main_nested = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        call(cpu, kOSDisableInterrupts, {});
        const std::uint32_t level = cpu.gpr[3];
        bump(kIrqCounter);
        call(cpu, kOSRestoreInterrupts, {level});
        ++main_irq;
        // Same order as the fiber (kMutex2, then kMutex) every other round.
        const bool nested = (main_mutex & 1u) != 0;
        if (nested) {
            call(cpu, kOSLockMutex, {kMutex2});
            bump(kMutex2Counter);
            ++main_nested;
        }
        call(cpu, kOSLockMutex, {kMutex});
        bump(kMutexCounter);
        call(cpu, kOSUnlockMutex, {kMutex});
        if (nested) {
            call(cpu, kOSUnlockMutex, {kMutex2});
        }
        ++main_mutex;
    }
    // Let the last message be handled, then stop the interrupt and settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    call(cpu, kOSDisableInterrupts, {});  // the audio thread cannot run a callback now
    const int callbacks = g_callbacks.load();
    const int rounds = g_fiber_rounds.load();
    const std::uint32_t irq = Memory::Read32(kIrqCounter);
    const std::uint32_t mtx = Memory::Read32(kMutexCounter);
    std::printf("sound_thread: callbacks=%d fiber rounds=%d game irq=%u game mutex=%u -> irq=%u mutex=%u\n", callbacks,
                rounds, main_irq, main_mutex, irq, mtx);
    if (callbacks < 20) return fail("the audio thread must deliver AI blocks on its own");
    if (rounds < 10) return fail("the sound thread must run on the audio thread");
    if (!g_fiber_on_audio || !g_callback_on_audio) return fail("guest threads must stay on the audio thread");
    if (irq != main_irq + static_cast<std::uint32_t>(callbacks)) return fail("interrupt-disabled sections raced");
    if (mtx != main_mutex + static_cast<std::uint32_t>(rounds)) return fail("OSMutex sections raced");
    if (Memory::Read32(kMutex2Counter) != main_nested + static_cast<std::uint32_t>(rounds)) {
        return fail("nested OSMutex sections raced");
    }
    std::printf("sound_thread: ok\n");
    std::fflush(stdout);
    std::_Exit(0);
}
