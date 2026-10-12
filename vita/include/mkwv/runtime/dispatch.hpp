#pragma once

#include "mkwv/runtime/cpu_context.hpp"

#include <cstdint>

namespace mkwv::runtime {

class OverlayCache;

void bind_dispatch(OverlayCache* overlay);
void unbind_dispatch();

bool invoke_guest(std::uint32_t address, CpuContext* ctx);
std::uint32_t missing_target_count();
std::uint32_t dispatch_count();
std::uint32_t recent_dispatch(unsigned back);
std::uint32_t last_calc_scene();
std::uint32_t last_missing_target();
void note_missing_target(std::uint32_t address);
void bind_static_dispatch(const void* table);
// invoke_guest caches resolved targets; clear after anything that changes what an address
// resolves to.
void clear_dispatch_cache();
std::uint32_t dispatch_cache_misses();

} // namespace mkwv::runtime
