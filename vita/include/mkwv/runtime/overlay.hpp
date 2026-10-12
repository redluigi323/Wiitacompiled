#pragma once

#include "mkwv/runtime/cpu_context.hpp"

#include <cstddef>
#include <cstdint>

namespace mkwv::runtime {

inline constexpr unsigned kOverlayCatalogCap = 512;
inline constexpr unsigned kOverlaySlots = 64;
inline constexpr std::size_t kOverlayWindowBytes = 1024u * 1024u;
inline constexpr char kOverlayMagic[8] = {'M', 'K', 'W', 'V', 'S', 'H', 'R', 'D'};

struct OverlayStats {
    unsigned catalog = 0;
    unsigned resident = 0;
    std::size_t resident_bytes = 0;
    std::uint32_t hits = 0;
    std::uint32_t misses = 0;
    std::uint32_t evictions = 0;
};

class OverlayCache {
public:
    void reset();
    bool set_root(const char* path);
    const char* root() const { return root_; }

    bool register_native(std::uint32_t guest, TranslatedFn fn, std::uint32_t bytes, const char* name);
    bool write_shard_file(std::uint32_t guest, const void* payload, std::uint32_t bytes, const char* name);
    TranslatedFn resolve(std::uint32_t guest);
    bool is_resident(std::uint32_t guest) const;
    OverlayStats stats() const { return stats_; }

private:
    struct CatalogEntry {
        std::uint32_t guest = 0;
        std::uint32_t bytes = 0;
        TranslatedFn fn = nullptr;
        char name[32]{};
        bool used = false;
    };
    struct Slot {
        std::uint32_t guest = 0;
        std::uint32_t bytes = 0;
        TranslatedFn fn = nullptr;
        std::uint32_t last_use = 0;
        bool occupied = false;
    };

    CatalogEntry* find_catalog(std::uint32_t guest);
    const CatalogEntry* find_catalog(std::uint32_t guest) const;
    Slot* find_slot(std::uint32_t guest);
    const Slot* find_slot(std::uint32_t guest) const;
    bool load_payload(const CatalogEntry& entry, std::uint8_t* dst, std::uint32_t cap) const;
    void evict_until(std::size_t need);
    bool shard_path(std::uint32_t guest, char* out, std::size_t cap) const;

    CatalogEntry catalog_[kOverlayCatalogCap]{};
    Slot slots_[kOverlaySlots]{};
    char root_[160]{};
    OverlayStats stats_{};
    std::uint32_t clock_ = 1;
};

} // namespace mkwv::runtime
