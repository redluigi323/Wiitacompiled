#include "mkwv/runtime/guest_access.hpp"
#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/hle_fifo.hpp"

#include <cstring>

std::uintptr_t mkwv_fast_page_bias[4096] = {};
mkwv::runtime::HleFifo* mkwv_guest_fifo_ptr = nullptr;

namespace mkwv::runtime {
namespace {

CompactMemory* g_memory = nullptr;
HleFifo* g_fifo = nullptr;
std::uint32_t g_slow = 0;

} // namespace

void bind_guest_access(CompactMemory* memory, HleFifo* fifo) {
    g_memory = memory;
    g_fifo = fifo;
    mkwv_guest_fifo_ptr = fifo;
    if (memory != nullptr) {
        memory->export_fast_table(mkwv_fast_page_bias);
    } else {
        std::memset(mkwv_fast_page_bias, 0, sizeof(mkwv_fast_page_bias));
    }
}

void unbind_guest_access() {
    g_memory = nullptr;
    g_fifo = nullptr;
    mkwv_guest_fifo_ptr = nullptr;
    std::memset(mkwv_fast_page_bias, 0, sizeof(mkwv_fast_page_bias));
}

// FlatWriteRam's checked fallback remains RAM-only (including locked cache); never FIFO.
void guest_write_ram8_slow(std::uint32_t addr, std::uint8_t value) {
    ++g_slow;
    if (g_memory != nullptr) {
        g_memory->write8(addr, value);
    }
}

void guest_write_ram16_slow(std::uint32_t addr, std::uint16_t value) {
    ++g_slow;
    if (g_memory != nullptr) {
        g_memory->write16(addr, value);
    }
}

void guest_write_ram32_slow(std::uint32_t addr, std::uint32_t value) {
    ++g_slow;
    if (g_memory != nullptr) {
        g_memory->write32(addr, value);
    }
}

void guest_write_ram64_slow(std::uint32_t addr, std::uint64_t value) {
    ++g_slow;
    if (g_memory != nullptr) {
        g_memory->write64(addr, value);
    }
}

std::uint8_t* guest_pointer_slow(std::uint32_t addr, std::size_t length) {
    if (g_memory == nullptr) {
        return nullptr;
    }
    return g_memory->host_pointer(addr, length);
}

std::uint32_t guest_slow_accesses() {
    return g_slow;
}

CompactMemory* guest_memory() {
    return g_memory;
}

HleFifo* guest_fifo() {
    return g_fifo;
}

std::uint8_t guest_read8_slow(std::uint32_t addr) {
    ++g_slow;
    if (g_memory == nullptr || is_mmio_address(addr)) {
        return 0;
    }
    return g_memory->read8(addr);
}

std::uint16_t guest_read16_slow(std::uint32_t addr) {
    ++g_slow;
    if (g_memory == nullptr || is_mmio_address(addr)) {
        return 0;
    }
    return g_memory->read16(addr);
}

std::uint32_t guest_read32_slow(std::uint32_t addr) {
    ++g_slow;
    if (g_memory == nullptr || is_mmio_address(addr)) {
        return 0;
    }
    return g_memory->read32(addr);
}

std::uint64_t guest_read64_slow(std::uint32_t addr) {
    ++g_slow;
    if (g_memory == nullptr || is_mmio_address(addr)) {
        return 0;
    }
    return g_memory->read64(addr);
}

void guest_write8_slow(std::uint32_t addr, std::uint8_t value) {
    ++g_slow;
    if (is_gpu_fifo_address(addr) && g_fifo != nullptr) {
        g_fifo->write8(value);
        return;
    }
    if (g_memory != nullptr && !is_mmio_address(addr)) {
        g_memory->write8(addr, value);
    }
}

void guest_write16_slow(std::uint32_t addr, std::uint16_t value) {
    ++g_slow;
    if (is_gpu_fifo_address(addr) && g_fifo != nullptr) {
        g_fifo->write16(value);
        return;
    }
    if (g_memory != nullptr && !is_mmio_address(addr)) {
        g_memory->write16(addr, value);
    }
}

void guest_write32_slow(std::uint32_t addr, std::uint32_t value) {
    ++g_slow;
    if (is_gpu_fifo_address(addr) && g_fifo != nullptr) {
        g_fifo->write32(value);
        return;
    }
    if (g_memory != nullptr && !is_mmio_address(addr)) {
        g_memory->write32(addr, value);
    }
}

void guest_write64_slow(std::uint32_t addr, std::uint64_t value) {
    ++g_slow;
    if (is_gpu_fifo_address(addr) && g_fifo != nullptr) {
        g_fifo->write64(value);
        return;
    }
    if (g_memory != nullptr && !is_mmio_address(addr)) {
        g_memory->write64(addr, value);
    }
}

void guest_write_f32(std::uint32_t addr, float value) {
    if (is_gpu_fifo_address(addr) && g_fifo != nullptr) {
        g_fifo->write_f32(value);
        return;
    }
    if (g_memory == nullptr || is_mmio_address(addr)) {
        return;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    g_memory->write32(addr, bits);
}

void guest_write_f64(std::uint32_t addr, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, 8);
    guest_write64(addr, bits);
}

void guest_write_burst(std::uint32_t addr, const void* data, std::uint32_t bytes) {
    if (is_gpu_fifo_address(addr) && g_fifo != nullptr) {
        g_fifo->write_burst(data, bytes);
        return;
    }
    if (g_memory != nullptr && !is_mmio_address(addr) && data != nullptr) {
        g_memory->write(addr, data, bytes);
    }
}

} // namespace mkwv::runtime
