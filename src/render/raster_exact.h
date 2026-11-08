/* Pixel-exact quad coverage at 640x400: for each row, which pixels a quad covers and which texel each gets, so
   the new renderer matches the original picture at the game's own resolution (R2-3). pcmap.h is the
   resolution-free approximation.

   Outline:
   - Corners are sorted as in pcmap.h; a small orientation table picks the texture walk.
   - Rows are walked from the top corner to the lowest in three runs, with two edges carrying x, U and V.
   - Each row is clipped to 0..639 and handed to the caller; texel() gives the texel for any pixel of the row.

   Arithmetic uses doubles for intermediate values and floats for stored values, so results match the original
   bit for bit. Build with SSE2 doubles. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace rexact {

/* One row of a quad: position, length and the values texel() needs. fv, dv, fu, du hold the fractions at the
   precision texel() uses (16 bits when adv is 0, otherwise 8). */
struct Row {
    int32_t y, x, n;            /* picture row, first pixel, pixel count (already clipped) */
    int32_t t0, adv, sv, su;    /* in texels from the texture's first texel */
    int32_t fv, dv, fu, du;
};

struct Tables { const float *recip, *recip65536; };   /* 1/n and 65536/n tables, read from the game's memory */
constexpr uint32_t kRecipTable = 0x112E5A0, kRecip65536Table = 0x11B25A0, kRecipCount = 0x21000;

inline int32_t fist(double v) { return (int32_t)nearbyint(v); }

/* Calls row(const Row &) for every visible row of the quad. Returns false for a quad with nothing to draw. */
template <typename RowFn>
bool rows(const int16_t xy[8], int mode, int w, int h, const Tables &tb, RowFn &&row)
{
    static const int order[4][4] = { { 3, 2, 1, 0 }, { 2, 3, 0, 1 }, { 0, 1, 2, 3 }, { 1, 0, 3, 2 } };
    static const uint8_t orient[16] = { 0, 1, 4, 5, 7, 6, 3, 4, 2, 3, 6, 7, 3, 4, 7, 6 };
    auto rc = [&](int n) -> double { return (unsigned)n < kRecipCount ? (double)tb.recip[n] : 0.0; };
    auto r65 = [&](int n) -> double { return (unsigned)n < kRecipCount ? (double)tb.recip65536[n] : 0.0; };

    int32_t p[4];
    for (int k = 0; k < 4; k++) { const int c = order[mode & 3][k]; p[k] = (int32_t)((uint32_t)(uint16_t)xy[2 * c + 1] << 16 | (uint16_t)xy[2 * c]); }
    int flags = 0; int32_t t;
    if (p[0] > p[1]) { flags |= 8; t = p[0]; p[0] = p[1]; p[1] = t; t = p[2]; p[2] = p[3]; p[3] = t; }
    if (p[0] > p[2]) { flags |= 4; t = p[0]; p[0] = p[2]; p[2] = t; }
    if (p[0] > p[3]) { flags |= 2; t = p[0]; p[0] = p[3]; p[3] = t; t = p[1]; p[1] = p[2]; p[2] = t; }
    if (p[1] > p[3]) { flags |= 1; t = p[1]; p[1] = p[3]; p[3] = t; }
    const int32_t X[4] = { (int16_t)p[0], (int16_t)p[1], (int16_t)p[2], (int16_t)p[3] };
    const int32_t Y[4] = { p[0] >> 16, p[1] >> 16, p[2] >> 16, p[3] >> 16 };
    const float fx[4] = { (float)X[0], (float)X[1], (float)X[2], (float)X[3] }, fy[4] = { (float)Y[0], (float)Y[1], (float)Y[2], (float)Y[3] };
    const bool swap23 = Y[2] > Y[3];
    const int32_t ymin = swap23 ? Y[3] : Y[2], ymax = swap23 ? Y[2] : Y[3];
    if (Y[0] > ymax) return false;

    /* texture walk */
    const int al = orient[flags];
    const float f770 = (float)((al & 1) ? h : w), f774 = (float)((al & 1) ? w : h);
    const int32_t rowt = w;                           /* texels per texture row */
    int32_t d0 = 0, c0 = rowt, c4 = 1;               /* in texels */
    if (al & 4) { d0 += (h - 1) * rowt; c0 = -rowt; }
    if (al & 2) { d0 += rowt - 1; c4 = -1; }
    if (al & 1) { t = c0; c0 = c4; c4 = t; }

    const double k01 = 0.009999999776482582;          /* 0.01 as a float */
    const double E770 = (double)f770 - k01, E774 = (double)f774 - k01;
    const float f778 = (float)((double)(Y[1] - Y[0]) * rc(ymin - Y[0]) * E774);
    const float f77c = (float)((double)(ymin - Y[1]) * rc(ymax - Y[1]) * E774);
    auto slope = [&](int a, int b) -> float {
        const int32_t dy = Y[b] - Y[a];
        if ((uint32_t)dy >= 0x800) return (float)(((double)fx[b] - fx[a]) / ((double)fy[b] - fy[a]));
        return (float)(((double)fx[b] - fx[a]) * rc(dy));
    };

    /* edge a: xa, slope sa, ua / va with steps dua / dva; edge b likewise */
    float xa = fx[1], xb = fx[0], sa = 0, sb = slope(0, 3);
    float ua = 0, va = 0, ub = 0, vb = 0, dua = 0, dva = 0, dub = 0, dvb = 0;
    int32_t y = Y[0];

    auto run = [&](int32_t end) {
        double ra = xa, rb = xb;                       /* x in registers for the run */
        for (; y < end; y++) {
            if ((uint32_t)y < 400u) {
                const int32_t ia = fist(ra), ib = fist(rb);
                int32_t xs, n, U, V, dU, dV;
                if (ia < ib) { xs = ia; n = ib + 1 - ia; U = fist(ua); V = fist(va); dU = fist(((double)ub - ua) * rc(n)); dV = fist(((double)vb - va) * rc(n)); }
                else         { xs = ib; n = ia + 1 - ib; U = fist(ub); V = fist(vb); dU = fist(((double)ua - ub) * rc(n)); dV = fist(((double)va - vb) * rc(n)); }
                bool draw = xs < 640;
                if (draw && xs < 0) {
                    const int32_t k0 = -xs; n -= k0;
                    if (n <= 0) draw = false;
                    U = (int32_t)((uint32_t)U + (uint32_t)dU * (uint32_t)k0); V = (int32_t)((uint32_t)V + (uint32_t)dV * (uint32_t)k0);
                    xs = 0;
                }
                if (draw) {
                    if (n > 640 - xs) n = 640 - xs;
                    Row r;
                    r.y = y; r.x = xs; r.n = n;
                    r.t0 = d0 + (U >> 16) * c4 + (V >> 16) * c0;
                    /* negative steps: negate the value, the step and the direction */
                    const int32_t mv = dV >> 31, mu = dU >> 31;
                    const int32_t Vn = (V ^ mv) - mv, dVn = (dV ^ mv) - mv, c0n = (c0 ^ mv) - mv;
                    const int32_t Un = (U ^ mu) - mu, dUn = (dU ^ mu) - mu, c4n = (c4 ^ mu) - mu;
                    r.adv = c0n * (int32_t)(((uint32_t)dVn >> 16) & 0x7FFF) + c4n * (int32_t)(((uint32_t)dUn >> 16) & 0x7FFF);
                    r.sv = c0n; r.su = c4n;
                    if (r.adv == 0) { r.fv = Vn & 0xFFFF; r.dv = dVn & 0xFFFF; r.fu = Un & 0xFFFF; r.du = dUn & 0xFFFF; }
                    else { r.fv = (Vn >> 8) & 0xFF; r.dv = (dVn >> 8) & 0xFF; r.fu = (Un >> 8) & 0xFF; r.du = (dUn >> 8) & 0xFF; }
                    row(r);
                }
            }
            ra = ra + sa; rb = rb + sb;
            ub = (float)((double)ub + dub); vb = (float)((double)vb + dvb);
            ua = (float)((double)ua + dua); va = (float)((double)va + dva);
        }
        xa = (float)ra; xb = (float)rb;
    };

    /* run 1: edges 0-1 and 0-3 */
    if (Y[0] < Y[1]) {
        ub = 0; dub = 0; vb = 0; ua = 0; dva = 0; ua = 0; va = 0;
        dvb = (float)((double)f778 * r65(Y[1] - Y[0]));
        dua = (float)(E770 * r65(Y[1] - Y[0]));
        sa = slope(0, 1); xa = fx[0];
        run(Y[1]);
    }
    /* run 2: edges 1-2 and (still) 0-3 */
    bool ran2 = false;
    if (Y[1] < ymin) {
        const int32_t n = ymin - Y[1];
        if (!swap23) {
            ub = 0; dub = 0; dua = 0; va = 0;
            dvb = (float)(((double)f77c - f778) * r65(n));
            ua = (float)(E770 * 65536.0);
            vb = (float)((double)f778 * 65536.0);
            dva = (float)(E774 * r65(n));
        } else {
            ub = 0; dub = 0; dua = 0; va = 0;
            vb = (float)((double)f778 * 65536.0);
            dvb = (float)((E774 - f778) * r65(n));
            dva = (float)((double)f77c * r65(n));
            ua = (float)(E770 * 65536.0);
        }
        ran2 = true;
        sa = slope(1, 2); xa = fx[1];
        run(ymin);
    }
    /* run 3: down to the lowest corner, inclusive */
    {
        const int32_t n = ymax - ymin;
        if (!swap23) {
            dub = 0; ub = 0; dvb = 0;
            const double e = E770 * 65536.0;
            ua = (float)e;
            dua = (float)(-e * rc(n));
            va = (float)(E774 * 65536.0);
            vb = (float)((double)f77c * 65536.0);
            dvb = (float)((E774 - f77c) * r65(n));
            dva = 0;
        } else {
            dua = 0; ub = 0; dvb = 0;
            const double e = E770 * 65536.0;
            ua = (float)e;
            dub = (float)(e * rc(n));
            vb = (float)(E774 * 65536.0);
            va = (float)((double)f77c * 65536.0);
            dva = (float)(((double)f774 - f77c) * r65(n));
        }
        const float s = slope(2, 3);
        if (!swap23) { sa = s; xa = fx[2]; }
        else {
            sb = s; xb = fx[3];
            if (!ran2) sa = slope(1, 2);
        }
        run(ymax + 1);
    }
    return true;
}

inline int32_t texel(const Row &r, int32_t k)
{
    if (r.adv == 0) return r.t0 + r.sv * ((r.fv + k * r.dv) >> 16) + r.su * ((r.fu + k * r.du) >> 16);
    const int32_t j = (k > 0 && ((r.x + k) & 1)) ? k - 1 : k;
    return r.t0 + j * r.adv + r.sv * ((r.fv + j * r.dv) >> 8) + r.su * ((r.fu + j * r.du) >> 8) + (j != k ? r.adv : 0);
}

}  // namespace rexact
