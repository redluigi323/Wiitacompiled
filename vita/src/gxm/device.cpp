#include "mkwv/gxm/device.hpp"
#include "mkwv/gxm/gpu_memory.hpp"
#include "mkwv/gxm/shacc.hpp"
#include "mkwv/gx/cg_emit.hpp"

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/types.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
extern const uint8_t clear_v_gxp_start;
extern const uint8_t clear_f_gxp_start;
extern const uint8_t color_v_gxp_start;
extern const uint8_t color_f_gxp_start;
extern const uint8_t texture_v_gxp_start;
extern const uint8_t texture_f_gxp_start;
}

namespace mkwv::gxm {
namespace {

constexpr unsigned kWidth = 960;
constexpr unsigned kHeight = 544;
constexpr unsigned kStride = 960;
constexpr unsigned kBufferCount = 3;
constexpr unsigned kPendingSwaps = 2;
// Big enough for the largest texture this port expands to RGBA8 plus its mips: 1024x1024 is 4 MiB and
// ~5.3 MiB with a full mip chain. The old 1 MiB could not even hold the 832x456 strap image
// (1,517,568 bytes), so bind_fragment_texture rejected it and every textured draw silently fell back
// to the 8x8 placeholder texture.
constexpr std::size_t kTextureBytes = 8u * 1024u * 1024u;

constexpr std::size_t kParameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
constexpr std::size_t kVdmRingSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
constexpr std::size_t kVertexRingSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
constexpr std::size_t kFragmentRingSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
constexpr std::size_t kFragmentUsseRingSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
constexpr std::size_t kPatcherBufferSize = 64u * 1024u;
constexpr std::size_t kPatcherVertexUsseSize = 16u * 1024u;
constexpr std::size_t kPatcherFragmentUsseSize = 16u * 1024u;
// Real hardware only needs SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE (2 KiB).
// Vita3K placement-news its emulator context into this buffer, so 2 KiB fails
// sceGxmCreateContext with INVALID_VALUE.
constexpr std::size_t kHostContextSize = 512u * 1024u;

struct DisplayJob {
    void* address = nullptr;
};

struct ClearVertex {
    float x, y;
};

SceGxmDepthFunc map_depth_func(bool test, unsigned gx_compare) {
    if (!test) {
        return SCE_GXM_DEPTH_FUNC_ALWAYS;
    }
    switch (gx_compare) {
    case 0:
        return SCE_GXM_DEPTH_FUNC_NEVER;
    case 1:
        return SCE_GXM_DEPTH_FUNC_LESS;
    case 2:
        return SCE_GXM_DEPTH_FUNC_EQUAL;
    case 3:
        return SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
    case 4:
        return SCE_GXM_DEPTH_FUNC_GREATER;
    case 5:
        return SCE_GXM_DEPTH_FUNC_NOT_EQUAL;
    case 6:
        return SCE_GXM_DEPTH_FUNC_GREATER_EQUAL;
    default:
        return SCE_GXM_DEPTH_FUNC_ALWAYS;
    }
}

SceGxmCullMode map_cull(unsigned gx_cull) {
    switch (gx_cull) {
    case 1:
        return SCE_GXM_CULL_CCW;
    case 2:
        return SCE_GXM_CULL_CW;
    default:
        return SCE_GXM_CULL_NONE;
    }
}

void apply_depth(SceGxmContext* context, bool test, bool write, unsigned gx_compare) {
    const SceGxmDepthFunc func = map_depth_func(test, gx_compare);
    const SceGxmDepthWriteMode mode = write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    sceGxmSetFrontDepthFunc(context, func);
    sceGxmSetBackDepthFunc(context, func);
    sceGxmSetFrontDepthWriteEnable(context, mode);
    sceGxmSetBackDepthWriteEnable(context, mode);
}

SceGxmBlendInfo make_blend(SceGxmBlendFactor src, SceGxmBlendFactor dst, SceGxmBlendFunc func) {
    SceGxmBlendInfo blend{};
    blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
    blend.colorFunc = func;
    blend.alphaFunc = func;
    blend.colorSrc = static_cast<std::uint8_t>(src);
    blend.colorDst = static_cast<std::uint8_t>(dst);
    blend.alphaSrc = static_cast<std::uint8_t>(src);
    blend.alphaDst = static_cast<std::uint8_t>(dst);
    return blend;
}

SceGxmBlendInfo make_blend(SceGxmBlendFactor src, SceGxmBlendFactor dst) {
    return make_blend(src, dst, SCE_GXM_BLEND_FUNC_ADD);
}

SceGxmTextureAddrMode map_wrap(unsigned gx_wrap) {
    switch (gx_wrap) {
    case 0:
        return SCE_GXM_TEXTURE_ADDR_CLAMP;
    case 2:
        return SCE_GXM_TEXTURE_ADDR_MIRROR;
    default:
        return SCE_GXM_TEXTURE_ADDR_REPEAT;
    }
}

void fill_fallback_texture(std::uint32_t* pixels, unsigned size) {
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
            const bool cell = ((x >> 3) ^ (y >> 3)) & 1u;
            const unsigned grit = ((x * 13u) ^ (y * 7u)) & 7u;
            std::uint8_t r = static_cast<std::uint8_t>((cell ? 52u : 38u) + grit);
            std::uint8_t g = static_cast<std::uint8_t>((cell ? 56u : 42u) + grit);
            std::uint8_t b = static_cast<std::uint8_t>((cell ? 70u : 58u) + grit);
            if ((y & 15u) == 0u) {
                r = 210;
                g = 196;
                b = 72;
            }
            pixels[y * size + x] = static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
                                   (static_cast<std::uint32_t>(b) << 16) | 0xFF000000u;
        }
    }
}

void* patcher_alloc(void* /*user*/, SceSize size) {
    return std::malloc(size);
}

void patcher_free(void* /*user*/, void* mem) {
    std::free(mem);
}

void display_callback(const void* payload) {
    const auto* job = static_cast<const DisplayJob*>(payload);
    SceDisplayFrameBuf framebuf;
    std::memset(&framebuf, 0, sizeof(framebuf));
    framebuf.size = sizeof(framebuf);
    framebuf.base = job->address;
    framebuf.pitch = kStride;
    framebuf.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    framebuf.width = kWidth;
    framebuf.height = kHeight;
    sceDisplaySetFrameBuf(&framebuf, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
}

const SceGxmProgram* program_from_binary(const uint8_t* start) {
    return reinterpret_cast<const SceGxmProgram*>(start);
}

Device* fail_init(Device* device, const char* where) {
    std::printf("mkwv gxm: %s failed\n", where);
    destroy_device(device);
    return nullptr;
}

} // namespace

struct Device {
    SceGxmContext* context = nullptr;
    SceGxmRenderTarget* render_target = nullptr;
    SceGxmShaderPatcher* patcher = nullptr;
    SceGxmShaderPatcherId clear_vid = {};
    SceGxmShaderPatcherId clear_fid = {};
    SceGxmShaderPatcherId color_vid = {};
    SceGxmShaderPatcherId color_fid = {};
    SceGxmShaderPatcherId texture_vid = {};
    SceGxmShaderPatcherId texture_fid = {};
    SceGxmVertexProgram* clear_vprog = nullptr;
    SceGxmFragmentProgram* clear_fprog = nullptr;
    SceGxmVertexProgram* color_vprog = nullptr;
    SceGxmFragmentProgram* color_fprog = nullptr;
    SceGxmFragmentProgram* color_modulate_fprog = nullptr;
    SceGxmFragmentProgram* color_alpha_fprog = nullptr;
    SceGxmFragmentProgram* color_subtract_fprog = nullptr;
    SceGxmVertexProgram* texture_vprog = nullptr;
    SceGxmFragmentProgram* texture_fprog = nullptr;
    SceGxmFragmentProgram* texture_modulate_fprog = nullptr;
    SceGxmShaderPatcherId tev_vid = {};
    SceGxmVertexProgram* tev_vprog = nullptr;
    const SceGxmProgramParameter* tev_wvp_param = nullptr;
    void* tev_v_gxp = nullptr;
    bool tev_runtime = false;
    static constexpr unsigned kTevCache = 8;
    struct TevSlot {
        std::uint64_t key = 0;
        void* gxp = nullptr;
        SceGxmShaderPatcherId fid = {};
        SceGxmFragmentProgram* fprog = nullptr;
    };
    TevSlot tev_slots[kTevCache]{};
    unsigned tev_slot_count = 0;
    const SceGxmProgramParameter* clear_color_param = nullptr;
    const SceGxmProgramParameter* wvp_param = nullptr;
    const SceGxmProgramParameter* texture_wvp_param = nullptr;
    Buffer texture_mem{};
    Buffer texture_mem1{};
    SceGxmTexture fragment_tex{};
    SceGxmTexture fragment_tex1{};
    bool fragment_tex_valid = false;
    bool fragment_tex1_valid = false;
    const void* bound_src = nullptr;
    std::uint32_t bound_bytes = 0;
    unsigned bound_w = 0;
    unsigned bound_h = 0;
    unsigned bound_layout = 0;
    unsigned bound_wrap_s = 1;
    unsigned bound_wrap_t = 1;

    Buffer display[kBufferCount]{};
    SceGxmColorSurface color_surface[kBufferCount]{};
    SceGxmSyncObject* sync[kBufferCount]{};
    Buffer depth{};
    SceGxmDepthStencilSurface depth_surface{};

    Buffer vdm{};
    Buffer vertex_ring{};
    Buffer fragment_ring{};
    Buffer fragment_usse{};
    Buffer patcher_buffer{};
    Buffer patcher_vertex_usse{};
    Buffer patcher_fragment_usse{};
    Buffer vertices{};
    Buffer indices{};
    // Return codes of the GXM calls that are otherwise discarded. draw_textured_mesh used to return
    // true without ever checking sceGxmDraw, so a rejected draw looked identical to a successful one.
    int err_begin_scene = 0;
    int err_clear_draw = 0;
    int err_mesh_prog = 0;
    int err_mesh_stream = 0;
    int err_mesh_tex = 0;
    int err_mesh_draw = 0;
    // The clear triangle is the only draw that uses the FRAGMENT default uniform buffer; every mesh
    // draw uses the VERTEX one to upload wvp. None of them checked it. A failed reserve leaves wvp
    // unwritten, which collapses every vertex to a degenerate position while sceGxmDraw still
    // returns 0 - exactly the "GXM accepts everything and nothing appears" symptom.
    int err_vunif = 0;
    int err_vunif_null = 0;
    int err_probe_clear = 0;
    int err_probe_reserve = 0;
    int err_probe_null = 0;
    std::size_t vert_used = 0;
    std::size_t idx_used = 0;
    void* host_context_mem = nullptr;

    unsigned back = 0;
    unsigned front = 0;
    unsigned frame = 0;
    bool ready = false;
};

std::size_t align_up_bytes(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

bool bump_mesh(Device* device, std::size_t vert_bytes, std::size_t vert_align, std::size_t idx_bytes, void** verts,
               void** idxs) {
    const std::size_t vert_off = align_up_bytes(device->vert_used, vert_align);
    const std::size_t idx_off = align_up_bytes(device->idx_used, sizeof(std::uint16_t));
    if (vert_off + vert_bytes > device->vertices.size || idx_off + idx_bytes > device->indices.size) {
        return false;
    }
    *verts = static_cast<std::uint8_t*>(device->vertices.host) + vert_off;
    *idxs = static_cast<std::uint8_t*>(device->indices.host) + idx_off;
    device->vert_used = vert_off + vert_bytes;
    device->idx_used = idx_off + idx_bytes;
    return true;
}

Device* create_device() {
    auto* device = new (std::nothrow) Device();
    if (!device) {
        return nullptr;
    }

    SceGxmInitializeParams init;
    std::memset(&init, 0, sizeof(init));
    init.flags = SCE_GXM_INITIALIZE_FLAG_DEFAULT;
    init.displayQueueMaxPendingCount = kPendingSwaps;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = sizeof(DisplayJob);
    init.parameterBufferSize = kParameterBufferSize;
    const int init_err = sceGxmInitialize(&init);
    if (init_err < 0) {
        std::printf("mkwv gxm: sceGxmInitialize 0x%08X\n", init_err);
        delete device;
        return nullptr;
    }

    unsigned fragment_usse_off = 0;
    unsigned patcher_vusse_off = 0;
    unsigned patcher_fusse_off = 0;
    device->vdm = allocate(Pool::LpddrUncached, kVdmRingSize,
                           SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    device->vertex_ring = allocate(Pool::LpddrUncached, kVertexRingSize,
                                   SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    device->fragment_ring = allocate(Pool::LpddrUncached, kFragmentRingSize,
                                     SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    device->fragment_usse = allocate_fragment_usse(kFragmentUsseRingSize, &fragment_usse_off);
    device->host_context_mem = std::malloc(kHostContextSize);
    if (!device->vdm.host || !device->vertex_ring.host || !device->fragment_ring.host ||
        !device->fragment_usse.host || !device->host_context_mem) {
        return fail_init(device, "ring buffers");
    }

    SceGxmContextParams context_params;
    std::memset(&context_params, 0, sizeof(context_params));
    context_params.hostMem = device->host_context_mem;
    context_params.hostMemSize = kHostContextSize;
    context_params.vdmRingBufferMem = device->vdm.host;
    context_params.vdmRingBufferMemSize = device->vdm.size;
    context_params.vertexRingBufferMem = device->vertex_ring.host;
    context_params.vertexRingBufferMemSize = device->vertex_ring.size;
    context_params.fragmentRingBufferMem = device->fragment_ring.host;
    context_params.fragmentRingBufferMemSize = device->fragment_ring.size;
    context_params.fragmentUsseRingBufferMem = device->fragment_usse.host;
    context_params.fragmentUsseRingBufferMemSize = device->fragment_usse.size;
    context_params.fragmentUsseRingBufferOffset = fragment_usse_off;
    if (sceGxmCreateContext(&context_params, &device->context) < 0) {
        return fail_init(device, "sceGxmCreateContext");
    }

    SceGxmRenderTargetParams rt;
    std::memset(&rt, 0, sizeof(rt));
    rt.width = kWidth;
    rt.height = kHeight;
    rt.scenesPerFrame = 1;
    rt.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rt.driverMemBlock = static_cast<SceUID>(-1);
    if (sceGxmCreateRenderTarget(&rt, &device->render_target) < 0) {
        return fail_init(device, "sceGxmCreateRenderTarget");
    }

    const std::size_t color_size = kStride * kHeight * 4u;
    for (unsigned i = 0; i < kBufferCount; ++i) {
        device->display[i] = allocate(Pool::Cdram, color_size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
        if (!device->display[i].host || sceGxmSyncObjectCreate(&device->sync[i]) < 0) {
            return fail_init(device, "display buffer");
        }
        std::memset(device->display[i].host, 0, color_size);
        if (sceGxmColorSurfaceInit(&device->color_surface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                   SCE_GXM_COLOR_SURFACE_TILED, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                   SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, kWidth, kHeight, kStride,
                                   device->display[i].host) < 0) {
            return fail_init(device, "color surface");
        }
    }

    const unsigned ds_stride = (kWidth + 31u) & ~31u;
    const std::size_t depth_size = ds_stride * kHeight * 4u;
    device->depth = allocate(Pool::Cdram, depth_size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (!device->depth.host) {
        destroy_device(device);
        return nullptr;
    }
    if (sceGxmDepthStencilSurfaceInit(&device->depth_surface, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                      SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, ds_stride, device->depth.host,
                                      nullptr) < 0) {
        destroy_device(device);
        return nullptr;
    }

    device->patcher_buffer = allocate(Pool::LpddrUncached, kPatcherBufferSize,
                                      SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    device->patcher_vertex_usse = allocate_vertex_usse(kPatcherVertexUsseSize, &patcher_vusse_off);
    device->patcher_fragment_usse = allocate_fragment_usse(kPatcherFragmentUsseSize, &patcher_fusse_off);
    if (!device->patcher_buffer.host || !device->patcher_vertex_usse.host || !device->patcher_fragment_usse.host) {
        destroy_device(device);
        return nullptr;
    }

    SceGxmShaderPatcherParams patcher_params;
    std::memset(&patcher_params, 0, sizeof(patcher_params));
    patcher_params.hostAllocCallback = &patcher_alloc;
    patcher_params.hostFreeCallback = &patcher_free;
    patcher_params.bufferMem = device->patcher_buffer.host;
    patcher_params.bufferMemSize = device->patcher_buffer.size;
    patcher_params.vertexUsseMem = device->patcher_vertex_usse.host;
    patcher_params.vertexUsseMemSize = device->patcher_vertex_usse.size;
    patcher_params.vertexUsseOffset = patcher_vusse_off;
    patcher_params.fragmentUsseMem = device->patcher_fragment_usse.host;
    patcher_params.fragmentUsseMemSize = device->patcher_fragment_usse.size;
    patcher_params.fragmentUsseOffset = patcher_fusse_off;
    if (sceGxmShaderPatcherCreate(&patcher_params, &device->patcher) < 0) {
        destroy_device(device);
        return nullptr;
    }

    const SceGxmProgram* clear_v = program_from_binary(&clear_v_gxp_start);
    const SceGxmProgram* clear_f = program_from_binary(&clear_f_gxp_start);
    const SceGxmProgram* color_v = program_from_binary(&color_v_gxp_start);
    const SceGxmProgram* color_f = program_from_binary(&color_f_gxp_start);
    const SceGxmProgram* texture_v = program_from_binary(&texture_v_gxp_start);
    const SceGxmProgram* texture_f = program_from_binary(&texture_f_gxp_start);
    if (sceGxmProgramCheck(clear_v) != 0 || sceGxmProgramCheck(clear_f) != 0 ||
        sceGxmProgramCheck(color_v) != 0 || sceGxmProgramCheck(color_f) != 0 ||
        sceGxmProgramCheck(texture_v) != 0 || sceGxmProgramCheck(texture_f) != 0) {
        destroy_device(device);
        return nullptr;
    }
    if (sceGxmShaderPatcherRegisterProgram(device->patcher, clear_v, &device->clear_vid) < 0 ||
        sceGxmShaderPatcherRegisterProgram(device->patcher, clear_f, &device->clear_fid) < 0 ||
        sceGxmShaderPatcherRegisterProgram(device->patcher, color_v, &device->color_vid) < 0 ||
        sceGxmShaderPatcherRegisterProgram(device->patcher, color_f, &device->color_fid) < 0 ||
        sceGxmShaderPatcherRegisterProgram(device->patcher, texture_v, &device->texture_vid) < 0 ||
        sceGxmShaderPatcherRegisterProgram(device->patcher, texture_f, &device->texture_fid) < 0) {
        destroy_device(device);
        return nullptr;
    }

    const SceGxmProgramParameter* clear_pos = sceGxmProgramFindParameterByName(clear_v, "aPosition");
    const SceGxmProgramParameter* color_pos = sceGxmProgramFindParameterByName(color_v, "aPosition");
    const SceGxmProgramParameter* color_col = sceGxmProgramFindParameterByName(color_v, "aColor");
    const SceGxmProgramParameter* texture_pos = sceGxmProgramFindParameterByName(texture_v, "aPosition");
    const SceGxmProgramParameter* texture_uv = sceGxmProgramFindParameterByName(texture_v, "aTexcoord");
    device->clear_color_param = sceGxmProgramFindParameterByName(clear_f, "uClearColor");
    device->wvp_param = sceGxmProgramFindParameterByName(color_v, "wvp");
    device->texture_wvp_param = sceGxmProgramFindParameterByName(texture_v, "wvp");
    if (!clear_pos || !color_pos || !color_col || !texture_pos || !texture_uv || !device->clear_color_param ||
        !device->wvp_param || !device->texture_wvp_param) {
        return fail_init(device, "shader parameters");
    }

    SceGxmVertexAttribute clear_attr{};
    clear_attr.streamIndex = 0;
    clear_attr.offset = 0;
    clear_attr.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    clear_attr.componentCount = 2;
    clear_attr.regIndex = sceGxmProgramParameterGetResourceIndex(clear_pos);
    SceGxmVertexStream clear_stream{};
    clear_stream.stride = sizeof(ClearVertex);
    clear_stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(device->patcher, device->clear_vid, &clear_attr, 1,
                                               &clear_stream, 1, &device->clear_vprog) < 0) {
        destroy_device(device);
        return nullptr;
    }
    if (sceGxmShaderPatcherCreateFragmentProgram(device->patcher, device->clear_fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE, nullptr, clear_v,
                                                 &device->clear_fprog) < 0) {
        destroy_device(device);
        return nullptr;
    }

    SceGxmVertexAttribute color_attr[2]{};
    color_attr[0].streamIndex = 0;
    color_attr[0].offset = 0;
    color_attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    color_attr[0].componentCount = 3;
    color_attr[0].regIndex = sceGxmProgramParameterGetResourceIndex(color_pos);
    color_attr[1].streamIndex = 0;
    color_attr[1].offset = 12;
    color_attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    color_attr[1].componentCount = 4;
    color_attr[1].regIndex = sceGxmProgramParameterGetResourceIndex(color_col);
    SceGxmVertexStream color_stream{};
    color_stream.stride = sizeof(ColorVertex);
    color_stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(device->patcher, device->color_vid, color_attr, 2,
                                               &color_stream, 1, &device->color_vprog) < 0) {
        destroy_device(device);
        return nullptr;
    }
    if (sceGxmShaderPatcherCreateFragmentProgram(device->patcher, device->color_fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE, nullptr, color_v,
                                                 &device->color_fprog) < 0) {
        destroy_device(device);
        return nullptr;
    }
    SceGxmBlendInfo modulate = make_blend(SCE_GXM_BLEND_FACTOR_DST_COLOR, SCE_GXM_BLEND_FACTOR_ZERO);
    if (sceGxmShaderPatcherCreateFragmentProgram(device->patcher, device->color_fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE, &modulate, color_v,
                                                 &device->color_modulate_fprog) < 0) {
        return fail_init(device, "color modulate fragment program");
    }
    SceGxmBlendInfo alpha = make_blend(SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    if (sceGxmShaderPatcherCreateFragmentProgram(device->patcher, device->color_fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE, &alpha, color_v,
                                                 &device->color_alpha_fprog) < 0) {
        return fail_init(device, "color alpha fragment program");
    }
    SceGxmBlendInfo subtract =
        make_blend(SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT);
    if (sceGxmShaderPatcherCreateFragmentProgram(device->patcher, device->color_fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE, &subtract, color_v,
                                                 &device->color_subtract_fprog) < 0) {
        return fail_init(device, "color subtract fragment program");
    }

    SceGxmVertexAttribute texture_attr[2]{};
    texture_attr[0].streamIndex = 0;
    texture_attr[0].offset = 0;
    texture_attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    texture_attr[0].componentCount = 3;
    texture_attr[0].regIndex = sceGxmProgramParameterGetResourceIndex(texture_pos);
    texture_attr[1].streamIndex = 0;
    texture_attr[1].offset = 12;
    texture_attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    texture_attr[1].componentCount = 2;
    texture_attr[1].regIndex = sceGxmProgramParameterGetResourceIndex(texture_uv);
    SceGxmVertexStream texture_stream{};
    texture_stream.stride = sizeof(TexturedVertex);
    texture_stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(device->patcher, device->texture_vid, texture_attr, 2,
                                               &texture_stream, 1, &device->texture_vprog) < 0) {
        return fail_init(device, "texture vertex program");
    }
    if (sceGxmShaderPatcherCreateFragmentProgram(device->patcher, device->texture_fid,
                                                 SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                                                 nullptr, texture_v, &device->texture_fprog) < 0) {
        return fail_init(device, "texture fragment program");
    }
    SceGxmBlendInfo tex_modulate = make_blend(SCE_GXM_BLEND_FACTOR_DST_COLOR, SCE_GXM_BLEND_FACTOR_ZERO);
    if (sceGxmShaderPatcherCreateFragmentProgram(device->patcher, device->texture_fid,
                                                 SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                                                 &tex_modulate, texture_v, &device->texture_modulate_fprog) < 0) {
        return fail_init(device, "texture modulate fragment program");
    }

    char vsrc[1024];
    std::size_t vbytes = 0;
    std::uint32_t vgxp_size = 0;
    if (mkwv::gx::emit_tev_vertex_cg(vsrc, sizeof(vsrc), &vbytes) &&
        shacc_compile(vsrc, false, &device->tev_v_gxp, &vgxp_size)) {
        const auto* tev_v = reinterpret_cast<const SceGxmProgram*>(device->tev_v_gxp);
        if (sceGxmProgramCheck(tev_v) != 0) {
            std::free(device->tev_v_gxp);
            device->tev_v_gxp = nullptr;
            vgxp_size = 0;
            tev_v = nullptr;
            if (shacc_compile(vsrc, false, &device->tev_v_gxp, &vgxp_size, true)) {
                tev_v = reinterpret_cast<const SceGxmProgram*>(device->tev_v_gxp);
            }
        }
        if (tev_v && sceGxmProgramCheck(tev_v) == 0 &&
            sceGxmShaderPatcherRegisterProgram(device->patcher, tev_v, &device->tev_vid) == 0) {
            const SceGxmProgramParameter* tev_pos = sceGxmProgramFindParameterByName(tev_v, "aPosition");
            const SceGxmProgramParameter* tev_col = sceGxmProgramFindParameterByName(tev_v, "aColor");
            const SceGxmProgramParameter* tev_uv = sceGxmProgramFindParameterByName(tev_v, "aTexcoord");
            const SceGxmProgramParameter* tev_uv1 = sceGxmProgramFindParameterByName(tev_v, "aTexcoord1");
            device->tev_wvp_param = sceGxmProgramFindParameterByName(tev_v, "wvp");
            if (tev_pos && tev_col && tev_uv && tev_uv1 && device->tev_wvp_param) {
                SceGxmVertexAttribute tev_attr[4]{};
                tev_attr[0].streamIndex = 0;
                tev_attr[0].offset = 0;
                tev_attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
                tev_attr[0].componentCount = 3;
                tev_attr[0].regIndex = sceGxmProgramParameterGetResourceIndex(tev_pos);
                tev_attr[1].streamIndex = 0;
                tev_attr[1].offset = 12;
                tev_attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
                tev_attr[1].componentCount = 4;
                tev_attr[1].regIndex = sceGxmProgramParameterGetResourceIndex(tev_col);
                tev_attr[2].streamIndex = 0;
                tev_attr[2].offset = 16;
                tev_attr[2].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
                tev_attr[2].componentCount = 2;
                tev_attr[2].regIndex = sceGxmProgramParameterGetResourceIndex(tev_uv);
                tev_attr[3].streamIndex = 0;
                tev_attr[3].offset = 24;
                tev_attr[3].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
                tev_attr[3].componentCount = 2;
                tev_attr[3].regIndex = sceGxmProgramParameterGetResourceIndex(tev_uv1);
                SceGxmVertexStream tev_stream{};
                tev_stream.stride = sizeof(TevVertex);
                tev_stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
                if (sceGxmShaderPatcherCreateVertexProgram(device->patcher, device->tev_vid, tev_attr, 4,
                                                           &tev_stream, 1, &device->tev_vprog) == 0) {
                    device->tev_runtime = true;
                    std::printf("mkwv gxm: runtime TEV vertex ready\n");
                }
            }
        }
    }
    if (!device->tev_runtime) {
        std::printf("mkwv gxm: runtime TEV vertex compile failed; keeping two-pass\n");
    }

    device->vertices = allocate(Pool::LpddrUncached, 256 * 1024,
                                SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    device->indices = allocate(Pool::LpddrUncached, 64 * 1024,
                               SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (!device->vertices.host || !device->indices.host) {
        destroy_device(device);
        return nullptr;
    }

    auto* clear_verts = static_cast<ClearVertex*>(device->vertices.host);
    clear_verts[0] = {-1.0f, -1.0f};
    clear_verts[1] = {3.0f, -1.0f};
    clear_verts[2] = {-1.0f, 3.0f};

    auto* color_verts = reinterpret_cast<ColorVertex*>(clear_verts + 3);
    color_verts[0] = {0.0f, 0.7f, 0.5f, 0xFFB85159u};
    color_verts[1] = {-0.7f, -0.65f, 0.5f, 0xFF2EC7F2u};
    color_verts[2] = {0.7f, -0.65f, 0.5f, 0xFF8CD932u};

    auto* idx = static_cast<uint16_t*>(device->indices.host);
    idx[0] = 0;
    idx[1] = 1;
    idx[2] = 2;

    device->texture_mem = allocate(Pool::Cdram, kTextureBytes, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (!device->texture_mem.host) {
        return fail_init(device, "gx texture");
    }
    fill_fallback_texture(static_cast<std::uint32_t*>(device->texture_mem.host), 8);
    if (sceGxmTextureInitLinear(&device->fragment_tex, device->texture_mem.host, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 8, 8,
                                0) < 0) {
        return fail_init(device, "gx texture init");
    }
    sceGxmTextureSetMinFilter(&device->fragment_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&device->fragment_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetUAddrMode(&device->fragment_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
    sceGxmTextureSetVAddrMode(&device->fragment_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
    device->fragment_tex_valid = true;

    device->texture_mem1 = allocate(Pool::Cdram, kTextureBytes, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (!device->texture_mem1.host) {
        return fail_init(device, "gx texture1");
    }
    std::memcpy(device->texture_mem1.host, device->texture_mem.host, 8u * 8u * 4u);
    if (sceGxmTextureInitLinear(&device->fragment_tex1, device->texture_mem1.host, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 8, 8,
                                0) < 0) {
        return fail_init(device, "gx texture1 init");
    }
    sceGxmTextureSetMinFilter(&device->fragment_tex1, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&device->fragment_tex1, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetUAddrMode(&device->fragment_tex1, SCE_GXM_TEXTURE_ADDR_REPEAT);
    sceGxmTextureSetVAddrMode(&device->fragment_tex1, SCE_GXM_TEXTURE_ADDR_REPEAT);
    device->fragment_tex1_valid = true;

    device->ready = true;
    return device;
}

void destroy_device(Device* device) {
    if (!device) {
        return;
    }
    if (device->context) {
        sceGxmFinish(device->context);
    }
    sceGxmDisplayQueueFinish();

    if (device->patcher) {
        if (device->color_fprog) {
            sceGxmShaderPatcherReleaseFragmentProgram(device->patcher, device->color_fprog);
        }
        if (device->color_modulate_fprog) {
            sceGxmShaderPatcherReleaseFragmentProgram(device->patcher, device->color_modulate_fprog);
        }
        if (device->color_alpha_fprog) {
            sceGxmShaderPatcherReleaseFragmentProgram(device->patcher, device->color_alpha_fprog);
        }
        if (device->color_subtract_fprog) {
            sceGxmShaderPatcherReleaseFragmentProgram(device->patcher, device->color_subtract_fprog);
        }
        if (device->color_vprog) {
            sceGxmShaderPatcherReleaseVertexProgram(device->patcher, device->color_vprog);
        }
        for (unsigned i = 0; i < device->tev_slot_count; ++i) {
            if (device->tev_slots[i].fprog) {
                sceGxmShaderPatcherReleaseFragmentProgram(device->patcher, device->tev_slots[i].fprog);
            }
        }
        if (device->tev_vprog) {
            sceGxmShaderPatcherReleaseVertexProgram(device->patcher, device->tev_vprog);
        }
        if (device->texture_fprog) {
            sceGxmShaderPatcherReleaseFragmentProgram(device->patcher, device->texture_fprog);
        }
        if (device->texture_modulate_fprog) {
            sceGxmShaderPatcherReleaseFragmentProgram(device->patcher, device->texture_modulate_fprog);
        }
        if (device->texture_vprog) {
            sceGxmShaderPatcherReleaseVertexProgram(device->patcher, device->texture_vprog);
        }
        if (device->clear_fprog) {
            sceGxmShaderPatcherReleaseFragmentProgram(device->patcher, device->clear_fprog);
        }
        if (device->clear_vprog) {
            sceGxmShaderPatcherReleaseVertexProgram(device->patcher, device->clear_vprog);
        }
        for (unsigned i = 0; i < device->tev_slot_count; ++i) {
            if (device->tev_slots[i].fid) {
                sceGxmShaderPatcherUnregisterProgram(device->patcher, device->tev_slots[i].fid);
            }
        }
        if (device->tev_vid) {
            sceGxmShaderPatcherUnregisterProgram(device->patcher, device->tev_vid);
        }
        if (device->texture_fid) {
            sceGxmShaderPatcherUnregisterProgram(device->patcher, device->texture_fid);
        }
        if (device->texture_vid) {
            sceGxmShaderPatcherUnregisterProgram(device->patcher, device->texture_vid);
        }
        if (device->color_fid) {
            sceGxmShaderPatcherUnregisterProgram(device->patcher, device->color_fid);
        }
        if (device->color_vid) {
            sceGxmShaderPatcherUnregisterProgram(device->patcher, device->color_vid);
        }
        if (device->clear_fid) {
            sceGxmShaderPatcherUnregisterProgram(device->patcher, device->clear_fid);
        }
        if (device->clear_vid) {
            sceGxmShaderPatcherUnregisterProgram(device->patcher, device->clear_vid);
        }
        sceGxmShaderPatcherDestroy(device->patcher);
    }
    if (device->context) {
        sceGxmDestroyContext(device->context);
    }
    if (device->render_target) {
        sceGxmDestroyRenderTarget(device->render_target);
    }
    for (unsigned i = 0; i < kBufferCount; ++i) {
        if (device->sync[i]) {
            sceGxmSyncObjectDestroy(device->sync[i]);
        }
        deallocate(device->display[i]);
    }
    deallocate(device->depth);
    deallocate(device->texture_mem);
    deallocate(device->texture_mem1);
    deallocate(device->vertices);
    deallocate(device->indices);
    deallocate(device->vdm);
    deallocate(device->vertex_ring);
    deallocate(device->fragment_ring);
    if (device->fragment_usse.host) {
        sceGxmUnmapFragmentUsseMemory(device->fragment_usse.host);
        deallocate(device->fragment_usse);
    }
    deallocate(device->patcher_buffer);
    if (device->patcher_vertex_usse.host) {
        sceGxmUnmapVertexUsseMemory(device->patcher_vertex_usse.host);
        deallocate(device->patcher_vertex_usse);
    }
    if (device->patcher_fragment_usse.host) {
        sceGxmUnmapFragmentUsseMemory(device->patcher_fragment_usse.host);
        deallocate(device->patcher_fragment_usse);
    }
    std::free(device->tev_v_gxp);
    for (unsigned i = 0; i < device->tev_slot_count; ++i) {
        std::free(device->tev_slots[i].gxp);
    }
    shacc_shutdown();
    std::free(device->host_context_mem);
    sceGxmTerminate();
    delete device;
}

void begin_frame(Device* device, float r, float g, float b, float a) {
    if (!device || !device->ready) {
        return;
    }
    device->err_begin_scene = sceGxmBeginScene(device->context, 0, device->render_target, nullptr, nullptr,
                                               device->sync[device->back], &device->color_surface[device->back],
                                               &device->depth_surface);

    sceGxmSetCullMode(device->context, SCE_GXM_CULL_NONE);
    apply_depth(device->context, false, true, 7);

    sceGxmSetVertexProgram(device->context, device->clear_vprog);
    sceGxmSetFragmentProgram(device->context, device->clear_fprog);
    void* uniforms = nullptr;
    sceGxmReserveFragmentDefaultUniformBuffer(device->context, &uniforms);
    const float color[4] = {r, g, b, a};
    sceGxmSetUniformDataF(uniforms, device->clear_color_param, 0, 4, color);
    sceGxmSetVertexStream(device->context, 0, device->vertices.host);
    device->err_clear_draw =
        sceGxmDraw(device->context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, device->indices.host, 3);
    device->vert_used = 3u * sizeof(ClearVertex);
    device->idx_used = 3u * sizeof(std::uint16_t);
}

void draw_color_triangle(Device* device) {
    if (!device || !device->ready) {
        return;
    }
    auto* color_verts = reinterpret_cast<ColorVertex*>(static_cast<ClearVertex*>(device->vertices.host) + 3);
    const float t = static_cast<float>(device->frame) * 0.015f;
    const float pulse = 0.5f + 0.5f * std::sin(t);
    const auto r = static_cast<std::uint32_t>((0.35f + 0.65f * pulse) * 255.0f);
    const auto b = static_cast<std::uint32_t>((0.85f - 0.4f * pulse) * 255.0f);
    color_verts[0].color = 0xFF000000u | (b << 16) | (0x51u << 8) | r;
    ColorVertex verts[3] = {color_verts[0], color_verts[1], color_verts[2]};
    const std::uint16_t idx[3] = {0, 1, 2};
    set_cull_mode(device, 0);
    set_depth(device, false, false, 7);
    draw_color_mesh(device, verts, 3, idx, 3);
}

void set_cull_mode(Device* device, unsigned gx_cull) {
    if (!device || !device->ready) {
        return;
    }
    sceGxmSetCullMode(device->context, map_cull(gx_cull));
}

void set_depth(Device* device, bool test, bool write, unsigned gx_compare) {
    if (!device || !device->ready) {
        return;
    }
    apply_depth(device->context, test, write, gx_compare);
}

bool draw_color_mesh(Device* device, const ColorVertex* verts, std::uint32_t vert_count, const std::uint16_t* indices,
                     std::uint32_t index_count, unsigned blend) {
    if (!device || !device->ready || !verts || !indices || vert_count == 0 || index_count == 0) {
        return false;
    }
    const std::size_t vert_bytes = static_cast<std::size_t>(vert_count) * sizeof(ColorVertex);
    const std::size_t idx_bytes = static_cast<std::size_t>(index_count) * sizeof(std::uint16_t);
    void* vert_mem = nullptr;
    void* idx_mem = nullptr;
    if (!bump_mesh(device, vert_bytes, 16, idx_bytes, &vert_mem, &idx_mem)) {
        return false;
    }
    std::memcpy(vert_mem, verts, vert_bytes);
    std::memcpy(idx_mem, indices, idx_bytes);

    sceGxmSetVertexProgram(device->context, device->color_vprog);
    SceGxmFragmentProgram* fprog = device->color_fprog;
    if (blend == kBlendModulate) {
        fprog = device->color_modulate_fprog;
    } else if (blend == kBlendAlpha) {
        fprog = device->color_alpha_fprog;
    } else if (blend == kBlendSubtract) {
        fprog = device->color_subtract_fprog;
    }
    sceGxmSetFragmentProgram(device->context, fprog);
    void* uniforms = nullptr;
    sceGxmReserveVertexDefaultUniformBuffer(device->context, &uniforms);
    const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
    sceGxmSetUniformDataF(uniforms, device->wvp_param, 0, 16, identity);
    sceGxmSetVertexStream(device->context, 0, vert_mem);
    sceGxmDraw(device->context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, idx_mem, index_count);
    return true;
}

bool draw_textured_mesh(Device* device, const TexturedVertex* verts, std::uint32_t vert_count,
                        const std::uint16_t* indices, std::uint32_t index_count, unsigned blend) {
    if (!device || !device->ready || !verts || !indices || vert_count == 0 || index_count == 0) {
        return false;
    }
    const std::size_t vert_bytes = static_cast<std::size_t>(vert_count) * sizeof(TexturedVertex);
    const std::size_t idx_bytes = static_cast<std::size_t>(index_count) * sizeof(std::uint16_t);
    void* vert_mem = nullptr;
    void* idx_mem = nullptr;
    if (!bump_mesh(device, vert_bytes, 4, idx_bytes, &vert_mem, &idx_mem)) {
        return false;
    }
    std::memcpy(vert_mem, verts, vert_bytes);
    std::memcpy(idx_mem, indices, idx_bytes);

    sceGxmSetVertexProgram(device->context, device->texture_vprog);
    device->err_mesh_prog = 0;
    sceGxmSetFragmentProgram(device->context,
                             blend == kBlendModulate ? device->texture_modulate_fprog : device->texture_fprog);
    void* uniforms = nullptr;
    device->err_vunif = sceGxmReserveVertexDefaultUniformBuffer(device->context, &uniforms);
    device->err_vunif_null = (uniforms == nullptr) ? 1 : 0;
    const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
    if (uniforms != nullptr) {
        sceGxmSetUniformDataF(uniforms, device->texture_wvp_param, 0, 16, identity);
    }
    device->err_mesh_tex = sceGxmSetFragmentTexture(device->context, 0, &device->fragment_tex);
    device->err_mesh_stream = sceGxmSetVertexStream(device->context, 0, vert_mem);
    device->err_mesh_draw =
        sceGxmDraw(device->context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, idx_mem, index_count);
    return device->err_mesh_draw >= 0;
}

bool has_runtime_tev(const Device* device) {
    return device && device->ready && device->tev_runtime && device->tev_vprog;
}

Device::TevSlot* find_tev_slot(Device* device, std::uint64_t key) {
    if (!device) {
        return nullptr;
    }
    for (unsigned i = 0; i < device->tev_slot_count; ++i) {
        if (device->tev_slots[i].key == key) {
            return &device->tev_slots[i];
        }
    }
    return nullptr;
}

const Device::TevSlot* find_tev_slot(const Device* device, std::uint64_t key) {
    return find_tev_slot(const_cast<Device*>(device), key);
}

bool has_tev_fragment(const Device* device, std::uint64_t tev_key) {
    return find_tev_slot(device, tev_key) != nullptr;
}

bool ensure_tev_fragment(Device* device, std::uint64_t tev_key, const char* fragment_cg) {
    if (!has_runtime_tev(device) || fragment_cg == nullptr) {
        return false;
    }
    if (find_tev_slot(device, tev_key)) {
        return true;
    }
    if (device->tev_slot_count >= Device::kTevCache) {
        std::printf("mkwv gxm: TEV cache full, skipping %llx\n", static_cast<unsigned long long>(tev_key));
        return false;
    }

    void* gxp = nullptr;
    std::uint32_t bytes = 0;
    auto compile = [&](bool force) { return shacc_compile(fragment_cg, true, &gxp, &bytes, force); };
    if (!compile(false)) {
        return false;
    }
    const auto* tev_f = reinterpret_cast<const SceGxmProgram*>(gxp);
    if (sceGxmProgramCheck(tev_f) != 0) {
        std::free(gxp);
        gxp = nullptr;
        bytes = 0;
        if (!compile(true)) {
            return false;
        }
        tev_f = reinterpret_cast<const SceGxmProgram*>(gxp);
        if (sceGxmProgramCheck(tev_f) != 0) {
            std::free(gxp);
            return false;
        }
    }

    Device::TevSlot& slot = device->tev_slots[device->tev_slot_count];
    const auto* tev_v = reinterpret_cast<const SceGxmProgram*>(device->tev_v_gxp);
    if (sceGxmShaderPatcherRegisterProgram(device->patcher, tev_f, &slot.fid) != 0 ||
        sceGxmShaderPatcherCreateFragmentProgram(device->patcher, slot.fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE, nullptr, tev_v, &slot.fprog) != 0) {
        if (slot.fid) {
            sceGxmShaderPatcherUnregisterProgram(device->patcher, slot.fid);
            slot.fid = {};
        }
        std::free(gxp);
        std::printf("mkwv gxm: TEV fragment register failed %llx\n", static_cast<unsigned long long>(tev_key));
        return false;
    }
    slot.key = tev_key;
    slot.gxp = gxp;
    ++device->tev_slot_count;
    std::printf("mkwv gxm: TEV fragment %llx ready\n", static_cast<unsigned long long>(tev_key));
    return true;
}

bool draw_tev_mesh(Device* device, const TevVertex* verts, std::uint32_t vert_count, const std::uint16_t* indices,
                   std::uint32_t index_count, std::uint64_t tev_key) {
    Device::TevSlot* slot = find_tev_slot(device, tev_key);
    if (!has_runtime_tev(device) || !slot || !slot->fprog || !verts || !indices || vert_count == 0 ||
        index_count == 0) {
        return false;
    }
    const std::size_t vert_bytes = static_cast<std::size_t>(vert_count) * sizeof(TevVertex);
    const std::size_t idx_bytes = static_cast<std::size_t>(index_count) * sizeof(std::uint16_t);
    void* vert_mem = nullptr;
    void* idx_mem = nullptr;
    if (!bump_mesh(device, vert_bytes, 16, idx_bytes, &vert_mem, &idx_mem)) {
        return false;
    }
    std::memcpy(vert_mem, verts, vert_bytes);
    std::memcpy(idx_mem, indices, idx_bytes);

    sceGxmSetVertexProgram(device->context, device->tev_vprog);
    sceGxmSetFragmentProgram(device->context, slot->fprog);
    void* uniforms = nullptr;
    sceGxmReserveVertexDefaultUniformBuffer(device->context, &uniforms);
    const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
    sceGxmSetUniformDataF(uniforms, device->tev_wvp_param, 0, 16, identity);
    sceGxmSetFragmentTexture(device->context, 0, &device->fragment_tex);
    if (device->fragment_tex1_valid) {
        sceGxmSetFragmentTexture(device->context, 1, &device->fragment_tex1);
    }
    sceGxmSetVertexStream(device->context, 0, vert_mem);
    sceGxmDraw(device->context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, idx_mem, index_count);
    return true;
}

bool bind_fragment_texture(Device* device, const void* pixels, std::uint32_t bytes, unsigned width, unsigned height,
                           unsigned layout, unsigned wrap_s, unsigned wrap_t, unsigned slot, unsigned min_filter,
                           unsigned mag_filter, int lod_bias, unsigned mip_count) {
    if (!device || !device->ready || !pixels || bytes == 0 || width == 0 || height == 0) {
        return false;
    }
    Buffer& mem = (slot != 0) ? device->texture_mem1 : device->texture_mem;
    SceGxmTexture& tex = (slot != 0) ? device->fragment_tex1 : device->fragment_tex;
    bool& valid = (slot != 0) ? device->fragment_tex1_valid : device->fragment_tex_valid;
    if (!mem.host || bytes > mem.size) {
        static bool logged = false;
        if (!logged) {
            std::printf("mkwv: bind_fragment_texture too big %ux%u bytes=%u cap=%u slot=%u\n", width, height, bytes,
                        static_cast<unsigned>(mem.size), slot);
            std::fflush(stdout);
            logged = true;
        }
        return false;
    }
    std::memcpy(mem.host, pixels, bytes);
    const SceGxmTextureFormat fmt = (layout == 1) ? SCE_GXM_TEXTURE_FORMAT_UBC1 : SCE_GXM_TEXTURE_FORMAT_A8B8G8R8;
    const unsigned mips = mip_count > 1 ? mip_count : 0;
    if (sceGxmTextureInitLinear(&tex, mem.host, fmt, width, height, mips) < 0) {
        valid = false;
        return false;
    }
    const bool min_point = (min_filter == 0 || min_filter == 2 || min_filter == 4);
    const bool mag_point = (mag_filter == 0);
    const bool mip_lin = (min_filter == 4 || min_filter == 5);
    sceGxmTextureSetMinFilter(&tex, min_point ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&tex, mag_point ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMipFilter(&tex, (mips != 0 && mip_lin) ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
                                                            : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    sceGxmTextureSetUAddrMode(&tex, map_wrap(wrap_s));
    sceGxmTextureSetVAddrMode(&tex, map_wrap(wrap_t));
    (void)lod_bias;
    valid = true;
    if (slot == 0) {
        device->bound_src = pixels;
        device->bound_bytes = bytes;
        device->bound_w = width;
        device->bound_h = height;
        device->bound_layout = layout;
        device->bound_wrap_s = wrap_s;
        device->bound_wrap_t = wrap_t;
    }
    return true;
}

void set_viewport(Device* device, float left, float top, float width, float height, float near_z, float far_z) {
    if (!device || !device->ready || width <= 0.0f || height <= 0.0f) {
        return;
    }
    const float x_off = left + width * 0.5f;
    const float y_off = top + height * 0.5f;
    const float x_scale = width * 0.5f;
    // GX NDC has +Y at the top; GXM framebuffer coordinates grow downward.
    const float y_scale = -height * 0.5f;
    const float z_scale = (far_z - near_z) * 0.5f;
    const float z_off = (far_z + near_z) * 0.5f;
    // GXM takes offset/scale pairs, not an offset vector followed by a scale vector.
    // Mixing these up put the strap viewport's depth offset at 228 (height / 2).
    sceGxmSetViewport(device->context, x_off, x_scale, y_off, y_scale, z_off, z_scale);
}

void set_scissor(Device* device, int x, int y, int width, int height) {
    if (!device || !device->ready || width <= 0 || height <= 0) {
        return;
    }
    int x1 = x + width - 1;
    int y1 = y + height - 1;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x1 >= static_cast<int>(kWidth)) {
        x1 = static_cast<int>(kWidth) - 1;
    }
    if (y1 >= static_cast<int>(kHeight)) {
        y1 = static_cast<int>(kHeight) - 1;
    }
    sceGxmSetRegionClip(device->context, SCE_GXM_REGION_CLIP_OUTSIDE, static_cast<unsigned>(x), static_cast<unsigned>(y),
                        static_cast<unsigned>(x1), static_cast<unsigned>(y1));
}

void end_frame(Device* device) {
    if (!device || !device->ready) {
        return;
    }
    sceGxmEndScene(device->context, nullptr, nullptr);
    sceGxmPadHeartbeat(&device->color_surface[device->back], device->sync[device->back]);
    DisplayJob job{device->display[device->back].host};
    sceGxmDisplayQueueAddEntry(device->sync[device->front], device->sync[device->back], &job);
    device->front = device->back;
    device->back = (device->back + 1u) % kBufferCount;
    ++device->frame;
}

unsigned display_width(const Device*) {
    return kWidth;
}

unsigned display_height(const Device*) {
    return kHeight;
}

unsigned frame_index(const Device* device) {
    return device ? device->frame : 0;
}

// Probe: draw a quad with the CLEAR program (the one program known to reach the screen) from
// bump_mesh memory, bright green, no vertex uniforms. Green on screen means bump memory and
// post-clear draws are fine and the fault is specific to the vertex-uniform programs; no green means
// the problem is that anything drawn after the initial clear draw is lost.
int probe_clear_quad(Device* device) {
    if (!device || !device->ready) {
        return -1;
    }
    const ClearVertex verts[4] = {{-0.6f, 0.6f}, {0.6f, 0.6f}, {0.6f, -0.6f}, {-0.6f, -0.6f}};
    const std::uint16_t idx[6] = {0, 1, 2, 2, 3, 0};
    void* vm = nullptr;
    void* im = nullptr;
    if (!bump_mesh(device, sizeof(verts), 4, sizeof(idx), &vm, &im)) {
        return -2;
    }
    std::memcpy(vm, verts, sizeof(verts));
    std::memcpy(im, idx, sizeof(idx));
    sceGxmSetCullMode(device->context, SCE_GXM_CULL_NONE);
    apply_depth(device->context, false, false, 7);
    sceGxmSetVertexProgram(device->context, device->clear_vprog);
    sceGxmSetFragmentProgram(device->context, device->clear_fprog);
    void* u = nullptr;
    device->err_probe_reserve = sceGxmReserveFragmentDefaultUniformBuffer(device->context, &u);
    device->err_probe_null = (u == nullptr) ? 1 : 0;
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    if (u != nullptr) {
        sceGxmSetUniformDataF(u, device->clear_color_param, 0, 4, green);
    }
    sceGxmSetVertexStream(device->context, 0, vm);
    device->err_probe_clear =
        sceGxmDraw(device->context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, im, 6);
    return device->err_probe_clear;
}

void gxm_errors(const Device* device, int* begin_scene, int* clear_draw, int* mesh_prog, int* mesh_stream,
                int* mesh_tex, int* mesh_draw) {
    if (!device) {
        return;
    }
    *begin_scene = device->err_begin_scene;
    *clear_draw = device->err_clear_draw;
    *mesh_prog = device->err_mesh_prog;
    *mesh_stream = device->err_mesh_stream;
    *mesh_tex = device->err_mesh_tex;
    *mesh_draw = device->err_mesh_draw;
}

void gxm_uniform_errors(const Device* device, int* vunif, int* vunif_null, int* probe_clear, int* probe_reserve,
                        int* probe_null) {
    if (!device) {
        return;
    }
    *vunif = device->err_vunif;
    *vunif_null = device->err_vunif_null;
    *probe_clear = device->err_probe_clear;
    *probe_reserve = device->err_probe_reserve;
    *probe_null = device->err_probe_null;
}

} // namespace mkwv::gxm
