#pragma once

// Game-side rendering options applied by HLE natives (not shard-visible).
namespace mkwv::runtime {

// EGG::DrawPathDOF (the race depth of field). Off by default, as upstream WiiCompiled does
// (it strips the DOF bit from ScnRenderer::createPath's path mask unconditionally).
void set_race_dof(bool enabled);
bool race_dof_enabled();

// EGG::ExpHeap consistency walks around every allocation and free (HEAP_CORRUPT lines).
// Debugging only - they cost more than the allocator itself. Marker `heap_check`.
void set_exp_heap_checks(bool enabled);

} // namespace mkwv::runtime
