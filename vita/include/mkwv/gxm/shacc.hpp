#pragma once

#include <cstdint>

namespace mkwv::gxm {

// Loads libshacccg.suprx if present. Safe to call when the module is missing.
bool shacc_init();
void shacc_shutdown();
bool shacc_ready();

// Compile a Cg string to a GXP blob the caller owns (malloc). Profile is
// vertex when fragment is false. Successful blobs are stored under
// ux0:data/viicompiled so later boots skip SceShaccCg.
bool shacc_compile(const char* source, bool fragment, void** gxp, std::uint32_t* bytes,
                   bool force_recompile = false);

} // namespace mkwv::gxm
