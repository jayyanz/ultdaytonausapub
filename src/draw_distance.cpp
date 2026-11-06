/*
 * Longer track draw distance (0.0.17, jayyanz 1 Nov 2026; off by default).
 *
 * How the game draws the course (docs/investigation/draw-distance.md): per frame, after the 3x3 cells round the
 * camera and the cars, it jumps (0x449A4A, 0x449A64) to the Clip Size dispatcher 0x445450, which draws a window of
 * cells of the 64x64 course grid from per-heading tables: NORMAL 5 cells ahead, WIDE 6, VAST 7 (cells of 32 units).
 * There is no far clip: the window is the only limit. Every polygon goes into the display list of 1,800 records.
 *
 * Both jumps now come here.
 *   Game's Clip Size (default): the game's own dispatcher, unchanged, except that it is skipped when the list is
 *     already full. The game's routines shorten each cell to "1,800 minus the count" as a 16-bit number, which
 *     wraps round to a huge count (and overruns the list) if cars and near cells ever filled it first.
 *   FAR and MAX: our own routine. The camera's cell and heading as the game takes them; every cell within the radius
 *     that can be in view (the screen's horizontal field of view, 47.7 degrees either side, plus a cell of margin),
 *     in rings from the nearest outwards, so a full list trims the horizon and never the road ahead. Each cell's two
 *     polygon lists go to the game's own transform-and-project routine (0x452F10, 0x41DE30 on the Expert course), as
 *     the game's routines do, each clamped to the room left in the list. The game's out-of-grid safety is kept: the
 *     player car's position is saved every frame, and restored with nothing drawn if the camera leaves the grid.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include "proxy.h"
#include "draw_distance.h"

namespace {

BYTE *g_base;
template <typename T> T &at(DWORD va) { return *(T *)(g_base + (va - 0x400000)); }
template <typename T> T *ptr(DWORD va) { return (T *)(g_base + (va - 0x400000)); }

constexpr DWORD kCount = 0x4931A8, kCourse = 0x11B7288, kCamX = 0x11D5A90, kCamZ = 0x11D5A98, kYaw = 0x4D5134;
constexpr DWORD kBank = 0x4D6BC2, kPlayerCar = 0x11B7A5C, kSavedPos = 0x490600, kDispatcher = 0x445450;
constexpr int kListMax = 1800;

volatile LONG g_level;
int g_stats;
int g_radius[3] = { 0, 12, 18 };   /* FAR, MAX: set by measurement (status.md, 1 Nov 2026) */

/* statistics for [Test] DrawDistanceLog */
DWORD g_t0;
int g_frames, g_before_max, g_after_max, g_full, g_cells, g_cut;

using emit_t = void (__cdecl *)(BYTE *poly, BYTE *verts, int bank, int count);

void emit(emit_t fn, BYTE *poly, BYTE *verts, int n)
{
    if (n <= 0) return;
    int room = kListMax - at<int>(kCount);
    if (room <= 0) { g_cut++; return; }
    if (n > room) { n = room; g_cut++; }
    fn(poly, verts, at<WORD>(kBank), n);
}

void own_window(int R)
{
    DWORD x = at<DWORD>(kCamX), z = at<DWORD>(kCamZ);
    unsigned row = (0x3FFFFFFu - z) >> 21, col = (x + 0x4000000u) >> 21;
    BYTE *car = at<BYTE *>(kPlayerCar);
    /* the game's out-of-grid safety, as in its own window routines (0x4451EF to 0x445434) */
    if (row > 71 || col > 71) {
        if (car) {
            *(DWORD *)(car + 0x10) = at<DWORD>(kSavedPos);
            *(DWORD *)(car + 0x14) = at<DWORD>(kSavedPos + 4);
            *(DWORD *)(car + 0x18) = at<DWORD>(kSavedPos + 8);
            *(DWORD *)(car + 0x0C) = 0;
            ((void (__cdecl *)(BYTE *))(g_base + (0x448570 - 0x400000)))(car);
        }
        at<int>(kCount) = 0;
        return;
    }
    if (car) {
        at<DWORD>(kSavedPos) = *(DWORD *)(car + 0x10);
        at<DWORD>(kSavedPos + 4) = *(DWORD *)(car + 0x14);
        at<DWORD>(kSavedPos + 8) = *(DWORD *)(car + 0x18);
    }
    int c = at<int>(kCourse);
    if (c < 0 || c > 2) return;
    BYTE *va = at<BYTE *>(0x481404 + 8 * c), *vb = at<BYTE *>(0x481408 + 8 * c);
    BYTE *pa = at<BYTE *>(0x4813EC + 8 * c), *pb = at<BYTE *>(0x4813F0 + 8 * c);
    short *cnA = at<short *>(0x48141C + 16 * c), *cnB = at<short *>(0x481424 + 16 * c);
    int *ofA = at<int *>(0x481420 + 16 * c), *ofB = at<int *>(0x481428 + 16 * c);
    emit_t fn = (emit_t)(g_base + ((c == 2 ? 0x41DE30 : 0x452F10) - 0x400000));

    /* heading 0 faces +Z (row decreasing), heading 0x4000 faces -X (column decreasing) */
    double a = (at<DWORD>(kYaw) & 0xFFFF) * (6.283185307179586 / 65536.0);
    double fr = -cos(a), fc = -sin(a);
    const double tan_half = 176.0 / 160.0, margin = 1.25;
    /* rings from the nearest outwards; within a ring, the centre of view first, so a full list trims the horizon's
       edges before its middle (the 3x3 round the camera is drawn by the game, 0x444740) */
    struct Cell { int cell; double side; } ring[8 * 32];
    for (int d = 2; d <= R; d++) {
        int n = 0;
        for (int dr = -d; dr <= d; dr++)
            for (int dc = -d; dc <= d; dc++) {
                if (abs(dr) != d && abs(dc) != d) continue;
                if (dr * dr + dc * dc > (R + 0.5) * (R + 0.5)) continue;
                double fwd = dr * fr + dc * fc, side = fabs(dc * fr - dr * fc);
                if (fwd < -margin || side > fwd * tan_half + margin * 1.5) continue;
                int r = (int)row + dr, cc = (int)col + dc;
                if (r < 0 || r > 63 || cc < 0 || cc > 63) continue;
                ring[n++] = { r * 64 + cc, side };
            }
        for (int i = 1; i < n; i++)                     /* insertion sort: at most a few dozen cells */
            for (int j = i; j > 0 && ring[j].side < ring[j - 1].side; j--) { Cell t = ring[j]; ring[j] = ring[j - 1]; ring[j - 1] = t; }
        for (int i = 0; i < n; i++) {
            int cell = ring[i].cell;
            g_cells++;
            emit(fn, pa + ofA[cell], va, cnA[cell]);
            emit(fn, pb + ofB[cell], vb, cnB[cell]);
        }
    }
}

void __cdecl course()
{
    int before = at<int>(kCount);
    int level = (int)g_level;
    if (level == 0) {
        if (before < kListMax) ((void (__cdecl *)())(g_base + (kDispatcher - 0x400000)))();
        else g_cut++;
    } else {
        own_window(g_radius[level]);
    }
    if (!g_stats) return;
    int after = at<int>(kCount);
    g_frames++;
    if (before > g_before_max) g_before_max = before;
    if (after > g_after_max) g_after_max = after;
    if (after >= kListMax) g_full++;
    DWORD now = GetTickCount();
    if (!g_t0) g_t0 = now;
    if (now - g_t0 >= 1000) {
        plog("draw distance: level %d (radius %d) course %d mode %d: %d frames, before course max %d, list max %d, full %d, cut %d, cells/frame %d",
             level, g_radius[level], at<int>(kCourse), at<int>(0x482230), g_frames, g_before_max, g_after_max, g_full, g_cut,
             g_frames ? g_cells / g_frames : 0);
        g_t0 = now; g_frames = g_before_max = g_after_max = g_full = g_cut = g_cells = 0;
    }
}

} // namespace

int draw_distance_radius(DrawDistance level) { return g_radius[(int)level]; }
DrawDistance draw_distance_get() { return (DrawDistance)g_level; }
void draw_distance_set(DrawDistance level)
{
    InterlockedExchange(&g_level, (LONG)level);
    plog("draw distance: %s", level == DrawDistance::Max ? "MAX" : level == DrawDistance::Far ? "FAR" : "the game's Clip Size");
}

bool draw_distance_install(BYTE *game_base, DrawDistance level, int stats_log, int far_radius, int max_radius)
{
    g_base = game_base;
    g_stats = stats_log;
    if (far_radius >= 3 && far_radius <= 31) g_radius[1] = far_radius;
    if (max_radius >= 3 && max_radius <= 31) g_radius[2] = max_radius;
    static const DWORD sites[] = { 0x449A4A, 0x449A64 };
    for (DWORD s : sites) {
        BYTE *p = ptr<BYTE>(s);
        if (p[0] != 0xE9 || (DWORD)(s + 5 + *(LONG *)(p + 1)) != kDispatcher) {
            plog("draw distance: unexpected code at %08lX, the game's course drawing kept", s);
            return false;
        }
    }
    for (DWORD s : sites) {
        BYTE *p = ptr<BYTE>(s); DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        *(LONG *)(p + 1) = (LONG)((BYTE *)fnptr(course) - (p + 5));
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    g_level = (LONG)level;
    plog("draw distance: course drawing hooked; %s (FAR radius %d, MAX radius %d cells)",
         level == DrawDistance::Max ? "MAX" : level == DrawDistance::Far ? "FAR" : "the game's Clip Size", g_radius[1], g_radius[2]);
    return true;
}
