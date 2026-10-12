#include "mkwv/runtime/compact_memory.hpp"

#include <cstdio>
#include <cstring>
#include <new>

#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#include <psp2/types.h>
#endif

namespace mkwv::runtime {
namespace {

BadAccessHook g_bad_access_hook = nullptr;

std::uint16_t bswap16(std::uint16_t value) {
    return static_cast<std::uint16_t>((value << 8) | (value >> 8));
}

std::uint32_t bswap32(std::uint32_t value) {
    return (value << 24) | ((value << 8) & 0x00FF0000u) | ((value >> 8) & 0x0000FF00u) | (value >> 24);
}

std::uint64_t bswap64(std::uint64_t value) {
    return (static_cast<std::uint64_t>(bswap32(static_cast<std::uint32_t>(value))) << 32) |
           bswap32(static_cast<std::uint32_t>(value >> 32));
}

// Where each guest RAM block landed, for the boot log (see mkwv_guest_ram_placement below).
char g_placement[192] = "";

void note_placement(const char* name, const char* where, std::size_t size, int err) {
    const std::size_t used = std::strlen(g_placement);
    if (used + 48 >= sizeof(g_placement)) {
        return;
    }
    if (err >= 0) {
        std::snprintf(g_placement + used, sizeof(g_placement) - used, "%s%s=%s(%uMiB)", used ? " " : "", name, where,
                      static_cast<unsigned>(size >> 20));
    } else {
        std::snprintf(g_placement + used, sizeof(g_placement) - used, "%s%s=FAILED(0x%08X)", used ? " " : "", name,
                      static_cast<unsigned>(err));
    }
}

// Guest RAM goes in ordinary user memory when it fits. A stock Vita app has ~80 MiB of it
// left once the executable and libc heap are resident, less than the 90 MiB MEM1 + MEM2 +
// Kamek need, so a block that does not fit falls back to physically contiguous memory
// (cached, 1 MiB granularity) and then to CDRAM (256 KiB granularity; slower for the CPU but
// plentiful). Extended-memory mode (ATTRIBUTE2=12 in param.sfo) usually makes the fallbacks
// unnecessary.
std::uint8_t* allocate_unique(std::size_t size, int* uid, const char* name) {
    if (size == 0) {
        return nullptr;
    }
#if defined(__vita__)
    struct Kind {
        SceKernelMemBlockType type;
        std::size_t align;
        const char* label;
    };
    static const Kind kinds[] = {
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4096u, "user"},
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, 1024u * 1024u, "phycont"},
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256u * 1024u, "cdram"},
    };
    int last_err = 0;
    for (const Kind& k : kinds) {
        const std::size_t sz = (size + k.align - 1u) & ~(k.align - 1u);
        const SceUID block = sceKernelAllocMemBlock("mkwv_guest", k.type, static_cast<SceSize>(sz), nullptr);
        if (block < 0) {
            last_err = block;
            continue;
        }
        void* base = nullptr;
        if (sceKernelGetMemBlockBase(block, &base) < 0) {
            sceKernelFreeMemBlock(block);
            continue;
        }
        std::memset(base, 0, sz);
        *uid = block;
        note_placement(name, k.label, sz, 0);
        return static_cast<std::uint8_t*>(base);
    }
    note_placement(name, "", size, last_err);
    return nullptr;
#else
    (void)name;
    *uid = -1;
    return new (std::nothrow) std::uint8_t[size]();
#endif
}

void free_unique(std::uint8_t* pointer, int uid) {
#if defined(__vita__)
    (void)pointer;
    if (uid >= 0) {
        sceKernelFreeMemBlock(uid);
    }
#else
    (void)uid;
    delete[] pointer;
#endif
}

} // namespace

CompactMemory::~CompactMemory() {
    shutdown();
}

bool CompactMemory::initialize() {
    shutdown();

    g_placement[0] = '\0';
    // Largest first, so MEM2 gets ordinary memory and the smaller blocks take the fallbacks.
    mem2_ = allocate_unique(kMem2Size, &mem2_uid_, "MEM2");
    mem1_ = allocate_unique(kMem1Size, &mem1_uid_, "MEM1");
    kamek_ = allocate_unique(kKamekSize, &kamek_uid_, "Kamek");
    locked_ = allocate_unique(kLockedCacheSize, &locked_uid_, "L2");
    if (!mem1_ || !mem2_ || !kamek_ || !locked_) {
        shutdown();
        return false;
    }

    stats_ = {};
    stats_.unique_bytes = kMem1Size + kMem2Size + kKamekSize + kLockedCacheSize;

    const bool ok = map_alias(kMem1Physical, mem1_, kMem1Size) && map_alias(kMem1Cached, mem1_, kMem1Size) &&
                    map_alias(kMem1Uncached, mem1_, kMem1Size) && map_alias(kMem2Physical, mem2_, kMem2Size) &&
                    map_alias(kMem2Cached, mem2_, kMem2Size) && map_alias(kMem2Uncached, mem2_, kMem2Size) &&
                    map_alias(kKamekBase, kamek_, kKamekSize) &&
                    map_alias(kLockedCacheBase, locked_, kLockedCacheSize);
    if (!ok) {
        shutdown();
        return false;
    }
    return true;
}

void CompactMemory::shutdown() {
    free_unique(mem1_, mem1_uid_);
    free_unique(mem2_, mem2_uid_);
    free_unique(kamek_, kamek_uid_);
    free_unique(locked_, locked_uid_);
    mem1_ = mem2_ = kamek_ = locked_ = nullptr;
    mem1_uid_ = mem2_uid_ = kamek_uid_ = locked_uid_ = -1;
    std::memset(pages_, 0, sizeof(pages_));
    stats_ = {};
}

bool CompactMemory::map_alias(std::uint32_t guest_base, std::uint8_t* host, std::size_t size) {
    if (host == nullptr || size == 0) {
        return false;
    }

    std::size_t offset = 0;
    while (offset < size) {
        const std::uint32_t guest = guest_base + static_cast<std::uint32_t>(offset);
        const std::uint32_t page = guest >> kPageShift;
        const std::uint32_t page_base = page << kPageShift;
        const std::uint32_t into_page = guest - page_base;
        std::uint32_t span = kPageSize - into_page;
        if (span > size - offset) {
            span = static_cast<std::uint32_t>(size - offset);
        }

        PageEntry& entry = pages_[page];
        if (entry.base != nullptr && entry.base != host + offset - into_page) {
            // A 1 MiB page already belongs to a different unique buffer.
            return false;
        }

        entry.base = host + offset - into_page;
        entry.limit = into_page + span;
        // ILP32 wrap is intentional: host + guest reconstructs via mod 2^32.
        const auto host_page = reinterpret_cast<std::uintptr_t>(entry.base);
        entry.encoded_bias = (host_page - static_cast<std::uintptr_t>(page_base)) + 1u;

        if (into_page == 0 && span == kPageSize) {
            ++stats_.mapped_pages;
        }
        stats_.mapped_alias_bytes += span;
        offset += span;
    }
    return true;
}

std::uint8_t* CompactMemory::lookup(std::uint32_t guest, std::size_t length) const {
    if (length == 0 || (guest & 0xFE000000u) == 0xCC000000u) {
        return nullptr;
    }
    const std::uint32_t page = guest >> kPageShift;
    const PageEntry& entry = pages_[page];
    if (entry.encoded_bias == 0) {
        return nullptr;
    }
    const std::uint32_t offset = guest & (kPageSize - 1u);
    if (offset + length > entry.limit) {
        return nullptr;
    }
    return reinterpret_cast<std::uint8_t*>((entry.encoded_bias - 1u) + guest);
}

void CompactMemory::export_fast_table(std::uintptr_t* table) const {
    for (std::uint32_t page = 0; page < kPageCount; ++page) {
        const PageEntry& entry = pages_[page];
        const bool whole = entry.encoded_bias != 0 && entry.limit == kPageSize &&
                           ((page << kPageShift) & 0xFE000000u) != 0xCC000000u;
        table[page] = whole ? entry.encoded_bias : 0u;
    }
}

std::uint8_t* CompactMemory::host_pointer(std::uint32_t guest, std::size_t length) const {
    return lookup(guest, length);
}

bool CompactMemory::read(std::uint32_t guest, void* dst, std::size_t length) const {
    if (dst == nullptr) {
        return false;
    }
    auto* out = static_cast<std::uint8_t*>(dst);
    std::size_t done = 0;
    while (done < length) {
        const std::uint32_t addr = guest + static_cast<std::uint32_t>(done);
        const std::uint32_t into_page = addr & (kPageSize - 1u);
        std::size_t chunk = kPageSize - into_page;
        if (chunk > length - done) {
            chunk = length - done;
        }
        std::uint8_t* src = lookup(addr, chunk);
        if (src == nullptr) {
            if (g_bad_access_hook != nullptr && (addr & 0xFE000000u) != 0xCC000000u &&
                pages_[addr >> kPageShift].encoded_bias == 0) {
                g_bad_access_hook(false, addr, chunk);
            }
            return false;
        }
        std::memcpy(out + done, src, chunk);
        done += chunk;
    }
    return true;
}

bool CompactMemory::write(std::uint32_t guest, const void* src, std::size_t length) {
    if (src == nullptr) {
        return false;
    }
    const auto* in = static_cast<const std::uint8_t*>(src);
    std::size_t done = 0;
    while (done < length) {
        const std::uint32_t addr = guest + static_cast<std::uint32_t>(done);
        const std::uint32_t into_page = addr & (kPageSize - 1u);
        std::size_t chunk = kPageSize - into_page;
        if (chunk > length - done) {
            chunk = length - done;
        }
        std::uint8_t* dst = lookup(addr, chunk);
        if (dst == nullptr) {
            if (g_bad_access_hook != nullptr && (addr & 0xFE000000u) != 0xCC000000u &&
                pages_[addr >> kPageShift].encoded_bias == 0) {
                g_bad_access_hook(true, addr, chunk);
            }
            return false;
        }
        std::memcpy(dst, in + done, chunk);
        done += chunk;
    }
    return true;
}

std::uint8_t CompactMemory::read8(std::uint32_t guest) const {
    std::uint8_t value = 0;
    read(guest, &value, 1);
    return value;
}

std::uint16_t CompactMemory::read16(std::uint32_t guest) const {
    std::uint16_t value = 0;
    read(guest, &value, 2);
    return bswap16(value);
}

std::uint32_t CompactMemory::read32(std::uint32_t guest) const {
    std::uint32_t value = 0;
    read(guest, &value, 4);
    return bswap32(value);
}

void CompactMemory::write8(std::uint32_t guest, std::uint8_t value) {
    write(guest, &value, 1);
}

void CompactMemory::write16(std::uint32_t guest, std::uint16_t value) {
    const std::uint16_t swapped = bswap16(value);
    write(guest, &swapped, 2);
}

void CompactMemory::write32(std::uint32_t guest, std::uint32_t value) {
    const std::uint32_t swapped = bswap32(value);
    write(guest, &swapped, 4);
}

std::uint64_t CompactMemory::read64(std::uint32_t guest) const {
    std::uint64_t packed = 0;
    if (read(guest, &packed, 8)) {
        return bswap64(packed);
    }
    const std::uint32_t hi = read32(guest);
    const std::uint32_t lo = read32(guest + 4u);
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

void CompactMemory::write64(std::uint32_t guest, std::uint64_t value) {
    const std::uint32_t hi = static_cast<std::uint32_t>(value >> 32);
    const std::uint32_t lo = static_cast<std::uint32_t>(value);
    const std::uint8_t bytes[8] = {
        static_cast<std::uint8_t>(hi >> 24), static_cast<std::uint8_t>(hi >> 16),
        static_cast<std::uint8_t>(hi >> 8),  static_cast<std::uint8_t>(hi),
        static_cast<std::uint8_t>(lo >> 24), static_cast<std::uint8_t>(lo >> 16),
        static_cast<std::uint8_t>(lo >> 8),  static_cast<std::uint8_t>(lo),
    };
    if (write(guest, bytes, 8)) {
        return;
    }
    write32(guest, hi);
    write32(guest + 4u, lo);
}

void set_bad_access_hook(BadAccessHook hook) {
    g_bad_access_hook = hook;
}

} // namespace mkwv::runtime

// Declared where it is used (main.cpp) rather than in compact_memory.hpp, which the
// translated shards include.
extern "C" const char* mkwv_guest_ram_placement() {
    return mkwv::runtime::g_placement;
}
