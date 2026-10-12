#pragma once

namespace mkwv::platform {

struct Clocks {
    int arm_mhz = 0;
    int gpu_mhz = 0;
    int bus_mhz = 0;
    int xbar_mhz = 0;
    bool arm_500_accepted = false;
};

// Request the fastest userland clocks, then try 500 MHz ARM. 500 only sticks
// when a kernel overclock plugin is present; failure is not fatal.
Clocks apply_performance_clocks();
Clocks read_clocks();

} // namespace mkwv::platform
