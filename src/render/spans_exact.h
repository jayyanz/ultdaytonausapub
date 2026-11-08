/* Car shadows and SeeThrough outlines for the new renderer (Release 2, R2-4). Both produce pixels or row spans
   only, matching the original picture exactly; the GPU fills them. Header only; plain C++. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

namespace rspans {

/* Truncation toward zero. */
inline int32_t ftol(double v) { return (int32_t)v; }

/* A rectangle: left, top, right, bottom. */
struct Rect { int32_t l, t, r, b; };

/* ---- car shadows ---- */

/* One horizontal run of a shadow triangle: pixels x .. x + n - 1 of row y, of which those with (x + y) even are
   written. */
struct Span { int32_t y, x, n; };

/* One part of a shadow triangle: rows from ytop to ybot (16.16), left and right edges from xl / xr with
   slopes dl / dr. */
template <typename Emit>
inline void shadow_part(int32_t xl, int32_t xr, int32_t ytop, int32_t ybot, int32_t dl, int32_t dr, Emit &&emit)
{
    const int32_t row0 = (ytop + 0x8000) >> 16;
    const int32_t rows = ((ybot - ytop) + 0x8000) >> 16;
    for (int32_t i = 0; i < rows; i++) {
        const int32_t xs = (xl + 0x8000) >> 16;
        const int32_t n = (((xr - xl) + 0x8000) >> 16) + 1;
        if (n > 0) emit(Span{ row0 + i, xs, n });
        xl += dl; xr += dr;
    }
}

/* One shadow triangle (x, y pairs, whole pixels). */
template <typename Emit>
inline void shadow_triangle(int32_t p[6], Emit &&emit)
{
    auto swap = [&](int a, int b) { int32_t x = p[2 * a], y = p[2 * a + 1]; p[2 * a] = p[2 * b]; p[2 * a + 1] = p[2 * b + 1]; p[2 * b] = x; p[2 * b + 1] = y; };
    if (p[1] > p[3]) swap(0, 1);
    if (p[5] < p[3]) swap(1, 2);
    if (p[1] > p[3]) swap(0, 1);
    if (p[5] == p[1]) return;
    const int32_t L = ftol((double)((p[4] - p[0]) << 16) / (double)(p[5] - p[1]));   /* long edge */
    int32_t xa = p[0] << 16, y = p[1] << 16;
    if (p[1] < p[3]) {   /* upper part: both edges start at the top point */
        const int32_t S = ftol((double)((p[2] - p[0]) << 16) / (double)(p[3] - p[1]));
        const int32_t ybot = p[3] << 16;
        shadow_part(xa, xa, y, ybot, S > L ? L : S, S > L ? S : L, emit);
        xa += (((ybot - y) + 0x8000) >> 16) * L;
        y = ybot;
    }
    if (p[5] == p[3]) return;
    const int32_t S = ftol((double)((p[4] - p[2]) << 16) / (double)(p[5] - p[3]));
    const int32_t xs = p[2] << 16, ybot = p[5] << 16;
    if (xs > xa) shadow_part(xa, xs, y, ybot, L, S, emit);   /* the short edge on the right */
    else shadow_part(xs, xa, y, ybot, S, L, emit);
}

/* The overlay viewport. */
inline Rect overlay_viewport(int32_t flag_d0, int32_t flag_f4, int32_t w, int32_t h)
{
    if (!flag_d0 && flag_f4) return Rect{ 0, 0, 0x160, 0xE0 };
    return Rect{ 0, 0, w, h };
}

/* A shadow record's corners to two triangles' spans. rec_xy: the record's four float x, y pairs; add_x / add_y
   and kx / ky: the offsets and scales the caller reads from the game. emit(tri, Span) with tri 0 or 1. */
template <typename Emit>
inline void shadow(const float rec_xy[8], float add_x, float add_y, double kx, double ky, Rect vp, Emit &&emit)
{
    int16_t c[8];
    for (int i = 0; i < 4; i++) {
        c[2 * i] = (int16_t)ftol((double)rec_xy[2 * i] + (double)add_x);
        c[2 * i + 1] = (int16_t)ftol((double)rec_xy[2 * i + 1] + (double)add_y);
    }
    const double sx = (double)(vp.r - vp.l) * kx, sy = (double)(vp.b - vp.t) * ky;
    static const int order[4] = { 0, 1, 2, 3 };   /* corners in record order */
    int32_t q[8];
    for (int i = 0; i < 4; i++) {
        int32_t x = ftol((double)c[2 * order[i]] * sx), y = ftol((double)c[2 * order[i] + 1] * sy);
        if (vp.l > x) x = vp.l;
        if (vp.t > y) y = vp.t;
        if (vp.r <= x) x = vp.r - 1;
        if (vp.b <= y) y = vp.b - 1;
        q[2 * i] = x; q[2 * i + 1] = y;
    }
    int32_t a[6] = { q[0], q[1], q[2], q[3], q[4], q[5] };
    int32_t b[6] = { q[0], q[1], q[4], q[5], q[6], q[7] };
    shadow_triangle(a, [&](const Span &s) { emit(0, s); });
    shadow_triangle(b, [&](const Span &s) { emit(1, s); });
}

/* ---- SeeThrough outlines ---- */

/* The outline clip; inclusive on all four sides. */
inline Rect outline_clip(int32_t flag_d0, int32_t flag_f4)
{
    if (flag_d0) return Rect{ 0, 0, 0x140, 0xC7 };
    if (flag_f4) return Rect{ 0, 0, 0x15F, 0xDF };
    return Rect{ 0, 0, 0x280, 0x190 };
}

/* A line from (x0, y0) towards (x1, y1), end point left out; emit(x, y) for each pixel inside the clip. */
template <typename Emit>
inline void outline_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Rect c, Emit &&emit)
{
    const int32_t dx = x1 - x0, dy = y1 - y0;
    const int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    const int32_t sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    auto plot = [&](int32_t x, int32_t y) {
        x = (int16_t)x; y = (int16_t)y;
        if (c.l <= x && x <= c.r && c.t <= y && y <= c.b) emit(x, y);
    };
    int32_t x = x0, y = y0;
    if (ady <= adx) {
        int32_t err = -adx;
        for (int32_t n = adx; n > 0; n--) {
            plot(x, y);
            x += sx; err += 2 * ady;
            if (err >= 0) { y += sy; err -= 2 * adx; }
        }
    } else {
        int32_t err = -ady;
        for (int32_t n = ady; n > 0; n--) {
            plot(x, y);
            y += sy; err += 2 * adx;
            if (err >= 0) { x += sx; err -= 2 * ady; }
        }
    }
}

/* The four sides c0-c1, c1-c2, c2-c3, c3-c0 of a record's corners. */
template <typename Emit>
inline void outline(const int16_t xy[8], Rect c, Emit &&emit)
{
    for (int i = 0; i < 4; i++) {
        const int j = (i + 1) & 3;
        outline_line(xy[2 * i], xy[2 * i + 1], xy[2 * j], xy[2 * j + 1], c, emit);
    }
}

}  // namespace rspans
