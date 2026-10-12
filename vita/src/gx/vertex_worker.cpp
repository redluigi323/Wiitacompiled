#include "mkwv/gx/vertex_worker.hpp"

#include "mkwv/gx/capture.hpp"
#include "mkwv/gx/dl_cache.hpp"
#include "mkwv/gx/perf.hpp"

#include <atomic>
#include <cstring>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace mkwv::gx {
namespace {

// One producer (the decoding thread) publishes jobs; any number of consumers claim them
// (compare-and-swap on g_claim) and mark them done. The producer retires done jobs in order
// (g_retire) and only then reuses their slots, snapshots and stashed bytes. Job and snapshot
// contents are written before the release store of g_head that publishes them.
constexpr std::uint32_t kJobs = 2048;          // power of two
// static64: 64 -> 256. A race frame takes ~1000 snapshots; with 64 slots the decoding thread kept
// stopping to wait for (and help with) jobs 64 snapshots back.
constexpr std::uint32_t kSnapshots = 256;      // ~5.5 KiB each
constexpr std::uint32_t kPublishVerts = 64;    // hand a growing run over once it is this long
constexpr std::uint32_t kStashBytes = 512u * 1024u;
// A long spin pinned the worker's core between menu jobs. A few polls, then the semaphore.
constexpr unsigned kWorkerSpin = 64;

struct Job {
    DecodedVertex* verts;
    const std::uint8_t* bytes;
    const CachedVertex* cached;
    CachedVertex* fill;
    std::uint32_t first;
    std::uint32_t count;
    std::uint32_t snap;
    std::uint32_t stride;
    std::atomic<std::uint32_t> done;
};

Job g_jobs[kJobs];
std::atomic<std::uint32_t> g_head{0};   // published (producer)
std::atomic<std::uint32_t> g_claim{0};  // claimed (consumers)
std::uint32_t g_retire = 0;             // retired (producer only)
// One flag per worker thread (static63: two workers). A producer that clears a flag owns one
// wake of the shared semaphore; a worker whose flag was cleared that way consumes exactly one.
constexpr unsigned kMaxWorkers = 4;
std::atomic<bool> g_sleeping[kMaxWorkers] = {};
std::atomic<unsigned> g_worker_ids{0};
std::atomic<bool> g_quit{false};
std::atomic<bool> g_active{false};
VertexWorkerHooks g_hooks{};

VertexSnapshot g_snaps[kSnapshots];
std::uint32_t g_snap_next = 0;

struct Pending {
    DecodedVertex* verts;
    const std::uint8_t* bytes;
    const CachedVertex* cached;
    CachedVertex* fill;
    std::uint32_t first, count, snap, stride;
};
Pending g_pending{};
bool g_has_pending = false;

alignas(16) std::uint8_t g_stash[kStashBytes];
std::uint32_t g_stash_used = 0;

// Batch output. Without a ring (host tests, or the ring failed to allocate) every batch packs
// from offset 0 of the static arena. Staged rings upload this cached arena once per batch;
// direct rings pack into GPU memory with a persistent wrapping cursor.
constexpr std::uint32_t kArenaBytes = kMaxDecodedVerts * (kPackOffTex + 12u * kMaxTexGens + kPackLitBytes);
alignas(16) std::uint8_t g_static_arena[kArenaBytes];
std::uint8_t* g_arena = g_static_arena;
std::uint32_t g_arena_bytes = kArenaBytes;
bool g_ring = false;
bool g_stage_wanted = true;
bool g_ring_staged = false;
bool g_stage_ready = false;
std::uint8_t* g_gpu_arena = nullptr;
std::uint32_t g_gpu_bytes = 0;
std::uint32_t g_gpu_offset[kMaxDecodedVerts];
void (*g_ring_sync)() = nullptr;
std::uint64_t g_ring_abs = 0;         // absolute bytes written into the ring (never wraps)
std::uint64_t g_frame_start_abs[4] = {};
std::uint32_t g_frame_serial = 0;
std::uint32_t g_ring_syncs = 0;
bool g_mirror = true;  // processed vertices also go to Processor::verts
std::uint32_t g_offset[kMaxDecodedVerts];
std::uint32_t g_segment[kMaxDecodedVerts];
std::int32_t g_remap[kMaxDecodedVerts];
const Processor* g_owner = nullptr;
std::uint32_t g_cursor = 0;
std::uint32_t g_seg = 0;
std::uint32_t g_seg_stride = 0;

void relax() {
    if (g_hooks.relax != nullptr) {
        g_hooks.relax();
    }
}

void wake_worker() {
    // Exactly one wake per sleep: whoever clears a flag owns that wake.
    for (unsigned k = 0; k < kMaxWorkers; ++k) {
        if (g_sleeping[k].load(std::memory_order_seq_cst) &&
            g_sleeping[k].exchange(false, std::memory_order_seq_cst) && g_hooks.wake != nullptr) {
            g_hooks.wake();
        }
    }
}

void run_job(const Job& job) {
    VertexJob j;
    j.snap = &g_snaps[job.snap % kSnapshots];
    j.verts = job.verts;
    j.bytes = job.bytes;
    j.cached = job.cached;
    j.fill = job.fill;
    j.first = job.first;
    j.count = job.count;
    j.stride = job.stride;
    j.remap = g_remap;
    j.arena = g_arena;
    j.offset = g_offset;
    const std::uint32_t errors = process_vertex_job(j);
    if (errors != 0) {
        perf::count(perf::kVertexErrors, errors);
    }
}

// Claims and runs one job; false when there is none.
bool try_run_one() {
    std::uint32_t c = g_claim.load(std::memory_order_relaxed);
    for (;;) {
        if (c == g_head.load(std::memory_order_acquire)) {
            return false;
        }
        if (g_claim.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            break;
        }
    }
    Job& job = g_jobs[c % kJobs];
    run_job(job);
    job.done.store(1, std::memory_order_release);
    return true;
}

void retire_done() {
    const std::uint32_t head = g_head.load(std::memory_order_relaxed);
    while (g_retire != head && g_jobs[g_retire % kJobs].done.load(std::memory_order_acquire) != 0) {
        ++g_retire;
    }
}

// Producer-side wait step: do a job if there is one, else let the others finish.
void help_or_relax() {
    if (!vertex_worker_help()) {
        relax();
    }
}

void publish(const Pending& p) {
    const std::uint32_t head = g_head.load(std::memory_order_relaxed);
    for (;;) {
        retire_done();
        if (head - g_retire < kJobs) {
            break;
        }
        help_or_relax();
    }
    Job& job = g_jobs[head % kJobs];
    job.verts = p.verts;
    job.bytes = p.bytes;
    job.cached = p.cached;
    job.fill = p.fill;
    job.first = p.first;
    job.count = p.count;
    job.snap = p.snap;
    job.stride = p.stride;
    job.done.store(0, std::memory_order_relaxed);
    g_head.store(head + 1, std::memory_order_seq_cst);
    wake_worker();
}

void publish_pending() {
    if (g_has_pending) {
        g_has_pending = false;
        publish(g_pending);
    }
}

} // namespace

void vertex_worker_install(const VertexWorkerHooks& hooks) {
    g_hooks = hooks;
    g_quit.store(false);
    g_active.store(true, std::memory_order_release);
}

void vertex_worker_uninstall() {
    if (!g_active.load()) {
        return;
    }
    vertex_worker_drain();
    g_active.store(false, std::memory_order_release);
}

bool vertex_worker_active() {
    return g_active.load(std::memory_order_relaxed);
}

void vertex_worker_quit() {
    g_quit.store(true, std::memory_order_seq_cst);
    wake_worker();
}

bool vertex_worker_help() {
    if (g_claim.load(std::memory_order_relaxed) == g_head.load(std::memory_order_acquire)) {
        return false;
    }
    const std::uint64_t t0 = perf::now_us();
    const bool ran = try_run_one();
    if (ran) {
        perf::add(perf::kHelp, perf::now_us() - t0);
    }
    return ran;
}

void vertex_worker_main() {
    const unsigned me = g_worker_ids.fetch_add(1) % kMaxWorkers;
    std::atomic<bool>& g_sleeping_me = g_sleeping[me];
    for (;;) {
        if (g_quit.load(std::memory_order_acquire)) {
            return;
        }
        bool idle = g_claim.load(std::memory_order_relaxed) == g_head.load(std::memory_order_acquire);
        if (idle) {
            for (unsigned k = 0; k < kWorkerSpin; ++k) {
                idle = g_claim.load(std::memory_order_relaxed) == g_head.load(std::memory_order_acquire);
                if (!idle || g_quit.load(std::memory_order_relaxed)) {
                    break;
                }
            }
            if (idle) {
                g_sleeping_me.store(true, std::memory_order_seq_cst);
                if (g_claim.load(std::memory_order_seq_cst) != g_head.load(std::memory_order_seq_cst) ||
                    g_quit.load(std::memory_order_seq_cst)) {
                    // Work (or quit) arrived meanwhile. If the producer already took the flag,
                    // its wake is on the way: consume it so the next wait really waits.
                    if (!g_sleeping_me.exchange(false, std::memory_order_seq_cst) && g_hooks.wait != nullptr) {
                        g_hooks.wait();
                    }
                } else if (g_hooks.wait != nullptr) {
                    g_hooks.wait();
                } else {
                    g_sleeping_me.store(false);
                }
                continue;
            }
        }
        const std::uint64_t t0 = perf::now_us();
        while (try_run_one()) {
        }
        perf::add(perf::kWorker, perf::now_us() - t0);
    }
}

std::uint32_t vertex_worker_snapshot(const State& state, const PreparedLighting& prepared,
                                     const float (*su_scale)[2], bool su_any, const VertexDecoder& decoder,
                                     bool gpu_lit) {
    // A new snapshot starts a new run: nothing pending can merge with what follows.
    publish_pending();
    const std::uint32_t id = g_snap_next++;
    // The slot last held snapshot id - kSnapshots: wait until no unretired job uses it.
    for (;;) {
        retire_done();
        if (g_retire == g_head.load(std::memory_order_relaxed) || id - g_jobs[g_retire % kJobs].snap < kSnapshots) {
            break;
        }
        help_or_relax();
    }
    VertexSnapshot& snap = g_snaps[id % kSnapshots];
    capture_vertex_snapshot(state, prepared, su_scale, su_any, snap);
    snap.gpu_lit = gpu_lit;
    snap.decoder = decoder;
    return id;
}

const std::uint8_t* vertex_worker_stash(const std::uint8_t* src, std::uint32_t n) {
    if (n > kStashBytes) {
        return nullptr;
    }
    if (kStashBytes - g_stash_used < n) {
        // Full: once everything queued is done the space is free again.
        vertex_worker_drain();
        g_stash_used = 0;
    }
    std::uint8_t* dst = g_stash + g_stash_used;
    std::memcpy(dst, src, n);
    g_stash_used += n;
    return dst;
}

void vertex_worker_push(DecodedVertex* verts, std::uint32_t first, std::uint32_t count, std::uint32_t snap,
                        const std::uint8_t* bytes, std::uint32_t stride, const CachedVertex* cached,
                        CachedVertex* fill) {
    if (count == 0) {
        return;
    }
    if (!g_mirror && !capture_armed() && !capture_active()) {
        verts = nullptr;
    }
    const bool contiguous =
        g_has_pending && g_pending.verts == verts && g_pending.snap == snap && g_pending.first + g_pending.count == first &&
        (cached != nullptr
             ? (g_pending.cached != nullptr && g_pending.cached + g_pending.count == cached)
             : (g_pending.cached == nullptr && g_pending.stride == stride &&
                g_pending.bytes + g_pending.count * stride == bytes &&
                (fill == nullptr ? g_pending.fill == nullptr
                                 : (g_pending.fill != nullptr && g_pending.fill + g_pending.count == fill))));
    if (contiguous) {
        g_pending.count += count;
    } else {
        publish_pending();
        g_pending = Pending{verts, bytes, cached, fill, first, count, snap, stride};
        g_has_pending = true;
    }
    if (g_pending.count >= kPublishVerts) {
        publish_pending();
    }
}

void vertex_worker_flush() {
    publish_pending();
}

void vertex_worker_drain() {
    publish_pending();
    retire_done();
    if (g_retire == g_head.load(std::memory_order_relaxed)) {
        return;
    }
    perf::Scope waiting(perf::kWorkerWait);
    for (;;) {
        retire_done();
        if (g_retire == g_head.load(std::memory_order_relaxed)) {
            return;
        }
        help_or_relax();
    }
}

void vertex_batch_begin(const Processor* owner) {
    // The previous batch was drained before it was submitted; nothing still reads the stash.
    vertex_worker_drain();
    g_owner = owner;
    dl_cache_batch_begin();  // fills queued before the drain are complete
    if (!g_ring || g_ring_staged) {
        g_cursor = 0;
    }
    g_stage_ready = false;
    ++g_seg;  // a new batch never continues the previous batch's segment
    g_seg_stride = 0;
    g_stash_used = 0;
}

std::uint32_t vertex_batch_alloc(std::uint32_t first, std::uint32_t count, unsigned ntc, bool lit) {
    const std::uint32_t stride = pack_stride(ntc, lit);
    const std::uint32_t bytes = count * stride;
    g_stage_ready = false;
    if (g_ring && !g_ring_staged && g_cursor + bytes > g_arena_bytes) {
        // Wrap. Offsets restart at 0, so the next vertices start a new segment.
        g_ring_abs += g_arena_bytes - g_cursor;
        g_cursor = 0;
        g_seg_stride = 0;
    }
    if (g_ring && !g_ring_staged) {
        // Overwriting ring bytes written since the start of frame N-3 (the GPU may still read
        // them): wait for the GPU. Never in steady state (the ring holds ~4 race frames).
        const std::uint64_t old = g_frame_start_abs[(g_frame_serial + 1u) & 3u];  // frame - 3
        if (g_ring_abs + bytes > old + g_arena_bytes && g_ring_sync != nullptr) {
            g_ring_sync();
            ++g_ring_syncs;
            for (auto& f : g_frame_start_abs) {
                f = g_ring_abs;
            }
        }
        g_ring_abs += bytes;
    }
    if (stride != g_seg_stride) {
        ++g_seg;
        g_seg_stride = stride;
    }
    const std::uint32_t off = g_cursor;
    for (std::uint32_t i = 0; i < count && first + i < kMaxDecodedVerts; ++i) {
        g_offset[first + i] = off + i * stride;
        g_segment[first + i] = g_seg;
    }
    g_cursor += count * stride;
    return off;
}

void vertex_batch_unalloc(std::uint32_t, std::uint32_t count, unsigned ntc, bool lit) {
    const std::uint32_t bytes = count * pack_stride(ntc, lit);
    if (bytes <= g_cursor) {
        g_cursor -= bytes;
        if (g_ring && !g_ring_staged) {
            g_ring_abs -= bytes;
        }
    }
}

void vertex_ring_install(std::uint8_t* base, std::uint32_t bytes, void (*gpu_sync)()) {
    vertex_worker_drain();
    if (base == nullptr || bytes < 1024u * 1024u) {
        g_arena = g_static_arena;
        g_arena_bytes = kArenaBytes;
        g_ring = false;
        g_ring_staged = false;
        g_gpu_arena = nullptr;
        g_gpu_bytes = 0;
        g_stage_ready = false;
        g_cursor = 0;
        return;
    }
    g_gpu_arena = base;
    g_gpu_bytes = bytes;
    g_ring_staged = g_stage_wanted;
    g_stage_ready = false;
    g_arena = g_ring_staged ? g_static_arena : base;
    g_arena_bytes = g_ring_staged ? kArenaBytes : bytes;
    g_ring = true;
    g_ring_sync = gpu_sync;
    g_cursor = 0;
    g_ring_abs = 0;
    for (auto& f : g_frame_start_abs) {
        f = 0;
    }
    g_frame_serial = 0;
}

void vertex_ring_set_staging(bool on) {
    // Configure before install; changing a live ring would invalidate queued jobs/offsets.
    g_stage_wanted = on;
}

bool vertex_ring_staging() {
    return g_ring_staged;
}

// Completed CPU batches remain in cached RAM until submission. The GPU sees a single
// streaming upload, rather than interleaved small uncached stores from multiple workers.
void publish_staged_ring() {
    if (!g_ring_staged || g_stage_ready || g_owner == nullptr || g_cursor == 0 || g_cursor > g_gpu_bytes) {
        return;
    }
    vertex_worker_drain();
    std::uint32_t cursor = static_cast<std::uint32_t>(g_ring_abs % g_gpu_bytes);
    if (cursor + g_cursor > g_gpu_bytes) {
        g_ring_abs += g_gpu_bytes - cursor;
        cursor = 0;
    }
    const std::uint64_t old = g_frame_start_abs[(g_frame_serial + 1u) & 3u];
    if (g_ring_abs + g_cursor > old + g_gpu_bytes && g_ring_sync != nullptr) {
        g_ring_sync();
        ++g_ring_syncs;
        for (auto& frame : g_frame_start_abs) frame = g_ring_abs;
    }
    perf::Scope upload(perf::kRingUpload);
    std::uint32_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 64u <= g_cursor; i += 64u) {
        const uint8x16x4_t block = vld1q_u8_x4(g_static_arena + i);
        vst1q_u8_x4(g_gpu_arena + cursor + i, block);
    }
#endif
    std::memcpy(g_gpu_arena + cursor + i, g_static_arena + i, g_cursor - i);
#if defined(__arm__) || defined(__aarch64__)
    // Complete uncached streaming stores before the GPU can be kicked on another bus.
    asm volatile("dsb sy" ::: "memory");
#endif
    for (std::uint32_t v = 0; v < g_owner->vert_count && v < kMaxDecodedVerts; ++v) {
        g_gpu_offset[v] = cursor + g_offset[v];
    }
    g_ring_abs += g_cursor;
    g_stage_ready = true;
}

bool vertex_ring_active() {
    return g_ring;
}

void vertex_ring_frame_mark() {
    ++g_frame_serial;
    g_frame_start_abs[g_frame_serial & 3u] = g_ring_abs;
}

std::uint32_t vertex_ring_take_syncs() {
    const std::uint32_t n = g_ring_syncs;
    g_ring_syncs = 0;
    return n;
}

std::uint32_t vertex_ring_frame_bytes() {
    return static_cast<std::uint32_t>(g_ring_abs - g_frame_start_abs[g_frame_serial & 3u]);
}

void vertex_worker_set_mirror(bool on) {
    g_mirror = on;
}

bool vertex_worker_mirror() {
    return g_mirror;
}

std::uint8_t* vertex_batch_arena() {
    return g_arena;
}

void vertex_batch_set_remap(std::uint32_t index, std::int32_t src) {
    g_remap[index] = src;
}

bool packed_batch(const Processor& processor, PackedBatch* out) {
    if (g_owner != &processor) {
        return false;
    }
    publish_staged_ring();
    out->arena = g_stage_ready ? g_gpu_arena : g_arena;
    out->offset = g_stage_ready ? g_gpu_offset : g_offset;
    out->segment = g_segment;
    out->remap = g_remap;
    return true;
}

void resolve_vertex_copies(Processor& processor) {
    if (g_owner != &processor) {
        return;
    }
    vertex_worker_drain();
    for (std::uint32_t v = 0; v < processor.vert_count && v < kMaxDecodedVerts; ++v) {
        if (g_remap[v] >= 0) {
            processor.verts[v] = processor.verts[g_remap[v]];
        }
    }
}

} // namespace mkwv::gx
