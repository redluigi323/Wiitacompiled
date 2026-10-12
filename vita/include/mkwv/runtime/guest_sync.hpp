#pragma once

// Guest code on more than one host thread (not shard-visible).
//
// The game thread runs on core 0. With the sound thread enabled (hle_audio.hpp), the audio
// interrupt (AI DMA callback, AX, the DSP HLE mix) and nw4r's sound/task threads run on a
// second host thread at the same time. On the Wii they share one core: "interrupts disabled"
// means nothing else runs, and that is what every guest critical section relies on
// (OSDisableInterrupts around AX parameter blocks, message queues, thread queues...). Here it
// becomes one global recursive lock:
//   - taken when a thread's interrupt state goes enabled -> disabled, released on the way back;
//   - held by the audio thread for the whole audio interrupt (callback + mix);
//   - taken by the HLE natives that touch shared OS state (message queues, mutexes, threads,
//     the fiber scheduler, ExpHeap);
//   - fully released while a thread blocks (a fiber waiting for a message or mutex, the game
//     thread sleeping or waiting for a mutex a fiber holds), as a context switch would.
// Guest code outside critical sections runs in parallel. Nothing changes until
// set_guest_threads_active(true); before that (and in the host tests) every call is a no-op.

#include "mkwv/runtime/cpu_context.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mkwv::runtime {

// Which guest thread the caller is: 0 = the game thread (and any other host thread), 1 = the
// audio thread. No thread_local: on the Vita this toolchain's TLS support is unverified (the
// runtime headers avoid it - MKW_TLS), and these are read on every guest call. The user-mode
// thread pointer (TPIDRURO, per thread on the hardware and in Vita3K) identifies the thread in
// one instruction.
inline std::uintptr_t host_thread_pointer() {
#if defined(__vita__)
    std::uintptr_t tp;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
    return tp;
#else
    static thread_local char marker;
    return reinterpret_cast<std::uintptr_t>(&marker);
#endif
}
extern std::uintptr_t g_audio_thread_tp;
inline unsigned guest_thread_slot() {
    return host_thread_pointer() == g_audio_thread_tp ? 1u : 0u;
}

bool guest_threads_active();
// Called on the game thread. `game_irqs_disabled`: the game thread is inside a critical
// section right now (it then owns the lock from the start).
void set_guest_threads_active(bool active, bool game_irqs_disabled);

void irq_lock();
void irq_unlock();
// Releases every level this thread holds (returns the count) / takes them back.
unsigned irq_release_all();
void irq_reacquire(unsigned depth);
bool irq_held_by_me();

class IrqLockScope {
public:
    IrqLockScope() { irq_lock(); }
    ~IrqLockScope() { irq_unlock(); }
    IrqLockScope(const IrqLockScope&) = delete;
    IrqLockScope& operator=(const IrqLockScope&) = delete;
};

// A small spin lock for host-side state shared by both guest threads (the DVD reader).
class HostSpinLock {
public:
    void lock();
    void unlock() { flag_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> flag_{false};
};

class HostSpinGuard {
public:
    explicit HostSpinGuard(HostSpinLock& l) : l_(l) { l_.lock(); }
    ~HostSpinGuard() { l_.unlock(); }
    HostSpinGuard(const HostSpinGuard&) = delete;
    HostSpinGuard& operator=(const HostSpinGuard&) = delete;

private:
    HostSpinLock& l_;
};

// The audio (sound) host thread (mark_audio_thread: called first thing on it).
inline bool on_audio_thread() {
    return guest_thread_slot() == 1u;
}
void mark_audio_thread();
void set_audio_thread_pointer(std::uintptr_t tp);
// invoke_guest's resolved-target cache: the calling thread gets its own (dispatch.cpp).
void use_private_dispatch_cache();

// The guest context the calling host thread is running (set by invoke_guest). The runtime's
// SPR/carry helpers use it: g_currentCpuContext is a plain global on the Vita.
CpuContext* current_guest_ctx();
void set_current_guest_ctx(CpuContext* ctx);
CpuContext*& guest_ctx_slot(unsigned slot);

// Platform services for the audio thread (Vita: kernel semaphore and delay).
struct GuestThreadOps {
    void (*signal)();                    // wake the audio thread
    void (*wait)(unsigned timeout_us);   // audio thread: sleep until signalled or timeout
    void (*sleep_us)(unsigned us);       // any thread
    bool (*init_fibers)();               // audio thread: become the fiber scheduler (FiberOps.main)
    bool (*start_audio_thread)();        // create the audio thread; it calls audio_thread_main()
    // The sound thread shares a core with the vertex worker and normally stays below it. While
    // the game thread is blocked on a guest mutex or the irq lock the sound thread holds, that
    // ordering is priority inversion: the worker runs, the owner cannot release, and the frame
    // waits. raise/restore nest. Null on the host.
    void (*raise_sound)();
    void (*restore_sound)();
};
void set_guest_thread_ops(const GuestThreadOps& ops);
const GuestThreadOps& guest_thread_ops();
void host_sleep_us(unsigned us);
void signal_audio_thread();
// See GuestThreadOps::raise_sound. No-ops when the platform did not install them, and when
// called on the audio thread itself.
void raise_sound_thread();
void restore_sound_thread();

} // namespace mkwv::runtime
