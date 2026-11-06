/*
 * The pause screen as a menu (0.0.17, jayyanz 1 Nov 2026): CONTINUE / RESTART RACE / EXIT TO TITLE, in the game's own
 * text, chosen with Up / Down and Start, A or C (B continues). Replaces the only way out of a race the Windows menu
 * gave (its "Restart Game", the Saturn soft reset), which goes with that menu.
 *
 * The game's paused screen is screen mode 19, handler 0x44AEF0 in the handler table 0x4818B8. It is wrapped, not
 * replaced: every frame the game's own handler still runs (its pause text, its music-track choice, its X + Y + Z
 * restart and the keyboard's R key), with the menu's buttons taken out of its new-press word. Our three lines are
 * then written over its two "PRESS X, Y, AND Z BUTTON / TO RESTART GAME" lines with the same text routine.
 *   CONTINUE      the game's own unpause: its handler run with Start as the new press.
 *   RESTART RACE  the game's own restart: its handler run with X + Y + Z held (as a pad would), the device shown
 *                 as a joystick for that call (the keyboard device checks the R key instead); repeated next frame
 *                 if the screen did not change.
 *   EXIT TO TITLE the soft reset the Windows menu's Restart Game used: flag 0x48AC24, which makes the next input poll
 *                 press A + B + C + Start together.
 * During a replay (word 0x11B7A9C set) the game's own pause screen is left as it is (X + Y + Z skips the replay).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "proxy.h"
#include "pause_menu.h"

namespace {

BYTE *g_base;
template <typename T> T &at(DWORD va) { return *(T *)(g_base + (va - 0x400000)); }

constexpr DWORD kHandlerTable = 0x4818B8, kPauseHandler = 0x44AEF0, kMode = 0x482230;
constexpr DWORD kPadHeld = 0xC583E0, kPadNew = 0xC583E2, kDevice = 0x48ABB0, kReplay = 0x11B7A9C, kSoftReset = 0x48AC24;
constexpr int kPausedMode = 19;
constexpr WORD kUp = 0x1000, kDown = 0x2000, kStart = 0x0800, kA = 0x0400, kC = 0x0200, kB = 0x0100, kXYZ = 0x0070;
constexpr WORD kMenuButtons = kUp | kDown | kStart | kA | kC | kB;

using handler_t = void (__cdecl *)();
handler_t g_orig;

enum Action { None, Continue, Restart, Exit };
int g_cursor;          /* 0 CONTINUE, 1 RESTART RACE, 2 EXIT TO TITLE */
bool g_open;           /* the menu is on screen (reset when the pause ends) */
Action g_pending;      /* chosen, still being carried out */
bool g_wait_release;   /* chosen: carried out once the select button is let go (a held Start would pause the
                          restarted race at once, as the game reads Start again when the race begins) */
int g_tries;

/* The game's text routine on its 8x8 grid (column, row), the one its pause text uses. */
void text(int col, int row, const char *s) { ((void (__cdecl *)(int, int, const char *))(g_base + (0x4243F0 - 0x400000)))(col, row, s); }

constexpr int kCol = 10, kWidth = 24, kRow0 = 0x0D, kRowStep = 2;
const char *const kItems[] = { "CONTINUE", "RESTART RACE", "EXIT TO TITLE" };

void line(int row, const char *s, bool selected)
{
    char b[kWidth + 1];
    memset(b, ' ', kWidth); b[kWidth] = 0;
    int n = (int)strlen(s), at0 = (kWidth - n) / 2;
    memcpy(b + at0, s, (size_t)n);
    if (selected) b[at0 - 2] = '@';   /* '@' is a right-pointing arrow in this font (no '[' or ']') */
    text(kCol, row, b);
}

void draw()
{
    for (int i = 0; i < 3; i++) line(kRow0 + kRowStep * i, kItems[i], i == g_cursor);
}

/* The game's own text calls in its paused handler: the "PRESS X, Y, AND Z BUTTON" / "PRESS [R] KEY ..." line (row
   0x0D) and "TO RESTART GAME" (row 0x0F) at 0x44B117, 0x44B144, 0x44B155. This text is drawn afresh every frame with
   see-through spaces, so it cannot be written over: while the menu is open those two rows are not drawn. PAUSE (row
   0x0A, 0x44B0E7) and everything else are drawn as the game does. */
const DWORD kTextCalls[] = { 0x44B117, 0x44B144, 0x44B155 };
using text_t = void (__cdecl *)(int, int, const char *);
text_t g_text;
void __cdecl filter_text(int col, int row, const char *s)
{
    if (g_open && !at<WORD>(kReplay) && (row == 0x0D || row == 0x0F)) return;
    g_text(col, row, s);
}

void clear() {}   /* nothing to clear: the text is drawn every frame */

/* the game's handler with a new-press word and extra held buttons of our choosing */
void run_game(WORD new_presses, WORD extra_held)
{
    WORD held = at<WORD>(kPadHeld), fresh = at<WORD>(kPadNew);
    at<WORD>(kPadNew) = new_presses;
    at<WORD>(kPadHeld) = held | extra_held;
    g_orig();
    at<WORD>(kPadHeld) = held;
    at<WORD>(kPadNew) = fresh;
}

DWORD g_last_call;

void __cdecl paused()
{
    /* a new pause (the handler was not running a moment ago): the menu starts afresh on CONTINUE */
    DWORD now = GetTickCount();
    if (now - g_last_call > 250 && g_pending == None) g_open = false;
    g_last_call = now;
    if (at<WORD>(kReplay)) { g_open = false; g_orig(); return; }   /* replay: the game's own pause (X + Y + Z skips it) */
    WORD fresh = at<WORD>(kPadNew);

    if (g_pending != None && g_wait_release) {
        run_game(fresh & ~kMenuButtons, 0);
        draw();
        if (at<WORD>(kPadHeld) & (kStart | kA | kC)) return;
        g_wait_release = false;
        if (g_pending == Exit) at<int>(kSoftReset) = 1;   /* as the Windows menu's Restart Game did */
        return;
    }
    if (g_pending == Restart) {
        /* X + Y + Z as a pad presses them; the keyboard device would check the R key instead */
        WORD dev = at<WORD>(kDevice);
        if (dev == 8) at<WORD>(kDevice) = 0x20;
        run_game(0, kXYZ);
        if (at<WORD>(kDevice) == 0x20 && dev == 8) at<WORD>(kDevice) = dev;
        if (at<int>(kMode) != kPausedMode || ++g_tries > 10) {
            plog("pause menu: RESTART RACE -> screen mode %d", at<int>(kMode));
            clear(); g_pending = None; g_open = false;
        }
        return;
    }
    if (g_pending == Exit) {
        run_game(fresh & ~kMenuButtons, 0);
        if (at<int>(kMode) != kPausedMode || ++g_tries > 30) {
            plog("pause menu: EXIT TO TITLE -> screen mode %d", at<int>(kMode));
            clear(); g_pending = None; g_open = false;
        }
        return;
    }

    if (!g_open) { g_open = true; g_cursor = 0; }
    Action act = None;
    if (fresh & kUp) g_cursor = (g_cursor + 2) % 3;
    else if (fresh & kDown) g_cursor = (g_cursor + 1) % 3;
    else if (fresh & kB) act = Continue;
    else if (fresh & (kStart | kA | kC)) act = (Action)(g_cursor + 1);

    if (act == Continue) {
        run_game(kStart, 0);                    /* the game's own unpause, with its own clearing of the text */
        clear();
        g_open = false;
        plog("pause menu: CONTINUE");
        return;
    }
    if (act == Restart || act == Exit) {
        g_pending = act; g_tries = 0; g_wait_release = true;
        plog("pause menu: %s chosen", act == Restart ? "RESTART RACE" : "EXIT TO TITLE");
        run_game(fresh & ~kMenuButtons, 0);
        draw();
        return;
    }
    run_game(fresh & ~kMenuButtons, 0);
    if (at<int>(kMode) == kPausedMode) draw();
    else { clear(); g_open = false; }            /* left by the game's own X + Y + Z or R */
}

} // namespace

bool pause_menu_install(BYTE *game_base)
{
    g_base = game_base;
    g_text = (text_t)(g_base + (0x4243F0 - 0x400000));
    for (DWORD a : kTextCalls) {
        BYTE *p = g_base + (a - 0x400000);
        if (p[0] != 0xE8 || (DWORD)(a + 5 + *(LONG *)(p + 1)) != 0x4243F0) { plog("pause menu: unexpected code at %08lX, game's pause kept", a); return false; }
    }
    DWORD *slot = &at<DWORD>(kHandlerTable + 4 * kPausedMode);
    if (*slot != kPauseHandler) { plog("pause menu: handler table not as expected (%08lX), game's pause kept", *slot); return false; }
    g_orig = (handler_t)(g_base + (kPauseHandler - 0x400000));
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = (DWORD)(ULONG_PTR)fnptr(paused);
    VirtualProtect(slot, 4, old, &old);
    for (DWORD a : kTextCalls) {
        BYTE *p = g_base + (a - 0x400000);
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        *(LONG *)(p + 1) = (LONG)((BYTE *)fnptr(filter_text) - (p + 5));
        VirtualProtect(p, 5, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), g_base + (0x44B000 - 0x400000), 0x200);
    plog("pause menu: on (CONTINUE / RESTART RACE / EXIT TO TITLE)");
    return true;
}
