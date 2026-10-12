#pragma once

#include <cstdint>

namespace GuestFlat {

inline constexpr bool RequiresCheckedAccess() noexcept { return true; }

} // namespace GuestFlat

#define MKW_FLAT_GUEST_BASE (static_cast<std::uint8_t*>(nullptr))
