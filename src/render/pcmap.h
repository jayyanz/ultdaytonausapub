/* Resolution-free quad mapping for the new renderer: which texel each point of a quad gets, matching the
   original 1996 PC picture. Written in single-precision float with the same operations as the HLSL pixel shader
   (shaders/pcmap.hlsli mirrors it line for line), so `replay raster` measures what the GPU will draw.

   Outline: corners are sorted so corner 0 is the top one; rows run from the top corner to the lowest; on each row
   both edges give x, u and v, x is rounded to whole pixels and u, v are stepped in 16.16 across the row. On a
   mirrored texture axis the texel is counted from the far side. */
#pragma once
#include <math.h>
#include <stdint.h>

namespace pcmap {

struct Quad {
    float x[4], y[4], u[4], v[4];   /* sorted: 0 top, 1 the upper neighbour, 2 opposite, 3 the other neighbour;
                                       u, v in 16.16, counted from the far side on a mirrored axis */
    int y_top, y_bottom;
    int w, h;                       /* texture size */
    bool mirror_u, mirror_v;        /* this texture axis is walked backwards */
};

inline float round_ne(float f) { return nearbyintf(f); }   /* nearest, ties to even, as HLSL round() */

/* xy: record corners A B C D as integers (x, y pairs); mode: corner mode 0..3 (from flags 2-3). */
inline void setup(const int16_t xy[8], int mode, int w, int h, Quad &q)
{
    static const int order[4][4] = { { 3, 2, 1, 0 }, { 2, 3, 0, 1 }, { 0, 1, 2, 3 }, { 1, 0, 3, 2 } };
    const float W = ((float)w - 0.01f) * 65536.0f, H = ((float)h - 0.01f) * 65536.0f;
    const float cu[4] = { 0, W, W, 0 }, cv[4] = { 0, 0, H, H };
    int px[4], py[4]; float pu[4], pv[4];
    for (int k = 0; k < 4; k++) {
        const int c = order[mode & 3][k];
        px[k] = xy[2 * c]; py[k] = xy[2 * c + 1]; pu[k] = cu[k]; pv[k] = cv[k];
    }
    /* sort by packed y:x, keeping the corners in cycle order */
    auto key = [&](int i) { return (int32_t)((uint32_t)(uint16_t)py[i] << 16 | (uint16_t)px[i]); };
    int idx[4] = { 0, 1, 2, 3 }, flags = 0;
    auto sw = [&](int a, int b) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; };
    if (key(idx[0]) > key(idx[1])) { sw(0, 1); sw(2, 3); flags |= 8; }
    if (key(idx[0]) > key(idx[2])) { sw(0, 2); flags |= 4; }
    if (key(idx[0]) > key(idx[3])) { sw(0, 3); sw(1, 2); flags |= 2; }
    if (key(idx[1]) > key(idx[3])) { sw(1, 3); flags |= 1; }
    /* orientation table: bit 1 walks texture columns backwards, bit 2 rows */
    static const uint8_t orient[16] = { 0, 1, 4, 5, 7, 6, 3, 4, 2, 3, 6, 7, 3, 4, 7, 6 };
    q.mirror_u = (orient[flags] & 2) != 0; q.mirror_v = (orient[flags] & 4) != 0;
    q.w = w; q.h = h;
    for (int k = 0; k < 4; k++) {
        q.x[k] = (float)px[idx[k]]; q.y[k] = (float)py[idx[k]];
        q.u[k] = q.mirror_u ? W - pu[idx[k]] : pu[idx[k]];
        q.v[k] = q.mirror_v ? H - pv[idx[k]] : pv[idx[k]];
    }
    q.y_top = (int)q.y[0];
    q.y_bottom = (int)fmaxf(q.y[2], q.y[3]);
}

/* A point on edge a->b at row yr: x, u, v linear in y. */
inline void edge_at(const Quad &q, int a, int b, float yr, float &x, float &u, float &v)
{
    const float dy = q.y[b] - q.y[a];
    const float t = dy != 0.0f ? (yr - q.y[a]) / dy : 0.0f;
    x = q.x[a] + (q.x[b] - q.x[a]) * t;
    u = q.u[a] + (q.u[b] - q.u[a]) * t;
    v = q.v[a] + (q.v[b] - q.v[a]) * t;
}

/* Does the quad cover pixel (px, py) of the 640x400 picture, and with which texel. */
inline bool texel_at(const Quad &q, int px, int py, int &tu, int &tv)
{
    if (py < q.y_top || py > q.y_bottom || py < 0 || py >= 400 || px < 0 || px >= 640) return false;
    const float yr = (float)py;
    const int y1 = (int)q.y[1], y2 = (int)q.y[2], y3 = (int)q.y[3];
    const int ymid = y2 < y3 ? y2 : y3;
    int a0, a1, b0, b1;                       /* edges a0->a1 and b0->b1 for this row */
    if (py < y1) { a0 = 0; a1 = 1; b0 = 0; b1 = 3; }
    else if (py < ymid) { a0 = 1; a1 = 2; b0 = 0; b1 = 3; }
    else if (y2 <= y3) { a0 = 2; a1 = 3; b0 = 0; b1 = 3; }
    else { a0 = 1; a1 = 2; b0 = 3; b1 = 2; }
    float xa, ua, va, xb, ub, vb;
    edge_at(q, a0, a1, yr, xa, ua, va);
    edge_at(q, b0, b1, yr, xb, ub, vb);
    float xl = round_ne(xa), xr = round_ne(xb), ul = ua, vl = va, ur = ub, vr = vb;
    if (xr < xl) { float t = xl; xl = xr; xr = t; ul = ub; vl = vb; ur = ua; vr = va; }
    if ((float)px < xl || (float)px > xr) return false;
    const float n = xr - xl + 1.0f;
    const int32_t du = (int32_t)round_ne((ur - ul) * (1.0f / n)), dv = (int32_t)round_ne((vr - vl) * (1.0f / n));
    const int32_t k = (int32_t)((float)px - xl);
    const int32_t fu = (int32_t)round_ne(ul) + k * du, fv = (int32_t)round_ne(vl) + k * dv;
    tu = q.mirror_u ? q.w - 1 - (fu >> 16) : fu >> 16;
    tv = q.mirror_v ? q.h - 1 - (fv >> 16) : fv >> 16;
    return true;
}

}  // namespace pcmap
