#include "mkwv/gxm/shacc.hpp"
#include "mkwv/platform/data_root.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/shacccg.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mkwv::gxm {
namespace {

constexpr const char* kModulePaths[] = {
    "ur0:data/libshacccg.suprx",
    "ur0:/data/libshacccg.suprx",
    "vs0:sys/external/libshacccg.suprx",
    "app0:libshacccg.suprx",
};

constexpr const char* kCacheDir = mkwv::platform::kDataRoot;

SceUID g_modid = -1;
bool g_ready = false;
SceShaccCgSourceFile g_file{};
SceShaccCgCallbackList g_callbacks{};

void* shacc_malloc(unsigned int size) {
    return std::malloc(size);
}

void shacc_free(void* ptr) {
    std::free(ptr);
}

SceShaccCgSourceFile* open_file(const char* fileName, const SceShaccCgSourceLocation*, const SceShaccCgCompileOptions*,
                                const char**) {
    g_file.fileName = fileName;
    return &g_file;
}

std::uint32_t fnv1a(const char* text, bool fragment) {
    std::uint32_t h = 2166136261u;
    if (text) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
            h ^= *p;
            h *= 16777619u;
        }
    }
    h ^= fragment ? 1u : 0u;
    h *= 16777619u;
    return h;
}

void cache_path(char* out, std::size_t cap, std::uint32_t key) {
    std::snprintf(out, cap, "%s/%08x.gxp", kCacheDir, key);
}

void ensure_cache_dir() {
    sceIoMkdir("ux0:data", 0777);
    const int err = sceIoMkdir(kCacheDir, 0777);
    (void)err;
}

bool load_cached_gxp(std::uint32_t key, void** gxp, std::uint32_t* bytes) {
    char path[64];
    cache_path(path, sizeof(path), key);
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }
    const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size <= 0 || size > 1024 * 1024) {
        sceIoClose(fd);
        return false;
    }
    if (sceIoLseek(fd, 0, SCE_SEEK_SET) < 0) {
        sceIoClose(fd);
        return false;
    }
    void* copy = std::malloc(static_cast<std::size_t>(size));
    if (!copy) {
        sceIoClose(fd);
        return false;
    }
    const int read = sceIoRead(fd, copy, static_cast<SceSize>(size));
    sceIoClose(fd);
    if (read != size) {
        std::free(copy);
        return false;
    }
    *gxp = copy;
    *bytes = static_cast<std::uint32_t>(size);
    return true;
}

void save_cached_gxp(std::uint32_t key, const void* gxp, std::uint32_t bytes) {
    if (!gxp || bytes == 0) {
        return;
    }
    ensure_cache_dir();
    char path[64];
    cache_path(path, sizeof(path), key);
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        std::printf("mkwv shacc: cache write failed %s\n", path);
        return;
    }
    const int wrote = sceIoWrite(fd, gxp, bytes);
    sceIoClose(fd);
    if (wrote != static_cast<int>(bytes)) {
        std::printf("mkwv shacc: cache short write %s\n", path);
    }
}

bool compile_fresh(const char* source, bool fragment, void** gxp, std::uint32_t* bytes) {
    g_file.fileName = "mkwv.cg";
    g_file.text = source;
    g_file.size = static_cast<SceUInt32>(std::strlen(source));

    SceShaccCgCompileOptions options;
    sceShaccCgInitializeCompileOptions(&options);
    options.mainSourceFile = g_file.fileName;
    options.targetProfile = fragment ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
    options.entryFunctionName = "main";
    options.useFx = 1;
    options.optimizationLevel = 2;
    options.useFastmath = 1;
    options.warningLevel = 0;

    const SceShaccCgCompileOutput* output = sceShaccCgCompileProgram(&options, &g_callbacks, 0);
    if (output == nullptr) {
        std::printf("mkwv shacc: compile returned null\n");
        return false;
    }
    for (int i = 0; i < output->diagnosticCount; ++i) {
        const SceShaccCgDiagnosticMessage& msg = output->diagnostics[i];
        const int line = msg.location ? static_cast<int>(msg.location->lineNumber) : -1;
        std::printf("mkwv shacc: %s (line %d): %s\n",
                    msg.level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR ? "error" : "warn", line,
                    msg.message ? msg.message : "");
    }
    if (output->programData == nullptr || output->programSize == 0) {
        sceShaccCgDestroyCompileOutput(output);
        return false;
    }
    void* copy = std::malloc(output->programSize);
    if (!copy) {
        sceShaccCgDestroyCompileOutput(output);
        return false;
    }
    std::memcpy(copy, output->programData, output->programSize);
    *gxp = copy;
    *bytes = output->programSize;
    sceShaccCgDestroyCompileOutput(output);
    return true;
}

} // namespace

bool shacc_init() {
    if (g_ready) {
        return true;
    }
    for (const char* path : kModulePaths) {
        const SceUID id = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
        if (id >= 0) {
            g_modid = id;
            std::printf("mkwv shacc: loaded %s\n", path);
            break;
        }
    }
    if (g_modid < 0) {
        std::printf("mkwv shacc: libshacccg.suprx missing; TEV stays two-pass\n");
        return false;
    }
    sceShaccCgSetDefaultAllocator(shacc_malloc, shacc_free);
    sceShaccCgInitializeCallbackList(&g_callbacks, SCE_SHACCCG_TRIVIAL);
    g_callbacks.openFile = open_file;
    g_ready = true;
    return true;
}

void shacc_shutdown() {
    // Leave libshacccg mapped. Vita3K crashes on SceShaccCg module_stop after a
    // compile; process teardown is enough, and cache-hit boots never load it.
    g_ready = false;
    g_modid = -1;
}

bool shacc_ready() {
    return g_ready;
}

bool shacc_compile(const char* source, bool fragment, void** gxp, std::uint32_t* bytes, bool force_recompile) {
    if (source == nullptr || gxp == nullptr || bytes == nullptr) {
        return false;
    }
    const std::uint32_t key = fnv1a(source, fragment);
    if (!force_recompile && load_cached_gxp(key, gxp, bytes)) {
        std::printf("mkwv shacc: cache hit %08x (%u bytes)\n", key, *bytes);
        return true;
    }
    if (!g_ready && !shacc_init()) {
        return false;
    }
    if (!compile_fresh(source, fragment, gxp, bytes)) {
        return false;
    }
    save_cached_gxp(key, *gxp, *bytes);
    std::printf("mkwv shacc: compiled %s %u bytes key %08x\n", fragment ? "fp" : "vp", *bytes, key);
    return true;
}

} // namespace mkwv::gxm
