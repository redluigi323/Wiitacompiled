#pragma once

namespace mkwv::gx {
// Startup-only setting; keep fixed while render/worker threads are running.
void set_prepared_texgen(bool enabled);
bool prepared_texgen_enabled();
}
