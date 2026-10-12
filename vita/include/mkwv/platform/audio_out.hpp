#pragma once

#include <cstdint>

namespace mkwv::platform {

// Opens the audio output (32 kHz stereo) and installs it as the audio HLE's sink.
bool start_audio_out();
void audio_out_stats(std::uint32_t* queued_ms, std::uint32_t* underruns, std::uint32_t* dropped_ms);

} // namespace mkwv::platform
