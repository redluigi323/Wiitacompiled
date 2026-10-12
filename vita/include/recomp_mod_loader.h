#pragma once

#include <cstdint>

struct CpuContext;

namespace RecompMod {

inline constexpr unsigned kExecutableWriteGuardPageShift = 12;

#if defined(__vita__)
#define MKW_TLS
#else
#define MKW_TLS thread_local
#endif

inline MKW_TLS uint32_t g_currentTranslatedExecutionAddress = 0;

class ScopedTranslatedExecutionAddress {
public:
    explicit ScopedTranslatedExecutionAddress(uint32_t address) noexcept : previous_(g_currentTranslatedExecutionAddress) {
        if (address != 0) {
            g_currentTranslatedExecutionAddress = address;
        }
    }
    ~ScopedTranslatedExecutionAddress() noexcept { g_currentTranslatedExecutionAddress = previous_; }

private:
    uint32_t previous_ = 0;
};

} // namespace RecompMod
