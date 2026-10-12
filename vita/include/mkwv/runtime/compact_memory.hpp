#pragma once

#include <cstddef>
#include <cstdint>

namespace mkwv::runtime {

// Retail Wii map, not the NDEV 128 MiB MEM2 that upstream uses on PC.
inline constexpr std::size_t kMem1Size = 24u * 1024u * 1024u;
inline constexpr std::size_t kMem2Size = 64u * 1024u * 1024u;
inline constexpr std::size_t kKamekSize = 2u * 1024u * 1024u;
inline constexpr std::size_t kLockedCacheSize = 16u * 1024u;

inline constexpr std::uint32_t kMem1Physical = 0x00000000u;
inline constexpr std::uint32_t kMem1Cached = 0x80000000u;
inline constexpr std::uint32_t kMem1Uncached = 0xC0000000u;
inline constexpr std::uint32_t kMem2Physical = 0x10000000u;
inline constexpr std::uint32_t kMem2Cached = 0x90000000u;
inline constexpr std::uint32_t kMem2Uncached = 0xD0000000u;
inline constexpr std::uint32_t kKamekBase = 0x81800000u;
inline constexpr std::uint32_t kLockedCacheBase = 0xE0000000u;
inline constexpr std::uint32_t kFifoBase = 0xCC008000u;
inline constexpr std::uint32_t kFifoEnd = 0xCC008100u;

inline constexpr std::uint32_t kMem2IosReserved = 0x20000u;
inline constexpr std::uint32_t kMem2IpcArena = 0x20000u;
inline constexpr std::uint32_t kDvdFstReserve = 0x200000u;
inline constexpr std::uint32_t kDvdFstBase =
    kMem2Cached + static_cast<std::uint32_t>(kMem2Size) - kMem2IosReserved - kMem2IpcArena - kDvdFstReserve;

inline constexpr unsigned kPageShift = 20;
inline constexpr std::uint32_t kPageSize = 1u << kPageShift;
inline constexpr std::uint32_t kPageCount = 1u << (32 - kPageShift);

struct CompactMemoryStats {
    std::size_t unique_bytes = 0;
    std::size_t mapped_alias_bytes = 0;
    unsigned mapped_pages = 0;
};

class CompactMemory {
public:
    CompactMemory() = default;
    CompactMemory(const CompactMemory&) = delete;
    CompactMemory& operator=(const CompactMemory&) = delete;
    ~CompactMemory();

    // Allocates unique backing (MEM1+MEM2+Kamek+locked cache) and installs
    // cached/uncached/physical aliases into the 1 MiB page table.
    bool initialize();
    void shutdown();
    bool is_active() const { return mem1_ != nullptr; }

    std::uint8_t* host_pointer(std::uint32_t guest, std::size_t length) const;
    bool read(std::uint32_t guest, void* dst, std::size_t length) const;
    bool write(std::uint32_t guest, const void* src, std::size_t length);

    std::uint8_t read8(std::uint32_t guest) const;
    std::uint16_t read16(std::uint32_t guest) const;
    std::uint32_t read32(std::uint32_t guest) const;
    std::uint64_t read64(std::uint32_t guest) const;
    void write8(std::uint32_t guest, std::uint8_t value);
    void write16(std::uint32_t guest, std::uint16_t value);
    void write32(std::uint32_t guest, std::uint32_t value);
    void write64(std::uint32_t guest, std::uint64_t value);

    // Fills the 4096-entry fast page table (see guest_access.hpp): (host - guest) + 1 for
    // every 1 MiB page backed entirely by one buffer, 0 for everything else.
    void export_fast_table(std::uintptr_t* table) const;

    CompactMemoryStats stats() const { return stats_; }
    std::uint8_t* mem1() { return mem1_; }
    std::uint8_t* mem2() { return mem2_; }

private:
    bool map_alias(std::uint32_t guest_base, std::uint8_t* host, std::size_t size);
    std::uint8_t* lookup(std::uint32_t guest, std::size_t length) const;

    std::uint8_t* mem1_ = nullptr;
    std::uint8_t* mem2_ = nullptr;
    std::uint8_t* kamek_ = nullptr;
    std::uint8_t* locked_ = nullptr;
    int mem1_uid_ = -1;
    int mem2_uid_ = -1;
    int kamek_uid_ = -1;
    int locked_uid_ = -1;

    struct PageEntry {
        std::uint8_t* base = nullptr;
        std::uint32_t limit = 0;
        std::uintptr_t encoded_bias = 0;
    };
    PageEntry pages_[kPageCount]{};
    CompactMemoryStats stats_{};
};

// Called (rate-limited by the installer) when a guest access hits a page that is not mapped at all.
// Without it unmapped reads silently return 0 and writes vanish, which hides wild pointers.
using BadAccessHook = void (*)(bool write, std::uint32_t guest, std::size_t length);
void set_bad_access_hook(BadAccessHook hook);

} // namespace mkwv::runtime
