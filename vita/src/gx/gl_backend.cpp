#include "mkwv/gx/backend.hpp"
#include "mkwv/gx/capture.hpp"
#include "mkwv/gx/demo.hpp"
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/efb_copy.hpp"
#include "mkwv/gx/perf.hpp"
#include "mkwv/gx/dl_cache.hpp"
#include "mkwv/gx/submit.hpp"
#include "mkwv/gx/vertex_worker.hpp"

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <vitaGL.h>

#include <cstdio>
#include <new>

// vitaGL's per-frame circular pool (third_party/vitaGL/source/vgl.c), read for the log.
extern "C" {
extern uint8_t* circular_data_pool[];
extern uint8_t* circular_data_pool_ptr[];
extern int vgl_circular_idx;
}

namespace mkwv::gx {
namespace {

bool g_ring_wanted = true;
bool g_dl_pool_wanted = true;

// static64: GPU vertex ring and display-list cache memory, outside vitaGL's pools. The ring is
// uncached main RAM mapped for the GPU, filled by completed cached batches by default;
// the DL cache pool is ordinary cached RAM.
constexpr unsigned kRingBytes = 6u * 1024u * 1024u;  // static64 log: ~1.3 MiB packed a frame
constexpr unsigned kDlPoolBytes = 6u * 1024u * 1024u;  // ~15k hit vertices a frame (~0.8 MiB)

void ring_gpu_sync() {
    glFinish();
}

void setup_vertex_memory() {
    if (g_ring_wanted) {
        const SceUID block = sceKernelAllocMemBlock("mkwv_vring", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, kRingBytes, nullptr);
        void* base = nullptr;
        if (block >= 0 && sceKernelGetMemBlockBase(block, &base) >= 0 && base != nullptr &&
            sceGxmMapMemory(base, kRingBytes, SCE_GXM_MEMORY_ATTRIB_READ) >= 0) {
            GLuint vbo = 0;
            glGenBuffers(1, &vbo);
            if (vbo != 0) {
                vertex_ring_install(static_cast<std::uint8_t*>(base), kRingBytes, &ring_gpu_sync);
                set_vertex_ring_buffer(vbo, static_cast<const std::uint8_t*>(base), kRingBytes);
                std::printf("mkwv: GPU vertex ring %u KiB at %p (vbo %u, %s)\n", kRingBytes >> 10, base, vbo,
                            vertex_ring_staging() ? "cached batch uploads" : "direct uncached writes");
            }
        } else {
            std::printf("mkwv: GPU vertex ring unavailable (block %d)\n", static_cast<int>(block));
            if (block >= 0) {
                sceKernelFreeMemBlock(block);
            }
        }
    }
    if (g_dl_pool_wanted) {
        const SceUID block = sceKernelAllocMemBlock("mkwv_dlcache", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, kDlPoolBytes, nullptr);
        void* base = nullptr;
        if (block >= 0 && sceKernelGetMemBlockBase(block, &base) >= 0 && base != nullptr) {
            dl_cache_install(base, kDlPoolBytes);
            std::printf("mkwv: display-list vertex cache %u KiB\n", kDlPoolBytes >> 10);
        } else {
            std::printf("mkwv: display-list vertex cache unavailable (block %d)\n", static_cast<int>(block));
        }
    }
}

constexpr int kDisplayW = 960;
constexpr int kDisplayH = 544;

// A new frame starts from GXSetCopyClear's colour, as the EFB does after the previous
// frame's display copy. Alpha is opaque: the default EFB format has none.
void clear_frame() {
    begin_efb_frame(efb_clear_color());
}

// Capture one real 3D frame without relying on an input chord during a slow
// race. Menu layout quads have w=1; perspective meshes have varying w. This
// captures the renderer's actual draws and EFB-copy ordering for host replay.
void maybe_capture_3d(const Processor& processor) {
    static bool captured = false;
    if (captured || capture_armed() || capture_active()) {
        return;
    }
    unsigned perspective = 0;
    for (std::uint32_t i = 0; i < processor.vert_count; ++i) {
        if (processor.verts[i].w > 1.5f && ++perspective >= 32u) {
            capture_arm("ux0:data/viicompiled/capture_auto_3d");
            captured = true;
            std::printf("mkwv: auto 3D capture armed verts=%u perspective=%u\n", processor.vert_count, perspective);
            return;
        }
    }
}

// The Processor filled up mid-frame: draw what it holds and let it carry on. This runs from
// inside a decode, so its time moves from the decode zone to the submit zone.
void flush_batch(Processor& processor) {
    const std::uint64_t start = perf::now_us();
    if (!frame_submit_open()) {
        clear_frame();
    }
    maybe_capture_3d(processor);
    submit_processor(processor);
    const std::uint64_t took = perf::now_us() - start;
    perf::add(perf::kSubmit, took);
    perf::exclude(perf::kDecode, took);
}

// Deferred swap (render thread): present_processor submits and leaves the swap pending, so
// the render thread can release the guest before waiting on vsync.
bool g_defer_swap = false;
bool g_swap_pending = false;

void do_swap() {
    perf::Scope timing(perf::kSwap);
    present_efb();
    {
        const int i = vgl_circular_idx;
        const std::uint32_t used = static_cast<std::uint32_t>(circular_data_pool_ptr[i] - circular_data_pool[i]);
        perf::count(perf::kVglPoolKb, used >> 10);
    }
    if (vertex_ring_active()) {
        perf::count(perf::kRingKb, vertex_ring_frame_bytes() >> 10);
        perf::count(perf::kRingSyncs, vertex_ring_take_syncs());
        vertex_ring_frame_mark();
    }
    vglSwapBuffers(GL_FALSE);
}

class GlBackend final : public Backend {
public:
    ~GlBackend() override { shutdown(); }

    bool initialize() override {
        if (ready_) {
            return true;
        }
        SceKernelFreeMemorySizeInfo free_mem{};
        free_mem.size = sizeof(free_mem);
        sceKernelGetFreeMemorySize(&free_mem);
        std::printf("mkwv: mem free user=%d cdram=%d phycont=%d\n", free_mem.size_user, free_mem.size_cdram,
                    free_mem.size_phycont);


        // vitaGL's return is NOT success/failure: GL_TRUE means it clamped the display size,
        // GL_FALSE means 960x544 was accepted. Every vitaGL sample ignores this value. Treating
        // GL_FALSE as failure made us exit main() immediately, which is the Vita3K "crash".
        // vitaGL's pools are hard reservations. On hardware, main memory is what runs out (the
        // guest's 90 MiB live there) while CDRAM sits mostly idle, so textures get a large
        // CDRAM pool and the RAM pool takes what is left after a reserve for the shader
        // compiler module, thread stacks and libc.
        constexpr int kMiB = 1024 * 1024;
        const int free_user = free_mem.size_user;
        const int free_cdram = free_mem.size_cdram;
        // The circular pool is where vitaGL puts every draw's vertices, indices and uniforms
        // for the frame being built (split between the display buffers). When a frame
        // outgrows it, each further draw falls back to a heap allocation from the RAM pool,
        // and once that is exhausted to a full sceGxmFinish + garbage collection - a GPU
        // stall per draw. 4 MiB (2 MiB a frame) was far too small for a race frame. It comes
        // out of the RAM pool (falling back to CDRAM), so the RAM pool grows with it.
        int circular = free_user - 24 * kMiB - 16 * kMiB;
        circular = circular > 12 * kMiB ? 12 * kMiB : (circular < 4 * kMiB ? 4 * kMiB : circular);
        circular &= ~(kMiB - 1);
        vglSetCircularPoolSize(static_cast<unsigned>(circular));  // must precede vglInit*
        int ram_pool = free_user - 24 * kMiB;
        const int ram_cap = 16 * kMiB + circular;
        ram_pool = ram_pool > ram_cap ? ram_cap : (ram_pool < 4 * kMiB ? 4 * kMiB : ram_pool);
        int cdram_pool = free_cdram - 24 * kMiB;
        cdram_pool = cdram_pool > 64 * kMiB ? 64 * kMiB : (cdram_pool < 16 * kMiB ? 16 * kMiB : cdram_pool);
        ram_pool &= ~(4096 - 1);
        cdram_pool &= ~(256 * 1024 - 1);
        std::printf("mkwv: vitaGL pools ram=%d KiB cdram=%d KiB circular=%d KiB\n", ram_pool >> 10, cdram_pool >> 10,
                    circular >> 10);
        // Scenes per frame for FBO render targets (vitaGL's default is 1). The EFB is one FBO and
        // every EFB copy ends and restarts its GXM scene: a race frame runs ~10-15 scenes on it.
        // Beyond the declared count the GPU has to wait for the render target's earlier scenes
        // (and Vita3K warns "Render Target is using more scenes per frame than what was
        // planned!" - static47's crash followed that warning). 8 is libgxm's maximum.
        vglSetupRenderTargetScenesNum(1, 8);
        const GLboolean clamped = vglInitWithCustomSizes(2 * kMiB, kDisplayW, kDisplayH, ram_pool, cdram_pool,
                                                         1 * kMiB, 0, SCE_GXM_MULTISAMPLE_NONE);
        vglWaitVblankStart(GL_TRUE);
        glViewport(0, 0, kDisplayW, kDisplayH);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        // Processor verts are already GX NDC. Identity MVP leaves them in clip
        // space. GLES clips -w<=z<=w, so GX near z=-1 / far z=0 is inside the
        // volume without the old GXM z+1 remap.
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glFrontFace(GL_CW);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        efb_target_init();
        setup_vertex_memory();
        set_batch_flush(&flush_batch);
        ready_ = true;
        std::printf("mkwv: vitaGL GLES ready %dx%d clamped=%d\n", kDisplayW, kDisplayH, clamped ? 1 : 0);
        return true;
    }

    void shutdown() override {
        ready_ = false;
    }

    void begin_frame() override {
        if (!ready_) {
            return;
        }
        begin_efb_frame(0xFF381A14u);  // the demo's dark blue
    }

    void draw_debug_geometry() override {
        if (!ready_) {
            return;
        }
        static unsigned frames = 0;
        render_demo(static_cast<float>(frames++) / 60.0f);
    }

    void present_processor(const Processor& processor) override {
        if (!ready_) {
            return;
        }
        if (processor.draw_count == 0 && !frame_submit_open()) {
            // Only reached for a drain that carries EFB copies (hle_present_frame drops other
            // empty drains). Clearing or swapping here would destroy the very image the copy
            // is about to read, which is on the front buffer.
            submit_efb_copies_only(processor);
            return;
        }
        if (!frame_submit_open()) {
            clear_frame();
        }
        maybe_capture_3d(processor);
        {
            perf::Scope timing(perf::kSubmit);
            submit_processor(processor);
        }
        if (g_defer_swap) {
            g_swap_pending = true;
            return;
        }
        do_swap();
        end_submit_frame();
    }

    void end_frame() override {
        if (!ready_) {
            return;
        }
        present_efb();
        vglSwapBuffers(GL_FALSE);
    }

private:
    bool ready_ = false;
};

} // namespace

void set_deferred_swap(bool deferred) {
    g_defer_swap = deferred;
}

bool complete_deferred_swap() {
    if (!g_swap_pending) {
        return false;
    }
    g_swap_pending = false;
    do_swap();
    end_submit_frame();
    return true;
}

void set_vertex_ring_enabled(bool on) {
    g_ring_wanted = on;
}

void set_dl_cache_memory(bool on) {
    g_dl_pool_wanted = on;
}

void set_vsync(bool on) {
    vglWaitVblankStart(on ? GL_TRUE : GL_FALSE);
}

Backend* create_gl_backend() {
    return new (std::nothrow) GlBackend();
}

} // namespace mkwv::gx
