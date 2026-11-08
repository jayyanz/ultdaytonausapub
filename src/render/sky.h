/* The sky at high resolution (R2-5, build 1.1.9). See sky.cpp. */
#pragma once
#include <windows.h>
#include <stdint.h>

namespace sky {

/* One of the game's sky copies, in 640x400 picture units: the rectangle it covers, and where the panorama's art
   texel (0, 0) of that copy would land (art texels are 2x2 picture pixels; u wraps round at kArtW). */
struct Quad { float l, t, r, b; float ox, oy; float u0, v0; };

constexpr int kArtW = 900, kArtH = 400;   /* the panorama: two 450x400 DD_*.BMP files side by side */

/* Patches the game's 8 calls to its sky copy 0x457BE0 (checked first); false if the code is not as expected. */
bool install(BYTE *game_base);

/* This frame's sky, taken by the list flush: the copies the game made since the last call, and the panorama as
   BGRA (premultiplied, alpha 1) rebuilt from the game's pages when they change (version counts the rebuilds).
   false: no sky this frame, or pages not readable (Windows' DirectDraw, or not the 2x layout): the game's own. */
bool take(const Quad **quads, int *count, const uint32_t **art, uint32_t *version);

}  // namespace sky
