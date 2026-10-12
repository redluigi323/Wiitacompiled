#pragma once

// Per-frame timing, printed to the watchdog log as a `mkwv: PERF` line every couple of seconds.
//
// The point is to say where a slow frame's time goes - the guest's own code, decoding the GX
// stream, issuing GL, EFB copies, the swap, or deliberate sleeps - without a profiler on the
// device. Zones are wall-clock on the thread that measured them; `guest` is what is left of
// the frame on the guest thread after everything else. Not shard-visible.

#include <cstdint>

namespace mkwv::perf {

enum Zone : unsigned {
    kDecode = 0,   // Processor::process on the guest thread (frame drains, mid-frame drains)
    kSubmit,       // submit_processor: GL state + draws, including EFB copies
    kCopy,         // EFB copies (inside kSubmit)
    kSwap,         // present blit + vglSwapBuffers (vsync wait included)
    kSleep,        // deliberate sleeps in HLE (retrace pacing, OSSleepThread)
    kRenderWait,   // guest thread waiting for the render thread (render-thread mode)
    kRender,       // render thread busy time (decode + submit + swap there)
    kWorker,       // vertex worker busy time (texgen, lighting, transform on core 2)
    kWorkerWait,   // decoding thread waiting for the vertex jobs (before a submit; it helps meanwhile)
    kHelp,         // vertex jobs run by threads other than the worker (guest / render thread waits)
    kSubmitIndex,  // submit: expanding and remapping the indices of merged runs (inside kSubmit)
    kSubmitDraw,   // submit: draw_run - program, textures, uniforms, attributes, glDrawElements
    kRingUpload,   // cached batch -> GPU ring streaming upload (inside submit)
    kZoneCount
};

enum Counter : unsigned {
    kDraws = 0,     // GX draws decoded
    kGlDraws,       // glDrawElements actually issued
    kGpuCopies,     // EFB copies done on the GPU
    kCpuCopies,     // EFB copies read back to the CPU
    kSkippedCopies, // EFB copies skipped by the effects setting
    kTexDecodes,    // textures converted and uploaded
    kPrograms,      // TEV programs compiled
    kMidDrains,     // FIFO buffer drained mid-frame
    kVertices,
    kReusedVertices,
    kReusedDrawStates,
    kSkippedFrames,  // frames decoded but not drawn (frameskip)
    kWorkerVertices, // vertices queued as vertex jobs
    kVertexErrors,   // vertices whose array element could not be read in a vertex job
    kGpuMeshHits,    // cached mesh runs drawn without decoding
    kGpuMeshRebuilds,// mesh runs decoded into the cache
    kGpuMeshVerts,   // vertices served from a cache hit
    kFastVerts,      // vertices transformed by the combined clip matrix
    kGpuLitVerts,    // vertices whose lighting runs in the vertex shader
    kDlHitVerts,     // display-list vertices served decoded from the DL cache (static64)
    kDlFillVerts,    // display-list vertices decoded into the DL cache
    kDlRejects,      // DL cache spot checks that failed (arrays changed under the cache)
    kDlBusy,         // DL primitives drawn again before their fill completed
    kVglPoolKb,      // vitaGL circular pool used per frame (KiB, summed per frame)
    kRingKb,         // GPU vertex ring bytes packed per frame (KiB, summed per frame)
    kRingSyncs,      // GPU waits forced by the ring wrapping onto recent frames
    kSlowCpuLit,     // job vertices off the fast path: lit on the CPU (not GPU-lit eligible)
    kSlowGenCount,   // ... more than two texgens (or SU scale without one)
    kSlowGenKind,    // ... a texgen that is not a 2x4 matrix of its own texcoord
    kCounterCount
};

std::uint64_t now_us();
void add(Zone zone, std::uint64_t us);
void count(Counter counter, std::uint32_t n = 1);
// Takes time back out of a zone - for work nested inside it that is accounted elsewhere
// (a batch submitted from inside a decode).
void exclude(Zone zone, std::uint64_t us);

class Scope {
public:
    explicit Scope(Zone zone) : zone_(zone), start_(now_us()) {}
    ~Scope() { add(zone_, now_us() - start_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone_;
    std::uint64_t start_;
};

// Threads whose CPU time the PERF line reports (`cpu:` ms per frame, from the kernel's run
// clock - unlike the wall-clock zones this excludes time the thread was preempted). `uid` is a
// SceUID; ignored off the Vita. At most 8.
void register_thread(const char* tag, int uid);
unsigned registered_thread_count();
const char* registered_thread_tag(unsigned i);
int registered_thread_uid(unsigned i);

// Marks the end of a presented guest frame (on the guest thread). `extra` is appended to the
// line (dispatch/memory counters the gx library cannot see). Returns the PERF line when one is
// due (about every two seconds), else nullptr.
const char* frame_mark(const char* extra);
// True when the next frame_mark() will return a line (for extras computed per window).
bool frame_mark_due();

} // namespace mkwv::perf
