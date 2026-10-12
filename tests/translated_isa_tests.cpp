#include "abi_bridge.h"
#include "isa/ppc_isa_cr.h"
#include "ppc_runtime.h"

#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/runtime.hpp"

#include <cstdio>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "translated_isa: %s\n", message);
    return 1;
}

extern "C" void func_8000BEEF(CpuContext* ctx) {
    ctx->gpr[3] = ctx->gpr[3] + 1;
}

} // namespace

int main() {
    using namespace mkwv::runtime;

    uint32_t cr = 0;
    uint32_t xer = 0;
    SetCRResident(cr, xer, 0, static_cast<uint32_t>(2), static_cast<uint32_t>(2));
    if ((cr & 0x20000000u) == 0) {
        return fail("cr eq");
    }
    if (PPC_CntlzwInline(0) != 32u || PPC_CntlzwInline(0x00F00000u) != 8u) {
        return fail("cntlzw");
    }
    if (PpcGetPs0Inline(PpcPackPairedInline(1.5f, 2.5f)) != 1.5f ||
        PpcGetPs1Inline(PpcPackPairedInline(1.5f, 2.5f)) != 2.5f) {
        return fail("paired");
    }

    RawDispatchRecord entries[] = {
        {0x8000BEEFu, &func_8000BEEF, 0, false},
    };
    StaticIndirectDispatchPage pages[] = {
        {0, 1, 0},
    };
    StaticIndirectDispatchSegment segments[256]{};
    segments[0x80].pages = pages;
    segments[0x80].firstPage = 0xBu;
    segments[0x80].pageCount = 1;
    const StaticIndirectDispatchTable table{"test", segments, entries, 1};
    RegisterStaticIndirectDispatchTable(&table);

    Runtime runtime;
    if (!runtime.initialize("/tmp/mkwv-isa-overlay")) {
        return fail("runtime");
    }
    runtime.cpu().gpr[3] = 41;
    if (!invoke_guest(0x8000BEEFu, &runtime.cpu()) || runtime.cpu().gpr[3] != 42) {
        return fail("static dispatch");
    }
    if (invoke_guest(0x80100000u, &runtime.cpu())) {
        return fail("missing should fail");
    }
    if (last_missing_target() != 0x80100000u) {
        return fail("last missing");
    }

    runtime.cpu().gpr[3] = 1;
    const std::uint32_t before = runtime.fifo().size();
    if (!invoke_guest(0x8016E5A4u, &runtime.cpu())) {
        return fail("gx setnumtexgens native");
    }
    if (runtime.fifo().size() <= before) {
        return fail("gx fifo write");
    }
    if (!invoke_guest(0x801AA9B8u, &runtime.cpu())) {
        return fail("ossleepthread native");
    }
    if (runtime.memory().read32(0x80003118u) != 64u * 1024u * 1024u) {
        return fail("mem2 size");
    }
    if (runtime.memory().read32(0x80003124u) != 0x90000800u || runtime.memory().read32(0x80000030u) == 0) {
        return fail("mem arena");
    }
    if (!invoke_guest(0x8019F33Cu, &runtime.cpu()) || runtime.cpu().gpr[3] != 0x12u) {
        return fail("getconsoletype");
    }
    if (!invoke_guest(0x801A65ACu, &runtime.cpu()) || runtime.cpu().gpr[3] != 1u) {
        return fail("disable interrupts");
    }
    runtime.memory().write32(0x80004000u, 0xFFFFFFFFu);
    runtime.cpu().gpr[3] = 0x80004000u;
    runtime.cpu().gpr[4] = 32;
    if (!invoke_guest(0x801A16E4u, &runtime.cpu()) || runtime.memory().read32(0x80004000u) != 0) {
        return fail("dczero");
    }
    if (!invoke_guest(0x8016B850u, &runtime.cpu()) || runtime.cpu().gpr[3] != 0x80343740u) {
        return fail("gx init");
    }
    if (!invoke_guest(0x801B99ECu, &runtime.cpu())) {
        return fail("vi wait");
    }
    if (!invoke_guest(0x8015EA1Cu, &runtime.cpu())) {
        return fail("dvd init");
    }
    if (!invoke_guest(0x80165A30u, &runtime.cpu()) || runtime.cpu().gpr[3] != 1u) {
        return fail("dvd inquiry");
    }
    if (!invoke_guest(0x801B1D0Cu, &runtime.cpu()) || runtime.cpu().gpr[3] != 1u) {
        return fail("sc language");
    }
    if (!invoke_guest(0x801B90F4u, &runtime.cpu())) {
        return fail("vi pre retrace");
    }
    if (!invoke_guest(0x8012E5A4u, &runtime.cpu())) {
        return fail("ppchalt");
    }
    runtime.cpu().gpr[3] = 0;
    if (!invoke_guest(0x801AA4ECu, &runtime.cpu())) {
        return fail("os detach");
    }
    if (!invoke_guest(0x801938F8u, &runtime.cpu()) || runtime.cpu().gpr[3] != 1u) {
        return fail("ios open");
    }
    if (!invoke_guest(0x80193AD8u, &runtime.cpu()) || runtime.cpu().gpr[3] != 0) {
        return fail("ios close");
    }
    runtime.cpu().gpr[4] = 0x80004000u;
    runtime.cpu().gpr[5] = 16;
    runtime.cpu().gpr[6] = 0;
    runtime.cpu().gpr[7] = 0;
    if (!invoke_guest(0x8019B80Cu, &runtime.cpu()) || runtime.memory().read32(0x80004000u) != 0x53437630u) {
        return fail("nand read");
    }
    runtime.cpu().gpr[4] = 0x80004020u;
    if (!invoke_guest(0x8019BF4Cu, &runtime.cpu()) || runtime.memory().read32(0x80004020u) != 0x4000u) {
        return fail("nand length");
    }
    runtime.cpu().gpr[4] = 0;
    runtime.cpu().gpr[5] = 0;
    runtime.cpu().gpr[6] = 0;
    runtime.cpu().gpr[7] = 0;
    if (!invoke_guest(0x8019BA04u, &runtime.cpu()) || runtime.cpu().gpr[3] != 0) {
        return fail("nand rewind");
    }
    runtime.cpu().gpr[4] = 0x80004000u;
    runtime.cpu().gpr[5] = 0x4000u;
    runtime.cpu().gpr[6] = 0;
    runtime.cpu().gpr[7] = 0;
    if (!invoke_guest(0x8019B80Cu, &runtime.cpu()) || runtime.memory().read32(0x80004000u) != 0x53437630u ||
        runtime.memory().read32(0x80007FFCu) != 0x53436564u) {
        return fail("sysconf magic");
    }
    if ((runtime.memory().read32(0x80007FE4u) >> 16) == 0) {
        return fail("sysconf lng lut");
    }
    runtime.cpu().gpr[4] = 32;
    runtime.cpu().gpr[5] = 0;
    runtime.cpu().gpr[6] = 0;
    runtime.cpu().gpr[7] = 0;
    if (!invoke_guest(0x8019BA04u, &runtime.cpu()) || runtime.cpu().gpr[3] != 0) {
        return fail("nand seek");
    }
    runtime.cpu().gpr[3] = 0x80004100u;
    runtime.cpu().gpr[7] = 0;
    if (!invoke_guest(0x801A0870u, &runtime.cpu())) {
        return fail("ossetalarm");
    }
    runtime.cpu().gpr[13] = 0;
    runtime.cpu().gpr[2] = 0;
    if (!invoke_guest(0x801A98B0u, &runtime.cpu()) || runtime.cpu().gpr[13] != 0x8038CC00u ||
        runtime.cpu().gpr[2] != 0x8038EFA0u) {
        return fail("sda seed");
    }
    if (!invoke_guest(0x801AE4A0u, &runtime.cpu()) || runtime.cpu().gpr[3] != 1u) {
        return fail("title installed");
    }
    runtime.cpu().gpr[6] = 0x80004010u;
    if (!invoke_guest(0x801AD1D4u, &runtime.cpu()) || runtime.cpu().gpr[3] != 0 ||
        runtime.memory().read32(0x80004010u) != 3u) {
        return fail("check install");
    }
    runtime.memory().write32(0x80004200u, 0x8000BEEFu);
    runtime.memory().write32(0x80004204u, 41);
    runtime.cpu().gpr[3] = 0x80004300u;
    runtime.cpu().gpr[4] = 0x80004320u;
    runtime.cpu().gpr[5] = 4;
    if (!invoke_guest(0x801A72FCu, &runtime.cpu())) {
        return fail("msg queue init");
    }
    runtime.cpu().gpr[3] = 0x80004300u;
    runtime.cpu().gpr[4] = 0x80004200u;
    runtime.cpu().gpr[5] = 0;
    if (!invoke_guest(0x801A735Cu, &runtime.cpu()) || runtime.cpu().gpr[3] != 1u ||
        runtime.memory().read32(0x80004200u) != 0) {
        return fail("task job send");
    }
    InvokeIndirectCpu(0x80100000u, &runtime.cpu());
    InvokeIndirectCpu(0x80100000u, &runtime.cpu());
    if (!invoke_guest(0x801AAAA4u, &runtime.cpu())) {
        return fail("wakeup thread");
    }
    if (!invoke_guest(0x8016E37Cu, &runtime.cpu()) || !invoke_guest(0x80170F2Cu, &runtime.cpu())) {
        return fail("gx tex load");
    }
    if (!invoke_guest(0x8017039Cu, &runtime.cpu()) || !invoke_guest(0x801733E0u, &runtime.cpu())) {
        return fail("gx strap draw");
    }
    // The ripper now runs translated code, not the removed shortcut HLE.
    if (find_hle_native(0x8000B53Cu) != nullptr) {
        return fail("obsolete process rip override");
    }
    if (!invoke_guest(0x800077C8u, &runtime.cpu()) || runtime.cpu().gpr[3] != 1u) {
        return fail("strap check input");
    }
    if (find_hle_native(0x8055533Cu) != nullptr) {
        return fail("interior prolog address must not be an entry");
    }
    if (last_missing_target() != 0x80100000u) {
        return fail("repeat missing");
    }

    const std::uint32_t egg = 0x80010000u;
    const std::uint32_t handle = 0x80010040u;
    const std::uint32_t start = handle + 80u;
    const std::uint32_t end = start + 0x8000u;
    runtime.memory().write32(egg + 16u, handle);
    runtime.memory().write32(handle, 0x45585048u);
    runtime.memory().write32(handle + 24u, start);
    runtime.memory().write32(handle + 28u, end);
    runtime.memory().write32(handle + 56u, 4u);
    runtime.memory().write16(start, 0x4652);
    runtime.memory().write16(start + 2u, 0);
    runtime.memory().write32(start + 4u, end - start - 16u);
    runtime.memory().write32(start + 8u, 0);
    runtime.memory().write32(start + 12u, 0);
    runtime.memory().write32(handle + 60u, start);
    runtime.memory().write32(handle + 64u, start);
    runtime.cpu().gpr[3] = egg;
    runtime.cpu().gpr[4] = 256;
    runtime.cpu().gpr[5] = 4;
    if (!invoke_guest(0x80226C04u, &runtime.cpu()) || runtime.cpu().gpr[3] < start || runtime.cpu().gpr[3] >= end) {
        return fail("expheap alloc");
    }
    runtime.cpu().gpr[3] = egg;
    runtime.cpu().gpr[4] = 4096;
    runtime.cpu().gpr[5] = 32;
    if (!invoke_guest(0x80226C04u, &runtime.cpu()) || runtime.cpu().gpr[3] < start || runtime.cpu().gpr[3] >= end ||
        (runtime.cpu().gpr[3] & 31u) != 0) {
        return fail("expheap alloc 4k");
    }

    runtime.shutdown();
    return 0;
}
