#pragma once

#include "ppc_isa_fpenv.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept;

#ifndef MKW_RESTRICT
#define MKW_RESTRICT __restrict
#endif

#if defined(__vita__)
#define MKW_TLS
#else
#define MKW_TLS thread_local
#endif

union PPC_FPR {
    uint64_t raw = 0;
    double d;
    struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        float ps1;
        float ps0;
#else
        float ps0;
        float ps1;
#endif
    } paired;
};

struct CpuContext {
    uint32_t gpr[32]{};
    uint32_t cr = 0;
    uint32_t lr = 0;
    uint32_t ctr = 0;
    uint32_t xer = 0;
    uint32_t fpscr = 0;
    uint32_t pc = 0;
    PPC_FPR fpr[32]{};
    uint32_t gqr[8]{};
    uint32_t hid0 = 0;
    uint32_t hid1 = 0;
    uint32_t hid2 = 0;
    uint32_t srr0 = 0;
    uint32_t srr1 = 0;
    uint32_t msr = 0;
};

inline MKW_TLS CpuContext* g_currentCpuContext = nullptr;
inline CpuContext g_persistentCpuContext{};

inline CpuContext& GetPersistentCpuContext() {
    return g_persistentCpuContext;
}

inline void InitializePersistentCpuContext() {
    g_persistentCpuContext = CpuContext{};
}

class CpuContextScope {
public:
    explicit CpuContextScope(CpuContext* ctx) : previous_(g_currentCpuContext) {
        g_currentCpuContext = ctx;
        savedMxcsr_ = MkwGetHostFpControl();
        if (ctx != nullptr) {
            MkwApplyHostNiMode(ctx->fpscr);
        }
    }

    ~CpuContextScope() {
        g_currentCpuContext = previous_;
        if (previous_ != nullptr) {
            MkwApplyHostNiMode(previous_->fpscr);
        } else {
            MkwRestoreHostMxcsr(savedMxcsr_);
        }
    }

    CpuContextScope(const CpuContextScope&) = delete;
    CpuContextScope& operator=(const CpuContextScope&) = delete;

private:
    CpuContext* previous_ = nullptr;
    uint32_t savedMxcsr_ = 0;
};

inline CpuContext* TryGetCpuContext() noexcept { return g_currentCpuContext; }

inline CpuContext* CurrentCpuContext() {
    CpuContext* cpu = TryGetCpuContext();
    if (cpu == nullptr) {
        std::fprintf(stderr, "[runtime] CurrentCpuContext is NULL\n");
        ShowRuntimeFatalPopup("Runtime context failure", "translated function ran without a CPU context");
        std::abort();
    }
    if (cpu->gpr[1] == 0) {
        std::fprintf(stderr, "[runtime] Guest r1 is NULL\n");
        ShowRuntimeFatalPopup("Guest execution failure", "guest stack pointer became null");
        std::abort();
    }
    return cpu;
}

#define CR_LT 0
#define CR_GT 1
#define CR_EQ 2
#define CR_SO 3

inline void DumpHostStackTraceForRuntimeHelper() {}
inline void MarkFatalErrorReported() {}

[[noreturn]] inline void PPC_Undefined(uint32_t pc, uint32_t rawInstruction, const char* details) {
    std::fprintf(stderr, "[runtime] UNDEFINED pc=0x%08X raw=0x%08X %s\n", pc, rawInstruction,
                 details ? details : "");
    ShowRuntimeFatalPopup("Unsupported guest instruction", details ? details : "");
    std::abort();
}

#define UNDEFINED(pc, rawInstruction, details) PPC_Undefined((pc), (rawInstruction), (details))

inline void SetCR(CpuContext* cpu, int field, int32_t a, int32_t b) {
    uint32_t crField = 0;
    if (a < b) {
        crField |= 0x8;
    }
    if (a > b) {
        crField |= 0x4;
    }
    if (a == b) {
        crField |= 0x2;
    }
    crField |= (cpu->xer >> 31) & 1u;
    const int shift = (7 - field) * 4;
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (crField << shift);
}

inline void SetCR(CpuContext* cpu, int field, uint32_t a, uint32_t b) {
    uint32_t crField = 0;
    if (a < b) {
        crField |= 0x8;
    }
    if (a > b) {
        crField |= 0x4;
    }
    if (a == b) {
        crField |= 0x2;
    }
    crField |= (cpu->xer >> 31) & 1u;
    const int shift = (7 - field) * 4;
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (crField << shift);
}

inline void SetCRFloat(CpuContext* cpu, int field, double a, double b) {
    uint32_t crField = 0;
    if (std::isnan(a) || std::isnan(b)) {
        crField = 0x1;
    } else {
        if (a < b) {
            crField |= 0x8;
        }
        if (a > b) {
            crField |= 0x4;
        }
        if (a == b) {
            crField |= 0x2;
        }
    }
    const int shift = (7 - field) * 4;
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (crField << shift);
}

inline bool GetCRBit(CpuContext* cpu, int field, int bit) {
    const int shift = (7 - field) * 4 + (3 - bit);
    return (cpu->cr >> shift) & 1;
}
