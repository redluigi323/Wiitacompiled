#include "mkwv/runtime/guest_prof.hpp"

#include "mkwv/runtime/diagnostics.hpp"
#include "mkwv/runtime/hle_audio.hpp"

#include <cstdio>
#include <cstring>

namespace mkwv::runtime::prof {
namespace {


constexpr unsigned kSlots = 2048;  // power of two
struct Slot {
    std::uint32_t addr;
    std::uint32_t self;
    std::uint32_t incl;
};
// One table set per guest thread slot (0 = game thread, 1 = sound thread).
struct Tables {
    Slot funcs[kSlots];
    unsigned used = 0;
    // Link-register samples (exact return addresses).
    Slot lrs[kSlots];
    unsigned lr_used = 0;
    std::uint32_t act[kActivityCount];
    std::uint32_t samples = 0;
    std::uint32_t overflow = 0;  // addresses dropped: table full
};
Tables g_tables[2];
unsigned g_report_every = 20000;  // ~10 s at 2 kHz
bool g_enabled = true;

bool sound_thread_sampled() {
    return sound_thread_running();
}

Slot* slot_in(Slot* table, unsigned& used, std::uint32_t addr) {
    unsigned h = (addr >> 2) * 2654435761u;
    for (unsigned probe = 0; probe < kSlots; ++probe) {
        Slot& s = table[(h + probe) & (kSlots - 1u)];
        if (s.addr == addr) {
            return &s;
        }
        if (s.addr == 0) {
            if (used >= kSlots * 3u / 4u) {
                return nullptr;
            }
            s.addr = addr;
            ++used;
            return &s;
        }
    }
    return nullptr;
}

void top(const Tables& t, bool by_self, char* out, std::size_t cap, const Slot* table, const char* prefix,
         const char* label) {
    constexpr unsigned kTop = 24;
    const Slot* best[kTop] = {};
    for (unsigned k = 0; k < kSlots; ++k) {
        const Slot& s = table[k];
        if (s.addr == 0) continue;
        const std::uint32_t v = by_self ? s.self : s.incl;
        if (v == 0) continue;
        for (unsigned i = 0; i < kTop; ++i) {
            const std::uint32_t bv = best[i] == nullptr ? 0u : (by_self ? best[i]->self : best[i]->incl);
            if (best[i] == nullptr || v > bv) {
                for (unsigned j = kTop - 1; j > i; --j) best[j] = best[j - 1];
                best[i] = &s;
                break;
            }
        }
    }
    int n = std::snprintf(out, cap, "mkwv: PROF %s%s", prefix, label);
    const double total = t.samples != 0 ? static_cast<double>(t.samples) : 1.0;
    for (unsigned i = 0; i < kTop && best[i] != nullptr && n > 0 && static_cast<std::size_t>(n) < cap; ++i) {
        const std::uint32_t v = by_self ? best[i]->self : best[i]->incl;
        n += std::snprintf(out + n, cap - static_cast<std::size_t>(n), " %08X:%.1f", best[i]->addr,
                           100.0 * static_cast<double>(v) / total);
    }
    if (n > 0 && static_cast<std::size_t>(n) < cap - 1) {
        out[n] = '\n';
        out[n + 1] = '\0';
    }
}

void report_slot(unsigned which) {
    static const char* kNames[kActivityCount] = {"game", "mix", "irq", "sndthr", "rwait", "sleep", "handoff", "lock"};
    Tables& t = g_tables[which];
    const char* prefix = which == 0 ? "" : "snd ";
    char line[640];
    int n = std::snprintf(line, sizeof(line), "mkwv: PROF %ssamples=%u act", prefix, t.samples);
    const double total = t.samples != 0 ? static_cast<double>(t.samples) : 1.0;
    for (unsigned a = 0; a < kActivityCount && n > 0 && static_cast<std::size_t>(n) < sizeof(line); ++a) {
        // The sound thread never waits for the renderer; name its slots by what it does.
        const char* name = which == 1 && a == kGame ? "other" : kNames[a];
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s=%.1f%%", name,
                           100.0 * static_cast<double>(t.act[a]) / total);
    }
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(line)) {
        std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " | funcs=%u dropped=%u\n", t.used,
                      t.overflow);
    }
    watchdog_log_line(line);
    top(t, true, line, sizeof(line), t.funcs, prefix, "self");
    watchdog_log_line(line);
    top(t, false, line, sizeof(line), t.funcs, prefix, "incl");
    watchdog_log_line(line);
    top(t, true, line, sizeof(line), t.lrs, prefix, "lr");
    watchdog_log_line(line);
    std::memset(t.lrs, 0, sizeof(t.lrs));
    t.lr_used = 0;
    std::memset(t.funcs, 0, sizeof(t.funcs));
    std::memset(t.act, 0, sizeof(t.act));
    t.used = 0;
    t.samples = 0;
    t.overflow = 0;
}

void sample_slot(unsigned which) {
    Tables& t = g_tables[which];
    const std::uint32_t act = g_activity_slot[which];
    ++t.samples;
    if (act < kActivityCount) {
        ++t.act[act];
    }
    // Guest code: attribute the sample to the shadow stack.
    if (act != kGame && act != kSoundThread && act != kAudioIrq) {
        return;
    }
    const Stack* s = g_stack_slot[which];
    std::uint32_t depth = s->depth;
    if (depth > kStackDepth) depth = kStackDepth;
    std::uint32_t frames[kStackDepth];
    for (std::uint32_t i = 0; i < depth; ++i) frames[i] = s->addr[i];
    if (const volatile std::uint32_t* lrp = s->lr) {
        const std::uint32_t lr = *lrp;
        if (lr >= 0x80000000u && lr < 0x81800000u) {
            if (Slot* ls = slot_in(t.lrs, t.lr_used, lr)) ++ls->self;
        }
    }
    if (depth == 0) {
        return;
    }
    if (Slot* top_slot = slot_in(t.funcs, t.used, frames[depth - 1])) {
        ++top_slot->self;
    } else {
        ++t.overflow;
    }
    for (std::uint32_t i = 0; i < depth; ++i) {
        bool seen = false;
        for (std::uint32_t j = 0; j < i && !seen; ++j) seen = frames[j] == frames[i];
        if (seen) continue;
        if (Slot* sl = slot_in(t.funcs, t.used, frames[i])) {
            ++sl->incl;
        } else {
            ++t.overflow;
        }
    }
}

} // namespace

Stack g_own_stacks[2];
Stack* volatile g_stack_slot[2] = {&g_own_stacks[0], &g_own_stacks[1]};
volatile std::uint32_t g_activity_slot[2] = {kGame, kGame};

void sample() {
    if (!g_enabled) {
        return;
    }
    sample_slot(0);
    // The sound thread only once it exists (its activity starts as kGame, i.e. "other").
    if (sound_thread_sampled()) {
        sample_slot(1);
    }
    if (g_tables[0].samples >= g_report_every) {
        report_slot(0);
        if (g_tables[1].samples != 0) {
            report_slot(1);
        }
    }
}

void set_report_interval(unsigned samples) {
    g_report_every = samples < 100u ? 100u : samples;
}

bool enabled() {
    return g_enabled;
}

void set_enabled(bool on) {
    g_enabled = on;
}

} // namespace mkwv::runtime::prof
