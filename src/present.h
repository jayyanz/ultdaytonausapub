/* Showing the game's frames on screen. See present_d3d11.cpp. */
#pragma once
#include <windows.h>

enum class Scaling { SharpBilinear, Nearest, Integer };
enum class Picture { Frame, Fill, Native };   /* Native: the 640x400 picture at 16:10 with square pixels, as large as
                                           the screen allows (default, jayyanz 31 Oct 2026).
                                           Fill: the 640x400 picture stretched to fill 4:3, as a 4:3 CRT showed a 640x400 mode.
                                           Frame: the whole 640x480 frame at 4:3, with the game's own bars. */

struct DisplayConfig {
    bool windowed = false;    /* [Display] Mode=Windowed; default Fullscreen (borderless, whole monitor) */
    int window_scale = 2;     /* [Display] WindowScale: windowed client size = 640x480 times this (limited to the screen) */
    Scaling scaling = Scaling::SharpBilinear;
    Picture picture = Picture::Native;
    int monitor = 0;          /* [Display] Monitor: 0 = the one the game starts on, 1.. = that display */
    bool pause_in_background = true;   /* [Display] PauseInBackground: the game's own pause when it loses focus */
    bool vsync = true;
    bool d3d11 = true;        /* [Display] Renderer=D3D11 (default) or GDI */
};

class Presenter {
public:
    virtual ~Presenter() {}
    /* bits: top-down RGB565. picture_h: rows of real picture in the middle of the frame (400 for the game's 640x480 frames). */
    virtual void present(HWND hwnd, const BYTE *bits, int w, int h, int pitch, bool vsync) = 0;
    virtual const char *name() const = 0;
    virtual void set_config(const DisplayConfig &c) = 0;   /* live change of scaling, picture, vsync */
};

Presenter *make_d3d11_presenter(HWND hwnd, const DisplayConfig &cfg);   /* nullptr if Direct3D 11 is not available */
Presenter *make_gdi_presenter(const DisplayConfig &cfg);

/* Destination rectangle of the picture inside a client area, and the source rows to show. */
struct Placement { int x, y, w, h; int src_y, src_h; };
Placement place_picture(int client_w, int client_h, int frame_w, int frame_h, const DisplayConfig &cfg);
/* The picture mode in force (kept for callers; every mode is fixed now). */
Picture resolve_picture(HWND hwnd, const DisplayConfig &cfg);
