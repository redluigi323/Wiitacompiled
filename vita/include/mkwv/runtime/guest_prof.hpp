#pragma once

// Sampling profiler for the guest thread (not shard-visible).
//
// The guest thread keeps two things up to date, both a couple of stores:
//   - a shadow stack of the guest functions entered through invoke_guest (virtual calls,
//     function pointers, dynamically dispatched natives) - static translated calls do not go
//     through it, so each frame covers everything its target calls statically;
//   - an activity tag (game code, audio mixing, the audio interrupt, the sound thread, waiting
//     for the render thread, sleeping, handing FIFO bytes over).
// A sampler thread on another core reads both about 2000 times a second and every ~10 s logs
// `mkwv: PROF` lines: the activity split, then the hottest guest functions by self samples
// (innermost frame), by inclusive samples (anywhere on the stack) and by the guest link register
// (the call site most recently passed, inside static code too). Addresses are guest
// addresses (upstream/projects/mkwii/MAP.txt names them).

#include <cstdint>

#include "mkwv/runtime/guest_sync.hpp"

namespace mkwv::runtime::prof {

enum Activity : std::uint32_t {
    kGame = 0,
    kAudioMix,     // the DSP HLE mixing a command list
    kAudioIrq,     // the AI DMA callback (the "audio interrupt"), guest code
    kSoundThread,  // a cooperative guest thread (nw4r's sound/task threads)
    kRenderWait,   // waiting for the render thread (includes helping with vertex jobs)
    kSleep,        // retrace pacing and OSSleepThread/OSSleepTicks
    kHandoff,      // copying FIFO bytes into the render queue
    kLockWait,     // waiting for the guest lock or a guest mutex the other guest thread holds
    kActivityCount
};

constexpr unsigned kStackDepth = 48;

struct Stack {
    volatile std::uint32_t depth = 0;
    volatile std::uint32_t addr[kStackDepth] = {};
    // The guest link register of the context running on this stack. Translated code stores the
    // return address into ctx->lr before every call, static ones included, so a sample of it
    // points into the function that made the latest call: a call-site-granular flat profile
    // that sees inside statically called code (PROF lr lines; name them with MAP.txt).
    const volatile std::uint32_t* volatile lr = nullptr;
};

// Per guest thread slot (guest_sync.hpp: 0 = game thread, 1 = audio thread). The sampler
// watches both and reports them separately (PROF ... and PROF snd ...).
extern Stack* volatile g_stack_slot[2];
extern volatile std::uint32_t g_activity_slot[2];
inline void register_sampled_thread() {}

inline void push(unsigned slot, std::uint32_t address, const std::uint32_t* lr) {
    Stack* s = g_stack_slot[slot];
    s->lr = lr;
    const std::uint32_t d = s->depth;
    if (d < kStackDepth) {
        s->addr[d] = address;
    }
    s->depth = d + 1u;
}

inline void pop(unsigned slot) {
    Stack* s = g_stack_slot[slot];
    const std::uint32_t d = s->depth;
    if (d != 0) {
        s->depth = d - 1u;
    }
}

class ActivityScope {
public:
    explicit ActivityScope(Activity a) : slot_(guest_thread_slot()), saved_(g_activity_slot[slot_]) {
        g_activity_slot[slot_] = a;
    }
    ~ActivityScope() { g_activity_slot[slot_] = saved_; }
    ActivityScope(const ActivityScope&) = delete;
    ActivityScope& operator=(const ActivityScope&) = delete;

private:
    unsigned slot_;
    std::uint32_t saved_;
};

// Swaps the shadow stack (a cooperative fiber has its own call stack).
class StackScope {
public:
    explicit StackScope(Stack* s) : slot_(guest_thread_slot()), saved_(g_stack_slot[slot_]) { g_stack_slot[slot_] = s; }
    ~StackScope() { g_stack_slot[slot_] = saved_; }
    StackScope(const StackScope&) = delete;
    StackScope& operator=(const StackScope&) = delete;

private:
    unsigned slot_;
    Stack* saved_;
};

// Sampler side (any thread): take one sample; every `report_every` samples, write the report
// to the watchdog log and start over.
void sample();
void set_report_interval(unsigned samples);
bool enabled();
void set_enabled(bool on);

} // namespace mkwv::runtime::prof
