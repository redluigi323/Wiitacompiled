#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/diagnostics.hpp"
#include "mkwv/runtime/guest_prof.hpp"
#include "mkwv/runtime/guest_sync.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/overlay.hpp"
#include "mkwv/gx/capture.hpp"

#include "abi_bridge.h"
#include "memory.h"

#include <atomic>
#include <cstdio>
#include <string_view>

namespace mkwv::runtime {
namespace {

OverlayCache* g_overlay = nullptr;
std::uint32_t g_missing = 0;
std::uint32_t g_last_missing = 0;

// Distinct missing dispatch targets with hit counts. `lastmiss` alone only ever names one address,
// which turns closing these into whack-a-mole: one full shard rebuild per address. The histogram
// gives the complete set in a single run.
constexpr unsigned kMissSlots = 24;
std::uint32_t g_miss_addr[kMissSlots]{};
std::uint32_t g_miss_hits[kMissSlots]{};
unsigned g_miss_used = 0;
std::uint32_t g_dispatch_count = 0;
std::uint32_t g_ring[64]{};
std::uint32_t g_calc_scene = 0;
unsigned g_invalid_indirect_reports = 0;

struct DispatchSlot {
    std::uint32_t address = 0;
    TranslatedFn fn = nullptr;
    bool translated = false;
};
constexpr unsigned kDispatchCacheSize = 4096;  // power of two
DispatchSlot g_dcache[kDispatchCacheSize];
std::uint32_t g_dcache_misses = 0;
// A second guest host thread (the audio thread) gets its own cache: a slot is two words and a
// flag, and a torn slot would call the wrong function. It is cleared lazily when the shared
// generation moves (clear_dispatch_cache).
DispatchSlot g_dcache_audio[kDispatchCacheSize];
std::atomic<std::uint32_t> g_dcache_gen{1};
std::uint32_t g_dcache_audio_gen = 0;

DispatchSlot* thread_cache(unsigned slot) {
    if (slot == 0) {
        return g_dcache;
    }
    const std::uint32_t gen = g_dcache_gen.load(std::memory_order_acquire);
    if (g_dcache_audio_gen != gen) {
        for (unsigned i = 0; i < kDispatchCacheSize; ++i) g_dcache_audio[i] = DispatchSlot{};
        g_dcache_audio_gen = gen;
    }
    return g_dcache_audio;
}

// Restores the calling thread's current guest context on the way out.
struct GuestCtxScope {
    unsigned slot;
    CpuContext* saved;
    GuestCtxScope(unsigned s, CpuContext* ctx) : slot(s), saved(guest_ctx_slot(s)) { guest_ctx_slot(s) = ctx; }
    ~GuestCtxScope() { guest_ctx_slot(slot) = saved; }
};

void report_invalid_indirect(std::uint32_t address, CpuContext* ctx) {
    if (ctx == nullptr || g_invalid_indirect_reports >= 16u || (address >= 0x80000000u && address < 0x81800000u)) {
        return;
    }
    const std::uint32_t receiver = ctx->gpr[29];
    const bool receiver_in_mem1 = receiver >= 0x80000000u && receiver < 0x81800000u;
    const std::uint32_t vtable = receiver_in_mem1 ? Memory::Read32(receiver) : 0u;
    char line[192];
    std::snprintf(line, sizeof(line),
                  "mkwv: invalid indirect target=%08X pc=%08X lr=%08X r3=%08X r4=%08X r12=%08X r29=%08X vt=%08X ctr=%08X\n",
                  address, ctx->pc, ctx->lr, ctx->gpr[3], ctx->gpr[4], ctx->gpr[12], receiver, vtable, ctx->ctr);
    watchdog_log_line(line);
    ++g_invalid_indirect_reports;
}

} // namespace

void bind_dispatch(OverlayCache* overlay) {
    g_overlay = overlay;
    clear_dispatch_cache();
    g_missing = 0;
    g_last_missing = 0;
    g_invalid_indirect_reports = 0;
}

void unbind_dispatch() {
    g_overlay = nullptr;
}

void bind_static_dispatch(const void* table) {
    RegisterStaticIndirectDispatchTable(static_cast<const StaticIndirectDispatchTable*>(table));
}

void note_missing_target(std::uint32_t address) {
    ++g_missing;
    g_last_missing = address;
    for (unsigned i = 0; i < g_miss_used; ++i) {
        if (g_miss_addr[i] == address) {
            ++g_miss_hits[i];
            return;
        }
    }
    if (g_miss_used < kMissSlots) {
        g_miss_addr[g_miss_used] = address;
        g_miss_hits[g_miss_used] = 1;
        ++g_miss_used;
    }
}

bool invoke_guest(std::uint32_t address, CpuContext* ctx) {
    // RaceScene::OnCalc (upstream/projects/mkwii/MAP.txt). Give scene setup a
    // few updates, then capture a race frame independently of the menu capture.
    if (address == 0x80554E6Cu && ctx != nullptr) {
        static unsigned race_updates = 0;
        static bool race_captured = false;
        if (!race_captured && ++race_updates >= 5u && !gx::capture_armed() && !gx::capture_active()) {
            gx::capture_arm("ux0:data/viicompiled/capture_auto_race");
            race_captured = true;
            watchdog_log_line("mkwv: RaceScene::OnCalc auto race capture armed\n");
        }
    }
    g_ring[g_dispatch_count & 63u] = address;
    if (address == 0x800079D0u) {
        g_calc_scene = ctx->gpr[3];
    }
    ++g_dispatch_count;
    report_invalid_indirect(address, ctx);
    if (ctx == nullptr || address == 0) {
        note_missing_target(address);
        // A leftover r3 from the caller becomes a fake scene/object pointer
        // and then every frame's vtable call dispatches to zero again.
        if (ctx != nullptr) {
            ctx->gpr[3] = 0;
        }
        return false;
    }
    // Resolved targets are cached: this path runs for every virtual call, every function
    // pointer and every HLE native not bound at compile time - thousands of times a frame -
    // and resolving meant scanning the 576-entry overlay cache linearly, then a binary search
    // of the natives, then the paged static table.
    const unsigned thread_slot = guest_thread_slot();
    GuestCtxScope guest_ctx(thread_slot, ctx);
    DispatchSlot& slot = thread_cache(thread_slot)[(address >> 2) & (kDispatchCacheSize - 1u)];
    if (slot.address != address || slot.fn == nullptr) {
        if (g_overlay != nullptr) {
            // Overlay entries (bring-up stubs) stay uncached: the overlay cache has its own
            // residency and eviction.
            TranslatedFn fn = nullptr;
            {
                // The overlay cache has its own residency state; two guest threads may miss at once.
                static HostSpinLock s_overlay_lock;
                HostSpinGuard guard(s_overlay_lock);
                fn = g_overlay->resolve(address);
            }
            if (fn != nullptr) {
                ctx->pc = address;
                CpuContextScope scope(ctx);
                prof::push(thread_slot, address, &ctx->lr);
                fn(ctx);
                prof::pop(thread_slot);
                return true;
            }
        }
        if (const TranslatedFn native = find_hle_native(address)) {
            slot.address = address;
            slot.fn = native;
            slot.translated = false;
        } else {
            const RawDispatchRecord* record = FindStaticIndirectDispatchEntry(
                g_publishedStaticIndirectDispatchTable.load(std::memory_order_acquire), address);
            if (record == nullptr || record->entry == nullptr) {
                note_missing_target(address);
                return false;
            }
            slot.address = address;
            slot.fn = record->entry;
            slot.translated = true;
        }
        ++g_dcache_misses;
    }
    ctx->pc = address;
    // Shadow stack for the sampling profiler (mkwv/runtime/guest_prof.hpp): two stores.
    prof::push(thread_slot, address, &ctx->lr);
    if (slot.translated) {
        const TranslatedFn fn = slot.fn;
        RecompMod::ScopedTranslatedExecutionAddress translated(address);
        CpuContextScope scope(ctx);
        fn(ctx);
    } else {
        const TranslatedFn fn = slot.fn;
        CpuContextScope scope(ctx);
        fn(ctx);
    }
    prof::pop(thread_slot);
    return true;
}

void clear_dispatch_cache() {
    for (auto& slot : g_dcache) {
        slot = DispatchSlot{};
    }
    g_dcache_gen.fetch_add(1, std::memory_order_acq_rel);
}

void use_private_dispatch_cache() {
    g_dcache_audio_gen = 0;  // (the audio thread's cache is picked by guest_thread_slot())
}

std::uint32_t dispatch_cache_misses() {
    return g_dcache_misses;
}

std::uint32_t dispatch_count() {
    return g_dispatch_count;
}

std::uint32_t last_calc_scene() {
    return g_calc_scene;
}

std::uint32_t recent_dispatch(unsigned back) {
    return g_ring[(g_dispatch_count - 1u - back) & 63u];
}

unsigned missing_histogram(std::uint32_t* addrs, std::uint32_t* hits, unsigned cap) {
    const unsigned n = g_miss_used < cap ? g_miss_used : cap;
    for (unsigned i = 0; i < n; ++i) {
        addrs[i] = g_miss_addr[i];
        hits[i] = g_miss_hits[i];
    }
    // Hottest first: the table is small (kMissSlots), so an insertion sort is plenty and keeps the
    // most-wanted native at the front of a log line that may get truncated.
    for (unsigned i = 1; i < n; ++i) {
        const std::uint32_t a = addrs[i];
        const std::uint32_t h = hits[i];
        unsigned j = i;
        while (j != 0 && hits[j - 1] < h) {
            addrs[j] = addrs[j - 1];
            hits[j] = hits[j - 1];
            --j;
        }
        addrs[j] = a;
        hits[j] = h;
    }
    return n;
}

std::uint32_t missing_target_count() {
    return g_missing;
}

std::uint32_t last_missing_target() {
    return g_last_missing;
}

} // namespace mkwv::runtime

void RegisterStaticIndirectDispatchTable(const StaticIndirectDispatchTable* table) {
    g_publishedStaticIndirectDispatchTable.store(table, std::memory_order_release);
    mkwv::runtime::clear_dispatch_cache();
}

void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept {
    std::fprintf(stderr, "mkwv: fatal %.*s: %.*s\n", static_cast<int>(category.size()), category.data(),
                 static_cast<int>(details.size()), details.data());
}
