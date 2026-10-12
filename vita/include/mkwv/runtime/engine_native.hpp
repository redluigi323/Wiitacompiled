#pragma once

#include "mkwv/runtime/cpu_context.hpp"
#include <cstdint>

namespace mkwv::runtime {
void set_native_engine(bool enabled);
bool native_engine_enabled();
// False leaves context/memory unchanged: the wrapper then runs the original PPC routine.
bool try_native_engine(std::uint32_t address, CpuContext* cpu);
struct NativeEngineStats { std::uint32_t calls, fallback; };
NativeEngineStats take_native_engine_stats();
}
