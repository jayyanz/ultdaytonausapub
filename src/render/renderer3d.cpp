/* The new renderer in the proxy (Release 2, R2-3). See renderer3d.h. */
#include "renderer3d.h"
#include "sky.h"
#include "persp.h"
#include "list_flush.h"
#include "proxy.h"
#include "../present.h"
#include <string.h>
#include <string>

namespace {

constexpr DWORD kFlushCall = 0x45523E, kListFlush = 0x455650;
/* R2-8: the game's depths that become ReShade's linear 0 and 1, in the game's camera units. Measured 8 Nov 2026 under
   Wine (Beginner, a race): the corners' depths run from about 126,000 (the road at the bottom of the view) and
   190,000 (the player's car) through 300,000 to 1,000,000 (the course around it) to the horizon, about
   10,000,000 at the game's own draw distance and 22,000,000 with MAX (1.1.21). 1.1.19: ReShade's linear depth is the
   game's depth / 30,000,000, which puts the car about
   6 to 8 of ReShade's units away (ReShade's far plane 1000), as in a usual third-person game, so depth effects' default
   radii fit (1.1.18's / 10,000,000 put it at 25 and MXAO showed nothing); the horizon at a third. */
constexpr float kDepthNear = 0.0f, kDepthFar = 30000000.0f;

BYTE *g_game;
Renderer3D g_mode = Renderer3D::Original;
SeeThroughLook g_look = SeeThroughLook::Textured;
int g_res = 0;                 /* Resolution3D: 0 the display's own, 1 640x400, 2..4 times that */
int g_filter = 3;              /* TextureFilter: 0 nearest, 1 sharp bilinear, 2 bilinear, 3 trilinear, 4 anisotropic */
int g_msaa = 4;                /* Antialiasing: coverage samples per pixel, 1 (off), 2, 4 or 8 */
int g_msaa_used = -1;          /* as last logged */
int g_mapping = 0;             /* R2-7 Mapping: 0 PC, 1 Saturn, 2 perspective-correct */
bool g_persp = false;          /* the emitter hooks are in */
bool g_depth = false;          /* R2-8 DepthBuffer: the depth buffer for ReShade */
int g_depth_state = -1;        /* as last logged: 0 off, 1 on, 2 not available */
int g_hud_pct = 100;            /* HudScale: the race HUD's size, 100, 75 or 50 (whole multiples of its pixels) */
int g_hud_filter = 1;           /* HudFilter: 0 nearest, 1 sharp bilinear, 2 bilinear */
ID3D11Device *g_shared = nullptr;   /* the presenter's device the renderer was made on, if any */
int g_last_w = -1, g_last_h = -1;
gpulist::Renderer *g_r;
bool g_failed;               /* could not start or lost its device: original path until the setting changes */
unsigned g_frames, g_fallbacks;

void __cdecl flush_hook() { renderer3d_list_flush(false); }

/* R2-9: the presenter's device was lost: everything on it goes now; the next frame starts the renderer again on the
   new device */
void device_lost()
{
    present_set_layer(nullptr, nullptr, nullptr);
    if (g_r) { plog("new renderer: released for the new display device"); delete g_r; g_r = nullptr; }
    g_shared = nullptr; g_last_w = g_last_h = -1; g_depth_state = g_depth ? -1 : 0;
}
void game_flush() { ((void (__cdecl *)(void))(g_game + (kListFlush - 0x400000)))(); }

/* R2-5: on the presenter's device once it exists (its own device, at 640x400 only, with the GDI presenter); the
   game's own drawing until the first frame has been shown. */
bool ensure_renderer()
{
    if (g_failed) return false;
    const PresentKind kind = present_kind();
    if (kind == PresentKind::None) return false;
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    const bool shared = kind == PresentKind::D3D11 && present_device(&dev, &ctx);
    if (g_r && (shared ? dev != g_shared : g_shared != nullptr)) {   /* the presenter was made again */
        present_set_layer(nullptr, nullptr, nullptr);
        delete g_r; g_r = nullptr;
    }
    if (g_r) return true;
    std::string err;
    g_r = new gpulist::Renderer;
    if (!g_r->init(err, true, shared ? dev : nullptr, shared ? ctx : nullptr)) {
        plog("new renderer: cannot start (%s); the original renderer is used", err.c_str());
        delete g_r; g_r = nullptr; g_failed = true;
        return false;
    }
    g_shared = shared ? dev : nullptr;
    g_last_w = g_last_h = -1;
    plog("new renderer: started (Direct3D 11, %s device)", shared ? "the presenter's" : g_r->driver());
    return true;
}

/* The high-resolution target for this frame, from Resolution3D (only on the presenter's device). */
void size_hires()
{
    int w = 0, h = 0;
    if (g_shared && g_res != 1) {
        if (g_res == 0) { present_picture_size(w, h); if (w < 640 || h < 400) w = h = 0; }
        else { w = 640 * g_res; h = 400 * g_res; }
    }
    if (w == g_last_w && h == g_last_h) return;
    if (!g_r->set_hires(w, h)) { plog("new renderer: no %dx%d target; 640x400 only", w, h); w = h = 0; g_r->set_hires(0, 0); }
    else if (w) plog("new renderer: 3D at %dx%d", w, h);
    else plog("new renderer: 3D at 640x400");
    g_last_w = w; g_last_h = h;
}

}  // namespace

void renderer3d_list_flush(bool original)
{
    if (original || g_mode != Renderer3D::New || !ensure_renderer()) { present_set_layer(nullptr, nullptr, nullptr); game_flush(); return; }
    size_hires();
    g_r->set_filter(g_filter);
    g_r->set_verify_textures(!r2flush::race_screen());   /* 1.1.17: menus reuse texture places (the car select) */
    {   /* 1.1.9: the sky at high resolution, from the game's copies this frame */
        const sky::Quad *q = nullptr; int n = 0; const uint32_t *art = nullptr; uint32_t ver = 0;
        if (g_r->hires_width() && sky::take(&q, &n, &art, &ver)) g_r->set_sky(q, n, art, ver);
        else g_r->set_sky(nullptr, 0, nullptr, 0);
    }
    if (g_r->hires_width()) {
        g_r->set_mapping(g_mapping == 2 && !g_persp ? 0 : g_mapping);
        {   /* R2-8: the presenter's depth buffer (once a frame with the 3D layer has been shown), filled with this frame's depths */
            ID3D11DepthStencilView *dsv = nullptr; float rect[4] = { 0, 0, 0, 0 };
            const bool have = g_depth && g_persp && present_depth(&dsv, rect);
            const bool ok = g_r->set_depth(have ? dsv : nullptr, rect, kDepthNear, kDepthFar);
            const int state = !g_depth ? 0 : !g_persp || (have && !ok) ? 2 : have ? 1 : g_depth_state;
            if (state != g_depth_state) {
                if (state == 1) plog("depth buffer: on (ReShade's linear depth: the game's depth / %.0f)", kDepthFar);
                else if (state == 2) plog("depth buffer: not available (%s)", !g_persp ? "no captured depths" : g_r->depth_error() ? g_r->depth_error() : "no depth shader");
                else if (state == 0 && g_depth_state > 0) plog("depth buffer: off");
                g_depth_state = state;
            }
        }
        const int used = g_r->set_msaa(g_msaa);
        if (used != g_msaa_used) {
            g_msaa_used = used;
            if (used > 1) plog("new renderer: antialiasing %dx", used);
            else plog("new renderer: antialiasing off");
        }
    }
    if (r2flush::flush(*g_r, g_look == SeeThroughLook::Outline)) {
        g_frames++;
        if (g_persp && (g_frames == 400 || g_frames == 4000)) {   /* once early, once later: the hooks at work */
            unsigned c, n, chk, m; persp::stats(c, n, chk, m);
            if (chk) plog("perspective: %u records' depths captured, %u cut at the near plane; corners checked %u, matching %u", c, n, chk, m);
            else plog("perspective: %u records' depths captured, %u cut at the near plane", c, n);
            float zn, z0, z1;
            if (persp::range(zn, z0, z1)) plog("perspective: depths from %.2f to %.1f", z0, z1);
            (void)zn;
        }
        if (g_r->hires_width()) {
            present_set_layer(g_r->hires_view(), g_r->picture_view(), g_r->background_view());
            /* the HUD step: on the race screens the presenter draws the race HUD itself, scaled */
            const bool incar = *(const uint16_t *)(g_game + (0x4D5154 - 0x400000)) == 1;   /* view word: 1 the in-car view */
            present_set_hud(r2flush::race_screen() ? g_r->tail_view() : nullptr, g_hud_pct / 100.0f, g_hud_filter, incar);
        }
        else present_set_layer(nullptr, nullptr, nullptr);
        return;
    }
    present_set_layer(nullptr, nullptr, nullptr);
    /* device trouble: this frame and the following ones by the game, until the setting is changed */
    g_fallbacks++;
    plog("new renderer: drawing failed after %u frames; the original renderer is used", g_frames);
    delete g_r; g_r = nullptr; g_failed = true;
    game_flush();
}

bool renderer3d_install(BYTE *game_base, Renderer3D mode, SeeThroughLook look, int resolution, int filter, int msaa, int hud_pct, int hud_filter)
{
    g_game = game_base; g_mode = mode; g_look = look; g_res = resolution; g_filter = filter; g_msaa = msaa;
    renderer3d_hud_scale_set(hud_pct); renderer3d_hud_filter_set(hud_filter);
    BYTE *p = game_base + (kFlushCall - 0x400000);
    LONG rel; memcpy(&rel, p + 1, 4);
    if (p[0] != 0xE8 || kFlushCall + 5 + rel != kListFlush) {
        /* the render capture (debug build) already owns the end of frame and calls renderer3d_list_flush */
        plog("new renderer: list flush call already taken over (render capture)");
    } else {
        BYTE code[5]; code[0] = 0xE8;
        LONG r = (LONG)((DWORD)(ULONG_PTR)fnptr(flush_hook) - (kFlushCall + 5)); memcpy(code + 1, &r, 4);
        DWORD old; VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p, code, 5);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    sky::install(game_base);
    g_persp = persp::install(game_base);
    present_set_lost_handler(device_lost);
    plog("new renderer: installed, %s", mode == Renderer3D::New ? "on (Renderer3D=New)" : "off (Renderer3D=Original)");
    return true;
}

Renderer3D renderer3d_get() { return g_mode; }

SeeThroughLook renderer3d_seethrough_get() { return g_look; }

int renderer3d_resolution_get() { return g_res; }

int renderer3d_filter_get() { return g_filter; }

int renderer3d_msaa_get() { return g_msaa; }

int renderer3d_mapping_get() { return g_mapping; }

bool renderer3d_depth_get() { return g_depth; }

void renderer3d_depth_set(bool on)
{
    present_set_depth(on);
    if (on == g_depth) return;
    g_depth = on;
    if (!on && g_r) { float r[4] = { 0, 0, 0, 0 }; g_r->set_depth(nullptr, r, 0, 0); }
}

void renderer3d_mapping_set(int m)
{
    m = m >= 0 && m <= 2 ? m : 0;
    if (m == g_mapping) return;
    g_mapping = m;
    static const char *const names[3] = { "PC", "Saturn", "perspective-correct" };
    plog("new renderer: texture mapping %s%s", names[m], m == 2 && !g_persp ? " (not available: the PC mapping is used)" : "");
}

int renderer3d_hud_scale_get() { return g_hud_pct; }

void renderer3d_hud_scale_set(int pct) { g_hud_pct = pct < 63 ? 50 : pct < 88 ? 75 : 100; }   /* older 85, 70: 75; 55: 50 */

int renderer3d_hud_filter_get() { return g_hud_filter; }

void renderer3d_hud_filter_set(int f) { g_hud_filter = f < 0 ? 0 : f > 2 ? 2 : f; }

void renderer3d_msaa_set(int samples) { g_msaa = samples >= 8 ? 8 : samples >= 4 ? 4 : samples >= 2 ? 2 : 1; }

void renderer3d_filter_set(int f)
{
    if (f == g_filter) return;
    g_filter = f;
    static const char *const names[5] = { "nearest", "sharp bilinear", "bilinear", "trilinear", "anisotropic" };
    plog("new renderer: texture filter %s", names[f >= 0 && f <= 4 ? f : 3]);
}

void renderer3d_resolution_set(int res)
{
    if (res == g_res) return;
    g_res = res;
    plog("new renderer: resolution %s", res == 0 ? "the display's" : res == 1 ? "640x400" : res == 2 ? "2x" : res == 3 ? "3x" : "4x");
}

void renderer3d_seethrough_set(SeeThroughLook look)
{
    if (look == g_look) return;
    g_look = look;
    plog("new renderer: SeeThrough look %s", look == SeeThroughLook::Outline ? "outline" : "textured");
}

void renderer3d_set(Renderer3D mode)
{
    if (mode == g_mode) return;
    g_mode = mode; g_failed = false;   /* a change of setting tries again */
    plog("new renderer: %s", mode == Renderer3D::New ? "on" : "off");
}

bool renderer3d_active() { return g_mode == Renderer3D::New && !g_failed; }

void renderer3d_shutdown()
{
    if (g_frames || g_fallbacks) plog("new renderer: %u frames drawn, %u fallbacks", g_frames, g_fallbacks);
    if (g_persp) {
        unsigned c, n, chk, m; persp::stats(c, n, chk, m);
        if (chk) plog("perspective: %u records' depths captured, %u cut at the near plane; corners checked %u, matching %u", c, n, chk, m);
        else plog("perspective: %u records' depths captured, %u cut at the near plane", c, n);
        float zn, z0, z1;
        if (persp::range(zn, z0, z1)) plog("perspective: depths from %.2f to %.1f", z0, z1);
            (void)zn;
    }
    present_set_layer(nullptr, nullptr, nullptr);
    delete g_r; g_r = nullptr;
}
