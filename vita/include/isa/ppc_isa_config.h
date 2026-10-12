#pragma once

#include <cstdint>

#define MKW_RESTRICT __restrict
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

inline constexpr bool MkwStateFreeAbiEnabled(uint32_t) noexcept
{
    return true;
}

#if defined(_WIN32)
#define MKW_PPC_FORCE_INLINE __forceinline
#define MKW_PPC_NO_INLINE __declspec(noinline)
#define MKW_PPC_INTERNAL_CALL __regcall
#else
// Vita: no forced inlining anywhere in the translated code. Upstream marks the ISA helpers and
// some translated bodies always_inline; forced inlining in the shards cost compile time and code
// size without measurable FPS here, so these are plain `inline` and GCC's -O2 heuristics decide.
#define MKW_PPC_FORCE_INLINE inline
#define MKW_PPC_NO_INLINE __attribute__((noinline))
#define MKW_PPC_INTERNAL_CALL
#endif
#if defined(_WIN32)
#define MKW_PPC_ALWAYS_INLINE_BODY __attribute__((always_inline))
#else
#define MKW_PPC_ALWAYS_INLINE_BODY
#endif
#define MKW_PPC_COLD __attribute__((cold))

using MkwStateFreeResult2 = struct MkwStateFreeResult2 {
    std::uint64_t lane0 = 0;
    std::uint64_t lane1 = 0;
    std::uint64_t operator[](unsigned index) const noexcept { return index == 0 ? lane0 : lane1; }
};
