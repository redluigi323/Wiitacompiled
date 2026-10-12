#pragma once

#include "memory.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "mkwv/runtime/dispatch.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>

inline void ApplyRuntimeCallOptions(uint32_t, CpuContext*) {}

enum class FunctionKind : uint8_t {
    BaseTranslated = 0,
    ModTranslated = 1,
    Native = 2,
};

inline constexpr uint32_t kPpcAllNonvolatileFprMask = 0xffffc000u;

struct TranslatedFunctionInfo {
    uint32_t address = 0;
    const char* name = "";
    uint64_t moduleId = 0;
    uint32_t priority = 0;
    uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    void* entryPoint = nullptr;
    void (*rawCpuInvoker)(CpuContext*) = nullptr;
    bool mustRemainDynamicallyDispatchable = true;
    FunctionKind kind = FunctionKind::BaseTranslated;
};

struct RawDispatchRecord {
    uint32_t address = 0;
    void (*entry)(CpuContext*) = nullptr;
    uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    bool preserveNonvolatileGprs = false;
};

struct StaticIndirectDispatchPage {
    uint32_t firstEntry = 0;
    uint16_t entryCount = 0;
    uint16_t reserved = 0;
};

struct StaticIndirectDispatchSegment {
    const StaticIndirectDispatchPage* pages = nullptr;
    uint16_t firstPage = 0;
    uint16_t pageCount = 0;
};

struct StaticIndirectDispatchTable {
    const char* profileName = nullptr;
    const StaticIndirectDispatchSegment* segments = nullptr;
    const RawDispatchRecord* entries = nullptr;
    size_t entryCount = 0;
};

inline const RawDispatchRecord* FindStaticIndirectDispatchEntry(const StaticIndirectDispatchTable* table,
                                                                uint32_t address) noexcept {
    if (!table || !table->segments || !table->entries || table->entryCount == 0) {
        return nullptr;
    }
    const auto& segment = table->segments[address >> 24];
    const uint32_t pageNumber = (address >> 12) & 0x0FFFu;
    if (!segment.pages || pageNumber < segment.firstPage) {
        return nullptr;
    }
    const uint32_t relativePage = pageNumber - segment.firstPage;
    if (relativePage >= segment.pageCount) {
        return nullptr;
    }
    const auto& page = segment.pages[relativePage];
    const size_t first = page.firstEntry;
    const size_t count = page.entryCount;
    if (first > table->entryCount || count > table->entryCount - first) {
        return nullptr;
    }
    size_t lower = first;
    size_t upper = first + count;
    while (lower < upper) {
        const size_t middle = lower + (upper - lower) / 2;
        const uint32_t candidate = table->entries[middle].address;
        if (candidate < address) {
            lower = middle + 1;
        } else {
            upper = middle;
        }
    }
    return lower < first + count && table->entries[lower].address == address ? &table->entries[lower] : nullptr;
}

void RegisterStaticIndirectDispatchTable(const StaticIndirectDispatchTable* table);
inline std::atomic<const StaticIndirectDispatchTable*> g_publishedStaticIndirectDispatchTable{nullptr};

class StaticIndirectDispatchTableRegistrar {
public:
    explicit StaticIndirectDispatchTableRegistrar(const StaticIndirectDispatchTable* table) {
        RegisterStaticIndirectDispatchTable(table);
    }
};

inline unsigned PpcLowestSetBitIndex(uint32_t value) noexcept {
    return static_cast<unsigned>(__builtin_ctz(value));
}

class PpcNonvolatileFprGuard {
public:
    explicit PpcNonvolatileFprGuard(CpuContext*, uint32_t = 0) noexcept {}
};

class PpcNonvolatileGprGuard {
public:
    explicit PpcNonvolatileGprGuard(CpuContext*, bool = false) noexcept {}
};

template <uint32_t>
struct KnownTranslatedCpuCall {
    static constexpr bool kAvailable = false;
    static constexpr uint32_t kNonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    static constexpr bool kMustRemainDynamicallyDispatchable = true;
    static constexpr void (*Entry)(CpuContext*) = nullptr;
};

#define MKW_TRANSLATED_TRAIT(addr, winner, nonvolatile_fpr_write_mask)                   \
    extern "C" void winner(CpuContext* ctx);                                             \
    template <>                                                                          \
    struct KnownTranslatedCpuCall<0x##addr##u> {                                         \
        static constexpr bool kAvailable = true;                                         \
        static constexpr uint32_t kNonvolatileFprWriteMask = nonvolatile_fpr_write_mask; \
        static constexpr bool kMustRemainDynamicallyDispatchable = false;                \
        static constexpr void (*Entry)(CpuContext*) = &winner;                           \
    }

template <uint32_t>
struct KnownNativeCpuCall {
    static constexpr bool kAvailable = false;
    static constexpr uint32_t kNonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    static constexpr void (*Entry)(CpuContext*) = nullptr;
};

template <uint32_t>
struct KnownTypedNativeCpuCall {
    static constexpr bool kAvailable = false;
};

#include "native_cpu_calls.inc"

struct BulkTranslatedFunctionRecord {
    uint32_t address;
    const char* name;
    void (*entry)(CpuContext*);
    FunctionKind kind;
    bool preservesNonvolatileFprs;
    uint32_t nonvolatileFprWriteMask;
    uint32_t priority;
    uint64_t moduleId;
    bool mustRemainDynamicallyDispatchable;
};

inline void RegisterBulkTranslatedFunctions(const BulkTranslatedFunctionRecord*, size_t) {}

class BulkTranslatedFunctionRegistrar {
public:
    BulkTranslatedFunctionRegistrar(const BulkTranslatedFunctionRecord* records, size_t count) {
        RegisterBulkTranslatedFunctions(records, count);
    }
};

inline void ReportMissingCpuTarget(uint32_t target, CpuContext* ctx) {
    static uint32_t seen[128]{};
    static uint32_t seen_count = 0;
    for (uint32_t i = 0; i < seen_count; ++i) {
        if (seen[i] == target) {
            return;
        }
    }
    if (seen_count < 128) {
        seen[seen_count++] = target;
    }
    std::fprintf(stderr, "mkwv: missing cpu target %08x pc=%08x\n", target, ctx ? ctx->pc : 0u);
}

inline void InvokeIndirectCpu(uint32_t target, CpuContext* ctx) {
    if (!mkwv::runtime::invoke_guest(target, ctx)) {
        ReportMissingCpuTarget(target, ctx);
    }
}

template <uint32_t Target>
inline bool IsBaseTranslatedCpuTargetActive() {
    return KnownTranslatedCpuCall<Target>::kAvailable &&
           !KnownTranslatedCpuCall<Target>::kMustRemainDynamicallyDispatchable;
}

template <uint32_t Target>
inline void InvokeDirectCpu(CpuContext* ctx) {
    static_assert(Target != 0, "InvokeDirectCpu cannot target address 0");
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    ApplyRuntimeCallOptions(Target, cpu);
    if constexpr (KnownNativeCpuCall<Target>::kAvailable) {
        KnownNativeCpuCall<Target>::Entry(cpu);
        return;
    }
    if constexpr (KnownTypedNativeCpuCall<Target>::kAvailable) {
        KnownTypedNativeCpuCall<Target>::Invoke(cpu);
        return;
    }
    if constexpr (KnownTranslatedCpuCall<Target>::kAvailable) {
        KnownTranslatedCpuCall<Target>::Entry(cpu);
        return;
    }
    InvokeIndirectCpu(Target, cpu);
}

inline void InvokeIndirectJump(uint32_t target, CpuContext* ctx) {
    InvokeIndirectCpu(target, ctx);
}

#ifndef MKW_STATIC_TRANSLATED_CALL
#define MKW_STATIC_TRANSLATED_CALL(Target, Entry, Context)                 \
    do {                                                                   \
        if constexpr (KnownNativeCpuCall<Target>::kAvailable) {            \
            KnownNativeCpuCall<Target>::Entry(Context);                    \
        } else if constexpr (KnownTypedNativeCpuCall<Target>::kAvailable) { \
            KnownTypedNativeCpuCall<Target>::Invoke(Context);              \
        } else {                                                           \
            ApplyRuntimeCallOptions(Target, Context);                      \
            Entry(Context);                                                \
        }                                                                  \
    } while (false)
#endif
