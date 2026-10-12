#include "mkwv/runtime/guest_sync.hpp"
#include "mkwv/runtime/hle_os.hpp"

#include "mkwv/platform/data_root.hpp"
#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/guest_image.hpp"

#include "abi_bridge.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>

namespace {

constexpr std::uint32_t kDvdCbState = 0x0Cu;
constexpr std::uint32_t kDvdCbTransferred = 0x20u;
constexpr std::uint32_t kDvdFileStart = 0x30u;
constexpr int kMaxDvdFiles = 2100;
constexpr int kMaxPath = 128;

struct DvdFile {
    std::uint32_t start = 0;
    std::uint32_t size = 0;
    std::uint32_t fst_index = 0;
    char path[kMaxPath]{};
};

DvdFile g_files[kMaxDvdFiles];
int g_nfiles = 0;
bool g_ready = false;
std::uint32_t g_fst_bytes = 0;
std::uint8_t g_fst_blob[65536];

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

void set_r3(CpuContext* ctx, std::uint32_t value) {
    if (ctx != nullptr) {
        ctx->gpr[3] = value;
    }
}

void append_path(char* dst, const char* parent, const char* name) {
    dst[0] = 0;
    if (parent != nullptr && parent[0] != 0) {
        std::snprintf(dst, kMaxPath, "%s/%s", parent, name);
    } else if (name != nullptr) {
        std::snprintf(dst, kMaxPath, "%s", name);
    }
}

bool copy_to_guest(std::uint32_t addr, const std::uint8_t* src, std::uint32_t length) {
    std::uint32_t done = 0;
    while (done < length) {
        const std::uint32_t at = addr + done;
        const std::uint32_t into_page = at & (mkwv::runtime::kPageSize - 1u);
        std::uint32_t chunk = mkwv::runtime::kPageSize - into_page;
        if (chunk > length - done) {
            chunk = length - done;
        }
        std::uint8_t* dest = Memory::GetPointer(at, chunk);
        if (dest == nullptr) {
            return false;
        }
        std::memcpy(dest, src + done, chunk);
        done += chunk;
    }
    return true;
}

bool publish_fst() {
    g_nfiles = 0;
    g_fst_bytes = 0;
    std::FILE* file = std::fopen(mkwv::platform::kFstPath, "rb");
    if (file == nullptr) {
        std::printf("mkwv: DVD fst missing at %s\n", mkwv::platform::kFstPath);
        return false;
    }
    const std::size_t got = std::fread(g_fst_blob, 1, sizeof(g_fst_blob), file);
    std::fclose(file);
    if (got < 12) {
        return false;
    }
    g_fst_bytes = static_cast<std::uint32_t>(got);
    if (!copy_to_guest(mkwv::runtime::kDvdFstBase, g_fst_blob, g_fst_bytes)) {
        std::printf("mkwv: DVD fst copy failed\n");
        return false;
    }
    Memory::Write32(0x80000038u, mkwv::runtime::kDvdFstBase);
    Memory::Write32(0x8000003Cu, g_fst_bytes);
    const std::uint32_t sda = mkwv::runtime::kSdaBase;
    const std::uint32_t fst = mkwv::runtime::kDvdFstBase;
    const std::uint32_t nent = Memory::Read32(fst + 8u);
    Memory::Write32(sda - 26044u, 0x80000000u);
    Memory::Write32(sda - 26048u, fst);
    Memory::Write32(sda - 26056u, nent);
    Memory::Write32(sda - 26052u, fst + nent * 12u);
    Memory::Write32(sda - 26072u, 0);

    const std::uint8_t* host = g_fst_blob;
    const std::uint32_t entries = be32(host + 8);
    if (entries < 1 || entries > 0x2000u || entries * 12u >= g_fst_bytes) {
        return false;
    }
    const char* strings = reinterpret_cast<const char*>(host + entries * 12u);

    struct Dir {
        std::uint32_t end;
        char path[kMaxPath];
    };
    Dir stack[32];
    int depth = 0;
    stack[0].end = entries;
    stack[0].path[0] = 0;

    for (std::uint32_t i = 1; i < entries; ++i) {
        while (depth >= 0 && i >= stack[static_cast<unsigned>(depth)].end) {
            --depth;
        }
        if (depth < 0) {
            depth = 0;
        }
        const std::uint8_t* e = host + i * 12u;
        const bool dir = (e[0] & 1u) != 0;
        const std::uint32_t name_off = (static_cast<std::uint32_t>(e[1]) << 16) |
                                       (static_cast<std::uint32_t>(e[2]) << 8) | e[3];
        const char* name = strings + name_off;
        if (dir) {
            if (depth + 1 >= 32) {
                continue;
            }
            ++depth;
            stack[static_cast<unsigned>(depth)].end = be32(e + 8);
            append_path(stack[static_cast<unsigned>(depth)].path, stack[static_cast<unsigned>(depth - 1)].path, name);
        } else if (g_nfiles < kMaxDvdFiles) {
            DvdFile& slot = g_files[g_nfiles++];
            slot.start = be32(e + 4);
            slot.size = be32(e + 8);
            slot.fst_index = i;
            append_path(slot.path, stack[static_cast<unsigned>(depth)].path, name);
        }
    }
    std::printf("mkwv: DVD fst bytes=%u files=%d at 0x%08X\n", g_fst_bytes, g_nfiles, mkwv::runtime::kDvdFstBase);
    return g_nfiles > 0;
}

const DvdFile* find_file(std::uint32_t disc_off) {
    for (int i = 0; i < g_nfiles; ++i) {
        const DvdFile& f = g_files[i];
        if (disc_off >= f.start && disc_off < f.start + f.size) {
            return &f;
        }
    }
    return nullptr;
}

const DvdFile* find_exact_start(std::uint32_t start) {
    for (int i = 0; i < g_nfiles; ++i) {
        if (g_files[i].start == start) {
            return &g_files[i];
        }
    }
    return nullptr;
}

// DVDRead offset is file-relative. fileInfo+0x30 is this file's FST start (bytes
// from translated FastOpen, or start/4 from HLE FastOpen). Range lookup is wrong
// because packed strap SZS files overlap if you treat start+offset as a disc
// address — that is how English.szs became a read from the middle of Dutch.szs.
const DvdFile* resolve_read(std::uint32_t start_field, std::uint32_t offset, std::uint32_t* into_out) {
    const DvdFile* file = find_exact_start(start_field);
    if (file == nullptr) {
        file = find_exact_start(start_field * 4u);
    }
    if (file != nullptr) {
        *into_out = offset;
        return file;
    }
    std::uint32_t disc = start_field + offset;
    file = find_file(disc);
    if (file != nullptr) {
        *into_out = disc - file->start;
        return file;
    }
    disc = start_field * 4u + offset;
    file = find_file(disc);
    if (file != nullptr) {
        *into_out = disc - file->start;
        return file;
    }
    *into_out = offset;
    return nullptr;
}

char fold_char(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

bool path_ieq(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a == '/') {
        ++a;
    }
    while (*b == '/') {
        ++b;
    }
    while (*a != 0 && *b != 0) {
        if (fold_char(*a) != fold_char(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

const char* skip_dump_prefix(const char* path) {
    if (path == nullptr) {
        return path;
    }
    while (*path == '/') {
        ++path;
    }
    auto starts = [](const char* p, const char* pre) {
        while (*pre != 0) {
            if (*p == 0 || fold_char(*p) != fold_char(*pre)) {
                return false;
            }
            ++p;
            ++pre;
        }
        return true;
    };
    if (starts(path, "data/files/")) {
        return path + 11;
    }
    if (starts(path, "data/")) {
        return path + 5;
    }
    if (starts(path, "files/")) {
        return path + 6;
    }
    return path;
}

const DvdFile* find_path(const char* path) {
    if (path == nullptr || path[0] == 0) {
        return nullptr;
    }
    const char* stripped = skip_dump_prefix(path);
    for (int i = 0; i < g_nfiles; ++i) {
        if (path_ieq(g_files[i].path, path) || path_ieq(g_files[i].path, stripped)) {
            return &g_files[i];
        }
    }
    return nullptr;
}

std::FILE* fopen_disc(const char* rel) {
    if (rel == nullptr || rel[0] == 0) {
        return nullptr;
    }
    const char* stripped = skip_dump_prefix(rel);
    const char* names[2] = {rel, stripped};
    const char* roots[3] = {"", "DATA/files/", "files/"};
    char host_path[256];
    for (const char* name : names) {
        for (const char* root : roots) {
            std::snprintf(host_path, sizeof(host_path), "%s/%s%s", mkwv::platform::kDiscDir, root, name);
            std::FILE* fp = std::fopen(host_path, "rb");
            if (fp != nullptr) {
                return fp;
            }
        }
    }
    return nullptr;
}

// Open disc files, kept between reads. Every read used to fopen (trying up to six path
// spellings, each failure a syscall and a Vita3K error line) and fclose again - streamed audio
// (BRSTM) and archive loads read the same few files over and over, on the game thread.
struct DiscHandle {
    const DvdFile* file = nullptr;
    std::FILE* fp = nullptr;
    std::uint32_t last_use = 0;
};
DiscHandle g_disc_handles[6];
std::uint32_t g_disc_clock = 0;
// Reads come from the game thread and, with the sound thread on, nw4r's stream loader on the
// audio thread: the handle cache and the bounce buffer are shared.
mkwv::runtime::HostSpinLock g_disc_lock;

std::FILE* disc_handle(const DvdFile* file) {
    ++g_disc_clock;
    DiscHandle* victim = &g_disc_handles[0];
    for (DiscHandle& h : g_disc_handles) {
        if (h.file == file && h.fp != nullptr) {
            h.last_use = g_disc_clock;
            return h.fp;
        }
        if (h.fp == nullptr) {
            victim = &h;
        } else if (victim->fp != nullptr && h.last_use < victim->last_use) {
            victim = &h;
        }
    }
    std::FILE* fp = fopen_disc(file->path);
    if (fp == nullptr) {
        return nullptr;
    }
    // Reads are random access into large files; the stdio buffer only costs an extra copy.
    std::setvbuf(fp, nullptr, _IONBF, 0);
    if (victim->fp != nullptr) {
        std::fclose(victim->fp);
    }
    victim->file = file;
    victim->fp = fp;
    victim->last_use = g_disc_clock;
    return fp;
}

// Reads `want` bytes at the handle's position into guest memory: straight into the guest
// buffer where it is contiguous host memory, else through a bounce buffer.
std::uint32_t read_into_guest(std::FILE* fp, std::uint32_t buffer, std::uint32_t want) {
    static std::uint8_t tmp[32768];
    std::uint32_t got = 0;
    while (got < want) {
        const std::uint32_t at = buffer + got;
        const std::uint32_t into_page = at & (mkwv::runtime::kPageSize - 1u);
        std::uint32_t chunk = mkwv::runtime::kPageSize - into_page;
        if (chunk > want - got) {
            chunk = want - got;
        }
        std::size_t n = 0;
        if (std::uint8_t* dest = Memory::GetPointer(at, chunk)) {
            n = std::fread(dest, 1, chunk, fp);
        } else {
            if (chunk > sizeof(tmp)) {
                chunk = static_cast<std::uint32_t>(sizeof(tmp));
            }
            n = std::fread(tmp, 1, chunk, fp);
            for (std::size_t i = 0; i < n; ++i) {
                Memory::Write8(at + static_cast<std::uint32_t>(i), tmp[i]);
            }
        }
        got += static_cast<std::uint32_t>(n);
        if (n < chunk) {
            break;
        }
    }
    return got;
}

const DvdFile* find_index(std::uint32_t fst_index) {
    for (int i = 0; i < g_nfiles; ++i) {
        if (g_files[i].fst_index == fst_index) {
            return &g_files[i];
        }
    }
    return nullptr;
}

void guest_path(std::uint32_t addr, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0) {
        return;
    }
    out[0] = 0;
    if (addr == 0) {
        return;
    }
    std::size_t n = 0;
    while (n + 1 < cap) {
        const char ch = static_cast<char>(Memory::Read8(addr + static_cast<std::uint32_t>(n)));
        if (ch == 0) {
            break;
        }
        out[n++] = ch;
    }
    out[n] = 0;
}

std::int32_t convert_path(const char* path) {
    if (path == nullptr || path[0] == 0 || (path[0] == '/' && path[1] == 0)) {
        return 0;
    }
    const DvdFile* file = find_path(path);
    if (file == nullptr) {
        return -1;
    }
    return static_cast<std::int32_t>(file->fst_index);
}

void fill_info(std::uint32_t info, const DvdFile& file) {
    if (info == 0) {
        return;
    }
    Memory::Write32(info + kDvdFileStart, file.start / 4u);
    Memory::Write32(info + 0x34u, file.size);
    Memory::Write32(info + kDvdCbState, 0);
    Memory::Write32(info + kDvdCbTransferred, 0);
}

std::int32_t read_file(std::uint32_t file_info, std::uint32_t buffer, std::int32_t length, std::int32_t offset) {
    if (file_info == 0 || length <= 0 || offset < 0) {
        return 0;
    }
    mkwv::runtime::HostSpinGuard disc_guard(g_disc_lock);
    const std::uint32_t start_field = Memory::Read32(file_info + kDvdFileStart);
    std::uint32_t into = 0;
    const DvdFile* file = resolve_read(start_field, static_cast<std::uint32_t>(offset), &into);
    if (file == nullptr) {
        static bool logged = false;
        if (!logged) {
            std::printf("mkwv: DVD read miss field=0x%X off=%d len=%d\n", start_field, offset, length);
            logged = true;
        }
        return 0;
    }
    static bool logged_hit = false;
    if (!logged_hit) {
        std::printf("mkwv: DVD read %s start=0x%X into=%u len=%d size=%u\n", file->path, file->start, into, length,
                    file->size);
        logged_hit = true;
    }
    std::uint32_t want = static_cast<std::uint32_t>(length);
    if (into >= file->size) {
        return 0;
    }
    if (want > file->size - into) {
        want = file->size - into;
    }
    std::FILE* fp = disc_handle(file);
    if (fp == nullptr) {
        static bool logged = false;
        if (!logged) {
            std::printf("mkwv: DVD open fail %s/%s (also DATA/files)\n", mkwv::platform::kDiscDir, file->path);
            logged = true;
        }
        return 0;
    }
    if (std::fseek(fp, static_cast<long>(into), SEEK_SET) != 0) {
        return 0;
    }
    const std::uint32_t got = read_into_guest(fp, buffer, want);
    static bool logged = false;
    if (!logged) {
        std::printf("mkwv: DVD read %s +%u -> %u bytes\n", file->path, into, static_cast<unsigned>(got));
        logged = true;
    }
    Memory::Write32(file_info + kDvdCbTransferred, static_cast<std::uint32_t>(got));
    Memory::Write32(file_info + kDvdCbState, 0);
    return static_cast<std::int32_t>(got);
}

void write_disc_id(std::uint32_t dest) {
    if (dest == 0) {
        return;
    }
    Memory::Write32(dest, 0x524D4350u);
    Memory::Write16(dest + 4u, 0x3031u);
    Memory::Write8(dest + 6u, 1);
}

std::int32_t read_abs(std::uint32_t buffer, std::int32_t length, std::uint32_t disc) {
    if (length <= 0 || buffer == 0) {
        return 0;
    }
    if (disc == 0) {
        write_disc_id(buffer);
        return length;
    }
    const DvdFile* file = find_file(disc);
    if (file == nullptr) {
        static bool logged = false;
        if (!logged) {
            std::printf("mkwv: DVD abs miss off=0x%X len=%d\n", disc, length);
            logged = true;
        }
        return 0;
    }
    const std::uint32_t into = disc - file->start;
    if (into >= file->size) {
        return 0;
    }
    mkwv::runtime::HostSpinGuard disc_guard(g_disc_lock);
    std::FILE* fp = disc_handle(file);
    if (fp == nullptr || std::fseek(fp, static_cast<long>(into), SEEK_SET) != 0) {
        return 0;
    }
    std::uint32_t want = static_cast<std::uint32_t>(length);
    if (want > file->size - into) {
        want = file->size - into;
    }
    const std::uint32_t got = read_into_guest(fp, buffer, want);
    return static_cast<std::int32_t>(got);
}

// Same SDK globals upstream completes after every HLE'd transfer (DVDCancelAll / __DVDPrepareReset
// wait on these; the drive work is HLE'd, so nothing is ever left pending).
void complete_dvd_cancel_state() {
    constexpr std::uint32_t kQueueBase = 0x80343230u;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t q = kQueueBase + i * 8u;
        Memory::Write32(q, q);
        Memory::Write32(q + 4u, q);
    }
    Memory::Write32(0x80386664u, 0);
    Memory::Write32(0x80386668u, 0);
    Memory::Write32(0x80386670u, 0);
    Memory::Write32(0x8038667Cu, 1);
    Memory::Write32(0x803866A8u, 1);
    Memory::Write32(0x803866F0u, 0);
}

void invoke_dvd_callback(const CpuContext* ctx, std::uint32_t callback, std::int32_t result, std::uint32_t block) {
    if (callback < 0x80000000u || callback >= 0x81800000u || ctx == nullptr) {
        return;
    }
    CpuContext cb = *ctx;
    cb.gpr[3] = static_cast<std::uint32_t>(result);
    cb.gpr[4] = block;
    InvokeIndirectCpu(callback, &cb);
}

void finish_low(CpuContext* ctx, std::uint32_t callback, std::int32_t result) {
    set_r3(ctx, 1);
    if (callback >= 0x80000000u && callback < 0x81800000u && ctx != nullptr) {
        CpuContext cb = *ctx;
        cb.gpr[3] = static_cast<std::uint32_t>(result);
        InvokeIndirectCpu(callback, &cb);
    }
}

} // namespace

namespace mkwv::runtime {

void publish_dvd_fst() {
    g_ready = publish_fst();
}

} // namespace mkwv::runtime

extern "C" void DVDInit_8015ea1c(CpuContext* ctx) {
    Memory::Write8(0x80386724u, 1);
    Memory::Write8(0x80386725u, 1);
    Memory::Write32(0x80386720u, 0);
    Memory::Write8(0x803866A0u, 1);
    Memory::Write32(0x80386664u, 0);
    Memory::Write32(0x80386668u, 0);
    Memory::Write32(0x80386670u, 0);
    Memory::Write32(0x8038667Cu, 1);
    Memory::Write32(0x803866A8u, 1);
    Memory::Write32(0x803866F0u, 0);
    constexpr std::uint32_t kQueueBase = 0x80343230u;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t q = kQueueBase + i * 8u;
        Memory::Write32(q, q);
        Memory::Write32(q + 4u, q);
    }
    constexpr std::uint32_t kContextBase = 0x803434E0u;
    for (int i = 0; i < 4; ++i) {
        const std::uint32_t c = kContextBase + static_cast<std::uint32_t>(i) * 0x20u;
        Memory::Write32(c + 0x0Cu, 0xFEEBDAEDu);
        Memory::Write32(c + 0x10u, static_cast<std::uint32_t>(i));
    }
    mkwv::runtime::publish_dvd_fst();
    set_r3(ctx, 0);
}

extern "C" void DVDReadPrio_8015e834(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::int32_t got = read_file(ctx->gpr[3], ctx->gpr[4], static_cast<std::int32_t>(ctx->gpr[5]),
                                       static_cast<std::int32_t>(ctx->gpr[6]));
    complete_dvd_cancel_state();
    set_r3(ctx, static_cast<std::uint32_t>(got));
}

extern "C" void DVDReadAsyncPrio_8015e74c(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    // Completes synchronously and runs the callback immediately (same contract as upstream's HLE);
    // the guest ripper / DvdFile code then sees a finished command block on return.
    const std::uint32_t info = ctx->gpr[3];
    const std::int32_t length = static_cast<std::int32_t>(ctx->gpr[5]);
    const std::uint32_t callback = ctx->gpr[7];
    std::int32_t got = read_file(info, ctx->gpr[4], length, static_cast<std::int32_t>(ctx->gpr[6]));
    if (got <= 0 && length > 0) {
        got = -1;
        if (info != 0) {
            Memory::Write32(info + kDvdCbState, static_cast<std::uint32_t>(-1));
        }
    }
    complete_dvd_cancel_state();
    set_r3(ctx, got >= 0 ? 1u : 0u);
    invoke_dvd_callback(ctx, callback, got, info);
    complete_dvd_cancel_state();
}

// DVDReadAbsAsyncPrio(block, buf, len, discOffset, cb, prio) - used for raw-disc reads.
extern "C" void DVDReadAbsAsyncPrio_801628cc(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::uint32_t block = ctx->gpr[3];
    const std::int32_t length = static_cast<std::int32_t>(ctx->gpr[5]);
    const std::uint32_t callback = ctx->gpr[7];
    std::int32_t got = read_abs(ctx->gpr[4], length, ctx->gpr[6]);
    if (got <= 0 && length > 0) {
        got = -1;
    }
    if (block != 0) {
        Memory::Write32(block + kDvdCbState, got >= 0 ? 0u : static_cast<std::uint32_t>(-1));
        Memory::Write32(block + kDvdCbTransferred, got >= 0 ? static_cast<std::uint32_t>(got) : 0u);
    }
    complete_dvd_cancel_state();
    set_r3(ctx, got >= 0 ? 1u : 0u);
    invoke_dvd_callback(ctx, callback, got, block);
    complete_dvd_cancel_state();
}

extern "C" void DVDCheckDevice_801643fc(CpuContext* ctx) {
    set_r3(ctx, 1);
}

extern "C" void DVDLowInit_80164848(CpuContext* ctx) {
    mkwv::runtime::publish_dvd_fst();
    set_r3(ctx, 1);
}

extern "C" void DVDLowReadDiskID_80164aac(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    write_disc_id(ctx->gpr[3]);
    finish_low(ctx, ctx->gpr[4], 1);
}

extern "C" void DVDLowRead_80166330(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const std::int32_t got = read_abs(ctx->gpr[3], static_cast<std::int32_t>(ctx->gpr[4]), ctx->gpr[5]);
    finish_low(ctx, ctx->gpr[6], got > 0 ? 1 : -1);
}

extern "C" void DVDLowUnencryptedRead_80165708(CpuContext* ctx) {
    DVDLowRead_80166330(ctx);
}

extern "C" void DVDLowInquiry_80165a30(CpuContext* ctx) {
    if (ctx != nullptr && ctx->gpr[3] != 0) {
        Memory::Write32(ctx->gpr[3] + kDvdCbState, 0);
    }
    finish_low(ctx, ctx != nullptr ? ctx->gpr[4] : 0, 1);
}

extern "C" void DVDLowClearCoverInterrupt_80166964(CpuContext* ctx) {
    finish_low(ctx, ctx != nullptr ? ctx->gpr[3] : 0, 1);
}

extern "C" void DVDFSInit_8015df1c(CpuContext* ctx) {
    mkwv::runtime::publish_dvd_fst();
    set_r3(ctx, 0);
}

extern "C" void DVDConvertPathToEntrynum_8015df4c(CpuContext* ctx) {
    char path[kMaxPath];
    guest_path(ctx != nullptr ? ctx->gpr[3] : 0, path, sizeof(path));
    const std::int32_t entry = convert_path(path);
    static int logged = 0;
    const bool strap = std::strstr(path, "Boot") != nullptr || std::strstr(path, ".szs") != nullptr;
    if (logged < 8 || strap) {
        std::printf("mkwv: DVD path %s -> %d\n", path[0] != 0 ? path : "(null)", static_cast<int>(entry));
        if (!strap) {
            ++logged;
        }
    } else if (entry < 0) {
        static bool miss = false;
        if (!miss) {
            std::printf("mkwv: DVD path miss %s\n", path);
            miss = true;
        }
    }
    set_r3(ctx, static_cast<std::uint32_t>(entry));
}

extern "C" void DVDFastOpen_8015e254(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const DvdFile* file = find_index(ctx->gpr[3]);
    if (file == nullptr || ctx->gpr[4] == 0) {
        set_r3(ctx, 0);
        return;
    }
    fill_info(ctx->gpr[4], *file);
    set_r3(ctx, 1);
}

extern "C" void DVDOpen_8015e2bc(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    char path[kMaxPath];
    guest_path(ctx->gpr[3], path, sizeof(path));
    const DvdFile* file = find_path(path);
    static bool logged = false;
    if (!logged) {
        std::printf("mkwv: DVD open %s %s\n", path[0] != 0 ? path : "(null)", file != nullptr ? "ok" : "fail");
        logged = true;
    }
    if (file == nullptr || ctx->gpr[4] == 0) {
        set_r3(ctx, 0);
        return;
    }
    fill_info(ctx->gpr[4], *file);
    set_r3(ctx, 1);
}

extern "C" void DVDClose_8015e568(CpuContext* ctx) {
    set_r3(ctx, 1);
}

