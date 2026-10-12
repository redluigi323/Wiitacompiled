#pragma once

namespace mkwv::gx {

struct Processor;

// GX backend boundary. FIFO decode stays on the Processor; this object owns the
// host GPU. The live path is VitaGL GLES (DriftDroid/Aurora's GLES idea, hosted
// on Vita through VitaGL with HAVE_VITA3K_SUPPORT). The old GXM guess-work
// submit remains in-tree but is not linked.
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual void begin_frame() = 0;
    virtual void draw_debug_geometry() = 0;
    virtual void present_processor(const Processor& processor) = 0;
    virtual void end_frame() = 0;
};

Backend* create_gl_backend();

} // namespace mkwv::gx
