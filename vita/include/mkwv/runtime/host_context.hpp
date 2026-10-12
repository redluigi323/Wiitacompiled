#pragma once

#include <cstddef>

namespace mkwv::runtime {

// Cooperative guest OSThread contexts. Handles are only valid on the thread
// that called initialize_scheduler. Vita uses ARMv7 assembly; there is no
// libco mprotect blob and no Win32 Fiber.
namespace HostContext {

using Handle = void*;
using Entry = void (*)(void*);

bool initialize_scheduler(Handle* scheduler);
void shutdown_scheduler(Handle scheduler);
// Later create() calls take their stacks from [base, base + bytes), lowest address first.
// The range has to lie inside the calling thread's kernel stack (see kFiberThreadReserve).
void adopt_thread_stack(void* base, std::size_t bytes);
Handle create(std::size_t stack_size, Entry entry, void* argument);
void destroy(Handle context);
bool is_current(Handle context);
void switch_to(Handle target);

} // namespace HostContext

} // namespace mkwv::runtime
