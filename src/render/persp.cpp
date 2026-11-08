/* R2-7, the perspective-correct mapping (1.1.16): each display-list record's true depths.
 *
 * The records hold only the four projected corners (x = 160 X / Z, y = -160 Y / Z, Saturn units about the centre).
 * When a record is written, the camera-space vertex of each corner is available at kVerts. A hook at each of the
 * five places that store the new record count (kSites) copies the four depths, and the record's corners (to know
 * later that the record is still the one they belong to), into a side array indexed like the records.
 * Each hook site becomes a call to a small trampoline that runs the replaced instruction (copied unchanged; it is
 * not relative), then saves every register and the flags, stores its site's number, calls capture() and returns.
 *
 * Sites 1 and 3 cut polygons at the near plane and still map the whole texture. A corner whose stored vertex does
 * not project onto it is a cut point: it gets the depth where its screen ray meets the polygon's plane (for the
 * depth buffer, R2-8) and the record keeps the original mapping. The other sites store only z (x and y there are
 * stale) and do not cut. Records written elsewhere (menus, shadows) have no entry or a stale one; lookup() checks
 * the corners and says no. capture() uses SSE arithmetic only (the proxy is built with -mfpmath=sse): the game's
 * x87 stack and control word are untouched.
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../proxy.h"
#include "persp.h"

namespace persp {
namespace {

constexpr DWORD kRecords = 0xC464B0, kCount = 0x4931A8, kVerts = 0x11D5780, kNear = 0x4D5180;
constexpr int kMax = 1800;

struct Site { DWORD at; BYTE len; BYTE code[7]; const char *what; };
/* Each site is a 6-byte store of the new record count, with no branch landing inside it. The record is complete by
   then. */
const Site kSites[5] = {
    { 0x453C1F, 6, { 0x89, 0x15, 0xA8, 0x31, 0x49, 0x00 }, "course" },
    { 0x41353C, 6, { 0x89, 0x0D, 0xA8, 0x31, 0x49, 0x00 }, "course, clipping" },
    { 0x41EB4F, 6, { 0x89, 0x0D, 0xA8, 0x31, 0x49, 0x00 }, "course 2" },
    { 0x435C8E, 6, { 0x89, 0x0D, 0xA8, 0x31, 0x49, 0x00 }, "course 2, clipping" },
    { 0x406CCA, 6, { 0x89, 0x15, 0xA8, 0x31, 0x49, 0x00 }, "models" },
};

BYTE *g_base;
template <typename T> T &at(DWORD va) { return *(T *)(g_base + (va - 0x400000)); }

struct Entry { float xy[8]; float z[4]; bool ok, depth; };   /* ok: for the mapping; depth: every z a depth (1.1.18) */
Entry g_e[kMax];
bool g_on;
unsigned g_captured, g_near, g_checked, g_matched, g_cut;
float g_zmin = 1e30f, g_zmax = 0, g_znear = 0;
#ifdef ULT_DEBUG
unsigned g_hist[16];   /* corners' depths by halves of a power of ten from 100 (tests) */
#endif

} // namespace

int g_site;   /* the hook site that called (kSites index), set by its trampoline */
extern "C" void __cdecl persp_capture()
{
    const int i = at<int>(kCount) - 1;
    if (i < 0 || i >= kMax) return;
    Entry &e = g_e[i];
    const float *rec = (const float *)(g_base + (kRecords + 40u * i + 8 - 0x400000));
    memcpy(e.xy, rec, sizeof e.xy);
    e.ok = true; e.depth = true;
    float v[4][3];
    for (int k = 0; k < 4; k++) for (int c = 0; c < 3; c++) v[k][c] = at<float>(kVerts + 12 * k + 4 * c);
    for (int k = 0; k < 4; k++) e.z[k] = v[k][2];
    /* 1.1.20: no test against [0x4D5180] any more. 1.1.16 took it for the near plane; it is a camera value that steps
       by 409.6 as the view changes (0 in the chase view, 36,044 to 49,152 in others, seen 8 Nov 2026), so in those
       views every record with a corner nearer than it lost the perspective mapping (46% of records on jayyanz's PC).
       Cut records are found by the corner check below. */
    /* 1.1.18: the clipping emitters (sites 1 and 3) leave each slot holding the polygon's own camera-space vertex, also
       where they moved the corner to the near plane: there the record's corner is the cut point, not the slot's
       projection (seen 8 Nov 2026: a slot's depth of 6,000 for a corner whose screen point needs 314,000). Each corner is
       checked (x = 160 X / Z, y = -160 Y / Z); a cut one gets the depth where its screen ray meets the polygon's plane
       (through the slots), and the record keeps the PC mapping (its texture spans the uncut polygon). */
    if (g_site == 1 || g_site == 3) {
        bool cut[4], any = false;
        for (int k = 0; k < 4; k++) {
            const float z = v[k][2], px = e.xy[2 * k], py = e.xy[2 * k + 1];
            cut[k] = !(z > 0) || fabsf(160.0f * v[k][0] / z - px) > 0.5f + 0.02f * fabsf(px) ||
                     fabsf(-160.0f * v[k][1] / z - py) > 0.5f + 0.02f * fabsf(py);
            any = any || cut[k];
        }
        if (any) {
            e.ok = false; g_cut++;
            /* the plane through the slots: the normal from the two diagonals (a quad's best fit) */
            const float d1[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
            const float d2[3] = { v[3][0] - v[1][0], v[3][1] - v[1][1], v[3][2] - v[1][2] };
            const float n[3] = { d1[1] * d2[2] - d1[2] * d2[1], d1[2] * d2[0] - d1[0] * d2[2], d1[0] * d2[1] - d1[1] * d2[0] };
            float c[3] = { 0, 0, 0 };
            for (int k = 0; k < 4; k++) for (int q = 0; q < 3; q++) c[q] += 0.25f * v[k][q];
            const double dd = (double)n[0] * c[0] + (double)n[1] * c[1] + (double)n[2] * c[2];
            for (int k = 0; k < 4; k++) {
                if (!cut[k]) continue;
                const double r[3] = { e.xy[2 * k] / 160.0, -e.xy[2 * k + 1] / 160.0, 1.0 };   /* the screen ray, at depth 1 */
                const double nr = n[0] * r[0] + n[1] * r[1] + n[2] * r[2];
                const double z = nr != 0 ? dd / nr : -1.0;
                e.z[k] = z > 0 && z < 1e9 ? (float)z : -1.0f;
            }
        }
    }
    for (int k = 0; k < 4; k++) {
        if (!(e.z[k] > 0 && e.z[k] < 1e9f)) { e.depth = false; e.ok = false; }   /* 1.1.21: MAX draw distance goes past 10,000,000 */
        else {
            if (e.z[k] < g_zmin) g_zmin = e.z[k];
            if (e.z[k] > g_zmax) g_zmax = e.z[k];
#ifdef ULT_DEBUG
            int b = 0; for (float t = 316.2f; e.z[k] >= t && b < 15; t *= 3.1623f) b++;
            g_hist[b]++;
#endif
        }
    }
    g_znear = at<float>(kNear);
    g_captured++;
    if (!e.ok) g_near++;
#ifdef ULT_DEBUG
    /* check that slot k is corner k where the emitter leaves camera x as well (the clipping emitters): 160 x / z is the
       record's x, the cut corners aside */
    if (g_site == 1 || g_site == 3)
        for (int k = 0; k < 4; k++) {
            const float x = v[k][0], z = v[k][2];
            if (z > 0) { g_checked++; float d = 160.0f * x / z - e.xy[2 * k]; if (d > -0.05f && d < 0.05f) g_matched++; }
        }
#endif
}

bool install(BYTE *base)
{
    g_base = base;
    for (const Site &s : kSites)
        if (memcmp(base + (s.at - 0x400000), s.code, s.len)) {
            plog("perspective: unexpected code at %08lX (%s); the perspective-correct mapping is not available", s.at, s.what);
            return false;
        }
    BYTE *tramp = (BYTE *)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    BYTE *t = tramp;
    for (const Site &s : kSites) {
        BYTE *entry = t;
        memcpy(t, s.code, s.len); t += s.len;     /* the replaced instructions */
        *t++ = 0x9C;                              /* pushfd */
        *t++ = 0x60;                              /* pushad */
        *t++ = 0xC7; *t++ = 0x05; *(DWORD *)t = (DWORD)(uintptr_t)&g_site; t += 4; *(int32_t *)t = (int32_t)(&s - kSites); t += 4;   /* mov [g_site], n */
        *t++ = 0xE8;                              /* call persp_capture */
        *(int32_t *)t = (int32_t)((BYTE *)&persp_capture - (t + 4)); t += 4;
        *t++ = 0x61;                              /* popad */
        *t++ = 0x9D;                              /* popfd */
        *t++ = 0xC3;                              /* ret */
        BYTE *p = base + (s.at - 0x400000);
        DWORD old; VirtualProtect(p, s.len, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE8; *(int32_t *)(p + 1) = (int32_t)(entry - (p + 5));
        for (int k = 5; k < s.len; k++) p[k] = 0x90;
        VirtualProtect(p, s.len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, s.len);
    }
    g_on = true;
    plog("perspective: depths captured in the 5 emitters (course, course clipping, course 2 and its clipping, models)");
    return true;
}

int lookup(uint32_t rec, float z[4])
{
    if (!g_on || rec < kRecords) return 0;
    const uint32_t i = (rec - kRecords) / 40;
    if (i >= (uint32_t)kMax || (rec - kRecords) % 40) return 0;
    const Entry &e = g_e[i];
    if (!e.depth || memcmp(e.xy, g_base + (rec + 8 - 0x400000), sizeof e.xy)) return 0;
    memcpy(z, e.z, sizeof e.z);
    return e.ok ? 2 : 1;
}

bool range(float &znear, float &zmin, float &zmax)
{
#ifdef ULT_DEBUG
    char line[400]; int n = 0; float t = 316.2f;
    for (int b = 0; b < 16; b++, t *= 3.1623f) n += _snprintf(line + n, sizeof line - n, " <%.0f:%u", t, g_hist[b]);
    plog("perspective: depth histogram%s", line);
#endif
    znear = g_znear; zmin = g_zmin; zmax = g_zmax;
    return g_zmax > 0;
}

void stats(unsigned &captured, unsigned &near_plane, unsigned &checked, unsigned &matched)
{
    captured = g_captured; near_plane = g_near; checked = g_checked; matched = g_matched;   /* g_near counts the cut ones too */
}

} // namespace persp
