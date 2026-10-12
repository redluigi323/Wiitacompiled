#include "mkwv/runtime/runtime.hpp"
#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/guest_access.hpp"
#include "mkwv/runtime/guest_image.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/hle_os.hpp"
#include "mkwv/platform/data_root.hpp"

#include "memory.h"

#include <cstring>

namespace mkwv::runtime {
namespace {

void pack_pos_clr(gx::Fifo& fifo, float x, float y, float z, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                  std::uint8_t a) {
    fifo.write_f32(x);
    fifo.write_f32(y);
    fifo.write_f32(z);
    fifo.write_u8(r);
    fifo.write_u8(g);
    fifo.write_u8(b);
    fifo.write_u8(a);
}

} // namespace

void stub_gx_triangle(CpuContext* ctx) {
    gx::Fifo built;
    gx::configure_pos_clr0_direct(built, gx::VtxFmt0);
    gx::Fifo pack;
    pack_pos_clr(pack, 0.0f, 0.7f, 0.0f, 255, 64, 80, 255);
    pack_pos_clr(pack, -0.7f, -0.6f, 0.0f, 64, 180, 255, 255);
    pack_pos_clr(pack, 0.7f, -0.6f, 0.0f, 255, 220, 64, 255);
    built.draw(gx::Triangles, gx::VtxFmt0, 3, pack.data(), pack.size());
    guest_write_burst(kFifoBase, built.data(), built.size());
    if (ctx != nullptr) {
        ctx->gpr[3] = built.size();
    }
}

void guest_probe(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    ctx->gpr[3] = MemoryInline::FlatRead32(0x80004000u);
    ctx->gpr[4] = MemoryInline::FlatRead32(0x805102E0u);
}

bool Runtime::initialize(const char* overlay_root, const char* guest_data_root) {
    shutdown();
    if (!memory_.initialize()) {
        return false;
    }
    cpu_ = CpuContext{};
    cpu_.gpr[1] = kGuestStack;
    cpu_.gpr[2] = kSda2Base;
    cpu_.gpr[13] = kSdaBase;
    memory_.write32(kGuestStack, 0);
    bind_guest_access(&memory_, &fifo_);
    bind_dispatch(&overlay_);
    bind_hle_frame(&fifo_, &processor_, nullptr);
    register_hle_natives();
    bound_ = true;
    if (overlay_root != nullptr) {
        overlay_.set_root(overlay_root);
    }
    overlay_.register_native(kStubGxTriangle, stub_gx_triangle, 4096, "stub_gx_triangle");
    overlay_.register_native(kGuestProbe, guest_probe, 256, "guest_probe");
    if (overlay_root != nullptr) {
        const std::uint8_t marker[4] = {0x7F, 0xFF, 0x00, 0x00};
        overlay_.write_shard_file(kStubGxTriangle, marker, sizeof(marker), "stub_gx_triangle");
        overlay_.write_shard_file(kGuestProbe, marker, sizeof(marker), "guest_probe");
    }
    g_persistentCpuContext = cpu_;
    g_currentCpuContext = &cpu_;
    const char* guest_root = guest_data_root;
    if (guest_root == nullptr || guest_root[0] == '\0') {
        guest_root = mkwv::platform::kGuestDir;
    }
    image_ = load_guest_image(memory_, guest_root);
    write_os_lowmem(memory_);
    publish_dvd_fst();
    return overlay_.resolve(kStubGxTriangle) != nullptr && overlay_.resolve(kGuestProbe) != nullptr;
}

Runtime::~Runtime() {
    shutdown();
}

void Runtime::shutdown() {
    if (bound_) {
        unbind_hle_frame();
        unbind_dispatch();
        unbind_guest_access();
        bound_ = false;
    }
    overlay_.reset();
    fifo_.reset();
    processor_.reset();
    cpu_ = CpuContext{};
    image_ = {};
    g_currentCpuContext = nullptr;
    memory_.shutdown();
}

bool Runtime::tick_stub() {
    fifo_.reset();
    processor_.reset();
    if (!invoke_guest(kStubGxTriangle, &cpu_)) {
        return false;
    }
    return fifo_.drain(processor_);
}

bool Runtime::tick_probe() {
    cpu_.gpr[3] = 0;
    cpu_.gpr[4] = 0;
    return invoke_guest(kGuestProbe, &cpu_);
}

bool Runtime::tick_start() {
    fifo_.reset();
    processor_.reset();
    cpu_.gpr[3] = 0;
    cpu_.lr = 0;
    g_persistentCpuContext = cpu_;
    g_currentCpuContext = &cpu_;
    run_rel_constructors(&cpu_);
    g_persistentCpuContext = cpu_;
    return invoke_guest(kGuestStart, &cpu_);
}

void Runtime::set_present(HlePresentFn present) {
    bind_hle_frame(&fifo_, &processor_, present);
}

} // namespace mkwv::runtime
