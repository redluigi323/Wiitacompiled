// Host-PC sampling profiler (static65).
//
// The guest profiler (guest_prof.cpp) only sees functions entered through the dispatcher; the
// translated code calls most functions statically, so its "self" time is a whole subtree and
// the game thread's profile came out flat. This samples the ARM program counter instead.
//
// One sampler thread per core, at the highest priority on that core: when it wakes it preempts
// whatever runs there, so that thread's registers are in its saved context. It then reads the
// saved context of each registered thread (perf::register_thread) that is READY - i.e. was just
// preempted - through sceKernelGetThreadContextForVM, and counts the PC and LR (32-byte
// buckets). A report every ~5 s lists the top buckets per thread as raw addresses (symbolised
// offline against the ELF). If the kernel refuses the call, the log says so once.
#include "mkwv/platform/profiler.hpp"

#include "mkwv/gx/perf.hpp"
#include "mkwv/runtime/diagnostics.hpp"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" int sceKernelGetThreadContextForVM(SceUID thid, void* cpu_registers, void* vfp_registers);

namespace mkwv::platform {
namespace {

constexpr unsigned kMaxThreads = 8;
constexpr unsigned kBuckets = 4096;  // power of two
constexpr unsigned kTop = 40;

struct Hist {
    std::uint32_t key[kBuckets];
    std::uint32_t count[kBuckets];
    std::uint32_t dropped;
    void clear() {
        std::memset(key, 0, sizeof(key));
        std::memset(count, 0, sizeof(count));
        dropped = 0;
    }
    void add(std::uint32_t k) {
        std::uint32_t h = (k >> 5) * 0x9E3779B1u;
        for (unsigned probe = 0; probe < 16; ++probe) {
            const std::uint32_t i = (h + probe) & (kBuckets - 1u);
            if (key[i] == k) {
                ++count[i];
                return;
            }
            if (key[i] == 0) {
                key[i] = k;
                count[i] = 1;
                return;
            }
        }
        ++dropped;
    }
};

struct Tracked {
    Hist pc;
    Hist lr;
    std::uint32_t ready = 0, running = 0, waiting = 0, fails = 0;
    int last_error = 0;
    bool dumped = false;
};

Tracked g_tracked[kMaxThreads];
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
std::uint64_t g_last_report = 0;
std::atomic<bool> g_reported_unavailable{false};
char g_line[2048];

bool try_lock() {
    // Sampling must never burn another core waiting for a log write or a preempted sampler.
    return !g_lock.test_and_set(std::memory_order_acquire);
}
void unlock() {
    g_lock.clear(std::memory_order_release);
}

int sampler_main(SceSize, void* argp);

void report_hist(const char* tag, const char* what, Hist& h, std::uint32_t total) {
    // Top kTop by count (selection; the table is small).
    std::uint32_t idx[kTop];
    unsigned n = 0;
    for (unsigned i = 0; i < kBuckets; ++i) {
        if (h.count[i] == 0) continue;
        unsigned pos = n < kTop ? n++ : kTop;
        if (pos == kTop) {
            if (h.count[i] <= h.count[idx[kTop - 1]]) continue;
            pos = kTop - 1;
        }
        while (pos > 0 && h.count[idx[pos - 1]] < h.count[i]) {
            if (pos < kTop) idx[pos] = idx[pos - 1];
            --pos;
        }
        idx[pos] = i;
    }
    int used = std::snprintf(g_line, sizeof(g_line), "mkwv: HPROF %s %s n=%u:", tag, what, total);
    for (unsigned k = 0; k < n && used > 0 && used + 24 < static_cast<int>(sizeof(g_line)); ++k) {
        used += std::snprintf(g_line + used, sizeof(g_line) - used, " %08X:%.1f", h.key[idx[k]],
                              total != 0 ? 100.0 * h.count[idx[k]] / total : 0.0);
    }
    std::snprintf(g_line + used, sizeof(g_line) - used, "\n");
    runtime::watchdog_log_line(g_line);
}

void maybe_report() {
    const std::uint64_t now = sceKernelGetProcessTimeWide();
    if (g_last_report == 0) {
        g_last_report = now;
        return;
    }
    if (now - g_last_report < 5000000u) {
        return;
    }
    // Take a bounded snapshot, then release before sorting, formatting or filesystem I/O.
    // Other cores skip a sample while copying instead of spinning at highest priority.
    static Tracked snapshot[kMaxThreads];
    if (!try_lock()) return;
    std::memcpy(snapshot, g_tracked, sizeof(snapshot));
    for (Tracked& tr : g_tracked) {
        tr.pc.clear();
        tr.lr.clear();
        tr.ready = tr.running = tr.waiting = tr.fails = 0;
    }
    unlock();
    g_last_report = now;
    // Where this code runs, to line the samples up with the ELF's symbols (the loader may
    // relocate the executable).
    std::snprintf(g_line, sizeof(g_line), "mkwv: HPROF anchor start_host_profiler=%p sampler=%p\n",
                  reinterpret_cast<void*>(&start_host_profiler), reinterpret_cast<void*>(&sampler_main));
    runtime::watchdog_log_line(g_line);
    const unsigned nt = perf::registered_thread_count();
    for (unsigned t = 0; t < nt && t < kMaxThreads; ++t) {
        const char* tag = perf::registered_thread_tag(t);
        Tracked& tr = snapshot[t];
        std::snprintf(g_line, sizeof(g_line),
                      "mkwv: HPROF %s ready=%u running=%u waiting=%u fail=%u err=%08X dropped=%u\n", tag, tr.ready,
                      tr.running, tr.waiting, tr.fails, static_cast<unsigned>(tr.last_error), tr.pc.dropped);
        runtime::watchdog_log_line(g_line);
        if (tr.ready != 0) {
            report_hist(tag, "pc", tr.pc, tr.ready);
            report_hist(tag, "lr", tr.lr, tr.ready);
        }
    }
}

int sampler_main(SceSize, void* argp) {
    const unsigned core = *static_cast<unsigned*>(argp);
    alignas(16) static std::uint32_t cpu[3][256];
    alignas(16) static std::uint32_t vfp[3][512];
    char sample_line[2048];  // owned by this thread; reports own g_line
    for (;;) {
        sceKernelDelayThread(1500 + core * 97);
        const unsigned nt = perf::registered_thread_count();
        for (unsigned t = 0; t < nt && t < kMaxThreads; ++t) {
            const int uid = perf::registered_thread_uid(t);
            SceKernelThreadInfo info;
            std::memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelGetThreadInfo(uid, &info) < 0) {
                continue;
            }
            Tracked& tr = g_tracked[t];
            if (info.status != SCE_THREAD_READY) {
                if (!try_lock()) continue;
                if (info.status == SCE_THREAD_RUNNING) {
                    ++tr.running;
                } else {
                    ++tr.waiting;
                }
                unlock();
                continue;
            }
            std::memset(cpu[core], 0, sizeof(cpu[core]));
            const int rc = sceKernelGetThreadContextForVM(uid, cpu[core], vfp[core]);
            if (!try_lock()) continue;
            if (rc < 0) {
                ++tr.fails;
                tr.last_error = rc;
                unlock();
                if (!g_reported_unavailable.exchange(true)) {
                    std::snprintf(sample_line, sizeof(sample_line), "mkwv: HPROF context read failed %08X\n",
                                  static_cast<unsigned>(rc));
                    runtime::watchdog_log_line(sample_line);
                }
                continue;
            }
            const bool dump = !tr.dumped;
            tr.dumped = true;
            // Record under the lock; format/write only after releasing it.
            ++tr.ready;
            tr.pc.add(cpu[core][15] & ~31u);
            tr.lr.add(cpu[core][14] & ~31u);
            unlock();
            if (dump) {
                int used = std::snprintf(sample_line, sizeof(sample_line), "mkwv: HPROF raw %s rc=%d:", perf::registered_thread_tag(t), rc);
                for (unsigned k = 0; k < 24 && used > 0; ++k) {
                    used += std::snprintf(sample_line + used, sizeof(sample_line) - used, " %08X", cpu[core][k]);
                }
                std::snprintf(sample_line + used, sizeof(sample_line) - used, "\n");
                runtime::watchdog_log_line(sample_line);
            }
        }
        if (core == 0) {
            maybe_report();
        }
    }
    return 0;
}

} // namespace

bool start_host_profiler() {
    static unsigned cores[3] = {0, 1, 2};
    const int masks[3] = {SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_2};
    bool any = false;
    for (unsigned k = 0; k < 3; ++k) {
        char name[32];
        std::snprintf(name, sizeof(name), "mkwv_hprof%u", k);
        // Above everything else on its core (the audio-out thread is 0x10000100 - 20).
        const SceUID tid = sceKernelCreateThread(name, sampler_main, 0x10000100 - 40, 16 * 1024, 0, masks[k], nullptr);
        if (tid < 0) {
            std::printf("mkwv: host profiler thread %u create failed 0x%08X\n", k, static_cast<unsigned>(tid));
            continue;
        }
        sceKernelStartThread(tid, sizeof(unsigned), &cores[k]);
        any = true;
    }
    return any;
}

} // namespace mkwv::platform
