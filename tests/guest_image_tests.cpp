#include "mkwv/runtime/guest_image.hpp"
#include "mkwv/runtime/runtime.hpp"

#include "memory.h"

#include <cstdio>
#include <cstring>

#ifndef MKWV_GUEST_BLOBS_DIR
#define MKWV_GUEST_BLOBS_DIR ""
#endif

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "guest_image: %s\n", message);
    return 1;
}

} // namespace

int main() {
    using namespace mkwv::runtime;
    if (MKWV_GUEST_BLOBS_DIR[0] == '\0') {
        return fail("no blob dir");
    }

    char init_path[320];
    std::snprintf(init_path, sizeof(init_path), "%s/_init.bin", MKWV_GUEST_BLOBS_DIR);
    std::FILE* init = std::fopen(init_path, "rb");
    if (init == nullptr) {
        return fail("open _init.bin");
    }
    std::uint8_t raw[4]{};
    if (std::fread(raw, 1, 4, init) != 4) {
        std::fclose(init);
        return fail("read _init");
    }
    std::fclose(init);
    const std::uint32_t expect = (static_cast<std::uint32_t>(raw[0]) << 24) | (static_cast<std::uint32_t>(raw[1]) << 16) |
                                 (static_cast<std::uint32_t>(raw[2]) << 8) | raw[3];

    Runtime runtime;
    if (!runtime.initialize("/tmp/mkwv-guest-overlay", MKWV_GUEST_BLOBS_DIR)) {
        return fail("runtime init");
    }
    const GuestImageStats image = runtime.image();
    if (!image.loaded || image.sections != 11 || image.bytes != 7701072u) {
        std::fprintf(stderr, "guest_image: loaded=%d sections=%u bytes=%u\n", image.loaded ? 1 : 0, image.sections,
                     image.bytes);
        return fail("image stats");
    }
    if (image.init_word != expect) {
        std::fprintf(stderr, "guest_image: init_word=%08x expect=%08x\n", image.init_word, expect);
        return fail("init word");
    }
    if (!Memory::Contains(0x80004000u, 4) || Memory::Read32(0x80004000u) != expect) {
        return fail("Memory::Read32");
    }
    if (!runtime.tick_probe() || runtime.cpu().gpr[3] != expect) {
        return fail("probe");
    }
    if (runtime.cpu().gpr[2] != kSda2Base || runtime.cpu().gpr[13] != kSdaBase) {
        return fail("sda");
    }
    if (!runtime.tick_stub() || runtime.processor().draw_count != 1) {
        return fail("stub still works");
    }
    runtime.shutdown();
    return 0;
}
