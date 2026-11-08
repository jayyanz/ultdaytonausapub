/* Our own DirectDraw ([Proxy] OwnDirectDraw=1): the part of IDirectDraw and IDirectDrawSurface the game uses,
   with system-memory surfaces, presented to the game window by us. See ddraw_own.cpp. */
#pragma once
#include <windows.h>
#include <ddraw.h>
#include "present.h"

struct OwnDDConfig {
    int frame_hash;        /* [Test] FrameHash: log a CRC of the first N presented frames (0 = off) */
    int frame_dump_every;  /* [Test] FrameDumpEvery: write every Nth presented frame as a BMP (0 = off) */
    int frame_dump_max;    /* [Test] FrameDumpMax: stop after this many BMP files */
    DisplayConfig display;
};

/* Callbacks into ddraw_proxy.cpp */
void proxy_on_cooperative_level(HWND hwnd, DWORD flags);

/* Live change of the display settings (from the options screen). */
void own_display_config(const DisplayConfig &d);

HRESULT own_directdraw_create(LPDIRECTDRAW *out, const OwnDDConfig &cfg);

/* The pixels of one of our surfaces (an IDirectDrawSurface pointer the game holds); false if it is not ours
   (OwnDirectDraw=0) or gone. RGB565 (or 555, as the game set), pitch in bytes. */
bool own_surface_bits(const void *surface, const BYTE **bits, int *w, int *h, int *pitch);

/* Frame capture shared by both paths: the frame about to be shown (top-down RGB565). */
void frame_capture(const BYTE *bits, int w, int h, int pitch);
void frame_capture_config(const OwnDDConfig &cfg);
