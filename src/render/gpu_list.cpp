/* The new renderer's list drawing (R2-3). See gpu_list.h. */
#include "persp.h"
#include "gpu_list.h"
#include "sky.h"
#include "raster_exact.h"
#include "spans_exact.h"
#include "textures.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>

namespace gpulist {

namespace {

constexpr int kW = 640, kH = 400, kAtlas = 2048, kMaxInst = 4096;
/* R2-5 filtering: every texture is placed with a border of 8 repeated edge texels on all sides, its place aligned to
   16, so the RGBA atlas's mip levels 1 to 3 never mix neighbouring textures. */
constexpr int kPad = 8, kAlign = 16, kRgbaLevels = 4;
inline int align_up(int v) { return (v + kAlign - 1) & ~(kAlign - 1); }
/* a game texel as the presenter shows it (565 green as 6 bits), premultiplied; 0x0020 is clear */
inline uint32_t rgba_texel(uint16_t v, bool is555)
{
    if (v == 0x0020) return 0;
    uint32_t r, g, b;
    if (is555) { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; r = r << 3 | r >> 2; g = g << 3 | g >> 2; }
    else { r = v >> 11; g = (v >> 5) & 63; b = v & 31; r = r << 3 | r >> 2; g = g << 2 | g >> 4; }
    b = b << 3 | b >> 2;
    return r | g << 8 | b << 16 | 0xFF000000u;
}
constexpr uint32_t kScaleX = 0xBD9DF8, kScaleY = 0xC464A8, kDrawOn = 0x48ABC8, kScroll = 0xC0A700;
constexpr uint32_t kDescTable = 0xBFD660, kDescCount = 0xC0C080, kCacheCursor = 0xBDBA20, kMode555 = 0x48ABEC;
constexpr uint32_t kShadowSX = 0x46E4A8, kShadowSY = 0x46E4B8, kShadowAddX = 0x47E2C4, kShadowAddY = 0x47E2C8;
constexpr uint32_t kViewFlagD0 = 0x48ABD0, kViewFlagF4 = 0x48ABF4, kViewW = 0xC584F4, kViewH = 0xC58486;

template <typename T> T rd(uint32_t va) { T v; memcpy(&v, (const void *)(uintptr_t)va, sizeof v); return v; }

uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* One instance: a quad's box, its rows in the row table (rexact::Row, one per picture row from first_row), its
   texels in the atlas. Stored for the shader as four float4 (all whole numbers below 2^24, so exact). */
struct Inst { float box[4], rows[4], tex[4], misc[4]; };
enum Kind { kQuad = 0, kShadow = 1, kSolid = 2 };
constexpr int kRowsPerLine = 256, kRowLines = 1024, kMaxRows = kRowsPerLine * kRowLines;

const char kShader[] = R"(
Texture2D<float4> insts : register(t0);  // 4 texels per instance: box, rows (base, first, count, kind), tex (atlas x, y, w, h), misc (keyed, colour)
Texture2D<float4> rows : register(t1);   // 3 texels per row, 256 rows per line: (x, n, t0, adv), (sv, su, fv, dv), (fu, du, 0, 0)
Texture2D<uint> atlas : register(t2);

struct VSOut { float4 pos : SV_Position; nointerpolation uint id : INST; };

VSOut vs_main(uint v : SV_VertexID)
{
    uint iid = v / 6, vid = v - iid * 6;        // six vertices (two triangles) per quad, in list order
    float4 box = insts.Load(int3(0, iid, 0));
    uint c = vid < 3 ? vid : vid - 2;           // 0 1 2, 1 2 3
    float px = (c & 1) ? box.z : box.x;
    float py = (c & 2) ? box.w : box.y;
    VSOut o;
    o.pos = float4(px / 320.0 - 1.0, 1.0 - py / 200.0, 0, 1);
    o.id = iid;
    return o;
}

uint ps_main(VSOut i) : SV_Target
{
    int4 rinfo = (int4)insts.Load(int3(1, i.id, 0));
    int4 tex = (int4)insts.Load(int3(2, i.id, 0));
    int4 misc = (int4)insts.Load(int3(3, i.id, 0));
    int px = (int)i.pos.x, py = (int)i.pos.y;
    int ri = py - rinfo.y;
    if (ri < 0 || ri >= rinfo.z) discard;
    int r = rinfo.x + ri;
    int lx = (r % 256) * 3, ly = r / 256;
    int4 a = (int4)rows.Load(int3(lx, ly, 0));
    int k = px - a.x;
    if (k < 0 || k >= a.y) discard;
    if (rinfo.w == 1) {                         // car shadow: black on every other pixel
        if (((px + py) & 1) != 0) discard;
        return (uint)misc.y;
    }
    if (rinfo.w == 2) return (uint)misc.y;     // one colour (SeeThrough outlines)
    int4 b = (int4)rows.Load(int3(lx + 1, ly, 0));
    int4 c = (int4)rows.Load(int3(lx + 2, ly, 0));
    int t;                                      // rexact::texel
    if (a.w == 0) t = a.z + b.x * ((b.z + k * b.w) >> 16) + b.y * ((c.x + k * c.y) >> 16);
    else {
        int j = (k > 0 && ((px & 1) != 0)) ? k - 1 : k;
        t = a.z + j * a.w + b.x * ((b.z + j * b.w) >> 8) + b.y * ((c.x + j * c.y) >> 8) + (j != k ? a.w : 0);
    }
    if (t < 0 || t >= tex.z * tex.w) discard;   // the game reads past the texture here
    int row = t / tex.z, col = t - row * tex.z;
    uint tx = atlas.Load(int3(tex.x + col, tex.y + row, 0));
    if (misc.x != 0 && tx == 0x20) discard;
    return tx;
}
)";

/* The high-resolution pass (R2-5): the same records drawn again at the chosen resolution, with the game's scanline
   mapping evaluated continuously (each edge linear in y, then linear across the row, as pcmap.h) and the game's
   coverage rule (rows and spans half a pixel wider than the edges), into a BGRA target that is clear (alpha 0)
   where nothing was drawn. Shadows and outlines come as their 640x400 spans, scaled up. */
struct HInst { float box[4], p01[4], p23[4], uv01[4], uv23[4], tex[4], misc[4], pad[4]; };
enum HKind { kHQuad = 0, kHChecker = 1, kHSolid = 2, kHSky = 3 };
constexpr int kHInstW = 8;

const char kHiresShader[] = R"(
Texture2D<float4> hinst : register(t0);   // 8 texels per instance: box, p01, p23, uv01, uv23, tex, misc (kind, keyed, colour, 555), z (corners' depths; 0: none)
Texture2D<uint> atlas : register(t2);
Texture2D rgba : register(t3);             // the same textures, premultiplied RGBA with mip levels (filtering)
SamplerState s_linear : register(s0);      // bilinear (mip 0)
SamplerState s_tri : register(s1);         // trilinear
SamplerState s_aniso : register(s2);       // anisotropic
Texture2D skytex : register(t4);           // the sky panorama (900x400 art, BGRA)
SamplerState s_sky : register(s3);         // bilinear, wrapping across
cbuffer hp : register(b0) {
    float4 to1x;    // xy: 640x400 units per target pixel; z: coverage widening; w: filter
    float4 ms;      // x: coverage samples per pixel (1, 2, 4 or 8: edge antialiasing); y: mapping (0 PC, 1 Saturn, 2 perspective);
                    // zw: the depth pass's origin (where the 3D's top left is in the screen-sized depth buffer)
    float4 dz;      // the depth pass (R2-8): x the near plane, y the farthest depth (game units)
};

struct VSOut { float4 pos : SV_Position; nointerpolation uint id : INST; };

VSOut vs_main(uint v : SV_VertexID)
{
    uint iid = v / 6, vid = v - iid * 6;
    float4 box = hinst.Load(int3(0, iid, 0));
    uint c = vid < 3 ? vid : vid - 2;
    float px = (c & 1) ? box.z : box.x;
    float py = (c & 2) ? box.w : box.y;
    VSOut o;
    o.pos = float4(px / 320.0 - 1.0, 1.0 - py / 200.0, 0, 1);
    o.id = iid;
    return o;
}

float4 colour(uint t, bool is555)
{
    uint r, g, b;
    if (is555) { r = (t >> 10) & 31; g = (t >> 5) & 31; b = t & 31; r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); }
    else { r = (t >> 11) & 31; g = (t >> 5) & 63; b = t & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); }
    b = (b << 3) | (b >> 2);
    return float4(r / 255.0, g / 255.0, b / 255.0, 1.0);
}

/* The game's coverage rule for quad P at point S (640x400 units): the rows from ymin to ymax and, on the row through
   S, the span between the edges, each widened by d. xl / xr and the texel coordinates at the span's ends come back
   for the texture mapping. */
bool covers(float2 S, float2 P[4], float2 T[4], float d, out float xl, out float xr, out float2 tl, out float2 tr)
{
    float ymin = min(min(P[0].y, P[1].y), min(P[2].y, P[3].y)), ymax = max(max(P[0].y, P[1].y), max(P[2].y, P[3].y));
    bool inside = !(S.y < ymin - d || S.y >= ymax + d);
    float yc = clamp(S.y, ymin, ymax);
    xl = 1e9; xr = -1e9; tl = 0; tr = 0;
    [unroll] for (int e = 0; e < 4; e++) {
        float2 a = P[e], b = P[(e + 1) & 3];
        float2 ta = T[e], tb = T[(e + 1) & 3];
        if (yc < min(a.y, b.y) || yc > max(a.y, b.y)) continue;
        float t = b.y != a.y ? (yc - a.y) / (b.y - a.y) : 0.0;
        float x0 = lerp(a.x, b.x, t); float2 t0 = lerp(ta, tb, t);
        float x1 = b.y != a.y ? x0 : b.x; float2 t1 = b.y != a.y ? t0 : tb;
        if (x0 < xl) { xl = x0; tl = t0; } if (x0 > xr) { xr = x0; tr = t0; }
        if (x1 < xl) { xl = x1; tl = t1; } if (x1 > xr) { xr = x1; tr = t1; }
    }
    return inside && !(xl > xr || S.x < xl - d || S.x >= xr + d);
}

/* R2-7, the Saturn mapping (1.1.15): the texture spread over the quad as the Saturn's VDP1 draws a distorted sprite,
   bilinearly in the quad's own parameters: the point at (s, t) is the bilinear blend of the corners P0..P3 (in their
   cycle order), and its texel the same blend of the texture corners. Finding (s, t) for this pixel is inverting a
   bilinear map (a quadratic); false where there is no answer (a twisted quad), and the PC mapping is used. */
float cross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }
bool inv_bilinear(float2 p, float2 P[4], out float2 st)
{
    float2 e = P[1] - P[0], f = P[3] - P[0], g = P[0] - P[1] + P[2] - P[3], h = p - P[0];
    float k2 = cross2(g, f), k1 = cross2(e, f) + cross2(h, g), k0 = cross2(h, e);
    float v;
    st = float2(0, 0);
    if (abs(k2) < 1e-4 * max(abs(k1), 1e-6)) {   // a parallelogram or trapezium across: linear
        if (abs(k1) < 1e-8) return false;
        v = -k0 / k1;
    } else {
        float w = k1 * k1 - 4.0 * k0 * k2;
        if (w < 0.0) return false;
        w = sqrt(w);
        float ik2 = 0.5 / k2;
        v = (-k1 - w) * ik2;
        if (v < -0.5 || v > 1.5) v = (-k1 + w) * ik2;
    }
    float2 den = e + g * v;
    float u = abs(den.x) > abs(den.y) ? (h.x - f.x * v) / den.x : (h.y - f.y * v) / (abs(den.y) > 1e-8 ? den.y : 1e-8);
    st = saturate(float2(u, v));
    return true;
}

/* Edge antialiasing: the coverage rule tested at 2, 4 or 8 points of the pixel (the standard MSAA sample positions,
   in 1/16 pixel from the centre); the fraction covered becomes the pixel's opacity, blended in the game's back-to-front
   order over what is already there. Edges between neighbouring quads stay closed: the widened rule makes them overlap
   past each edge, so only outlines against what is behind are softened. */
static const float2 kPos2[2] = { float2(4, 4), float2(-4, -4) };
static const float2 kPos4[4] = { float2(-2, -6), float2(6, -2), float2(-6, 2), float2(2, 6) };
static const float2 kPos8[8] = { float2(1, -3), float2(-1, 3), float2(5, 1), float2(-3, -5),
                                 float2(-5, 5), float2(-7, -1), float2(3, 7), float2(7, -7) };

float4 ps_main(VSOut i) : SV_Target
{
    float4 misc = hinst.Load(int3(6, i.id, 0));
    int kind = (int)misc.x;
    bool is555 = misc.w != 0;
    float2 S = i.pos.xy * to1x.xy;             // this pixel's centre in 640x400 units
    if (kind == 1) {                            // car shadow span: every other 640x400 pixel
        int2 p = (int2)floor(S);
        if (((p.x + p.y) & 1) != 0) discard;
        return colour((uint)misc.z, is555);
    }
    if (kind == 2) return colour((uint)misc.z, is555);
    if (kind == 3) {                            // the sky: a copy of the game's, from the panorama's art (2x2 pixels each)
        float4 o = hinst.Load(int3(1, i.id, 0));        // where art texel (0, 0) lands; the art's u, v there
        float2 a = o.zw + (S - o.xy) * 0.5;
        a.y = clamp(a.y, 0.5, 399.5);
        int filter = (int)to1x.w;
        if (filter == 0) {                      // nearest: the game's own look
            int2 q = (int2)floor(a);
            q.x = ((q.x % 900) + 900) % 900;
            return skytex.Load(int3(q, 0));
        }
        if (filter == 1) {                      // sharp bilinear: whole art pixels with smoothed edges
            float2 scale = max(2.0 / to1x.xy, 1.0);
            float2 b = floor(a - 0.5) + 0.5;
            a = b + saturate((a - b - 0.5) * scale + 0.5);
        }
        return skytex.SampleLevel(s_sky, a / float2(900.0, 400.0), 0);
    }
    float4 p01 = hinst.Load(int3(1, i.id, 0)), p23 = hinst.Load(int3(2, i.id, 0));
    float4 uv01 = hinst.Load(int3(3, i.id, 0)), uv23 = hinst.Load(int3(4, i.id, 0));
    float2 P[4] = { p01.xy, p01.zw, p23.xy, p23.zw };
    float2 T[4] = { uv01.xy, uv01.zw, uv23.xy, uv23.zw };
    float d = to1x.z;
    float xl, xr; float2 tl, tr;
    bool inside = covers(S, P, T, d, xl, xr, tl, tr);   // the pixel's centre: its texture mapping
    float cov = 1.0;
    int ns = (int)ms.x;
    if (ns > 1) {                               // edge antialiasing: the same rule at each sample position
        int hits = 0;
        float xl2, xr2; float2 tl2, tr2;
        [loop] for (int s = 0; s < ns; s++) {
            float2 o = ns == 2 ? kPos2[s & 1] : ns == 4 ? kPos4[s & 3] : kPos8[s & 7];
            if (covers(S + o / 16.0 * to1x.xy, P, T, d, xl2, xr2, tl2, tr2)) hits++;
        }
        cov = hits / (float)ns;
        inside = hits > 0;
    }
    float k = xr > xl ? (clamp(S.x, xl, xr) - xl) / (xr - xl) : 0.0;
    float2 uv = lerp(tl, tr, k);
    float2 st;
    if (ms.y == 1 && inv_bilinear(S, P, st))   // Saturn: bilinear in the quad's own parameters
        uv = lerp(lerp(T[0], T[1], st.x), lerp(T[3], T[2], st.x), st.y);
    if (ms.y == 2) {   /* perspective-correct (1.1.16): the quad as a bilinear patch between its corners in 3D (each
                          corner at its depth z on the ray through its screen point); the pixel's ray meets it where
                          the corners' z (P - S) blend to nothing, an inverse bilinear of the point 0 */
        float4 zc = hinst.Load(int3(7, i.id, 0));   // negative: depths for the depth buffer only (cut at the near plane)
        float2 R[4] = { zc.x * (P[0] - S), zc.y * (P[1] - S), zc.z * (P[2] - S), zc.w * (P[3] - S) };
        if (zc.x > 0 && inv_bilinear(float2(0, 0), R, st))
            uv = lerp(lerp(T[0], T[1], st.x), lerp(T[3], T[2], st.x), st.y);
    }
    float2 g0 = ddx(uv), g1 = ddy(uv);          // before any discard: every pixel of the 2x2 group has its uv
    if (!inside) discard;
    float4 tex = hinst.Load(int3(5, i.id, 0));
    int filter = (int)to1x.w;
    if (filter == 0) {                          // nearest: the game's own texels
        int2 q = clamp((int2)floor(uv), int2(0, 0), (int2)tex.zw - 1);
        uint tx = atlas.Load(int3((int)tex.x + q.x, (int)tex.y + q.y, 0));
        if (misc.y != 0 && tx == 0x20) discard;
        return colour(tx, is555) * cov;         // premultiplied by the coverage
    }
    // filtered: texel coordinates kept inside the texture (its border repeats the edge texels)
    float2 t = clamp(uv, 0.5, tex.zw - 0.5);
    float4 c;
    if (filter == 1) {                          // sharp bilinear: whole texels with smoothed edges between them
        float2 scale = max(1.0 / max(abs(g0) + abs(g1), 1e-4), 1.0);
        float2 b = floor(t - 0.5) + 0.5, f = t - b;
        f = saturate((f - 0.5) * scale + 0.5);
        c = rgba.SampleLevel(s_linear, (tex.xy + b + f) / 2048.0, 0);
    } else if (filter == 2) c = rgba.SampleLevel(s_linear, (tex.xy + t) / 2048.0, 0);
    else if (filter == 3) c = rgba.SampleGrad(s_tri, (tex.xy + t) / 2048.0, g0 / 2048.0, g1 / 2048.0);
    else c = rgba.SampleGrad(s_aniso, (tex.xy + t) / 2048.0, g0 / 2048.0, g1 / 2048.0);
    if (misc.y == 0) return float4(c.a > 0 ? c.rgb / c.a : float3(0, 0.0039, 0), 1) * cov;   // opaque record
    c *= cov;
    if (c.a < 1.0 / 255.0) discard;
    return c;                                   // premultiplied: blended over what is under it
}

/* R2-8 (1.1.18), the depth buffer for ReShade: a quad's depth at this pixel, where the colour pass covers it (the
   pixel's centre, the same rule and widening), from the same perspective solve (the corners' true depths blended
   where the pixel's ray meets the quad). Keyed texels (see-through) write none. The depth is written as ReShade's
   default settings read it (1.1.19: not reversed, RESHADE_DEPTH_INPUT_IS_REVERSED=0 as its settings page writes it;
   far plane 1000): hyperbolic with near 1 and far 1000 after the game's dz.x .. dz.y are mapped to 1 .. 1000, so
   ReShade's linearised depth is 0 at dz.x and 1 at dz.y. */
float ps_depth(VSOut i) : SV_Depth
{
    float4 misc = hinst.Load(int3(6, i.id, 0));
    float4 p01 = hinst.Load(int3(1, i.id, 0)), p23 = hinst.Load(int3(2, i.id, 0));
    float4 uv01 = hinst.Load(int3(3, i.id, 0)), uv23 = hinst.Load(int3(4, i.id, 0));
    float2 P[4] = { p01.xy, p01.zw, p23.xy, p23.zw };
    float2 T[4] = { uv01.xy, uv01.zw, uv23.xy, uv23.zw };
    float2 S = (i.pos.xy - ms.zw) * to1x.xy;
    float xl, xr; float2 tl, tr;
    if (!covers(S, P, T, to1x.z, xl, xr, tl, tr)) discard;
    float4 zr = hinst.Load(int3(7, i.id, 0));
    float4 zc = abs(zr);
    float2 R[4] = { zc.x * (P[0] - S), zc.y * (P[1] - S), zc.z * (P[2] - S), zc.w * (P[3] - S) };
    float2 st;
    if (!inv_bilinear(float2(0, 0), R, st)) discard;
    if (misc.y != 0) {                          // keyed: the colour pass's texel (nearest), none where it is clear
        float k = xr > xl ? (clamp(S.x, xl, xr) - xl) / (xr - xl) : 0.0;
        float2 uv = lerp(tl, tr, k);
        float2 st2;
        if (ms.y == 1 && inv_bilinear(S, P, st2)) uv = lerp(lerp(T[0], T[1], st2.x), lerp(T[3], T[2], st2.x), st2.y);
        if (ms.y == 2 && zr.x > 0) uv = lerp(lerp(T[0], T[1], st.x), lerp(T[3], T[2], st.x), st.y);
        float4 tex = hinst.Load(int3(5, i.id, 0));
        int2 q = clamp((int2)floor(uv), int2(0, 0), (int2)tex.zw - 1);
        if (atlas.Load(int3((int)tex.x + q.x, (int)tex.y + q.y, 0)) == 0x20) discard;
    }
    float z = lerp(lerp(zc.x, zc.y, st.x), lerp(zc.w, zc.z, st.x), st.y);
    float zn = 1.0 + 999.0 * saturate((z - dz.x) / max(dz.y - dz.x, 1e-3));
    return (1000.0 / 999.0) * (1.0 - 1.0 / zn);
}
)";

template <typename T> void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }

}  // namespace

struct Renderer::Impl {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Texture2D *rt = nullptr, *staging = nullptr, *atlas = nullptr, *rows_tex = nullptr;
    ID3D11ShaderResourceView *rows_srv = nullptr;
    std::vector<float> rows;     /* 12 floats per row (3 texels) */
    uint32_t nrows = 0;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11ShaderResourceView *atlas_srv = nullptr, *inst_srv = nullptr;
    ID3D11Texture2D *inst_buf = nullptr;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11RasterizerState *rs = nullptr;
    std::vector<Inst> pending;
    /* high-resolution pass */
    int hw = 0, hh = 0;                       /* target size; 0: off */
    /* coverage widening, 640x400 pixels each side. The game covers [round(left), round(right)] on whole rows, which
       closes cracks between neighbouring quads up to about 1.5 pixels; 0.75 each side does the same on the smooth
       edges (0.5 left hairline cracks on the Expert bridge and cliffs, found 7 Nov 2026). */
    float widen = 0.75f;
    int filter = 3;                           /* 0 nearest, 1 sharp bilinear, 2 bilinear, 3 trilinear, 4 anisotropic */
    ID3D11Texture2D *rgba = nullptr;
    ID3D11ShaderResourceView *rgba_srv = nullptr;
    ID3D11SamplerState *samp[4] = { nullptr, nullptr, nullptr, nullptr };
    ID3D11Texture2D *sky_tex = nullptr;        /* the sky panorama (sky.cpp), BGRA */
    ID3D11ShaderResourceView *sky_srv = nullptr;
    uint32_t sky_version = 0;
    std::vector<HInst> hsky;                   /* this frame's sky copies, drawn before the 3D */
    ID3D11BlendState *premul = nullptr;
    ID3D11BlendState *under = nullptr;        /* 1.1.17: drawn under what is there (crack fill, sky) */
    std::vector<uint32_t> pad_buf, mip_buf;
    ID3D11Texture2D *hrt = nullptr, *hinst_buf = nullptr;
    ID3D11RenderTargetView *hrtv = nullptr;
    ID3D11ShaderResourceView *hsrv = nullptr, *hinst_srv = nullptr, *rt_srv = nullptr;
    ID3D11Texture2D *bg = nullptr;
    ID3D11Texture2D *tailtex = nullptr;          /* the picture after the flush's tail (HUD split), R16_UINT */
    ID3D11ShaderResourceView *tail_srv = nullptr;               /* the picture before the 3D (as uploaded), under the high-resolution 3D */
    ID3D11ShaderResourceView *bg_srv = nullptr;
    ID3D11VertexShader *hvs = nullptr;
    ID3D11PixelShader *hps = nullptr;
    ID3D11Buffer *hcb = nullptr;
    /* R2-8 (1.1.18): the depth buffer (the presenter's, the screen's size; set each frame), where the 3D is in it */
    ID3D11DepthStencilView *dsv = nullptr;
    float drect[4] = { 0, 0, 0, 0 };
    float dnear = 1.0f, dfar = 1000.0f;
    ID3D11PixelShader *hps_depth = nullptr;
    ID3D11DepthStencilState *ds_exact = nullptr, *ds_fill = nullptr;
    ID3D11Texture2D *hstaging = nullptr;      /* the replayer's read-back of the high-resolution picture */
    int samples = 4;                          /* edge antialiasing: coverage samples per pixel (1: off) */
    int mapping = 0;                          /* R2-7: 0 PC (the 1996 release's), 1 Saturn, 2 perspective-correct */
    std::vector<HInst> hpending;
    bool shared = false;
    struct Entry { int32_t x, y; uint32_t hash; uint32_t checked; };
    uint32_t frame = 0;                       /* begin() count: each texture's contents checked once a frame */
    bool verify = false;                      /* 1.1.17: check every texture's contents (menus: the car select reuses places) */
    std::map<uint64_t, Entry> entries;
    int shelf_x = 0, shelf_y = 0, shelf_h = 0;
    uint32_t signature = 0;
    bool failed = false;

    ~Impl()
    {
        rel(rs); rel(ps); rel(vs); rel(inst_srv); rel(inst_buf); rel(rows_srv); rel(rows_tex); rel(atlas_srv); rel(atlas);
        rel(rtv); rel(staging); rel(rt);
        rel(hrt); rel(hrtv); rel(hsrv); rel(hinst_buf); rel(hinst_srv); rel(rt_srv); rel(bg_srv); rel(bg); rel(tail_srv); rel(tailtex); rel(hvs); rel(hps); rel(hcb); rel(hstaging);
        rel(rgba); rel(rgba_srv); rel(samp[0]); rel(samp[1]); rel(samp[2]); rel(samp[3]); rel(premul); rel(under); rel(sky_srv); rel(sky_tex);
        rel(hps_depth); rel(ds_exact); rel(ds_fill);
        rel(ctx); rel(dev);
    }

    void reset_atlas() { entries.clear(); shelf_x = shelf_y = shelf_h = 0; }

    /* Places w x h texels in the atlas; false when it is full. */
    bool place(int w, int h, int32_t &x, int32_t &y)
    {
        const int pw = align_up(w + 2 * kPad), ph = align_up(h + 2 * kPad);
        if (shelf_x + pw > kAtlas) { shelf_y += shelf_h; shelf_x = 0; shelf_h = 0; }
        if (shelf_y + ph > kAtlas || pw > kAtlas) return false;
        x = shelf_x + kPad; y = shelf_y + kPad; shelf_x += pw; if (ph > shelf_h) shelf_h = ph;
        return true;
    }

    void upload(int32_t x, int32_t y, int w, int h, const uint16_t *texels)
    {
        D3D11_BOX b{ (UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1 };
        ctx->UpdateSubresource(atlas, 0, &b, texels, (UINT)w * 2, 0);
        if (!rgba) return;
        /* the RGBA copy: the texture with its repeated border, then three box-filtered levels */
        const int rx = x - kPad, ry = y - kPad, pw = align_up(w + 2 * kPad), ph = align_up(h + 2 * kPad);
        const bool is555 = rd<uint32_t>(kMode555) != 0;
        pad_buf.resize((size_t)pw * ph);
        for (int py = 0; py < ph; py++) {
            const int sy = py - kPad < 0 ? 0 : py - kPad >= h ? h - 1 : py - kPad;
            for (int px = 0; px < pw; px++) {
                const int sx = px - kPad < 0 ? 0 : px - kPad >= w ? w - 1 : px - kPad;
                pad_buf[(size_t)py * pw + px] = rgba_texel(texels[(size_t)sy * w + sx], is555);
            }
        }
        int lw = pw, lh = ph;
        for (int l = 0; l < kRgbaLevels; l++) {
            if (l) {
                const int nw = lw / 2, nh = lh / 2;
                mip_buf.resize((size_t)nw * nh);
                for (int yy = 0; yy < nh; yy++)
                    for (int xx = 0; xx < nw; xx++) {
                        const uint32_t *p0 = &pad_buf[(size_t)(2 * yy) * lw + 2 * xx], *p1 = p0 + lw;
                        uint32_t o = 0;
                        for (int c = 0; c < 32; c += 8)
                            o |= ((((p0[0] >> c) & 255) + ((p0[1] >> c) & 255) + ((p1[0] >> c) & 255) + ((p1[1] >> c) & 255) + 2) / 4) << c;
                        mip_buf[(size_t)yy * nw + xx] = o;
                    }
                pad_buf.swap(mip_buf); lw = nw; lh = nh;
            }
            D3D11_BOX lb{ (UINT)(rx >> l), (UINT)(ry >> l), 0, (UINT)((rx >> l) + lw), (UINT)((ry >> l) + lh), 1 };
            ctx->UpdateSubresource(rgba, (UINT)l, &lb, pad_buf.data(), (UINT)lw * 4, 0);
        }
    }

    /* The atlas place of a texture (key, w x h texels at texels). dynamic: compare contents every frame. */
    bool texture(uint64_t key, const uint16_t *texels, int w, int h, bool dynamic, int32_t &x, int32_t &y, uint32_t &textures)
    {
        auto it = entries.find(key);
        if (it != entries.end()) {
            if ((dynamic || verify) && it->second.checked != frame) {
                it->second.checked = frame;
                uint32_t hh = fnv(texels, (size_t)w * h * 2);
                if (hh != it->second.hash) { upload(it->second.x, it->second.y, w, h, texels); it->second.hash = hh; }
            }
            x = it->second.x; y = it->second.y;
            return true;
        }
        if (!place(w, h, x, y)) {   /* full: draw what uses the atlas now, then start it again */
            draw();
            hdraw(false);
            reset_atlas();
            if (!place(w, h, x, y)) return false;
        }
        upload(x, y, w, h, texels);
        entries[key] = Entry{ x, y, fnv(texels, (size_t)w * h * 2), frame };
        textures++;
        return true;
    }

    void draw()
    {
        if (pending.empty() || failed) { pending.clear(); nrows = 0; return; }
        if (nrows) {   /* the row table, whole lines */
            const UINT lines = (nrows + kRowsPerLine - 1) / kRowsPerLine;
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(ctx->Map(rows_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { failed = true; return; }
            for (UINT l = 0; l < lines; l++)
                memcpy((uint8_t *)m.pData + (size_t)l * m.RowPitch, &rows[(size_t)l * kRowsPerLine * 12], kRowsPerLine * 12 * sizeof(float));
            ctx->Unmap(rows_tex, 0);
        }
        for (size_t first = 0; first < pending.size(); first += kMaxInst) {
            const UINT n = (UINT)(pending.size() - first < (size_t)kMaxInst ? pending.size() - first : kMaxInst);
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(ctx->Map(inst_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { failed = true; break; }
            for (UINT k = 0; k < n; k++) memcpy((uint8_t *)m.pData + (size_t)k * m.RowPitch, &pending[first + k], sizeof(Inst));
            ctx->Unmap(inst_buf, 0);
            D3D11_VIEWPORT vp{ 0, 0, (float)kW, (float)kH, 0, 1 };
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(rs);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs, nullptr, 0);
            ctx->VSSetShaderResources(0, 1, &inst_srv);
            ctx->PSSetShader(ps, nullptr, 0);
            ID3D11ShaderResourceView *srvs[3] = { inst_srv, rows_srv, atlas_srv };
            ctx->PSSetShaderResources(0, 3, srvs);
            ctx->Draw(6 * n, 0);
        }
        pending.clear(); nrows = 0;
    }

    /* Adds a quad's rows (from rexact) and the instance; kind kShadow ignores the texture. */
    template <typename Fill>
    bool add(Fill &&fill, int kind, int32_t ax, int32_t ay, int w, int h, bool keyed, uint32_t colour)
    {
        if (nrows + kH > (uint32_t)kMaxRows || pending.size() >= (size_t)kMaxInst * 64) draw();
        const uint32_t base = nrows;
        int32_t first = -1, last = -1, bx0 = kW, bx1 = 0;
        bool drew = fill([&](const rexact::Row &r) {
            if (first < 0) first = r.y;
            while (last + 1 < r.y && last >= 0) { /* rows not drawn in between: zero pixels */
                last++; float *f = &rows[(size_t)(nrows++) * 12]; memset(f, 0, 12 * sizeof(float));
            }
            last = r.y;
            float *f = &rows[(size_t)(nrows++) * 12];
            f[0] = (float)r.x; f[1] = (float)r.n; f[2] = (float)r.t0; f[3] = (float)r.adv;
            f[4] = (float)r.sv; f[5] = (float)r.su; f[6] = (float)r.fv; f[7] = (float)r.dv;
            f[8] = (float)r.fu; f[9] = (float)r.du; f[10] = 0; f[11] = 0;
            if (r.x < bx0) bx0 = r.x;
            if (r.x + r.n > bx1) bx1 = r.x + r.n;
        });
        if (!drew || first < 0 || bx0 >= bx1) { nrows = base; return drew; }
        Inst in{};
        in.box[0] = (float)bx0; in.box[1] = (float)first; in.box[2] = (float)bx1; in.box[3] = (float)(last + 1);
        in.rows[0] = (float)base; in.rows[1] = (float)first; in.rows[2] = (float)(last - first + 1); in.rows[3] = (float)kind;
        in.tex[0] = (float)ax; in.tex[1] = (float)ay; in.tex[2] = (float)w; in.tex[3] = (float)h;
        in.misc[0] = keyed ? 1.0f : 0.0f; in.misc[1] = (float)colour;
        pending.push_back(in);
        return true;
    }

    /* Pixels of one colour (kSolid) or a shadow's checker (kShadow), given as runs; one instance per run of a
       row (the first run of every row, then the second, ...), rows in order. */
    void add_runs(std::vector<rspans::Span> &runs, int kind, uint32_t colour)
    {
        if (runs.empty()) return;
        std::sort(runs.begin(), runs.end(), [](const rspans::Span &a, const rspans::Span &b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
        for (int layer = 0;; layer++) {
            bool any = false;
            add([&](auto &&emit) {
                size_t i = 0;
                while (i < runs.size()) {
                    size_t j = i; int k = 0;
                    while (j < runs.size() && runs[j].y == runs[i].y) {
                        if (k == layer) { rexact::Row r{}; r.y = runs[j].y; r.x = runs[j].x; r.n = runs[j].n; emit(r); any = true; }
                        j++; k++;
                    }
                    i = j;
                }
                return true;
            }, kind, 0, 0, 1, 1, false, colour);
            if (!any) break;
        }
    }

    void hpush(const HInst &h) { if (hw) hpending.push_back(h); }

    bool hires_size(int w, int h)
    {
        if (w == hw && h == hh) return true;
        rel(hrtv); rel(hsrv); rel(hrt); rel(hstaging); hw = hh = 0;
        if (w <= 0 || h <= 0) return true;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &hrt)) || FAILED(dev->CreateRenderTargetView(hrt, nullptr, &hrtv)) ||
            FAILED(dev->CreateShaderResourceView(hrt, nullptr, &hsrv))) { rel(hrtv); rel(hsrv); rel(hrt); return false; }
        hw = w; hh = h;
        return true;
    }

    /* 640x400 spans as rectangles at high resolution (shadows, outlines) */
    void hruns(const std::vector<rspans::Span> &runs, int kind, uint32_t colour, bool is555)
    {
        if (!hw) return;
        for (const auto &r : runs) {
            HInst h{};
            h.box[0] = (float)r.x; h.box[1] = (float)r.y; h.box[2] = (float)(r.x + r.n); h.box[3] = (float)(r.y + 1);
            h.misc[0] = (float)kind; h.misc[2] = (float)colour; h.misc[3] = is555 ? 1.0f : 0.0f;
            hpending.push_back(h);
        }
    }

    /* 1.1.17: three passes. (1) every record at its exact edges (no widening), with edge antialiasing, over what is
       there (back to front); (2) the quads again, widened to close hairline cracks, but only into what (1) left
       uncovered (blended under: the farther quad of a crack fills it first); (3) the sky, under everything. Before,
       one widened pass let each nearer quad's edge texels overlap its neighbour by 0.75 of a 640x400 pixel, which
       showed as faint lines along shared edges (jayyanz 7 Nov, at 1080p). last: the sky is drawn in the frame's
       last call only (an atlas reset draws part of a frame early). */
    void hdraw(bool last = true)
    {
        if (!hw || failed) { hpending.clear(); if (last) hsky.clear(); return; }
        std::vector<HInst> quads;
        for (const HInst &h : hpending) if ((int)h.misc[0] == kHQuad) quads.push_back(h);
        hpass(hpending, 0.0f, premul);
        /* R2-8: the depths in the same order: (1) every quad with depths at its exact edges, the last one drawn over a
           pixel winning (the game's painter's order decides, as for the colour; stencil marks the pixel); (2) the crack
           fill, only where (1) wrote nothing, the first quad there winning (as the colour's fill drawn under); (3) the
           same 2 pixels wide, for gaps the colour fills from the game's own 640x400 picture (seen along wall tops) */
        std::vector<HInst> dquads;
        if (dsv && hps_depth && ds_exact && ds_fill)
            for (const HInst &h : quads) if (h.pad[0] != 0) dquads.push_back(h);
        if (!failed) dpass(dquads, 0.0f, ds_exact, 1);
        if (!failed) hpass(quads, widen, under ? under : premul);
        if (!failed) dpass(dquads, widen, ds_fill, 0);
        if (!failed) dpass(dquads, 2.0f, ds_fill, 0);   /* (3) wider gaps (the colour shows the game's own pixel there) */
        if (last) { if (!failed) hpass(hsky, 0.0f, under ? under : premul); hsky.clear(); }
        hpending.clear();
        ID3D11RenderTargetView *none = nullptr;
        ctx->OMSetRenderTargets(1, &none, nullptr);   /* free the targets for the presenter */
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(nullptr, 0);
    }

    void dpass(const std::vector<HInst> &list, float d, ID3D11DepthStencilState *state, UINT ref)
    {
        if (list.empty() || drect[2] <= 0 || drect[3] <= 0) return;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(hcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float *f = (float *)m.pData; f[0] = 640.0f / drect[2]; f[1] = 400.0f / drect[3]; f[2] = d; f[3] = 0;
            f[4] = 1; f[5] = (float)mapping; f[6] = drect[0]; f[7] = drect[1];
            f[8] = dnear; f[9] = dfar; f[10] = f[11] = 0;
            ctx->Unmap(hcb, 0);
        }
        /* in draw calls of 64 quads, each uploaded on its own: ReShade's Generic Depth ignores a frame whose only depth
           buffer had 8 draw calls or fewer (its guard against emulators presenting twice), so 1.1.18's one instanced
           draw a pass went unseen (jayyanz 8 Nov) */
        constexpr size_t kChunk = 64;
        for (size_t first = 0; first < list.size(); first += kChunk) {
            const UINT n = (UINT)(list.size() - first < kChunk ? list.size() - first : kChunk);
            if (FAILED(ctx->Map(hinst_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { failed = true; break; }
            for (UINT k = 0; k < n; k++) memcpy((uint8_t *)m.pData + (size_t)k * m.RowPitch, &list[first + k], sizeof(HInst));
            ctx->Unmap(hinst_buf, 0);
            D3D11_VIEWPORT vp{ drect[0], drect[1], drect[2], drect[3], 0, 1 };
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(rs);
            ctx->OMSetRenderTargets(0, nullptr, dsv);
            ctx->OMSetDepthStencilState(state, ref);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(hvs, nullptr, 0);
            ctx->VSSetShaderResources(0, 1, &hinst_srv);
            ctx->PSSetShader(hps_depth, nullptr, 0);
            ID3D11ShaderResourceView *srvs[3] = { hinst_srv, nullptr, atlas_srv };
            ctx->PSSetShaderResources(0, 3, srvs);
            ctx->PSSetConstantBuffers(0, 1, &hcb);
            ctx->Draw(6 * n, 0);
        }
        ctx->OMSetDepthStencilState(nullptr, 0);
    }

    void hpass(const std::vector<HInst> &list, float d, ID3D11BlendState *blend)
    {
        if (list.empty()) return;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(hcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float *f = (float *)m.pData; f[0] = 640.0f / (float)hw; f[1] = 400.0f / (float)hh; f[2] = d; f[3] = (float)(rgba_srv ? filter : 0);
            f[4] = (float)samples; f[5] = (float)mapping; f[6] = f[7] = 0;
            f[8] = f[9] = f[10] = f[11] = 0;
            ctx->Unmap(hcb, 0);
        }
        for (size_t first = 0; first < list.size(); first += kMaxInst) {
            const UINT n = (UINT)(list.size() - first < (size_t)kMaxInst ? list.size() - first : kMaxInst);
            if (FAILED(ctx->Map(hinst_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { failed = true; break; }
            for (UINT k = 0; k < n; k++) memcpy((uint8_t *)m.pData + (size_t)k * m.RowPitch, &list[first + k], sizeof(HInst));
            ctx->Unmap(hinst_buf, 0);
            D3D11_VIEWPORT vp{ 0, 0, (float)hw, (float)hh, 0, 1 };
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(rs);
            ctx->OMSetRenderTargets(1, &hrtv, nullptr);
            ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(hvs, nullptr, 0);
            ctx->VSSetShaderResources(0, 1, &hinst_srv);
            ctx->PSSetShader(hps, nullptr, 0);
            ID3D11ShaderResourceView *srvs[5] = { hinst_srv, nullptr, atlas_srv, rgba_srv, sky_srv };
            ctx->PSSetShaderResources(0, 5, srvs);
            ctx->PSSetSamplers(0, 4, samp);
            ctx->PSSetConstantBuffers(0, 1, &hcb);
            ctx->Draw(6 * n, 0);
        }
    }
};

Renderer::~Renderer() { delete p_; }

bool Renderer::init(std::string &err, bool allow_hardware, ID3D11Device *shared_dev, ID3D11DeviceContext *shared_ctx)
{
    delete p_; p_ = new Impl;
    D3D_FEATURE_LEVEL got;
    HRESULT r = E_FAIL;
    if (shared_dev && shared_ctx) {
        p_->dev = shared_dev; p_->ctx = shared_ctx; shared_dev->AddRef(); shared_ctx->AddRef();
        p_->shared = true; got = shared_dev->GetFeatureLevel(); driver_ = "shared";
    } else {
    HMODULE d3d = LoadLibraryA("d3d11.dll");
    if (!d3d) { err = "d3d11.dll not found"; return false; }
    PFN_D3D11_CREATE_DEVICE create = (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(d3d, "D3D11CreateDevice");
    if (!create) { err = "D3D11CreateDevice not found"; return false; }
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    if (allow_hardware) r = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &p_->dev, &got, &p_->ctx);
    driver_ = "hardware";
    if (FAILED(r)) {
        r = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &p_->dev, &got, &p_->ctx);
        driver_ = "WARP";
    }
    if (FAILED(r)) { err = "no Direct3D 11 device"; return false; }
    }

    HMODULE cm = LoadLibraryA("d3dcompiler_47.dll");
    pD3DCompile compile = cm ? (pD3DCompile)(void *)GetProcAddress(cm, "D3DCompile") : nullptr;
    if (!compile) { err = "d3dcompiler_47.dll not found"; return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *eb = nullptr;
    const char *profile_vs = got >= D3D_FEATURE_LEVEL_11_0 ? "vs_5_0" : "vs_4_0";
    const char *profile_ps = got >= D3D_FEATURE_LEVEL_11_0 ? "ps_5_0" : "ps_4_0";
    r = compile(kShader, sizeof kShader - 1, "gpu_list", nullptr, nullptr, "vs_main", profile_vs, D3DCOMPILE_IEEE_STRICTNESS, 0, &vsb, &eb);
    if (SUCCEEDED(r)) r = compile(kShader, sizeof kShader - 1, "gpu_list", nullptr, nullptr, "ps_main", profile_ps, D3DCOMPILE_IEEE_STRICTNESS, 0, &psb, &eb);
    if (FAILED(r)) { err = std::string("shader: ") + (eb ? (const char *)eb->GetBufferPointer() : "compile failed"); rel(eb); rel(vsb); rel(psb); return false; }
    bool ok = SUCCEEDED(p_->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &p_->vs)) &&
              SUCCEEDED(p_->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &p_->ps));
    rel(vsb); rel(psb); rel(eb);
    if (!ok) { err = "shader creation failed"; return false; }
    r = compile(kHiresShader, sizeof kHiresShader - 1, "gpu_list_hires", nullptr, nullptr, "vs_main", profile_vs, D3DCOMPILE_IEEE_STRICTNESS, 0, &vsb, &eb);
    if (SUCCEEDED(r)) r = compile(kHiresShader, sizeof kHiresShader - 1, "gpu_list_hires", nullptr, nullptr, "ps_main", profile_ps, D3DCOMPILE_IEEE_STRICTNESS, 0, &psb, &eb);
    if (FAILED(r)) { err = std::string("hires shader: ") + (eb ? (const char *)eb->GetBufferPointer() : "compile failed"); rel(eb); rel(vsb); rel(psb); return false; }
    ok = SUCCEEDED(p_->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &p_->hvs)) &&
         SUCCEEDED(p_->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &p_->hps));
    rel(vsb); rel(psb); rel(eb);
    if (!ok) { err = "hires shader creation failed"; return false; }
    if (SUCCEEDED(compile(kHiresShader, sizeof kHiresShader - 1, "gpu_list_hires", nullptr, nullptr, "ps_depth", profile_ps, D3DCOMPILE_IEEE_STRICTNESS, 0, &psb, &eb))) {
        if (FAILED(p_->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &p_->hps_depth))) p_->hps_depth = nullptr;
    } else depth_error_ = eb ? (const char *)eb->GetBufferPointer() : "compile failed";
    rel(psb); rel(eb);
    {   /* R2-8: depth always written in the game's order; the stencil marks pixels the exact pass wrote */
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.StencilEnable = TRUE; dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
        dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE; dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dd.BackFace = dd.FrontFace;
        if (FAILED(p_->dev->CreateDepthStencilState(&dd, &p_->ds_exact))) p_->ds_exact = nullptr;
        dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_INCR_SAT; dd.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;   /* ref 0: unwritten only, first wins */
        dd.BackFace = dd.FrontFace;
        if (FAILED(p_->dev->CreateDepthStencilState(&dd, &p_->ds_fill))) p_->ds_fill = nullptr;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW; td.Height = kH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16_UINT;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(p_->dev->CreateTexture2D(&td, nullptr, &p_->rt)) || FAILED(p_->dev->CreateRenderTargetView(p_->rt, nullptr, &p_->rtv)) ||
        FAILED(p_->dev->CreateShaderResourceView(p_->rt, nullptr, &p_->rt_srv))) { err = "render target"; return false; }
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(p_->dev->CreateTexture2D(&td, nullptr, &p_->bg)) || FAILED(p_->dev->CreateShaderResourceView(p_->bg, nullptr, &p_->bg_srv))) { err = "background texture"; return false; }
    if (FAILED(p_->dev->CreateTexture2D(&td, nullptr, &p_->tailtex)) || FAILED(p_->dev->CreateShaderResourceView(p_->tailtex, nullptr, &p_->tail_srv))) { err = "tail texture"; return false; }
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(p_->dev->CreateTexture2D(&td, nullptr, &p_->staging))) { err = "staging texture"; return false; }
    td.Width = td.Height = kAtlas; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = 0;
    if (FAILED(p_->dev->CreateTexture2D(&td, nullptr, &p_->atlas)) || FAILED(p_->dev->CreateShaderResourceView(p_->atlas, nullptr, &p_->atlas_srv))) { err = "texture atlas"; return false; }
    {   /* R2-5: the filtering atlas, samplers and premultiplied blending (without them: nearest only) */
        D3D11_TEXTURE2D_DESC rdsc = td;
        rdsc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rdsc.MipLevels = kRgbaLevels;
        bool ok2 = SUCCEEDED(p_->dev->CreateTexture2D(&rdsc, nullptr, &p_->rgba)) && SUCCEEDED(p_->dev->CreateShaderResourceView(p_->rgba, nullptr, &p_->rgba_srv));
        D3D11_SAMPLER_DESC sd{};
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = (float)(kRgbaLevels - 1);
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        ok2 = ok2 && SUCCEEDED(p_->dev->CreateSamplerState(&sd, &p_->samp[0]));
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ok2 = ok2 && SUCCEEDED(p_->dev->CreateSamplerState(&sd, &p_->samp[1]));
        sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 16;
        ok2 = ok2 && SUCCEEDED(p_->dev->CreateSamplerState(&sd, &p_->samp[2]));
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT; sd.MaxAnisotropy = 1; sd.MaxLOD = 0;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        ok2 = ok2 && SUCCEEDED(p_->dev->CreateSamplerState(&sd, &p_->samp[3]));
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok2 = ok2 && SUCCEEDED(p_->dev->CreateBlendState(&bd, &p_->premul));
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        if (ok2 && FAILED(p_->dev->CreateBlendState(&bd, &p_->under))) p_->under = nullptr;
        if (!ok2) { rel(p_->rgba_srv); rel(p_->rgba); }   /* nearest only */
    }
    static_assert(sizeof(Inst) == 4 * 16, "Inst is four float4");
    D3D11_TEXTURE2D_DESC id{};
    id.Width = 4; id.Height = kMaxInst; id.MipLevels = 1; id.ArraySize = 1; id.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    id.SampleDesc.Count = 1; id.Usage = D3D11_USAGE_DYNAMIC; id.BindFlags = D3D11_BIND_SHADER_RESOURCE; id.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(p_->dev->CreateTexture2D(&id, nullptr, &p_->inst_buf)) || FAILED(p_->dev->CreateShaderResourceView(p_->inst_buf, nullptr, &p_->inst_srv))) { err = "instance data"; return false; }
    static_assert(sizeof(HInst) == kHInstW * 16, "HInst is eight float4");
    id.Width = kHInstW;
    if (FAILED(p_->dev->CreateTexture2D(&id, nullptr, &p_->hinst_buf)) || FAILED(p_->dev->CreateShaderResourceView(p_->hinst_buf, nullptr, &p_->hinst_srv))) { err = "instance data (hires)"; return false; }
    id.Width = 4;
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(p_->dev->CreateBuffer(&bd, nullptr, &p_->hcb))) { err = "constant buffer"; return false; }
    }
    id.Width = kRowsPerLine * 3; id.Height = kRowLines;
    if (FAILED(p_->dev->CreateTexture2D(&id, nullptr, &p_->rows_tex)) || FAILED(p_->dev->CreateShaderResourceView(p_->rows_tex, nullptr, &p_->rows_srv))) { err = "row table"; return false; }
    p_->rows.assign((size_t)kMaxRows * 12, 0.0f);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (FAILED(p_->dev->CreateRasterizerState(&rd, &p_->rs))) { err = "rasterizer state"; return false; }
    p_->pending.reserve(kMaxInst);
    return true;
}

void Renderer::begin(const uint16_t *rows, uint32_t pitch)
{
    stats_.quads = stats_.shadows = stats_.outlines = stats_.skipped = 0;
    if (!p_ || p_->failed) return;
    p_->pending.clear();
    p_->frame++;
    pitch_ = pitch;
    p_->hpending.clear();
    if (p_->hw) { const float clear[4] = { 0, 0, 0, 0 }; p_->ctx->ClearRenderTargetView(p_->hrtv, clear); }
    if (p_->dsv) p_->ctx->ClearDepthStencilView(p_->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);   /* 1: far */
    p_->ctx->UpdateSubresource(p_->rt, 0, nullptr, rows, pitch, 0);
    p_->ctx->UpdateSubresource(p_->bg, 0, nullptr, rows, pitch, 0);
    /* a course or screen load re-decodes the textures: start the atlas again (pointers are not part of it, so the
       flip-books do not count) */
    const uint32_t n = rd<uint32_t>(kDescCount) > 4096 ? 4096 : rd<uint32_t>(kDescCount);
    uint32_t h = fnv(&n, 4); const uint32_t cursor = rd<uint32_t>(kCacheCursor); h = fnv(&cursor, 4, h);
    for (uint32_t i = 0; i < n; i++) { uint8_t e[24]; memcpy(e, (const void *)(uintptr_t)(kDescTable + 24 * i), 24); memset(e + 4, 0, 4); h = fnv(e, 24, h); }
    if (h != p_->signature) { p_->signature = h; p_->reset_atlas(); stats_.atlas_resets++; }
}

bool Renderer::quad(uint32_t rec)
{
    if (!p_ || p_->failed) return false;
    const uint32_t num = rd<uint16_t>(rec + 6);
    if (num > 0x300 || !(rd<uint32_t>(kDrawOn) & 1)) { stats_.skipped++; return false; }
    const int di = r2tex::descriptor_for(rd<uint8_t>(rec + 5), num);
    if (di < 0) { stats_.skipped++; return false; }
    const r2tex::Descriptor d = r2tex::descriptor((uint32_t)di);
    const uint8_t flags = rd<uint8_t>(rec + 4);
    /* corners as 0x454C10: int16(trunc(float((x + 176) * sx))) */
    const float sx = rd<float>(kScaleX), sy = rd<float>(kScaleY);
    int16_t xy[8]; float fxy[8];
    for (int i = 0; i < 4; i++) {
        fxy[2 * i] = (float)(((double)rd<float>(rec + 8 + 8 * i) + 176.0) * (double)sx);
        fxy[2 * i + 1] = (float)(((double)rd<float>(rec + 12 + 8 * i) + 127.0) * (double)sy);
        xy[2 * i] = (int16_t)(int32_t)fxy[2 * i];
        xy[2 * i + 1] = (int16_t)(int32_t)fxy[2 * i + 1];
    }
    static const int mode_of[4] = { 2, 0, 3, 1 };
    const int mode = mode_of[(flags >> 2) & 3];
    int32_t ax, ay; uint32_t &tx = stats_.atlas_textures;
    int w, h;
    if (d.ptr) {
        outline_colour_ = rd<uint16_t>(d.ptr);   /* as 0x454C10 sets 0x493620 */
        w = (int)d.w; h = (int)d.h;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { stats_.skipped++; return false; }
        const uint64_t key = (uint64_t)d.ptr << 20 | (uint64_t)w << 10 | (uint64_t)h;
        if (!p_->texture(key, (const uint16_t *)(uintptr_t)d.ptr, w, h, d.ptr == kScroll, ax, ay, tx)) { stats_.skipped++; return false; }
    } else {   /* screen mode 0x1B: decoded from VRAM per draw, keyed by the record */
        std::vector<uint16_t> t; uint32_t tw, th;
        r2tex::decode_vram(d.src, d.colr, d.sat_w, d.sat_h, flags & 1, 127, t, tw, th);
        w = (int)tw; h = (int)th;
        const uint64_t key = (uint64_t)fnv(t.data(), t.size() * 2) << 24 | (uint64_t)tw << 12 | th | 1ull << 63;
        if (!p_->texture(key, t.data(), w, h, false, ax, ay, tx)) { stats_.skipped++; return false; }
    }
    const int cw = w >= 128 ? 127 : w, ch = h >= 128 ? 127 : h;
    const rexact::Tables tb{ (const float *)(uintptr_t)rexact::kRecipTable, (const float *)(uintptr_t)rexact::kRecip65536Table };
    p_->add([&](auto &&emit) { return rexact::rows(xy, mode, cw, ch, tb, emit); }, kQuad, ax, ay, cw, ch, flags & 1, 0);
    if (p_->hw) {   /* the same quad at high resolution: texture corners attached as the rasteriser does (pcmap.h) */
        static const int order[4][4] = { { 3, 2, 1, 0 }, { 2, 3, 0, 1 }, { 0, 1, 2, 3 }, { 1, 0, 3, 2 } };
        const float cu[4] = { 0, (float)cw, (float)cw, 0 }, cv[4] = { 0, 0, (float)ch, (float)ch };
        float u[4], v[4];
        for (int k = 0; k < 4; k++) { const int c = order[mode][k]; u[c] = cu[k]; v[c] = cv[k]; }
        HInst h{};
        float x0 = fxy[0], x1 = fxy[0], y0 = fxy[1], y1 = fxy[1];
        for (int i = 1; i < 4; i++) {
            x0 = fxy[2 * i] < x0 ? fxy[2 * i] : x0; x1 = fxy[2 * i] > x1 ? fxy[2 * i] : x1;
            y0 = fxy[2 * i + 1] < y0 ? fxy[2 * i + 1] : y0; y1 = fxy[2 * i + 1] > y1 ? fxy[2 * i + 1] : y1;
        }
        if (x1 >= -1.0f && x0 <= kW + 1.0f && y1 >= -1.0f && y0 <= kH + 1.0f) {
            const float d = (p_->dsv && p_->widen < 2.0f ? 2.0f : p_->widen) + 0.01f;   /* the depth's wider fill (R2-8) needs 2 */
            h.box[0] = x0 - d; h.box[1] = y0 - d; h.box[2] = x1 + d; h.box[3] = y1 + d;
            for (int i = 0; i < 2; i++) {
                h.p01[2 * i] = fxy[2 * i]; h.p01[2 * i + 1] = fxy[2 * i + 1];
                h.p23[2 * i] = fxy[4 + 2 * i]; h.p23[2 * i + 1] = fxy[5 + 2 * i];
                h.uv01[2 * i] = u[i]; h.uv01[2 * i + 1] = v[i];
                h.uv23[2 * i] = u[2 + i]; h.uv23[2 * i + 1] = v[2 + i];
            }
            h.tex[0] = (float)ax; h.tex[1] = (float)ay; h.tex[2] = (float)cw; h.tex[3] = (float)ch;
            h.misc[0] = kHQuad; h.misc[1] = (flags & 1) ? 1.0f : 0.0f; h.misc[3] = rd<uint32_t>(kMode555) ? 1.0f : 0.0f;
            if (p_->mapping == 2 || p_->dsv) {   /* the corners' depths: the perspective mapping and the depth buffer */
                const int got = persp::lookup(rec, h.pad);
                if (got == 0) h.pad[0] = 0;                            /* none: the PC mapping, no depth */
                else if (got == 1) for (float &z : h.pad) z = -z;      /* cut at the near plane: depth only */
            }
            p_->hpush(h);
        }
    }
    stats_.quads++;
    return true;
}

void Renderer::shadow(uint32_t rec)
{
    if (!p_ || p_->failed) return;
    /* exactly as 0x454B50 and the overlay's triangle fill: rspans::shadow */
    float xy[8];
    for (int i = 0; i < 8; i++) xy[i] = rd<float>(rec + 8 + 4 * i);
    const rspans::Rect vp = rspans::overlay_viewport(rd<int32_t>(kViewFlagD0), rd<int32_t>(kViewFlagF4), rd<int16_t>(kViewW), rd<int16_t>(kViewH));
    std::vector<rspans::Span> tri[2];
    rspans::shadow(xy, rd<float>(kShadowAddX), rd<float>(kShadowAddY), rd<double>(kShadowSX), rd<double>(kShadowSY), vp,
                   [&](int t, const rspans::Span &sp) {
                       /* rows and pixels outside the picture are never there (the corners are clamped) */
                       if (sp.y < 0 || sp.y >= kH) return;
                       int32_t x0 = sp.x < 0 ? 0 : sp.x, x1 = sp.x + sp.n > kW ? kW : sp.x + sp.n;
                       if (x1 > x0) tri[t].push_back(rspans::Span{ sp.y, x0, x1 - x0 });
                   });
    const uint32_t colour = rd<uint32_t>(kMode555) ? 0x8000 : 0x0000;
    for (auto &t : tri) { p_->hruns(t, kHChecker, colour, colour != 0); p_->add_runs(t, kShadow, colour); }
    stats_.shadows++;
}

bool Renderer::outline(uint32_t rec)
{
    if (!p_ || p_->failed) return false;
    const uint32_t num = rd<uint16_t>(rec + 6);
    if (num > 0x300 || !(rd<uint32_t>(kDrawOn) & 1)) { stats_.skipped++; return false; }
    const int di = r2tex::descriptor_for(rd<uint8_t>(rec + 5), num);
    if (di < 0) { stats_.skipped++; return false; }
    const r2tex::Descriptor d = r2tex::descriptor((uint32_t)di);
    if (d.ptr) outline_colour_ = rd<uint16_t>(d.ptr);
    const float sx = rd<float>(kScaleX), sy = rd<float>(kScaleY);
    int16_t xy[8];
    for (int i = 0; i < 4; i++) {
        xy[2 * i] = (int16_t)(int32_t)(float)(((double)rd<float>(rec + 8 + 8 * i) + 176.0) * (double)sx);
        xy[2 * i + 1] = (int16_t)(int32_t)(float)(((double)rd<float>(rec + 12 + 8 * i) + 127.0) * (double)sy);
    }
    const rspans::Rect clip = rspans::outline_clip(rd<int32_t>(kViewFlagD0), rd<int32_t>(kViewFlagF4));
    const uint32_t pp = pitch_ / 2;
    std::vector<rspans::Span> px;
    rspans::outline(xy, clip, [&](int32_t x, int32_t y) {
        /* the clip allows x = 640 and y = 400: the game writes there all the same, which on a 640-pixel pitch is the
           next row's first pixel; past the picture's last row nothing shows */
        if (x >= kW) { if (pp != (uint32_t)kW) return; x -= kW; y += 1; }
        if (y < 0 || y >= kH || x < 0) return;
        px.push_back(rspans::Span{ y, x, 1 });
    });
    /* single pixels to runs */
    std::sort(px.begin(), px.end(), [](const rspans::Span &a, const rspans::Span &b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    std::vector<rspans::Span> runs;
    for (const auto &q : px) {
        if (!runs.empty() && runs.back().y == q.y && runs.back().x + runs.back().n >= q.x) {
            if (q.x + 1 > runs.back().x + runs.back().n) runs.back().n = q.x + 1 - runs.back().x;
        } else runs.push_back(q);
    }
    p_->hruns(runs, kHSolid, outline_colour_, rd<uint32_t>(kMode555) != 0);
    p_->add_runs(runs, kSolid, outline_colour_);
    stats_.outlines++;
    return true;
}

bool Renderer::end(uint16_t *rows, uint32_t pitch)
{
    if (!p_ || p_->failed) return false;
    p_->draw();
    p_->hdraw();
    if (p_->failed) return false;
    p_->ctx->CopyResource(p_->staging, p_->rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(p_->ctx->Map(p_->staging, 0, D3D11_MAP_READ, 0, &m))) { p_->failed = true; return false; }
    for (int y = 0; y < kH; y++) memcpy((uint8_t *)rows + (size_t)y * pitch, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, kW * 2);
    if (keep_picture_) { picture_.resize((size_t)kW * kH); for (int y = 0; y < kH; y++) memcpy(&picture_[(size_t)y * kW], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, kW * 2); }
    p_->ctx->Unmap(p_->staging, 0);
    return true;
}

bool Renderer::set_hires(int w, int h)
{
    if (!p_ || p_->failed) return false;
    return p_->hires_size(w, h);
}

void Renderer::set_widen(float w) { if (p_) p_->widen = w; }
void Renderer::set_verify_textures(bool on) { if (p_) p_->verify = on; }
int Renderer::set_msaa(int n)
{
    if (!p_) return 1;
    p_->samples = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
    return p_->samples;
}
int Renderer::msaa() const { return p_ ? p_->samples : 1; }
void Renderer::set_mapping(int m) { if (p_) p_->mapping = m >= 0 && m <= 2 ? m : 0; }
bool Renderer::set_depth(ID3D11DepthStencilView *dsv, const float rect[4], float znear, float zfar)
{
    if (!p_) return false;
    if (!dsv || !p_->hw || !p_->hps_depth || !p_->ds_exact || !p_->ds_fill) { p_->dsv = nullptr; return dsv == nullptr; }
    p_->dsv = dsv;
    for (int i = 0; i < 4; i++) p_->drect[i] = rect[i];
    p_->dnear = znear; p_->dfar = zfar > znear ? zfar : znear + 1.0f;
    return true;
}

void Renderer::set_sky(const sky::Quad *q, int n, const uint32_t *art, uint32_t version)
{
    if (!p_) return;
    p_->hsky.clear();
    if (!p_->hw || !q || n <= 0 || !art || !p_->samp[3]) return;
    if (!p_->sky_tex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = sky::kArtW; td.Height = sky::kArtH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(p_->dev->CreateTexture2D(&td, nullptr, &p_->sky_tex)) || FAILED(p_->dev->CreateShaderResourceView(p_->sky_tex, nullptr, &p_->sky_srv))) {
            rel(p_->sky_srv); rel(p_->sky_tex); return;
        }
        p_->sky_version = version - 1;
    }
    if (version != p_->sky_version) { p_->ctx->UpdateSubresource(p_->sky_tex, 0, nullptr, art, sky::kArtW * 4, 0); p_->sky_version = version; }
    for (int k = 0; k < n; k++) {
        HInst h{};
        h.box[0] = q[k].l; h.box[1] = q[k].t; h.box[2] = q[k].r; h.box[3] = q[k].b;
        h.p01[0] = q[k].ox; h.p01[1] = q[k].oy; h.p01[2] = q[k].u0; h.p01[3] = q[k].v0;
        h.misc[0] = (float)kHSky;
        p_->hsky.push_back(h);
    }
}
void Renderer::set_filter(int f) { if (p_) p_->filter = f < 0 ? 0 : f > 4 ? 4 : f; }

int Renderer::hires_width() const { return p_ ? p_->hw : 0; }
int Renderer::hires_height() const { return p_ ? p_->hh : 0; }
ID3D11ShaderResourceView *Renderer::hires_view() const { return p_ ? p_->hsrv : nullptr; }
ID3D11ShaderResourceView *Renderer::picture_view() const { return p_ ? p_->rt_srv : nullptr; }
ID3D11ShaderResourceView *Renderer::background_view() const { return p_ ? p_->bg_srv : nullptr; }
ID3D11ShaderResourceView *Renderer::tail_view() const { return p_ ? p_->tail_srv : nullptr; }
void Renderer::upload_tail(const uint16_t *rows, uint32_t pitch)
{
    if (p_ && p_->tailtex) p_->ctx->UpdateSubresource(p_->tailtex, 0, nullptr, rows, pitch, 0);
}

bool Renderer::read_hires(std::vector<uint32_t> &bgra)
{
    if (!p_ || p_->failed || !p_->hw) return false;
    if (!p_->hstaging) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = p_->hw; td.Height = p_->hh; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(p_->dev->CreateTexture2D(&td, nullptr, &p_->hstaging))) return false;
    }
    p_->ctx->CopyResource(p_->hstaging, p_->hrt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(p_->ctx->Map(p_->hstaging, 0, D3D11_MAP_READ, 0, &m))) return false;
    bgra.resize((size_t)p_->hw * p_->hh);
    for (int y = 0; y < p_->hh; y++) memcpy(&bgra[(size_t)y * p_->hw], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)p_->hw * 4);
    p_->ctx->Unmap(p_->hstaging, 0);
    return true;
}

}  // namespace gpulist
