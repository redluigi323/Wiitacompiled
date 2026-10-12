#pragma once

#include "guest_flat_memory.h"
#include "mkwv/runtime/guest_access.hpp"
#include "mkwv/runtime/compact_memory.hpp"

#include <cstddef>
#include <cstdint>

class Memory {
public:
    static std::uint8_t Read8(std::uint32_t addr) { return mkwv::runtime::guest_read8(addr); }
    static std::uint16_t Read16(std::uint32_t addr) { return mkwv::runtime::guest_read16(addr); }
    static std::uint32_t Read32(std::uint32_t addr) { return mkwv::runtime::guest_read32(addr); }
    static std::uint64_t Read64(std::uint32_t addr) { return mkwv::runtime::guest_read64(addr); }
    static void Write8(std::uint32_t addr, std::uint8_t val) { mkwv::runtime::guest_write8(addr, val); }
    static void Write16(std::uint32_t addr, std::uint16_t val) { mkwv::runtime::guest_write16(addr, val); }
    static void Write32(std::uint32_t addr, std::uint32_t val) { mkwv::runtime::guest_write32(addr, val); }
    static void Write64(std::uint32_t addr, std::uint64_t val) { mkwv::runtime::guest_write64(addr, val); }

    static std::uint8_t* GetPointer(std::uint32_t addr) { return GetPointer(addr, 1); }
    static std::uint8_t* GetPointer(std::uint32_t addr, std::size_t length) {
        return mkwv::runtime::guest_pointer(addr, length);
    }
    static bool Contains(std::uint32_t addr, std::size_t length = 1) { return GetPointer(addr, length) != nullptr; }
};

#include "memory_access.h"
