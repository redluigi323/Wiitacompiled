#pragma once

#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/guest_access.hpp"
#include "mkwv/runtime/hle_fifo.hpp"

#include <cstdint>
#include <cstring>
#include <cstddef>

// GX command writes from translated code (every GXPosition/GXColor/BP/XF word). Inline: the
// common case is a bounds check and a store into the FIFO buffer.
extern "C" {
inline void GX_HLE_FIFO_Write8(std::uint8_t value) {
    if (mkwv::runtime::HleFifo* fifo = mkwv_guest_fifo_ptr) {
        fifo->write8(value);
    }
}
inline void GX_HLE_FIFO_Write16(std::uint16_t value) {
    if (mkwv::runtime::HleFifo* fifo = mkwv_guest_fifo_ptr) {
        fifo->write16(value);
    }
}
inline void GX_HLE_FIFO_Write32(std::uint32_t value) {
    if (mkwv::runtime::HleFifo* fifo = mkwv_guest_fifo_ptr) {
        fifo->write32(value);
    }
}
inline void GX_HLE_FIFO_WriteFloat(float value) {
    if (mkwv::runtime::HleFifo* fifo = mkwv_guest_fifo_ptr) {
        fifo->write_f32(value);
    }
}
inline void GX_HLE_FIFO_WriteBurst(const std::uint8_t* data, std::uint32_t bytes) {
    if (mkwv::runtime::HleFifo* fifo = mkwv_guest_fifo_ptr) {
        fifo->write_burst(data, bytes);
    }
}
}

// Translator-facing guest memory ABI. Names match upstream MemoryInline::Flat*
// so stock TUs compile. On Vita there is no 4 GiB reservation: every helper
// goes through CompactMemory's page table, and 0xCC008000 stores hit HLE FIFO.
namespace MemoryInline {

inline constexpr std::uint32_t kPageShift = mkwv::runtime::kPageShift;
inline constexpr std::uint32_t kPageSize = mkwv::runtime::kPageSize;

inline constexpr bool FlatWriteNeedsPolicy(std::uint32_t address) {
    return mkwv::runtime::is_mmio_address(address);
}

inline std::uint32_t ConvertPpcDoubleToSingleBits(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t exponent = static_cast<std::uint32_t>((bits >> 52) & 0x7FFu);
    if (exponent - 874u <= 22u) {
        std::uint32_t narrowed =
            static_cast<std::uint32_t>(0x80000000ULL | ((bits & 0x000FFFFFFFFFFFFFULL) >> 21));
        narrowed >>= (905u - exponent);
        narrowed |= static_cast<std::uint32_t>((bits >> 32) & 0x80000000ULL);
        return narrowed;
    }
    return static_cast<std::uint32_t>(((bits >> 32) & 0xC0000000ULL) | ((bits >> 29) & 0x3FFFFFFFULL));
}

inline float PpcSingleBitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline std::uint16_t ByteSwap16(std::uint16_t value) {
    return static_cast<std::uint16_t>((value << 8) | (value >> 8));
}

inline std::uint32_t ByteSwap32(std::uint32_t value) {
    return (value << 24) | ((value << 8) & 0x00FF0000u) | ((value >> 8) & 0x0000FF00u) | (value >> 24);
}

inline std::uint64_t ByteSwap64(std::uint64_t value) {
    return (static_cast<std::uint64_t>(ByteSwap32(static_cast<std::uint32_t>(value))) << 32) |
           ByteSwap32(static_cast<std::uint32_t>(value >> 32));
}

inline std::uint8_t* ResolveRangeHost(std::uint32_t base, std::int32_t min_offset, std::uint32_t length,
                                     bool needs_read, bool needs_write) {
    (void)needs_read;
    const std::uint32_t guest_start = base + static_cast<std::uint32_t>(min_offset);
    // Fast pages are ordinary RAM (never MMIO), so reads and writes both qualify.
    if (length != 0 && length <= kPageSize) {
        if (std::uint8_t* p = mkwv::runtime::guest_fast_ptr(guest_start, length)) {
            return p;
        }
    }
    if (length == 0 || length > kPageSize) {
        return nullptr;
    }
    if (guest_start > 0xFFFFFFFFu - (length - 1u)) {
        return nullptr;
    }
    if (needs_write && (FlatWriteNeedsPolicy(guest_start) || FlatWriteNeedsPolicy(guest_start + (length - 1u)))) {
        return nullptr;
    }
    return mkwv::runtime::guest_pointer_slow(guest_start, length);
}

inline std::uint8_t FlatRead8(std::uint32_t address) {
    return mkwv::runtime::guest_read8(address);
}
inline std::uint16_t FlatRead16(std::uint32_t address) {
    return mkwv::runtime::guest_read16(address);
}
inline std::uint32_t FlatRead32(std::uint32_t address) {
    return mkwv::runtime::guest_read32(address);
}
inline std::uint64_t FlatRead64(std::uint32_t address) {
    return mkwv::runtime::guest_read64(address);
}

inline std::uint64_t Read64Slow(std::uint32_t address) { return FlatRead64(address); }

inline float FlatReadFloat8(std::uint32_t address) {
    return static_cast<float>(FlatRead8(address));
}
inline float FlatReadFloat16(std::uint32_t address) {
    return static_cast<float>(FlatRead16(address));
}
inline float FlatReadFloat32(std::uint32_t address) {
    return PpcSingleBitsToFloat(FlatRead32(address));
}
inline double FlatReadFloat64(std::uint32_t address) {
    const std::uint64_t bits = FlatRead64(address);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void FlatWrite8(std::uint32_t address, std::uint8_t value) {
    mkwv::runtime::guest_write8(address, value);
}
inline void FlatWrite16(std::uint32_t address, std::uint16_t value) {
    mkwv::runtime::guest_write16(address, value);
}
inline void FlatWrite32(std::uint32_t address, std::uint32_t value) {
    mkwv::runtime::guest_write32(address, value);
}
inline void FlatWrite64(std::uint32_t address, std::uint64_t value) {
    mkwv::runtime::guest_write64(address, value);
}

inline void FlatWriteFloat8(std::uint32_t address, double value) {
    FlatWrite8(address, static_cast<std::uint8_t>(ConvertPpcDoubleToSingleBits(value) >> 24));
}
inline void FlatWriteFloat16(std::uint32_t address, double value) {
    FlatWrite16(address, static_cast<std::uint16_t>(ConvertPpcDoubleToSingleBits(value) >> 16));
}
inline void FlatWriteFloat32(std::uint32_t address, double value) {
    const std::uint32_t bits = ConvertPpcDoubleToSingleBits(value);
    if (mkwv::runtime::is_gpu_fifo_address(address)) {
        GX_HLE_FIFO_WriteFloat(PpcSingleBitsToFloat(bits));
        return;
    }
    FlatWrite32(address, bits);
}
inline void FlatWriteFloat64(std::uint32_t address, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    FlatWrite64(address, bits);
}

inline void FlatWriteRam8(std::uint32_t address, std::uint8_t value) {
    if (std::uint8_t* p = mkwv::runtime::guest_fast_ptr(address, 1)) {
        *p = value;
        return;
    }
    mkwv::runtime::guest_write_ram8_slow(address, value);
}
inline void FlatWriteRam16(std::uint32_t address, std::uint16_t value) {
    if (std::uint8_t* p = mkwv::runtime::guest_fast_ptr(address, 2)) {
        const std::uint16_t raw = __builtin_bswap16(value);
        std::memcpy(p, &raw, 2);
        return;
    }
    mkwv::runtime::guest_write_ram16_slow(address, value);
}
inline void FlatWriteRam32(std::uint32_t address, std::uint32_t value) {
    if (std::uint8_t* p = mkwv::runtime::guest_fast_ptr(address, 4)) {
        mkwv::runtime::guest_store_be32(p, value);
        return;
    }
    mkwv::runtime::guest_write_ram32_slow(address, value);
}
inline void FlatWriteRam64(std::uint32_t address, std::uint64_t value) {
    if (std::uint8_t* p = mkwv::runtime::guest_fast_ptr(address, 8)) {
        mkwv::runtime::guest_store_be32(p, static_cast<std::uint32_t>(value >> 32));
        mkwv::runtime::guest_store_be32(p + 4, static_cast<std::uint32_t>(value));
        return;
    }
    mkwv::runtime::guest_write_ram64_slow(address, value);
}
inline void FlatWriteRamFloat8(std::uint32_t address, double value) {
    FlatWriteRam8(address, static_cast<std::uint8_t>(ConvertPpcDoubleToSingleBits(value) >> 24));
}
inline void FlatWriteRamFloat16(std::uint32_t address, double value) {
    FlatWriteRam16(address, static_cast<std::uint16_t>(ConvertPpcDoubleToSingleBits(value) >> 16));
}
inline void FlatWriteRamFloat32(std::uint32_t address, double value) {
    FlatWriteRam32(address, ConvertPpcDoubleToSingleBits(value));
}
inline void FlatWriteRamFloat64(std::uint32_t address, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    FlatWriteRam64(address, bits);
}

template <typename T>
inline bool ReadResolvedScalar(std::uint8_t* host, std::uint32_t range_offset, T& out_value) {
    if (host == nullptr) {
        return false;
    }
    if constexpr (sizeof(T) == 1) {
        out_value = host[range_offset];
    } else if constexpr (sizeof(T) == 2) {
        std::uint16_t raw = 0;
        std::memcpy(&raw, host + range_offset, 2);
        out_value = static_cast<T>(ByteSwap16(raw));
    } else if constexpr (sizeof(T) == 4) {
        std::uint32_t raw = 0;
        std::memcpy(&raw, host + range_offset, 4);
        out_value = static_cast<T>(ByteSwap32(raw));
    } else {
        std::uint64_t raw = 0;
        std::memcpy(&raw, host + range_offset, 8);
        out_value = static_cast<T>(ByteSwap64(raw));
    }
    return true;
}

template <typename T>
inline bool WriteResolvedScalar(std::uint8_t* host, std::uint32_t range_offset, T value) {
    if (host == nullptr) {
        return false;
    }
    if constexpr (sizeof(T) == 1) {
        host[range_offset] = static_cast<std::uint8_t>(value);
    } else if constexpr (sizeof(T) == 2) {
        const std::uint16_t swapped = ByteSwap16(static_cast<std::uint16_t>(value));
        std::memcpy(host + range_offset, &swapped, 2);
    } else if constexpr (sizeof(T) == 4) {
        const std::uint32_t swapped = ByteSwap32(static_cast<std::uint32_t>(value));
        std::memcpy(host + range_offset, &swapped, 4);
    } else {
        const std::uint64_t swapped = ByteSwap64(static_cast<std::uint64_t>(value));
        std::memcpy(host + range_offset, &swapped, 8);
    }
    return true;
}

inline std::uint8_t ReadResolved8(std::uint8_t* host, std::uint32_t offset, std::uint32_t address) {
    std::uint8_t value = 0;
    return ReadResolvedScalar(host, offset, value) ? value : FlatRead8(address);
}
inline std::uint16_t ReadResolved16(std::uint8_t* host, std::uint32_t offset, std::uint32_t address) {
    std::uint16_t value = 0;
    return ReadResolvedScalar(host, offset, value) ? value : FlatRead16(address);
}
inline std::uint32_t ReadResolved32(std::uint8_t* host, std::uint32_t offset, std::uint32_t address) {
    std::uint32_t value = 0;
    return ReadResolvedScalar(host, offset, value) ? value : FlatRead32(address);
}
inline std::uint64_t ReadResolved64(std::uint8_t* host, std::uint32_t offset, std::uint32_t address) {
    std::uint64_t value = 0;
    return ReadResolvedScalar(host, offset, value) ? value : FlatRead64(address);
}
inline float ReadResolvedFloat32(std::uint8_t* host, std::uint32_t offset, std::uint32_t address) {
    return PpcSingleBitsToFloat(ReadResolved32(host, offset, address));
}
inline double ReadResolvedFloat64(std::uint8_t* host, std::uint32_t offset, std::uint32_t address) {
    const std::uint64_t bits = ReadResolved64(host, offset, address);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void WriteResolved8(std::uint8_t* host, std::uint32_t offset, std::uint32_t address, std::uint8_t value) {
    if (!WriteResolvedScalar(host, offset, value)) {
        FlatWrite8(address, value);
    }
}
inline void WriteResolved16(std::uint8_t* host, std::uint32_t offset, std::uint32_t address, std::uint16_t value) {
    if (!WriteResolvedScalar(host, offset, value)) {
        FlatWrite16(address, value);
    }
}
inline void WriteResolved32(std::uint8_t* host, std::uint32_t offset, std::uint32_t address, std::uint32_t value) {
    if (!WriteResolvedScalar(host, offset, value)) {
        FlatWrite32(address, value);
    }
}
inline void WriteResolved64(std::uint8_t* host, std::uint32_t offset, std::uint32_t address, std::uint64_t value) {
    if (!WriteResolvedScalar(host, offset, value)) {
        FlatWrite64(address, value);
    }
}
inline void WriteResolvedFloat32(std::uint8_t* host, std::uint32_t offset, std::uint32_t address, double value) {
    const std::uint32_t bits = ConvertPpcDoubleToSingleBits(value);
    if (!WriteResolvedScalar(host, offset, bits)) {
        FlatWriteFloat32(address, value);
    }
}
inline void WriteResolvedFloat64(std::uint8_t* host, std::uint32_t offset, std::uint32_t address, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (!WriteResolvedScalar(host, offset, bits)) {
        FlatWriteFloat64(address, value);
    }
}

struct ResolvedLoadPair {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    bool valid = false;
};

inline ResolvedLoadPair ReadResolvedPair16(std::uint8_t* host, std::uint32_t offset) {
    std::uint32_t packed = 0;
    if (!ReadResolvedScalar(host, offset, packed)) {
        return {};
    }
    return {packed >> 16, packed & 0xFFFFu, true};
}
inline ResolvedLoadPair ReadResolvedPair32(std::uint8_t* host, std::uint32_t offset) {
    std::uint64_t packed = 0;
    if (!ReadResolvedScalar(host, offset, packed)) {
        return {};
    }
    return {static_cast<std::uint32_t>(packed >> 32), static_cast<std::uint32_t>(packed), true};
}
inline bool WriteResolvedPair16(std::uint8_t* host, std::uint32_t offset, std::uint32_t packed) {
    return WriteResolvedScalar(host, offset, packed);
}
inline bool WriteResolvedPair32(std::uint8_t* host, std::uint32_t offset, std::uint64_t packed) {
    return WriteResolvedScalar(host, offset, packed);
}

inline void Write64Slow(std::uint32_t address, std::uint64_t value) { FlatWrite64(address, value); }

template <typename T>
inline T ReadResolvedFallback(std::uint32_t addr) {
    if constexpr (sizeof(T) == 1) {
        return static_cast<T>(FlatRead8(addr));
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(FlatRead16(addr));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(FlatRead32(addr));
    } else {
        return static_cast<T>(FlatRead64(addr));
    }
}

template <typename T>
inline void WriteResolvedFallback(std::uint32_t addr, T value) {
    if constexpr (sizeof(T) == 1) {
        FlatWrite8(addr, static_cast<std::uint8_t>(value));
    } else if constexpr (sizeof(T) == 2) {
        FlatWrite16(addr, static_cast<std::uint16_t>(value));
    } else if constexpr (sizeof(T) == 4) {
        FlatWrite32(addr, static_cast<std::uint32_t>(value));
    } else {
        FlatWrite64(addr, static_cast<std::uint64_t>(value));
    }
}

inline bool TryGetPointerFast(std::uint32_t address, std::size_t length, std::uint8_t*& pointer) {
    pointer = ResolveRangeHost(address, 0, static_cast<std::uint32_t>(length), true, false);
    return pointer != nullptr;
}

inline bool TryGetWritablePointerFast(std::uint32_t address, std::size_t length, std::uint8_t*& pointer) {
    pointer = ResolveRangeHost(address, 0, static_cast<std::uint32_t>(length), true, true);
    return pointer != nullptr;
}

inline std::uint8_t ReadStack8(std::uint32_t address) { return FlatRead8(address); }
inline std::uint16_t ReadStack16(std::uint32_t address) { return FlatRead16(address); }
inline std::uint32_t ReadStack32(std::uint32_t address) { return FlatRead32(address); }
inline std::uint64_t ReadStack64(std::uint32_t address) { return FlatRead64(address); }
inline void WriteStack8(std::uint32_t address, std::uint8_t value) { FlatWriteRam8(address, value); }
inline void WriteStack16(std::uint32_t address, std::uint16_t value) { FlatWriteRam16(address, value); }
inline void WriteStack32(std::uint32_t address, std::uint32_t value) { FlatWriteRam32(address, value); }
inline void WriteStack64(std::uint32_t address, std::uint64_t value) { FlatWriteRam64(address, value); }

} // namespace MemoryInline
