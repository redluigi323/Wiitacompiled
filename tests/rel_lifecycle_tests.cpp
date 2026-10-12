#include "abi_bridge.h"
#include "memory.h"
#include "mkwv/runtime/runtime.hpp"
#include "mkwv/runtime/hle_os.hpp"

#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t kModuleManager = 0x80005000u;
constexpr uint32_t kModule = 0x805102E0u;
constexpr uint32_t kHeap = 0x80510250u;
constexpr uint32_t kHeapHandle = 0x80510288u;
constexpr uint32_t kSystem = 0x80007000u;
constexpr uint32_t kSceneManager = 0x80008000u;
unsigned prologs = 0;

void translated_prolog(CpuContext* ctx) {
    ++prologs;
    Memory::Write32(kSceneManager + 44u, 0x80009000u);
    Memory::Write32(kSceneManager + 20u, 0);
    ctx->gpr[3] = 0;
}

int fail(const char* what) {
    std::fprintf(stderr, "rel_lifecycle: %s\n", what);
    return 1;
}
} // namespace

int main() {
    using namespace mkwv::runtime;
    RawDispatchRecord entries[] = {
        {0x8055531Cu, translated_prolog, 0, false},
    };
    StaticIndirectDispatchPage pages[0x54Fu]{};
    pages[0x54Eu].firstEntry = 0;
    pages[0x54Eu].entryCount = 1;
    StaticIndirectDispatchSegment segments[256]{};
    segments[0x80].pages = pages;
    segments[0x80].firstPage = 7;
    segments[0x80].pageCount = 0x54Fu;
    const StaticIndirectDispatchTable table{"rel-test", segments, entries, 1};
    RegisterStaticIndirectDispatchTable(&table);
    Runtime runtime;
    if (!runtime.initialize(nullptr)) return fail("initialize");
    Memory::Write32(0x80385FF8u, kModuleManager);
    Memory::Write32(0x80385FC8u, kSystem);
    Memory::Write32(kSystem + 84u, kSceneManager);
    Memory::Write32(kSceneManager + 20u, 0xFFFFFFFFu);
    Memory::Write32(0x802A40E0u, kHeap);
    Memory::Write32(kHeap + 16u, kHeapHandle);
    Memory::Write32(kHeapHandle, 0x45585048u);
    Memory::Write32(kHeapHandle + 24u, 0x805102E0u);
    Memory::Write32(kHeapHandle + 28u, 0x80F00000u);
    Memory::Write32(kHeapHandle + 60u, 0x805102E0u);
    Memory::Write32(kHeapHandle + 64u, 0x805102E0u);
    Memory::Write16(0x805102E0u, 0x4652u);
    Memory::Write32(0x805102E4u, 0x80F00000u - 0x805102F0u);
    Memory::Write32(0x805103B4u, 0x9421FFF0u);
    Memory::Write32(0x808B3CB0u, 0x80543B84u);
    const uint32_t stack = runtime.cpu().gpr[1];

    // StaticR is preloaded at the fixed base used by its translated code. The
    // strap loader must register that image rather than relocate it a second time.
    if (!invoke_guest(0x800074A4u, &runtime.cpu()) ||
        Memory::Read32(kModuleManager + 32u) != kModule ||
        Memory::Read32(kModule + 52u) != 0x8055531Cu || runtime.cpu().gpr[3] != kModule)
        return fail("preloaded module not registered");
    // Heap metadata lives at 805102F0; the image header and executable section
    // must still be the manager-facing module, not a memset fake at 80510300.
    // A subsequent allocation must not overwrite the preloaded REL or its
    // registration header, which the old non-owning registration allowed.
    runtime.cpu().gpr[3] = kHeap;
    runtime.cpu().gpr[4] = 64;
    runtime.cpu().gpr[5] = 32;
    invoke_guest(0x80226C04u, &runtime.cpu());
    if (runtime.cpu().gpr[3] < 0x805102E0u + 4934832u ||
        Memory::Read32(0x805103B4u) != 0x9421FFF0u ||
        Memory::Read32(0x808B3CB0u) != 0x80543B84u ||
        Memory::Read32(kModule + 52u) != 0x8055531Cu)
        return fail("REL storage reused or changed");
    if (!invoke_guest(Memory::Read32(kModule + 52u), &runtime.cpu()) || prologs != 1 ||
        Memory::Read32(kSceneManager + 44u) != 0x80009000u ||
        Memory::Read32(kSceneManager + 20u) != 0 || runtime.cpu().gpr[1] != stack)
        return fail("prolog registration or stack");
    invoke_guest(0x8055531Cu, &runtime.cpu());
    if (prologs != 1) return fail("duplicate prolog without reload");
    invoke_guest(0x800074A4u, &runtime.cpu());
    invoke_guest(0x8055531Cu, &runtime.cpu());
    if (prologs != 1) return fail("resident registration must not rerun prolog");

    if (std::strstr(rel_boot_debug(), "phase=ready") == nullptr)
        return fail("completed prolog diagnostic");
    // The native registration must fail visibly if boot has not constructed the
    // module manager yet.
    RegisterStaticIndirectDispatchTable(nullptr);
    Memory::Write32(0x80385FF8u, 0);
    invoke_guest(0x800074A4u, &runtime.cpu());
    if (runtime.cpu().gpr[3] != 0 || std::strstr(rel_boot_debug(), "phase=missing-manager") == nullptr)
        return fail("missing manager silently accepted");
    if (!runtime.initialize(nullptr)) return fail("reinitialize");
    invoke_guest(0x8055531Cu, &runtime.cpu());
    if (last_missing_target() != 0x8055531Cu) return fail("missing prolog silently accepted");
    RegisterStaticIndirectDispatchTable(&table);
    invoke_guest(0x8055531Cu, &runtime.cpu());
    if (prologs != 2) return fail("failed prolog cannot be retried");
    RegisterStaticIndirectDispatchTable(nullptr);
    return 0;
}
