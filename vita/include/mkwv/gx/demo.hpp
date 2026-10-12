#pragma once

#include "mkwv/gx/fifo.hpp"

namespace mkwv::gx {

// Builds a GX FIFO for one frame: ground quads, strip, fan, spinning triangle.
bool build_demo_frame(Fifo& fifo, float time_seconds);

// Decode that FIFO and submit through the active GLES path.
void render_demo(float time_seconds);

} // namespace mkwv::gx
