#pragma once

#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/cpu_context.hpp"
#include "mkwv/runtime/guest_image.hpp"
#include "mkwv/runtime/hle_fifo.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/overlay.hpp"

#include "mkwv/gx/fifo.hpp"

namespace mkwv::runtime {

void stub_gx_triangle(CpuContext* ctx);
void guest_probe(CpuContext* ctx);

class Runtime {
public:
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime();

    bool initialize(const char* overlay_root, const char* guest_data_root = nullptr);
    void shutdown();
    bool tick_stub();
    bool tick_probe();
    bool tick_start();
    void set_present(HlePresentFn present);

    CompactMemory& memory() { return memory_; }
    OverlayCache& overlay() { return overlay_; }
    HleFifo& fifo() { return fifo_; }
    gx::Processor& processor() { return processor_; }
    CpuContext& cpu() { return cpu_; }
    GuestImageStats image() const { return image_; }

private:
    CompactMemory memory_{};
    OverlayCache overlay_{};
    HleFifo fifo_{};
    gx::Processor processor_{};
    CpuContext cpu_{};
    GuestImageStats image_{};
    bool bound_ = false;
};

} // namespace mkwv::runtime
