#include "mkwv/gx/dl_cache.hpp"

#include "mkwv/gx/perf.hpp"

#include <cstring>

namespace mkwv::gx {
namespace {

constexpr std::uint32_t kSlots = 16384;  // power of two
constexpr unsigned kProbe = 4;
constexpr std::uint32_t kMinVerts = 3;
// Vertices one batch can allocate (fills) plus the waste of one wrap. A hit must sit further
// from the overwrite point than this, or the current batch could overwrite it before its job runs.
constexpr std::uint64_t kBatchGuard = 2ull * kMaxDecodedVerts;

struct Entry {
    std::uint64_t sig = 0;
    std::uint64_t content = 0;
    std::uint64_t pos = 0;  // absolute position in the ring (vertices)
    std::uint32_t addr = 0;
    std::uint32_t count = 0;
    std::uint32_t ready = 0;  // batch serial that filled it
    std::uint32_t hits = 0;
    bool used = false;
    bool volatile_data = false;  // its arrays changed under it once: never cached again
};

Entry g_table[kSlots];
CachedVertex* g_pool = nullptr;
std::uint64_t g_cap = 0;   // vertices
std::uint64_t g_head = 0;  // absolute vertices allocated
bool g_enabled = true;
std::uint32_t g_serial = 1;
DlCacheStats g_stats;

inline std::uint32_t slot_of(std::uint32_t addr, std::uint64_t sig, std::uint32_t count) {
    std::uint64_t h = (static_cast<std::uint64_t>(addr) * 0x9E3779B97F4A7C15ull) ^ sig ^ (count * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29;
    return static_cast<std::uint32_t>(h) & (kSlots - 1u);
}

inline bool data_valid(const Entry& e) {
    return e.used && g_head <= g_cap + e.pos;  // not yet overwritten
}

inline bool data_safe(const Entry& e) {
    // Not overwritten, and no allocation of this batch can reach it.
    return e.used && g_head + kBatchGuard <= g_cap + e.pos;
}

bool allocate(Entry& e, std::uint32_t count) {
    if (g_pool == nullptr || count > g_cap / 8u) {
        return false;
    }
    std::uint64_t cursor = g_head % g_cap;
    if (cursor + count > g_cap) {
        g_head += g_cap - cursor;  // wrap: the tail is skipped
    }
    e.pos = g_head;
    g_head += count;
    e.ready = g_serial;
    e.hits = 0;
    return true;
}

} // namespace

std::uint64_t hash_bytes64(const void* data, std::uint32_t n, std::uint64_t seed) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = seed ^ (0x9E3779B97F4A7C15ull * (n + 1u));
    std::uint32_t i = 0;
    for (; i + 8u <= n; i += 8u) {
        std::uint32_t a, b;
        std::memcpy(&a, p + i, 4);
        std::memcpy(&b, p + i + 4, 4);
        // Two 32x32->64 multiplies are cheap on ARMv7 (umull); a 64x64 one is three.
        const std::uint64_t m = static_cast<std::uint64_t>(a ^ static_cast<std::uint32_t>(h)) *
                                static_cast<std::uint64_t>(b ^ 0x85EBCA6Bu ^ static_cast<std::uint32_t>(h >> 32));
        h = (h ^ m) * 0x100000001B3ull + (h >> 23);
    }
    for (; i < n; ++i) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    h ^= h >> 31;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return h;
}

void dl_cache_install(void* pool, std::uint32_t bytes) {
    for (Entry& e : g_table) {
        e = Entry{};
    }
    g_head = 0;
    if (pool == nullptr || bytes / sizeof(CachedVertex) < 3u * kBatchGuard) {
        g_pool = nullptr;
        g_cap = 0;
        return;
    }
    g_pool = static_cast<CachedVertex*>(pool);
    g_cap = bytes / sizeof(CachedVertex);
}

void dl_cache_set_enabled(bool on) {
    g_enabled = on;
}

bool dl_cache_active() {
    return g_enabled && g_pool != nullptr;
}

DlCacheResult dl_cache_lookup(std::uint32_t addr, std::uint64_t sig, std::uint32_t count, std::uint64_t content,
                              DlCacheRef* ref) {
    if (!dl_cache_active() || count < kMinVerts) {
        return kDlNone;
    }
    const std::uint32_t base = slot_of(addr, sig, count);
    std::uint32_t victim = base;
    std::uint64_t victim_pos = ~0ull;
    for (unsigned k = 0; k < kProbe; ++k) {
        const std::uint32_t s = (base + k) & (kSlots - 1u);
        Entry& e = g_table[s];
        if (e.used && e.addr == addr && e.sig == sig && e.count == count) {
            ref->slot = s;
            if (e.volatile_data) {
                return kDlNone;  // animated vertex data (it failed a spot check)
            }
            if (e.ready == g_serial) {
                ++g_stats.busy;  // filled by this batch's jobs, not done yet
                perf::count(perf::kDlBusy);
                return kDlNone;
            }
            if (e.content == content && data_safe(e)) {
                ref->data = g_pool + (e.pos % g_cap);
                ref->check = (++e.hits & 3u) == 0u;
                ++g_stats.hits;
                g_stats.hit_verts += count;
                perf::count(perf::kDlHitVerts, count);
                return kDlHit;
            }
            // Changed or aged out: refill in place.
            e.content = content;
            if (!allocate(e, count)) {
                e.used = false;
                return kDlNone;
            }
            ref->data = g_pool + (e.pos % g_cap);
            ref->check = false;
            ++g_stats.fills;
            g_stats.fill_verts += count;
            perf::count(perf::kDlFillVerts, count);
            return kDlFill;
        }
        const std::uint64_t pos = data_valid(e) ? e.pos : 0ull;
        if (pos < victim_pos) {
            victim_pos = pos;
            victim = s;
        }
    }
    Entry& e = g_table[victim];
    e.used = true;
    e.volatile_data = false;
    e.addr = addr;
    e.sig = sig;
    e.count = count;
    e.content = content;
    if (!allocate(e, count)) {
        e.used = false;
        return kDlNone;
    }
    ref->slot = victim;
    ref->data = g_pool + (e.pos % g_cap);
    ref->check = false;
    ++g_stats.fills;
    g_stats.fill_verts += count;
    perf::count(perf::kDlFillVerts, count);
    return kDlFill;
}

DlCacheResult dl_cache_refill(DlCacheRef* ref) {
    Entry& e = g_table[ref->slot & (kSlots - 1u)];
    ++g_stats.rejects;
    perf::count(perf::kDlRejects);
    // The arrays changed under the cache: this primitive's vertex data is animated. Decode it
    // every frame from now on (the entry stays, marked, so the key is not refilled again).
    e.volatile_data = true;
    ref->data = nullptr;
    return kDlNone;
}

void dl_cache_drop(const DlCacheRef& ref) {
    g_table[ref.slot & (kSlots - 1u)].used = false;
}

void dl_cache_batch_begin() {
    ++g_serial;
    if (g_serial == 0) {
        g_serial = 1;
    }
}

void dl_cache_take_stats(DlCacheStats* out) {
    if (out != nullptr) {
        *out = g_stats;
    }
    g_stats = DlCacheStats{};
}

} // namespace mkwv::gx
