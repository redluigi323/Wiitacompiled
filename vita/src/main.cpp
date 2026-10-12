#include "mkwv/gx/backend.hpp"
#include "mkwv/gx/cg_emit.hpp"
#include "mkwv/gx/gpu_mesh.hpp"
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/submit.hpp"
#include "mkwv/gx/vertex_worker.hpp"
#include "mkwv/gx/vertex_tuning.hpp"
#include "mkwv/platform/clocks.hpp"
#include "mkwv/platform/data_root.hpp"
#include "mkwv/platform/render_thread.hpp"
#include "mkwv/platform/audio_out.hpp"
#include "mkwv/platform/profiler.hpp"
#include "mkwv/platform/sound_thread.hpp"
#include "mkwv/runtime/guest_prof.hpp"
#include "mkwv/runtime/hle_audio.hpp"
#include "mkwv/runtime/game_options.hpp"
#include "mkwv/runtime/engine_native.hpp"
#include "mkwv/runtime/host_context.hpp"
#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/diagnostics.hpp"
#include "mkwv/runtime/runtime.hpp"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>

#include <cstdarg>
#include <cstdio>
#include <memory>

extern "C" const char* mkwv_guest_ram_placement();

namespace {

std::FILE* g_boot_log = nullptr;
mkwv::gx::Backend* g_gx = nullptr;

// ux0 does not truncate boot.log on open - neither fopen("w") nor SCE_O_TRUNC does, at least under
// Vita3K - so a run that logs fewer bytes than the last one leaves the previous run's tail in
// place and it reads as extra lines that never happened. Every write therefore leaves an end
// marker behind it and rewinds over the marker on the next write: the file always ends with the
// marker, and anything past it is visibly stale.
constexpr const char* kBootLogEnd = "mkwv: --- end of this run's log ---\n";

void boot_log(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::printf("%s", line);
    if (g_boot_log != nullptr) {
        std::fputs(line, g_boot_log);
        const long mark = std::ftell(g_boot_log);
        std::fputs(kBootLogEnd, g_boot_log);
        std::fflush(g_boot_log);
        if (mark >= 0) {
            std::fseek(g_boot_log, mark, SEEK_SET);
        }
    }
}

void log_free_memory(const char* when) {
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) >= 0) {
        boot_log("mkwv: free memory (%s) user=%u KiB cdram=%u KiB phycont=%u KiB\n", when,
                 static_cast<unsigned>(info.size_user) >> 10, static_cast<unsigned>(info.size_cdram) >> 10,
                 static_cast<unsigned>(info.size_phycont) >> 10);
    }
}

void present_guest(mkwv::gx::Processor& processor) {
    if (g_gx != nullptr) {
        g_gx->present_processor(processor);
    }
}

} // namespace

// Runtime switches: an empty file with this name next to the game data (ux0:data/viicompiled/).
bool marker_present(const char* name) {
    char path[256];
    std::snprintf(path, sizeof(path), "%s/%s", mkwv::platform::kDataRoot, name);
    const SceUID probe = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (probe >= 0) {
        sceIoClose(probe);
        return true;
    }
    return false;
}

int main() {
    const mkwv::platform::Clocks clocks = mkwv::platform::apply_performance_clocks();
    // Explicit quality-for-throughput preset. Keep real game/render frames (no frame skip).
    const bool perf_turbo = marker_present("perf_turbo");
    // static73: perf_turbo used to switch both profilers off. `prof` keeps them on with it, so a
    // profiled run measures the same configuration that is actually played.
    const bool profilers = (!perf_turbo || marker_present("prof"));
    mkwv::runtime::set_native_engine(!marker_present("no_native_engine"));
    mkwv::gx::set_prepared_texgen(!marker_present("no_prepared_texgen"));

    // A rebuild here costs a full shard pass, so the two renderer paths are switchable
    // from the same VPK: drop an empty file named no_shaders next to the game data and the
    // compiled-TEV path is skipped entirely, leaving the fixed-function GL_COMBINE path
    // that shipped before it. Delete the file to switch back.
    if (marker_present("no_shaders")) {
        mkwv::gx::set_shader_path_enabled(false);
    }
    // Effects level (EFB copies used for post-processing): fx_low skips the ones that need a
    // CPU readback, fx_off skips every copy that is not a plain colour snapshot.
    if (marker_present("no_vertex_reuse")) mkwv::gx::set_vertex_reuse(false);
    if (marker_present("no_merge")) {
        mkwv::gx::set_draw_merging(false);
    }
    // The object-space mesh cache stays off. static52 served the menu from it and the
    // lookup (a 512KB fingerprint per draw) cost more than the CPU transform it replaced.
    // Guest sampling profiler (PROF lines in the watchdog log); no_prof turns it off.
    if (profilers && !marker_present("no_prof")) {
        mkwv::runtime::prof::register_sampled_thread();  // this thread runs the game
        mkwv::platform::start_guest_profiler();
    }
    // Race depth of field (EGG::DrawPathDOF): off like upstream; `race_dof` puts it back.
    mkwv::runtime::set_race_dof(marker_present("race_dof"));
    mkwv::runtime::set_exp_heap_checks(marker_present("heap_check"));
    mkwv::gx::set_race_probe_sheet(marker_present("race_sheet"));
    if (perf_turbo || marker_present("fx_off")) {
        mkwv::gx::set_effects_level(0);
    } else if (marker_present("fx_low")) {
        mkwv::gx::set_effects_level(1);
    }
    // Real-hardware budget (static60). The Vita's GPU is ALU-bound on GX-exact TEV arithmetic,
    // so fragment programs use the fast float combiner unless exact_tev exists. efb_low renders
    // the EFB at GX's own 456 lines (30% fewer pixels), efb_lower at 342 (60% fewer); both are
    // upscaled to the panel at present.
    mkwv::gx::set_tev_exact(marker_present("exact_tev"));
    // static63: GX lighting in the vertex shader (the CPU only decodes and transforms lit
    // vertices); cpu_lighting keeps it on the CPU.
    mkwv::gx::set_gpu_lighting(!marker_present("cpu_lighting"));
    // static70: the GPU transform. DL cache hits are drawn straight from a GPU copy of the cache
    // with a vertex shader doing the matrices, texgen and lighting - no vertex jobs for them.
    // no_gpu_xf turns it off (it also needs the DL cache and GPU lighting).
    mkwv::gx::set_gpu_xf(!marker_present("no_gpu_xf") && !marker_present("cpu_lighting") &&
                         !marker_present("no_dl_cache"));
    mkwv::gx::set_gpu_xf_limits((marker_present("xf_no_skin") ? mkwv::gx::kGpuXfNoSkin : 0u) |
                                (marker_present("xf_no_lit") ? mkwv::gx::kGpuXfNoLit : 0u) |
                                (marker_present("xf_no_texgen") ? mkwv::gx::kGpuXfNoTexgen : 0u));
    // static64: display-list vertex cache (decoded model-space vertices kept across frames) and
    // the GPU vertex ring (vertex jobs pack straight into memory the GPU draws from; vitaGL
    // stops copying every draw's vertices). no_dl_cache / no_vbo_ring turn them off.
    {
        const bool dl = !marker_present("no_dl_cache");
        mkwv::gx::set_dl_cache(dl);
        mkwv::gx::set_dl_cache_memory(dl);
        mkwv::gx::set_vertex_ring_enabled(!marker_present("no_vbo_ring"));
        mkwv::gx::vertex_ring_set_staging(!marker_present("direct_vertex_ring"));
    }
    if (perf_turbo || marker_present("efb_lower")) {
        mkwv::gx::set_efb_scale(0.75f);
    } else if (marker_present("efb_low")) {
        mkwv::gx::set_efb_scale(1.0f);
    }
    // nw4r's reverb/effects (translated DSP code on the sound thread, 333 times a second) are
    // off unless `reverb` exists. mix_inline runs the DSP mix inside the audio interrupt again.
    mkwv::runtime::set_audio_reverb(!perf_turbo && marker_present("reverb"));
    mkwv::runtime::set_audio_mix_inline(marker_present("mix_inline"));
    mkwv::runtime::set_audio_voice_cap(perf_turbo || marker_present("snd_voice_cap_low") ? 35u : marker_present("snd_voice_cap") ? 60u : 100u);

    // The boot log opens first. Everything that can fail before the renderer is up used to
    // return from main() silently - on a real Vita that is the app opening and closing again
    // with no error at all - so every step below says what it did.
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(mkwv::platform::kDataRoot, 0777);
    // fopen("w") does not truncate on this path: a shorter run leaves the previous run's tail in
    // place, which reads as extra log lines that never happened. Truncate explicitly first.
    const SceUID boot_reset = sceIoOpen(mkwv::platform::kBootLog, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (boot_reset >= 0) {
        sceIoClose(boot_reset);
    }
    g_boot_log = std::fopen(mkwv::platform::kBootLog, "w");
    boot_log("mkwv: static73-loads perf_turbo=%d profilers=%d native_engine=%d forced_inline=0\n", perf_turbo ? 1 : 0, profilers ? 1 : 0, mkwv::runtime::native_engine_enabled() ? 1 : 0);
    boot_log("mkwv: gpu mesh cache %s\n", mkwv::gx::gpu_mesh_active() ? "on" : "off");
    // static73: compiled shaders are kept by vitaGL (HAVE_SHADER_CACHE); the first run of a scene
    // still compiles, later runs load. Deleting the folder only costs that first-run time again.
    boot_log("mkwv: shader disk cache ux0:data/shader_cache/<titleid>/ (vitaGL)\n");
    boot_log("mkwv: lighting on the %s\n", mkwv::gx::gpu_lighting_enabled() ? "GPU" : "CPU");
    boot_log("mkwv: prepared texgen %s\n", mkwv::gx::prepared_texgen_enabled() ? "on" : "off");
    boot_log("mkwv: dl vertex cache %s, gpu vertex ring %s, gpu transform %s\n", marker_present("no_dl_cache") ? "off" : "on",
             marker_present("no_vbo_ring") ? "off" : "on", mkwv::gx::gpu_xf_enabled() ? "wanted" : "off");
    boot_log("mkwv: tev %s, efb scale %.3f, reverb %s, dsp mix %s\n", mkwv::gx::tev_exact() ? "exact" : "fast",
             static_cast<double>(mkwv::gx::efb_scale()), mkwv::runtime::audio_reverb_enabled() ? "on" : "off",
             marker_present("mix_inline") ? "inline" : "deferred");
    boot_log("mkwv: clocks ARM=%d GPU=%d BUS=%d XBAR=%d arm500=%d\n", clocks.arm_mhz, clocks.gpu_mhz,
             clocks.bus_mhz, clocks.xbar_mhz, clocks.arm_500_accepted ? 1 : 0);
    log_free_memory("start");

    // Static, not automatic: Runtime embeds the GX Processor, whose decode buffers are far
    // larger than the main thread's stack.
    static mkwv::runtime::Runtime runtime;
    if (!runtime.initialize(mkwv::platform::kOverlayDir, mkwv::platform::kGuestDir)) {
        if (!runtime.memory().is_active()) {
            // Which of the guest RAM blocks the system refused, and with what error.
            const struct {
                const char* name;
                unsigned size;
            } blocks[] = {{"MEM1", 24u << 20}, {"MEM2", 64u << 20}, {"Kamek", 2u << 20}};
            for (const auto& b : blocks) {
                const SceUID id = sceKernelAllocMemBlock("mkwv_probe", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, b.size, nullptr);
                boot_log("mkwv: FATAL guest RAM probe %s %u MiB -> %s (0x%08X)\n", b.name, b.size >> 20,
                         id >= 0 ? "ok" : "REFUSED", static_cast<unsigned>(id));
                if (id >= 0) {
                    sceKernelFreeMemBlock(id);
                }
            }
            boot_log("mkwv: FATAL runtime init failed: could not allocate the 90 MiB of guest RAM (%s)\n",
                     mkwv_guest_ram_placement());
        } else {
            boot_log("mkwv: FATAL runtime init failed: cannot write %s (overlay stubs)\n", mkwv::platform::kOverlayDir);
        }
        log_free_memory("at failure");
        // Leave the log readable and the failure visible: hold for a few seconds, then exit.
        sceKernelDelayThread(3 * 1000 * 1000);
        sceKernelExitProcess(1);
        return 1;
    }
    boot_log("mkwv: guest RAM %s\n", mkwv_guest_ram_placement());
    log_free_memory("after guest RAM");
    const auto mem = runtime.memory().stats();
    boot_log("mkwv: guest unique=%u MiB alias=%u MiB\n",
             static_cast<unsigned>(mem.unique_bytes / (1024u * 1024u)),
             static_cast<unsigned>(mem.mapped_alias_bytes / (1024u * 1024u)));
    const auto image = runtime.image();
    if (image.loaded) {
        boot_log("mkwv: guest image sections=%u bytes=%u init=%08x sda r2=%08x r13=%08x\n", image.sections,
                 image.bytes, image.init_word, runtime.cpu().gpr[2], runtime.cpu().gpr[13]);
        if (runtime.tick_probe()) {
            boot_log("mkwv: probe r3=%08x r4=%08x\n", runtime.cpu().gpr[3], runtime.cpu().gpr[4]);
        }
    } else {
        boot_log("mkwv: guest image missing (copy blobs to %s)\n", mkwv::platform::kGuestDir);
    }
    if (std::FILE* fst = std::fopen(mkwv::platform::kFstPath, "rb")) {
        std::fclose(fst);
        boot_log("mkwv: disc fst at %s\n", mkwv::platform::kFstPath);
    } else {
        boot_log("mkwv: disc missing (run scripts/setup-ux0-data.sh)\n");
    }
    if (runtime.tick_stub()) {
        const auto overlay = runtime.overlay().stats();
        boot_log("mkwv: overlay catalog=%u resident=%u hits=%u misses=%u evict=%u\n", overlay.catalog, overlay.resident,
                 overlay.hits, overlay.misses, overlay.evictions);
        boot_log("mkwv: stub gx draws=%u verts=%u fifo=%u\n", runtime.processor().draw_count,
                 runtime.processor().vert_count, runtime.fifo().size());
    } else {
        boot_log("mkwv: stub gx tick failed missing=%u\n", mkwv::runtime::missing_target_count());
    }

    log_free_memory("before vitaGL");
    {
        // vitaGL compiles every shader at runtime through SceShaccCg. On a real Vita that is
        // ur0:data/libshacccg.suprx (extracted with ShaRKF00D); without it nothing can draw.
        SceIoStat st{};
        const bool shacccg = sceIoGetstat("ur0:data/libshacccg.suprx", &st) >= 0 ||
                             sceIoGetstat("ur0:data/external/libshacccg.suprx", &st) >= 0;
        boot_log("mkwv: libshacccg.suprx %s\n", shacccg ? "present" : "MISSING (real Vita: extract it with ShaRKF00D)");
    }
    std::unique_ptr<mkwv::gx::Backend> gx(mkwv::gx::create_gl_backend());
    // Render thread (on by default when the game runs): GX decode, GL and the swap on core 1
    // while the game runs on core 0. no_render_thread turns it off; render_nosync lets the
    // guest run ahead of the renderer instead of waiting for it at each frame boundary.
    bool threaded = false;
    bool gl_ready = false;
#if MKWV_HAVE_TRANSLATED
    if (gx && image.loaded && !marker_present("no_render_thread")) {
        // Default: the game may run one frame ahead of the renderer (render_thread.hpp).
        // render_sync restores the wait at every present; render_nosync never waits.
        const int sync = marker_present("render_nosync") ? mkwv::platform::kSyncNone
                         : marker_present("render_sync") ? mkwv::platform::kSyncFrame
                                                         : mkwv::platform::kSyncAheadOne;
        // The vertex worker (core 2) takes lighting and the transform off the render thread;
        // no_vertex_worker keeps them there.
        const bool worker = !marker_present("no_vertex_worker");
        mkwv::platform::set_single_vertex_worker(marker_present("one_vertex_worker"));
        mkwv::platform::set_vertex_workers_low(marker_present("workers_low"));
        const int rt = mkwv::platform::start_render_thread(gx.get(), sync, worker);
        threaded = rt == 1;
        gl_ready = rt == 1;
        if (rt == 0) {
            boot_log("mkwv: vitaGL GLES backend init failed (render thread)\n");
            sceKernelExitProcess(1);
            return 1;
        }
        boot_log("mkwv: render thread %s%s%s\n", threaded ? "on" : "unavailable, rendering on the guest thread",
                 threaded ? (sync == mkwv::platform::kSyncFrame ? " (sync at frame end)"
                             : sync == mkwv::platform::kSyncAheadOne ? " (one frame ahead)" : " (nosync)") : "",
                 threaded && worker ? ", vertex worker on cores 0-2" : "");
    }
#endif
    if (!gl_ready && (!gx || !gx->initialize())) {
        boot_log("mkwv: vitaGL GLES backend init failed\n");
        sceKernelExitProcess(1);
        return 1;
    }
    if (marker_present("no_vsync")) {
        mkwv::gx::set_vsync(false);
        boot_log("mkwv: vsync off\n");
    }
    // Frame skipping (hacky, for speed): frameskip draws every second frame, frameskip2 every
    // third. The game logic still runs every frame, so it plays closer to full speed.
    if (marker_present("frameskip2")) {
        mkwv::runtime::set_frameskip(2);
        boot_log("mkwv: frameskip 2\n");
    } else if (marker_present("frameskip")) {
        mkwv::runtime::set_frameskip(1);
        boot_log("mkwv: frameskip 1\n");
    }
    // The render thread starts on a frame's commands while the game is still issuing them
    // (8 KiB at a time); no_early_handoff hands them over only per 64 KiB buffer and at the
    // frame end, as before.
    if (marker_present("no_early_handoff")) {
        mkwv::runtime::set_render_handoff_threshold(0);
    }
    boot_log("mkwv: vitaGL GLES ready\n");
    boot_log("mkwv: GPU transform %s (limits %u)\n", mkwv::gx::gpu_xf_enabled() ? "on" : "off", mkwv::gx::gpu_xf_limits());
    log_free_memory("after vitaGL");
    g_gx = gx.get();
    runtime.set_present(present_guest);

    // Audio: the AX/DSP HLE plays through a 32 kHz BGM port. nw4r's sound thread and its
    // stream-loading thread run as cooperative fibers on this (the guest's) thread.
    {
        static void* s_main_context = nullptr;
        if (mkwv::runtime::HostContext::initialize_scheduler(&s_main_context)) {
            mkwv::runtime::FiberOps ops{};
            ops.create = [](std::size_t bytes, void (*entry)(void*), void* arg) -> void* {
                return mkwv::runtime::HostContext::create(bytes, entry, arg);
            };
            ops.switch_to = [](void* fiber) { mkwv::runtime::HostContext::switch_to(fiber); };
            ops.main = s_main_context;
            mkwv::runtime::set_fiber_ops(ops);
        }
        mkwv::runtime::set_audio_coef_rom("app0:dsp_coef.bin");
        if (marker_present("no_audio")) {
            mkwv::runtime::set_audio_enabled(false);
            boot_log("mkwv: audio off (no_audio)\n");
        } else {
            boot_log("mkwv: audio out %s\n", mkwv::platform::start_audio_out() ? "32 kHz" : "FAILED");
            mkwv::runtime::set_audio_out_stats(&mkwv::platform::audio_out_stats);
            // The audio interrupt and nw4r's threads on their own host thread (guest_sync.hpp);
            // no_snd_thread keeps them on the game thread as fibers polled at frame points.
            if (!marker_present("no_snd_thread")) {
                const bool share = marker_present("snd_share");
                mkwv::platform::install_sound_thread(share);
                boot_log("mkwv: sound thread requested (%s)\n",
                         share ? "cores 1-2, above the renderer" : "core 2, above the vertex worker");
            }
        }
    }
    // static65: host-PC sampling of the game, render, worker and sound threads (HPROF lines).
    if (profilers && !marker_present("no_hprof")) {
        boot_log("mkwv: host profiler %s\n", mkwv::platform::start_host_profiler() ? "on" : "unavailable");
    }
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    // Whether the guest was ever handed control. Only a build with no guest image should fall
    // through to the demo scene below.
    bool guest_started = false;
#if MKWV_HAVE_TRANSLATED
    if (image.loaded) {
        guest_started = true;
        mkwv::runtime::start_diagnostics();
        boot_log("mkwv: entering __start\n");
        const bool started = runtime.tick_start();
        boot_log("mkwv: __start ok=%d r3=%08x lr=%08x missing=%u last=%08x\n", started ? 1 : 0, runtime.cpu().gpr[3],
                 runtime.cpu().lr, mkwv::runtime::missing_target_count(), mkwv::runtime::last_missing_target());
        // tick_start() is synchronous, so reaching this line at all means the guest's __start
        // returned - the game is over, not running. That used to be invisible: the loop below
        // simply started drawing the demo scene over it. Say so where the log will be read.
        char line[192];
        std::snprintf(line, sizeof(line),
                      "mkwv: __START RETURNED ok=%d r3=%08X lr=%08X missing=%u last=%08X - guest is no longer running\n",
                      started ? 1 : 0, runtime.cpu().gpr[3], runtime.cpu().lr,
                      mkwv::runtime::missing_target_count(), mkwv::runtime::last_missing_target());
        mkwv::runtime::watchdog_log_line(line);
    }
#endif

    for (;;) {
        if (guest_started) {
            // The guest ran and returned. Hold the last frame it presented rather than painting
            // the demo scene over it, which looks like a working renderer and hides the exit.
            sceKernelDelayThread(16000);
            continue;
        }
        gx->begin_frame();
        gx->draw_debug_geometry();
        gx->end_frame();
    }

    g_gx = nullptr;
    gx->shutdown();
    runtime.shutdown();
    if (g_boot_log != nullptr) {
        std::fclose(g_boot_log);
        g_boot_log = nullptr;
    }
    sceKernelExitProcess(0);
    return 0;
}
