#include "abi_bridge.h"
#include "memory.h"
#include "mkwv/runtime/runtime.hpp"
#include "mkwv/runtime/hle_os.hpp"

#include <cstdio>
#include <vector>

extern "C" void func_80198D88(CpuContext*);
extern "C" void func_80199038(CpuContext*);
extern "C" void func_80198E38(CpuContext*);
extern "C" void func_80199180(CpuContext*);
extern "C" void func_80199358(CpuContext*);

namespace {
constexpr uint32_t egg = 0x81000000u, heap = egg + 0x38u;
constexpr uint32_t begin = heap + 0x54u, end = egg + 0x10000u;

struct Trial {
    CpuContext cpu;
    bool reference;
    std::vector<uint32_t> result;

    void snapshot() {
        // Compare the live metadata consumed by the translated sizing,
        // resizing and adjustment routines, ignoring dead payload bytes.
        result.push_back(Memory::Read32(heap + 28));
        for (uint32_t offset : {60u, 64u, 68u, 72u})
            result.push_back(Memory::Read32(heap + offset));
        for (uint32_t offset : {60u, 68u}) {
            unsigned count = 0;
            for (uint32_t p = Memory::Read32(heap + offset); p; p = Memory::Read32(p + 12)) {
                if (++count > 1000) { result.push_back(0xBAD); return; }
                result.push_back(p);
                for (unsigned i = 0; i < 16; i += 4) result.push_back(Memory::Read32(p + i));
            }
            result.push_back(0);
        }
        for (int alignment : {4, 32, -32, 256}) {
            cpu.gpr[3] = heap; cpu.gpr[4] = alignment;
            func_80199180(&cpu);
            result.push_back(cpu.gpr[3]);
        }
    }
    uint32_t alloc(uint32_t size, int alignment) {
        cpu.gpr[3] = reference ? heap : egg;
        cpu.gpr[4] = size; cpu.gpr[5] = alignment;
        if (reference) func_80198D88(&cpu);
        else ExpHeap_Alloc_80226c04(&cpu);
        const uint32_t ptr = cpu.gpr[3];
        result.push_back(ptr);
        if (ptr) {
            // Dirty the heap at setup so this also checks the clear option.
            result.push_back(Memory::Read32(ptr));
            Memory::Write32(ptr, 0x12345678u);
        }
        snapshot();
        return ptr;
    }
    uint32_t alloc_without_write(uint32_t size, int alignment) {
        cpu.gpr[3] = reference ? heap : egg;
        cpu.gpr[4] = size; cpu.gpr[5] = alignment;
        if (reference) func_80198D88(&cpu);
        else ExpHeap_Alloc_80226c04(&cpu);
        result.push_back(cpu.gpr[3]);
        snapshot();
        return cpu.gpr[3];
    }
    void free(uint32_t ptr) {
        cpu.gpr[3] = reference ? heap : egg; cpu.gpr[4] = ptr;
        if (reference) func_80199038(&cpu);
        else ExpHeap_Free_80226c78(&cpu);
        snapshot();
    }
    void resize(uint32_t ptr, uint32_t size) {
        cpu.gpr[3] = heap; cpu.gpr[4] = ptr; cpu.gpr[5] = size;
        func_80198E38(&cpu);
        result.push_back(cpu.gpr[3]);
        snapshot();
    }
};

std::vector<uint32_t> run(CpuContext cpu, bool reference, uint32_t options, uint16_t mode) {
    for (uint32_t p = egg; p < end; p += 4) Memory::Write32(p, 0xCDCDCDCDu);
    for (uint32_t p = egg; p < begin + 16; p += 4) Memory::Write32(p, 0);
    Memory::Write32(egg + 16, heap);
    Memory::Write32(heap, 0x45585048u);
    Memory::Write32(heap + 24, begin); Memory::Write32(heap + 28, end);
    Memory::Write32(heap + 56, options);
    Memory::Write32(heap + 60, begin); Memory::Write32(heap + 64, begin);
    Memory::Write16(heap + 76, 0x42); Memory::Write16(heap + 78, mode);
    Memory::Write16(begin, 0x4652); Memory::Write32(begin + 4, end - begin - 16);
    Trial t{cpu, reference, {}};
    const auto a = t.alloc(13, 32); // short unsplittable alignment prefix
    const auto b = t.alloc(44, -32);
    const auto c = t.alloc(63, 256); // split prefix, leaves address-ordered holes
    const auto d = t.alloc(512, 4);
    t.free(a); t.free(c);
    const auto e = t.alloc(20, 4); // first fit versus best fit
    t.resize(d, 64);              // translated shrink on HLE metadata
    t.resize(d, 160);             // translated grow
    t.free(b); t.free(e); t.free(d);
    t.alloc(0, 4);
    auto tail = t.alloc(17, -64);
    t.free(tail);
    // PPC rounding happens after the literal-zero substitution: -1 wraps to
    // zero.  Do not write through this zero-sized allocation.
    t.alloc_without_write(UINT32_MAX, 32);
    // After mixed frees, the last free block must be the physical tail.
    t.cpu.gpr[3] = heap;
    func_80199358(&t.cpu);
    t.result.push_back(t.cpu.gpr[3]);
    t.snapshot();
    return t.result;
}
}

int main() {
    mkwv::runtime::Runtime runtime;
    if (!runtime.initialize(nullptr)) return 1;
    for (uint32_t flags : {0u, 1u, 4u, 5u}) {
        for (uint16_t mode : {0, 1}) {
            auto expected = run(runtime.cpu(), true, flags, mode);
            auto actual = run(runtime.cpu(), false, flags, mode);
            if (actual != expected) {
                for (size_t i = 0; i < actual.size() && i < expected.size(); ++i) {
                    if (actual[i] != expected[i]) {
                        std::fprintf(stderr, "heap flags=%u mode=%u item=%zu SDK=%08X HLE=%08X\n",
                                     flags, mode, i, expected[i], actual[i]);
                        const size_t first = i > 4 ? i - 4 : 0;
                        const size_t last = std::min(i + 5, actual.size());
                        for (size_t j = first; j < last; ++j) {
                            std::fprintf(stderr, "  [%zu] SDK=%08X HLE=%08X\n", j, expected[j], actual[j]);
                        }
                        break;
                    }
                }
                return 1;
            }
        }
    }
    return 0;
}
