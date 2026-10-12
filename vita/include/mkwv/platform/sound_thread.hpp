#pragma once

namespace mkwv::platform {

// Installs the Vita services the sound thread needs (mkwv/runtime/guest_sync.hpp) and asks the
// runtime to start it with the AI DMA: the audio interrupt and nw4r's sound/task threads then
// run on their own host thread instead of inside the game thread.
// share_with_renderer: the old placement, cores 1-2 above the renderer. Otherwise the mix
// stays on core 2 at a lower priority than the vertex worker.
void install_sound_thread(bool share_with_renderer);

} // namespace mkwv::platform
