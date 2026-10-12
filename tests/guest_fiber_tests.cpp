// Cooperative guest threads (hle_os.cpp, hle_audio.hpp): a whitelisted OSThread runs as a fiber
// when woken and until it blocks. The thread function here stands in for nw4r's sound thread
// (registered at its entry address): it receives messages in a loop, and between messages it
// sleeps on a thread queue and takes a mutex the main thread may hold. Host fibers use ucontext.
#include "mkwv/runtime/hle_audio.hpp"
#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/game_options.hpp"
#include "mkwv/runtime/guest_prof.hpp"
#include "mkwv/runtime/runtime.hpp"
#include "memory.h"

#include <ucontext.h>

#include <cstdio>
#include <cstdlib>

namespace {

using mkwv::runtime::CpuContext;
using mkwv::runtime::invoke_guest;

constexpr std::uint32_t kOSInitMessageQueue = 0x801A72FCu;
constexpr std::uint32_t kOSSendMessage = 0x801A735Cu;
constexpr std::uint32_t kOSReceiveMessage = 0x801A7424u;
constexpr std::uint32_t kOSCreateThread = 0x801A9E84u;
constexpr std::uint32_t kOSResumeThread = 0x801AA58Cu;
constexpr std::uint32_t kOSSleepThread = 0x801AA9B8u;
constexpr std::uint32_t kOSWakeupThread = 0x801AAAA4u;
constexpr std::uint32_t kOSLockMutex = 0x801A7EE4u;
constexpr std::uint32_t kOSUnlockMutex = 0x801A7FC0u;
constexpr std::uint32_t kSoundThreadFunc = 0x800A49C0u;

constexpr std::uint32_t kQueue = 0x80600000u;
constexpr std::uint32_t kQueueArray = 0x80600100u;
constexpr std::uint32_t kThread = 0x80601000u;
constexpr std::uint32_t kThreadStack = 0x80680000u;
constexpr std::uint32_t kSleepQueue = 0x80602000u;
constexpr std::uint32_t kMutex = 0x80603000u;
constexpr std::uint32_t kMsgOut = 0x80604000u;
constexpr std::uint32_t kMainStack = 0x806F0000u;

int g_received = 0;
int g_after_sleep = 0;
int g_locked = 0;
bool g_exit = false;

bool call(CpuContext& cpu, std::uint32_t addr, std::initializer_list<std::uint32_t> args) {
    unsigned r = 3;
    for (std::uint32_t a : args) {
        cpu.gpr[r++] = a;
    }
    return invoke_guest(addr, &cpu);
}

mkwv::runtime::prof::Stack* g_main_prof_stack = nullptr;
bool g_fiber_prof_ok = true;

// The stand-in thread function (runs on the fiber, with the fiber's CpuContext).
void sound_thread(CpuContext* ctx) {
    CpuContext& cpu = *ctx;
    // The fiber has its own shadow stack, holding just this entry, and its activity tag.
    namespace prof = mkwv::runtime::prof;
    g_fiber_prof_ok = prof::g_stack_slot[0] != g_main_prof_stack && prof::g_stack_slot[0]->depth == 1 &&
                      prof::g_stack_slot[0]->addr[0] == kSoundThreadFunc && prof::g_activity_slot[0] == prof::kSoundThread;
    while (!g_exit) {
        call(cpu, kOSReceiveMessage, {kQueue, kMsgOut, 1});
        const std::uint32_t msg = Memory::Read32(kMsgOut);
        ++g_received;
        if (msg == 2) {
            call(cpu, kOSSleepThread, {kSleepQueue});
            ++g_after_sleep;
        } else if (msg == 3) {
            call(cpu, kOSLockMutex, {kMutex});
            ++g_locked;
            call(cpu, kOSUnlockMutex, {kMutex});
        } else if (msg == 9) {
            g_exit = true;
        }
    }
}

// ucontext fibers for FiberOps.
ucontext_t g_main_uc;
struct HostFiber {
    ucontext_t uc;
    void (*entry)(void*);
    void* arg;
};
ucontext_t* g_current = &g_main_uc;

void trampoline(int lo, int hi) {
    const std::uintptr_t p = (static_cast<std::uintptr_t>(static_cast<std::uint32_t>(hi)) << 32) |
                             static_cast<std::uint32_t>(lo);
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
    ucontext_t* to = target == &g_main_uc ? &g_main_uc : &static_cast<HostFiber*>(target)->uc;
    ucontext_t* from = g_current;
    if (to == from) return;
    g_current = to;
    swapcontext(from, to);
    g_current = from;
}

int fail(const char* what) {
    std::fprintf(stderr, "guest_fiber: %s (received=%d after_sleep=%d locked=%d)\n", what, g_received,
                 g_after_sleep, g_locked);
    return 1;
}

} // namespace

int main() {
    mkwv::runtime::Runtime runtime;
    if (!runtime.initialize("/tmp/mkwv-guest-fiber-overlay")) {
        return fail("runtime");
    }
    mkwv_register_extra_native(kSoundThreadFunc, &sound_thread);
    g_main_prof_stack = mkwv::runtime::prof::g_stack_slot[0];
    mkwv::runtime::FiberOps ops{};
    ops.create = &fiber_create;
    ops.switch_to = &fiber_switch;
    ops.main = &g_main_uc;
    mkwv::runtime::set_fiber_ops(ops);

    CpuContext& cpu = runtime.cpu();
    cpu.gpr[1] = kMainStack;
    Memory::Write32(0x800000E4u, 0x80605000u);  // __OSCurrentThread: the main thread (owns mutexes)
    call(cpu, kOSInitMessageQueue, {kQueue, kQueueArray, 8});
    // OSCreateThread(thread, func, arg, stack top, stack size, prio, attr)
    call(cpu, kOSCreateThread, {kThread, kSoundThreadFunc, 0, kThreadStack, 0x4000, 4, 1});
    call(cpu, kOSResumeThread, {kThread});
    if (g_received != 0) return fail("thread ran before any message");

    call(cpu, kOSSendMessage, {kQueue, 1, 0});
    if (g_received != 1) return fail("a message must run the thread at once");
    call(cpu, kOSSendMessage, {kQueue, 1, 0});
    call(cpu, kOSSendMessage, {kQueue, 1, 0});
    if (g_received != 3) return fail("each message runs the thread");

    // OSSleepThread on a thread queue until OSWakeupThread.
    call(cpu, kOSSendMessage, {kQueue, 2, 0});
    if (g_received != 4 || g_after_sleep != 0) return fail("the thread must sleep");
    call(cpu, kOSWakeupThread, {kSleepQueue});
    if (g_after_sleep != 1) return fail("OSWakeupThread must resume the sleeper");

    // A mutex the main thread holds: the thread waits for the unlock.
    call(cpu, kOSLockMutex, {kMutex});
    call(cpu, kOSSendMessage, {kQueue, 3, 0});
    if (g_received != 5 || g_locked != 0) return fail("the thread must wait for the main thread's mutex");
    call(cpu, kOSUnlockMutex, {kMutex});
    if (g_locked != 1) return fail("unlocking must let the waiter in");

    // Plain (non-EGG) messages must not be run as EGG jobs (that wrote zeros to low memory).
    const std::uint32_t low = Memory::Read32(0x80000000u);
    call(cpu, kOSSendMessage, {kQueue, 9, 0});
    if (Memory::Read32(0x80000000u) != low) return fail("a plain message clobbered low memory");
    if (!g_exit) return fail("exit message");
    // EGG::DrawPathDOF::DrawImpl is shadowed by a native that draws nothing (DOF off, as
    // upstream): a virtual call to it must dispatch without a miss.
    if (mkwv::runtime::race_dof_enabled()) return fail("race DOF must default to off");
    if (!call(cpu, 0x8021EEC0u, {0x80607000u, 0})) return fail("DrawPathDOF::DrawImpl must resolve to the native");
    if (!g_fiber_prof_ok) return fail("profiler: fiber shadow stack/activity");
    if (mkwv::runtime::prof::g_stack_slot[0] != g_main_prof_stack || g_main_prof_stack->depth != 0 ||
        mkwv::runtime::prof::g_activity_slot[0] != mkwv::runtime::prof::kGame) {
        return fail("profiler: shadow stack unbalanced after guest calls");
    }
    for (int i = 0; i < 50; ++i) mkwv::runtime::prof::sample();  // no stack: counted, nothing attributed
    std::printf("guest_fiber: ok (received=%d)\n", g_received);
    return 0;
}
