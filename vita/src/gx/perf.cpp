#include "mkwv/gx/perf.hpp"

#include "mkwv/gx/gpu_mesh.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <chrono>
#endif

namespace mkwv::perf {
namespace {

// Zones may be added from the render thread while the guest thread reads them; relaxed
// atomics keep the totals coherent without locks (the numbers are statistics).
std::atomic<std::uint64_t> g_zone[kZoneCount];
std::atomic<std::uint32_t> g_counter[kCounterCount];

std::uint64_t g_window_start = 0;
std::uint32_t g_window_frames = 0;
std::uint64_t g_worst_frame = 0;
std::uint64_t g_last_mark = 0;
char g_line[2048];

struct Tracked {
    const char* tag = nullptr;
    int uid = -1;
    std::uint64_t last = 0;
};
Tracked g_threads[8];
std::atomic<unsigned> g_thread_count{0};

std::uint64_t thread_run_us(int uid) {
#if defined(__vita__)
    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(uid, &info) < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(info.runClocks);
#else
    (void)uid;
    return 0;
#endif
}

} // namespace

std::uint64_t now_us() {
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

void register_thread(const char* tag, int uid) {
    const unsigned i = g_thread_count.load();
    if (i >= 8u) {
        return;
    }
    g_threads[i].tag = tag;
    g_threads[i].uid = uid;
    g_threads[i].last = thread_run_us(uid);
    g_thread_count.store(i + 1u);
}

unsigned registered_thread_count() {
    return g_thread_count.load();
}

const char* registered_thread_tag(unsigned i) {
    return i < 8u ? g_threads[i].tag : "";
}

int registered_thread_uid(unsigned i) {
    return i < 8u ? g_threads[i].uid : -1;
}

void add(Zone zone, std::uint64_t us) {
    g_zone[zone].fetch_add(us, std::memory_order_relaxed);
}

void exclude(Zone zone, std::uint64_t us) {
    g_zone[zone].fetch_sub(us, std::memory_order_relaxed);
}

void count(Counter counter, std::uint32_t n) {
    g_counter[counter].fetch_add(n, std::memory_order_relaxed);
}

bool frame_mark_due() {
    return g_window_start != 0 && now_us() - g_window_start >= 2000000u;
}

const char* frame_mark(const char* extra) {
    const std::uint64_t now = now_us();
    if (g_window_start == 0) {
        g_window_start = now;
        g_last_mark = now;
        return nullptr;
    }
    const std::uint64_t frame = now - g_last_mark;
    g_last_mark = now;
    if (frame > g_worst_frame) {
        g_worst_frame = frame;
    }
    ++g_window_frames;
    const std::uint64_t window = now - g_window_start;
    if (window < 2000000u) {
        return nullptr;
    }
    std::uint64_t z[kZoneCount];
    for (unsigned i = 0; i < kZoneCount; ++i) {
        z[i] = g_zone[i].exchange(0, std::memory_order_relaxed);
    }
    std::uint32_t c[kCounterCount];
    for (unsigned i = 0; i < kCounterCount; ++i) {
        c[i] = g_counter[i].exchange(0, std::memory_order_relaxed);
    }
    const double n = g_window_frames != 0 ? static_cast<double>(g_window_frames) : 1.0;
    char cpu[256];
    cpu[0] = 0;
    {
        std::size_t used = 0;
        const unsigned nt = g_thread_count.load();
        for (unsigned i = 0; i < nt && used + 32 < sizeof(cpu); ++i) {
            const std::uint64_t now_run = thread_run_us(g_threads[i].uid);
            const std::uint64_t d = now_run >= g_threads[i].last ? now_run - g_threads[i].last : 0;
            g_threads[i].last = now_run;
            const int w = std::snprintf(cpu + used, sizeof(cpu) - used, " %s=%.1f", g_threads[i].tag,
                                        static_cast<double>(d) / 1000.0 / n);
            if (w > 0) {
                used += static_cast<std::size_t>(w);
            }
        }
    }
    auto ms = [n](std::uint64_t us) { return static_cast<double>(us) / 1000.0 / n; };
    // Guest-thread time not accounted to any zone. In render-thread mode submit and swap run
    // on the other thread (they are in `render`), so they are not subtracted here.
    const std::uint64_t accounted = z[kRenderWait] + z[kSleep] +
                                    (z[kRender] == 0 ? z[kDecode] + z[kSubmit] + z[kSwap] : 0u);
    const std::uint64_t guest = window > accounted ? window - accounted : 0u;
    std::snprintf(g_line, sizeof(g_line),
                  "mkwv: PERF fps=%.1f frame=%.1fms worst=%.1fms | guest=%.1f decode=%.1f submit=%.1f (copy=%.1f) "
                  "swap=%.1f sleep=%.1f render=%.1f rwait=%.1f | per frame: draws=%.0f gl=%.0f copies=%.0f/%.0f/%.0f "
                  "texdec=%.1f prog=%.1f pbuild=%.1f mid=%.1f vreuse=%.0f/%.0f sreuse=%.0f skipped=%u/%u | worker=%.1f help=%.1f (%.0f verts) wwait=%.1f verr=%u vfast=%.0f glit=%.0f | sub: idx=%.1f draw=%.1f mesh=%.1f prep=%.1f uup=%.0f/%.0f | dl: hit=%.0f fill=%.0f rej=%u busy=%.0f | gxf=%.0f gcopy=%.0f gsync=%u | vpool=%.0fK ring=%.0fK rsync=%u rup=%.1f | slow: lit=%.0f ngen=%.0f gen=%.0f | cpu:%s%s\n",
                  n * 1000000.0 / static_cast<double>(window), static_cast<double>(window) / 1000.0 / n,
                  static_cast<double>(g_worst_frame) / 1000.0, ms(guest), ms(z[kDecode]), ms(z[kSubmit]),
                  ms(z[kCopy]), ms(z[kSwap]), ms(z[kSleep]), ms(z[kRender]), ms(z[kRenderWait]), c[kDraws] / n,
                  c[kGlDraws] / n, c[kGpuCopies] / n, c[kCpuCopies] / n, c[kSkippedCopies] / n, c[kTexDecodes] / n,
                  c[kPrograms] / n, ms(z[kProgBuild]), c[kMidDrains] / n, c[kReusedVertices] / n, c[kVertices] / n, c[kReusedDrawStates] / n,
                  c[kSkippedFrames], g_window_frames, ms(z[kWorker]), ms(z[kHelp]), c[kWorkerVertices] / n, ms(z[kWorkerWait]), c[kVertexErrors],
                  c[kFastVerts] / n, c[kGpuLitVerts] / n, ms(z[kSubmitIndex]), ms(z[kSubmitDraw]), ms(z[kSubmitMesh]),
                  ms(z[kSubmitPrep]), c[kLitUploads] / n, c[kXfUploads] / n,
                  c[kDlHitVerts] / n, c[kDlFillVerts] / n, c[kDlRejects], c[kDlBusy] / n, c[kGpuXfVerts] / n,
                  c[kGpuXfCopied] / n, c[kGpuXfSyncs], c[kVglPoolKb] / n,
                  c[kRingKb] / n, c[kRingSyncs], ms(z[kRingUpload]), c[kSlowCpuLit] / n, c[kSlowGenCount] / n, c[kSlowGenKind] / n, cpu,
                  extra != nullptr ? extra : "");
    g_window_start = now;
    g_window_frames = 0;
    g_worst_frame = 0;
    return g_line;
}

} // namespace mkwv::perf
