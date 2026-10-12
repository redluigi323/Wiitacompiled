#pragma once

#include <cstddef>
#include <cstdint>

namespace mkwv::runtime {

class CompactMemory;

inline constexpr std::uint32_t kSdaBase = 0x8038CC00u;
inline constexpr std::uint32_t kSda2Base = 0x8038EFA0u;

struct GuestImageStats {
    unsigned sections = 0;
    std::uint32_t bytes = 0;
    std::uint32_t init_word = 0;
    bool loaded = false;
};

GuestImageStats load_guest_image(CompactMemory& memory, const char* root);

} // namespace mkwv::runtime
