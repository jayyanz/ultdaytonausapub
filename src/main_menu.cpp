/* Two changes to the game's own screens (jayyanz, 1 Nov 2026, build 0.0.19):
 *
 * Mode Select (screen mode 7, 0x4072B0, called at 0x44A5E0): an EXIT row under OPTIONS that quits the game, now that
 * the Windows menu (and its Exit Game) is gone. The game's routine still draws and runs its four picture rows; while
 * EXIT is chosen its cursor (0x496E80) is set to 4, which no row matches, and it is given no presses but B. Up from
 * ARCADE MODE and Down from OPTIONS reach EXIT. EXIT is drawn with one of the game's own fonts. Choosing it asks
 * EXIT GAME?  YES  NO on the same line (0.0.20, jayyanz): NO first, Left / Right change, A / C choose, B is NO.
 *
 * Initials entry (screen mode 27, letter routine 0x42C0B0): with a pad (analogue flag 0xC583E6 = 1) the game sets
 * the letter from the steering position (0x42C8B0), like a wheel; with the keyboard (0x42C940) it moves a letter every
 * other frame while Left / Right is held, so a tap overshoots. Both calls (0x42C0D3, 0x42C0DA) come here: a D-pad or
 * Left / Right key press steps one letter (repeating after a short hold), through the game's own keyboard routine fed
 * a single press; the stick or wheel still sets the letter as before, whenever it moves.
 *
 * Initials timer (0.0.21, jayyanz): the entry's countdown (word 0xC59F6C, 600 at the start, set at 0x42BF0B) is
 * shown as countdown / 30 (0x4477D1), so 20 seconds at 30 ticks a second; the entry screen (mode 27) runs at the
 * 60 Hz menu rate and stepped it every frame (0x42C0FC: mov ax,[0xC59F6C] / dec ax / mov [0xC59F6C],ax, then test
 * and jg), so the entry ran out in 10 seconds. Those 14 bytes now call initials_countdown_stub, which steps it every
 * second frame and leaves the value in ax for the game's test as before. (The screen's other timer, 0x11BDB2C,
 * which ends the screen after the entry, is left as it is.)
 *
 * Speed in MPH or KM/H (GAMEPLAY page, [Gameplay] Speed): the game already has a km/h race display. Its four HUD
 * layouts (0x4234F9 / 0x42356C, 0x423909 / 0x42397C, 0x423B39 / 0x423C3C, 0x423D89 / 0x423E8C) read the word 0xC58388
 * twice each: once to pick the unit picture (HUD image 5 km/h, 4 mph) and once the factor on the car's speed (1.0,
 * the speed being km/h already, or 0.6125 for mph). 0xC58388 is set at start-up only on Japanese Windows (locale
 * 0411) and picks Japanese pictures and speech elsewhere, so it is left alone: those eight reads read our own word.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "proxy.h"
#include "input_sdl.h"
#include "main_menu.h"

extern "C" {
int ult_initials_frames;
void initials_countdown_stub();
}
/* replaces "mov ax,[0xC59F6C] / dec ax / mov [0xC59F6C],ax" (14 bytes): steps every second call, value left in ax */
asm(".text\n"
    ".globl _initials_countdown_stub\n"
    "_initials_countdown_stub:\n"
    "    incl _ult_initials_frames\n"
    "    testl $1, _ult_initials_frames\n"
    "    jz 1f\n"
    "    decw 0xC59F6C\n"
    "1:  movw 0xC59F6C, %ax\n"
    "    ret\n");

namespace {

BYTE *g_base;
template <typename T> T &at(DWORD va) { return *(T *)(g_base + (va - 0x400000)); }

using voidfn = void (__cdecl *)();
using draw_text_t = int (__cdecl *)(int font, int x, int y, const char *fmt, ...);
using load_font_t = int (__cdecl *)(const char *name);
voidfn game_mode_select;    /* 0x4072B0 */
voidfn game_letter_pad;     /* 0x42C8B0 */
voidfn game_letter_keys;    /* 0x42C940 */
draw_text_t game_draw_text; /* 0x428E20 */
load_font_t game_load_font; /* 0x428CC0 */

constexpr DWORD kMode = 0x482230, kPressed = 0xC583E2, kHeld = 0xC583E0, kHwnd = 0xC58488;
constexpr WORD kUp = 0x1008, kDown = 0x2080, kConfirm = 0x0600, kBack = 0x0100, kLeft = 0x4000, kRight = 0x8000;

/* ---------- Mode Select ---------- */
constexpr DWORD kCursor = 0x496E80, kBlink = 0x496E84, kChosen = 0x496EB4;
bool g_exit;            /* EXIT is the row chosen */
int g_confirm = -1;     /* EXIT GAME? shown: -1 no, 0 NO lit, 1 YES lit (0.0.20, jayyanz) */
/* Room for EXIT: the four picture rows moved up (y 48, 80, 120, 152 -> 40, 70, 102, 131; the game's table 0x481940
   and the same values pushed at 0x407331, 0x40735E, 0x40738B, 0x4073B8 while a choice flashes). EXIT is centred,
   above the bottom stripe. */
constexpr int kExitY = 0xA6;
/* Centred on the picture (jayyanz 1 Nov 2026), positions measured from screenshots under Wine (this font draws a
   little left of x and narrower than 8 units a character). */
constexpr int kExitTextX = 159, kConfirmX = 100;   /* "EXIT", "EXIT GAME?  YES  NO" */
const short kRowY[4] = { 40, 70, 102, 131 };

bool move_rows()
{
    struct Push { DWORD at; BYTE len; DWORD old; } pushes[4] = { { 0x407331, 2, 0x30 }, { 0x40735E, 2, 0x50 }, { 0x40738B, 2, 0x78 }, { 0x4073B8, 5, 0x98 } };
    const short kOldY[4] = { 48, 80, 120, 152 };
    short *table = (short *)(g_base + (0x481940 - 0x400000));
    for (int i = 0; i < 4; i++) {
        BYTE *p = g_base + (pushes[i].at - 0x400000);
        bool ok = pushes[i].len == 2 ? (p[0] == 0x6A && p[1] == pushes[i].old) : (p[0] == 0x68 && *(DWORD *)(p + 1) == pushes[i].old);
        if (!ok || table[i * 2 + 1] != kOldY[i]) return false;
    }
    for (int i = 0; i < 4; i++) {
        BYTE *p = g_base + (pushes[i].at - 0x400000);
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        if (pushes[i].len == 2) p[1] = (BYTE)kRowY[i]; else *(DWORD *)(p + 1) = (DWORD)kRowY[i];
        VirtualProtect(p, 5, old, &old);
        table[i * 2 + 1] = kRowY[i];
    }
    FlushInstructionCache(GetCurrentProcess(), g_base + (0x407300 - 0x400000), 0x100);
    return true;
}
int g_font[2];          /* the game's menu fonts, loaded once: normal, selected */


void load_fonts()
{
    if (g_font[0]) return;
    g_font[0] = game_load_font("00000001.FNT");
    g_font[1] = game_load_font("00000002.FNT");
    plog("mode select: menu fonts loaded (%s)", g_font[0] && g_font[1] ? "ok" : "FAILED");
}

void __cdecl mode_select()
{
    load_fonts();
    WORD in = at<WORD>(kPressed), pass = in;
    int &cursor = at<int>(kCursor);
    if (at<int>(kChosen) > 0) { game_mode_select(); return; }   /* a row was chosen: the game's flash and change of screen */
    if (g_confirm >= 0) {                                       /* EXIT GAME?  YES  NO: NO first, B is NO */
        if (in & (kLeft | kRight | kUp | kDown)) g_confirm ^= 1;
        else if (in & kBack) { g_confirm = -1; plog("mode select: exit cancelled"); }
        else if (in & kConfirm) {
            if (g_confirm == 1) {
                plog("mode select: EXIT confirmed, closing the game");
                PostMessageA(at<HWND>(kHwnd), WM_COMMAND, 0x67, 0);   /* the game's own Exit Game command */
            } else plog("mode select: exit cancelled");
            g_confirm = -1;
        }
        cursor = 4;
        at<WORD>(kPressed) = 0;
        game_mode_select();
        at<WORD>(kPressed) = in;
        if (g_confirm >= 0) {
            const int x = kConfirmX;
            game_draw_text(g_font[1], x, kExitY, "%s", "EXIT GAME?");
            game_draw_text(g_font[g_confirm == 1], x + 8 * 12, kExitY, "%s", "YES");
            game_draw_text(g_font[g_confirm == 0], x + 8 * 17, kExitY, "%s", "NO");
        } else game_draw_text(g_font[1], kExitTextX, kExitY, "%s", "EXIT");
        return;
    }
    if (!g_exit) {
        if ((cursor == 3 && (in & kDown)) || (cursor == 0 && (in & kUp))) { g_exit = true; at<int>(kBlink) |= 8; pass = 0; }
    } else if (in & (kUp | kDown)) {
        g_exit = false; cursor = (in & kUp) ? 3 : 0; at<int>(kBlink) |= 8; pass = 0;
    } else if (in & kConfirm) {
        g_confirm = 0;                                          /* ask first, NO lit */
        plog("mode select: EXIT chosen, asking");
        pass = 0;
    } else pass = in & kBack;                                   /* B still goes back to the title */
    if (g_exit) cursor = 4;                                     /* no picture row highlighted */
    at<WORD>(kPressed) = pass;
    game_mode_select();
    at<WORD>(kPressed) = in;
    if (at<int>(kMode) != 7) { if (g_exit) { g_exit = false; cursor = 3; } return; }
    if (!g_exit || (at<int>(kBlink) & 8)) game_draw_text(g_font[g_exit ? 1 : 0], kExitTextX, kExitY, "%s", "EXIT");
}

/* ---------- speed units ---------- */
DWORD g_kmh;   /* read by the HUD in place of 0xC58388 */
const DWORD kSpeedReads[8] = { 0x4234F9, 0x42356C, 0x423909, 0x42397C, 0x423B39, 0x423C3C, 0x423D89, 0x423E8C };

bool install_speed_units()
{
    for (DWORD a : kSpeedReads) {   /* mov ecx, [0xC58388] */
        BYTE *p = g_base + (a - 0x400000);
        if (p[0] != 0x8B || p[1] != 0x0D || *(DWORD *)(p + 2) != 0xC58388) return false;
    }
    for (DWORD a : kSpeedReads) {
        BYTE *p = g_base + (a - 0x400000);
        DWORD old;
        VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
        *(DWORD *)(p + 2) = (DWORD)(UINT_PTR)&g_kmh;
        VirtualProtect(p, 6, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), g_base + (0x423000 - 0x400000), 0x1000);
    return true;
}

/* Mission Select (0.0.22): the car panels ("MAX SPEED 196 mph") come in an mph and a km/h version, picked by the same
   Japanese-locale word: images 0x51 to 0x56 or 0x31 to 0x36. Four reads, two forms: mov ecx, [0xC58388] (8B 0D) and
   mov eax, [0xC58388] (A1). */
struct SpeedRead { DWORD va; BYTE op[2]; int oplen; };
const SpeedRead kSelectReads[4] = {
    { 0x44D389, { 0x8B, 0x0D }, 2 }, { 0x44D5D9, { 0xA1 }, 1 }, { 0x44E8CF, { 0xA1 }, 1 }, { 0x44E9B3, { 0xA1 }, 1 },
};

bool install_select_units()
{
    for (const SpeedRead &r : kSelectReads) {
        BYTE *p = g_base + (r.va - 0x400000);
        if (memcmp(p, r.op, r.oplen) != 0 || *(DWORD *)(p + r.oplen) != 0xC58388) return false;
    }
    for (const SpeedRead &r : kSelectReads) {
        BYTE *p = g_base + (r.va - 0x400000);
        DWORD old;
        VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
        *(DWORD *)(p + r.oplen) = (DWORD)(UINT_PTR)&g_kmh;
        VirtualProtect(p, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 6);
    }
    return true;
}

/* ---------- initials entry ---------- */
constexpr DWORD kAnalogue = 0xC583E6, kSteer = 0xC583E4, kFrameTimer = 0x11B7A4C;

int stick_letter()   /* the letter the game's pad routine would take from the steering byte */
{
    int si = (~(at<WORD>(kSteer) >> 3)) & 0x1F;
    return si > 0x1C ? 0x1C : si;
}

void step_letter(WORD dir)
{
    /* the game's keyboard routine, given one held press and its every-other-frame gate open */
    WORD held = at<WORD>(kHeld);
    DWORD timer = at<DWORD>(kFrameTimer);
    at<WORD>(kHeld) = dir;
    at<DWORD>(kFrameTimer) = timer & ~1u;
    game_letter_keys();
    at<WORD>(kHeld) = held;
    at<DWORD>(kFrameTimer) = timer;
}

void __cdecl initials_letter()
{
    static int last_stick = -1;
    static DWORD prev, held_since, last_repeat;
    bool pad = at<WORD>(kAnalogue) == 1;
    DWORD now = GetTickCount();
    DWORD dirs = 0;   /* 1 left, 2 right */
    if (pad) dirs = input_dpad_left_right();      /* the D-pad (the game's LEFT / RIGHT bits come from the stick here) */
    else {                                         /* the keyboard: the game's own Left / Right keys */
        WORD hk = at<WORD>(kHeld);
        if (hk & kLeft) dirs |= 1;
        if (hk & kRight) dirs |= 2;
    }
    if (dirs == 3) dirs = 0;
    DWORD edge = dirs & ~prev;
    WORD dir = 0;
    if (edge) { dir = (edge & 1) ? kLeft : kRight; held_since = last_repeat = now; }
    else if (dirs && now - held_since > 400 && now - last_repeat > 120) { dir = (dirs & 1) ? kLeft : kRight; last_repeat = now; }
    prev = dirs;
    if (dir) {
        int before = at<int>(0x4C3EA8);
        step_letter(dir);
        plog("initials: %s %s, letter %d -> %d", dir == kLeft ? "left" : "right", edge ? "press" : "repeat", before, at<int>(0x4C3EA8));
        return;
    }
    if (pad) {
        /* the stick or wheel: as the game, but only when it moves, so a stepped letter stays */
        int s = stick_letter();
        if (s != last_stick) { int before = at<int>(0x4C3EA8); last_stick = s; game_letter_pad(); plog("initials: stick, letter %d -> %d", before, at<int>(0x4C3EA8)); }
    } else last_stick = -1;
}

bool redirect(DWORD site, DWORD target, void *fn)
{
    BYTE *p = g_base + (site - 0x400000);
    if (p[0] != 0xE8 || (DWORD)(site + 5 + *(LONG *)(p + 1)) != target) { plog("main menu: unexpected code at %08lX", site); return false; }
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(LONG *)(p + 1) = (LONG)((BYTE *)fn - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

bool expect(DWORD site, DWORD target)
{
    BYTE *p = g_base + (site - 0x400000);
    return p[0] == 0xE8 && (DWORD)(site + 5 + *(LONG *)(p + 1)) == target;
}

} // namespace

bool speed_kmh_get() { return g_kmh != 0; }
void speed_kmh_set(bool kmh) { g_kmh = kmh ? 1 : 0; }

void main_menu_install(BYTE *base, bool kmh)
{
    g_base = base;
    g_kmh = kmh ? 1 : 0;
    if (install_speed_units()) plog("speed units: race display in %s", kmh ? "km/h" : "mph");
    else plog("speed units: unexpected code, the game's own units kept");
    if (install_select_units()) plog("speed units: Mission Select follows the setting");
    else plog("speed units: Mission Select code unexpected, left as the game has it");
    game_mode_select = (voidfn)(base + (0x4072B0 - 0x400000));
    game_letter_pad = (voidfn)(base + (0x42C8B0 - 0x400000));
    game_letter_keys = (voidfn)(base + (0x42C940 - 0x400000));
    game_draw_text = (draw_text_t)(base + (0x428E20 - 0x400000));
    game_load_font = (load_font_t)(base + (0x428CC0 - 0x400000));
    if (expect(0x44A5E0, 0x4072B0) && move_rows() && redirect(0x44A5E0, 0x4072B0, (void *)mode_select)) plog("mode select: EXIT row installed");
    else plog("mode select: unexpected code, left as the game's");
    {   /* the initials countdown at 30 ticks a second */
        BYTE *p = base + (0x42C0FC - 0x400000);
        const BYTE code[16] = { 0x66, 0xA1, 0x6C, 0x9F, 0xC5, 0x00, 0x66, 0x48, 0x66, 0xA3, 0x6C, 0x9F, 0xC5, 0x00, 0x66, 0x85 };
        if (!memcmp(p, code, 16)) {
            DWORD old;
            VirtualProtect(p, 14, PAGE_EXECUTE_READWRITE, &old);
            p[0] = 0xE8;
            *(LONG *)(p + 1) = (LONG)((BYTE *)initials_countdown_stub - (p + 5));
            memset(p + 5, 0x90, 9);
            VirtualProtect(p, 14, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 14);
            plog("initials entry: countdown at 30 a second (20 seconds, as shown)");
        } else plog("initials entry: unexpected code at 42C0FC, countdown left as the game's");
    }
    if (expect(0x42C0D3, 0x42C8B0) && expect(0x42C0DA, 0x42C940)) {
        redirect(0x42C0D3, 0x42C8B0, (void *)initials_letter);
        redirect(0x42C0DA, 0x42C940, (void *)initials_letter);
        plog("initials entry: one letter per press installed");
    } else plog("initials entry: unexpected code, left as the game's");
}
