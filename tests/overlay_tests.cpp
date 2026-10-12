#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/guest_access.hpp"
#include "mkwv/runtime/runtime.hpp"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "overlay: %s\n", message);
    return 1;
}

void fn_a(mkwv::runtime::CpuContext* ctx) {
    ctx->gpr[3] = 1;
}
void fn_b(mkwv::runtime::CpuContext* ctx) {
    ctx->gpr[3] = 2;
}
void fn_c(mkwv::runtime::CpuContext* ctx) {
    ctx->gpr[3] = 3;
}

} // namespace

int main() {
    using namespace mkwv::runtime;

    char root[160];
#if defined(_WIN32)
    std::snprintf(root, sizeof(root), "mkwv-overlay-test");
    _mkdir(root);
#else
    std::snprintf(root, sizeof(root), "/tmp/mkwv-overlay-test-%d", static_cast<int>(getpid()));
#endif

    OverlayCache cache;
    cache.reset();
    if (!cache.set_root(root)) {
        return fail("set_root");
    }
    if (!cache.register_native(0x80001000u, fn_a, 400u * 1024u, "a") ||
        !cache.register_native(0x80002000u, fn_b, 400u * 1024u, "b") ||
        !cache.register_native(0x80003000u, fn_c, 400u * 1024u, "c")) {
        return fail("register");
    }
    const std::uint8_t marker[] = {1, 2, 3, 4};
    if (!cache.write_shard_file(0x80001000u, marker, sizeof(marker), "a")) {
        return fail("write shard");
    }
    if (cache.resolve(0x80001000u) != fn_a || !cache.is_resident(0x80001000u)) {
        return fail("resolve a");
    }
    if (cache.resolve(0x80002000u) != fn_b) {
        return fail("resolve b");
    }
    if (cache.resolve(0x80003000u) != fn_c) {
        return fail("resolve c");
    }
    if (cache.is_resident(0x80001000u)) {
        return fail("lru should evict a");
    }
    if (!cache.is_resident(0x80002000u) || !cache.is_resident(0x80003000u)) {
        return fail("b/c resident");
    }
    if (cache.stats().evictions == 0 || cache.stats().misses < 3) {
        return fail("overlay stats");
    }
    if (cache.resolve(0x80001000u) != fn_a || !cache.is_resident(0x80001000u)) {
        return fail("reload a");
    }

    CompactMemory memory;
    HleFifo fifo;
    if (!memory.initialize()) {
        return fail("memory");
    }
    bind_guest_access(&memory, &fifo);
    memory.write32(kMem1Cached + 0x100, 0xAABBCCDDu);
    if (guest_read32(kMem1Cached + 0x100) != 0xAABBCCDDu) {
        return fail("guest ram");
    }
    guest_write32(kFifoBase, 0x00000000u);
    guest_write8(kFifoBase, 0x61);
    if (fifo.size() == 0 || memory.host_pointer(kFifoBase, 4) != nullptr) {
        return fail("fifo mmio");
    }
    unbind_guest_access();

    Runtime runtime;
    if (!runtime.initialize(root)) {
        return fail("runtime init");
    }
    if (!runtime.tick_stub()) {
        return fail("tick stub");
    }
    if (runtime.processor().draw_count != 1 || runtime.processor().vert_count != 3) {
        std::fprintf(stderr, "overlay: draws=%u verts=%u fifo=%u\n", runtime.processor().draw_count,
                     runtime.processor().vert_count, runtime.fifo().size());
        return fail("stub triangle");
    }
    if (runtime.cpu().gpr[1] != kGuestStack || runtime.cpu().gpr[3] == 0) {
        return fail("cpu context");
    }
    const auto missing_before = missing_target_count();
    if (invoke_guest(0x8000FFFFu, &runtime.cpu())) {
        return fail("unknown target");
    }
    if (missing_target_count() <= missing_before) {
        return fail("missing count");
    }
    runtime.shutdown();
    return 0;
}
