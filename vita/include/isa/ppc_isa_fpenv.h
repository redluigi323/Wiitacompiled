#pragma once

#include "ppc_isa_config.h"

#include <cstdint>

#if defined(__x86_64__)
inline constexpr uint32_t kMkwFpControlFlushToZeroBits = (1u << 15) | (1u << 6);
#elif defined(__aarch64__)
inline constexpr uint32_t kMkwFpControlFlushToZeroBits = (1u << 24);
#elif defined(__arm__)
inline constexpr uint32_t kMkwFpControlFlushToZeroBits = (1u << 24);
#else
inline constexpr uint32_t kMkwFpControlFlushToZeroBits = 0;
#endif

#if defined(__vita__)
#define MKW_TLS
#else
#define MKW_TLS thread_local
#endif

inline MKW_TLS bool g_mkwHostNiActive = false;
inline constexpr double kMkwNiFlushThreshold = 0x1p-126;
inline MKW_TLS double g_mkwNiFlushThreshold = 0.0;

inline uint32_t MkwGetHostFpControl() noexcept
{
#if defined(__x86_64__)
    return _mm_getcsr();
#elif defined(__aarch64__)
    uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    return static_cast<uint32_t>(fpcr);
#elif defined(__arm__)
    uint32_t fpscr = 0;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    return fpscr;
#else
    return 0;
#endif
}

inline void MkwSetHostFpControl(uint32_t value) noexcept
{
#if defined(__x86_64__)
    _mm_setcsr(value);
#elif defined(__aarch64__)
    uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = (fpcr & ~static_cast<uint64_t>(0xFFFFFFFFu)) | value;
    __asm__ __volatile__("msr fpcr, %0" ::"r"(fpcr));
#elif defined(__arm__)
    __asm__ __volatile__("vmsr fpscr, %0" ::"r"(value));
#else
    (void)value;
#endif
}

inline void MkwApplyHostNiMode(uint32_t fpscr) noexcept
{
    const uint32_t csr = MkwGetHostFpControl();
    const bool wantNi = (fpscr & 0x4u) != 0;
    const uint32_t want = wantNi ? (csr | kMkwFpControlFlushToZeroBits) : (csr & ~kMkwFpControlFlushToZeroBits);
    if (want != csr) {
        MkwSetHostFpControl(want);
    }
    g_mkwHostNiActive = wantNi;
    g_mkwNiFlushThreshold = wantNi ? kMkwNiFlushThreshold : 0.0;
}

inline void MkwRestoreHostMxcsr(uint32_t csr) noexcept
{
    MkwSetHostFpControl(csr);
    const bool niActive = (csr & kMkwFpControlFlushToZeroBits) != 0;
    g_mkwHostNiActive = niActive;
    g_mkwNiFlushThreshold = niActive ? kMkwNiFlushThreshold : 0.0;
}
