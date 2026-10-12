#pragma once

// The pieces of upstream WiiCompiled's runtime API the ported AX/DSP HLE (ax_*.cpp, taken from
// upstream runtime/src/hle/audio) relies on, mapped onto this runtime. Upstream's Memory has
// Try* accessors and region constants the Vita Memory class does not; they live here instead
// of in memory.h, which every translated shard includes.

#include "memory.h"
#include "mkwv/runtime/compact_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace AxMem {

inline constexpr std::size_t kMem1Size = mkwv::runtime::kMem1Size;
inline constexpr std::size_t kMem2Size = mkwv::runtime::kMem2Size;
inline constexpr std::uint32_t kMem1CachedBase = mkwv::runtime::kMem1Cached;
inline constexpr std::uint32_t kMem1UncachedBase = mkwv::runtime::kMem1Uncached;
inline constexpr std::uint32_t kMem2PhysicalBase = mkwv::runtime::kMem2Physical;
inline constexpr std::uint32_t kMem2CachedBase = mkwv::runtime::kMem2Cached;
inline constexpr std::uint32_t kMem2UncachedBase = mkwv::runtime::kMem2Uncached;

inline std::uint8_t Read8(std::uint32_t addr) { return Memory::Read8(addr); }
inline std::uint16_t Read16(std::uint32_t addr) { return Memory::Read16(addr); }
inline std::uint32_t Read32(std::uint32_t addr) { return Memory::Read32(addr); }
inline void Write16(std::uint32_t addr, std::uint16_t v) { Memory::Write16(addr, v); }
inline void Write32(std::uint32_t addr, std::uint32_t v) { Memory::Write32(addr, v); }
inline bool Contains(std::uint32_t addr, std::size_t length = 1) { return Memory::Contains(addr, length); }
inline std::uint8_t* GetPointer(std::uint32_t addr, std::size_t length) { return Memory::GetPointer(addr, length); }

inline bool TryRead32(std::uint32_t addr, std::uint32_t& out) {
    if (!Memory::Contains(addr, 4)) {
        return false;
    }
    out = Memory::Read32(addr);
    return true;
}

inline bool TryWrite32(std::uint32_t addr, std::uint32_t value) {
    if (!Memory::Contains(addr, 4)) {
        return false;
    }
    Memory::Write32(addr, value);
    return true;
}

inline std::uint16_t ByteSwap16(std::uint16_t v) { return __builtin_bswap16(v); }
inline std::uint32_t ByteSwap32(std::uint32_t v) { return __builtin_bswap32(v); }

} // namespace AxMem

#define AX_LOGF(...) std::printf("mkwv: audio: " __VA_ARGS__)
