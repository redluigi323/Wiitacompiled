#pragma once

#include "mkwv/gx/state.hpp"

namespace mkwv::gx {

TevOp classify_tev(const State& state);
std::uint64_t hash_tev(const State& state);

// Structure-only key for a compiled shader program: excludes the colour values that
// hash_tev mixes in, since those are uniforms.
std::uint64_t hash_tev_program(const State& state);
std::uint32_t eval_tev_color(const State& state, std::uint32_t ras, std::uint32_t tex, std::uint32_t prev = 0);
std::uint32_t eval_tev_color(const State& state, std::uint32_t ras0, std::uint32_t ras1, const std::uint32_t* texmaps,
                             unsigned ntex, std::uint32_t prev = 0);
float eval_fog_factor(const State& state, float z);
std::uint32_t mix_fog(const State& state, std::uint32_t color, float z);
void eval_indirect_st(const State& state, unsigned stage, std::uint32_t ind_tex, float base_s, float base_t, float* s,
                      float* t);
void tev_konst_rgb(std::uint8_t sel, const std::uint32_t konst[4], std::uint8_t rgb[3]);
std::uint8_t tev_konst_a(std::uint8_t sel, const std::uint32_t konst[4]);

} // namespace mkwv::gx
