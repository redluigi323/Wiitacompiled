#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Guest memory access, used by every translated load and store.
//
// This is the hottest code in the port: the translated game does millions of guest loads and
// stores per frame. They used to be two out-of-line calls each (guest_read32 ->
// CompactMemory::read32 -> read() -> lookup(), plus a memcpy), i.e. dozens of instructions
// and several branches for what is one LDR + REV on the host. Now each access is inline:
// one table load for its 1 MiB page, a range check and the access itself. Anything the table
// does not cover - MMIO (the GX FIFO at 0xCC008000), the locked cache, unmapped addresses, an
// access straddling two pages - takes the old out-of-line path unchanged.

// One entry per 1 MiB guest page (4096 entries). 0 = slow path; otherwise
// (host address of the page - guest address of the page) + 1, for a page backed entirely by
// one host buffer. Filled from the bound CompactMemory by bind_guest_access().
extern "C" std::uintptr_t mkwv_fast_page_bias[4096];

#if defined(__GNUC__)
// Plain inline on purpose: forcing these into every translated access did not pay off (see
// handoff.md, "Tried and dropped").
#define MKWV_MEM_INLINE inline
#define MKWV_MEM_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define MKWV_MEM_INLINE inline
#define MKWV_MEM_LIKELY(x) (x)
#endif

namespace mkwv::runtime {

class CompactMemory;
class HleFifo;

void bind_guest_access(CompactMemory* memory, HleFifo* fifo);
void unbind_guest_access();

CompactMemory* guest_memory();
HleFifo* guest_fifo();

// The out-of-line paths (MMIO/FIFO, partial pages, unmapped addresses, page-straddling).
std::uint8_t guest_read8_slow(std::uint32_t addr);
std::uint16_t guest_read16_slow(std::uint32_t addr);
std::uint32_t guest_read32_slow(std::uint32_t addr);
std::uint64_t guest_read64_slow(std::uint32_t addr);
void guest_write8_slow(std::uint32_t addr, std::uint8_t value);
void guest_write16_slow(std::uint32_t addr, std::uint16_t value);
void guest_write32_slow(std::uint32_t addr, std::uint32_t value);
void guest_write64_slow(std::uint32_t addr, std::uint64_t value);
void guest_write_ram8_slow(std::uint32_t addr, std::uint8_t value);
void guest_write_ram16_slow(std::uint32_t addr, std::uint16_t value);
void guest_write_ram32_slow(std::uint32_t addr, std::uint32_t value);
void guest_write_ram64_slow(std::uint32_t addr, std::uint64_t value);
std::uint8_t* guest_pointer_slow(std::uint32_t addr, std::size_t length);

// Host address of [addr, addr + bytes) when it lies in one fast page, else nullptr.
MKWV_MEM_INLINE std::uint8_t* guest_fast_ptr(std::uint32_t addr, std::uint32_t bytes) {
    const std::uintptr_t bias = mkwv_fast_page_bias[addr >> 20];
    if (MKWV_MEM_LIKELY(bias != 0 && (addr & 0xFFFFFu) <= 0x100000u - bytes)) {
        return reinterpret_cast<std::uint8_t*>(bias - 1u + addr);
    }
    return nullptr;
}

MKWV_MEM_INLINE std::uint32_t guest_load_be32(const std::uint8_t* p) {
    std::uint32_t raw;
    std::memcpy(&raw, p, 4);
    return __builtin_bswap32(raw);
}

MKWV_MEM_INLINE void guest_store_be32(std::uint8_t* p, std::uint32_t value) {
    const std::uint32_t raw = __builtin_bswap32(value);
    std::memcpy(p, &raw, 4);
}

MKWV_MEM_INLINE std::uint8_t guest_read8(std::uint32_t addr) {
    if (std::uint8_t* p = guest_fast_ptr(addr, 1)) {
        return *p;
    }
    return guest_read8_slow(addr);
}

MKWV_MEM_INLINE std::uint16_t guest_read16(std::uint32_t addr) {
    if (std::uint8_t* p = guest_fast_ptr(addr, 2)) {
        std::uint16_t raw;
        std::memcpy(&raw, p, 2);
        return __builtin_bswap16(raw);
    }
    return guest_read16_slow(addr);
}

MKWV_MEM_INLINE std::uint32_t guest_read32(std::uint32_t addr) {
    if (std::uint8_t* p = guest_fast_ptr(addr, 4)) {
        return guest_load_be32(p);
    }
    return guest_read32_slow(addr);
}

MKWV_MEM_INLINE std::uint64_t guest_read64(std::uint32_t addr) {
    if (std::uint8_t* p = guest_fast_ptr(addr, 8)) {
        // Two word loads: LDRD faults on an unaligned address on ARMv7, LDR does not.
        return (static_cast<std::uint64_t>(guest_load_be32(p)) << 32) | guest_load_be32(p + 4);
    }
    return guest_read64_slow(addr);
}

MKWV_MEM_INLINE void guest_write8(std::uint32_t addr, std::uint8_t value) {
    if (std::uint8_t* p = guest_fast_ptr(addr, 1)) {
        *p = value;
        return;
    }
    guest_write8_slow(addr, value);
}

MKWV_MEM_INLINE void guest_write16(std::uint32_t addr, std::uint16_t value) {
    if (std::uint8_t* p = guest_fast_ptr(addr, 2)) {
        const std::uint16_t raw = __builtin_bswap16(value);
        std::memcpy(p, &raw, 2);
        return;
    }
    guest_write16_slow(addr, value);
}

MKWV_MEM_INLINE void guest_write32(std::uint32_t addr, std::uint32_t value) {
    if (std::uint8_t* p = guest_fast_ptr(addr, 4)) {
        guest_store_be32(p, value);
        return;
    }
    guest_write32_slow(addr, value);
}

MKWV_MEM_INLINE void guest_write64(std::uint32_t addr, std::uint64_t value) {
    if (std::uint8_t* p = guest_fast_ptr(addr, 8)) {
        guest_store_be32(p, static_cast<std::uint32_t>(value >> 32));
        guest_store_be32(p + 4, static_cast<std::uint32_t>(value));
        return;
    }
    guest_write64_slow(addr, value);
}

// Host pointer for a guest range (nullptr if it is not one contiguous mapped range).
MKWV_MEM_INLINE std::uint8_t* guest_pointer(std::uint32_t addr, std::size_t length) {
    if (length != 0 && length <= 0x100000u) {
        if (std::uint8_t* p = guest_fast_ptr(addr, static_cast<std::uint32_t>(length))) {
            return p;
        }
    }
    return guest_pointer_slow(addr, length);
}

void guest_write_f32(std::uint32_t addr, float value);
void guest_write_f64(std::uint32_t addr, double value);
void guest_write_burst(std::uint32_t addr, const void* data, std::uint32_t bytes);

// Slow-path counters, for the PERF log line (how often the fast table missed).
std::uint32_t guest_slow_accesses();

} // namespace mkwv::runtime
