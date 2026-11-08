/*
 * The sky at high resolution (R2-5, build 1.1.9; jayyanz 7 Nov 2026: "the sky will need to be filtered too").
 *
 * The game builds its sky panorama (900x400 of art shown at 2x) into eight 640x400 page surfaces and copies
 * parts of them to the top of the picture before the 3D, up to 8 times a frame, scrolled by heading and pitch.
 * Here those 8 calls come to sky_copy(), which records each copy as a quad in picture units and then calls the
 * game's own copy, so the 640x400 picture is unchanged. The new renderer draws those quads first at high
 * resolution from the art (rebuilt from the pages and checked), filtered, wrapping round the 900 art columns.
 */
#include "sky.h"
#include "../proxy.h"
#include "../ddraw_own.h"
#include <string.h>

namespace sky {
namespace {

constexpr DWORD kCopy = 0x457BE0, kPages = 0x4C86F8, kHorizon = 0x11C1980, kMode555 = 0x48ABEC;
constexpr DWORD kCalls[8] = { 0x416393, 0x4163BA, 0x4163D4, 0x4163FB, 0x416422, 0x416449, 0x416470, 0x41649F };

BYTE *g_base;
template <typename T> T rd(DWORD va) { return *(T *)(g_base + (va - 0x400000)); }

Quad g_quads[32];
int g_count;

uint32_t g_art[kArtW * kArtH];
uint32_t g_version, g_hash;
bool g_art_ok;

void add(int page, int dx, int dy, int sl, int st, int sr, int sb)
{
    if (sr <= sl || sb <= st) return;
    if (g_count >= (int)(sizeof g_quads / sizeof g_quads[0])) g_count = 0;   /* not taken (renderer off): start again */
    Quad &q = g_quads[g_count++];
    q.l = (float)dx; q.t = (float)dy; q.r = (float)(dx + sr - sl); q.b = (float)(dy + sb - st);
    if (q.r > 640) q.r = 640;
    if (q.b > 400) q.b = 400;
    q.ox = (float)(dx - sl); q.oy = (float)(dy - st);          /* where page texel (0, 0) lands */
    q.u0 = (float)((page & 3) * 225); q.v0 = page >= 4 ? 200.0f : 0.0f;
}

/* the game's sky copy, called the same way (cdecl, three words) */
void __cdecl sky_copy(int x, int y, int page)
{
    short xs = (short)x, ys = (short)y, ps = (short)page;
    if (ys > -59) ys = -59;
    int sl = 0, st = -ys, sr = 453, sb = 400, dx = xs;
    if (xs < 0) { sl = -xs; dx = 0; }
    if (xs + 453 > 640) sr = 640 - xs;
    if (ps >= 0 && ps < 4) {
        add(ps, dx, 0, sl, st, sr, sb);
        const int dy = ys + 400;
        float lim = (float)(2 * rd<int>(kHorizon));
        if (!(lim < 399.0f)) lim = 399.0f;
        int sb2 = 400;
        if (!((float)(dy + 400) <= lim)) sb2 = (int)(lim - (float)dy);
        add(ps + 4, dx, dy, sl, 0, sr, sb2);
    }
    ((void (__cdecl *)(int, int, int))(g_base + (kCopy - 0x400000)))(x, y, page);
}

uint32_t expand(uint16_t c, bool is555)
{
    uint32_t r, g, b;
    if (is555) { r = (c >> 10) & 31; g = (c >> 5) & 31; b = c & 31; r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); }
    else { r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); }
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* the art from the pages when they change (pointers or a sparse hash of their pixels) */
bool refresh()
{
    const BYTE *bits[8]; int pitch[8];
    uint32_t h = 2166136261u;
    for (int p = 0; p < 8; p++) {
        int w, ht;
        const void *s = rd<const void *>(kPages + 4 * p);
        if (!own_surface_bits(s, &bits[p], &w, &ht, &pitch[p]) || w < 454 || ht < 400) return false;
        h = (h ^ (uint32_t)(uintptr_t)s) * 16777619u;
        for (int y = 3; y < 400; y += 13)
            for (int x = 5; x < 450; x += 29) h = (h ^ ((const uint16_t *)(bits[p] + y * pitch[p]))[x]) * 16777619u;
    }
    if (g_art_ok && h == g_hash) return true;
    g_hash = h;
    const bool is555 = rd<uint32_t>(kMode555) != 0;
    int pairs = 0, odd = 0;
    for (int p = 0; p < 4; p++)
        for (int half = 0; half < 2; half++) {
            const BYTE *b = bits[p + 4 * half]; const int pt = pitch[p + 4 * half];
            for (int v = 0; v < 200; v++) {
                const uint16_t *r0 = (const uint16_t *)(b + (2 * v) * pt), *r1 = (const uint16_t *)(b + (2 * v + 1) * pt);
                uint32_t *out = &g_art[(size_t)(half * 200 + v) * kArtW + p * 225];
                for (int u = 0; u < 225; u++) {
                    const uint16_t c = r0[2 * u];
                    if ((u & 7) == 0) { pairs++; if (r0[2 * u + 1] != c || r1[2 * u] != c) odd++; }
                    out[u] = expand(c, is555);
                }
            }
        }
    g_art_ok = odd * 50 <= pairs;          /* the 2x layout (allow a few edits; 2% at most) */
    g_version++;
    plog("sky: panorama rebuilt from the pages (%s; %d of %d pixel pairs not doubled)", g_art_ok ? "2x layout" : "not the 2x layout, the game's own sky kept", odd, pairs);
    return g_art_ok;
}

}  // namespace

bool install(BYTE *game_base)
{
    g_base = game_base;
    for (DWORD a : kCalls) {
        const BYTE *p = game_base + (a - 0x400000);
        LONG rel; memcpy(&rel, p + 1, 4);
        if (p[0] != 0xE8 || a + 5 + rel != kCopy) { plog("sky: unexpected code at %08lX, the game's sky kept", a); return false; }
    }
    for (DWORD a : kCalls) {
        BYTE *p = game_base + (a - 0x400000);
        LONG rel = (LONG)((DWORD)(ULONG_PTR)fnptr(sky_copy) - (a + 5));
        DWORD old; VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p + 1, &rel, 4);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    plog("sky: copies hooked (8 calls to 0x457BE0)");
    return true;
}

bool take(const Quad **quads, int *count, const uint32_t **art, uint32_t *version)
{
    const int n = g_count;
    g_count = 0;
    if (!g_base || n == 0 || !refresh()) return false;
    static Quad copy[32];
    memcpy(copy, g_quads, sizeof(Quad) * n);
    *quads = copy; *count = n; *art = g_art; *version = g_version;
    return true;
}

}  // namespace sky
