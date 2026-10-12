#include "mkwv/platform/clocks.hpp"

#include <psp2/power.h>

namespace mkwv::platform {

Clocks read_clocks() {
    Clocks clocks;
    clocks.arm_mhz = scePowerGetArmClockFrequency();
    clocks.gpu_mhz = scePowerGetGpuClockFrequency();
    clocks.bus_mhz = scePowerGetBusClockFrequency();
    clocks.xbar_mhz = scePowerGetGpuXbarClockFrequency();
    clocks.arm_500_accepted = clocks.arm_mhz >= 490;
    return clocks;
}

Clocks apply_performance_clocks() {
    // Hard userland ceiling. 500 MHz is attempted second; it only sticks with
    // a kernel overclock plugin.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    const int requested_500 = scePowerSetArmClockFrequency(500);
    Clocks clocks = read_clocks();
    clocks.arm_500_accepted = requested_500 == 0 && clocks.arm_mhz >= 490;
    if (!clocks.arm_500_accepted) {
        scePowerSetArmClockFrequency(444);
        clocks = read_clocks();
        clocks.arm_500_accepted = false;
    }
    return clocks;
}

} // namespace mkwv::platform
