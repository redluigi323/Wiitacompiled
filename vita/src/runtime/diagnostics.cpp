#include "mkwv/runtime/diagnostics.hpp"

#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/guest_access.hpp"
#include "mkwv/gx/submit.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/hle_os.hpp"

#include "abi_bridge.h"
#include "recomp_mod_loader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__vita__)
#include <psp2/io/fcntl.h>
#include <psp2/types.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace mkwv::runtime {
namespace {

unsigned g_bad_reports = 0;
std::uint32_t g_bad_total = 0;

void on_bad_access(bool write, std::uint32_t guest, std::size_t length) {
    ++g_bad_total;
    if (g_bad_reports >= 48u) {
        return;
    }
    ++g_bad_reports;
    std::printf("mkwv: BAD %s guest=0x%08X len=%u near=0x%08X\n", write ? "write" : "read", guest,
                static_cast<unsigned>(length), RecompMod::g_currentTranslatedExecutionAddress);
    std::fflush(stdout);
}

#if defined(__vita__)
// Raw writes only: stdio may be locked by the (possibly stuck) guest thread, and the point of the
// watchdog is to say something even then.
void raw_log(const char* text, int len) {
    static SceUID tty = -2;
    if (tty == -2) {
        tty = sceIoOpen("tty0:", SCE_O_WRONLY, 0);
    }
    if (tty >= 0) {
        sceIoWrite(tty, text, static_cast<SceSize>(len));
    }
    // Truncate once per process so a run's log is only that run. The file was appended across every
    // launch, which made it easy to read a stale run's numbers by mistake.
    static bool truncated = false;
    if (!truncated) {
        truncated = true;
        const SceUID reset = sceIoOpen("ux0:data/viicompiled/watchdog.log",
                                       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (reset >= 0) {
            sceIoClose(reset);
        }
    }
    const SceUID fd = sceIoOpen("ux0:data/viicompiled/watchdog.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, text, static_cast<SceSize>(len));
        sceIoClose(fd);
    }
}

std::uint32_t rd(std::uint32_t addr) {
    CompactMemory* mem = guest_memory();
    return mem != nullptr ? mem->read32(addr) : 0u;
}

// StrapScene state dump: finds the scene by scanning MEM1 for the task-thread pointer stored at
// scene+3192, then prints the counters/floats StrapScene::calc gates on.
std::uint32_t g_scene = 0;

// func_80242C98 (TaskThread::isBusy) scans `count` job slots of 24 bytes and reports busy if any
// slot's callback word is non-zero. StrapScene::calc will not advance while that is true, so the
// slots are printed verbatim here.
void append_task(char* buf, int* n, int cap, const char* label, std::uint32_t tt) {
    if (tt == 0 || *n >= cap - 80) {
        return;
    }
    const std::uint32_t array = rd(tt + 76u);
    const std::uint32_t count = rd(tt + 80u);
    *n += std::snprintf(buf + *n, static_cast<std::size_t>(cap - *n), " | %s=%08X arr=%08X n=%u cur=%08X doneq=%08X slots:",
                        label, tt, array, count, rd(tt + 0x48u), rd(tt + 0x54u));
    if (array == 0 || count > 16u) {
        return;
    }
    for (std::uint32_t i = 0; i < count && *n < cap - 32; ++i) {
        *n += std::snprintf(buf + *n, static_cast<std::size_t>(cap - *n), " %08X", rd(array + i * 24u));
    }
}

void dump_state(unsigned tick) {
    const char* rel = rel_boot_debug();
    raw_log(rel, static_cast<int>(std::strlen(rel)));
    const std::uint32_t ripper = rd(0x80386000u);
    const std::uint32_t tt = ripper != 0 ? rd(ripper + 232u) : 0u;
    if (last_calc_scene() != 0) {
        g_scene = last_calc_scene();
    } else if (g_scene == 0 && tt != 0 && tick >= 1u) {
        for (std::uint32_t a = 0x80000000u; a < 0x81800000u; a += 4u) {
            if (rd(a) != tt) {
                continue;
            }
            const std::uint32_t s = a - 3192u;
            if (s < 0x80004000u || (s >= ripper && s < ripper + 4352u)) {
                continue;
            }
            const std::uint32_t frames = rd(s + 3264u);
            if (rd(s + 3184u) >= 0x80000000u && frames < 1000000u) {
                g_scene = s;
                break;
            }
        }
    }
    // The cached strap address can be freed/reused once scene 1 starts.
    // Only interpret the strap-specific offsets while its vtable is present.
    const bool strap_alive = g_scene != 0 && rd(g_scene) == 0x802585B0u;
    char buf[1200];
    int n = std::snprintf(buf, sizeof(buf), "mkwv: DUMP ripper=%08X tt=%08X scene=%08X", ripper, tt, g_scene);
    if (strap_alive) {
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                           " | s+3184=%08X +3188=%08X +3192=%08X +3264=%u +3268=%u +3272=%u +3276=%08X", rd(g_scene + 3184u),
                           rd(g_scene + 3188u), rd(g_scene + 3192u), rd(g_scene + 3264u), rd(g_scene + 3268u),
                           rd(g_scene + 3272u), rd(g_scene + 3276u));
    }
    if (ripper != 0) {
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                           " | r+96=%08X +112=%08X +124=%08X +172=%08X +180=%08X +232=%08X tt+76=%08X tt+80=%u", rd(ripper + 96u),
                           rd(ripper + 112u), rd(ripper + 124u), rd(ripper + 172u), rd(ripper + 180u), rd(ripper + 232u),
                           tt != 0 ? rd(tt + 76u) : 0u, tt != 0 ? rd(tt + 80u) : 0u);
    }
    const std::uint32_t r2 = 0x8038EFA0u;
    n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                       " | sda2 -32768=%08X -32764=%08X -32752=%08X:%08X mgr=%08X", rd(r2 - 32768u), rd(r2 - 32764u),
                       rd(r2 - 32752u), rd(r2 - 32748u), rd(0x80385FC8u));
    const std::uint32_t mgr = rd(0x80385FC8u);
    std::uint32_t sm = 0;
    if (mgr != 0) {
        sm = rd(mgr + 84u);
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n), " sm=%08X sm+20=%08X sm+16=%08X sm+24=%08X",
                           sm, rd(sm + 20u), rd(sm + 16u), rd(sm + 24u));
    }
    // StrapScene::calc's advance path (loc_80007B3C): a virtual call on read32(0x802A4080+96) via
    // vtable+40, then func_8000A3F8(read32(0x80385FF8), 1), which is a no-op when read32(base+32) is
    // null. Everything it touches is printed so a silent no-advance can be told from a failed gate.
    const std::uint32_t mgr96 = rd(0x802A4080u + 96u);
    const std::uint32_t mgr96_vt = mgr96 != 0 ? rd(mgr96) : 0u;
    const std::uint32_t base = rd(0x80385FF8u);
    const std::uint32_t obj = base != 0 ? rd(base + 32u) : 0u;
    n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                       " | adv: mgr96=%08X vt=%08X fn40=%08X base=%08X obj=%08X fn52=%08X", mgr96, mgr96_vt,
                       mgr96_vt != 0 ? rd(mgr96_vt + 40u) : 0u, base, obj, obj != 0 ? rd(obj + 52u) : 0u);
    append_task(buf, &n, static_cast<int>(sizeof(buf)), "tt", tt);
    if (strap_alive) {
        append_task(buf, &n, static_cast<int>(sizeof(buf)), "sceneTT", rd(g_scene + 3192u));
    }
    if (n > 0 && n < static_cast<int>(sizeof(buf)) - 4) {
        buf[n++] = '\n';
        raw_log(buf, n);
    }
    if (sm != 0) {
        const std::uint32_t factory = rd(sm + 4u);
        const std::uint32_t sc12 = rd(sm + 12u);
        const std::uint32_t sc36 = rd(sm + 36u);
        const std::uint32_t fac_vt = factory != 0 ? rd(factory) : 0u;
        const std::uint32_t s12_vt = sc12 != 0 ? rd(sc12) : 0u;
        const std::uint32_t s36_vt = sc36 != 0 ? rd(sc36) : 0u;
        n = std::snprintf(buf, sizeof(buf),
                          "mkwv: SMX fac=%08X facvt=%08X create=%08X sm12=%08X vt=%08X fn12=%08X sm32=%08X sm36=%08X vt=%08X fn24=%08X sm44=%08X globevt=%08X\n",
                          factory, fac_vt, fac_vt != 0 ? rd(fac_vt + 8u) : 0u, sc12, s12_vt, s12_vt != 0 ? rd(s12_vt + 12u) : 0u,
                          rd(sm + 32u), sc36, s36_vt, s36_vt != 0 ? rd(s36_vt + 24u) : 0u, rd(sm + 44u), rd(0x808B2D1Cu));
        if (n > 0) {
            raw_log(buf, n);
        }
    }
}

int watchdog_main(SceSize, void*) {
    std::uint32_t last_count = dispatch_count();
    unsigned tick = 0;
    for (;;) {
        sceKernelDelayThread(3u * 1000u * 1000u);
        ++tick;
        const std::uint32_t count = dispatch_count();
        char buf[640];
        std::uint32_t frames = 0, draws = 0, verts = 0, fifo = 0, max_draws = 0, skipped = 0;
        hle_frame_stats(&frames, &draws, &verts, &fifo, &max_draws, &skipped);
        int n = std::snprintf(buf, sizeof(buf),
                              "mkwv: HB t=%us dispatches=%u (+%u) frames=%u skipped=%u draws=%u verts=%u fifo=%u maxdraws=%u near=0x%08X bad=%u missing=%u lastmiss=%08X recent:",
                              tick * 3u, count, count - last_count, frames, skipped, draws, verts, fifo, max_draws,
                              RecompMod::g_currentTranslatedExecutionAddress, g_bad_total, missing_target_count(),
                              last_missing_target());
        for (unsigned i = 0; i < 12u && n > 0 && n < 560; ++i) {
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n), " %08X", recent_dispatch(i));
        }
        if (n > 0 && n < 620) {
            buf[n++] = '\n';
            raw_log(buf, n);
        }
        // Distinct unresolved call targets, hottest first. missing= above only counts how
        // often dispatch fell through; this says which addresses need HLE natives.
        {
            std::uint32_t miss_addrs[24];
            std::uint32_t miss_hits[24];
            const unsigned used = missing_histogram(miss_addrs, miss_hits, 24u);
            if (used != 0u) {
                int m = std::snprintf(buf, sizeof(buf), "mkwv: MISS distinct=%u", used);
                for (unsigned i = 0; i < used && m > 0 && m < 600; ++i) {
                    m += std::snprintf(buf + m, sizeof(buf) - static_cast<std::size_t>(m), " %08X:%u",
                                       miss_addrs[i], miss_hits[i]);
                }
                if (m > 0 && m < 630) {
                    buf[m++] = '\n';
                    raw_log(buf, m);
                }
            }
        }
        // Graphics diagnostics, back on for the UV investigation. One line each per heartbeat
        // (every 3s), not the per-frame FIFO/vertex spam the old dumps produced.
        {
            std::uint32_t lyt_emitted = 0;
            std::uint32_t lyt_skipped = 0;
            lyt_quad_stats(&lyt_emitted, &lyt_skipped);
            const int m = std::snprintf(buf, sizeof(buf), "mkwv: LYT quads=%u dropped=%u\n", lyt_emitted,
                                        lyt_skipped);
            if (m > 0) {
                raw_log(buf, m);
            }
            // What DrawQuad was handed, and the first texcoord it read back out of the guest's
            // table. uv0=[0.000 0.000] with a plausible tex pointer means the offsets in
            // emit_lyt_draw_quad are wrong; a null tex pointer means lyt never supplied any.
            if (lyt_emitted != 0u) {
                const int t = lyt_trace_line(buf, sizeof(buf));
                if (t > 0) {
                    raw_log(buf, t);
                }
            }
        }
        {
            const char* sd = gx::last_submit_debug();
            raw_log(sd, static_cast<int>(std::strlen(sd)));
        }
        if (const char* dd = hle_take_draw_dump()) {
            raw_log(dd, static_cast<int>(std::strlen(dd)));
            raw_log("\n", 1);
        }
        dump_state(tick);
        last_count = count;
        // Keep reporting through races: menu navigation alone often takes more
        // than two minutes, so the old 41-tick cutoff lost all race diagnostics.
    }
    return 0;
}
#endif

} // namespace

void watchdog_log_line(const char* text) {
    if (text == nullptr) {
        return;
    }
#if defined(__vita__)
    raw_log(text, static_cast<int>(std::strlen(text)));
#else
    std::fputs(text, stderr);
#endif
}

void start_diagnostics() {
    set_bad_access_hook(on_bad_access);
#if defined(__vita__)
    const SceUID tid = sceKernelCreateThread("mkwv_watchdog", watchdog_main, 0x10000100, 0x4000, 0, 0, nullptr);
    if (tid >= 0) {
        sceKernelStartThread(tid, 0, nullptr);
    }
#endif
}

} // namespace mkwv::runtime
