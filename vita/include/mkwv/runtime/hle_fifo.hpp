#pragma once

#include "mkwv/gx/fifo.hpp"
#include "mkwv/runtime/compact_memory.hpp"

#include <cstdint>

namespace mkwv::runtime {

inline constexpr bool is_mmio_address(std::uint32_t addr) {
    return (addr & 0xFE000000u) == 0xCC000000u;
}

inline constexpr bool is_gpu_fifo_address(std::uint32_t addr) {
    return addr >= kFifoBase && addr < kFifoEnd;
}

// Guest stores to 0xCC008000 land here instead of RAM. Drain feeds the GX
// command processor the same way Broadway's GP reads the CPU FIFO.
class HleFifo {
public:
    void reset();
    void write8(std::uint8_t value) { fifo_.write_u8(value); }
    void write16(std::uint16_t value) { fifo_.write_u16(value); }
    void write32(std::uint32_t value) { fifo_.write_u32(value); }
    void write64(std::uint64_t value) {
        fifo_.write_u32(static_cast<std::uint32_t>(value >> 32));
        fifo_.write_u32(static_cast<std::uint32_t>(value));
    }
    void write_f32(float value) { fifo_.write_f32(value); }
    void write_burst(const void* data, std::uint32_t bytes) { fifo_.write_bytes(data, bytes); }

    gx::Fifo& gx() { return fifo_; }
    const gx::Fifo& gx() const { return fifo_; }
    const std::uint8_t* data() const { return fifo_.data(); }
    std::uint32_t size() const { return fifo_.size(); }
    bool overflow() const { return fifo_.overflow(); }
    bool drain(gx::Processor& processor, bool big_endian = true);

private:
    gx::Fifo fifo_{};
};

} // namespace mkwv::runtime

// The bound HLE FIFO (bind_guest_access), read inline by the GX_HLE_FIFO_* writers the
// translated code calls for every GX command word.
extern "C" mkwv::runtime::HleFifo* mkwv_guest_fifo_ptr;
