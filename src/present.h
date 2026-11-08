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
    /* R2-9: the device was removed or reset (driver update or crash, GPU switch): the owner makes a new presenter */
    virtual bool lost() const { return false; }
};

Presenter *make_d3d11_presenter(HWND hwnd, const DisplayConfig &cfg);   /* nullptr if Direct3D 11 is not available */

/* Release 2 (R2-5): the new renderer draws on the presenter's own Direct3D 11 device and hands it a high-resolution
   layer to show under the game's HUD. */
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
enum class PresentKind { None, Gdi, D3D11 };       /* none yet: the first frame has not been shown */
PresentKind present_kind();
bool present_device(ID3D11Device **dev, ID3D11DeviceContext **ctx);   /* the D3D11 presenter's (not AddRef'd) */
/* The size the 640x400 picture is shown at, as of the last frame (0 x 0 before the first). */
void present_picture_size(int &w, int &h);
/* hires: the 3D at high resolution (BGRA, premultiplied, alpha 0 where nothing was drawn); picture: the 640x400 picture
   as the new renderer read it back, before the game drew its HUD (R16_UINT). Where the frame shown differs from
   picture (the HUD and anything drawn later) the frame wins; elsewhere hires is shown over background (the 640x400
   picture before the 3D: sky and 2D, R16_UINT), or over the frame inside solid 3D, where the game's own pixel fills any
   hairline crack. All nullptr: off. */
void present_set_layer(ID3D11ShaderResourceView *hires, ID3D11ShaderResourceView *picture, ID3D11ShaderResourceView *background);
/* R2-5 HUD step: on race screens, the picture after the flush's tail (R16_UINT, nullptr: off); the presenter then draws
   the race HUD itself, each group scaled towards its corner or edge and filtered (0 nearest, 1 sharp bilinear,
   2 bilinear), over the 3D. Needs present_set_layer's views. scale: HUD SCALE as a fraction (1, 0.75, 0.5); the HUD
   is drawn at a whole number of screen pixels for each of its own pixels, that share of the picture's scale rounded
   (at least 1, at most the picture's scale). */
/* incar (1.1.17): the in-car view; its dashboard and course map stay at the game's size */
void present_set_hud(ID3D11ShaderResourceView *tail, float scale, int filter, bool incar = false);
Presenter *make_gdi_presenter(const DisplayConfig &cfg);

/* R2-6: the CRT filter (Direct3D 11 only): DOSBox Staging's crt-hyllian shader with its arcade presets, picked as its
   crt-auto-arcade does by the screen pixels a CRT line has. 0 OFF; 1 AUTO (the picture's width on screen by its 400
   lines, off under 3 pixels a line, as DOSBox Staging); 2 SATURN (320x200); 3 PC (640x400); 4 800P (1280x800). */
void present_set_crt(int mode);
int present_crt();

/* R2-8 (1.1.18): a depth buffer for ReShade (Direct3D 11, the new renderer at high resolution). The presenter keeps it
   at the screen's size (D24S8, typeless) and says where the 3D lies in it (x, y, w, h in screen pixels, as of the
   last frame shown); the new renderer clears it and fills it each frame. false: none (off, not yet shown, or no 3D
   layer). A screenshot also saves the depth as <name>_depth.png. */
struct ID3D11DepthStencilView;
/* R2-9: called before a lost presenter is destroyed, so everything made on its device (the new renderer) is released
   first and the new device starts with the old one gone */
void present_set_lost_handler(void (*fn)());
void present_device_lost();   /* for the owner: calls the handler */
void present_set_depth(bool on);
bool present_depth_on();
bool present_depth(ID3D11DepthStencilView **dsv, float rect[4]);

/* Destination rectangle of the picture inside a client area, and the source rows to show. */
struct Placement { int x, y, w, h; int src_y, src_h; };
Placement place_picture(int client_w, int client_h, int frame_w, int frame_h, const DisplayConfig &cfg);
/* The picture mode in force (kept for callers; every mode is fixed now). */
Picture resolve_picture(HWND hwnd, const DisplayConfig &cfg);
