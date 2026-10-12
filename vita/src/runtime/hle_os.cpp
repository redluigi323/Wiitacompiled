#include "mkwv/runtime/hle_os.hpp"

#include "mkwv/gx/efb_copy.hpp"
#include "mkwv/gx/perf.hpp"

#include "mkwv/platform/data_root.hpp"
#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/cpu_context.hpp"
#include "mkwv/runtime/diagnostics.hpp"
#include "mkwv/runtime/dispatch.hpp"
#include "mkwv/runtime/guest_image.hpp"
#include "mkwv/runtime/hle_native.hpp"
#include "mkwv/runtime/hle_audio.hpp"
#include "mkwv/runtime/guest_prof.hpp"
#include "mkwv/runtime/guest_sync.hpp"

#include "abi_bridge.h"
#include "memory.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__vita__)
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// These SDK entries are linked from Vita's generated static image.  The EGG
// HLE boundary below calls them directly so it cannot accidentally return to
// itself through the native indirect-dispatch override.
extern "C" void func_80198D88(CpuContext* ctx);
extern "C" void func_80199038(CpuContext* ctx);

// This entry is compiled, but upstream's native override removes it from the
// generated indirect table. Calling the symbol avoids recursing into our HLE.
extern "C" void func_8055531C(CpuContext* ctx);
extern "C" void func_8052C7E4(CpuContext* ctx);
extern "C" void func_80672CC8(CpuContext* ctx);

namespace {

constexpr std::uint32_t kInterruptHandlerTablePtrAddr = 0x803868f8u;
constexpr std::uint32_t kInterruptHandlerTableAddr = 0x80003040u;
constexpr std::uint32_t kInterruptMaskLoAddr = 0x800000c4u;
constexpr std::uint32_t kInterruptMaskHiAddr = 0x800000c8u;
constexpr std::uint32_t kOSCurrentContextAddr = 0x800000d4u;
constexpr std::uint32_t kOSRunningContextAddr = 0x800000e4u;
constexpr std::uint32_t kDefaultThreadAddr = 0x80347498u;
constexpr std::uint32_t kOSThreadBytes = 0x318u;
constexpr std::uint32_t kGXDataAddr = 0x803437C0u;
constexpr std::uint32_t kGXDataPtrAddr = 0x803886C8u;
constexpr std::uint32_t kFifoObjAddr = 0x80343740u;

// Per guest thread slot (guest_sync.hpp): with the sound thread on, the game thread and the
// audio thread each have their own interrupt state (fibers save and restore theirs on the audio
// thread).
bool g_irqs_slot[2] = {true, true};
#define g_irqs (g_irqs_slot[mkwv::runtime::guest_thread_slot()])
std::atomic<std::uint64_t> g_timebase{0};
std::uint32_t g_power_cb = 0;
std::uint32_t g_vi_pre_cb = 0;
std::uint32_t g_vi_post_cb = 0;
std::uint32_t g_retrace = 0;
std::chrono::steady_clock::time_point g_retrace_epoch = std::chrono::steady_clock::now();
std::uint32_t g_wpad_sync_callback = 0;
std::uint32_t g_ai_dma_callback = 0;

struct HostAlarm {
    std::uint32_t alarm;
    std::uint32_t handler;
    std::uint64_t fire;
    std::uint64_t period;
};

HostAlarm g_alarms[16];
unsigned g_nalarms = 0;
bool g_alarm_busy = false;
unsigned g_job_depth = 0;
unsigned g_report_depth = 0;
unsigned g_ncreate = 0;
unsigned g_nsend = 0;
unsigned g_nqueue = 0;
std::uint32_t g_task_thread = 0;
std::uint32_t g_game_heap = 0x80399238u;
bool g_heap_seen = false;
bool g_ensure_task = false;
std::atomic<bool> g_rel_prolog_done{false};
std::atomic<unsigned> g_rel_link_calls{0};
std::atomic<std::uint32_t> g_rel_module{0};
std::atomic<std::uint32_t> g_rel_entry{0};
std::uint32_t g_rel_reservation = 0;
bool g_strap_input_logged = false;
std::atomic<unsigned> g_strap_input_samples{0};
std::atomic<std::uint32_t> g_strap_buttons{0};
std::atomic<const char*> g_rel_phase{"idle"};

constexpr std::uint32_t kRipperSdaOff = 27648u;
constexpr std::uint32_t kDefaultHeapSdaOff = 23904u;
constexpr std::uint32_t kRipperPtrAddr = mkwv::runtime::kSdaBase - kRipperSdaOff;
constexpr std::uint32_t kDefaultHeapPtrAddr = mkwv::runtime::kSdaBase - kDefaultHeapSdaOff;
constexpr std::uint32_t kRipperTaskOff = 232u;
constexpr std::uint32_t kRipperHeapOff = 172u;
constexpr std::uint32_t kRipperLangOff = 96u;
constexpr std::uint32_t kTaskJobArrayOff = 76u;
constexpr std::uint32_t kTaskJobCountOff = 80u;
constexpr std::uint32_t kStrapPathTable = 0x80258580u;
constexpr std::uint32_t kTaskThreadCreate = 0x802428E8u;
constexpr std::uint32_t kGetRipper = 0x8000A5D0u;
constexpr std::uint32_t kRipperCtor = 0x8000A688u;
constexpr std::uint32_t kExpHeapAlloc = 0x80226C04u;
constexpr std::uint32_t kEggHeapAlloc = 0x80229814u;
constexpr std::uint32_t kRipperBytes = 4352u;
constexpr std::uint32_t kFrSig = 0x4652u;
constexpr std::uint32_t kUdSig = 0x5544u;
constexpr std::uint32_t kThreadRunEntry = 0x8024373Cu;

constexpr std::uint32_t kSysconfSize = 0x4000u;
constexpr std::uint8_t kScByte = 3;
constexpr std::uint8_t kScBool = 7;
constexpr std::uint8_t kScLong = 5;
constexpr std::uint8_t kScSmallArray = 2;
constexpr std::uint8_t kScBigArray = 1;
std::uint8_t g_sysconf[kSysconfSize];
bool g_sysconf_ready = false;
std::uint32_t g_nand_off = 0;

void set_r3(CpuContext* ctx, std::uint32_t value) {
    if (ctx != nullptr) {
        ctx->gpr[3] = value;
    }
}

void set_time(CpuContext* ctx, std::uint64_t value) {
    if (ctx == nullptr) {
        return;
    }
    ctx->gpr[3] = static_cast<std::uint32_t>(value >> 32);
    ctx->gpr[4] = static_cast<std::uint32_t>(value);
}

std::uint64_t bump_time(std::uint64_t add) {
    return g_timebase.fetch_add(add, std::memory_order_relaxed) + add;
}

void guest_memset(std::uint32_t addr, std::uint32_t bytes, std::uint8_t value) {
    while (bytes > 0) {
        const std::uint32_t into = addr & (mkwv::runtime::kPageSize - 1u);
        std::uint32_t chunk = mkwv::runtime::kPageSize - into;
        if (chunk > bytes) {
            chunk = bytes;
        }
        std::uint8_t* host = Memory::GetPointer(addr, chunk);
        if (host != nullptr) {
            std::memset(host, value, chunk);
        } else {
            for (std::uint32_t i = 0; i < chunk; ++i) {
                Memory::Write8(addr + i, value);
            }
        }
        addr += chunk;
        bytes -= chunk;
    }
}

void guest_copy(std::uint32_t addr, const std::uint8_t* src, std::uint32_t bytes) {
    while (bytes > 0) {
        const std::uint32_t into = addr & (mkwv::runtime::kPageSize - 1u);
        std::uint32_t chunk = mkwv::runtime::kPageSize - into;
        if (chunk > bytes) {
            chunk = bytes;
        }
        std::uint8_t* host = Memory::GetPointer(addr, chunk);
        if (host != nullptr) {
            std::memcpy(host, src, chunk);
        } else {
            for (std::uint32_t i = 0; i < chunk; ++i) {
                Memory::Write8(addr + i, src[i]);
            }
        }
        addr += chunk;
        src += chunk;
        bytes -= chunk;
    }
}

void wr_be16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 8);
    p[1] = static_cast<std::uint8_t>(value);
}

void wr_be32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 24);
    p[1] = static_cast<std::uint8_t>(value >> 16);
    p[2] = static_cast<std::uint8_t>(value >> 8);
    p[3] = static_cast<std::uint8_t>(value);
}

void sysconf_add(std::uint8_t* body, std::uint32_t* used, std::uint32_t* offs, std::uint16_t* luts, int* count,
                 std::uint8_t type, const char* name, std::uint16_t lut, const std::uint8_t* data, std::uint32_t dlen) {
    offs[*count] = *used;
    luts[*count] = lut;
    std::uint8_t* p = body + *used;
    const std::uint32_t nlen = static_cast<std::uint32_t>(std::strlen(name));
    *p++ = static_cast<std::uint8_t>((type << 5) | (nlen - 1u));
    std::memcpy(p, name, nlen);
    p += nlen;
    if (type == kScBigArray) {
        wr_be16(p, static_cast<std::uint16_t>(dlen - 1u));
        p += 2;
        std::memcpy(p, data, dlen);
        p += dlen;
    } else if (type == kScSmallArray) {
        *p++ = static_cast<std::uint8_t>(dlen - 1u);
        std::memcpy(p, data, dlen);
        p += dlen;
    } else {
        std::memcpy(p, data, dlen);
        p += dlen;
    }
    *used = static_cast<std::uint32_t>(p - body);
    ++*count;
}

void build_sysconf() {
    if (g_sysconf_ready) {
        return;
    }
    std::uint8_t body[0x1800];
    std::uint32_t used = 0;
    std::uint32_t offs[32];
    std::uint16_t luts[32];
    int count = 0;

    const std::uint8_t lng = 1;
    const std::uint8_t ar = 1;
    const std::uint8_t e60 = 1;
    const std::uint8_t pgs = 1;
    const std::uint8_t ssv = 1;
    const std::uint8_t snd = 1;
    const std::uint8_t upt = 2;
    const std::uint8_t dh = 0;
    const std::uint8_t done = 1;
    const std::uint8_t bar = 1;
    const std::uint8_t mot = 1;
    const std::uint8_t spkv = 0x58;
    std::uint8_t cb[4]{};
    std::uint8_t inc[4]{};
    std::uint8_t frc[4]{};
    std::uint8_t sens[4]{};
    wr_be32(inc, 8);
    wr_be32(frc, 0x28);
    wr_be32(sens, 3);

    std::uint8_t nik[22]{};
    nik[1] = 'M';
    nik[3] = 'K';
    nik[5] = 'W';
    nik[21] = 3;
    std::uint8_t idl[2] = {1, 2};
    std::uint8_t pc[0x4A]{};
    pc[1] = 0x04;
    pc[2] = 0x14;
    std::uint8_t sadr[0x1008]{};
    sadr[0] = 0x6E;

    sysconf_add(body, &used, offs, luts, &count, kScByte, "IPL.LNG", 0x3FE4, &lng, 1);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "IPL.AR", 0x3FF8, &ar, 1);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "IPL.E60", 0x3FEE, &e60, 1);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "IPL.PGS", 0x3FDE, &pgs, 1);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "IPL.SSV", 0x3FDC, &ssv, 1);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "IPL.SND", 0x3FD8, &snd, 1);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "IPL.UPT", 0x3FD6, &upt, 1);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "IPL.DH", 0x3FF0, &dh, 1);
    sysconf_add(body, &used, offs, luts, &count, kScBool, "IPL.CD", 0x3FF4, &done, 1);
    sysconf_add(body, &used, offs, luts, &count, kScBool, "IPL.CD2", 0x3FF2, &done, 1);
    sysconf_add(body, &used, offs, luts, &count, kScBool, "IPL.EULA", 0x3FEC, &done, 1);
    sysconf_add(body, &used, offs, luts, &count, kScLong, "IPL.CB", 0x3FFA, cb, 4);
    sysconf_add(body, &used, offs, luts, &count, kScLong, "IPL.INC", 0x3FE6, inc, 4);
    sysconf_add(body, &used, offs, luts, &count, kScLong, "IPL.FRC", 0x3FEA, frc, 4);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "BT.BAR", 0x3FB8, &bar, 1);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "BT.MOT", 0x3FBA, &mot, 1);
    sysconf_add(body, &used, offs, luts, &count, kScByte, "BT.SPKV", 0x3FBC, &spkv, 1);
    sysconf_add(body, &used, offs, luts, &count, kScLong, "BT.SENS", 0x3FBE, sens, 4);
    sysconf_add(body, &used, offs, luts, &count, kScSmallArray, "IPL.NIK", 0x3FE2, nik, sizeof(nik));
    sysconf_add(body, &used, offs, luts, &count, kScSmallArray, "IPL.IDL", 0x3FE8, idl, sizeof(idl));
    sysconf_add(body, &used, offs, luts, &count, kScSmallArray, "IPL.PC", 0x3FE0, pc, sizeof(pc));
    sysconf_add(body, &used, offs, luts, &count, kScBigArray, "IPL.SADR", 0x3FDA, sadr, sizeof(sadr));

    std::memset(g_sysconf, 0, sizeof(g_sysconf));
    std::memcpy(g_sysconf, "SCv0", 4);
    wr_be16(g_sysconf + 4, static_cast<std::uint16_t>(count));
    const std::uint32_t table = 6;
    const std::uint32_t body_at = table + static_cast<std::uint32_t>(count + 1) * 2u;
    for (int i = 0; i < count; ++i) {
        wr_be16(g_sysconf + table + static_cast<std::uint32_t>(i) * 2u,
                static_cast<std::uint16_t>(body_at + offs[i]));
    }
    wr_be16(g_sysconf + table + static_cast<std::uint32_t>(count) * 2u,
            static_cast<std::uint16_t>(body_at + used));
    std::memcpy(g_sysconf + body_at, body, used);
    std::memcpy(g_sysconf + 0x3FFCu, "SCed", 4);
    for (int i = 0; i < count; ++i) {
        if (luts[i] != 0) {
            wr_be16(g_sysconf + luts[i], static_cast<std::uint16_t>(table + static_cast<std::uint32_t>(i) * 2u));
        }
    }
    g_sysconf_ready = true;
}

std::uint32_t sysconf_length() {
    build_sysconf();
    return kSysconfSize;
}

void reset_os_hle_state() {
    g_irqs = true;
    g_timebase = 0;
    g_power_cb = 0;
    g_vi_pre_cb = 0;
    g_vi_post_cb = 0;
    g_retrace = 0;
    g_retrace_epoch = std::chrono::steady_clock::now();
    g_wpad_sync_callback = 0;
    g_ai_dma_callback = 0;
    g_nalarms = 0;
    g_alarm_busy = false;
    g_nand_off = 0;
    g_job_depth = 0;
    g_report_depth = 0;
    g_ncreate = 0;
    g_nsend = 0;
    g_nqueue = 0;
    g_task_thread = 0;
    g_game_heap = 0x80399238u;
    g_heap_seen = false;
    g_ensure_task = false;
    g_rel_prolog_done = false;
    g_rel_link_calls = 0;
    g_rel_module = 0;
    g_rel_entry = 0;
    g_rel_reservation = 0;
    g_strap_input_logged = false;
    g_strap_input_samples = 0;
    g_strap_buttons = 0;
    g_rel_phase = "idle";
}

void guest_string(std::uint32_t addr, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0) {
        return;
    }
    out[0] = 0;
    if (addr == 0) {
        return;
    }
    std::size_t n = 0;
    while (n + 1 < cap) {
        const std::uint8_t ch = Memory::Read8(addr + static_cast<std::uint32_t>(n));
        if (ch == 0) {
            break;
        }
        out[n++] = static_cast<char>(ch);
    }
    out[n] = 0;
}

void format_guest_printf(char* out, std::size_t cap, CpuContext* ctx, std::uint32_t fmt_addr, int first_gpr) {
    if (out == nullptr || cap == 0) {
        return;
    }
    out[0] = 0;
    if (ctx == nullptr || fmt_addr == 0) {
        return;
    }
    char fmt[256];
    guest_string(fmt_addr, fmt, sizeof(fmt));
    std::size_t o = 0;
    int gpr = first_gpr;
    int fpr = 1;
    for (std::size_t i = 0; fmt[i] != 0 && o + 1 < cap; ++i) {
        if (fmt[i] != '%') {
            out[o++] = fmt[i];
            continue;
        }
        ++i;
        while (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' || fmt[i] == '0' ||
               (fmt[i] >= '0' && fmt[i] <= '9') || fmt[i] == '.' || fmt[i] == 'l' || fmt[i] == 'z') {
            ++i;
        }
        char tmp[64];
        tmp[0] = 0;
        switch (fmt[i]) {
        case '%':
            tmp[0] = '%';
            tmp[1] = 0;
            break;
        case 's': {
            char str[160];
            const std::uint32_t ptr = (gpr < 32) ? ctx->gpr[gpr++] : 0;
            guest_string(ptr, str, sizeof(str));
            std::snprintf(tmp, sizeof(tmp), "%s", str);
            break;
        }
        case 'd':
        case 'i':
            std::snprintf(tmp, sizeof(tmp), "%d", (gpr < 32) ? static_cast<int>(ctx->gpr[gpr++]) : 0);
            break;
        case 'u':
            std::snprintf(tmp, sizeof(tmp), "%u", (gpr < 32) ? ctx->gpr[gpr++] : 0u);
            break;
        case 'x':
        case 'X':
        case 'p': {
            // %llx is a title id. The other half was being printed as the
            // following %d, which turned the CINS line into a fake errcode.
            const bool wide = i >= 2 && fmt[i - 1] == 'l' && fmt[i - 2] == 'l';
            const std::uint32_t hi = (gpr < 32) ? ctx->gpr[gpr++] : 0u;
            const std::uint32_t lo = wide && gpr < 32 ? ctx->gpr[gpr++] : 0u;
            if (wide) {
                std::snprintf(tmp, sizeof(tmp), "%08X%08X", hi, lo);
            } else {
                std::snprintf(tmp, sizeof(tmp), "%08X", hi);
            }
            break;
        }
        case 'f':
        case 'g':
        case 'e':
            std::snprintf(tmp, sizeof(tmp), "%.1f", (fpr < 32) ? ctx->fpr[fpr++].d : 0.0);
            break;
        case 0:
            --i;
            break;
        default:
            tmp[0] = '%';
            tmp[1] = fmt[i];
            tmp[2] = 0;
            if (gpr < 32) {
                ++gpr;
            }
            break;
        }
        const std::size_t n = std::strlen(tmp);
        if (o + n >= cap) {
            break;
        }
        std::memcpy(out + o, tmp, n);
        o += n;
    }
    // CINS passes the install result in r7. %016llx/%d consume r4-r6, which are
    // the two title-id halves, so the printed "errcode" is not the result.
    if (std::strstr(fmt, "errcode") != nullptr && o + 16 < cap) {
        char extra[24];
        const int n = std::snprintf(extra, sizeof(extra), " result=%u", ctx->gpr[7]);
        if (n > 0 && o + static_cast<std::size_t>(n) < cap) {
            std::memcpy(out + o, extra, static_cast<std::size_t>(n));
            o += static_cast<std::size_t>(n);
        }
    }
    out[o] = 0;
}

void nand_complete(CpuContext* ctx, std::int32_t result, std::uint32_t callback, std::uint32_t block) {
    set_r3(ctx, result >= 0 ? 0u : static_cast<std::uint32_t>(result));
    if (callback == 0) {
        return;
    }
    CpuContext cb = *ctx;
    cb.gpr[3] = static_cast<std::uint32_t>(result);
    cb.gpr[4] = block;
    InvokeIndirectCpu(callback, &cb);
}

bool is_sysconf_path(const char* path) {
    return path != nullptr && std::strstr(path, "SYSCONF") != nullptr;
}

// Host mirror of the Wii NAND. SYSCONF stays the in-memory blob. Every other
// path is a real file under ux0:data/viicompiled/nand, created only when the
// game asks. Opening a path that was never created still returns NOEXISTS, so
// FaceLib does not get a fake file and spin on RFLiIsWorking.
constexpr char kNandRoot[] = "ux0:data/viicompiled/nand";
// Disc title 00010004 / RMCP. A path with no leading slash is relative to this
// directory, matching upstream NormalizeAbsoluteWiiPath.
constexpr char kNandDataDir[] = "/title/00010004/524d4350/data";
constexpr int kNandSlots = 8;
constexpr std::uint8_t kNandFlagOpen = 1;
constexpr std::uint8_t kNandFlagClosed = 2;

struct NandSlot {
    bool live = false;
    bool sysconf = false;
    std::FILE* file = nullptr;
};

NandSlot g_nand_slot[kNandSlots];

void nand_note(const char* op, const char* path, int result) {
    char line[192];
    std::snprintf(line, sizeof(line), "mkwv: NAND %s %s -> %d\n", op, path != nullptr && path[0] != 0 ? path : "(null)",
                  result);
    // Repeats are collapsed with a count, not dropped. CreateRKSYS writes 0x2BC000 bytes as 280
    // identical 10240-byte calls; silently swallowing 279 of them made the log read as a single
    // short write and sent the previous investigation after the wrong fault.
    static char last[192]{};
    static unsigned repeats = 0;
    if (std::strcmp(last, line) == 0) {
        ++repeats;
        return;
    }
    if (repeats != 0) {
        char tally[64];
        std::snprintf(tally, sizeof(tally), "mkwv: NAND   (previous line x%u more)\n", repeats);
        std::printf("%s", tally);
        mkwv::runtime::watchdog_log_line(tally);
        repeats = 0;
    }
    std::strncpy(last, line, sizeof(last) - 1u);
    last[sizeof(last) - 1u] = 0;
    std::printf("%s", line);
    mkwv::runtime::watchdog_log_line(line);
}

void nand_mkdir_one(const char* path) {
#if defined(__vita__)
    sceIoMkdir(path, 0777);
#else
    mkdir(path, 0777);
#endif
}

void nand_mkdir_parents(const char* file_path) {
    char buf[192];
    std::strncpy(buf, file_path, sizeof(buf) - 1u);
    buf[sizeof(buf) - 1u] = 0;
    for (char* p = buf + 1; *p != 0; ++p) {
        if (*p == '/') {
            *p = 0;
            nand_mkdir_one(buf);
            *p = '/';
        }
    }
}

void nand_absolutize(char* path, std::size_t cap) {
    if (path == nullptr || path[0] == 0 || path[0] == '/' || std::strstr(path, "..") != nullptr) {
        return;
    }
    char full[160];
    std::snprintf(full, sizeof(full), "%s/%s", kNandDataDir, path);
    std::strncpy(path, full, cap - 1u);
    path[cap - 1u] = 0;
}

void nand_write_guest(std::uint32_t addr, const char* text) {
    if (addr == 0 || text == nullptr) {
        return;
    }
    std::uint32_t i = 0;
    for (; text[i] != 0; ++i) {
        Memory::Write8(addr + i, static_cast<std::uint8_t>(text[i]));
    }
    Memory::Write8(addr + i, 0);
}

bool nand_host_path(const char* wii, char* out, std::size_t cap) {
    if (wii == nullptr || wii[0] == 0 || std::strstr(wii, "..") != nullptr) {
        return false;
    }
    char absolute[160];
    if (wii[0] == '/') {
        std::strncpy(absolute, wii, sizeof(absolute) - 1u);
        absolute[sizeof(absolute) - 1u] = 0;
    } else {
        std::snprintf(absolute, sizeof(absolute), "%s/%s", kNandDataDir, wii);
    }
    return std::snprintf(out, cap, "%s%s", kNandRoot, absolute) > 0;
}

int nand_kind(const char* host);

// Wii setting.txt is a 256-byte buffer XOR'd with a rotating key. MKW reads it
// for the console identity before it will accept a save. Missing file is -12,
// which is the "could not read system memory" path. PAL defaults match this disc.
void nand_ensure_setting_txt() {
    char host[192];
    if (!nand_host_path("/title/00000001/00000002/data/setting.txt", host, sizeof(host))) {
        return;
    }
    if (nand_kind(host) == 1) {
        return;
    }
    nand_mkdir_one(kNandRoot);
    nand_mkdir_parents(host);
    const char* lines[] = {
        "AREA=EUR\r\n", "MODEL=RVL-001(EUR)\r\n", "DVD=0\r\n", "MPCH=0x7FFE\r\n",
        "CODE=LEH\r\n", "SERNO=123456789\r\n", "VIDEO=PAL\r\n", "GAME=EU\r\n",
    };
    std::uint8_t bytes[256]{};
    std::size_t position = 0;
    std::uint32_t key = 0x73B5DBFAu;
    for (const char* line : lines) {
        const std::size_t start = position;
        const std::uint32_t saved = key;
        bool encoded_nul = false;
        for (std::size_t i = 0; line[i] != 0 && position < sizeof(bytes); ++i) {
            bytes[position] = static_cast<std::uint8_t>(line[i]) ^ static_cast<std::uint8_t>(key);
            encoded_nul = encoded_nul || bytes[position] == 0;
            key = (key << 1) | (key >> 31);
            ++position;
        }
        if (encoded_nul) {
            position = start;
            key = saved;
            if (position < sizeof(bytes)) {
                bytes[position] = static_cast<std::uint8_t>('\n') ^ static_cast<std::uint8_t>(key);
                key = (key << 1) | (key >> 31);
                ++position;
            }
            for (std::size_t i = 0; line[i] != 0 && position < sizeof(bytes); ++i) {
                bytes[position] = static_cast<std::uint8_t>(line[i]) ^ static_cast<std::uint8_t>(key);
                key = (key << 1) | (key >> 31);
                ++position;
            }
        }
    }
    if (std::FILE* file = std::fopen(host, "wb")) {
        std::fwrite(bytes, 1, sizeof(bytes), file);
        std::fclose(file);
        nand_note("Setting", host, 0);
    }
}

void nand_ensure_data_dir() {
    char host[192];
    if (!nand_host_path(kNandDataDir, host, sizeof(host))) {
        return;
    }
    nand_mkdir_one(kNandRoot);
    nand_mkdir_parents(host);
    nand_mkdir_one(host);
}

int nand_kind(const char* host);

int nand_rename(const char* from, const char* to) {
#if defined(__vita__)
    return sceIoRename(from, to) >= 0 ? 0 : -64;
#else
    return std::rename(from, to) == 0 ? 0 : -64;
#endif
}

// NANDMove's second argument is the destination directory. The source file's
// own name is appended, so /tmp/banner.bin moved onto the title data dir
// becomes <data>/banner.bin.
int nand_move_path(const char* src, const char* dst_dir) {
    char src_host[192];
    char dir_host[192];
    char dst_host[220];
    if (!nand_host_path(src, src_host, sizeof(src_host)) || !nand_host_path(dst_dir, dir_host, sizeof(dir_host))) {
        return -8;
    }
    if (nand_kind(src_host) == 0 || nand_kind(dir_host) != 2) {
        return -12;
    }
    const char* slash = std::strrchr(src_host, '/');
    const char* name = slash != nullptr ? slash + 1 : src_host;
    if (name[0] == 0 || std::snprintf(dst_host, sizeof(dst_host), "%s/%s", dir_host, name) <= 0) {
        return -8;
    }
    if (nand_kind(dst_host) != 0) {
        return -6;
    }
    return nand_rename(src_host, dst_host);
}

int nand_kind(const char* host) {
#if defined(__vita__)
    SceIoStat st{};
    if (sceIoGetstat(host, &st) < 0) {
        return 0;
    }
    if (SCE_S_ISDIR(st.st_mode)) {
        return 2;
    }
    if (SCE_S_ISREG(st.st_mode)) {
        return 1;
    }
    return 0;
#else
    struct stat st{};
    if (stat(host, &st) != 0) {
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        return 2;
    }
    if (S_ISREG(st.st_mode)) {
        return 1;
    }
    return 0;
#endif
}

void nand_publish(std::uint32_t info, int fd) {
    if (info == 0 || fd <= 0) {
        return;
    }
    Memory::Write32(info, static_cast<std::uint32_t>(fd));
    Memory::Write8(info + 0x8au, kNandFlagOpen);
}

int nand_open_path(const char* wii, int mode) {
    if (is_sysconf_path(wii)) {
        build_sysconf();
        g_nand_off = 0;
        g_nand_slot[0].live = true;
        g_nand_slot[0].sysconf = true;
        g_nand_slot[0].file = nullptr;
        return 1;
    }
    char host[192];
    if (!nand_host_path(wii, host, sizeof(host))) {
        return -8;
    }
    if (std::strcmp(wii, "/title/00000001/00000002/data/setting.txt") == 0) {
        nand_ensure_setting_txt();
    }
    const bool write = mode >= 2;
    // An all-zero rksys.dat is NOT a broken save to be deleted: NandMgr::CreateRKSYS deliberately
    // lays the file down as 0x2BC000 zero bytes (280 writes of a 10240-byte zero buffer) before
    // anything populates it. Deleting it on the next read put the game in a permanent
    // create-and-rediscover-missing loop.
    //
    // NANDOpen never creates, exactly as on the console. CreateRKSYS depends on that: it calls
    // Open(mode 2) first and only calls NandUtils::Create (perm 53) when that open fails. Creating
    // the file here made that open succeed, so the game's own create path never ran.
    if (nand_kind(host) != 1) {
        return -12;
    }
    int slot = -1;
    for (int i = 1; i < kNandSlots; ++i) {
        if (!g_nand_slot[i].live) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -10;
    }
    std::FILE* file = std::fopen(host, write ? "r+b" : "rb");
    if (file == nullptr) {
        return -12;
    }
    g_nand_slot[slot].live = true;
    g_nand_slot[slot].sysconf = false;
    g_nand_slot[slot].file = file;
    return slot + 1;
}

void nand_close_fd(int fd) {
    if (fd <= 0 || fd > kNandSlots) {
        return;
    }
    NandSlot& slot = g_nand_slot[fd - 1];
    if (slot.file != nullptr) {
        std::fclose(slot.file);
    }
    slot = {};
}

int nand_fd_from(std::uint32_t info) {
    if (info == 0) {
        return 0;
    }
    return static_cast<int>(Memory::Read32(info));
}

void guest_pull(std::uint8_t* dst, std::uint32_t addr, std::uint32_t bytes) {
    while (bytes > 0) {
        const std::uint32_t into = addr & (mkwv::runtime::kPageSize - 1u);
        std::uint32_t chunk = mkwv::runtime::kPageSize - into;
        if (chunk > bytes) {
            chunk = bytes;
        }
        const std::uint8_t* host = Memory::GetPointer(addr, chunk);
        if (host != nullptr) {
            std::memcpy(dst, host, chunk);
        } else {
            for (std::uint32_t i = 0; i < chunk; ++i) {
                dst[i] = Memory::Read8(addr + i);
            }
        }
        addr += chunk;
        dst += chunk;
        bytes -= chunk;
    }
}

std::int32_t nand_read_sysconf(std::uint32_t buffer, std::uint32_t length);
std::int32_t nand_seek_sysconf(std::int32_t offset, std::int32_t whence);

std::int32_t nand_read_fd(int fd, std::uint32_t buffer, std::uint32_t length) {
    if (fd <= 0 || fd > kNandSlots || !g_nand_slot[fd - 1].live) {
        return -8;
    }
    if (g_nand_slot[fd - 1].sysconf) {
        return nand_read_sysconf(buffer, length);
    }
    std::FILE* file = g_nand_slot[fd - 1].file;
    if (file == nullptr || buffer == 0 || length == 0) {
        return 0;
    }
    std::uint8_t chunk[4096];
    std::uint32_t got = 0;
    while (got < length) {
        std::uint32_t n = length - got;
        if (n > sizeof(chunk)) {
            n = static_cast<std::uint32_t>(sizeof(chunk));
        }
        const std::size_t read = std::fread(chunk, 1, n, file);
        if (read == 0) {
            break;
        }
        guest_copy(buffer + got, chunk, static_cast<std::uint32_t>(read));
        got += static_cast<std::uint32_t>(read);
        if (read < n) {
            break;
        }
    }
    return static_cast<std::int32_t>(got);
}

std::int32_t nand_write_fd(int fd, std::uint32_t buffer, std::uint32_t length) {
    if (fd <= 0 || fd > kNandSlots || !g_nand_slot[fd - 1].live) {
        return -8;
    }
    if (g_nand_slot[fd - 1].sysconf) {
        build_sysconf();
        if (buffer == 0 || length == 0 || g_nand_off >= kSysconfSize) {
            return 0;
        }
        std::uint32_t n = length;
        if (g_nand_off + n > kSysconfSize) {
            n = kSysconfSize - g_nand_off;
        }
        guest_pull(g_sysconf + g_nand_off, buffer, n);
        g_nand_off += n;
        return static_cast<std::int32_t>(n);
    }
    std::FILE* file = g_nand_slot[fd - 1].file;
    if (file == nullptr || buffer == 0 || length == 0) {
        return 0;
    }
    std::uint8_t chunk[4096];
    std::uint32_t put = 0;
    while (put < length) {
        std::uint32_t n = length - put;
        if (n > sizeof(chunk)) {
            n = static_cast<std::uint32_t>(sizeof(chunk));
        }
        guest_pull(chunk, buffer + put, n);
        const std::size_t wrote = std::fwrite(chunk, 1, n, file);
        put += static_cast<std::uint32_t>(wrote);
        if (wrote < n) {
            break;
        }
    }
    std::fflush(file);
    return static_cast<std::int32_t>(put);
}

std::int32_t nand_seek_fd(int fd, std::int32_t offset, std::int32_t whence) {
    if (fd <= 0 || fd > kNandSlots || !g_nand_slot[fd - 1].live) {
        return -8;
    }
    if (g_nand_slot[fd - 1].sysconf) {
        return nand_seek_sysconf(offset, whence);
    }
    std::FILE* file = g_nand_slot[fd - 1].file;
    if (file == nullptr) {
        return -8;
    }
    const int origin = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
    if (std::fseek(file, offset, origin) != 0) {
        return -64;
    }
    return static_cast<std::int32_t>(std::ftell(file));
}

std::uint32_t nand_length_fd(int fd) {
    if (fd <= 0 || fd > kNandSlots || !g_nand_slot[fd - 1].live) {
        return 0;
    }
    if (g_nand_slot[fd - 1].sysconf) {
        return sysconf_length();
    }
    std::FILE* file = g_nand_slot[fd - 1].file;
    if (file == nullptr) {
        return 0;
    }
    const long pos = std::ftell(file);
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    if (pos >= 0) {
        std::fseek(file, pos, SEEK_SET);
    }
    return size > 0 ? static_cast<std::uint32_t>(size) : 0u;
}

void nand_open_async(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char path[96];
    guest_string(ctx->gpr[3], path, sizeof(path));
    nand_absolutize(path, sizeof(path));
    const int mode = static_cast<int>(ctx->gpr[5]);
    const int fd = nand_open_path(path, mode);
    nand_note("Open", path, fd > 0 ? 0 : fd);
    if (fd > 0) {
        nand_publish(ctx->gpr[4], fd);
        nand_complete(ctx, 0, ctx->gpr[6], ctx->gpr[7]);
        return;
    }
    nand_complete(ctx, fd, ctx->gpr[6], ctx->gpr[7]);
}

std::int32_t nand_read_sysconf(std::uint32_t buffer, std::uint32_t length) {
    build_sysconf();
    if (buffer == 0 || length == 0) {
        return 0;
    }
    if (g_nand_off >= kSysconfSize) {
        return 0;
    }
    std::uint32_t remain = kSysconfSize - g_nand_off;
    if (length > remain) {
        length = remain;
    }
    guest_copy(buffer, g_sysconf + g_nand_off, length);
    g_nand_off += length;
    return static_cast<std::int32_t>(length);
}

std::int32_t nand_seek_sysconf(std::int32_t offset, std::int32_t whence) {
    build_sysconf();
    std::int64_t pos = static_cast<std::int64_t>(g_nand_off);
    if (whence == 0) {
        pos = offset;
    } else if (whence == 1) {
        pos += offset;
    } else if (whence == 2) {
        pos = static_cast<std::int64_t>(kSysconfSize) + offset;
    }
    if (pos < 0) {
        pos = 0;
    }
    if (pos > static_cast<std::int64_t>(kSysconfSize)) {
        pos = kSysconfSize;
    }
    g_nand_off = static_cast<std::uint32_t>(pos);
    return static_cast<std::int32_t>(g_nand_off);
}

void seed_ctor_abi(CpuContext* cpu) {
    cpu->gpr[1] = mkwv::runtime::kGuestStack;
    cpu->gpr[2] = mkwv::runtime::kSda2Base;
    cpu->gpr[13] = mkwv::runtime::kSdaBase;
    cpu->lr = 0;
}

unsigned run_ctor_table(CpuContext* cpu, std::uint32_t start, std::uint32_t end, unsigned* skipped) {
    unsigned ran = 0;
    unsigned skip = 0;
    for (std::uint32_t addr = start; addr < end; addr += 4u) {
        const std::uint32_t fn = Memory::Read32(addr);
        if (fn == 0 || fn == 0xFFFFFFFFu) {
            continue;
        }
        seed_ctor_abi(cpu);
        if (mkwv::runtime::invoke_guest(fn, cpu)) {
            ++ran;
        } else {
            ++skip;
        }
    }
    if (skipped != nullptr) {
        *skipped = skip;
    }
    return ran;
}

bool is_code_addr(std::uint32_t addr) {
    return addr >= 0x80000000u && addr < 0x81800000u;
}

std::uint32_t ripper_slot(const CpuContext* ctx) {
    const std::uint32_t sda = (ctx != nullptr && ctx->gpr[13] >= 0x80000000u && ctx->gpr[13] < 0x81800000u)
                                  ? ctx->gpr[13]
                                  : mkwv::runtime::kSdaBase;
    return sda - kRipperSdaOff;
}

void dump_ripper(const char* tag, CpuContext* ctx) {
    const std::uint32_t sda = ctx != nullptr ? ctx->gpr[13] : 0;
    const std::uint32_t slot = ripper_slot(ctx);
    const std::uint32_t ripper = Memory::Read32(slot);
    const std::uint32_t def_heap = Memory::Read32(mkwv::runtime::kSdaBase - kDefaultHeapSdaOff);
    const std::uint32_t tt = ripper != 0 ? Memory::Read32(ripper + kRipperTaskOff) : 0;
    const std::uint32_t jobs = tt != 0 ? Memory::Read32(tt + kTaskJobCountOff) : 0;
    const std::uint32_t arr = tt != 0 ? Memory::Read32(tt + kTaskJobArrayOff) : 0;
    const std::uint32_t lang = ripper != 0 ? Memory::Read32(ripper + kRipperLangOff) : 0;
    const std::uint32_t pathp = Memory::Read32(kStrapPathTable + ((lang & 7u) << 2));
    char path[80];
    guest_string(pathp, path, sizeof(path));
    std::printf(
        "mkwv: %s r13=0x%08X slot=0x%08X ripper=0x%08X tt=0x%08X jobs=%u arr=0x%08X defheap=0x%08X lang=%u path=%s\n",
        tag, sda, slot, ripper, tt, jobs, arr, def_heap, lang, path[0] != 0 ? path : "(null)");
}

void seed_sda(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    ctx->gpr[2] = mkwv::runtime::kSda2Base;
    ctx->gpr[13] = mkwv::runtime::kSdaBase;
    if (Memory::Read32(kDefaultHeapPtrAddr) == 0 && g_heap_seen && g_game_heap != 0) {
        Memory::Write32(kDefaultHeapPtrAddr, g_game_heap);
        std::printf("mkwv: default heap -> 0x%08X\n", g_game_heap);
    }
}

void dump_heap(const char* tag) {
    const std::uint32_t heap = g_game_heap;
    const std::uint32_t vt = Memory::Read32(heap);
    const std::uint32_t handle = Memory::Read32(heap + 16u);
    const std::uint32_t alloc_vt = vt != 0 ? Memory::Read32(vt + 20u) : 0;
    const std::uint32_t magic = handle != 0 ? Memory::Read32(handle) : 0;
    const std::uint32_t start = handle != 0 ? Memory::Read32(handle + 24u) : 0;
    const std::uint32_t end = handle != 0 ? Memory::Read32(handle + 28u) : 0;
    const std::uint32_t free_hd = handle != 0 ? Memory::Read32(handle + 60u) : 0;
    const std::uint32_t free_sz = free_hd != 0 ? Memory::Read32(free_hd + 4u) : 0;
    const std::uint32_t free_next = free_hd != 0 ? Memory::Read32(free_hd + 12u) : 0;
    const std::uint32_t free_sig = free_hd != 0 ? Memory::Read16(free_hd) : 0;
    const std::uint32_t flags = Memory::Read16(heap + 28u);
    const std::uint32_t thr = Memory::Read32(kOSRunningContextAddr);
    const std::uint32_t thr_heap = thr != 0 ? Memory::Read32(thr + 60u) : 0;
    const std::uint32_t chk = Memory::Read32(mkwv::runtime::kSdaBase - 23896u);
    std::printf(
        "mkwv: %s heap=0x%08X vt=0x%08X alloc=0x%08X handle=0x%08X magic=0x%08X start=0x%08X end=0x%08X free=0x%08X "
        "fsz=%u next=0x%08X sig=0x%X flags=0x%X th=0x%08X thheap=0x%08X chk=0x%08X\n",
        tag, heap, vt, alloc_vt, handle, magic, start, end, free_hd, free_sz, free_next, free_sig, flags, thr, thr_heap,
        chk);
}

std::uint32_t align_up(std::uint32_t value, std::uint32_t align) {
    if (align <= 1u) {
        return value;
    }
    return (value + (align - 1u)) & ~(align - 1u);
}

void write_free_block(std::uint32_t block, std::uint32_t payload) {
    Memory::Write16(block, static_cast<std::uint16_t>(kFrSig));
    Memory::Write16(block + 2u, 0);
    Memory::Write32(block + 4u, payload);
    Memory::Write32(block + 8u, 0);
    Memory::Write32(block + 12u, 0);
}

void insert_free_block(std::uint32_t handle, std::uint32_t block) {
    // MEMAdjustExpHeap inspects the list tail; it must be the highest address.
    std::uint32_t prev = 0;
    std::uint32_t next = Memory::Read32(handle + 60u);
    while (next != 0 && next < block) {
        prev = next;
        next = Memory::Read32(next + 12u);
    }
    Memory::Write32(block + 8u, prev);
    Memory::Write32(block + 12u, next);
    if (prev != 0) Memory::Write32(prev + 12u, block);
    else Memory::Write32(handle + 60u, block);
    if (next != 0) Memory::Write32(next + 8u, block);
    else Memory::Write32(handle + 64u, block);
}

void unlink_free_block(std::uint32_t handle, std::uint32_t block) {
    const std::uint32_t prev = Memory::Read32(block + 8u);
    const std::uint32_t next = Memory::Read32(block + 12u);
    if (prev != 0) {
        Memory::Write32(prev + 12u, next);
    } else {
        Memory::Write32(handle + 60u, next);
    }
    if (next != 0) {
        Memory::Write32(next + 8u, prev);
    } else {
        Memory::Write32(handle + 64u, prev);
    }
    Memory::Write32(block + 8u, 0);
    Memory::Write32(block + 12u, 0);
}

void insert_used_block(std::uint32_t handle, std::uint32_t block) {
    const std::uint32_t tail = Memory::Read32(handle + 72u);
    Memory::Write32(block + 8u, tail);
    Memory::Write32(block + 12u, 0);
    if (tail != 0) Memory::Write32(tail + 12u, block);
    else Memory::Write32(handle + 68u, block);
    Memory::Write32(handle + 72u, block);
}

void unlink_used_block(std::uint32_t handle, std::uint32_t block) {
    const std::uint32_t prev = Memory::Read32(block + 8u);
    const std::uint32_t next = Memory::Read32(block + 12u);
    if (prev != 0) Memory::Write32(prev + 12u, next);
    else Memory::Write32(handle + 68u, next);
    if (next != 0) Memory::Write32(next + 8u, prev);
    else Memory::Write32(handle + 72u, prev);
}

std::uint32_t merge_free_block(std::uint32_t handle, std::uint32_t block) {
    bool merged = true;
    while (merged) {
        merged = false;
        const std::uint32_t block_end = block + 16u + Memory::Read32(block + 4u);
        for (std::uint32_t other = Memory::Read32(handle + 60u); other != 0; other = Memory::Read32(other + 12u)) {
            if (other == block || Memory::Read16(other) != kFrSig) continue;
            const std::uint32_t other_end = other + 16u + Memory::Read32(other + 4u);
            if (other_end == block) {
                unlink_free_block(handle, other);
                Memory::Write32(other + 4u, Memory::Read32(other + 4u) + 16u + Memory::Read32(block + 4u));
                block = other;
                merged = true;
                break;
            }
            if (block_end == other) {
                unlink_free_block(handle, other);
                Memory::Write32(block + 4u, Memory::Read32(block + 4u) + 16u + Memory::Read32(other + 4u));
                merged = true;
                break;
            }
        }
    }
    return block;
}

// Keep this deliberately structural.  The SDK owns the allocation policy on
// Vita, but a broken list link or header lets us identify the operation that
// damaged a heap before its later allocation failures turn into null calls.
bool verify_exp_heap(std::uint32_t handle, const char*& reason, std::uint32_t& bad,
                     std::uint32_t& previous, std::uint32_t& next) {
    reason = "ok";
    bad = previous = next = 0;
    const std::uint32_t start = Memory::Read32(handle + 24u);
    const std::uint32_t end = Memory::Read32(handle + 28u);
    if (start >= end || !Memory::Contains(start, 1) || !Memory::Contains(end - 1u, 1)) {
        reason = "range";
        bad = start;
        return false;
    }

    auto verify_list = [&](std::uint32_t head_offset, std::uint32_t tail_offset,
                           std::uint16_t signature, bool sorted) {
        std::uint32_t prev = 0;
        std::uint32_t block = Memory::Read32(handle + head_offset);
        for (unsigned count = 0; block != 0; ++count) {
            if (count == 4096u) {
                reason = "loop";
                bad = block;
                previous = prev;
                return false;
            }
            // Check the header's first and last byte separately. Contains(block, 16) goes
            // through CompactMemory::lookup, which refuses any span crossing its 1 MB page,
            // so a perfectly valid header straddling 0x90200000 was reported as corruption
            // (bad=901FFFF8, 16 times a boot) while every allocation around it succeeded.
            if (block < start || block > end - 16u || !Memory::Contains(block, 1u) ||
                !Memory::Contains(block + 15u, 1u)) {
                reason = "block-range";
                bad = block;
                previous = prev;
                return false;
            }
            const std::uint32_t payload = Memory::Read32(block + 4u);
            const std::uint32_t listed_prev = Memory::Read32(block + 8u);
            const std::uint32_t listed_next = Memory::Read32(block + 12u);
            if (Memory::Read16(block) != signature) {
                reason = "signature";
                bad = block;
                previous = prev;
                next = listed_next;
                return false;
            }
            if (payload > end - block - 16u) {
                reason = "payload-range";
                bad = block;
                previous = payload;
                return false;
            }
            if (listed_prev != prev) {
                reason = "previous-link";
                bad = block;
                previous = listed_prev;
                next = prev;
                return false;
            }
            if (sorted && listed_next != 0 && listed_next <= block) {
                reason = "free-order";
                bad = block;
                previous = prev;
                next = listed_next;
                return false;
            }
            prev = block;
            block = listed_next;
        }
        if (Memory::Read32(handle + tail_offset) != prev) {
            reason = "tail-link";
            bad = Memory::Read32(handle + tail_offset);
            previous = prev;
            return false;
        }
        return true;
    };

    if (!verify_list(60u, 64u, kFrSig, true)) return false;
    return verify_list(68u, 72u, kUdSig, false);
}

// Full walks of both block lists before and after every allocation and free: a debugging aid
// from the allocator bring-up that turned into the game's single hottest function (EGG::ExpHeap
// alloc/free 5-9% of all guest samples in round 7's profile, thousands of blocks per walk).
// Opt-in now: marker `heap_check`.
bool g_exp_heap_checks = false;

void note_exp_heap_integrity(const char* phase, CpuContext* ctx, std::uint32_t egg,
                             std::uint32_t handle, std::uint32_t size, std::int32_t align,
                             std::uint32_t result) {
    if (!g_exp_heap_checks) return;
    const char* reason = nullptr;
    std::uint32_t bad = 0, previous = 0, next = 0;
    if (verify_exp_heap(handle, reason, bad, previous, next)) return;
    static unsigned reports = 0;
    if (reports++ >= 16u) return;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "mkwv: HEAP_CORRUPT phase=%s egg=%08X handle=%08X size=%u align=%d result=%08X lr=%08X "
                  "reason=%s bad=%08X prev=%08X next=%08X head=%08X tail=%08X used=%08X usedtail=%08X\n",
                  phase, egg, handle, size, align, result, ctx != nullptr ? ctx->lr : 0u, reason, bad, previous,
                  next, Memory::Read32(handle + 60u), Memory::Read32(handle + 64u),
                  Memory::Read32(handle + 68u), Memory::Read32(handle + 72u));
    mkwv::runtime::watchdog_log_line(line);
}

std::uint32_t alloc_exp_heap(std::uint32_t handle, std::uint32_t size, std::int32_t align) {
    if (handle == 0 || Memory::Read32(handle) != 0x45585048u) {
        return 0;
    }
    // MEM::AllocFromExpHeapEx substitutes one byte only for a literal zero,
    // then rounds with 32-bit PPC arithmetic.  In particular, -1 wraps to
    // zero here; several NW4R setup paths rely on that exact behavior.
    if (size == 0) size = 1;
    size = (size + 3u) & ~3u;
    const bool from_tail = align < 0;
    const std::uint32_t alignment = from_tail ? 0u - static_cast<std::uint32_t>(align)
                                              : static_cast<std::uint32_t>(align);
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0) return 0;
    const std::uint32_t heap_start = Memory::Read32(handle + 24u);
    const std::uint32_t heap_end = Memory::Read32(handle + 28u);
    const std::uint32_t span = heap_end > heap_start ? heap_end - heap_start : 0;
    if (size > 64u * 1024u * 1024u || (span != 0 && size > span)) {
        return 0;
    }
    std::uint32_t block = Memory::Read32(handle + (from_tail ? 64u : 60u));
    std::uint32_t chosen = 0, aligned = 0, best_size = UINT32_MAX;
    const bool best_fit = (Memory::Read16(handle + 78u) & 1u) != 0;
    unsigned guard = 0;
    while (block != 0 && guard++ < 4096u) {
        const std::uint32_t next = Memory::Read32(block + (from_tail ? 8u : 12u));
        if (Memory::Read16(block) != kFrSig) {
            block = next;
            continue;
        }
        const std::uint32_t blksize = Memory::Read32(block + 4u);
        const std::uint32_t data = block + 16u;
        const std::uint32_t candidate = from_tail ? (data + blksize - size) & ~(alignment - 1u)
                                                  : align_up(data, alignment);
        if (blksize >= size && candidate >= data && candidate - data <= blksize - size && blksize < best_size) {
            chosen = block;
            aligned = candidate;
            best_size = blksize;
            if (!best_fit || blksize == size) break;
        }
        block = next;
    }
    if (chosen == 0) return 0;

    const std::uint32_t used = aligned - 16u;
    const std::uint32_t original_end = chosen + 16u + best_size;
    const std::uint32_t original_start = chosen - ((Memory::Read16(chosen + 2u) >> 8u) & 0x7fu);
    std::uint32_t allocation_start = original_start;
    std::uint32_t allocation_end = aligned + size;
    unlink_free_block(handle, chosen);
    if (used - original_start >= 20u) {
        write_free_block(original_start, used - original_start - 16u);
        insert_free_block(handle, original_start);
        allocation_start = used;
    }
    // The SDK splits immediately after the four-byte-rounded payload, without
    // rounding the remainder header up to 16 bytes.
    if (original_end - allocation_end >= 20u) {
        write_free_block(allocation_end, original_end - allocation_end - 16u);
        insert_free_block(handle, allocation_end);
    } else {
        allocation_end = original_end;
    }
    if ((Memory::Read32(handle + 56u) & 1u) != 0)
        guest_memset(allocation_start, allocation_end - allocation_start, 0);
    const std::uint16_t attributes = (from_tail ? 0x8000u : 0u) |
        ((used - allocation_start) << 8u) | (Memory::Read16(handle + 76u) & 0xffu);
    Memory::Write16(used, static_cast<std::uint16_t>(kUdSig));
    Memory::Write16(used + 2u, attributes);
    Memory::Write32(used + 4u, allocation_end - aligned);
    insert_used_block(handle, used);
    return aligned;
}

bool read_guest_bytes(std::uint32_t addr, std::uint8_t* dest, std::uint32_t length) {
    std::uint32_t done = 0;
    while (done < length) {
        const std::uint32_t current = addr + done;
        const std::uint32_t into = current & (mkwv::runtime::kPageSize - 1u);
        std::uint32_t chunk = mkwv::runtime::kPageSize - into;
        if (chunk > length - done) {
            chunk = length - done;
        }
        const std::uint8_t* host = Memory::GetPointer(current, chunk);
        if (host != nullptr) {
            std::memcpy(dest + done, host, chunk);
        } else {
            for (std::uint32_t i = 0; i < chunk; ++i) {
                dest[done + i] = Memory::Read8(current + i);
            }
        }
        done += chunk;
    }
    return true;
}

bool copy_guest_bytes(std::uint32_t addr, const std::uint8_t* src, std::uint32_t length) {
    guest_copy(addr, src, length);
    return true;
}

std::uint32_t egg_alloc(std::uint32_t egg, std::uint32_t size, std::int32_t align) {
    if (egg == 0) {
        return 0;
    }
    std::uint32_t handle = Memory::Read32(egg + 16u);
    if (handle == 0 || Memory::Read32(handle) != 0x45585048u) {
        if (Memory::Read32(egg) == 0x45585048u) {
            handle = egg;
        } else {
            return 0;
        }
    }
    return alloc_exp_heap(handle, size, align);
}

std::uint32_t call_guest(CpuContext* ctx, std::uint32_t addr, std::uint32_t r3, std::uint32_t r4, std::uint32_t r5,
                         std::uint32_t r6) {
    CpuContext saved = *ctx;
    seed_sda(ctx);
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    const bool ok = mkwv::runtime::invoke_guest(addr, ctx);
    const std::uint32_t out = ctx->gpr[3];
    *ctx = saved;
    seed_sda(ctx);
    if (!ok) {
        std::printf("mkwv: invoke 0x%08X fail r3=0x%08X\n", addr, out);
    }
    return out;
}

std::uint32_t make_ripper(CpuContext* ctx) {
    dump_heap("pre-ripper");
    std::uint32_t ptr = call_guest(ctx, kEggHeapAlloc, kRipperBytes, 4, g_game_heap, 0);
    std::printf("mkwv: Heap::Alloc 4352 heap=0x%08X -> 0x%08X\n", g_game_heap, ptr);
    if (ptr == 0) {
        ptr = call_guest(ctx, kExpHeapAlloc, g_game_heap, kRipperBytes, 4, 0);
        std::printf("mkwv: ExpHeap::Alloc 4352 -> 0x%08X\n", ptr);
    }
    if (ptr == 0) {
        dump_heap("alloc-fail");
        return 0;
    }
    const std::uint32_t made = call_guest(ctx, kRipperCtor, ptr, g_game_heap, 0, 0);
    const std::uint32_t ripper = made != 0 ? made : ptr;
    Memory::Write32(kRipperPtrAddr, ripper);
    std::printf("mkwv: Ripper ctor this=0x%08X heap=0x%08X -> 0x%08X\n", ptr, g_game_heap, ripper);
    dump_heap("post-ripper");
    return ripper;
}

void ensure_task_thread(CpuContext* ctx) {
    if (g_ensure_task || ctx == nullptr || g_job_depth != 0) {
        return;
    }
    g_ensure_task = true;
    seed_sda(ctx);
    const std::uint32_t slot = kRipperPtrAddr;
    std::uint32_t ripper = Memory::Read32(slot);
    if (ripper == 0) {
        const std::uint32_t got = call_guest(ctx, kGetRipper, g_game_heap, 0, 0, 0);
        ripper = got != 0 ? got : Memory::Read32(slot);
        std::printf("mkwv: GetRipper -> 0x%08X heap=0x%08X\n", ripper, g_game_heap);
    }
    if (ripper == 0) {
        ripper = make_ripper(ctx);
    }
    if (ripper == 0) {
        return;
    }
    const std::uint32_t tt = Memory::Read32(ripper + kRipperTaskOff);
    const std::uint32_t jobs = tt != 0 ? Memory::Read32(tt + kTaskJobCountOff) : 0;
    if (tt != 0 && jobs != 0) {
        g_task_thread = tt;
        return;
    }
    CpuContext saved = *ctx;
    seed_sda(ctx);
    std::uint32_t heap = Memory::Read32(ripper + kRipperHeapOff);
    if (heap == 0) {
        heap = g_game_heap;
    }
    ctx->gpr[3] = 5;
    ctx->gpr[4] = 24;
    ctx->gpr[5] = 10240;
    ctx->gpr[6] = heap;
    const bool ok = mkwv::runtime::invoke_guest(kTaskThreadCreate, ctx);
    const std::uint32_t created = ctx->gpr[3];
    *ctx = saved;
    seed_sda(ctx);
    Memory::Write32(ripper + kRipperTaskOff, created);
    if (created != 0) {
        g_task_thread = created;
    }
    std::printf("mkwv: TaskThread create %s -> 0x%08X heap=0x%08X\n", ok ? "ok" : "fail", created, heap);
}

// EGG::TaskThread job pipeline.
//
// The guest worker thread (TaskThread::run, 0x80242D7C) never runs under this runtime, so jobs are
// executed where they are posted (OSSendMessage). Running them inline from the send used to drop any
// job queued *from inside* another job's callback, and a dropped job leaves its slot's callback word
// set forever - StrapScene::calc gates on func_80242C98 (scan of the slot array at tt+76, count at
// tt+80, stride kJobSize), so one stuck slot pins the scene on screen for good. Jobs are therefore
// parked in a small FIFO and drained iteratively: a nested post lands in the same FIFO and is picked
// up by the outer loop instead of being discarded. Retirement mirrors upstream's TaskThread_run HLE:
// callback, onDone, done-queue token, then clear the slot.
constexpr std::uint32_t kTaskMessageQueueOff = 0x0Cu;
constexpr std::uint32_t kTaskCurrentJobOff = 0x48u;
constexpr std::uint32_t kTaskDoneQueueOff = 0x54u;
constexpr std::uint32_t kJobCallbackOff = 0x00u;
constexpr std::uint32_t kJobArgOff = 0x04u;
constexpr std::uint32_t kJobTokenOff = 0x08u;
constexpr std::uint32_t kJobUnknown3Off = 0x0Cu;
constexpr std::uint32_t kJobUnknown4Off = 0x10u;
constexpr std::uint32_t kJobOnDoneOff = 0x14u;

struct PendingJob {
    std::uint32_t queue = 0;
    std::uint32_t job = 0;
};

PendingJob g_pending[32];
unsigned g_npending = 0;
unsigned g_jobs_run = 0;
unsigned g_jobs_dropped = 0;

bool push_guest_queue(std::uint32_t q, std::uint32_t msg) {
    if (q == 0) {
        return false;
    }
    const std::uint32_t array = Memory::Read32(q + 0x10u);
    const std::uint32_t count = Memory::Read32(q + 0x14u);
    const std::uint32_t first = Memory::Read32(q + 0x18u);
    const std::uint32_t used = Memory::Read32(q + 0x1Cu);
    if (count == 0 || count > 256u || array == 0 || used >= count) {
        return false;
    }
    Memory::Write32(array + ((first + used) % count) * 4u, msg);
    Memory::Write32(q + 0x1Cu, used + 1u);
    return true;
}

void queue_egg_job(std::uint32_t queue, std::uint32_t job) {
    if (job == 0) {
        return;
    }
    if (g_npending >= sizeof(g_pending) / sizeof(g_pending[0])) {
        ++g_jobs_dropped;
        std::printf("mkwv: task job FIFO full, dropping job=0x%08X\n", job);
        return;
    }
    g_pending[g_npending].queue = queue;
    g_pending[g_npending].job = job;
    ++g_npending;
}

void run_one_egg_job(const PendingJob& pj, CpuContext* ctx) {
    const std::uint32_t job = pj.job;
    const std::uint32_t tt = pj.queue >= kTaskMessageQueueOff ? pj.queue - kTaskMessageQueueOff : 0u;
    const std::uint32_t callback = Memory::Read32(job + kJobCallbackOff);
    const std::uint32_t arg = Memory::Read32(job + kJobArgOff);
    const std::uint32_t token = Memory::Read32(job + kJobTokenOff);
    const std::uint32_t on_done = Memory::Read32(job + kJobOnDoneOff);
    if (g_jobs_run < 16u) {
        std::printf("mkwv: task job cb=0x%08X arg=0x%08X tt=0x%08X done=0x%08X\n", callback, arg, tt, on_done);
        std::fflush(stdout);
    }
    ++g_jobs_run;
    if (tt != 0) {
        Memory::Write32(tt + kTaskCurrentJobOff, job);
    }
    if (is_code_addr(callback)) {
        CpuContext job_cpu = *ctx;
        seed_sda(&job_cpu);
        job_cpu.gqr[2] = 0x00040004u;
        job_cpu.gqr[3] = 0x00050005u;
        job_cpu.gqr[4] = 0x00060006u;
        job_cpu.gqr[5] = 0x00070007u;
        job_cpu.gpr[3] = arg;
        InvokeIndirectCpu(callback, &job_cpu);
    }
    if (is_code_addr(on_done)) {
        CpuContext done_cpu = *ctx;
        seed_sda(&done_cpu);
        done_cpu.gqr[2] = 0x00040004u;
        done_cpu.gqr[3] = 0x00050005u;
        done_cpu.gqr[4] = 0x00060006u;
        done_cpu.gqr[5] = 0x00070007u;
        done_cpu.gpr[3] = Memory::Read32(job + kJobArgOff);
        InvokeIndirectCpu(on_done, &done_cpu);
    }
    if (tt != 0) {
        push_guest_queue(Memory::Read32(tt + kTaskDoneQueueOff), token);
    }
    // Always clear, even when the callback was not runnable: a slot left set reads as "busy" forever.
    Memory::Write32(job + kJobCallbackOff, 0);
    Memory::Write32(job + kJobUnknown3Off, 0);
    Memory::Write32(job + kJobUnknown4Off, 0);
    Memory::Write32(job + kJobOnDoneOff, 0);
    if (tt != 0) {
        Memory::Write32(tt + kTaskCurrentJobOff, 0);
    }
}

void drain_egg_jobs(CpuContext* ctx) {
    if (ctx == nullptr || g_job_depth != 0) {
        return;
    }
    ++g_job_depth;
    unsigned guard = 0;
    while (g_npending != 0 && guard < 256u) {
        const PendingJob pj = g_pending[0];
        for (unsigned i = 1; i < g_npending; ++i) {
            g_pending[i - 1u] = g_pending[i];
        }
        --g_npending;
        run_one_egg_job(pj, ctx);
        ++guard;
    }
    --g_job_depth;
}

void cancel_alarm(std::uint32_t alarm) {
    unsigned o = 0;
    for (unsigned i = 0; i < g_nalarms; ++i) {
        if (g_alarms[i].alarm != alarm) {
            g_alarms[o++] = g_alarms[i];
        }
    }
    g_nalarms = o;
}

void drop_invalid_alarm(std::uint32_t alarm, std::uint32_t handler, const char* stage) {
    char line[128];
    std::snprintf(line, sizeof(line), "mkwv: dropped alarm callback stage=%s alarm=%08X handler=%08X\\n",
                  stage, alarm, handler);
    mkwv::runtime::watchdog_log_line(line);
}

void enqueue_alarm(std::uint32_t alarm, std::uint32_t handler, std::uint64_t delay, std::uint64_t period) {
    if (alarm == 0) {
        return;
    }
    if (!is_code_addr(handler)) {
        drop_invalid_alarm(alarm, handler, "enqueue");
        return;
    }
    cancel_alarm(alarm);
    if (g_nalarms >= sizeof(g_alarms) / sizeof(g_alarms[0])) {
        return;
    }
    HostAlarm& slot = g_alarms[g_nalarms++];
    slot.alarm = alarm;
    slot.handler = handler;
    slot.fire = g_timebase.load(std::memory_order_relaxed) + delay;
    slot.period = period;
    Memory::Write32(alarm + 0x00u, handler);
    Memory::Write32(alarm + 0x08u, static_cast<std::uint32_t>(slot.fire >> 32));
    Memory::Write32(alarm + 0x0Cu, static_cast<std::uint32_t>(slot.fire));
    Memory::Write32(alarm + 0x18u, static_cast<std::uint32_t>(period >> 32));
    Memory::Write32(alarm + 0x1Cu, static_cast<std::uint32_t>(period));
}

void fire_due_alarms(CpuContext* cpu, int max_to_process) {
    if (cpu == nullptr || g_alarm_busy || max_to_process <= 0 || g_nalarms <= 0) {
        return;
    }
    // Alarm handlers run in interrupt context: under the guest lock (guest_sync.hpp).
    mkwv::runtime::IrqLockScope lock;
    if (g_alarm_busy) {
        return;
    }
    g_alarm_busy = true;
    int fired = 0;
    while (fired < max_to_process && g_nalarms > 0) {
        int due = -1;
        for (unsigned i = 0; i < g_nalarms; ++i) {
            if (g_alarms[i].fire <= g_timebase.load(std::memory_order_relaxed)) {
                due = static_cast<int>(i);
                break;
            }
        }
        if (due < 0) {
            break;
        }
        const HostAlarm slot = g_alarms[static_cast<unsigned>(due)];
        for (unsigned i = static_cast<unsigned>(due) + 1u; i < g_nalarms; ++i) {
            g_alarms[i - 1u] = g_alarms[i];
        }
        --g_nalarms;
        Memory::Write32(slot.alarm + 0x00u, 0);
        if (!is_code_addr(slot.handler)) {
            drop_invalid_alarm(slot.alarm, slot.handler, "dispatch");
            ++fired;
            continue;
        }
        if (slot.period != 0) {
            enqueue_alarm(slot.alarm, slot.handler, slot.period, slot.period);
        }
        cpu->gpr[3] = slot.alarm;
        cpu->gpr[4] = Memory::Read32(kOSRunningContextAddr);
        InvokeIndirectCpu(slot.handler, cpu);
        ++fired;
    }
    g_alarm_busy = false;
}

} // namespace

namespace mkwv::runtime {

void write_os_lowmem(CompactMemory& memory) {
    reset_os_hle_state();

    constexpr std::uint32_t kMem1 = static_cast<std::uint32_t>(kMem1Size);
    constexpr std::uint32_t kMem2 = static_cast<std::uint32_t>(kMem2Size);
    constexpr std::uint32_t kMem1End = kMem1Cached + kMem1;
    constexpr std::uint32_t kMem1ArenaLo = 0x80399180u;
    constexpr std::uint32_t kMem1ArenaHi = 0x817F0520u;
    constexpr std::uint32_t kMem2End = kMem2Cached + kMem2;
    const std::uint32_t ios_lo = kMem2End - kMem2IosReserved;
    const std::uint32_t ipc_hi = ios_lo;
    const std::uint32_t ipc_lo = ipc_hi - kMem2IpcArena;
    const std::uint32_t mem2_arena_hi = kDvdFstBase;
    constexpr std::uint32_t kBusHz = 243000000u;

    memory.write32(0x80000000u, 0x524D4350u);
    memory.write32(0x80000004u, 0x30310100u);
    memory.write32(0x80000028u, kMem1);
    memory.write32(0x80000030u, kMem1ArenaLo);
    memory.write32(0x80000034u, kMem1ArenaHi);
    memory.write32(0x800000F0u, kMem1);
    memory.write32(0x800000F8u, kBusHz);
    memory.write32(0x800000FCu, kBusHz * 3u);
    memory.write32(0x80003100u, kMem1);
    memory.write32(0x80003104u, kMem1);
    memory.write32(0x80003108u, kMem1End);
    memory.write32(0x8000310Cu, kMem1ArenaLo);
    memory.write32(0x80003110u, kMem1ArenaHi);
    memory.write32(0x80003118u, kMem2);
    memory.write32(0x8000311Cu, kMem2);
    memory.write32(0x80003120u, ios_lo);
    memory.write32(0x80003124u, 0x90000800u);
    memory.write32(0x80003128u, mem2_arena_hi);
    memory.write32(0x80003130u, ipc_lo);
    memory.write32(0x80003134u, ipc_hi);
    memory.write32(0x80003138u, 2);
    memory.write32(0x80003140u, 0x00240E18u);
    memory.write32(0x80003144u, 0x00030110u);
    memory.write32(0x80003148u, ios_lo);
    memory.write32(0x8000314Cu, kMem2End);
    memory.write32(0x80003180u, 0x524D4350u);
    memory.write32(0x80003194u, 0x524D4350u);
}

void run_rel_constructors(CpuContext* cpu) {
    if (cpu == nullptr) {
        return;
    }
    unsigned skip = 0;
    const unsigned ran = run_ctor_table(cpu, 0x8088F400u, 0x8088F704u, &skip);
    std::printf("mkwv: StaticR ctors ran=%u skip=%u\n", ran, skip);
    seed_ctor_abi(cpu);
}

const char* rel_boot_debug() {
    static char line[256];
    const std::uint32_t system = Memory::Read32(0x80385FC8u);
    const std::uint32_t sm = system != 0 ? Memory::Read32(system + 84u) : 0u;
    std::snprintf(line, sizeof(line),
                  "mkwv: REL rev=static65-hprof phase=%s links=%u done=%u input_samples=%u buttons=%08X module=%08X entry=%08X creator=%08X next=%08X\n",
                  g_rel_phase.load(), g_rel_link_calls.load(), g_rel_prolog_done ? 1u : 0u, g_strap_input_samples.load(),
                  g_strap_buttons.load(), g_rel_module.load(), g_rel_module.load() != 0 ? Memory::Read32(g_rel_module.load() + 52u) : 0u, sm != 0 ? Memory::Read32(sm + 44u) : 0u,
                  sm != 0 ? Memory::Read32(sm + 20u) : 0u);
    return line;
}

} // namespace mkwv::runtime

extern "C" void __init_hardware_80006348(CpuContext*) {}
extern "C" void PPCMtmmcr0_8012e5b8(CpuContext*) {}
extern "C" void PPCMtmmcr1_8012e5c0(CpuContext*) {}
extern "C" void PPCMtpmc1_8012e5c8(CpuContext*) {}
extern "C" void PPCMtpmc2_8012e5d0(CpuContext*) {}
extern "C" void PPCMtpmc3_8012e5d8(CpuContext*) {}
extern "C" void PPCMtpmc4_8012e5e0(CpuContext*) {}
extern "C" void PPCDisableSpeculation_8012e654(CpuContext*) {}
extern "C" void PPCMthid4_8012e684(CpuContext*) {}

extern "C" void PPCMthid2_8012e638(CpuContext* ctx) {
    if (ctx != nullptr) {
        ctx->hid2 = ctx->gpr[3];
    }
}

extern "C" void ESP_InitLib_801671d0(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void ESP_CloseLib_80167224(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void SetExiInterruptMask_80167e78(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void EXIImm_80167f68(CpuContext* ctx) {
    if (ctx != nullptr) {
        const std::uint32_t buffer = ctx->gpr[4];
        const std::uint32_t length = ctx->gpr[5];
        const std::uint32_t type = ctx->gpr[6];
        if ((type == 0 || type == 2) && buffer != 0 && length > 0 && length < 0x1000u) {
            guest_memset(buffer, length, 0);
        }
    }
    set_r3(ctx, 1);
}
extern "C" void EXIDma_80168288(CpuContext* ctx) {
    set_r3(ctx, 1);
}
extern "C" void EXISync_80168380(CpuContext* ctx) {
    set_r3(ctx, 1);
}
extern "C" void EXISelect_801689d0(CpuContext* ctx) {
    set_r3(ctx, 1);
}
extern "C" void EXIDeselect_80168b00(CpuContext* ctx) {
    set_r3(ctx, 1);
}
extern "C" void EXIInit_80168fa0(CpuContext*) {}
extern "C" void EXIUnlock_80169260(CpuContext* ctx) {
    set_r3(ctx, 1);
}

extern "C" void IPCCltInit_80193478(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void IOS_Open_801938f8(CpuContext* ctx) {
    if (ctx != nullptr) {
        char path[96];
        guest_string(ctx->gpr[3], path, sizeof(path));
        static bool logged = false;
        if (!logged) {
            std::printf("mkwv: IOS_Open %s\n", path[0] != 0 ? path : "(null)");
            logged = true;
        }
    }
    set_r3(ctx, 1);
}
extern "C" void IOS_IoctlAsync_80194158(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void IOS_Ioctl_80194290(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void IOS_Close_80193ad8(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void IOS_CloseAsync_80193a18(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    nand_complete(ctx, 0, ctx->gpr[4], ctx->gpr[5]);
}
extern "C" void IOS_Read_80193c80(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void NANDCreate_8019b43c(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char path[96];
    guest_string(ctx->gpr[3], path, sizeof(path));
    nand_absolutize(path, sizeof(path));
    int result = -8;
    if (is_sysconf_path(path)) {
        result = -6;
    } else {
        char host[192];
        if (nand_host_path(path, host, sizeof(host))) {
            if (nand_kind(host) != 0) {
                result = -6;
            } else {
                nand_mkdir_one(kNandRoot);
                nand_mkdir_parents(host);
                if (std::FILE* created = std::fopen(host, "wb")) {
                    std::fclose(created);
                    result = 0;
                } else {
                    result = -64;
                }
            }
        }
    }
    nand_note("Create", path, result);
    set_r3(ctx, result >= 0 ? 0u : static_cast<std::uint32_t>(result));
}
extern "C" void NANDPrivateCreateAsync_8019b524(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::uint32_t callback = ctx->gpr[6];
    const std::uint32_t block = ctx->gpr[7];
    NANDCreate_8019b43c(ctx);
    nand_complete(ctx, static_cast<std::int32_t>(ctx->gpr[3]), callback, block);
}
extern "C" void NANDDelete_8019b59c(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char path[96];
    guest_string(ctx->gpr[3], path, sizeof(path));
    nand_absolutize(path, sizeof(path));
    int result = -8;
    char host[192];
    if (is_sysconf_path(path)) {
        result = -1;
    } else if (nand_host_path(path, host, sizeof(host))) {
        if (nand_kind(host) == 0) {
            result = -12;
        } else {
#if defined(__vita__)
            result = sceIoRemove(host) >= 0 ? 0 : -64;
#else
            result = std::remove(host) == 0 ? 0 : -64;
#endif
        }
    }
    nand_note("Delete", path, result);
    set_r3(ctx, result >= 0 ? 0u : static_cast<std::uint32_t>(result));
}
extern "C" void NANDCreateDir_8019bbe0(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char path[96];
    guest_string(ctx->gpr[3], path, sizeof(path));
    nand_absolutize(path, sizeof(path));
    int result = -8;
    char host[192];
    if (nand_host_path(path, host, sizeof(host))) {
        const int kind = nand_kind(host);
        if (kind == 2) {
            result = 0;
        } else if (kind == 1) {
            result = -6;
        } else {
            nand_mkdir_one(kNandRoot);
            nand_mkdir_parents(host);
            nand_mkdir_one(host);
            result = nand_kind(host) == 2 ? 0 : -64;
        }
    }
    nand_note("CreateDir", path, result);
    set_r3(ctx, result >= 0 ? 0u : static_cast<std::uint32_t>(result));
}
extern "C" void NANDPrivateCreateDirAsync_8019bcc8(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::uint32_t callback = ctx->gpr[6];
    const std::uint32_t block = ctx->gpr[7];
    NANDCreateDir_8019bbe0(ctx);
    nand_complete(ctx, static_cast<std::int32_t>(ctx->gpr[3]), callback, block);
}
extern "C" void NANDRead_8019b7a4(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const int fd = nand_fd_from(ctx->gpr[3]);
    const std::int32_t got = nand_read_fd(fd, ctx->gpr[4], ctx->gpr[5]);
    char note[48];
    std::snprintf(note, sizeof(note), "fd=%d", fd);
    nand_note("Read", note, got);
    set_r3(ctx, static_cast<std::uint32_t>(got));
}
extern "C" void NANDReadAsync_8019b80c(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::int32_t got = nand_read_fd(nand_fd_from(ctx->gpr[3]), ctx->gpr[4], ctx->gpr[5]);
    nand_complete(ctx, got, ctx->gpr[6], ctx->gpr[7]);
}
extern "C" void NANDWrite_8019b884(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const int fd = nand_fd_from(ctx->gpr[3]);
    const std::int32_t put = nand_write_fd(fd, ctx->gpr[4], ctx->gpr[5]);
    char note[48];
    std::snprintf(note, sizeof(note), "fd=%d", fd);
    nand_note("Write", note, put);
    set_r3(ctx, static_cast<std::uint32_t>(put));
}
extern "C" void NANDWriteAsync_8019b8ec(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::int32_t put = nand_write_fd(nand_fd_from(ctx->gpr[3]), ctx->gpr[4], ctx->gpr[5]);
    nand_complete(ctx, put, ctx->gpr[6], ctx->gpr[7]);
}
extern "C" void NANDSeek_8019b964(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::int32_t pos = nand_seek_fd(nand_fd_from(ctx->gpr[3]), static_cast<std::int32_t>(ctx->gpr[4]),
                                          static_cast<std::int32_t>(ctx->gpr[5]));
    set_r3(ctx, static_cast<std::uint32_t>(pos));
}
extern "C" void NANDSeekAsync_8019ba04(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::int32_t pos = nand_seek_fd(nand_fd_from(ctx->gpr[3]), static_cast<std::int32_t>(ctx->gpr[4]),
                                          static_cast<std::int32_t>(ctx->gpr[5]));
    nand_complete(ctx, pos, ctx->gpr[6], ctx->gpr[7]);
}
extern "C" void NANDGetLength_8019bf4c(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::uint32_t size = nand_length_fd(nand_fd_from(ctx->gpr[3]));
    if (ctx->gpr[4] != 0) {
        Memory::Write32(ctx->gpr[4], size);
    }
    // NandMgr::CheckRKSYSLength compares this against exactly 0x2BC000 (2,867,200) and fails the
    // whole save on any other value, so the number we return is worth seeing.
    nand_note("GetLength", "bytes", static_cast<int>(size));
    set_r3(ctx, 0);
}
extern "C" void NANDGetLengthAsync_8019c048(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::uint32_t size = nand_length_fd(nand_fd_from(ctx->gpr[3]));
    if (ctx->gpr[4] != 0) {
        Memory::Write32(ctx->gpr[4], size);
    }
    nand_note("GetLengthAsync", "bytes", static_cast<int>(size));
    nand_complete(ctx, 0, ctx->gpr[5], ctx->gpr[6]);
}
extern "C" void NANDPrivateDeleteAsync_8019b6e4(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::uint32_t callback = ctx->gpr[4];
    const std::uint32_t block = ctx->gpr[5];
    NANDDelete_8019b59c(ctx);
    nand_complete(ctx, static_cast<std::int32_t>(ctx->gpr[3]), callback, block);
}

// ReplaceBinary first recreates a zero-filled RKSYS, then calls this routine.
// Its result decides whether the game writes the initialized core and CRC.
extern "C" void NandMgrDeleteRKSYS_8052c7e4(CpuContext* ctx) {
    if (ctx == nullptr) return;
#if defined(__vita__)
    func_8052C7E4(ctx);
    char line[96];
    std::snprintf(line, sizeof(line), "mkwv: NAND DeleteRKSYS result=%d\n", static_cast<std::int32_t>(ctx->gpr[3]));
    std::printf("%s", line);
    mkwv::runtime::watchdog_log_line(line);
#else
    set_r3(ctx, 0);
#endif
}

// An offline Vita has no WiiConnect24 download volume. The game requires the
// save reset to continue even when that optional task cannot be initialized;
// otherwise ReplaceBinary never writes its freshly created RKSYS core.
extern "C" void NWC24OfflineSaveTask_80672cc8(CpuContext* ctx) {
    if (ctx == nullptr) return;
#if defined(__vita__)
    const std::uint32_t caller = ctx->lr;
    func_80672CC8(ctx);
    if (caller == 0x8052CAC8u) {
        const std::int32_t result = static_cast<std::int32_t>(ctx->gpr[3]);
        char line[112];
        std::snprintf(line, sizeof(line), "mkwv: NAND NWC24 task result=%d offline_continue=%u\n",
                      result, result != 0 ? 1u : 0u);
        std::printf("%s", line);
        mkwv::runtime::watchdog_log_line(line);
        if (result != 0) set_r3(ctx, 0);
    }
#else
    set_r3(ctx, 0);
#endif
}
extern "C" void NANDMove_8019bee8(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char src[96];
    char dst[96];
    guest_string(ctx->gpr[3], src, sizeof(src));
    guest_string(ctx->gpr[4], dst, sizeof(dst));
    nand_absolutize(src, sizeof(src));
    nand_absolutize(dst, sizeof(dst));
    const int result = nand_move_path(src, dst);
    char note[160];
    std::snprintf(note, sizeof(note), "%s -> %s", src, dst);
    nand_note("Move", note, result);
    set_r3(ctx, result >= 0 ? 0u : static_cast<std::uint32_t>(result));
}
extern "C" void NANDGetStatus_8019c380(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char path[96];
    guest_string(ctx->gpr[3], path, sizeof(path));
    nand_absolutize(path, sizeof(path));
    int result = -12;
    if (is_sysconf_path(path)) {
        result = 0;
    } else {
        char host[192];
        if (!nand_host_path(path, host, sizeof(host))) {
            result = -8;
        } else if (nand_kind(host) != 0) {
            result = 0;
        }
    }
    if (result == 0 && ctx->gpr[4] != 0) {
        Memory::Write32(ctx->gpr[4], 0);
        Memory::Write32(ctx->gpr[4] + 4u, 0);
    }
    nand_note("GetStatus", path, result);
    set_r3(ctx, result >= 0 ? 0u : static_cast<std::uint32_t>(result));
}
extern "C" void NANDPrivateGetStatusAsync_8019c448(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::uint32_t callback = ctx->gpr[5];
    const std::uint32_t block = ctx->gpr[6];
    NANDGetStatus_8019c380(ctx);
    nand_complete(ctx, static_cast<std::int32_t>(ctx->gpr[3]), callback, block);
}
extern "C" void NANDInit_8019e18c(CpuContext* ctx) {
    nand_write_guest(0x80346D20u, kNandDataDir);
    Memory::Write32(0x80386848u, 2u);
    nand_ensure_data_dir();
    nand_ensure_setting_txt();
    set_r3(ctx, 0);
}
extern "C" void NANDGetCurrentDir_8019e390(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    if (ctx->gpr[3] == 0) {
        set_r3(ctx, static_cast<std::uint32_t>(-8));
        return;
    }
    nand_write_guest(ctx->gpr[3], kNandDataDir);
    set_r3(ctx, 0);
}
extern "C" void NANDGetType_8019e770(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char path[96];
    guest_string(ctx->gpr[3], path, sizeof(path));
    nand_absolutize(path, sizeof(path));
    int kind = 0;
    int result = -12;
    if (is_sysconf_path(path)) {
        kind = 1;
        result = 0;
    } else {
        char host[192];
        if (nand_host_path(path, host, sizeof(host))) {
            kind = nand_kind(host);
            result = kind != 0 ? 0 : -12;
        } else {
            result = -8;
        }
    }
    if (ctx->gpr[4] != 0) {
        Memory::Write8(ctx->gpr[4], static_cast<std::uint8_t>(kind));
    }
    set_r3(ctx, result >= 0 ? 0u : static_cast<std::uint32_t>(result));
}
extern "C" void NANDPrivateGetTypeAsync_8019e7b4(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    NANDGetType_8019e770(ctx);
    const std::int32_t result = static_cast<std::int32_t>(ctx->gpr[3]);
    nand_complete(ctx, result, ctx->gpr[5], ctx->gpr[6]);
}
extern "C" void NANDOpenAsync_8019c918(CpuContext* ctx) {
    nand_open_async(ctx);
}
extern "C" void NANDOpen_8019c800(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char path[96];
    guest_string(ctx->gpr[3], path, sizeof(path));
    nand_absolutize(path, sizeof(path));
    const int fd = nand_open_path(path, static_cast<int>(ctx->gpr[5]));
    if (!is_sysconf_path(path) || fd <= 0) {
        nand_note("Open", path, fd > 0 ? 0 : fd);
    }
    if (fd > 0) {
        nand_publish(ctx->gpr[4], fd);
    }
    set_r3(ctx, fd > 0 ? 0u : static_cast<std::uint32_t>(fd));
}
extern "C" void NANDPrivateOpenAsync_8019c990(CpuContext* ctx) {
    nand_open_async(ctx);
}
extern "C" void NANDClose_8019ca80(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const int fd = nand_fd_from(ctx->gpr[3]);
    nand_close_fd(fd);
    if (ctx->gpr[3] != 0) {
        Memory::Write8(ctx->gpr[3] + 0x8au, kNandFlagClosed);
    }
    nand_note("Close", "sync", 0);
    set_r3(ctx, 0);
}
extern "C" void NANDSafeOpen_8019cb74(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    ctx->gpr[8] = 0;
    ctx->gpr[9] = 0;
    NANDPrivateSafeOpenAsync_8019d104(ctx);
}
extern "C" void NANDSafeClose_8019cf28(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    nand_close_fd(nand_fd_from(ctx->gpr[3]));
    if (ctx->gpr[3] != 0) {
        Memory::Write8(ctx->gpr[3] + 0x8au, kNandFlagClosed);
    }
    nand_note("SafeClose", "sync", 0);
    set_r3(ctx, 0);
}
extern "C" void NANDCloseAsync_8019caec(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    nand_close_fd(nand_fd_from(ctx->gpr[3]));
    if (ctx->gpr[3] != 0) {
        Memory::Write8(ctx->gpr[3] + 0x8au, kNandFlagClosed);
    }
    nand_complete(ctx, 0, ctx->gpr[4], ctx->gpr[5]);
}
extern "C" void NANDPrivateSafeOpenAsync_8019d104(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char path[96];
    guest_string(ctx->gpr[3], path, sizeof(path));
    nand_absolutize(path, sizeof(path));
    const int mode = static_cast<int>(ctx->gpr[5]);
    char host[192];
    // Safe open never creates. A missing file, including FaceLib, stays absent.
    const int fd = mode < 1 || mode > 3 || (!is_sysconf_path(path) &&
                                            (!nand_host_path(path, host, sizeof(host)) || nand_kind(host) != 1))
                       ? (mode < 1 || mode > 3 ? -8 : -12)
                       : nand_open_path(path, mode);
    nand_note("SafeOpen", path, fd > 0 ? 0 : fd);
    if (fd > 0) {
        nand_publish(ctx->gpr[4], fd);
        nand_complete(ctx, 0, ctx->gpr[8], ctx->gpr[9]);
        return;
    }
    nand_complete(ctx, fd, ctx->gpr[8], ctx->gpr[9]);
}
extern "C" void NANDSafeCloseAsync_8019d720(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    nand_close_fd(nand_fd_from(ctx->gpr[3]));
    if (ctx->gpr[3] != 0) {
        Memory::Write8(ctx->gpr[3] + 0x8au, kNandFlagClosed);
    }
    nand_complete(ctx, 0, ctx->gpr[4], ctx->gpr[5]);
}

extern "C" void OSGetConsoleType_8019f33c(CpuContext* ctx) {
    const std::uint32_t mem2 = Memory::Read32(0x80003118u);
    set_r3(ctx, mem2 == 64u * 1024u * 1024u ? 0x00000012u : 0x10000012u);
}

extern "C" void OSExceptionInit_801a00e0(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void OSSetAlarm_801a0870(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    std::uint32_t handler = ctx->gpr[7];
    std::uint32_t tick_hi = ctx->gpr[5];
    std::uint32_t tick_lo = ctx->gpr[6];
    if (!is_code_addr(handler) && is_code_addr(ctx->gpr[6])) {
        handler = ctx->gpr[6];
        tick_hi = ctx->gpr[4];
        tick_lo = ctx->gpr[5];
    }
    const std::uint64_t delay = (static_cast<std::uint64_t>(tick_hi) << 32u) | tick_lo;
    if (!is_code_addr(handler)) {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "mkwv: rejected OSSetAlarm alarm=%08X delay=%08X%08X handler=%08X r4=%08X r5=%08X r6=%08X r7=%08X\n",
                      ctx->gpr[3], tick_hi, tick_lo, handler, ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7]);
        mkwv::runtime::watchdog_log_line(line);
        return;
    }
    enqueue_alarm(ctx->gpr[3], handler, delay, 0);
}
extern "C" void OSSetPeriodicAlarm_801a08e0(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    // OSSetPeriodicAlarm(OSAlarm*, OSTime start, OSTime period,
    // OSAlarmHandler) is passed in r3, r5:r6, r7:r8, and r9 respectively.
    // Treating r8 (the low period word) as the handler was the source of the
    // repeated 0x00062E08 callback seen in RFLiIsWorking.
    const std::uint64_t start = (static_cast<std::uint64_t>(ctx->gpr[5]) << 32u) | ctx->gpr[6];
    const std::uint64_t period = (static_cast<std::uint64_t>(ctx->gpr[7]) << 32u) | ctx->gpr[8];
    const std::uint32_t handler = ctx->gpr[9];
    if (!is_code_addr(handler)) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "mkwv: rejected OSSetPeriodicAlarm alarm=%08X start=%08X%08X period=%08X%08X handler=%08X r4=%08X\n",
                      ctx->gpr[3], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8], handler, ctx->gpr[4]);
        mkwv::runtime::watchdog_log_line(line);
        return;
    }
    const std::uint64_t now_tb = g_timebase.load(std::memory_order_relaxed);
    const std::uint64_t delay = start > now_tb ? start - now_tb : 0u;
    enqueue_alarm(ctx->gpr[3], handler, delay, period);
}
extern "C" void OSCancelAlarm_801a0964(CpuContext* ctx) {
    if (ctx != nullptr) {
        cancel_alarm(ctx->gpr[3]);
    }
}
extern "C" void OS_HLE_ProcessAlarms(int maxToProcess) {
    CpuContext* cpu = mkwv::runtime::current_guest_ctx();
    if (cpu == nullptr) {
        cpu = &GetPersistentCpuContext();
    }
    fire_due_alarms(cpu, maxToProcess);
}
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess) {
    OS_HLE_ProcessAlarms(maxToProcess);
}

extern "C" void RFLiIsWorking_HLE_800bd860(CpuContext* ctx) {
    bump_time(1012500u);
    fire_due_alarms(ctx, 32);
    constexpr std::uint32_t kRflManagerPtrAddr = 0x80386298u;
    constexpr std::uint32_t kWorkingFlagOffset = 0x1b34u;
    const std::uint32_t manager = Memory::Read32(kRflManagerPtrAddr);
    const std::uint32_t working = manager != 0 ? Memory::Read32(manager + kWorkingFlagOffset) : 0u;
    static unsigned logs = 0;
    if (logs < 4u) {
        std::printf("mkwv: RFLiIsWorking mgr=%08X working=%u\n", manager, working);
        ++logs;
    }
    set_r3(ctx, working);
}
// __OSInitAudioSystem: vita/src/runtime/audio/audio.cpp
extern "C" void DCInvalidateRange_801a1600(CpuContext*) {}
extern "C" void DCFlushRange_801a162c(CpuContext*) {}
extern "C" void DCFlushRangeNoSync_801a168c(CpuContext*) {}
extern "C" void ICInvalidateRange_801a1710(CpuContext*) {}
extern "C" void OSCacheInit_801a1ae4(CpuContext*) {}

extern "C" void DCZeroRange_801a16e4(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::uint32_t addr = ctx->gpr[3] & ~31u;
    const std::uint32_t len = (ctx->gpr[4] + 31u) & ~31u;
    guest_memset(addr, len, 0);
}

extern "C" void ConsoleWrite_80015500(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[5] == 0) {
        set_r3(ctx, 1);
        return;
    }
    const std::uint32_t length = Memory::Read32(ctx->gpr[5]);
    if (length == 0 || ctx->gpr[4] == 0) {
        set_r3(ctx, 0);
        return;
    }
    char buf[160];
    const std::uint32_t n = length < sizeof(buf) - 1u ? length : static_cast<std::uint32_t>(sizeof(buf) - 1u);
    for (std::uint32_t i = 0; i < n; ++i) {
        buf[i] = static_cast<char>(Memory::Read8(ctx->gpr[4] + i));
    }
    buf[n] = 0;
    std::printf("mkwv: console %s", buf);
    if (n == 0 || buf[n - 1] != '\n') {
        std::printf("\n");
    }
    set_r3(ctx, 0);
}

extern "C" void OSReport_801a25d0(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        return;
    }
    seed_sda(ctx);
    char line[320];
    format_guest_printf(line, sizeof(line), ctx, ctx->gpr[3], 4);
    std::printf("mkwv: OSReport %s", line);
    const std::size_t n = std::strlen(line);
    if (n == 0 || line[n - 1] != '\n') {
        std::printf("\n");
    }
    if (const char* heap_mark = std::strstr(line, "heap (0x")) {
        unsigned heap = 0;
        if (std::sscanf(heap_mark, "heap (0x%x", &heap) == 1 && heap >= 0x80000000u && heap < 0x81800000u) {
            g_game_heap = heap;
            g_heap_seen = true;
        }
    }
    if (g_report_depth != 0) {
        return;
    }
    if (std::strstr(line, "PAL60") != nullptr) {
        ++g_report_depth;
        dump_ripper("pal60", ctx);
        ensure_task_thread(ctx);
        dump_ripper("pal60-tt", ctx);
        --g_report_depth;
    }
}

extern "C" void OSPanic_801a2660(CpuContext* ctx) {
    static bool logged = false;
    if (!logged) {
        char file[96];
        char msg[256];
        guest_string(ctx != nullptr ? ctx->gpr[3] : 0, file, sizeof(file));
        format_guest_printf(msg, sizeof(msg), ctx, ctx != nullptr ? ctx->gpr[5] : 0, 6);
        std::printf("mkwv: OSPanic %s:%d %s\n", file[0] != 0 ? file : "?",
                    ctx != nullptr ? static_cast<int>(ctx->gpr[4]) : 0, msg);
        dump_ripper("panic", ctx);
        logged = true;
    }
#if defined(__vita__)
    for (;;) {
        mkwv::runtime::hle_present_frame();
        sceKernelDelayThread(16000);
    }
#endif
}

// __OSStopAudioSystem: vita/src/runtime/audio/audio.cpp

// Interrupt state per host thread; with the sound thread on, "disabled" also holds the guest
// critical-section lock (guest_sync.hpp), so the other guest thread's critical sections wait.
static void set_irqs(bool enabled) {
    if (enabled == g_irqs) {
        return;
    }
    g_irqs = enabled;
    if (enabled) {
        mkwv::runtime::irq_unlock();
    } else {
        mkwv::runtime::irq_lock();
    }
}

extern "C" void OSDisableInterrupts_801a65ac(CpuContext* ctx) {
    const bool prev = g_irqs;
    set_irqs(false);
    set_r3(ctx, prev ? 1u : 0u);
}

extern "C" void OSEnableInterrupts_801a65c0(CpuContext* ctx) {
    const bool prev = g_irqs;
    set_irqs(true);
    set_r3(ctx, prev ? 1u : 0u);
}

extern "C" void OSRestoreInterrupts_801a65d4(CpuContext* ctx) {
    const bool prev = g_irqs;
    set_irqs(ctx != nullptr && ctx->gpr[3] != 0);
    set_r3(ctx, prev ? 1u : 0u);
}

extern "C" void OSInterruptInit_801a661c(CpuContext* ctx) {
    set_irqs(false);
    Memory::Write32(kInterruptHandlerTablePtrAddr, kInterruptHandlerTableAddr);
    guest_memset(kInterruptHandlerTableAddr, 0x80u, 0);
    Memory::Write32(kInterruptMaskLoAddr, 0);
    Memory::Write32(kInterruptMaskHiAddr, 0);
    set_r3(ctx, 0);
}


// ---------------------------------------------------------------------------------------------
// Cooperative guest threads (see hle_audio.hpp). Only the threads listed in kFiberEntries run;
// every other OSThread keeps the inline model (never scheduled, its work done where it is
// posted). A fiber runs on the guest's host thread when something wakes it and until it blocks:
// all blocking is on an OSThreadQueue address - a message queue's receive queue (queue + 8),
// the queue OSSleepThread names, or a mutex (its queue is at offset 0).
// ---------------------------------------------------------------------------------------------
namespace {

constexpr std::uint32_t kFiberEntries[] = {
    0x800A49C0u,  // nw4r::snd::detail::SoundThread::SoundThreadFunc (the AX-frame sound tick)
    0x800A98F0u,  // nw4r::snd::detail::TaskThread::ThreadFunc (stream data loading)
};
using mkwv::runtime::kFiberStackBytes;
using mkwv::runtime::kMaxFibers;

enum class FiberState : std::uint8_t { Free, Created, Ready, Running, Blocked, Done };

struct GuestFiber {
    std::uint32_t thread = 0;
    std::uint32_t entry = 0;
    FiberState state = FiberState::Free;
    std::uint32_t wait_queue = 0;
    bool irqs = true;  // the fiber's own interrupt-enable state while it is switched out
    void* host = nullptr;
    CpuContext cpu{};
    mkwv::runtime::prof::Stack pstack;  // the profiler's shadow stack for this thread
};

GuestFiber g_fibers[kMaxFibers];
// The fiber running on this host thread (fibers run on the audio thread when the sound thread
// is on, on the game thread otherwise).
GuestFiber* g_current_fiber_slot[2] = {nullptr, nullptr};
#define g_current_fiber (g_current_fiber_slot[mkwv::runtime::guest_thread_slot()])

// The OSThread the calling guest code runs as. __OSCurrentThread (0x800000E4) stays the game
// thread's while fibers run on another host thread.
std::uint32_t current_os_thread() {
    if (g_current_fiber != nullptr) {
        return g_current_fiber->thread;
    }
    return Memory::Read32(kOSRunningContextAddr);
}
mkwv::runtime::FiberOps g_fiber_ops{};
bool g_fibers_on = false;
unsigned g_fiber_switches = 0;

bool is_fiber_entry(std::uint32_t entry) {
    for (std::uint32_t e : kFiberEntries) {
        if (e == entry) {
            return true;
        }
    }
    return false;
}

GuestFiber* fiber_for_thread(std::uint32_t thread) {
    for (GuestFiber& f : g_fibers) {
        if (f.state != FiberState::Free && f.thread == thread) {
            return &f;
        }
    }
    return nullptr;
}

void fiber_main(void* arg) {
    GuestFiber* f = static_cast<GuestFiber*>(arg);
    InvokeIndirectCpu(f->entry, &f->cpu);
    // The thread function returned: the thread has exited.
    f->state = FiberState::Done;
    std::printf("mkwv: fiber thread 0x%08X exited\n", f->thread);
    for (;;) {
        g_fiber_ops.switch_to(g_fiber_ops.main);
    }
}

// Main context: runs `f` until it blocks (or exits).
void run_fiber(GuestFiber* f) {
    if (f->host == nullptr) {
        f->host = g_fiber_ops.create(kFiberStackBytes, &fiber_main, f);
        if (f->host == nullptr) {
            std::printf("mkwv: fiber create failed for thread 0x%08X\n", f->thread);
            f->state = FiberState::Done;
            return;
        }
    }
    const bool threaded = mkwv::runtime::guest_threads_active();
    const std::uint32_t main_thread = Memory::Read32(kOSRunningContextAddr);
    CpuContext* const main_cpu = g_currentCpuContext;
    CpuContext* const main_guest_ctx = mkwv::runtime::current_guest_ctx();
    const bool main_irqs = g_irqs;
    g_irqs = f->irqs;
    if (!threaded) {
        Memory::Write32(kOSRunningContextAddr, f->thread);
    }
    g_current_fiber = f;  // (the scheduler already marked it Running, under the lock)
    ++g_fiber_switches;
    {
        mkwv::runtime::prof::StackScope stack(&f->pstack);
        mkwv::runtime::prof::ActivityScope activity(mkwv::runtime::prof::kSoundThread);
        g_fiber_ops.switch_to(f->host);
    }
    g_current_fiber = nullptr;
    f->irqs = g_irqs;
    g_irqs = main_irqs;
    if (!threaded) {
        Memory::Write32(kOSRunningContextAddr, main_thread);
    }
    g_currentCpuContext = main_cpu;
    mkwv::runtime::set_current_guest_ctx(main_guest_ctx);
}

// Fiber context: blocks the running fiber on `queue` until woken.
// Called with the guest lock held when the sound thread is on: everything this fiber holds is
// released while it is switched out (a blocked thread lets the others run) and taken back when
// it resumes.
void fiber_block(std::uint32_t queue) {
    GuestFiber* f = g_current_fiber;
    CpuContext* const cpu = g_currentCpuContext;
    CpuContext* const guest_ctx = mkwv::runtime::current_guest_ctx();
    f->state = FiberState::Blocked;
    f->wait_queue = queue;
    const unsigned held = mkwv::runtime::irq_release_all();
    g_fiber_ops.switch_to(g_fiber_ops.main);
    mkwv::runtime::irq_reacquire(held);
    g_currentCpuContext = cpu;
    mkwv::runtime::set_current_guest_ctx(guest_ctx);
}

// Returns true when a fiber became ready.
bool fiber_wake(std::uint32_t queue) {
    bool any = false;
    for (GuestFiber& f : g_fibers) {
        if (f.state == FiberState::Blocked && f.wait_queue == queue) {
            f.state = FiberState::Ready;
            f.wait_queue = 0;
            any = true;
        }
    }
    return any;
}

// A wake from guest code: the woken thread preempts at once, as a higher-priority OSThread
// would - unless this runs inside a fiber or an interrupt (the audio tick schedules afterwards).
void fiber_wake_and_run(std::uint32_t queue) {
    bool any = false;
    {
        mkwv::runtime::IrqLockScope lock;
        any = fiber_wake(queue);
    }
    if (!any) {
        return;
    }
    if (mkwv::runtime::guest_threads_active()) {
        // The audio thread runs fibers; from the game thread, just wake it.
        if (!mkwv::runtime::on_audio_thread()) {
            mkwv::runtime::signal_audio_thread();
        }
        return;
    }
    if (g_current_fiber == nullptr && !mkwv::runtime::audio_in_interrupt()) {
        mkwv::runtime::schedule_guest_fibers();
    }
}

} // namespace

namespace mkwv::runtime {

void set_exp_heap_checks(bool enabled) {
    g_exp_heap_checks = enabled;
}

void set_fiber_ops(const FiberOps& ops) {
    IrqLockScope lock;
    g_fiber_ops = ops;
    g_fibers_on = ops.create != nullptr && ops.switch_to != nullptr && ops.main != nullptr;
}

bool os_interrupts_enabled() {
    return g_irqs;
}

void schedule_guest_fibers() {
    if (!g_fibers_on || g_current_fiber != nullptr) {
        return;
    }
    if (guest_threads_active() && !on_audio_thread()) {
        signal_audio_thread();
        return;
    }
    for (unsigned guard = 0; guard < 64; ++guard) {
        GuestFiber* next = nullptr;
        {
            // Picked under the lock (a waker on the other host thread sets Ready under it).
            IrqLockScope lock;
            for (GuestFiber& f : g_fibers) {
                if (f.state == FiberState::Ready) {
                    next = &f;
                    next->state = FiberState::Running;
                    break;
                }
            }
        }
        if (next == nullptr) {
            return;
        }
        run_fiber(next);
    }
}

bool fiber_sleep_on(std::uint32_t queue) {
    if (g_current_fiber == nullptr) {
        return false;
    }
    IrqLockScope lock;
    fiber_block(queue);
    return true;
}

bool any_fiber_ready() {
    IrqLockScope lock;
    for (const GuestFiber& f : g_fibers) {
        if (f.state == FiberState::Ready) {
            return true;
        }
    }
    return false;
}

} // namespace mkwv::runtime

extern "C" void OSInitMessageQueue_801a72fc(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        return;
    }
    seed_sda(ctx);
    const std::uint32_t q = ctx->gpr[3];
    Memory::Write32(q + 0x00u, 0);
    Memory::Write32(q + 0x04u, 0);
    Memory::Write32(q + 0x08u, 0);
    Memory::Write32(q + 0x0Cu, 0);
    Memory::Write32(q + 0x10u, ctx->gpr[4]);
    Memory::Write32(q + 0x14u, ctx->gpr[5]);
    Memory::Write32(q + 0x18u, 0);
    Memory::Write32(q + 0x1Cu, 0);
    if (g_nqueue < 6u) {
        std::printf("mkwv: OSInitMQ q=0x%08X array=0x%08X count=%u\n", q, ctx->gpr[4], ctx->gpr[5]);
        ++g_nqueue;
    }
}

extern "C" void OSSendMessage_801a735c(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        set_r3(ctx, 0);
        return;
    }
    const std::uint32_t q = ctx->gpr[3];
    const std::uint32_t msg = ctx->gpr[4];
    std::uint32_t count = 0, used = 0;
    bool queued = false;
    {
        mkwv::runtime::IrqLockScope lock;
        const std::uint32_t array = Memory::Read32(q + 0x10u);
        count = Memory::Read32(q + 0x14u);
        const std::uint32_t first = Memory::Read32(q + 0x18u);
        used = Memory::Read32(q + 0x1Cu);
        if (count != 0 && count <= 256u && array != 0 && used < count) {
            Memory::Write32(array + ((first + used) % count) * 4u, msg);
            Memory::Write32(q + 0x1Cu, used + 1u);
            queued = true;
        }
    }
    if (g_nsend < 12u) {
        const std::uint32_t peek = msg != 0 ? Memory::Read32(msg) : 0;
        std::printf("mkwv: OSSend q=0x%08X msg=0x%08X peek=0x%08X count=%u used=%u queued=%u\n", q, msg, peek, count,
                    used, queued ? 1u : 0u);
        ++g_nsend;
    }
    // EGG TaskThread jobs are posted as job pointers; anything else (nw4r's sound thread gets
    // small integers) is a plain message, and treating it as a job used to scribble on low memory.
    if (msg >= 0x80000000u && msg < 0x94000000u) {
        queue_egg_job(q, msg);
        drain_egg_jobs(ctx);
    }
    if (queued) {
        fiber_wake_and_run(q + 8u);  // a fiber receiving on this queue
        set_r3(ctx, 1);
        return;
    }
    set_r3(ctx, ctx->gpr[5] != 0 ? 1u : 0u);
}

extern "C" void OSReceiveMessage_801a7424(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        set_r3(ctx, 0);
        return;
    }
    const std::uint32_t q = ctx->gpr[3];
    const std::uint32_t out = ctx->gpr[4];
    const std::uint32_t flags = ctx->gpr[5];
    mkwv::runtime::IrqLockScope lock;
    const std::uint32_t array = Memory::Read32(q + 0x10u);
    const std::uint32_t count = Memory::Read32(q + 0x14u);
    const std::uint32_t first = Memory::Read32(q + 0x18u);
    std::uint32_t used = Memory::Read32(q + 0x1Cu);
    // A fiber thread blocks for a message (OS_MESSAGE_BLOCK); the main thread never does.
    while (g_current_fiber != nullptr && (flags & 1u) != 0 && count != 0 && array != 0 && used == 0) {
        fiber_block(q + 8u);
        used = Memory::Read32(q + 0x1Cu);
    }
    if (count == 0 || array == 0 || used == 0) {
        if (out != 0) {
            Memory::Write32(out, 0);
        }
        set_r3(ctx, 0);
        return;
    }
    const std::uint32_t head = Memory::Read32(q + 0x18u);
    const std::uint32_t msg = Memory::Read32(array + (head % count) * 4u);
    Memory::Write32(q + 0x18u, (head + 1u) % count);
    Memory::Write32(q + 0x1Cu, used - 1u);
    if (out != 0) {
        Memory::Write32(out, msg);
    }
    set_r3(ctx, 1);
}

extern "C" void OSInitMemoryProtection_801a7dfc(CpuContext* ctx) {
    set_r3(ctx, 0);
}

// The guest mutex the game thread is waiting for (0: none). nw4r's sound thread locks the
// SoundThread mutex for every 3 ms frame and on a real Vita runs nearly back to back, so the game
// thread's poll rarely caught it free (static59: 400-640 ms a race frame in SoundPlayer::Update).
// The sound thread now checks this between frames, where its fibers are switched out, and gives
// the game thread a short window to take a free mutex (audio_yield_to_game). Fibers themselves
// never defer to it: static60 had them do that inside OSLockMutex, and a fiber that already held
// one mutex could park on a second one the game wanted and deadlock the pair.
std::atomic<std::uint32_t> g_game_mutex_wait{0};

// A game-thread mutex wait that lasted two seconds: who holds it and what the fibers are doing.
void report_stuck_mutex(std::uint32_t mutex, std::uint32_t owner, std::uint32_t self, std::uint32_t lr) {
    char line[512];
    int n = std::snprintf(line, sizeof(line),
                          "mkwv: STUCK game thread waits 2s for mutex %08X owner=%08X count=%u self=%08X lr=%08X |",
                          mutex, owner, Memory::Read32(mutex + 0x0Cu), self, lr);
    for (const GuestFiber& f : g_fibers) {
        if (f.state == FiberState::Free || n <= 0 || static_cast<std::size_t>(n) >= sizeof(line)) {
            continue;
        }
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " fiber %08X state=%u wait=%08X",
                           f.thread, static_cast<unsigned>(f.state), f.wait_queue);
    }
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(line) - 1) {
        line[n] = '\n';
        line[n + 1] = '\0';
    }
    std::printf("%s", line);
    mkwv::runtime::watchdog_log_line(line);
}

extern "C" void OSLockMutex_801a7ee4(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        return;
    }
    const std::uint32_t mutex = ctx->gpr[3];
    mkwv::runtime::IrqLockScope lock;
    const std::uint32_t self = current_os_thread();
    std::uint32_t owner = Memory::Read32(mutex + 0x08u);
    // A fiber waits for a mutex another thread holds (the main thread can hold one across a
    // wake that runs the fiber), and for a free one the game thread is queued on. The main
    // thread waits only when the sound thread runs on its own host thread (it then really can
    // own the mutex right now); otherwise it never blocks.
    if (g_current_fiber != nullptr) {
        while (owner != 0 && owner != self) {
            fiber_block(mutex);
            owner = Memory::Read32(mutex + 0x08u);
        }
    } else if (owner != 0 && owner != self && mkwv::runtime::guest_threads_active()) {
        mkwv::runtime::prof::ActivityScope waiting(mkwv::runtime::prof::kLockWait);
        g_game_mutex_wait.store(mutex, std::memory_order_release);
        const std::uint64_t t0 = mkwv::perf::now_us();
        bool reported = false;
        unsigned polls = 0;
        while (owner != 0 && owner != self) {
            const unsigned held = mkwv::runtime::irq_release_all();
            // Short polls first: the holder is usually inside one sound frame.
            mkwv::runtime::host_sleep_us(polls < 8u ? 20u : 50u);
            ++polls;
            mkwv::runtime::irq_reacquire(held);
            owner = Memory::Read32(mutex + 0x08u);
            if (!reported && owner != 0 && owner != self && mkwv::perf::now_us() - t0 > 2000000u) {
                reported = true;
                report_stuck_mutex(mutex, owner, self, ctx->lr);
            }
        }
        g_game_mutex_wait.store(0, std::memory_order_release);
    }
    if (owner == 0 || owner == self) {
        Memory::Write32(mutex + 0x08u, self);
        Memory::Write32(mutex + 0x0Cu, Memory::Read32(mutex + 0x0Cu) + 1u);
    }
}

extern "C" void OSUnlockMutex_801a7fc0(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        return;
    }
    const std::uint32_t mutex = ctx->gpr[3];
    mkwv::runtime::IrqLockScope lock;
    std::uint32_t count = Memory::Read32(mutex + 0x0Cu);
    if (count > 0) {
        --count;
        Memory::Write32(mutex + 0x0Cu, count);
        if (count == 0) {
            Memory::Write32(mutex + 0x08u, 0);
            fiber_wake_and_run(mutex);
        }
    }
}

void mkwv::runtime::audio_yield_to_game() {
    // Between sound frames, on the sound thread, with nothing of the guest held: if the game
    // thread is waiting for a mutex that is free right now, let it take it (up to 2 ms).
    const std::uint64_t t0 = mkwv::perf::now_us();
    for (;;) {
        const std::uint32_t m = g_game_mutex_wait.load(std::memory_order_acquire);
        if (m == 0 || Memory::Read32(m + 0x08u) != 0 || mkwv::perf::now_us() - t0 > 2000u) {
            return;
        }
        mkwv::runtime::host_sleep_us(20);
    }
}

extern "C" void OSGetResetCode_801a8a50(CpuContext* ctx) {
    set_r3(ctx, 0);
}

extern "C" void OSThreadInit_801a961c(CpuContext* ctx) {
    guest_memset(kDefaultThreadAddr, kOSThreadBytes, 0);
    Memory::Write16(kDefaultThreadAddr + 0x2C8u, 2);
    Memory::Write32(kDefaultThreadAddr + 0x2D0u, 16);
    Memory::Write32(kDefaultThreadAddr + 0x2D4u, 16);
    Memory::Write32(kOSCurrentContextAddr, kDefaultThreadAddr);
    Memory::Write32(kOSRunningContextAddr, kDefaultThreadAddr);
    Memory::Write32(0x800000DCu, kDefaultThreadAddr);
    Memory::Write32(0x800000E0u, kDefaultThreadAddr);
    if (ctx != nullptr) {
        Memory::Write32(kDefaultThreadAddr + 0x04u, ctx->gpr[1]);
    }
    set_r3(ctx, 0);
}

extern "C" void OSGetCurrentThread_801a98b0(CpuContext* ctx) {
    seed_sda(ctx);
    set_r3(ctx, current_os_thread());
}

extern "C" void ExpHeap_Alloc_80226c04(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        set_r3(ctx, 0);
        return;
    }
    const std::uint32_t egg = ctx->gpr[3];
    const std::uint32_t size = ctx->gpr[4];
    const std::int32_t align = static_cast<std::int32_t>(ctx->gpr[5]);
    const std::uint32_t handle = Memory::Read32(egg + 16u);
    if (handle == 0) {
        set_r3(ctx, 0);
        return;
    }
    mkwv::runtime::IrqLockScope lock;
    note_exp_heap_integrity("alloc-before", ctx, egg, handle, size, align, 0);
    // EGG's virtual method is only a thin adapter over MEM::AllocFromExpHeapEx.
    // Keep the virtual boundary in HLE, but execute the game's translated SDK
    // allocator for the mutation itself.  This prevents the hand-written HLE
    // free-list representation from being mixed with SDK allocations made by
    // NW4R during bootstrap.  The allocator's direct mutex calls are supplied
    // by the native catalog, so this now has the same locking semantics too.
#if defined(__vita__)
    ctx->gpr[3] = handle;
    ctx->gpr[4] = size;
    ctx->gpr[5] = static_cast<std::uint32_t>(align);
    func_80198D88(ctx);
    const std::uint32_t ptr = ctx->gpr[3];
#else
    // The lightweight host test targets intentionally do not link every
    // generated static SDK shard.  Retain the mirrored implementation there
    // so their allocator differential coverage remains available.
    const std::uint32_t ptr = alloc_exp_heap(handle, size, align);
#endif
    note_exp_heap_integrity("alloc-after", ctx, egg, handle, size, align, ptr);
    static unsigned failure_reports = 0;
    if (ptr == 0 && failure_reports++ < 16u) {
        std::uint32_t total = 0, largest = 0, blocks = 0;
        for (std::uint32_t p = Memory::Read32(handle + 60u);
             p != 0 && blocks < 4096u; p = Memory::Read32(p + 12u)) {
            if (!Memory::Contains(p, 16) || Memory::Read16(p) != kFrSig) break;
            const std::uint32_t bytes = Memory::Read32(p + 4u);
            total += bytes;
            if (bytes > largest) largest = bytes;
            ++blocks;
        }
        char line[256];
        std::snprintf(line, sizeof(line),
                      "mkwv: HEAP_FAIL egg=%08X handle=%08X size=%u align=%d lr=%08X start=%08X end=%08X free=%u largest=%u blocks=%u\n",
                      egg, handle, size, align, ctx->lr, Memory::Read32(handle + 24u),
                      Memory::Read32(handle + 28u), total, largest, blocks);
        mkwv::runtime::watchdog_log_line(line);
    }
    // The first few only (every allocation of 1 KiB or more used to be printed and flushed -
    // a TTY syscall each, hundreds per load).
    static unsigned nalloc = 0;
    if (nalloc < 10u) {
        std::printf("mkwv: ExpHeap alloc size=%u align=%d -> 0x%08X handle=0x%08X\n", size, align, ptr, handle);
        ++nalloc;
    }
    set_r3(ctx, ptr);
}

extern "C" void ExpHeap_Free_80226c78(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0 || ctx->gpr[4] == 0) return;
    const std::uint32_t egg = ctx->gpr[3];
    const std::uint32_t handle = Memory::Read32(egg + 16u);
    const std::uint32_t ptr = ctx->gpr[4];
    if (handle == 0 || Memory::Read32(handle) != 0x45585048u) return;
    mkwv::runtime::IrqLockScope lock;
    const std::uint32_t block = ptr - 16u;
    const std::uint32_t payload = Memory::Read16(block) == kUdSig ? Memory::Read32(block + 4u) : 0;
    note_exp_heap_integrity("free-before", ctx, egg, handle, payload, 0, ptr);

    // Pair the translated allocation path above with its translated SDK free.
    // MEM::FreeToExpHeap performs the exact list splice, coalescing and mutex
    // protocol expected by the other generated MEM callers.
#if defined(__vita__)
    ctx->gpr[3] = handle;
    ctx->gpr[4] = ptr;
    func_80199038(ctx);
#else
    if (Memory::Read16(block) != kUdSig) return;
    const std::uint32_t padding = (Memory::Read16(block + 2u) >> 8u) & 0x7fu;
    unlink_used_block(handle, block);
    write_free_block(block - padding, payload + padding);
    insert_free_block(handle, merge_free_block(handle, block - padding));
#endif
    note_exp_heap_integrity("free-after", ctx, egg, handle, payload, 0, ptr);
    if (payload >= 1024u * 1024u && g_exp_heap_checks) {
        std::printf("mkwv: ExpHeap free ptr=0x%08X size=%u handle=0x%08X\n", ptr, payload, handle);
    }
}

namespace {

// The regular dispatcher chooses HLE first. These wrappers must explicitly
// select the translated entry they replace, otherwise they recurse forever.
bool call_original_module_entry(std::uint32_t address, CpuContext* ctx) {
    if (ctx == nullptr) {
        return false;
    }
    const RawDispatchRecord* record = FindStaticIndirectDispatchEntry(
        g_publishedStaticIndirectDispatchTable.load(std::memory_order_acquire), address);
    if (record != nullptr && record->entry != nullptr) {
        CpuContextScope scope(ctx);
        RecompMod::ScopedTranslatedExecutionAddress translated(address);
        record->entry(ctx);
        return true;
    }
#if defined(__vita__)
    if (address == 0x8055531Cu) {
        CpuContextScope scope(ctx);
        RecompMod::ScopedTranslatedExecutionAddress translated(address);
        func_8055531C(ctx);
        return true;
    }
#endif
    std::printf("mkwv: REL translated entry missing %08X\n", address);
    mkwv::runtime::note_missing_target(address);
    set_r3(ctx, 0);
    return false;
}

std::uint32_t strap_module() {
    const std::uint32_t manager = Memory::Read32(0x80385FF8u);
    return manager != 0 ? Memory::Read32(manager + 32u) : 0u;
}

bool reload_static_r_image() {
    char path[256];
    std::snprintf(path, sizeof(path), "%s/StaticR.bin", mkwv::platform::kGuestDir);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    constexpr std::uint32_t kBase = 0x805102E0u;
    constexpr std::uint32_t kSize = 4934832u;
    static std::uint8_t buf[64u * 1024u];
    std::uint32_t done = 0;
    while (done < kSize) {
        std::uint32_t chunk = kSize - done;
        if (chunk > sizeof(buf)) {
            chunk = static_cast<std::uint32_t>(sizeof(buf));
        }
        if (std::fread(buf, 1, chunk, file) != chunk) {
            std::fclose(file);
            return false;
        }
        copy_guest_bytes(kBase + done, buf, chunk);
        done += chunk;
    }
    std::fclose(file);
    return true;
}

} // namespace

extern "C" void StrapScene_LinkModule_800074a4(CpuContext* ctx) {
    ++g_rel_link_calls;
    g_rel_phase = "linking";
    std::printf("mkwv: REL link begin call=%u module=%08X\n", g_rel_link_calls.load(), strap_module());
    std::fflush(stdout);
    // The image is preloaded, but its containing ExpHeap still regards it as
    // free memory and the heap create zeros that arena. Reserve the range,
    // then reload StaticR.bin so scene vtables survive.
    constexpr std::uint32_t kStaticRBase = 0x805102E0u;
    constexpr std::uint32_t kStaticRProlog = 0x8055531Cu;
    const std::uint32_t manager = Memory::Read32(0x80385FF8u);
    if (manager != 0) {
        if (g_rel_reservation == 0) {
            const std::uint32_t heap = Memory::Read32(0x802A40E0u);
            const std::uint32_t handle = heap != 0 ? Memory::Read32(heap + 16u) : 0u;
            const std::uint32_t free = handle != 0 ? Memory::Read32(handle + 60u) : 0u;
            const std::uint32_t start = free != 0 ? align_up(free + 16u, 32u) : 0u;
            // Header/table bytes precede executable section +0xD4. A small
            // allocation prefix is safe there, never in code or relocated data.
            if (start < kStaticRBase || start + 0x4Cu > kStaticRBase + 0xD4u) {
                g_rel_phase = "reservation-layout-failed";
                set_r3(ctx, 0);
                return;
            }
            g_rel_reservation = egg_alloc(heap, 4934832u, 32);
            if (g_rel_reservation != start) {
                g_rel_phase = "reservation-failed";
                set_r3(ctx, 0);
                return;
            }
            // The REL ExpHeap is created over the preloaded image and leaves
            // StaticR .data (scene vtables at 808Bxxxx) as zeros. Reload the
            // blob after the reservation, then put the used-block header back
            // so the heap still owns the range.
            std::uint8_t used_header[16];
            const std::uint32_t used_addr = g_rel_reservation - 16u;
            read_guest_bytes(used_addr, used_header, sizeof(used_header));
            const bool restored = reload_static_r_image();
            copy_guest_bytes(used_addr, used_header, sizeof(used_header));
            Memory::Write32(kStaticRBase, 1u);
            Memory::Write32(kStaticRBase + 4u, 0);
            Memory::Write32(kStaticRBase + 8u, 0);
            Memory::Write32(kStaticRBase + 12u, 0x11u);
            Memory::Write32(kStaticRBase + 52u, kStaticRProlog);
            std::printf("mkwv: REL image restore=%u create_fn=%08X globe_fn12=%08X\n", restored ? 1u : 0u,
                        Memory::Read32(0x808B3CB8u), Memory::Read32(0x808B2D1Cu));
            std::fflush(stdout);
        }
        Memory::Write32(manager + 32u, kStaticRBase);
        g_rel_phase = g_rel_prolog_done ? "ready" : "linked";
        set_r3(ctx, kStaticRBase);
    } else {
        g_rel_phase = "missing-manager";
        set_r3(ctx, 0);
    }
    const std::uint32_t module = strap_module();
    g_rel_module = module;
    g_rel_entry = module != 0 ? Memory::Read32(module + 52u) : 0u;
    if (module != 0 && g_rel_entry != kStaticRProlog) {
        g_rel_phase = "address-mismatch";
    }
    if (module == 0 && manager != 0) {
        g_rel_phase = "link-failed";
    }
    std::printf("mkwv: REL link end module=%08X prolog=%08X result=%08X\n", module,
                module != 0 ? Memory::Read32(module + 52u) : 0u, ctx != nullptr ? ctx->gpr[3] : 0u);
    std::fflush(stdout);
}

extern "C" void StrapScene_CheckInput_800077c8(CpuContext* ctx) {
    ++g_strap_input_samples;
    bool accepted = true;
#if defined(__vita__)
    SceCtrlData pad{};
    accepted = sceCtrlPeekBufferPositive(0, &pad, 1) > 0 && (pad.buttons & SCE_CTRL_CROSS) != 0;
    g_strap_buttons = pad.buttons;
#endif
    if (accepted || !g_strap_input_logged) {
        std::printf("mkwv: strap input cross=%u link_calls=%u module=%08X prolog_done=%u\n",
                    accepted ? 1u : 0u, g_rel_link_calls.load(), strap_module(), g_rel_prolog_done ? 1u : 0u);
        std::fflush(stdout);
        g_strap_input_logged = true;
    }
    set_r3(ctx, accepted ? 1u : 0u);
}

extern "C" void RelProlog_8055531c(CpuContext* ctx) {
    if (g_rel_prolog_done) {
        set_r3(ctx, 0);
        return;
    }
    g_rel_phase = "prolog";
    std::printf("mkwv: REL prolog begin module=%08X\n", strap_module());
    std::fflush(stdout);
    if (call_original_module_entry(0x8055531Cu, ctx)) {
        g_rel_prolog_done = true;
        g_rel_phase = "ready";
    } else {
        g_rel_phase = "missing-prolog";
    }
    const std::uint32_t manager = Memory::Read32(0x80385FC8u);
    const std::uint32_t scene_manager = manager != 0 ? Memory::Read32(manager + 84u) : 0u;
    // If registration returned without a creator, attempt the established
    // ExpHeap route. A null creator alone does not prove allocation failed:
    // overwritten guest state can also cause this symptom.
    if (g_rel_prolog_done && scene_manager != 0 && Memory::Read32(scene_manager + 44u) == 0) {
        const std::uint32_t creator = call_guest(ctx, kExpHeapAlloc, g_game_heap, 4u, 4u, 0u);
        if (creator != 0) {
            Memory::Write32(creator, 0x808B3CB0u);
            call_guest(ctx, 0x80009844u, scene_manager, 0u, creator, 0u);
        }
        const std::uint32_t registered = Memory::Read32(scene_manager + 44u);
        std::printf("mkwv: REL creator fallback alloc=%08X registered=%08X next=%08X\n", creator, registered,
                    Memory::Read32(scene_manager + 20u));
        std::fflush(stdout);
        g_rel_phase = registered != 0 ? "ready-fallback" : "scene-register-failed";
    }
    if (g_rel_prolog_done && scene_manager == 0) {
        g_rel_phase = "no-scene-manager";
    }
    const std::uint32_t create_vt = Memory::Read32(0x808B3CB0u);
    const std::uint32_t create_fn = Memory::Read32(0x808B3CB0u + 8u);
    std::printf("mkwv: REL prolog end done=%u scene_manager=%08X creator=%08X next=%08X create_vt=%08X create_fn=%08X sm12=%08X sm32=%08X sm36=%08X\n",
                g_rel_prolog_done ? 1u : 0u, scene_manager,
                scene_manager != 0 ? Memory::Read32(scene_manager + 44u) : 0u,
                scene_manager != 0 ? Memory::Read32(scene_manager + 20u) : 0u, create_vt, create_fn,
                scene_manager != 0 ? Memory::Read32(scene_manager + 12u) : 0u,
                scene_manager != 0 ? Memory::Read32(scene_manager + 32u) : 0u,
                scene_manager != 0 ? Memory::Read32(scene_manager + 36u) : 0u);
    std::fflush(stdout);
}

// EGG::Decomp::decodeSZS (Yaz0). The guest ripper reads the compressed file through the DVD HLE and
// then reaches this through InvokeDirectCpu<0x80218C2C> (via the translated Decomp::decode dispatcher).
// Desktop does this with a per-byte MemoryInline loop; on Vita every byte would go through
// CompactMemory, so the stream is read page-wise from guest memory, decoded into a host buffer,
// and copied out page-wise. The bounds checks mirror upstream (szsHaxx hardening).
namespace {

struct GuestByteCursor {
    std::uint32_t addr = 0;
    const std::uint8_t* host = nullptr;
    std::uint32_t left = 0;
    bool fault = false;

    bool next(std::uint8_t* out) {
        if (left == 0) {
            const std::uint32_t into = addr & (mkwv::runtime::kPageSize - 1u);
            std::uint32_t chunk = mkwv::runtime::kPageSize - into;
            host = Memory::GetPointer(addr, chunk);
            if (host == nullptr) {
                chunk = 1;
                host = Memory::GetPointer(addr, 1);
            }
            if (host == nullptr) {
                fault = true;
                return false;
            }
            left = chunk;
        }
        *out = *host++;
        --left;
        ++addr;
        return true;
    }
};

} // namespace

extern "C" void EGG_Decomp_decodeSZS_80218c2c(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::uint32_t src = ctx->gpr[3];
    const std::uint32_t dst = ctx->gpr[4];
    std::uint8_t header[16];
    if (src == 0 || dst == 0 || !read_guest_bytes(src, header, 16)) {
        set_r3(ctx, 0);
        return;
    }
    const std::uint32_t expand = (static_cast<std::uint32_t>(header[4]) << 24) |
                                 (static_cast<std::uint32_t>(header[5]) << 16) |
                                 (static_cast<std::uint32_t>(header[6]) << 8) | header[7];
    static unsigned s_logged = 0;
    if (s_logged < 8u) {
        std::printf("mkwv: decodeSZS src=0x%08X dst=0x%08X expand=%u magic=%02X%02X%02X%02X\n", src, dst, expand,
                    header[0], header[1], header[2], header[3]);
        std::fflush(stdout);
        ++s_logged;
    }
    if (expand == 0 || expand > 32u * 1024u * 1024u || Memory::GetPointer(dst, 1) == nullptr) {
        std::printf("mkwv: decodeSZS reject src=0x%08X dst=0x%08X expand=%u\n", src, dst, expand);
        set_r3(ctx, 0);
        return;
    }
    std::vector<std::uint8_t> out(expand);
    GuestByteCursor cur;
    cur.addr = src + 16u;
    std::uint32_t dst_idx = 0;
    std::uint32_t mask = 0;
    std::uint32_t flags = 0;
    bool ok = true;
    while (dst_idx < expand && ok) {
        std::uint8_t b0 = 0;
        if (mask == 0) {
            if (!cur.next(&b0)) {
                ok = false;
                break;
            }
            flags = b0;
            mask = 0x80;
        }
        if ((flags & mask) != 0) {
            if (!cur.next(&b0)) {
                ok = false;
                break;
            }
            out[dst_idx++] = b0;
        } else {
            std::uint8_t high = 0;
            std::uint8_t low = 0;
            if (!cur.next(&high) || !cur.next(&low)) {
                ok = false;
                break;
            }
            const std::uint32_t rep = (static_cast<std::uint32_t>(high) << 8) | low;
            const std::uint32_t distance = (rep & 0xFFFu) + 1u;
            if (distance > dst_idx) {
                std::printf("mkwv: decodeSZS malformed: back-reference before output\n");
                ok = false;
                break;
            }
            std::uint32_t copy_idx = dst_idx - distance;
            std::uint32_t count = rep >> 12;
            if (count != 0) {
                count += 2u;
            } else {
                std::uint8_t extra = 0;
                if (!cur.next(&extra)) {
                    ok = false;
                    break;
                }
                count = static_cast<std::uint32_t>(extra) + 18u;
            }
            if (count > expand - dst_idx) {
                std::printf("mkwv: decodeSZS malformed: output overrun\n");
                ok = false;
                break;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                out[dst_idx++] = out[copy_idx++];
            }
        }
        mask >>= 1;
    }
    if (!ok) {
        std::printf("mkwv: decodeSZS fail src=0x%08X dst=0x%08X at out=%u/%u fault=%d\n", src, dst, dst_idx, expand,
                    cur.fault ? 1 : 0);
        std::fflush(stdout);
        set_r3(ctx, 0);
        return;
    }
    copy_guest_bytes(dst, out.data(), expand);
    if (s_logged < 9u) {
        std::printf("mkwv: decodeSZS ok bytes=%u magic=%02X%02X%02X%02X\n", expand, out[0], out[1], out[2], out[3]);
        std::fflush(stdout);
        ++s_logged;
    }
    set_r3(ctx, expand);
}

extern "C" void OSCreateThread_801a9e84(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        set_r3(ctx, 0);
        return;
    }
    seed_sda(ctx);
    mkwv::runtime::IrqLockScope lock;
    const std::uint32_t thread = ctx->gpr[3];
    const std::uint32_t entry = ctx->gpr[4];
    const std::uint32_t arg = ctx->gpr[5];
    const std::uint32_t stack = ctx->gpr[6] & ~7u;
    const std::int32_t prio = static_cast<std::int32_t>(ctx->gpr[8]);
    if (prio < 0 || prio > 31) {
        set_r3(ctx, 0);
        return;
    }
    // OSThread ends at offset 0x317.  The task-thread allocation is exactly
    // 0x318 bytes, followed immediately by the ExpHeap's next free header;
    // clearing 0x320 bytes overwrites that header's signature and size.
    guest_memset(thread, kOSThreadBytes, 0);
    Memory::Write16(thread + 0x2C8u, 1);
    Memory::Write16(thread + 0x2CAu, static_cast<std::uint16_t>(ctx->gpr[9] & 1u));
    Memory::Write32(thread + 0x2CCu, 1);
    Memory::Write32(thread + 0x2D0u, static_cast<std::uint32_t>(prio));
    Memory::Write32(thread + 0x2D4u, static_cast<std::uint32_t>(prio));
    Memory::Write32(thread + 0x04u, stack - 8u);
    Memory::Write32(thread + 0x0Cu, arg);
    Memory::Write32(thread + 0x84u, 0x801AA0F0u);
    Memory::Write32(thread + 0x198u, entry);
    Memory::Write32(thread + 0x304u, ctx->gpr[6]);
    Memory::Write32(thread + 0x308u, ctx->gpr[6] - ctx->gpr[7]);
    const std::uint32_t tail = Memory::Read32(0x800000E0u);
    if (tail != 0) {
        Memory::Write32(tail + 0x2FCu, thread);
        Memory::Write32(thread + 0x300u, tail);
    } else {
        Memory::Write32(0x800000DCu, thread);
    }
    Memory::Write32(thread + 0x2FCu, 0);
    Memory::Write32(0x800000E0u, thread);
    if (g_ncreate < 6u) {
        std::printf("mkwv: OSCreateThread th=0x%08X entry=0x%08X arg=0x%08X prio=%d\n", thread, entry, arg, prio);
        ++g_ncreate;
    }
    if (entry == kThreadRunEntry || entry == 0x80242D7Cu) {
        g_task_thread = arg;
    }
    if (g_fibers_on && is_fiber_entry(entry) && fiber_for_thread(thread) == nullptr) {
        for (GuestFiber& f : g_fibers) {
            if (f.state == FiberState::Free || f.state == FiberState::Done) {
                f = GuestFiber{};
                f.thread = thread;
                f.entry = entry;
                f.state = FiberState::Created;
                f.cpu = *ctx;
                seed_sda(&f.cpu);
                const std::uint32_t sp = stack - 8u;
                Memory::Write32(sp, 0);  // back chain
                f.cpu.gpr[1] = sp;
                f.cpu.gpr[3] = arg;
                f.cpu.lr = 0x801AA0F0u;
                std::printf("mkwv: fiber for thread 0x%08X entry=0x%08X prio=%d\n", thread, entry, prio);
                break;
            }
        }
    }
    set_r3(ctx, 1);
}

extern "C" void OSCancelThread_801aa1d4(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        return;
    }
    const std::uint32_t thread = ctx->gpr[3];
    Memory::Write16(thread + 0x2C8u, 0);
}

extern "C" void OSDetachThread_801aa4ec(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        return;
    }
    const std::uint32_t thread = ctx->gpr[3];
    Memory::Write16(thread + 0x2CAu, static_cast<std::uint16_t>(Memory::Read16(thread + 0x2CAu) | 1u));
    if (Memory::Read16(thread + 0x2C8u) == 4) {
        Memory::Write16(thread + 0x2C8u, 0);
    }
}

extern "C" void OSResumeThread_801aa58c(CpuContext* ctx) {
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        set_r3(ctx, 0);
        return;
    }
    const std::uint32_t thread = ctx->gpr[3];
    mkwv::runtime::IrqLockScope lock;
    std::uint32_t suspend = Memory::Read32(thread + 0x2CCu);
    set_r3(ctx, suspend);
    if (suspend > 0) {
        --suspend;
        Memory::Write32(thread + 0x2CCu, suspend);
        if (suspend == 0) {
            Memory::Write16(thread + 0x2C8u, 1);
            const std::uint32_t entry = Memory::Read32(thread + 0x198u);
            const std::uint32_t arg = Memory::Read32(thread + 0x0Cu);
            static unsigned nresume = 0;
            if (nresume < 4u) {
                std::printf("mkwv: OSResume th=0x%08X entry=0x%08X arg=0x%08X\n", thread, entry, arg);
                ++nresume;
            }
            if (entry == kThreadRunEntry || entry == 0x80242D7Cu) {
                g_task_thread = arg;
            }
            if (GuestFiber* f = fiber_for_thread(thread); f != nullptr && f->state == FiberState::Created) {
                f->state = FiberState::Ready;
                if (g_current_fiber == nullptr) {
                    mkwv::runtime::schedule_guest_fibers();
                }
            }
        }
    }
}

extern "C" void OSWakeupThread_HLE_801aaaa4(CpuContext* ctx) {
    if (ctx != nullptr && ctx->gpr[3] != 0) {
        fiber_wake_and_run(ctx->gpr[3]);
    }
}

extern "C" void OSGetTime_801aad5c(CpuContext* ctx) {
    set_time(ctx, bump_time(40500u));
}
extern "C" void OSSleepTicks_801aaca8(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    // The save path waits here for an alarm. A missing SleepTicks returned at
    // once, so that alarm never became due and the written rksys stayed zeros.
    const std::uint64_t ticks = (static_cast<std::uint64_t>(ctx->gpr[3]) << 32u) | ctx->gpr[4];
    static bool logged = false;
    if (!logged) {
        char line[96];
        std::snprintf(line, sizeof(line), "mkwv: OSSleepTicks %08X%08X\n", ctx->gpr[3], ctx->gpr[4]);
        std::printf("%s", line);
        mkwv::runtime::watchdog_log_line(line);
        logged = true;
    }
    bump_time(ticks == 0 ? 1u : ticks);
    fire_due_alarms(ctx, 32);
    if (mkwv::runtime::guest_threads_active()) {
        // A wait loop may be waiting for the sound thread: give it the lock and a moment.
        const unsigned held = mkwv::runtime::irq_release_all();
        mkwv::runtime::host_sleep_us(100);
        mkwv::runtime::irq_reacquire(held);
    }
    mkwv::runtime::audio_poll(ctx);
}

extern "C" void OSGetSystemTime_801aad7c(CpuContext* ctx) {
    set_time(ctx, bump_time(40500u));
}

extern "C" void OSSetPowerCallback_801ab75c(CpuContext* ctx) {
    const std::uint32_t prev = g_power_cb;
    if (ctx != nullptr) {
        g_power_cb = ctx->gpr[3];
    }
    set_r3(ctx, prev);
}

extern "C" void OSInitSTM_801ab848(CpuContext* ctx) {
    if (ctx != nullptr && ctx->gpr[13] != 0) {
        Memory::Write32(ctx->gpr[13] - 0x62ccu, 1);
        Memory::Write32(ctx->gpr[13] - 0x62c8u, 0x00535401u);
        Memory::Write32(ctx->gpr[13] - 0x62c4u, 0x00535402u);
    }
    set_r3(ctx, 1);
}

extern "C" void OSCheckInstall_801ad1d4(CpuContext* ctx) {
    if (ctx != nullptr && ctx->gpr[6] != 0) {
        Memory::Write32(ctx->gpr[6], 3);
    }
    set_r3(ctx, 0);
}

extern "C" void OSIsTitleInstalled_801ae4a0(CpuContext* ctx) {
    set_r3(ctx, 1);
}

extern "C" void PADInit_801af2f0(CpuContext* ctx) {
    set_r3(ctx, 0xFu);
}
extern "C" void PADRead_801af44c(CpuContext* ctx) {
    // GameCube PADStatus is 12 bytes, with an error byte at +10. Port 0 is
    // the Vita controller; the other ports must explicitly say disconnected.
    if (ctx == nullptr || ctx->gpr[3] == 0) {
        set_r3(ctx, 0);
        return;
    }
    const std::uint32_t status = ctx->gpr[3];
    guest_memset(status, 4u * 12u, 0);
    for (std::uint32_t port = 1; port < 4; ++port) {
        Memory::Write8(status + port * 12u + 10u, 0xFFu); // PAD_ERR_NO_CONTROLLER
    }
#if defined(__vita__)
    SceCtrlData pad{};
    if (sceCtrlPeekBufferPositive(0, &pad, 1) <= 0) {
        Memory::Write8(status + 10u, 0xFFu);
    } else {
        std::uint16_t buttons = 0;
        if (pad.buttons & SCE_CTRL_LEFT)     buttons |= 0x0001u;
        if (pad.buttons & SCE_CTRL_RIGHT)    buttons |= 0x0002u;
        if (pad.buttons & SCE_CTRL_DOWN)     buttons |= 0x0004u;
        if (pad.buttons & SCE_CTRL_UP)       buttons |= 0x0008u;
        if (pad.buttons & SCE_CTRL_RTRIGGER) buttons |= 0x0020u;
        if (pad.buttons & SCE_CTRL_LTRIGGER) buttons |= 0x0040u;
        if (pad.buttons & SCE_CTRL_CROSS)    buttons |= 0x0100u; // GC A
        if (pad.buttons & SCE_CTRL_CIRCLE)   buttons |= 0x0200u; // GC B
        if (pad.buttons & SCE_CTRL_SQUARE)   buttons |= 0x0400u; // GC X
        if (pad.buttons & SCE_CTRL_TRIANGLE) buttons |= 0x0800u; // GC Y
        if (pad.buttons & SCE_CTRL_START)    buttons |= 0x1000u; // GC Start
        Memory::Write16(status, buttons);
        const auto stick = [](std::uint8_t value, bool invert) -> std::uint8_t {
            int axis = invert ? 128 - static_cast<int>(value) : static_cast<int>(value) - 128;
            if (axis > 127) axis = 127;
            return static_cast<std::uint8_t>(static_cast<std::int8_t>(axis));
        };
        Memory::Write8(status + 2u, stick(pad.lx, false));
        Memory::Write8(status + 3u, stick(pad.ly, true));
        Memory::Write8(status + 4u, stick(pad.rx, false));
        Memory::Write8(status + 5u, stick(pad.ry, true));
        Memory::Write8(status + 6u, (pad.buttons & SCE_CTRL_LTRIGGER) ? 255u : 0u);
        Memory::Write8(status + 7u, (pad.buttons & SCE_CTRL_RTRIGGER) ? 255u : 0u);
        Memory::Write8(status + 8u, (pad.buttons & SCE_CTRL_CROSS) ? 255u : 0u);
        Memory::Write8(status + 9u, (pad.buttons & SCE_CTRL_CIRCLE) ? 255u : 0u);
        static std::uint16_t last_buttons = 0xFFFFu;
        if (buttons != last_buttons) {
            std::printf("mkwv: PAD port=0 buttons=%04X lx=%d ly=%d rx=%d ry=%d\n", buttons,
                        static_cast<std::int8_t>(Memory::Read8(status + 2u)),
                        static_cast<std::int8_t>(Memory::Read8(status + 3u)),
                        static_cast<std::int8_t>(Memory::Read8(status + 4u)),
                        static_cast<std::int8_t>(Memory::Read8(status + 5u)));
            last_buttons = buttons;
        }
    }
#else
    Memory::Write8(status + 10u, 0xFFu);
#endif
    set_r3(ctx, 0);
}
extern "C" void PADControlMotor_801af908(CpuContext*) {}
extern "C" void SCCheckStatus_801b0220(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void SCGetAspectRatio_801b1be4(CpuContext* ctx) {
    set_r3(ctx, 1);
}
extern "C" void SCGetEuRgb60Mode_801b1cac(CpuContext* ctx) {
    set_r3(ctx, 1);
}
extern "C" void SCGetLanguage_801b1d0c(CpuContext* ctx) {
    set_r3(ctx, 1);
}
extern "C" void SCGetProductArea_801b23a0(CpuContext* ctx) {
    set_r3(ctx, 2);
}
extern "C" void SIInit_801b2de0(CpuContext*) {}

extern "C" void VISetPreRetraceCallback_801b90f4(CpuContext* ctx) {
    const std::uint32_t prev = g_vi_pre_cb;
    if (ctx != nullptr) {
        g_vi_pre_cb = ctx->gpr[3];
    }
    set_r3(ctx, prev);
}

extern "C" void VISetPostRetraceCallback_801b9138(CpuContext* ctx) {
    const std::uint32_t prev = g_vi_post_cb;
    if (ctx != nullptr) {
        g_vi_post_cb = ctx->gpr[3];
    }
    set_r3(ctx, prev);
}

extern "C" void VIInit_801b94a4(CpuContext*) {
    static bool logged = false;
    if (!logged) {
        std::printf("mkwv: VIInit PAL\n");
        logged = true;
    }
}

extern "C" void VIWaitForRetrace_801b99ec(CpuContext* ctx) {
    bump_time(1012500u);
    ++g_retrace;
    fire_due_alarms(ctx, 8);
    if ((g_vi_pre_cb != 0 || g_vi_post_cb != 0) && ctx != nullptr) {
        // Retrace callbacks are interrupt handlers: under the guest lock (guest_sync.hpp).
        mkwv::runtime::IrqLockScope lock;
        if (g_vi_pre_cb != 0) {
            CpuContext cb = *ctx;
            cb.gpr[3] = g_retrace;
            InvokeIndirectCpu(g_vi_pre_cb, &cb);
        }
        if (g_vi_post_cb != 0) {
            CpuContext cb = *ctx;
            cb.gpr[3] = g_retrace;
            InvokeIndirectCpu(g_vi_post_cb, &cb);
        }
    }
    if (g_retrace == 1 || (g_retrace % 60u) == 0) {
        std::printf("mkwv: VIWait retrace=%u\n", g_retrace);
    }
    // Audio: blocks due now, and more while the present waits for the render thread.
    mkwv::runtime::audio_poll(ctx);
    mkwv::runtime::audio_set_wait_ctx(ctx);
    mkwv::runtime::hle_present_frame();
    mkwv::runtime::hle_frame_boundary();
#if defined(__vita__)
    // Retrace pacing: hold the guest to 60 Hz measured from the previous retrace. This used to
    // be a flat 16 ms sleep on top of however long the frame had already taken (and on top of
    // the swap's own vsync wait), which capped even a fast frame well under 60.
    {
        constexpr std::uint64_t kRetraceUs = 16667;
        static std::uint64_t s_next = 0;
        const std::uint64_t now = mkwv::perf::now_us();
        if (s_next > now && s_next - now <= kRetraceUs) {
            mkwv::perf::Scope timing(mkwv::perf::kSleep);
            mkwv::runtime::prof::ActivityScope activity(mkwv::runtime::prof::kSleep);
            const unsigned held = mkwv::runtime::irq_release_all();  // a sleeping thread holds nothing
            sceKernelDelayThread(static_cast<SceUInt>(s_next - now));
            mkwv::runtime::irq_reacquire(held);
            s_next += kRetraceUs;
        } else {
            // Late (or first): no debt carried over, the next retrace is a period from now.
            s_next = now + kRetraceUs;
        }
    }
#endif
    mkwv::runtime::audio_set_wait_ctx(nullptr);
    mkwv::runtime::audio_poll(ctx);
    set_r3(ctx, 0);
}

extern "C" void VIConfigure_801b9f6c(CpuContext*) {}
extern "C" void VIFlush_801ba9a4(CpuContext*) {}
extern "C" void VISetBlack_801bab2c(CpuContext*) {}
extern "C" void VIGetRetraceCount_801baba4(CpuContext* ctx) {
    const auto elapsed = std::chrono::steady_clock::now() - g_retrace_epoch;
    const std::uint32_t realtime = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() * 60 / 1000);
    set_r3(ctx, realtime > g_retrace ? realtime : g_retrace);
}
extern "C" void VIGetDTVStatus_801bad38(CpuContext* ctx) {
    set_r3(ctx, 1);
}

extern "C" void WPADInit_801bf5c4(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void WPADGetStatus_801bf64c(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void WPADSetSyncDeviceCallback_801bf640(CpuContext* ctx) {
    const std::uint32_t previous = g_wpad_sync_callback;
    if (ctx != nullptr) g_wpad_sync_callback = ctx->gpr[3];
    set_r3(ctx, previous);
}
extern "C" void WPADControlMotor_801c0ec4(CpuContext*) {}
extern "C" void WPADGetDpdSensitivity_801c329c(CpuContext* ctx) {
    set_r3(ctx, 3);
}
extern "C" void WPADProbe_801c0990(CpuContext* ctx) {
    if (ctx != nullptr && ctx->gpr[4] != 0) {
        Memory::Write32(ctx->gpr[4], 0);
    }
    // No Wii Remote is advertised until the Vita controller is represented as
    // a full KPAD device. This keeps the title on its PAD fallback path.
    set_r3(ctx, static_cast<std::uint32_t>(-1));
}
extern "C" void KPADGetUnifiedWpadStatus_8019812c(CpuContext* ctx) {
    if (ctx != nullptr && ctx->gpr[4] != 0 && ctx->gpr[5] != 0) {
        const std::uint32_t count = ctx->gpr[5] > 16u ? 16u : ctx->gpr[5];
        guest_memset(ctx->gpr[4], count * 0x38u, 0);
    }
    set_r3(ctx, 0);
}

extern "C" void DCStoreRange_801a165c(CpuContext* ctx) { set_r3(ctx, 0); }
extern "C" void DCStoreRangeNoSync_801a16b8(CpuContext* ctx) { set_r3(ctx, 0); }
// AI / AX / DSP natives and SoundPlayer::SetVolume: vita/src/runtime/audio/audio.cpp

namespace mkwv::runtime {
// hle_gx.cpp: GX shadow reset + GXInit default register stream, GXFlush, display-list recording.
void hle_gx_init();
void hle_gx_flush();
bool hle_gx_in_display_list();
} // namespace mkwv::runtime

extern "C" void GX__Init_8016b850(CpuContext* ctx) {
    guest_memset(kGXDataAddr, 0x600u, 0);
    Memory::Write8(kGXDataAddr + 0x5f8u, 0);
    Memory::Write8(kGXDataAddr + 0x5f9u, 1);
    Memory::Write8(kGXDataAddr + 0x5fau, 1);
    Memory::Write32(kGXDataAddr + 0x174u, 0x0f0000ffu);
    Memory::Write32(kGXDataAddr + 0x07cu, 0x22000000u);
    Memory::Write32(kGXDataAddr + 0x170u, 0x27000000u);
    Memory::Write32(kGXDataPtrAddr, kGXDataAddr);
    guest_memset(kFifoObjAddr, 0x80u, 0);
    if (ctx != nullptr) {
        const std::uint32_t base = ctx->gpr[3];
        const std::uint32_t size = ctx->gpr[4];
        Memory::Write32(kFifoObjAddr + 0x00u, base);
        Memory::Write32(kFifoObjAddr + 0x04u, base + size);
        Memory::Write32(kFifoObjAddr + 0x08u, size);
        Memory::Write32(kFifoObjAddr + 0x14u, base);
        Memory::Write32(kFifoObjAddr + 0x18u, base);
    }
    static bool logged = false;
    if (!logged) {
        std::printf("mkwv: GXInit fifo=0x%08X\n", kFifoObjAddr);
        logged = true;
    }
    // Reset the HLE GXData shadow and emit the SDK's __GXInitGX default hardware state
    // (deferred to first FIFO use if the HLE FIFO is not bound yet).
    mkwv::runtime::hle_gx_init();
    set_r3(ctx, kFifoObjAddr);
}

extern "C" void GX__Flush_8016e654(CpuContext*) {
    mkwv::runtime::hle_gx_flush();
}
// GXSetDispCopySrc(left, top, width, height): the EFB rectangle the display copy sends out.
// The renderer presents exactly this region (see gx::display_copy_source).
extern "C" void GX__SetDispCopySrc_8016f438(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    mkwv::gx::set_display_copy_source(static_cast<int>(ctx->gpr[3] & 0xFFFFu), static_cast<int>(ctx->gpr[4] & 0xFFFFu),
                                      static_cast<int>(ctx->gpr[5] & 0xFFFFu), static_cast<int>(ctx->gpr[6] & 0xFFFFu));
}

// ---- Locked cache and PPC intrinsics the base translation leaves to the platform ----------
// The 16 KiB locked cache is plain memory at 0xE0000000 here (CompactMemory maps it), so
// the DMA engine is a synchronous copy and its queue is always empty.
namespace {
void lc_copy(std::uint32_t dst, std::uint32_t src, std::uint32_t bytes) {
    mkwv::runtime::CompactMemory* mem = mkwv::runtime::guest_memory();
    if (mem == nullptr || bytes == 0 || bytes > 0x4000u * 4u) {
        return;
    }
    static std::uint8_t tmp[0x10000];
    if (mem->read(src, tmp, bytes)) {
        mem->write(dst, tmp, bytes);
    }
}
}  // namespace

extern "C" void LCEnable_801a1834(CpuContext*) {}
extern "C" void LCDisable_801a186c(CpuContext*) {}

// LCLoadBlocks(lc_dst, mem_src, num_blocks): 32-byte blocks, 0 meaning 128.
extern "C" void LCLoadBlocks_801a1894(CpuContext* ctx) {
    const std::uint32_t n = ctx->gpr[5] & 0x7Fu;
    lc_copy(ctx->gpr[3], ctx->gpr[4], (n == 0 ? 128u : n) * 32u);
}

// LCStoreBlocks(mem_dst, lc_src, num_blocks).
extern "C" void LCStoreBlocks_801a18b8(CpuContext* ctx) {
    const std::uint32_t n = ctx->gpr[5] & 0x7Fu;
    lc_copy(ctx->gpr[3], ctx->gpr[4], (n == 0 ? 128u : n) * 32u);
}

// LCStoreData(mem_dst, lc_src, bytes) -> the number of DMA transactions it queued.
extern "C" void LCStoreData_801a18dc(CpuContext* ctx) {
    const std::uint32_t bytes = ctx->gpr[5];
    lc_copy(ctx->gpr[3], ctx->gpr[4], (bytes + 31u) & ~31u);
    set_r3(ctx, (bytes + 4095u) / 4096u);
}

extern "C" void LCQueueLength_801a197c(CpuContext* ctx) {
    set_r3(ctx, 0);
}
extern "C" void LCQueueWait_801a1988(CpuContext*) {}
extern "C" void PPCSync_8012e59c(CpuContext*) {}

// PADReset(mask) -> TRUE: nothing to reset on a Vita pad.
extern "C" void PADReset_801af0dc(CpuContext* ctx) {
    set_r3(ctx, 1);
}
extern "C" void GX__SetDispCopyDst_8016f4b8(CpuContext*) {}
extern "C" void GX__SetCopyFilter_8016fa40(CpuContext*) {}

extern "C" void GX__CopyDisp_8016fc38(CpuContext*) {
    // A display list being recorded still sits in the live FIFO buffer: never present it.
    if (mkwv::runtime::hle_gx_in_display_list()) {
        return;
    }
    mkwv::runtime::hle_present_frame();
}
