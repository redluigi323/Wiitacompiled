#include "mkwv/runtime/host_context.hpp"

#include <psp2/kernel/sysmem.h>

#include <cstdint>
#include <cstring>
#include <new>

extern "C" {
void mkw_co_switch(void** target_sp, void** source_sp);
void* mkw_co_init(void* stack_top, void (*entry)(void*), void* argument);
}

namespace mkwv::runtime::HostContext {
namespace {

constexpr std::size_t kMinStack = 16u * 1024u;

struct Context {
    void* saved_sp = nullptr;
    void* stack = nullptr;
    std::size_t stack_bytes = 0;
    int uid = -1;
    Entry entry = nullptr;
    void* argument = nullptr;
};

Context* g_current = nullptr;
std::uint8_t* g_pool = nullptr;
std::size_t g_pool_bytes = 0;
std::size_t g_pool_used = 0;

void* alloc_stack(std::size_t bytes, int* uid) {
    bytes = (bytes + 4095u) & ~4095u;
    SceUID block = sceKernelAllocMemBlock("mkwv_fiber", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                          static_cast<SceSize>(bytes), nullptr);
    if (block < 0) {
        return nullptr;
    }
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(block, &base) < 0) {
        sceKernelFreeMemBlock(block);
        return nullptr;
    }
    *uid = block;
    return base;
}

} // namespace

bool initialize_scheduler(Handle* scheduler) {
    auto* context = new (std::nothrow) Context();
    if (!context) {
        return false;
    }
    g_current = context;
    *scheduler = context;
    return true;
}

void shutdown_scheduler(Handle scheduler) {
    auto* context = static_cast<Context*>(scheduler);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

void adopt_thread_stack(void* base, std::size_t bytes) {
    g_pool = static_cast<std::uint8_t*>(base);
    g_pool_bytes = bytes;
    g_pool_used = 0;
}

Handle create(std::size_t stack_size, Entry entry, void* argument) {
    if (stack_size < kMinStack) {
        stack_size = kMinStack;
    }
    auto* context = new (std::nothrow) Context();
    if (!context) {
        return nullptr;
    }
    context->entry = entry;
    context->argument = argument;
    const std::size_t bytes = (stack_size + 4095u) & ~4095u;
    if (g_pool != nullptr) {
        // A memblock outside the thread stack is what the Vita calls a stack overflow.
        if (g_pool_used + bytes > g_pool_bytes) {
            delete context;
            return nullptr;
        }
        context->stack = g_pool + g_pool_used;
        g_pool_used += bytes;
        context->uid = -1;
    } else {
        context->stack = alloc_stack(bytes, &context->uid);
    }
    if (!context->stack) {
        delete context;
        return nullptr;
    }
    context->stack_bytes = bytes;
    auto* top = static_cast<std::uint8_t*>(context->stack) + context->stack_bytes;
    context->saved_sp = mkw_co_init(top, entry, argument);
    return context;
}

void destroy(Handle handle) {
    auto* context = static_cast<Context*>(handle);
    if (!context) {
        return;
    }
    if (g_current == context) {
        g_current = nullptr;
    }
    if (context->uid >= 0) {
        sceKernelFreeMemBlock(context->uid);
    }
    delete context;
}

bool is_current(Handle handle) {
    return handle != nullptr && handle == g_current;
}

void switch_to(Handle target) {
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }
    g_current = destination;
    mkw_co_switch(&destination->saved_sp, &source->saved_sp);
    g_current = source;
}

} // namespace mkwv::runtime::HostContext
