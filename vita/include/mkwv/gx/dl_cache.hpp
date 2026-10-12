#pragma once

// Display-list vertex cache (static64).
//
// Most of a race frame's vertices come from display lists that do not change between frames:
// nw4r's shape DLs, drawn with new matrices every frame. The expensive part of a vertex job was
// fetching and dequantising its attributes from the guest's big-endian arrays, not the
// transform. This cache keeps each DL primitive's decoded model-space vertices (CachedVertex)
// across frames. A primitive is identified by the guest address of its vertex data inside the
// display list, the vertex layout signature (VCD, VAT, array bindings), its vertex count and a
// hash of its packet bytes. Array contents are assumed to stay put (spot-checked: every fourth
// hit re-decodes the first vertex and compares; a primitive that fails is animated data and is
// decoded every frame from then on).
//
// Storage is a ring of CachedVertex installed by the platform. An entry filled by vertex jobs
// becomes usable once its batch has drained (the next batch); a hit never points at storage the
// current batch could overwrite. Not shard-visible.

#include "mkwv/gx/decode.hpp"

#include <cstdint>

namespace mkwv::gx {

// `pool` must stay valid while installed. nullptr (or too small) disables the cache.
void dl_cache_install(void* pool, std::uint32_t bytes);
void dl_cache_set_enabled(bool on);
bool dl_cache_active();

enum DlCacheResult : int {
    kDlNone = 0,  // not cached: decode as usual (no space, or the entry is still being filled)
    kDlHit = 1,   // `data` holds the decoded vertices
    kDlFill = 2,  // `data` is storage to decode into (the entry is valid from the next batch)
};

struct DlCacheRef {
    CachedVertex* data = nullptr;
    std::uint32_t slot = 0;
    bool check = false;  // a hit that should be verified (decode vertex 0 and compare)
};

DlCacheResult dl_cache_lookup(std::uint32_t guest_addr, std::uint64_t signature, std::uint32_t count,
                              std::uint64_t content, DlCacheRef* ref);
// A hit failed verification (the arrays changed under it): the entry is marked animated and
// never cached again. Returns kDlNone (decode as usual).
DlCacheResult dl_cache_refill(DlCacheRef* ref);
// Forget an entry whose fill failed.
void dl_cache_drop(const DlCacheRef& ref);
// A new batch (everything queued before has drained): pending fills become usable.
void dl_cache_batch_begin();

// ---- GPU copy (static70, "GPU transform") ----
// A second ring of CachedVertex, the same size as the pool, in memory the GPU reads (uncached
// and GPU-mapped on the Vita). A hit can then be drawn straight from it by a vertex shader that
// does the transform, texgen and lighting (gl_submit.cpp), with no vertex job at all. Entries are
// copied over the first time they are drawn this way. `gpu_sync` waits for the GPU; it runs
// before a copy would overwrite vertices drawn in the last three frames.
void dl_cache_install_gpu(void* base, void (*gpu_sync)());
bool dl_cache_gpu_active();
const void* dl_cache_gpu_base();
std::uint32_t dl_cache_gpu_bytes();
// For a kDlHit: makes sure the entry's vertices are in the GPU copy and returns the index of its
// first vertex there (the GPU copy is an array of CachedVertex), or ~0u when there is no GPU copy.
std::uint32_t dl_cache_gpu_vertices(const DlCacheRef& ref, std::uint32_t count);
// Once per presented frame.
void dl_cache_frame_mark();
// GPU waits forced by copies since the last call, and vertices copied.
std::uint32_t dl_cache_take_gpu_syncs();
std::uint32_t dl_cache_take_gpu_copied();

struct DlCacheStats {
    std::uint32_t hits = 0, fills = 0, rejects = 0, busy = 0;
    std::uint32_t hit_verts = 0, fill_verts = 0;
};
void dl_cache_take_stats(DlCacheStats* out);

// 64-bit hash of `n` bytes, 8 at a time (content keys, draw merge keys).
std::uint64_t hash_bytes64(const void* p, std::uint32_t n, std::uint64_t seed = 0);

} // namespace mkwv::gx
