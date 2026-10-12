// The AX/DSP HLE (vita/src/runtime/audio, upstream's port of Dolphin's AX HLE): a command
// list mailed to the "DSP" the way the AX library does it must mix a voice parameter block
// into the AI output buffer. One 16-bit PCM voice at a constant level, linear resampling at a
// 1:1 ratio, mixed to main L/R, then OUTPUT.
#include "mkwv/runtime/hle_audio.hpp"
#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/runtime.hpp"
#include "memory.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#include "mkwv/runtime/hle_native.hpp"

namespace {

using mkwv::runtime::CpuContext;
using mkwv::runtime::invoke_guest;

int fail(const char* message) {
    std::fprintf(stderr, "audio_hle: %s\n", message);
    return 1;
}

constexpr std::uint32_t kStack = 0x80600F00u;
constexpr std::uint32_t kPb = 0x80620000u;
constexpr std::uint32_t kSamplesPhys = 0x00630000u;
constexpr std::uint32_t kCmdList = 0x80640000u;
constexpr std::uint32_t kSetup = 0x80641000u;
constexpr std::uint32_t kOutLR = 0x80650000u;
constexpr std::uint32_t kOutS = 0x80651000u;
constexpr std::int16_t kLevel = 0x1000;

// The AXPBWii halfword layout the HLE reads (ax_internal.h), without the `updates` block
// (the SkipUpdatesOnly guest layout): offsets in halfwords.
struct PbWriter {
    std::vector<std::uint16_t> w = std::vector<std::uint16_t>(0xC0, 0);
    void set(unsigned hw, std::uint16_t v) { w[hw] = v; }
};

bool call(CpuContext& cpu, std::uint32_t addr, std::initializer_list<std::uint32_t> args) {
    unsigned r = 3;
    for (std::uint32_t a : args) {
        cpu.gpr[r++] = a;
    }
    return invoke_guest(addr, &cpu);
}

int g_callbacks = 0;
std::uint32_t g_sink_frames = 0;
std::int16_t g_first_left = 0;
void ai_callback(CpuContext*) { ++g_callbacks; }
void sink(const std::int16_t* lr, std::uint32_t frames) {
    if (g_sink_frames == 0) g_first_left = lr[0];
    g_sink_frames += frames;
}

} // namespace

int main() {
    mkwv::runtime::Runtime runtime;
    if (!runtime.initialize("/tmp/mkwv-audio-hle-overlay")) {
        return fail("runtime");
    }
    CpuContext& cpu = runtime.cpu();
    cpu.gpr[1] = kStack;

    // A synthetic coefficient ROM selects a different history tap in each phase.
    // Linear voices ignore it; the polyphase case below exercises the actual mixer.
    constexpr const char* coef_path = "/tmp/mkwv-audio-hle-coeff.bin";
    std::FILE* coef_file = std::fopen(coef_path, "wb");
    if (coef_file == nullptr) return fail("coefficient fixture");
    for (unsigned i = 0; i < 0x800; ++i) {
        const unsigned phase = (i & 0x1ffu) / 4;
        const std::uint16_t value = (i & 3u) == (phase & 3u) ? 0x7fff : 0;
        const unsigned char bytes[] = {static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
        if (std::fwrite(bytes, 1, sizeof(bytes), coef_file) != sizeof(bytes)) return fail("coefficient write");
    }
    std::fclose(coef_file);
    mkwv::runtime::set_audio_coef_rom(coef_path);

    // DSPInit: the HLE boots and posts its init mail.
    if (!call(cpu, 0x8015D444u, {})) return fail("DSPInit");
    std::remove(coef_path);  // DSPInit has loaded the ROM.
    if (!call(cpu, 0x8015D40Cu, {}) || cpu.gpr[3] != 1u) return fail("no mail from the DSP after init");
    if (!call(cpu, 0x8015D41Cu, {}) || cpu.gpr[3] != 0xDCD10000u) return fail("init mail");

    // Sample data: 16-bit PCM at a constant level.
    for (std::uint32_t i = 0; i < 4096; ++i) {
        Memory::Write16(0x80000000u | (kSamplesPhys + i * 2u), static_cast<std::uint16_t>(kLevel));
    }
    // Parameter block (halfword offsets per AXPBWii minus `updates`, see ax_internal.h).
    PbWriter pb;
    pb.set(0, 0);                                // next_pb
    pb.set(1, 0);
    pb.set(2, static_cast<std::uint16_t>(kPb >> 16));  // this_pb
    pb.set(3, static_cast<std::uint16_t>(kPb));
    pb.set(4, 1);                                // src_type: linear
    pb.set(5, 0);                                // coef_select
    pb.set(6, 0);                                // mixer_control hi
    pb.set(7, 0x0003);                           // mixer_control lo: main L + R
    pb.set(8, 1);                                // running
    pb.set(9, 0);                                // is_stream
    // mixer: main_left {vol, delta}, main_right {vol, delta} ...
    pb.set(10, 0x7FFF);
    pb.set(12, 0x7FFF);
    // initial_time_delay (7), dpop (12) follow: 10 + 24 = 34 .. 34+7 = 41 .. 41+12 = 53.
    const unsigned vol_env = 53;
    pb.set(vol_env + 0, 0x7FFF);                 // cur_volume
    pb.set(vol_env + 1, 0);
    const unsigned audio_addr = vol_env + 2;
    const std::uint32_t start = kSamplesPhys / 2u;  // 16-bit samples are addressed in halfwords
    const std::uint32_t end = start + 4000u;
    pb.set(audio_addr + 0, 1);                   // looping
    pb.set(audio_addr + 1, 0x000A);              // PCM16
    pb.set(audio_addr + 2, static_cast<std::uint16_t>(start >> 16));
    pb.set(audio_addr + 3, static_cast<std::uint16_t>(start));
    pb.set(audio_addr + 4, static_cast<std::uint16_t>(end >> 16));
    pb.set(audio_addr + 5, static_cast<std::uint16_t>(end));
    pb.set(audio_addr + 6, static_cast<std::uint16_t>(start >> 16));
    pb.set(audio_addr + 7, static_cast<std::uint16_t>(start));
    const unsigned adpcm = audio_addr + 8;       // coefs[16], gain, pred_scale, yn1, yn2
    pb.set(adpcm + 16, 0x0800);                  // gain: unity for PCM with the default shift
    const unsigned src = adpcm + 20;             // ratio_hi, ratio_lo, cur_addr_frac, last[4]
    pb.set(src + 0, 1);
    pb.set(src + 1, 0);
    for (unsigned i = 0; i < pb.w.size(); ++i) {
        Memory::Write16(kPb + i * 2u, pb.w[i]);
    }
    // SETUP data: zero initial bus values (0x20 buses x 3 halfwords).
    for (unsigned i = 0; i < 0x40; ++i) Memory::Write32(kSetup + i * 4u, 0);

    // Command list: SETUP, PROCESS, OUTPUT (volume, surround, L/R), END.
    const std::uint16_t cmds[] = {
        0x0000, static_cast<std::uint16_t>(kSetup >> 16), static_cast<std::uint16_t>(kSetup),
        0x0004, static_cast<std::uint16_t>(kPb >> 16), static_cast<std::uint16_t>(kPb),
        0x000B, 0x8000, static_cast<std::uint16_t>(kOutS >> 16), static_cast<std::uint16_t>(kOutS),
        static_cast<std::uint16_t>(kOutLR >> 16), static_cast<std::uint16_t>(kOutLR),
        0x000E,
    };
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        Memory::Write16(kCmdList + i * 2u, cmds[i]);
    }
    const std::uint32_t words = sizeof(cmds) / sizeof(cmds[0]);
    for (int frame = 0; frame < 3; ++frame) {
        if (!call(cpu, 0x8015D430u, {0xBABE0000u | words})) return fail("mail size");
        if (!call(cpu, 0x8015D430u, {kCmdList})) return fail("mail list");
    }
    // Right, left pairs, big-endian, per sample.
    const std::int16_t r = static_cast<std::int16_t>(Memory::Read16(kOutLR + 50u * 4u));
    const std::int16_t l = static_cast<std::int16_t>(Memory::Read16(kOutLR + 50u * 4u + 2u));
    std::printf("audio_hle: output sample 50 L=%d R=%d (voice level %d)\n", l, r, kLevel);
    if (l < kLevel - 64 || l > kLevel + 64 || r < kLevel - 64 || r > kLevel + 64) {
        return fail("mixed output does not carry the voice");
    }
    if (Memory::Read16(kPb + 8u * 2u) != 1u) {
        return fail("a looping voice must still be running");
    }

    // AI DMA: blocks fall due by wall-clock time; each goes to the sink (left/right, host order)
    // and runs the guest's DMA callback.
    constexpr std::uint32_t kCallback = 0x80001230u;
    mkwv_register_extra_native(kCallback, &ai_callback);
    mkwv::runtime::set_audio_sink(&sink);
    call(cpu, 0x801240B0u, {0});                       // AIInit
    call(cpu, 0x80123F88u, {kCallback});               // AIRegisterDMACallback
    call(cpu, 0x80123FCCu, {kOutLR, 96u * 4u});        // AIInitDMA: one 3 ms block
    call(cpu, 0x80124048u, {});                        // AIStartDMA
    mkwv::runtime::audio_poll(&cpu);                   // starts the clock
    std::this_thread::sleep_for(std::chrono::milliseconds(31));
    mkwv::runtime::audio_poll(&cpu);
    std::printf("audio_hle: %d callbacks, %u frames to the sink, first left %d\n", g_callbacks, g_sink_frames,
                g_first_left);
    if (g_callbacks < 8 || g_callbacks > 40 || g_sink_frames != static_cast<std::uint32_t>(g_callbacks) * 96u) {
        return fail("AI DMA blocks");
    }
    if (g_first_left < kLevel - 64 || g_first_left > kLevel + 64) {
        return fail("the sink must get the mixed block, left channel first");
    }
    char perf_text[160];
    mkwv::runtime::audio_perf_text(perf_text, sizeof(perf_text));
    std::printf("audio_hle:%s\n", perf_text);
    if (std::strstr(perf_text, " | audio: blocks=") == nullptr) {
        return fail("PERF text");
    }

    // Fractional polyphase resampling across three command lists. Use an independent
    // scalar circular-history reference, then check output and persisted PB history.
    for (std::uint32_t i = 0; i < 4096; ++i) {
        Memory::Write16(0x80000000u | (kSamplesPhys + i * 2u), 0x1000u + (i * 31u) % 2048u);
    }
    pb.set(4, 0);  // polyphase
    pb.set(5, 1);  // second coefficient bank
    for (const std::uint32_t ratio : {0xbeefu, 0x16359u, 0x328abu}) {
        pb.set(src, static_cast<std::uint16_t>(ratio >> 16));
        pb.set(src + 1, static_cast<std::uint16_t>(ratio));
        pb.set(src + 2, 0x2000);  // begin between samples
        std::int16_t history[4] = {137, 251, 367, 479};
        for (unsigned i = 0; i < 4; ++i) pb.set(src + 3 + i, history[i]);
        for (unsigned i = 0; i < pb.w.size(); ++i) Memory::Write16(kPb + i * 2u, pb.w[i]);
        std::uint32_t phase = 0x2000, read_samples = 0;
        for (unsigned frame = 0; frame < 3; ++frame) {
            std::int16_t expected[96];
            unsigned head = 0;
            for (unsigned i = 0; i < 96; ++i) {
                phase += ratio;
                while (phase >= 0x10000) {
                    history[head++ & 3u] = static_cast<std::int16_t>(0x1000u + (read_samples++ * 31u) % 2048u);
                    phase -= 0x10000;
                }
                const unsigned tap = ((phase >> 9) & 3u);
                expected[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(history[(head + tap) & 3u]) * 0x7fff) >> 15);
            }
            std::int16_t persisted[4];
            for (unsigned i = 0; i < 4; ++i) persisted[i] = history[(head + i) & 3u];
            std::memcpy(history, persisted, sizeof(history));
            if (!call(cpu, 0x8015D430u, {0xBABE0000u | words}) || !call(cpu, 0x8015D430u, {kCmdList})) {
                return fail("polyphase mail");
            }
            for (unsigned i = 0; i < 96; ++i) {
                const int left = static_cast<std::int16_t>(Memory::Read16(kOutLR + i * 4u + 2u));
                const int right = static_cast<std::int16_t>(Memory::Read16(kOutLR + i * 4u));
                // Envelope/main volumes are 0x7fff: each fixed-point stage rounds down.
                if (left > expected[i] || left < expected[i] - 4 || right != left) {
                    return fail("polyphase output/history rotation");
                }
            }
            if (Memory::Read16(kPb + (src + 2) * 2u) != phase) return fail("polyphase fractional position");
            for (unsigned i = 0; i < 4; ++i) {
                if (static_cast<std::int16_t>(Memory::Read16(kPb + (src + 3 + i) * 2u)) != history[i]) {
                    return fail("polyphase persisted history");
                }
            }
        }
    }
    return 0;
}
