#include "mkwv/runtime/hle_fifo.hpp"

namespace mkwv::runtime {

void HleFifo::reset() {
    fifo_.reset();
}

bool HleFifo::drain(gx::Processor& processor, bool big_endian) {
    if (fifo_.size() == 0) {
        return true;
    }
    return processor.process(fifo_.data(), fifo_.size(), big_endian);
}

} // namespace mkwv::runtime
