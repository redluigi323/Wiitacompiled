#pragma once

#include <cstddef>
#include <cstdint>

namespace mkwv::gxm {

// GPU-visible allocations. CDRAM is the 128 MiB VRAM pool; LPDDR is main RAM.
// USSE mappings are required for vertex/fragment microcode rings.
enum class Pool {
    Cdram,
    Lpddr,
    LpddrUncached,
};

struct Buffer {
    void* host = nullptr;
    std::size_t size = 0;
    int uid = -1;
};

Buffer allocate(Pool pool, std::size_t size, unsigned attribs);
void deallocate(Buffer& buffer);

Buffer allocate_vertex_usse(std::size_t size, unsigned* usse_offset);
Buffer allocate_fragment_usse(std::size_t size, unsigned* usse_offset);

} // namespace mkwv::gxm
