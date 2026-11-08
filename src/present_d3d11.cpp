/* Presenting frames: Direct3D 11 (default) or GDI (fallback).
 *
 * Each frame the game shows (RGB565, normally 640x480 with the picture in the middle 400 rows) is converted to
 * BGRA, uploaded to a texture and drawn into the window's swap chain with one full-screen triangle. The pixel
 * shader does the scaling: nearest, sharp-bilinear (nearest-looking but without uneven pixel widths, smoothing only
 * the edges between source pixels) or integer (nearest, at the largest whole multiple that fits).
 *
 * The swap chain uses the flip model where Windows supports it (Windows 10 and 11), with one frame of latency, and
 * presents with vsync when the game flips, as a full-screen DirectDraw flip did. Direct3D 11 and the shader compiler
 * are loaded at run time, so a missing piece falls back to GDI instead of stopping the game.
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <vector>
#include "proxy.h"
#include "present.h"
#include "screenshot.h"

Picture resolve_picture(HWND hwnd, const DisplayConfig &cfg)
{
    (void)hwnd;
    return cfg.picture;
}

Placement place_picture(int cw, int ch, int fw, int fh, const DisplayConfig &cfg)
{
    Placement p{};
    int ph = fh, py = 0;
    if (cfg.picture != Picture::Frame && fw == 640 && fh == 480) { ph = 400; py = 40; }   /* the picture rows only */
    p.src_y = py; p.src_h = ph;
    if (cw <= 0 || ch <= 0) return p;
    int w, h;
    if (cfg.picture == Picture::Native) {
        /* square pixels at the source's own shape (640x400: 16:10) */
        int k = cw / fw < ch / ph ? cw / fw : ch / ph;
        if (cfg.scaling == Scaling::Integer && k >= 1) { w = fw * k; h = ph * k; }
        else { w = cw; h = (int)((long long)cw * ph / fw); if (h > ch) { h = ch; w = (int)((long long)ch * fw / ph); } }
        p.x = (cw - w) / 2; p.y = (ch - h) / 2; p.w = w; p.h = h;
        return p;
    }
    /* Frame (640x480, square pixels) and Fill (640x400 stretched): a 4:3 area */
    if (cfg.scaling == Scaling::Integer) {
        int k = cw / fw < ch / ph ? cw / fw : ch / ph;
        if (cfg.picture == Picture::Fill) {           /* whole multiples of the 400 rows; width follows 4:3 */
            k = ch / ph;
            while (k > 1 && ph * k * 4 / 3 > cw) k--;
        }
        if (k >= 1) {
            h = ph * k;
            w = (cfg.picture == Picture::Fill) ? h * 4 / 3 : fw * k;
            p.x = (cw - w) / 2; p.y = (ch - h) / 2; p.w = w; p.h = h;
            return p;
        }
    }
    w = cw; h = cw * 3 / 4;
    if (h > ch) { h = ch; w = ch * 4 / 3; }
    p.x = (cw - w) / 2; p.y = (ch - h) / 2; p.w = w; p.h = h;
    return p;
}

namespace {

/* ---------- GDI ---------- */

struct Bmi565 { BITMAPINFOHEADER h; DWORD masks[3]; };

class GdiPresenter final : public Presenter {
public:
    explicit GdiPresenter(const DisplayConfig &c) : cfg_(c) {}
    const char *name() const override { return "GDI"; }
    void present(HWND hwnd, const BYTE *bits, int w, int h, int pitch, bool) override
    {
        RECT cr; GetClientRect(hwnd, &cr);
        DisplayConfig c = cfg_; c.picture = resolve_picture(hwnd, cfg_);
        Placement p = place_picture(cr.right, cr.bottom, w, h, c);
        if (p.w <= 0) return;
        if (screenshot_wanted(hwnd)) screenshot_save(bits + (size_t)p.src_y * pitch, w, p.src_h, pitch, true);   /* the game's picture */
        Bmi565 b{};
        b.h.biSize = sizeof b.h; b.h.biWidth = pitch / 2; b.h.biHeight = -p.src_h; b.h.biPlanes = 1;
        b.h.biBitCount = 16; b.h.biCompression = BI_BITFIELDS;
        b.masks[0] = 0xF800; b.masks[1] = 0x07E0; b.masks[2] = 0x001F;
        HDC dc = GetDC(hwnd);
        if (!dc) return;
        SetStretchBltMode(dc, cfg_.scaling == Scaling::SharpBilinear ? HALFTONE : COLORONCOLOR);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, p.x, p.y, p.w, p.h, 0, 0, w, p.src_h, bits + p.src_y * pitch, (BITMAPINFO *)&b, DIB_RGB_COLORS, SRCCOPY);
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bars[4] = { { 0, 0, cr.right, p.y }, { 0, p.y + p.h, cr.right, cr.bottom },
                         { 0, p.y, p.x, p.y + p.h }, { p.x + p.w, p.y, cr.right, p.y + p.h } };
        for (RECT &r : bars) if (r.right > r.left && r.bottom > r.top) FillRect(dc, &r, black);
        ReleaseDC(hwnd, dc);
    }
private:
    DisplayConfig cfg_;
public:
    void set_config(const DisplayConfig &c) override { cfg_ = c; }
};

/* ---------- Direct3D 11 ---------- */

const char kShader[] = R"(
Texture2D frame : register(t0);
Texture2D hires : register(t1);           // R2-5: the new renderer's 3D at high resolution (premultiplied)
Texture2D<uint> picture : register(t2);   // R2-5: its 640x400 picture before the HUD (RGB565)
Texture2D<uint> background : register(t3); // R2-5: the 640x400 picture before the 3D (sky, 2D; RGB565)
Texture2D<uint> tailpic : register(t4);    // R2-5 HUD step: the 640x400 picture after the flush's tail (RGB565)
Texture2D laprows : register(t5);          // 1.1.13: 1 for the rows of the lap time list below row 96 (lap_main)
SamplerState point_s : register(s0);
SamplerState linear_s : register(s1);
cbuffer params : register(b0) {
    float2 tex_size;    // texture size in texels
    float2 src_origin;  // first texel shown
    float2 src_size;    // texels shown
    float2 out_size;    // viewport size in pixels
    int mode;           // 0 sharp-bilinear, 1 nearest
    int layered;        // R2-5: hires and picture are bound
    int pic_y0;         // the picture's first row in the frame
    int pad;
    float2 hud_scale;   // R2-5 HUD step: the race HUD's size against the game's, across and down (a whole number of
                        // screen pixels for each of the HUD's own pixels, over the picture's scale)
    int hud_on;         // the race HUD drawn by hud() (tailpic bound; race screens)
    int hud_filter;     // 0 nearest, 1 sharp bilinear, 2 bilinear
    int hud_incar;      // 1.1.17: the in-car view: its dashboard (drawn with the messages) and the course map in it at
                        // the game's size, so the map stays in its place on the dashboard
    int3 pad3;
};
float3 rgb565(uint pv)
{
    uint3 ci = uint3((pv >> 11) & 31, (pv >> 5) & 63, pv & 31);
    return float3((ci.x << 3) | (ci.x >> 2), (ci.y << 2) | (ci.y >> 4), (ci.z << 3) | (ci.z >> 2)) / 255.0;
}
bool drew(int2 p)   // the game drew 3D on this 640x400 pixel
{
    if (p.x < 0 || p.y < 0 || p.x >= 640 || p.y >= 400) return false;
    return picture.Load(int3(p, 0)) != background.Load(int3(p, 0));
}
float3 under(float2 s)   // the picture before the 3D, at frame texel position s (as the frame is sampled)
{
    float2 q = float2(s.x, s.y - pic_y0) - 0.5;
    int2 i = (int2)floor(q);
    float2 f = mode == 1 ? float2(0, 0) : q - floor(q);
    if (mode == 1) i = (int2)floor(float2(s.x, s.y - pic_y0));
    int2 i0 = clamp(i, int2(0, 0), int2(639, 399)), i1 = clamp(i + 1, int2(0, 0), int2(639, 399));
    float3 a = rgb565(background.Load(int3(i0.x, i0.y, 0))), b = rgb565(background.Load(int3(i1.x, i0.y, 0)));
    float3 c = rgb565(background.Load(int3(i0.x, i1.y, 0))), d = rgb565(background.Load(int3(i1.x, i1.y, 0)));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
/* The race HUD (R2-5 HUD step). A HUD pixel is one the frame has and the picture before the HUD has not. Its group:
   drawn by the flush's tail (needle, CONDITION panel, course map; tailpic differs from picture) by where it is, or else
   (the overlay queues: LAP, time, speed, rev counter, POSITION, messages) by where it is; banners sliding across the
   middle are never tail, so they never join the course map. Each group is drawn scaled towards its anchor. */
static const float2 kAnchor[6] = { float2(0, 0), float2(0, 0), float2(320, 0), float2(640, 0), float2(640, 400), float2(320, 200) };
static const float4 kRect[6] = { float4(0, 0, 0, 0), float4(0, 0, 200, 230), float4(180, 0, 480, 140), float4(440, 0, 640, 200),
                                 float4(440, 200, 640, 400), float4(0, 0, 640, 400) };
int group_of(int2 i)
{
    if (i.x < 0 || i.y < 0 || i.x >= 640 || i.y >= 400) return 0;
    uint pv = picture.Load(int3(i, 0));
    uint3 ci = uint3((pv >> 11) & 31, (pv >> 5) & 63, pv & 31);
    ci = uint3((ci.x << 3) | (ci.x >> 2), (ci.y << 2) | (ci.y >> 4), (ci.z << 3) | (ci.z >> 2));
    uint3 fi = (uint3)round(frame.Load(int3(i.x, i.y + pic_y0, 0)).rgb * 255.0);
    if (all(fi == ci)) return 0;                                   // not HUD
    if (tailpic.Load(int3(i, 0)) != pv) return i.x >= 440 ? (i.y < 200 ? 3 : 4) : 2;   // tail: panel, map; needle
    if (i.x < 200) {   // LAP and the lap times (one row more each lap, down to row 169 on 8 laps); the GO board below them a message
        if (i.y < 96) return 1;
        return i.y < 230 && laprows.Load(int3(i.y, 0, 0)).r > 0.5 ? 1 : 5;
    }
    if (i.y < 125) return i.x < 470 ? 2 : 3;                       // speed and rev counter; POSITION
    if (i.x >= 470 && i.y < 200) return 3;                         // the CONDITION label
    return 5;                                                      // messages and banners
}
float4 hud_texel(int2 i, int g) { return group_of(i) == g ? float4(frame.Load(int3(i.x, i.y + pic_y0, 0)).rgb, 1) : float4(0, 0, 0, 0); }
float4 hud(float2 P, float2 k1)   // the HUD at picture position P (texels), premultiplied; k1: screen pixels a texel
{
    float4 acc = float4(0, 0, 0, 0);
    [unroll] for (int g = 1; g <= 5; g++) {
        float2 sc = hud_incar != 0 && g >= 4 ? float2(1, 1) : hud_scale;
        float2 k = max(k1 * sc, 1.0);
        float2 q = kAnchor[g] + (P - kAnchor[g]) / sc;
        float4 r = kRect[g];
        if (q.x < r.x - 1 || q.y < r.y - 1 || q.x > r.z + 1 || q.y > r.w + 1) continue;
        float4 c;
        if (hud_filter == 0) c = hud_texel((int2)floor(q), g);
        else {
            float2 b = floor(q - 0.5), f = q - 0.5 - b;
            if (hud_filter == 1) f = saturate((f - 0.5) * k + 0.5);
            int2 i = (int2)b;
            c = lerp(lerp(hud_texel(i, g), hud_texel(i + int2(1, 0), g), f.x),
                     lerp(hud_texel(i + int2(0, 1), g), hud_texel(i + int2(1, 1), g), f.x), f.y);
        }
        acc = acc * (1.0 - c.a) + c;
    }
    return acc;
}
float4 layer(float4 c, float2 texel, float2 s)
{
    int2 ft = (int2)floor(texel);
    int py = ft.y - pic_y0;
    if (layered == 0 || py < 0 || py >= 400 || ft.x < 0 || ft.x >= 640) return c;
    uint pv = picture.Load(int3(ft.x, py, 0));
    uint3 ci = uint3((pv >> 11) & 31, (pv >> 5) & 63, pv & 31);
    ci = uint3((ci.x << 3) | (ci.x >> 2), (ci.y << 2) | (ci.y >> 4), (ci.z << 3) | (ci.z >> 2));
    uint3 fi = (uint3)round(frame.Load(int3(ft, 0)).rgb * 255.0);
    if (hud_on == 0 && any(fi != ci)) return c;   // the HUD, or anything drawn after the 3D: the frame as it is
    float4 h = hires.Sample(linear_s, float2(texel.x / 640.0, (texel.y - pic_y0) / 400.0));
    /* under the high-resolution 3D: the picture before the 3D (sky, background), so no blocky 640x400 3D pixels show
       at its edges and through see-through texels; only inside solid 3D (this pixel and its four neighbours drawn)
       the game's own 3D pixel, which fills any hairline crack */
    int2 p = int2(ft.x, py);
    bool inner = drew(p) && drew(p + int2(-1, 0)) && drew(p + int2(1, 0)) && drew(p + int2(0, -1)) && drew(p + int2(0, 1));
    if (hud_on != 0) {   /* the race HUD scaled: the 3D everywhere (the picture's own pixel in cracks), the HUD over it */
        float3 u = inner ? float3(ci) / 255.0 : under(s);
        float3 base = u * (1.0 - h.a) + h.rgb;
        float4 hd = hud(float2(texel.x, texel.y - pic_y0), out_size / src_size);
        return float4(base * (1.0 - hd.a) + hd.rgb, 1);
    }
    float3 u = inner ? c.rgb : under(s);
    return float4(u * (1.0 - h.a) + h.rgb, 1);
}
/* lap_main (1.1.13, 1.1.14): which rows from 96 to 229 belong to the lap time list. The list starts at row 71 and
   each lap adds a row 14 lines lower (the third lap's from row 99, the eighth's from row 169; jayyanz's 1.1.13
   screenshot), each starting with the lap number at x 27; nothing else of the HUD is drawn at x 20 to 60 there (the
   GO board starts at x 115). A row is the list's if a HUD pixel that is not the tail's is within 5 rows of it at x 20
   to 60, and of each row 14, 28, ... above it down to row 96, so a banner passing the left edge lower down is not
   taken unless the list reaches it. Drawn into a 400x1 target, x = the row. */
bool overlay_hud(int2 i)
{
    uint pv = picture.Load(int3(i, 0));
    uint3 ci = uint3((pv >> 11) & 31, (pv >> 5) & 63, pv & 31);
    ci = uint3((ci.x << 3) | (ci.x >> 2), (ci.y << 2) | (ci.y >> 4), (ci.z << 3) | (ci.z >> 2));
    uint3 fi = (uint3)round(frame.Load(int3(i.x, i.y + pic_y0, 0)).rgb * 255.0);
    return any(fi != ci) && tailpic.Load(int3(i, 0)) == pv;
}
bool lap_band(int y)
{
    [loop] for (int dy = -5; dy <= 5; dy++)
        [loop] for (int x = 20; x <= 60; x++)
            if (overlay_hud(int2(x, y + dy))) return true;
    return false;
}
float4 lap_main(float4 pos : SV_Position) : SV_Target {
    int y = (int)pos.x;
    if (y < 96 || y >= 230 || hud_on == 0) return 0;
    [loop] for (int yy = y; yy >= 96; yy -= 14)
        if (!lap_band(yy)) return 0;
    return 1;
}
struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
vs_out vs_main(uint id : SV_VertexID) {
    vs_out o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_main(vs_out i) : SV_Target {
    float2 texel = src_origin + i.uv * src_size;
    if (mode == 1)
        return layer(float4(frame.Sample(point_s, texel / tex_size).rgb, 1), texel, texel);
    float2 scale = max(out_size / src_size, 1.0);
    float2 base = floor(texel - 0.5) + 0.5;
    float2 f = texel - 0.5 - floor(texel - 0.5);
    f = saturate((f - 0.5) * scale + 0.5);
    return layer(float4(frame.Sample(linear_s, (base + f) / tex_size).rgb, 1), texel, base + f);
}
)";

/* R2-6: the CRT pass, a port of DOSBox Staging's CRT shader (resources/shaders/crt/crt-hyllian.glsl, "Hyllian's CRT
   Shader": SPDX-FileCopyrightText 2020-2026 The DOSBox Staging Team, 2021-2020 Hyllian <sergiogdb@gmail.com>,
   2020-2020 Tyrells; SPDX-License-Identifier: MIT; its mask_weights() by hunterk, public domain; taken from
   dosbox-staging commit 27b1b7ca, 3 Nov 2026) with DOSBox Staging's arcade presets (resources/shader-presets/crt/
   crt-hyllian/arcade-*.preset) and its crt-auto-arcade choice between them. See THIRD-PARTY-NOTICES.md for the licence.
   Changes for this project: GLSL to HLSL (shader model 4, and code Wine's compiler accepts: no local arrays, so the
   three mask layouts the arcade presets use are packed into integers); the preset values are constants; the input is
   the finished picture (the new renderer's 3D at high resolution, the HUD) reduced by ds_main to the CRT's own
   resolution, each of its pixels the average of the screen pixels it covers, in linear light: AUTO the picture's
   width on screen by its 400 lines, SATURN 320x200, PC 640x400, 800P 1280x800 (jayyanz 7 Nov). */
const char kCrtShader[] = R"(
// SPDX-FileCopyrightText:  2020-2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2021-2020 Hyllian <sergiogdb@gmail.com>
// SPDX-FileCopyrightText:  2020-2020 Tyrells
// SPDX-License-Identifier: MIT
// Hyllian's CRT Shader, ported from Libretro's crt-hyllian.glslp to DOSBox by Tyrells; ported to HLSL for ultdaytonausa.
Texture2D img : register(t0);
SamplerState point_s : register(s0);
SamplerState linear_s : register(s1);
cbuffer crt : register(b0) {
    float2 input_size;  // the CRT's resolution: pixels across, lines down
    float2 tex_size;    // the picture at its size on screen
    int phosphor_layout;
    float3 pad;
};
// DOSBox Staging's arcade presets (arcade-1080p, arcade-1440p and arcade-4k differ only in PHOSPHOR_LAYOUT)
static const float BEAM_MIN_WIDTH = 1.00, BEAM_MAX_WIDTH = 1.20, SCANLINES_STRENGTH = 0.75, COLOR_BOOST = 2.00;
static const float HFILTER_PROFILE = 0.0, MASK_INTENSITY = 0.30, CRT_ANTI_RINGING = 1.00;
static const float INPUT_GAMMA = 2.40, OUTPUT_GAMMA = 2.25;
struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

/* mask_weights() (hunterk, public domain), the layouts the arcade presets use. Each row of a layout is packed into
   a uint, 3 bits a pixel: 4 red, 2 green, 1 blue (5 magenta, 0 black). */
float3 weight_of(uint row, int z, float mask_intensity)
{
    uint c = (row >> (3 * z)) & 7;
    return lerp(1. - mask_intensity, 1., float3((c & 4) != 0, (c & 2) != 0, (c & 1) != 0));
}
float3 mask_weights(float2 coord, float mask_intensity, int layout)
{
    if (layout == 11) {         // delta_1_4x1_rgb
        int w = (int)floor(fmod(coord.y, 2.0)), z = (int)floor(fmod(coord.x, 4.0));
        return weight_of(w == 1 ? 0x54u : 0x501u, z, mask_intensity);          // red green blue black | blue black red green
    }
    if (layout == 14) {         // slot mask for RGB panels
        int w = (int)floor(fmod(coord.y, 3.0)), z = (int)floor(fmod(coord.x, 6.0));
        return weight_of(w == 1 ? 0x15u : w == 2 ? 0x2A15u : 0x2A00u, z, mask_intensity);
    }
    if (layout == 17) {         // slot_2_5x4_bgr
        int w = (int)floor(fmod(coord.y, 4.0)), z = (int)floor(fmod(coord.x, 10.0));
        return weight_of(w == 1 || w == 3 ? 0x1236246Cu : w == 2 ? 0x00122448u : 0x12240024u, z, mask_intensity);
    }
    return 1;
}
float4 gamma_in(float4 c) { return pow(c, INPUT_GAMMA); }
/* the CRT's pixel at tc: the input to the shader, already in linear light (GAMMA_IN) */
float4 tap(float2 tc) { return img.SampleLevel(point_s, tc, 0); }
/* ds_main: the picture on screen (tex_size) reduced to the CRT's resolution (input_size): 4 x 4 bilinear samples
   spread over the screen pixels each CRT pixel covers, averaged in linear light */
float4 ds_main(vs_out i) : SV_Target {
    float2 j = floor(i.uv * input_size);
    float2 r = tex_size / input_size;   // screen pixels a CRT pixel covers; under 2, its middle alone (no blur)
    float4 acc = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++) {
            float2 o = (float2(x, y) + 0.5) / 4.0;
            o = float2(r.x < 2 ? 0.5 : o.x, r.y < 2 ? 0.5 : o.y);
            acc += gamma_in(img.SampleLevel(linear_s, (j + o) / input_size, 0));
        }
    return acc / 16.0;
}
vs_out vs_main(uint id : SV_VertexID) {
    vs_out o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_main(vs_out i) : SV_Target {
    // get_hfilter_profile(): B = 0, C = 0 (Hermite) or C = 0.5 (Catmull-Rom); GLSL's mat4 columns
    float bf = 0.0, cf = HFILTER_PROFILE == 1 ? 0.5 : 0.0;
    float4 col0 = float4((-bf - 6.0*cf)/6.0, (3.0*bf + 12.0*cf)/6.0, (-3.0*bf - 6.0*cf)/6.0, bf/6.0);
    float4 col1 = float4((12.0 - 9.0*bf - 6.0*cf)/6.0, (-18.0 + 12.0*bf + 6.0*cf)/6.0, 0.0, (6.0 - 2.0*bf)/6.0);
    float4 col2 = float4(-(12.0 - 9.0*bf - 6.0*cf)/6.0, (18.0 - 15.0*bf - 12.0*cf)/6.0, (3.0*bf + 6.0*cf)/6.0, bf/6.0);
    float4 col3 = float4((bf + 6.0*cf)/6.0, -cf, 0.0, 0.0);

    float scanlines_strength = 4.0 * SCANLINES_STRENGTH;
    float2 TextureSize = input_size;   // SHARPNESS_HACK 1, VSCANLINES off
    float2 dx = float2(1.0 / TextureSize.x, 0.0);
    float2 dy = float2(0.0, 1.0 / TextureSize.y);
    float2 pix_coord = i.uv * TextureSize + float2(-0.5, 0.5);
    float2 tc = (floor(pix_coord) + float2(0.5, 0.5)) / TextureSize;
    float2 fp = frac(pix_coord);

    float4 c00 = tap(tc - dx - dy), c01 = tap(tc - dy), c02 = tap(tc + dx - dy), c03 = tap(tc + 2.0 * dx - dy);
    float4 c10 = tap(tc - dx),      c11 = tap(tc),      c12 = tap(tc + dx),      c13 = tap(tc + 2.0 * dx);

    float4 t = float4(fp.x * fp.x * fp.x, fp.x * fp.x, fp.x, 1.0);
    float4 wx = float4(dot(t, col0), dot(t, col1), dot(t, col2), dot(t, col3));   // invX_Px
    float4 color0 = c00 * wx.x + c01 * wx.y + c02 * wx.z + c03 * wx.w;
    float4 color1 = c10 * wx.x + c11 * wx.y + c12 * wx.z + c13 * wx.w;

    // anti-ringing
    float4 aux = color0;
    color0 = clamp(color0, min(c01, c02), max(c01, c02));
    color0 = lerp(aux, color0, CRT_ANTI_RINGING * step(0.0, (c00 - c01) * (c02 - c03)));
    aux = color1;
    color1 = clamp(color1, min(c11, c12), max(c11, c12));
    color1 = lerp(aux, color1, CRT_ANTI_RINGING * step(0.0, (c10 - c11) * (c12 - c13)));

    float pos0 = fp.y;
    float pos1 = 1.0 - fp.y;
    float4 lum0 = lerp(BEAM_MIN_WIDTH, BEAM_MAX_WIDTH, color0);
    float4 lum1 = lerp(BEAM_MIN_WIDTH, BEAM_MAX_WIDTH, color1);
    float4 d0 = scanlines_strength * pos0 / (lum0 + 0.0000001);
    float4 d1 = scanlines_strength * pos1 / (lum1 + 0.0000001);
    d0 = exp(-d0 * d0);
    d1 = exp(-d1 * d1);
    float4 color = COLOR_BOOST * (color0 * d0 + color1 * d1);

    // mask, at the screen's pixels from the picture's top left
    float2 mask_coords = i.uv * tex_size;   // gl_FragCoord.xy - OUTPUT_START_OFFSET (pixel centres)
    color.rgb *= mask_weights(mask_coords, MASK_INTENSITY, phosphor_layout);

    color = clamp(pow(max(color, 0), 1.0 / OUTPUT_GAMMA), 0.0, 1.0);
    return float4(color.rgb, 1.0);
}
)";

struct CrtParams { float input_size[2], tex_size[2]; int layout; float pad[3]; };

struct Params { float tex_size[2], src_origin[2], src_size[2], out_size[2]; int mode, layered, pic_y0, pad; float hud_scale[2]; int hud_on, hud_filter; int hud_incar, pad3[3]; };

/* R2-5: what the new renderer shares with the presenter */
PresentKind g_kind = PresentKind::None;
ID3D11Device *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
ID3D11ShaderResourceView *g_hires = nullptr, *g_picture = nullptr, *g_background = nullptr;
ID3D11ShaderResourceView *g_tail = nullptr;    /* the HUD step: set on race screens */
float g_hud_frac = 1.0f; int g_hud_filter = 1; bool g_hud_incar = false;     /* HudScale as a fraction (1, 0.75, 0.5) */
int g_crt = 0;                                 /* R2-6: [Display] Crt (0 off, 1 arcade) */
int g_pic_w = 0, g_pic_h = 0;
/* R2-8 (1.1.18): the depth buffer for ReShade, the size of the screen; where the 3D lies in it (screen pixels) */
bool g_depth_on = false;
ID3D11DepthStencilView *g_dsv = nullptr;
float g_depth_rect[4] = { 0, 0, 0, 0 };

template <typename T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

class D3D11Presenter final : public Presenter {
public:
    explicit D3D11Presenter(const DisplayConfig &c) : cfg_(c) {}
    ~D3D11Presenter() override
    {
        release(tex_); release(srv_); release(rtv_); release(cb_); release(ps_); release(vs_);
        release(crt_srv_); release(crt_rtv_); release(crt_tex_); release(crt_cb_); release(crt_ps_);
        release(src_srv_); release(src_rtv_); release(src_tex_); release(ds_ps_);
        release(lap_srv_); release(lap_rtv_); release(lap_tex_); release(lap_ps_);
        release(dsv_); release(depth_tex_);
        release(samp_point_); release(samp_linear_); release(swap_); release(ctx_); release(dev_);
        if (g_dev == dev_) { g_dev = nullptr; g_ctx = nullptr; g_kind = PresentKind::None; g_hires = g_picture = g_background = g_tail = nullptr; g_dsv = nullptr; }
    }
    const char *name() const override { return flip_model_ ? "Direct3D 11 (flip model)" : "Direct3D 11"; }

    bool init(HWND hwnd)
    {
        HMODULE d3d = LoadLibraryA("d3d11.dll");
        if (!d3d) { plog("display: d3d11.dll not found"); return false; }
        PFN_D3D11_CREATE_DEVICE create = nullptr;
        set_fn(create, GetProcAddress(d3d, "D3D11CreateDevice"));
        if (!create) return false;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got;
        HRESULT r = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                           D3D11_SDK_VERSION, &dev_, &got, &ctx_);
        if (FAILED(r)) {
            r = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                       D3D11_SDK_VERSION, &dev_, &got, &ctx_);
            if (FAILED(r)) { plog("display: D3D11CreateDevice failed 0x%08lX", (unsigned long)r); return false; }
            plog("display: no hardware Direct3D 11 device, using WARP (software)");
        }
        if (!make_swap_chain(hwnd)) return false;
        if (!make_shaders()) return false;
        D3D11_SAMPLER_DESC sd{};
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        if (FAILED(dev_->CreateSamplerState(&sd, &samp_point_))) return false;
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        if (FAILED(dev_->CreateSamplerState(&sd, &samp_linear_))) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Params); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cb_))) return false;
        bd.ByteWidth = sizeof(CrtParams);
        if (crt_ps_ && FAILED(dev_->CreateBuffer(&bd, nullptr, &crt_cb_))) release(crt_ps_);
        plog("display: %s, feature level %X.%X", name(), (got >> 12) & 0xF, (got >> 8) & 0xF);
        return true;
    }

    void present(HWND hwnd, const BYTE *bits, int w, int h, int pitch, bool vsync) override
    {
        RECT cr; GetClientRect(hwnd, &cr);
        if (cr.right <= 0 || cr.bottom <= 0 || IsIconic(hwnd)) return;
        if ((int)cr.right != bw_ || (int)cr.bottom != bh_) resize(cr.right, cr.bottom);
        if (!rtv_ || !upload(bits, w, h, pitch)) return;
        DisplayConfig c = cfg_; c.picture = resolve_picture(hwnd, cfg_);
        if (c.picture != last_picture_) { last_picture_ = c.picture; plog("display: picture %s", c.picture == Picture::Fill ? "4:3 (640x400 stretched to 4:3)" : c.picture == Picture::Frame ? "Frame (640x480 with the game's bars)" : "16:10 (640x400, square pixels)"); }
        Placement p = place_picture(bw_, bh_, w, h, c);
        /* The CRT (R2-6). Its resolution: AUTO the picture's width on screen by its own lines (400; 480 with PICTURE
           FRAME); SATURN half the game's, PC the game's (640x400), 800P twice. The preset as DOSBox Staging's
           crt-auto-arcade picks it, by whole screen pixels per CRT line (3 or more 1080p, 5 1440p, 8 4K); under 3,
           AUTO is off (as DOSBox Staging) and the others use the 1080p preset. DOSBox Staging's integer_scaling
           auto: from 3 to 5 pixels a line the picture's height is a whole number of lines (or 3.5, 4.5), same shape. */
        int layout = 0, crt_cols = 0, crt_lines = 0;
        if (g_crt && crt_ps_ && ds_ps_ && crt_cb_ && p.src_h > 0) {
            crt_lines = g_crt == 2 ? p.src_h / 2 : g_crt == 4 ? p.src_h * 2 : p.src_h;
            const int ppl = p.h / crt_lines;
            layout = ppl >= 8 ? 17 : ppl >= 5 ? 14 : ppl >= 3 ? 11 : g_crt == 1 ? 0 : 11;
            const float f = (float)p.h / crt_lines;
            if (layout && f >= 3.0f && f < 5.0f) {
                const float k = floorf(f * 2) / 2;
                const int h2 = (int)(crt_lines * k + 0.5f), w2 = (int)((long long)p.w * h2 / p.h);
                p.x += (p.w - w2) / 2; p.y += (p.h - h2) / 2; p.w = w2; p.h = h2;
            }
            crt_cols = g_crt == 1 ? p.w : g_crt == 2 ? w / 2 : g_crt == 4 ? w * 2 : w;
        }
        if (p.w > 0 && p.src_h > 0) { g_pic_w = p.w * 640 / w; g_pic_h = p.h * 400 / p.src_h; }
        {   /* R2-8: the depth buffer, with the 3D where it is on screen (the game's row 0 of its 400 at pic_y0) */
            const bool layered3d = g_hires && g_picture && g_background && w == 640 && h >= 400 && p.w > 0 && p.src_h > 0;
            if (g_depth_on && layered3d && depth_target()) {
                const float k = (float)p.h / p.src_h;
                g_depth_rect[0] = (float)p.x; g_depth_rect[1] = p.y + ((h == 480 ? 40 : 0) - p.src_y) * k;
                g_depth_rect[2] = (float)p.w; g_depth_rect[3] = 400 * k;
                if (memcmp(depth_logged_, g_depth_rect, sizeof depth_logged_)) {
                    memcpy(depth_logged_, g_depth_rect, sizeof depth_logged_);
                    plog("display: depth buffer for ReShade %dx%d, the 3D at %.0f,%.0f %.0fx%.0f", bw_, bh_, g_depth_rect[0], g_depth_rect[1], g_depth_rect[2], g_depth_rect[3]);
                }
                g_dsv = dsv_;
            } else {
                if (g_dsv) g_dsv = nullptr;
                if (dsv_) ctx_->ClearDepthStencilView(dsv_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);   /* nothing: all far */
                if (!g_depth_on && dsv_) { release(dsv_); release(depth_tex_); }
            }
        }
        const float black[4] = { 0, 0, 0, 1 };
        ctx_->ClearRenderTargetView(rtv_, black);
        if (p.w > 0) {
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                Params *q = (Params *)m.pData;
                const bool layered = g_hires && g_picture && g_background && w == 640 && h >= 400;
                const int pic_y0 = h == 480 ? 40 : 0;
                /* the race HUD at a whole number of screen pixels for each of its own pixels: the preset's share of the
                   picture's scale, rounded, at least 1 and at most the picture's scale (100: as large as fits in the
                   game's own size) */
                const float hs[2] = { (float)p.w / w, (float)p.h / p.src_h };
                float hud_k[2];
                for (int a = 0; a < 2; a++) {
                    const float most = floorf(hs[a]);
                    hud_k[a] = most < 1 ? hs[a] : fminf(fmaxf(floorf(g_hud_frac * hs[a] + 0.5f), 1.0f), most);
                }
                if (layered && g_tail && (hud_k[0] != hud_logged_[0] || hud_k[1] != hud_logged_[1])) {
                    hud_logged_[0] = hud_k[0]; hud_logged_[1] = hud_k[1];
                    plog("display: race HUD at %gx%g screen pixels a HUD pixel (HUD SCALE %d, picture scale %.2f)", hud_k[0], hud_k[1], (int)(g_hud_frac * 100 + 0.5f), hs[1]);
                }
                *q = Params{ { (float)tw_, (float)th_ }, { 0, (float)p.src_y }, { (float)w, (float)p.src_h },
                             { (float)p.w, (float)p.h }, cfg_.scaling == Scaling::SharpBilinear ? 0 : 1, layered ? 1 : 0, pic_y0, 0,
                             { hud_k[0] / hs[0], hud_k[1] / hs[1] }, layered && g_tail ? 1 : 0, g_hud_filter, g_hud_incar ? 1 : 0, { 0, 0, 0 } };
                ctx_->Unmap(cb_, 0);
            }
            const bool crt = layout && crt_target(p.w, p.h, crt_cols, crt_lines);
            const int state = g_crt * 100000 + (crt ? layout * 1000 + crt_lines : crt_lines ? p.h / crt_lines : 0);
            if (g_crt && state != crt_logged_) {
                crt_logged_ = state;
                static const char *const names[5] = { "OFF", "AUTO", "SATURN", "PC", "800P" };
                if (crt) plog("display: CRT %s: %dx%d, preset arcade-%s (%.2f screen pixels a line), picture %dx%d", names[g_crt], crt_cols, crt_lines,
                              layout == 17 ? "4k" : layout == 14 ? "1440p" : "1080p", (float)p.h / crt_lines, p.w, p.h);
                else plog("display: CRT AUTO off for now: the picture has %d screen pixels a line (it needs 3)", crt_lines ? p.h / crt_lines : 0);
            }
            D3D11_VIEWPORT vp{ (float)p.x, (float)p.y, (float)p.w, (float)p.h, 0, 1 };
            D3D11_VIEWPORT vp0{ 0, 0, (float)p.w, (float)p.h, 0, 1 };
            ctx_->RSSetViewports(1, crt ? &vp0 : &vp);
            ctx_->OMSetRenderTargets(1, crt ? &crt_rtv_ : &rtv_, nullptr);
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx_->IASetInputLayout(nullptr);
            ctx_->VSSetShader(vs_, nullptr, 0);
            ctx_->PSSetShader(ps_, nullptr, 0);
            ID3D11ShaderResourceView *srvs[6] = { srv_, g_hires, g_picture, g_background, g_tail, nullptr };
            ID3D11SamplerState *ss[2] = { samp_point_, samp_linear_ };
            ctx_->PSSetSamplers(0, 2, ss);
            ctx_->PSSetConstantBuffers(0, 1, &cb_);
            if (lap_ps_ && g_tail && g_hires && g_picture && g_background) {   /* the lap time list's rows, for the HUD */
                const float none4[4] = { 0, 0, 0, 0 };
                ctx_->ClearRenderTargetView(lap_rtv_, none4);
                D3D11_VIEWPORT vl{ 0, 0, 400, 1, 0, 1 };
                ctx_->OMSetRenderTargets(1, &lap_rtv_, nullptr);
                ctx_->RSSetViewports(1, &vl);
                ctx_->PSSetShader(lap_ps_, nullptr, 0);
                ctx_->PSSetShaderResources(0, 6, srvs);
                ctx_->Draw(3, 0);
                ctx_->OMSetRenderTargets(1, crt ? &crt_rtv_ : &rtv_, nullptr);
                ctx_->RSSetViewports(1, crt ? &vp0 : &vp);
                ctx_->PSSetShader(ps_, nullptr, 0);
                srvs[5] = lap_srv_;
            }
            ctx_->PSSetShaderResources(0, 6, srvs);
            ctx_->Draw(3, 0);
            if (crt) {
                ID3D11ShaderResourceView *none[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
                ctx_->PSSetShaderResources(0, 6, none);
                if (SUCCEEDED(ctx_->Map(crt_cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    *(CrtParams *)m.pData = CrtParams{ { (float)crt_cols, (float)crt_lines }, { (float)p.w, (float)p.h }, layout, { 0, 0, 0 } };
                    ctx_->Unmap(crt_cb_, 0);
                }
                /* the picture reduced to the CRT's resolution, then the CRT */
                D3D11_VIEWPORT vps{ 0, 0, (float)crt_cols, (float)crt_lines, 0, 1 };
                ctx_->OMSetRenderTargets(1, &src_rtv_, nullptr);
                ctx_->RSSetViewports(1, &vps);
                ctx_->PSSetShader(ds_ps_, nullptr, 0);
                ctx_->PSSetShaderResources(0, 1, &crt_srv_);
                ctx_->PSSetConstantBuffers(0, 1, &crt_cb_);
                ctx_->Draw(3, 0);
                ctx_->PSSetShaderResources(0, 1, none);
                ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
                ctx_->RSSetViewports(1, &vp);
                ctx_->PSSetShader(crt_ps_, nullptr, 0);
                ctx_->PSSetShaderResources(0, 1, &src_srv_);
                ctx_->Draw(3, 0);
            }
        }
        { ID3D11ShaderResourceView *none[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }; ctx_->PSSetShaderResources(0, 6, none); }
        if (p.w > 0 && screenshot_wanted(hwnd)) shoot(p);
        HRESULT r = swap_->Present(vsync && cfg_.vsync ? 1 : 0, 0);
#ifdef ULT_DEBUG
        {   /* tests (debug build): ULT_TEST_DEVICE_LOST=n acts as if the device were removed at the n-th frame shown */
            static unsigned n = 0; static const char *at = getenv("ULT_TEST_DEVICE_LOST");
            if (at && ++n == (unsigned)strtoul(at, nullptr, 10)) r = DXGI_ERROR_DEVICE_REMOVED;
        }
#endif
        if (r == DXGI_ERROR_DEVICE_REMOVED || r == DXGI_ERROR_DEVICE_RESET) {   /* R2-9: made again by the owner */
            const HRESULT why = dev_->GetDeviceRemovedReason();
            plog("display: Direct3D 11 device lost (Present 0x%08lX, reason 0x%08lX); making the display again", (unsigned long)r, (unsigned long)why);
            lost_ = true;
        }
        else if (FAILED(r) && fails_++ < 5) plog("display: Present failed 0x%08lX", (unsigned long)r);
        if (flip_model_) ctx_->OMSetRenderTargets(0, nullptr, nullptr);   /* flip model unbinds the back buffer */
    }

private:
    bool make_swap_chain(HWND hwnd)
    {
        IDXGIDevice *dxdev = nullptr; IDXGIAdapter *ad = nullptr; IDXGIFactory *f = nullptr;
        if (FAILED(dev_->QueryInterface(IID_IDXGIDevice, (void **)&dxdev))) return false;
        dxdev->GetAdapter(&ad); dxdev->Release();
        if (!ad) return false;
        ad->GetParent(IID_IDXGIFactory, (void **)&f); ad->Release();
        if (!f) return false;
        RECT cr; GetClientRect(hwnd, &cr);
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferDesc.Width = cr.right > 0 ? cr.right : 640;
        d.BufferDesc.Height = cr.bottom > 0 ? cr.bottom : 480;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.OutputWindow = hwnd;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT r = f->CreateSwapChain(dev_, &d, &swap_);
        flip_model_ = SUCCEEDED(r);
        if (FAILED(r)) {
            d.BufferCount = 1;
            d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            r = f->CreateSwapChain(dev_, &d, &swap_);
        }
        f->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        f->Release();
        if (FAILED(r)) { plog("display: CreateSwapChain failed 0x%08lX", (unsigned long)r); return false; }
        IDXGIDevice1 *d1 = nullptr;
        if (SUCCEEDED(dev_->QueryInterface(IID_IDXGIDevice1, (void **)&d1))) { d1->SetMaximumFrameLatency(1); d1->Release(); }
        bw_ = d.BufferDesc.Width; bh_ = d.BufferDesc.Height;
        return make_rtv();
    }

    bool make_rtv()
    {
        ID3D11Texture2D *bb = nullptr;
        if (FAILED(swap_->GetBuffer(0, IID_ID3D11Texture2D, (void **)&bb))) return false;
        HRESULT r = dev_->CreateRenderTargetView(bb, nullptr, &rtv_);
        bb->Release();
        return SUCCEEDED(r);
    }

    void resize(int w, int h)
    {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        release(rtv_);
        HRESULT r = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(r)) { plog("display: ResizeBuffers(%d, %d) failed 0x%08lX", w, h, (unsigned long)r); return; }
        bw_ = w; bh_ = h;
        make_rtv();
        plog("display: output %dx%d", w, h);
    }

    /* the CRT pass's input: the composite at the picture's size on screen */
    /* a screenshot: the picture as drawn this frame, read back from the back buffer */
    void shoot(const Placement &p)
    {
        ID3D11Texture2D *bb = nullptr, *st = nullptr;
        if (FAILED(swap_->GetBuffer(0, IID_ID3D11Texture2D, (void **)&bb))) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = p.w; td.Height = p.h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &st))) {
            D3D11_BOX box{ (UINT)p.x, (UINT)p.y, 0, (UINT)(p.x + p.w), (UINT)(p.y + p.h), 1 };
            ctx_->CopySubresourceRegion(st, 0, 0, 0, 0, bb, 0, &box);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx_->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                std::vector<BYTE> grey;
                if (g_dsv && depth_tex_) depth_picture(p, grey);
                screenshot_save((const BYTE *)m.pData, p.w, p.h, (int)m.RowPitch, false, grey.empty() ? nullptr : grey.data(), p.w);
                ctx_->Unmap(st, 0);
            }
        }
        release(st); release(bb);
    }

    /* R2-8: the depth buffer at the screen's size (24-bit depth, 8-bit stencil; typeless so ReShade can read it) */
    bool depth_target()
    {
        if (depth_tex_ && depth_w_ == bw_ && depth_h_ == bh_) return true;
        g_dsv = nullptr; release(dsv_); release(depth_tex_);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = bw_; td.Height = bh_; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &depth_tex_)) || FAILED(dev_->CreateDepthStencilView(depth_tex_, &dd, &dsv_))) {
            release(dsv_); release(depth_tex_);
            if (!depth_failed_) { depth_failed_ = true; plog("display: no depth buffer (%dx%d)", bw_, bh_); }
            return false;
        }
        depth_w_ = bw_; depth_h_ = bh_;
        ctx_->ClearDepthStencilView(dsv_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        return true;
    }

    /* the screenshot's depth picture: the depth as ReShade's default settings read it (not reversed, far plane 1000), linearised,
       then on a log scale so the course near the car shows (a straight scale is black up close): 0.0001 of the far
       plane and nearer black, the far plane (and the sky) white */
    void depth_picture(const Placement &p, std::vector<BYTE> &grey)
    {
        ID3D11Texture2D *st = nullptr;
        D3D11_TEXTURE2D_DESC td{};
        depth_tex_->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &st))) return;
        ctx_->CopyResource(st, depth_tex_);   /* a depth buffer is copied whole */
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            grey.resize((size_t)p.w * p.h);
            /* D24S8 as read back: Windows keeps the depth in the low 24 bits and the stencil (0 or 1 here) in the top
               byte; Wine (OpenGL's DEPTH24_STENCIL8) the depth in the top 24 bits and the stencil in the low byte */
            unsigned top_small = 0, n = 0;
            for (int y = 0; y < p.h; y += 8) {
                const UINT *row = (const UINT *)((const BYTE *)m.pData + (size_t)(p.y + y) * m.RowPitch) + p.x;
                for (int x = 0; x < p.w; x += 8) { n++; if ((row[x] >> 24) <= 2) top_small++; }
            }
            const bool high = top_small < n * 9 / 10;
            for (int y = 0; y < p.h; y++) {
                const UINT *row = (const UINT *)((const BYTE *)m.pData + (size_t)(p.y + y) * m.RowPitch) + p.x;
                for (int x = 0; x < p.w; x++) {
                    const float d = (high ? row[x] >> 8 : row[x] & 0xFFFFFF) / 16777215.0f;
                    const float lin = d / (1000.0f - d * 999.0f);
                    const float g = lin <= 1e-4f ? 0.0f : lin >= 1 ? 1.0f : (log10f(lin) + 4.0f) / 4.0f;
                    grey[(size_t)y * p.w + x] = (BYTE)(int)(g * 255.0f + 0.5f);
                }
            }
            ctx_->Unmap(st, 0);
        }
        release(st);
    }

    /* the CRT pass's inputs: the composite at the picture's size on screen, and the same reduced to the CRT's
       resolution (16-bit float: linear light) */
    bool crt_target(int w, int h, int cols, int lines)
    {
        if (crt_tex_ && w == crt_w_ && h == crt_h_ && src_tex_ && cols == src_w_ && lines == src_h_) return true;
        release(crt_srv_); release(crt_rtv_); release(crt_tex_);
        release(src_srv_); release(src_rtv_); release(src_tex_);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        bool ok = SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &crt_tex_)) && SUCCEEDED(dev_->CreateRenderTargetView(crt_tex_, nullptr, &crt_rtv_)) &&
                  SUCCEEDED(dev_->CreateShaderResourceView(crt_tex_, nullptr, &crt_srv_));
        td.Width = cols; td.Height = lines; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ok = ok && SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &src_tex_)) && SUCCEEDED(dev_->CreateRenderTargetView(src_tex_, nullptr, &src_rtv_)) &&
             SUCCEEDED(dev_->CreateShaderResourceView(src_tex_, nullptr, &src_srv_));
        if (!ok) {
            release(crt_srv_); release(crt_rtv_); release(crt_tex_);
            release(src_srv_); release(src_rtv_); release(src_tex_);
            crt_w_ = crt_h_ = src_w_ = src_h_ = 0;
            return false;
        }
        crt_w_ = w; crt_h_ = h; src_w_ = cols; src_h_ = lines;
        return true;
    }

    bool make_shaders()
    {
        HMODULE c = LoadLibraryA("d3dcompiler_47.dll");
        if (!c) { plog("display: d3dcompiler_47.dll not found"); return false; }
        pD3DCompile compile = nullptr;
        set_fn(compile, GetProcAddress(c, "D3DCompile"));
        if (!compile) return false;
        ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
        HRESULT r = compile(kShader, sizeof kShader - 1, "present", nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vs, &err);
        if (SUCCEEDED(r)) r = compile(kShader, sizeof kShader - 1, "present", nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &ps, &err);
        if (FAILED(r)) {
            plog("display: shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
            release(err); release(vs); release(ps);
            return false;
        }
        bool ok = SUCCEEDED(dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) &&
                  SUCCEEDED(dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_));
        release(vs); release(ps); release(err);
        if (ok) {
            r = compile(kShader, sizeof kShader - 1, "present", nullptr, nullptr, "lap_main", "ps_4_0", 0, 0, &ps, &err);
            if (FAILED(r)) plog("display: lap rows shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
            else dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &lap_ps_);
            release(ps); release(err);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = 400; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (lap_ps_ && (FAILED(dev_->CreateTexture2D(&td, nullptr, &lap_tex_)) || FAILED(dev_->CreateRenderTargetView(lap_tex_, nullptr, &lap_rtv_)) ||
                            FAILED(dev_->CreateShaderResourceView(lap_tex_, nullptr, &lap_srv_)))) release(lap_ps_);
        }
        /* the CRT pass is optional: if it fails to compile the game runs without it */
        r = compile(kCrtShader, sizeof kCrtShader - 1, "crt", nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &ps, &err);
        if (FAILED(r)) plog("display: CRT shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
        else dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &crt_ps_);
        release(ps); release(err);
        r = compile(kCrtShader, sizeof kCrtShader - 1, "crt", nullptr, nullptr, "ds_main", "ps_4_0", 0, 0, &ps, &err);
        if (FAILED(r)) plog("display: CRT shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
        else dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ds_ps_);
        release(ps); release(err);
        return ok;
    }

    bool upload(const BYTE *bits, int w, int h, int pitch)
    {
        if (!tex_ || w != tw_ || h != th_) {
            release(srv_); release(tex_);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tex_))) return false;
            if (FAILED(dev_->CreateShaderResourceView(tex_, nullptr, &srv_))) return false;
            tw_ = w; th_ = h;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        for (int y = 0; y < h; y++) {
            const WORD *s = (const WORD *)(bits + y * pitch);
            DWORD *d = (DWORD *)((BYTE *)m.pData + y * m.RowPitch);
            for (int x = 0; x < w; x++) {
                DWORD v = s[x];
                DWORD r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                d[x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
        }
        ctx_->Unmap(tex_, 0);
        return true;
    }

    DisplayConfig cfg_;
public:
    void set_config(const DisplayConfig &c) override { cfg_ = c; }
    bool lost() const override { return lost_; }
    bool lost_ = false;
    ID3D11Device *dev_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    IDXGISwapChain *swap_ = nullptr;
    ID3D11RenderTargetView *rtv_ = nullptr;
    ID3D11Texture2D *tex_ = nullptr;
    ID3D11ShaderResourceView *srv_ = nullptr;
    ID3D11VertexShader *vs_ = nullptr;
    ID3D11PixelShader *ps_ = nullptr;
    ID3D11SamplerState *samp_point_ = nullptr, *samp_linear_ = nullptr;
    ID3D11Buffer *cb_ = nullptr;
    ID3D11PixelShader *crt_ps_ = nullptr;
    ID3D11Buffer *crt_cb_ = nullptr;
    ID3D11Texture2D *crt_tex_ = nullptr;
    ID3D11RenderTargetView *crt_rtv_ = nullptr;
    ID3D11ShaderResourceView *crt_srv_ = nullptr;
    ID3D11PixelShader *ds_ps_ = nullptr;
    ID3D11PixelShader *lap_ps_ = nullptr;
    ID3D11Texture2D *lap_tex_ = nullptr;
    ID3D11RenderTargetView *lap_rtv_ = nullptr;
    ID3D11ShaderResourceView *lap_srv_ = nullptr;
    ID3D11Texture2D *src_tex_ = nullptr;
    ID3D11RenderTargetView *src_rtv_ = nullptr;
    ID3D11ShaderResourceView *src_srv_ = nullptr;
    int crt_w_ = 0, crt_h_ = 0, src_w_ = 0, src_h_ = 0;
    ID3D11Texture2D *depth_tex_ = nullptr;
    ID3D11DepthStencilView *dsv_ = nullptr;
    int depth_w_ = 0, depth_h_ = 0;
    bool depth_failed_ = false;
    float depth_logged_[4] = { 0, 0, 0, 0 };
    int crt_logged_ = 0;
    float hud_logged_[2] = { 0, 0 };
    bool flip_model_ = false;
    int bw_ = 0, bh_ = 0, tw_ = 0, th_ = 0;
    int fails_ = 0;
    Picture last_picture_ = (Picture)-1;
};

} // namespace

Presenter *make_d3d11_presenter(HWND hwnd, const DisplayConfig &cfg)
{
    D3D11Presenter *p = new D3D11Presenter(cfg);
    if (p->init(hwnd)) { g_kind = PresentKind::D3D11; g_dev = p->dev_; g_ctx = p->ctx_; return p; }
    delete p;
    return nullptr;
}

Presenter *make_gdi_presenter(const DisplayConfig &cfg) { g_kind = PresentKind::Gdi; return new GdiPresenter(cfg); }

PresentKind present_kind() { return g_kind; }

bool present_device(ID3D11Device **dev, ID3D11DeviceContext **ctx)
{
    if (g_kind != PresentKind::D3D11 || !g_dev) return false;
    *dev = g_dev; *ctx = g_ctx;
    return true;
}

void present_picture_size(int &w, int &h) { w = g_pic_w; h = g_pic_h; }

void present_set_layer(ID3D11ShaderResourceView *hires, ID3D11ShaderResourceView *picture, ID3D11ShaderResourceView *background)
{
    const bool on = hires && picture && background;
    g_hires = on ? hires : nullptr;
    g_picture = on ? picture : nullptr;
    g_background = on ? background : nullptr;
    if (!on) g_tail = nullptr;
}

void present_set_hud(ID3D11ShaderResourceView *tail, float scale, int filter, bool incar)
{
    g_hud_incar = incar;
    g_tail = tail;
    g_hud_frac = scale > 0.2f && scale <= 1.0f ? scale : 1.0f;
    g_hud_filter = filter < 0 ? 0 : filter > 2 ? 2 : filter;
}

/* R2-6: CRT on the Options DISPLAY page and [Display] Crt */
void present_set_crt(int mode) { g_crt = mode >= 0 && mode <= 4 ? mode : 0; }
int present_crt() { return g_crt; }

void (*g_lost_handler)() = nullptr;
void present_set_lost_handler(void (*fn)()) { g_lost_handler = fn; }
void present_device_lost() { if (g_lost_handler) g_lost_handler(); }

/* R2-8 (1.1.18) */
void present_set_depth(bool on) { g_depth_on = on; if (!on) g_dsv = nullptr; }
bool present_depth_on() { return g_depth_on; }
bool present_depth(ID3D11DepthStencilView **dsv, float rect[4])
{
    if (!g_dsv) return false;
    *dsv = g_dsv;
    for (int i = 0; i < 4; i++) rect[i] = g_depth_rect[i];
    return true;
}
