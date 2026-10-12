#pragma once

#include "isa/ppc_isa_context.h"

#include <cstdint>

namespace mkwv::runtime {

using ::CpuContext;
using PpcFpr = ::PPC_FPR;
using TranslatedFn = void (*)(CpuContext*);

// Bring-up native stubs. Not guest DOL addresses — 0x80004000 is real _init.
inline constexpr std::uint32_t kStubGxTriangle = 0x7FFF0000u;
inline constexpr std::uint32_t kGuestProbe = 0x7FFF0004u;
inline constexpr std::uint32_t kGuestStart = 0x800060A4u;
inline constexpr std::uint32_t kGuestStack = 0x817E0000u;

} // namespace mkwv::runtime
