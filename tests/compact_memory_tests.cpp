#include "mkwv/runtime/compact_memory.hpp"

#include <cstdlib>
#include <cstdio>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "compact_memory: %s\n", message);
    return 1;
}

} // namespace

int main() {
    using mkwv::runtime::CompactMemory;
    using mkwv::runtime::kMem1Cached;
    using mkwv::runtime::kMem1Physical;
    using mkwv::runtime::kMem1Size;
    using mkwv::runtime::kMem1Uncached;
    using mkwv::runtime::kMem2Cached;
    using mkwv::runtime::kMem2Size;
    using mkwv::runtime::kKamekBase;

    CompactMemory memory;
    if (!memory.initialize()) {
        return fail("initialize");
    }

    const auto stats = memory.stats();
    const std::size_t expected_unique = mkwv::runtime::kMem1Size + mkwv::runtime::kMem2Size +
                                        mkwv::runtime::kKamekSize + mkwv::runtime::kLockedCacheSize;
    if (stats.unique_bytes != expected_unique) {
        return fail("unique_bytes");
    }
    if (stats.unique_bytes >= 96u * 1024u * 1024u) {
        return fail("unique guest RAM exceeded 96 MiB budget");
    }

    memory.write32(kMem1Cached + 0x100, 0x12345678u);
    if (memory.read32(kMem1Physical + 0x100) != 0x12345678u) {
        return fail("MEM1 physical alias");
    }
    if (memory.read32(kMem1Uncached + 0x100) != 0x12345678u) {
        return fail("MEM1 uncached alias");
    }
    if (memory.read8(kMem1Cached + 0x100) != 0x12) {
        return fail("big-endian byte 0");
    }
    if (memory.read8(kMem1Cached + 0x103) != 0x78) {
        return fail("big-endian byte 3");
    }

    memory.write16(kMem2Cached + 0x200, 0xABCDu);
    if (memory.read16(kMem2Cached + 0x200) != 0xABCDu) {
        return fail("MEM2 store");
    }

    const std::uint32_t top = static_cast<std::uint32_t>(kMem1Size - 4);
    memory.write32(kMem1Cached + top, 0xCAFEBABEu);
    if (memory.read32(kMem1Cached + top) != 0xCAFEBABEu) {
        return fail("MEM1 top");
    }
    // 0x81800000 is the Kamek overlay, so MEM1-cached+size is mapped on purpose.
    // Uncached MEM1 does not continue there.
    if (memory.host_pointer(kMem1Uncached + kMem1Size, 1) != nullptr) {
        return fail("MEM1 uncached overrun should miss");
    }
    if (memory.host_pointer(0xCC008000u, 4) != nullptr) {
        return fail("GX FIFO must not be RAM");
    }

    memory.write8(kKamekBase, 0x5A);
    if (memory.read8(kKamekBase) != 0x5A) {
        return fail("kamek overlay");
    }

    // Bias reconstruction: host pointer from cached and physical must match.
    if (memory.host_pointer(kMem1Cached + 0x100, 4) != memory.host_pointer(kMem1Physical + 0x100, 4)) {
        return fail("alias host pointer");
    }

    std::printf("compact_memory: unique=%zu alias=%zu pages=%u ok\n", stats.unique_bytes,
                stats.mapped_alias_bytes, stats.mapped_pages);
    return 0;
}
