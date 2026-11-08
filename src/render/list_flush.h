/* The game's list flush (0x455650) with the new renderer (R2-3, R2-4). Shared by the proxy and tools/replay. */
#pragma once
#include "gpu_list.h"

namespace r2flush {

/* Does what 0x455650 does, with the records drawn by r on the GPU and read back into the back buffer before the
   game's own tail (needle, secondary list, fixed records) draws on top. Returns false, having drawn nothing, if r
   cannot draw: the caller then runs the game's own flush. outlines: draw the SeeThrough records as the game's
   outlines (else textured, the default decided 1 Nov 2026). */
bool flush(gpulist::Renderer &r, bool outlines = false);

/* The game's own flush (0x455650). */
void game_flush();

/* the race screens (countdown, race, pause): [0x11B7AA8] & 0x202A8000, as the flush's tail tests */
bool race_screen();
}  // namespace r2flush
