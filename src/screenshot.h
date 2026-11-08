/* Screenshots: Page Down saves the picture as shown as a PNG in the game folder's `screenshots` folder. See
   screenshot.cpp. */
#pragma once
#include <windows.h>

/* true once for each press of Page Down while the game's window has the focus (and no screenshot is being written) */
bool screenshot_wanted(HWND hwnd);
/* copies the pixels (top-down; BGRA, or RGB565 if rgb565) and writes the PNG on a worker thread; grey (8 bits a pixel,
   the same size), if given, is written beside it as <name>_depth.png (1.1.18: the depth buffer) */
void screenshot_save(const BYTE *bits, int w, int h, int pitch, bool rgb565, const BYTE *grey = nullptr, int grey_pitch = 0);
