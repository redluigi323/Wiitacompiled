#include "mkwv/runtime/guest_sync.hpp"
#include "mkwv/runtime/guest_prof.hpp"
#include "mkwv/gx/perf.hpp"
#include "mkwv/runtime/diagnostics.hpp"

#include <cstdio>

#if !defined(__vita__)
#include <thread>
#include <chrono>
#endif

namespace mkwv::runtime {
namespace {

std::atomic<bool> g_active{false};
std::atomic<int> g_owner{0};  // 0 = free, else guest thread slot + 1
unsigned g_depth[2] = {0, 0};  // per guest thread slot (only that thread touches its entry)
CpuContext* g_ctx[2] = {nullptr, nullptr};
GuestThreadOps g_ops{};

int my_token() {
    return static_cast<int>(guest_thread_slot()) + 1;
}

// Test-and-sleep, as through static59. static60 added a hand-over to a waiting thread; it is
// gone again while the static60 title-screen hang is open. The wait is tagged for the profiler
// (activity `lock`) and a wait of two seconds is logged once with the lock's state.
void report_stuck(unsigned slot) {
    static std::atomic<int> reported{0};
    if (reported.exchange(1) != 0) {
        return;
    }
    char line[160];
    std::snprintf(line, sizeof(line), "mkwv: STUCK guest lock: slot %u waited 2s, owner=%d depth0=%u depth1=%u\n", slot,
                  g_owner.load(std::memory_order_relaxed), g_depth[0], g_depth[1]);
    std::printf("%s", line);
    watchdog_log_line(line);
}

void acquire() {
    const int me = my_token();
    const unsigned slot = static_cast<unsigned>(me - 1);
    unsigned spins = 0;
    bool waited = false;
    std::uint32_t saved_activity = 0;
    std::uint64_t t0 = 0;
    for (;;) {
        int expected = 0;
        if (g_owner.load(std::memory_order_relaxed) == 0 &&
            g_owner.compare_exchange_weak(expected, me, std::memory_order_acquire, std::memory_order_relaxed)) {
            if (waited) {
                prof::g_activity_slot[slot] = saved_activity;
            }
            return;
        }
        if (!waited) {
            waited = true;
            saved_activity = prof::g_activity_slot[slot];
            prof::g_activity_slot[slot] = prof::kLockWait;
            t0 = mkwv::perf::now_us();
        }
        // The holder is on another core and most sections are short: spin a while before
        // paying for a sleep.
        if (++spins > 512u) {
            host_sleep_us(20);
            spins = 0;
            if (mkwv::perf::now_us() - t0 > 2000000u) {
                report_stuck(slot);
            }
        }
    }
}

void release_owner() {
    g_owner.store(0, std::memory_order_release);
}

} // namespace

std::uintptr_t g_audio_thread_tp = ~static_cast<std::uintptr_t>(0);

bool guest_threads_active() {
    return g_active.load(std::memory_order_relaxed);
}

void set_guest_threads_active(bool active, bool game_irqs_disabled) {
    unsigned& depth = g_depth[guest_thread_slot()];
    if (active) {
        depth = 0;
        if (game_irqs_disabled) {
            acquire();
            depth = 1;
        }
        g_active.store(true, std::memory_order_release);
    } else {
        g_active.store(false, std::memory_order_release);
        if (g_owner.load() == my_token()) {
            g_owner.store(0, std::memory_order_release);
        }
        depth = 0;
    }
}

void irq_lock() {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    const unsigned slot = guest_thread_slot();
    unsigned& depth = g_depth[slot];
    if (depth != 0 && g_owner.load(std::memory_order_relaxed) == static_cast<int>(slot) + 1) {
        ++depth;
        return;
    }
    acquire();
    depth = 1;
}

void irq_unlock() {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    unsigned& depth = g_depth[guest_thread_slot()];
    if (depth == 0) {
        return;
    }
    if (--depth == 0) {
        release_owner();
    }
}

unsigned irq_release_all() {
    unsigned& depth = g_depth[guest_thread_slot()];
    const unsigned d = depth;
    if (d != 0) {
        depth = 0;
        release_owner();
    }
    return d;
}

void irq_reacquire(unsigned depth) {
    if (depth == 0 || !g_active.load(std::memory_order_relaxed)) {
        return;
    }
    acquire();
    g_depth[guest_thread_slot()] = depth;
}

bool irq_held_by_me() {
    const unsigned slot = guest_thread_slot();
    return g_depth[slot] != 0 && g_owner.load(std::memory_order_relaxed) == static_cast<int>(slot) + 1;
}

void HostSpinLock::lock() {
    unsigned spins = 0;
    while (flag_.exchange(true, std::memory_order_acquire)) {
        if (++spins > 64u) {
            host_sleep_us(20);
            spins = 0;
        }
    }
}

void mark_audio_thread() {
    g_audio_thread_tp = host_thread_pointer();
}

void set_audio_thread_pointer(std::uintptr_t tp) {
    g_audio_thread_tp = tp;
}

CpuContext* current_guest_ctx() {
    return g_ctx[guest_thread_slot()];
}

void set_current_guest_ctx(CpuContext* ctx) {
    g_ctx[guest_thread_slot()] = ctx;
}

CpuContext*& guest_ctx_slot(unsigned slot) {
    return g_ctx[slot & 1u];
}

void set_guest_thread_ops(const GuestThreadOps& ops) {
    g_ops = ops;
}

const GuestThreadOps& guest_thread_ops() {
    return g_ops;
}

void host_sleep_us(unsigned us) {
    if (g_ops.sleep_us != nullptr) {
        g_ops.sleep_us(us);
    } else {
#if !defined(__vita__)
        std::this_thread::sleep_for(std::chrono::microseconds(us));
#endif
    }
}

void signal_audio_thread() {
    if (g_ops.signal != nullptr) {
        g_ops.signal();
    }
}

void raise_sound_thread() {
    if (g_ops.raise_sound != nullptr && !on_audio_thread()) {
        g_ops.raise_sound();
    }
}

void restore_sound_thread() {
    if (g_ops.restore_sound != nullptr && !on_audio_thread()) {
        g_ops.restore_sound();
    }
}

} // namespace mkwv::runtime
