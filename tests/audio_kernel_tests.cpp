#include "../vita/src/runtime/audio/ax_mix_kernels.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <random>

int main() {
    std::mt19937 random(66);
    std::array<std::uint8_t, 600> src{}, out{}, back{};
    for (auto& byte : src) byte = static_cast<std::uint8_t>(random());
    for (unsigned offset = 0; offset < 4; ++offset) {
        for (unsigned bytes = 0; bytes <= 544; bytes += 2) {
            out.fill(0xCD); back.fill(0xCD);
            AxMixKernels::CopySwap16(out.data() + offset, src.data() + offset, bytes);
            for (unsigned i = 0; i < bytes; ++i) {
                if (out[offset + i] != src[offset + (i ^ 1u)]) return 1;
            }
            if (out[offset + bytes] != 0xCD) return 1;
            AxMixKernels::CopySwap16(back.data() + offset, out.data() + offset, bytes);
            if (std::memcmp(back.data() + offset, src.data() + offset, bytes)) return 1;
        }
    }
    std::int16_t samples[4], coeffs[4];
    for (unsigned trial = 0; trial < 20000; ++trial) {
        for (unsigned i = 0; i < 4; ++i) {
            samples[i] = trial == 0 ? -32768 : static_cast<std::int16_t>(random());
            coeffs[i] = trial == 0 ? -32768 : static_cast<std::int16_t>(random());
        }
        if (AxMixKernels::Dot4S16(samples, coeffs) != AxMixKernels::Dot4S16Scalar(samples, coeffs)) return 1;
    }
    std::puts("audio kernels: byte swapping and wide FIR dot products match");
}
