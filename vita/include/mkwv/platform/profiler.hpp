#pragma once

namespace mkwv::platform {

// Starts the guest sampling profiler's thread (mkwv/runtime/guest_prof.hpp): ~2 kHz samples,
// a PROF report in the watchdog log about every 10 s.
bool start_guest_profiler();
// Host-PC sampling of the threads registered with perf::register_thread (host_profiler.cpp):
// HPROF lines in the watchdog log about every 5 s.
bool start_host_profiler();

} // namespace mkwv::platform
