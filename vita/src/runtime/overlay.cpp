#include "mkwv/runtime/overlay.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#if defined(__vita__)
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#endif

namespace mkwv::runtime {
namespace {

void copy_name(char* dst, std::size_t cap, const char* name) {
    if (cap == 0) {
        return;
    }
    if (name == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::size_t n = 0;
    while (name[n] != '\0' && n + 1 < cap) {
        dst[n] = name[n];
        ++n;
    }
    dst[n] = '\0';
}

bool mkdir_one(const char* path) {
#if defined(__vita__)
    const int err = sceIoMkdir(path, 0777);
    return err >= 0 || err == static_cast<int>(0x80010011);
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

bool mkdir_parents(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    char buf[160];
    copy_name(buf, sizeof(buf), path);
    for (char* p = buf + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir_one(buf);
            *p = '/';
        }
    }
    return mkdir_one(buf);
}

} // namespace

void OverlayCache::reset() {
    std::memset(catalog_, 0, sizeof(catalog_));
    std::memset(slots_, 0, sizeof(slots_));
    root_[0] = '\0';
    stats_ = {};
    clock_ = 1;
}

bool OverlayCache::set_root(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    copy_name(root_, sizeof(root_), path);
    return mkdir_parents(root_);
}

OverlayCache::CatalogEntry* OverlayCache::find_catalog(std::uint32_t guest) {
    for (auto& entry : catalog_) {
        if (entry.used && entry.guest == guest) {
            return &entry;
        }
    }
    return nullptr;
}

const OverlayCache::CatalogEntry* OverlayCache::find_catalog(std::uint32_t guest) const {
    for (const auto& entry : catalog_) {
        if (entry.used && entry.guest == guest) {
            return &entry;
        }
    }
    return nullptr;
}

OverlayCache::Slot* OverlayCache::find_slot(std::uint32_t guest) {
    for (auto& slot : slots_) {
        if (slot.occupied && slot.guest == guest) {
            return &slot;
        }
    }
    return nullptr;
}

const OverlayCache::Slot* OverlayCache::find_slot(std::uint32_t guest) const {
    for (const auto& slot : slots_) {
        if (slot.occupied && slot.guest == guest) {
            return &slot;
        }
    }
    return nullptr;
}

bool OverlayCache::shard_path(std::uint32_t guest, char* out, std::size_t cap) const {
    if (out == nullptr || cap == 0 || root_[0] == '\0') {
        return false;
    }
    const int n = std::snprintf(out, cap, "%s/%08x.shard", root_, guest);
    return n > 0 && static_cast<std::size_t>(n) < cap;
}

bool OverlayCache::register_native(std::uint32_t guest, TranslatedFn fn, std::uint32_t bytes, const char* name) {
    if (guest == 0 || fn == nullptr) {
        return false;
    }
    CatalogEntry* entry = find_catalog(guest);
    if (entry == nullptr) {
        for (auto& slot : catalog_) {
            if (!slot.used) {
                entry = &slot;
                ++stats_.catalog;
                break;
            }
        }
    }
    if (entry == nullptr) {
        return false;
    }
    entry->used = true;
    entry->guest = guest;
    entry->fn = fn;
    entry->bytes = bytes == 0 ? 256u : bytes;
    copy_name(entry->name, sizeof(entry->name), name);
    return true;
}

bool OverlayCache::write_shard_file(std::uint32_t guest, const void* payload, std::uint32_t bytes, const char* name) {
    char path[192];
    if (!shard_path(guest, path, sizeof(path))) {
        return false;
    }
    std::FILE* file = std::fopen(path, "wb");
    if (!file) {
        return false;
    }
    char magic[8];
    std::memcpy(magic, kOverlayMagic, 8);
    const std::uint32_t version = 1;
    char stored_name[32]{};
    copy_name(stored_name, sizeof(stored_name), name);
    const bool ok = std::fwrite(magic, 1, 8, file) == 8 && std::fwrite(&version, 4, 1, file) == 1 &&
                    std::fwrite(&guest, 4, 1, file) == 1 && std::fwrite(&bytes, 4, 1, file) == 1 &&
                    std::fwrite(stored_name, 1, 32, file) == 32 &&
                    (bytes == 0 || payload == nullptr || std::fwrite(payload, 1, bytes, file) == bytes);
    std::fclose(file);
    return ok;
}

bool OverlayCache::load_payload(const CatalogEntry& entry, std::uint8_t* dst, std::uint32_t cap) const {
    if (dst == nullptr || cap == 0) {
        return false;
    }
    std::memset(dst, 0, cap);
    char path[192];
    if (!shard_path(entry.guest, path, sizeof(path))) {
        return true;
    }
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        return true;
    }
    char magic[8];
    std::uint32_t version = 0;
    std::uint32_t guest = 0;
    std::uint32_t bytes = 0;
    char name[32];
    const bool header = std::fread(magic, 1, 8, file) == 8 && std::memcmp(magic, kOverlayMagic, 8) == 0 &&
                        std::fread(&version, 4, 1, file) == 1 && version == 1 &&
                        std::fread(&guest, 4, 1, file) == 1 && guest == entry.guest &&
                        std::fread(&bytes, 4, 1, file) == 1 && std::fread(name, 1, 32, file) == 32;
    if (header && bytes != 0) {
        const std::uint32_t n = bytes < cap ? bytes : cap;
        std::fread(dst, 1, n, file);
    }
    std::fclose(file);
    return true;
}

void OverlayCache::evict_until(std::size_t need) {
    while ((stats_.resident >= kOverlaySlots || stats_.resident_bytes + need > kOverlayWindowBytes) &&
           stats_.resident > 0) {
        Slot* victim = nullptr;
        for (auto& slot : slots_) {
            if (!slot.occupied) {
                continue;
            }
            if (victim == nullptr || slot.last_use < victim->last_use) {
                victim = &slot;
            }
        }
        if (victim == nullptr) {
            return;
        }
        stats_.resident_bytes -= victim->bytes;
        --stats_.resident;
        ++stats_.evictions;
        *victim = Slot{};
    }
}

TranslatedFn OverlayCache::resolve(std::uint32_t guest) {
    Slot* live = find_slot(guest);
    if (live != nullptr) {
        live->last_use = clock_++;
        ++stats_.hits;
        return live->fn;
    }
    CatalogEntry* entry = find_catalog(guest);
    if (entry == nullptr || entry->fn == nullptr) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.misses;
    evict_until(entry->bytes);
    if (stats_.resident >= kOverlaySlots || stats_.resident_bytes + entry->bytes > kOverlayWindowBytes) {
        return nullptr;
    }
    Slot* free_slot = nullptr;
    for (auto& slot : slots_) {
        if (!slot.occupied) {
            free_slot = &slot;
            break;
        }
    }
    if (free_slot == nullptr) {
        return nullptr;
    }
    std::uint8_t scratch[256];
    const std::uint32_t load_bytes = entry->bytes < sizeof(scratch) ? entry->bytes : static_cast<std::uint32_t>(sizeof(scratch));
    (void)load_payload(*entry, scratch, load_bytes);
    free_slot->occupied = true;
    free_slot->guest = entry->guest;
    free_slot->bytes = entry->bytes;
    free_slot->fn = entry->fn;
    free_slot->last_use = clock_++;
    ++stats_.resident;
    stats_.resident_bytes += entry->bytes;
    return entry->fn;
}

bool OverlayCache::is_resident(std::uint32_t guest) const {
    return find_slot(guest) != nullptr;
}

} // namespace mkwv::runtime
