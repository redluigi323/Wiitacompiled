#include "mkwv/gxm/gpu_memory.hpp"

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/types.h>

#include <cstring>

namespace mkwv::gxm {
namespace {

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

SceKernelMemBlockType kernel_type(Pool pool) {
    switch (pool) {
    case Pool::Cdram:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    case Pool::LpddrUncached:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    case Pool::Lpddr:
    default:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
    }
}

} // namespace

Buffer allocate(Pool pool, std::size_t size, unsigned attribs) {
    Buffer buffer;
    const bool cdram = pool == Pool::Cdram;
    const std::size_t alignment = cdram ? 256u * 1024u : 4u * 1024u;
    size = align_up(size, alignment);
    if (size == 0) {
        return buffer;
    }

    SceKernelAllocMemBlockOpt opt;
    std::memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = static_cast<SceSize>(alignment);

    buffer.uid = sceKernelAllocMemBlock("mkwv_gpu", kernel_type(pool), static_cast<SceSize>(size),
                                        cdram ? nullptr : &opt);
    if (buffer.uid < 0) {
        buffer.uid = -1;
        return buffer;
    }
    if (sceKernelGetMemBlockBase(buffer.uid, &buffer.host) < 0) {
        sceKernelFreeMemBlock(buffer.uid);
        buffer = {};
        return buffer;
    }
    if (sceGxmMapMemory(buffer.host, size, static_cast<SceGxmMemoryAttribFlags>(attribs)) < 0) {
        sceKernelFreeMemBlock(buffer.uid);
        buffer = {};
        return buffer;
    }
    buffer.size = size;
    return buffer;
}

void deallocate(Buffer& buffer) {
    if (buffer.host && buffer.size) {
        sceGxmUnmapMemory(buffer.host);
    }
    if (buffer.uid >= 0) {
        sceKernelFreeMemBlock(buffer.uid);
    }
    buffer = {};
}

Buffer allocate_vertex_usse(std::size_t size, unsigned* usse_offset) {
    Buffer buffer = allocate(Pool::LpddrUncached, size, SCE_GXM_MEMORY_ATTRIB_READ);
    if (!buffer.host || sceGxmMapVertexUsseMemory(buffer.host, buffer.size, usse_offset) < 0) {
        deallocate(buffer);
    }
    return buffer;
}

Buffer allocate_fragment_usse(std::size_t size, unsigned* usse_offset) {
    Buffer buffer = allocate(Pool::LpddrUncached, size, SCE_GXM_MEMORY_ATTRIB_READ);
    if (!buffer.host || sceGxmMapFragmentUsseMemory(buffer.host, buffer.size, usse_offset) < 0) {
        deallocate(buffer);
    }
    return buffer;
}

} // namespace mkwv::gxm
