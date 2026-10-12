#pragma once

#include <cstdint>

namespace mkwv::gxm {

struct Device;

struct ColorVertex {
    float x, y, z;
    std::uint32_t color;
};

struct TexturedVertex {
    float x, y, z;
    float u, v;
};

struct TevVertex {
    float x, y, z;
    std::uint32_t color;
    float u, v;
    float u1, v1;
};

Device* create_device();
void destroy_device(Device* device);

void begin_frame(Device* device, float r, float g, float b, float a);
void draw_color_triangle(Device* device);
void set_cull_mode(Device* device, unsigned gx_cull);
void set_depth(Device* device, bool test, bool write, unsigned gx_compare);
inline constexpr unsigned kBlendOpaque = 0;
inline constexpr unsigned kBlendModulate = 1;
inline constexpr unsigned kBlendAlpha = 2;
inline constexpr unsigned kBlendSubtract = 3;

bool draw_color_mesh(Device* device, const ColorVertex* verts, std::uint32_t vert_count, const std::uint16_t* indices,
                     std::uint32_t index_count, unsigned blend = kBlendOpaque);
bool draw_textured_mesh(Device* device, const TexturedVertex* verts, std::uint32_t vert_count,
                        const std::uint16_t* indices, std::uint32_t index_count, unsigned blend = kBlendOpaque);
bool draw_tev_mesh(Device* device, const TevVertex* verts, std::uint32_t vert_count, const std::uint16_t* indices,
                   std::uint32_t index_count, std::uint64_t tev_key);
bool has_runtime_tev(const Device* device);

// Return codes of GXM calls whose results are otherwise discarded.
int probe_clear_quad(Device* device);
void gxm_uniform_errors(const Device* device, int* vunif, int* vunif_null, int* probe_clear, int* probe_reserve,
                        int* probe_null);
void gxm_errors(const Device* device, int* begin_scene, int* clear_draw, int* mesh_prog, int* mesh_stream,
                int* mesh_tex, int* mesh_draw);
bool has_tev_fragment(const Device* device, std::uint64_t tev_key);
bool ensure_tev_fragment(Device* device, std::uint64_t tev_key, const char* fragment_cg);
bool bind_fragment_texture(Device* device, const void* pixels, std::uint32_t bytes, unsigned width, unsigned height,
                           unsigned layout, unsigned wrap_s, unsigned wrap_t, unsigned slot = 0,
                           unsigned min_filter = 1, unsigned mag_filter = 1, int lod_bias = 0, unsigned mip_count = 0);
void set_viewport(Device* device, float left, float top, float width, float height, float near_z, float far_z);
void set_scissor(Device* device, int x, int y, int width, int height);
void end_frame(Device* device);

unsigned display_width(const Device* device);
unsigned display_height(const Device* device);
unsigned frame_index(const Device* device);

} // namespace mkwv::gxm
