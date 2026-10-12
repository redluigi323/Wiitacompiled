#include "memory_access.h"

#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/guest_access.hpp"
#include "mkwv/runtime/hle_fifo.hpp"

#include <cstdio>
#include <cstring>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "memory_inline: %s\n", message);
    return 1;
}

} // namespace

int main() {
    using mkwv::runtime::CompactMemory;
    using mkwv::runtime::HleFifo;
    using mkwv::runtime::kFifoBase;
    using mkwv::runtime::kMem1Cached;
    using mkwv::runtime::kMem1Physical;

    CompactMemory memory;
    HleFifo fifo;
    if (!memory.initialize()) {
        return fail("initialize");
    }
    mkwv::runtime::bind_guest_access(&memory, &fifo);

    MemoryInline::FlatWrite32(kMem1Cached + 0x100, 0x12345678u);
    if (MemoryInline::FlatRead32(kMem1Physical + 0x100) != 0x12345678u) {
        return fail("flat alias");
    }
    if (MemoryInline::FlatRead8(kMem1Cached + 0x100) != 0x12) {
        return fail("flat be byte");
    }

    MemoryInline::FlatWrite64(kMem1Cached + 0x200, 0x1122334455667788ull);
    if (MemoryInline::FlatRead64(kMem1Cached + 0x200) != 0x1122334455667788ull) {
        return fail("flat u64");
    }
    if (MemoryInline::FlatRead32(kMem1Cached + 0x200) != 0x11223344u ||
        MemoryInline::FlatRead32(kMem1Cached + 0x204) != 0x55667788u) {
        return fail("flat u64 words");
    }

    const std::uint32_t one = MemoryInline::ConvertPpcDoubleToSingleBits(1.0);
    if (one != 0x3F800000u) {
        std::fprintf(stderr, "memory_inline: stfs 1.0 -> %08x\n", one);
        return fail("stfs 1.0");
    }
    MemoryInline::FlatWriteFloat32(kMem1Cached + 0x300, 1.0);
    if (MemoryInline::FlatRead32(kMem1Cached + 0x300) != 0x3F800000u) {
        return fail("flat stfs bits");
    }
    if (MemoryInline::FlatReadFloat32(kMem1Cached + 0x300) != 1.0f) {
        return fail("flat lfs");
    }

    MemoryInline::FlatWrite32(kFifoBase, 0x00000000u);
    MemoryInline::FlatWrite8(kFifoBase, 0x61);
    if (fifo.size() < 5) {
        return fail("fifo mmio");
    }
    const std::uint32_t before = fifo.size();
    MemoryInline::FlatWriteRam32(kFifoBase, 0xDEADBEEFu);
    if (fifo.size() != before) {
        return fail("ram helper must not hit fifo");
    }

    std::uint8_t* range = MemoryInline::ResolveRangeHost(kMem1Cached + 0x400, 0, 16u, true, true);
    if (range == nullptr) {
        return fail("resolve range");
    }
    MemoryInline::WriteResolved32(range, 4u, kMem1Cached + 0x404, 0xAABBCCDDu);
    if (MemoryInline::ReadResolved32(range, 4u, kMem1Cached + 0x404) != 0xAABBCCDDu) {
        return fail("resolved");
    }
    if (MemoryInline::FlatRead32(kMem1Cached + 0x404) != 0xAABBCCDDu) {
        return fail("resolved visible");
    }
    if (MemoryInline::ResolveRangeHost(kFifoBase, 0, 4u, false, true) != nullptr) {
        return fail("fifo range");
    }

    mkwv::runtime::unbind_guest_access();
    return 0;
}
