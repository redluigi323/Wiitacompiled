#pragma once

#include <cstdint>

namespace mkwv::gx {

// First FIFO decode error of the current frame. Declared here, not in mkwv/gx/fifo.hpp, because
// that header reaches every translated shard via memory_access.h -> hle_fifo.hpp.
void last_fifo_error(std::uint32_t* site, std::uint32_t* cmd, std::uint32_t* offset, std::uint32_t* size,
                     std::uint32_t* depth);

// Where this frame's XF loads landed, bucketed by address. Here for the same reason as
// last_fifo_error: mkwv/gx/fifo.hpp is shard-visible and must not grow for a diagnostic.
void xf_load_stats(std::uint32_t* pn, std::uint32_t* tex, std::uint32_t* ident, std::uint32_t* nrm,
                   std::uint32_t* dtt, std::uint32_t* light, std::uint32_t* reg, std::uint32_t* drop,
                   std::uint32_t* first_tex_addr, std::uint32_t* first_tex_count);

} // namespace mkwv::gx

namespace mkwv::runtime {

// Declared here rather than in dispatch.hpp on purpose: dispatch.hpp is included by abi_bridge.h,
// so touching it would invalidate every translated shard and force a full rebuild.
unsigned missing_histogram(std::uint32_t* addrs, std::uint32_t* hits, unsigned cap);

// nw4r::lyt quads that reached the FIFO, and those dropped because the guest's vertex descriptor
// did not match the packet layout. Both zero means the 2D path is not being called at all.
void lyt_quad_stats(std::uint32_t* emitted, std::uint32_t* skipped);
int lyt_trace_line(char* out, unsigned cap);

// Installs the unmapped-guest-access reporter and, on Vita, starts a watchdog thread that reports
// the last translated address / dispatch count whenever the guest makes no progress for a few
// seconds (a stuck guest otherwise just keeps presenting the clear color).
void start_diagnostics();

// Writes a short runtime event through the same raw Vita path as watchdog.log.
void watchdog_log_line(const char* text);

} // namespace mkwv::runtime
