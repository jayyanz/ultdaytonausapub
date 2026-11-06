/* The game's Options screen, rearranged into pages (jayyanz, 31 Oct 2026):
 *
 *   OPTIONS   GAMEPLAY, SOUND, DISPLAY, CONTROLS, EXIT
 *   GAMEPLAY  the game's GAME MODE, DIFFICULTY, ENEMY LEVEL, and SPEED (ours, 0.0.19: MPH or KM/H)
 *   SOUND     the game's SOUND MODE, MUSIC TEST, SOUND TEST
 *   DISPLAY   SCREEN, SCALING, PICTURE (16:10, 4:3, FRAME), VSYNC (ours) and the game's CLIP SIZE, MOTION, BG DISP, SEETHROUGH
 *             (until now only in its Windows menu)
 *   CONTROLS  LAYOUT, TRIGGER POINT, DEAD ZONE, KEYS WITH PAD and BUTTONS (ours)
 *   CHEATS    (0.0.23) CHEATS UNLOCK and one switch per cheat the game contains (cheats.cpp)
 *   BUTTONS   (0.0.18) one row per function, a keyboard and a controller column, over the game's Saturn pad
 *             picture; replaces the game's Key Assign dialog and its TYPE A to D page (decisions log, 1 Nov 2026)
 *
 * The game draws and runs its Options page in one routine (0x432170), called once per frame by 0x432090 while no
 * sub-page is open. That call is redirected here. The GAMEPLAY and SOUND pages run the game's own routine, so its
 * value pictures, blinking, music and sound tests behave exactly as before: its 30 drawing calls go through the two
 * filters below, which drop the rows of other pages and move the kept rows, and Up / Down / B are taken here.
 * The other pages are drawn here with the game's own background, cursor arrow and fonts (text routine 0x428E20).
 * [Proxy] OriginalOptionsMenu=1 leaves the original screen.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "proxy.h"
#include "present.h"
#include "input_sdl.h"
#include "options_menu.h"
#include "draw_distance.h"
#include "main_menu.h"
#include "cheats.h"

DisplayConfig proxy_display_get();
void proxy_display_set(const DisplayConfig &d);
void proxy_draw_distance_set(DrawDistance level);
void proxy_speed_set(bool kmh);

namespace {

BYTE *g_base;
template <typename T> T &at(DWORD va) { return *(T *)(g_base + (va - 0x400000)); }

/* game routines */
using draw_image_t = int (__cdecl *)(int x, int y, int image);
using draw_text_t = int (__cdecl *)(int font, int x, int y, const char *fmt, ...);
using get_image_t = int (__fastcall *)(int set, int index);
using page_t = void (__cdecl *)();
draw_image_t game_draw_image;   /* 0x42A3A0 */
draw_text_t game_draw_text;     /* 0x428E20 */
get_image_t game_get_image;     /* 0x428AB0 */
page_t game_page;               /* 0x432170 */

/* game state */
constexpr DWORD kInputPressed = 0xC583E2;   /* WORD: buttons pressed this frame */
constexpr DWORD kCursor = 0x4C3FD4, kSubPage = 0x4C3FD0, kFrame = 0x4C3FFC, kMode = 0x482230;
constexpr DWORD kImages = 0x4C4064, kFontNormal = 0x4C4074, kFontSelected = 0x4C4078;
constexpr DWORD kClipLevel = 0x48AC30, kBuffers = 0x481D80, kBgDisp = 0x48ABD4, kSeeThrough = 0x48AC08;
constexpr DWORD kHwnd = 0xC58488;
constexpr WORD kUp = 0x1008, kDown = 0x2080, kRight = 0x8000, kLeft = 0x4000, kConfirm = 0x0600, kBack = 0x0100;

enum Page { Top, Gameplay, Sound, Display, Controls, Cheats, Buttons };
Page g_page = Top;
int g_row;             /* cursor on our page */
int g_col, g_top;      /* BUTTONS page: column (0 keyboard, 1 controller) and the first row shown */
const int *g_map;      /* drawing filter while the game's routine runs: game row -> our row, -1 dropped */

/* ---------- drawing filter for the game's routine ---------- */

int game_row(int y) { return y < 0x10 ? -1 : (y - 0x10) / 0x18; }   /* rows at y = 0x18 + 0x18 x row */

int __cdecl filter_image(int x, int y, int image)
{
    if (g_map) {
        int r = game_row(y);
        if (r >= 0) {
            if (r > 7 || g_map[r] < 0) return 0;
            y += 0x18 * (g_map[r] - r);
        }
    }
    return game_draw_image(x, y, image);
}

int __cdecl filter_text(int font, int x, int y, const char *s)
{
    if (g_map) {
        if (s == (const char *)(g_base + (0x488F2C - 0x400000))) return 0;   /* its EXIT: our BACK instead */
        int r = game_row(y);
        if (r >= 0) {
            if (r > 7 || g_map[r] < 0) return 0;
            y += 0x18 * (g_map[r] - r);
        }
    }
    return game_draw_text(font, x, y, "%s", s);
}

/* ---------- our pages ---------- */

struct Item {
    const char *label;
    const char *const *choices;   /* nullptr: a number shown as "< n% >", or a link */
    int (*get)();
    void (*set)(int);
    int lo, hi, step;
    Page link;                    /* for links: the page it opens (Top = none) */
    bool (*locked)();             /* CHEATS page: not yet earned (shown as LOCKED, not changed) */
    const char *hint, *locked_hint;   /* CHEATS page: the line at the bottom for the row chosen */
};

const char *const kScreen[] = { "FULL SCREEN", "WINDOW", nullptr };
const char *const kScaling[] = { "SHARP", "NEAREST", "INTEGER", nullptr };
const char *const kPicture[] = { "16:10", "4:3", "FRAME", nullptr };
const char *const kOnOff[] = { "ON", "OFF", nullptr };
const char *const kClip[] = { "NORMAL", "WIDE", "VAST", "FAR", "MAX", nullptr };   /* FAR and MAX: ours (draw_distance.cpp) */
const char *const kMotion[] = { "FAST", "SMOOTH", nullptr };
const char *const kNoYes[] = { "NO", "YES", nullptr };
const char *const kLayout[] = { "MODERN", "CLASSIC", "CUSTOM", nullptr };   /* CUSTOM: shown, not chosen */

void post_command(int id) { PostMessageA(at<HWND>(kHwnd), WM_COMMAND, id, 0); }   /* the game's own menu command */

DisplayConfig dget() { return proxy_display_get(); }
InputConfig iget() { return input_get_config(); }

const Item kDisplayItems[] = {
    { "SCREEN", kScreen, [] { return (int)dget().windowed; }, [](int v) { DisplayConfig d = dget(); d.windowed = v; proxy_display_set(d); } },
    { "SCALING", kScaling, [] { Scaling s = dget().scaling; return s == Scaling::Nearest ? 1 : s == Scaling::Integer ? 2 : 0; },
      [](int v) { DisplayConfig d = dget(); d.scaling = v == 1 ? Scaling::Nearest : v == 2 ? Scaling::Integer : Scaling::SharpBilinear; proxy_display_set(d); } },
    { "PICTURE", kPicture, [] { Picture p = dget().picture; return p == Picture::Fill ? 1 : p == Picture::Frame ? 2 : 0; },
      [](int v) { DisplayConfig d = dget(); d.picture = v == 1 ? Picture::Fill : v == 2 ? Picture::Frame : Picture::Native; proxy_display_set(d); } },
    { "VSYNC", kOnOff, [] { return dget().vsync ? 0 : 1; }, [](int v) { DisplayConfig d = dget(); d.vsync = v == 0; proxy_display_set(d); } },
    { "CLIP SIZE", kClip,
      [] { int dd = (int)draw_distance_get(); if (dd) return 2 + dd; int c = at<int>(kClipLevel); return c >= 0 && c <= 2 ? c : 0; },
      [](int v) {
          /* FAR and MAX keep the game's own Clip Size at VAST, so the game alone never meets a value it does not know */
          if (v <= 2) { proxy_draw_distance_set(DrawDistance::Game); post_command(0x1CE + v); }
          else { proxy_draw_distance_set((DrawDistance)(v - 2)); if (at<int>(kClipLevel) != 2) post_command(0x1D0); }
      } },
    { "MOTION", kMotion, [] { return at<int>(kBuffers) == 3 ? 1 : 0; }, [](int v) { post_command(v ? 0x1D2 : 0x1D1); } },
    { "BG DISP", kOnOff, [] { return at<int>(kBgDisp) ? 0 : 1; }, [](int v) { if ((v == 0) != (at<int>(kBgDisp) != 0)) post_command(0x1D3); } },
    { "SEETHROUGH", kOnOff, [] { return at<int>(kSeeThrough) ? 0 : 1; }, [](int v) { if ((v == 0) != (at<int>(kSeeThrough) != 0)) post_command(0x1D4); } },
};

const Item kControlItems[] = {
    { "LAYOUT", kLayout, [] { return input_preset_shown(); },   /* CUSTOM once the controller column is changed by hand */
      [](int v) {
          if (v == 2) v = input_preset_shown() == 0 ? 1 : 0;      /* stepping onto CUSTOM goes on to the other preset */
          input_apply_preset(v ? PadLayout::PS1 : PadLayout::Modern);
      } },
    { "TRIGGER POINT", nullptr, [] { return iget().trigger_threshold; }, [](int v) { InputConfig c = iget(); c.trigger_threshold = v; input_set_config(c); }, 10, 90, 10 },
    { "DEAD ZONE", nullptr, [] { return iget().dead_zone; }, [](int v) { InputConfig c = iget(); c.dead_zone = v; input_set_config(c); }, 0, 30, 2 },
    { "KEYS WITH PAD", kOnOff, [] { return iget().keyboard_with_pad ? 0 : 1; }, [](int v) { InputConfig c = iget(); c.keyboard_with_pad = v == 0; input_set_config(c); } },
    { "BUTTONS", nullptr, nullptr, nullptr, 0, 0, 0, Buttons },
};

/* CHEATS (0.0.23): ON / OFF as the other pages; a cheat with an earning rule shows LOCKED until earned */
template <int C> int cheat_onoff() { return cheat_get(C) ? 0 : 1; }
template <int C> void cheat_put(int v) { cheat_set(C, v == 0); }
template <int C> bool cheat_locked() { return !cheat_available(C); }
#define CHEAT_ROW(label, c, hint, lhint) { label, kOnOff, cheat_onoff<c>, cheat_put<c>, 0, 0, 0, Top, cheat_locked<c>, hint, lhint }
const Item kCheatItems[] = {
    { "CHEATS UNLOCK", kNoYes, [] { return cheat_get(CHEAT_UNLOCK) ? 1 : 0; }, [](int v) { cheat_set(CHEAT_UNLOCK, v == 1); },
      0, 0, 0, Top, nullptr, "NO: EARN THE CARS FIRST  YES: ALL NOW", nullptr },
    CHEAT_ROW("BONUS CARS", CHEAT_BONUS_CARS, "MORE CARS AT THE PC MODE CAR SELECT", "EARN: TOP 3 ON A COURSE (PC, NORMAL+)"),
    CHEAT_ROW("HORSE", CHEAT_HORSE, "THE HORSE AT THE PC MODE CAR SELECT", "EARN: WIN ALL 3 COURSES (PC, NORMAL+)"),
    CHEAT_ROW("HORSE 2", CHEAT_HORSE2, "HORSE 2 AT THE PC MODE CAR SELECT", "EARN: WIN ENDURANCE WITH THE HORSE"),
    CHEAT_ROW("MIRROR", CHEAT_MIRROR, "PC MODE: EVERY COURSE MIRRORED", nullptr),
    CHEAT_ROW("TIME LAP", CHEAT_TIME_LAP, "RACE ALONE AGAINST THE CLOCK", nullptr),
    CHEAT_ROW("KARAOKE", CHEAT_KARAOKE, "ARCADE MODE, GAME MODE NORMAL: SING!", nullptr),
    CHEAT_ROW("MANIAC", CHEAT_MANIAC, "EXTRA HARD RIVALS (M ON THE DISPLAY)", nullptr),
    CHEAT_ROW("BILLBOARD", CHEAT_BILLBOARD, "ADVANCED: THE SECRET BILLBOARD", nullptr),
    CHEAT_ROW("STATUE", CHEAT_STATUE, "EXPERT: STOP BESIDE THE STATUE", nullptr),
    CHEAT_ROW("AFRO", CHEAT_AFRO, "THE TEAM PHOTO IN THE ENDING: AFROS", nullptr),
};

const Item kTopItems[] = {
    { "GAMEPLAY", nullptr, nullptr, nullptr, 0, 0, 0, Gameplay },
    { "SOUND", nullptr, nullptr, nullptr, 0, 0, 0, Sound },
    { "DISPLAY", nullptr, nullptr, nullptr, 0, 0, 0, Display },
    { "CONTROLS", nullptr, nullptr, nullptr, 0, 0, 0, Controls },
    { "CHEATS", nullptr, nullptr, nullptr, 0, 0, 0, Cheats },
};

struct PageDef { const Item *items; int n; int last_row; int spacing; const char *last_label; int value_x = 0x78; int top = 0x18; };
PageDef page_def(Page p)
{
    switch (p) {
    case Display: return { kDisplayItems, 8, 8, 0x14, "BACK" };     /* 9 rows: a little closer together */
    case Controls: return { kControlItems, 5, 7, 0x18, "BACK" };
    case Cheats: return { kCheatItems, 11, 11, 0x0F, "BACK", 0x90, 0x16 };   /* 12 rows, closer still; BACK just above the hint line */
    default: return { kTopItems, cheats_installed() ? 5 : 4, 7, 0x18, "EXIT" };
    }
}

/* the game's pages: which of its rows are shown, on which of our rows */
const int kGameplayMap[8] = { 0, 1, 2, -1, -1, -1, -1, 7 };
const int kSoundMap[8] = { -1, -1, -1, 0, 1, 2, -1, 7 };
const int kGameplayRows[] = { 0, 1, 2 }, kSoundRows[] = { 3, 4, 5 };

int font(bool selected) { return selected ? at<int>(kFontSelected) : at<int>(kFontNormal); }
int image(int i) { return game_get_image(at<int>(kImages), i); }

void enter(Page p, int row) { g_page = p; g_row = row; }

/* ---------- the BUTTONS page ---------- */

/* Laid out as jayyanz's mock-ups (Mockups\controls, 1_browse, 2_waiting, 3_end_of_list): the game's "Key
   Configuration" title, column heads, 8 rows of a list that also holds DEFAULTS and BACK, one hint line. */
constexpr int kFns = INPUT_FUNCTIONS, kDefaultsRow = kFns, kBackRow = kFns + 1, kRows = kFns + 2, kShown = 8;
constexpr int kPadPicture = 0x2B;   /* the Saturn pad picture of the game's Key Configuration page (0x432CF5) */
constexpr int kTitle = 0x2A;        /* its title, drawn at (0x40, 0) as the game does */
constexpr int kColKey = 0x6D, kColPad = 0xD3, kHeadY = 0x36, kRowsY = 0x47, kRowStep = 0x0F;
constexpr int kHintY = 0xCA;

/* Delete: the one key the game's menu reading does not give (it gives Enter as C and Esc as B, in both its
   keyboard and joystick readers). An edge, only while the game has the focus. */
bool delete_pressed()
{
    static bool was;
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    bool down = fg && (GetWindowThreadProcessId(fg, &pid), pid == GetCurrentProcessId()) && (GetAsyncKeyState(VK_DELETE) & 0x8000);
    bool e = down && !was; was = down; return e;
}

void buttons_back() { enter(Controls, 4); }

void buttons_input(WORD in)
{
    static int quiet;              /* frames to ignore after a wait ends (Esc or the bound key may reach the game a frame later) */
    static bool was_waiting;
    bool del = delete_pressed();   /* every frame, so no stale edge */
    if (input_capturing()) { input_capture_poll(); was_waiting = true; return; }
    if (was_waiting) { was_waiting = false; quiet = 8; }
    if (quiet > 0) { quiet--; return; }
    bool pad = at<WORD>(0x48ABB0) == 0x20;
    if (in & kUp) g_row = (g_row + kRows - 1) % kRows;
    else if (in & kDown) g_row = (g_row + 1) % kRows;
    else if (g_row < kFns && (in & (kLeft | kRight))) g_col ^= 1;
    else if (in & kConfirm) {
        if (g_row == kBackRow) buttons_back();
        else if (g_row == kDefaultsRow) input_reset_defaults();
        else input_capture_start(g_row, g_col == 1);
    } else if (g_row < kFns && (del || (pad && (in & 0x0040)))) input_bind_clear(g_row, g_col == 1);
    else if (in & kBack) buttons_back();
    /* one row at a time */
    if (g_row < g_top) g_top = g_row;
    if (g_row >= g_top + kShown) g_top = g_row - kShown + 1;
}

void buttons_draw()
{
    game_draw_image(0, 0, image(kPadPicture));
    game_draw_image(0x40, 0, image(kTitle));
    game_draw_text(font(false), kColKey, kHeadY, "%s", "KEYBOARD");
    game_draw_text(font(false), kColPad, kHeadY, "%s", "CONTROLLER");
    int cf = -1; bool cpad = false;
    bool waiting = input_capturing(&cf, &cpad);
    for (int i = 0; i < kShown && g_top + i < kRows; i++) {
        int r = g_top + i, y = kRowsY + kRowStep * i;
        bool sel = r == g_row;
        if (sel) game_draw_image(0, y, image(1));
        if (r >= kFns) { game_draw_text(font(sel), 8, y, "%s", r == kBackRow ? "BACK" : "DEFAULTS"); continue; }
        game_draw_text(font(sel), 8, y, "%s", input_function_label(r));
        for (int c = 0; c < 2; c++) {
            char b[24];
            bool here = waiting && cf == r && cpad == (c == 1);
            if (here) snprintf(b, sizeof b, "%s", cpad ? "PRESS BUTTON" : "PRESS KEY");
            else input_binding_name(r, c == 1, b, sizeof b);
            game_draw_text(font(here || (sel && g_col == c)), c ? kColPad : kColKey, y, "%s", b);
        }
    }
    /* where the list is (the mock-ups have small up / down arrows; the font has none: it ends at @, a right arrow) */
    char pos[16]; snprintf(pos, sizeof pos, "%d-%d OF %d", g_top + 1, g_top + kShown, kRows);
    game_draw_text(font(false), 8, kHeadY, "%s", pos);
    char h[64];
    bool pad = at<WORD>(0x48ABB0) == 0x20;
    const char *a = pad ? input_pad_button_name(0) : "A", *bk = pad ? input_pad_button_name(1) : "B";
    if (waiting) snprintf(h, sizeof h, "PRESS A %s FOR %s   %d", cpad ? "BUTTON" : "KEY", input_function_label(cf), input_capture_seconds_left());
    else if (g_row == kDefaultsRow) snprintf(h, sizeof h, "ENTER OR %s: RESET ALL TO DEFAULT", a);
    else {
        snprintf(h, sizeof h, "ENTER OR %s: ASSIGN   ESC OR %s: BACK", a, bk);
        if (strlen(h) > 39) snprintf(h, sizeof h, "ENTER %s: ASSIGN  ESC %s: BACK", a, bk);
    }
    game_draw_text(font(false), 8, kHintY, "%s", h);
}

void our_page_input(WORD in)
{
    PageDef d = page_def(g_page);
    int nrows = d.last_row + 1;
    /* rows: items 0..n-1, a gap, then BACK / EXIT on last_row */
    if (in & kUp) { do g_row = (g_row + nrows - 1) % nrows; while (g_row >= d.n && g_row < d.last_row); }
    else if (in & kDown) { do g_row = (g_row + 1) % nrows; while (g_row >= d.n && g_row < d.last_row); }
    else if (g_row < d.n && (in & (kLeft | kRight))) {
        const Item &it = d.items[g_row];
        if (it.get && it.set && !(it.locked && it.locked())) {
            int v = it.get();
            if (it.choices) {
                int n = 0; while (it.choices[n]) n++;
                v = (in & kRight) ? (v + 1) % n : (v + n - 1) % n;
            } else {
                v += (in & kRight) ? it.step : -it.step;
                if (v < it.lo) v = it.lo;
                if (v > it.hi) v = it.hi;
            }
            it.set(v);
        }
    } else if (in & kConfirm) {
        if (g_row == d.last_row) {
            if (g_page == Top) at<int>(kMode) = 6;    /* EXIT to Mode Select, as the game's own */
            else enter(Top, (int)g_page - 1);
        } else if (g_row < d.n && d.items[g_row].link != Top) {
            if (g_page == Top) enter(d.items[g_row].link, 0);
            else if (d.items[g_row].link == Buttons) { enter(Buttons, 0); g_col = 0; g_top = 0; }
        }
    } else if (in & kBack) {
        if (g_page != Top) enter(Top, (int)g_page - 1);
        else if (g_row != d.last_row) g_row = d.last_row;      /* as the game's: B first moves to EXIT */
        else at<int>(kMode) = 6;
    }
}

void our_page_draw()
{
    PageDef e = page_def(g_page);
    auto row_y = [&](int r) { return e.top + e.spacing * r; };
    game_draw_image(0, 0, image(0));               /* the game's background and cursor arrow */
    game_draw_image(0, row_y(g_row), image(1));
    for (int r = 0; r < e.n; r++) {
        const Item &it = e.items[r];
        game_draw_text(font(r == g_row), 8, row_y(r), "%s", it.label);
        if (!it.get) continue;
        if (it.locked && it.locked()) { game_draw_text(font(false), e.value_x, row_y(r), "%s", "LOCKED"); continue; }
        int v = it.get();
        if (it.choices) {                           /* every choice, the one in force highlighted (as the game's) */
            int x = e.value_x;
            for (int c = 0; it.choices[c]; c++) {
                game_draw_text(font(c == v), x, row_y(r), "%s", it.choices[c]);
                x += 8 * ((int)strlen(it.choices[c]) + 2);
            }
        } else {
            char b[24]; snprintf(b, sizeof b, "%s %d %s", v > it.lo ? "<" : " ", v, v < it.hi ? ">" : " ");   /* the font's % is not a percent sign */
            game_draw_text(font(true), e.value_x, row_y(r), "%s", b);
        }
    }
    game_draw_text(font(g_row == e.last_row), 8, row_y(e.last_row), "%s", e.last_label);
    if (g_row < e.n && e.items[g_row].hint) {
        const Item &it = e.items[g_row];
        const char *h = it.locked && it.locked() && it.locked_hint ? it.locked_hint : it.hint;
        game_draw_text(font(false), 8, 0xCA, "%s", h);
    }
}

/* GAMEPLAY and SOUND: the game's own routine with its cursor on the matching row. Returns false if we left.
   GAMEPLAY also has our SPEED row (0.0.19) under the game's three; while it is chosen the game's cursor sits on its
   hidden KEY ASSIGN row (6), whose drawing the filter drops, and the game gets no presses. */
bool game_page_run(WORD in)
{
    bool gp = g_page == Gameplay;
    const int *rows = gp ? kGameplayRows : kSoundRows;
    const int n = gp ? 4 : 3, last = 7;
    if (in & kUp) g_row = g_row == last ? n - 1 : g_row == 0 ? last : g_row - 1;
    else if (in & kDown) g_row = g_row == n - 1 ? last : g_row == last ? 0 : g_row + 1;
    else if ((in & kBack) || (g_row == last && (in & kConfirm))) { enter(Top, (int)g_page - 1); return false; }
    bool ours = gp && g_row == 3;
    if (ours && (in & (kLeft | kRight))) proxy_speed_set(!speed_kmh_get());
    at<int>(kCursor) = g_row == last ? 7 : ours ? 6 : rows[g_row];
    WORD pass = (g_row == last || ours) ? 0 : (WORD)(in & ~(kUp | kDown | kBack | 0x0800));   /* Left / Right / confirm only */
    at<WORD>(kInputPressed) = pass;
    g_map = gp ? kGameplayMap : kSoundMap;
    game_page();
    g_map = nullptr;
    at<WORD>(kInputPressed) = in;
    if (gp) {
        int y = 0x18 + 0x18 * 3;
        if (ours) game_draw_image(0, y, image(1));
        game_draw_text(font(ours), 8, y, "%s", "SPEED");
        bool kmh = speed_kmh_get();
        game_draw_text(font(!kmh), 0x6E, y, "%s", "MPH");
        game_draw_text(font(kmh), 0x6E + 8 * 5, y, "%s", "KM/H");
    }
    game_draw_text(font(g_row == last), 8, 0x18 + 0x18 * last, "%s", "BACK");
    return true;
}

void log_state(WORD in)
{
    static int last_page = -1, last_row = -1;
    static const char *const names[] = { "OPTIONS", "GAMEPLAY", "SOUND", "DISPLAY", "CONTROLS", "CHEATS", "BUTTONS" };
    if ((int)g_page != last_page || g_row != last_row) {
        plog("options screen: %s page, row %d (buttons %04x)", names[g_page], g_row, in);
        last_page = g_page; last_row = g_row;
    }
}

void __cdecl options_page()
{
    if (at<int>(kFrame) <= 1) { enter(Top, 0); }   /* the screen has just opened (its init clears the counter) */
    WORD in = at<WORD>(kInputPressed);
    if (g_page == Buttons) {
        buttons_input(in);
        if (g_page == Buttons) buttons_draw(); else our_page_draw();
        log_state(in);
        return;
    }
    if (g_page == Gameplay || g_page == Sound) {
        if (!game_page_run(in)) our_page_draw();    /* went back to the top page this frame */
        log_state(in);
        return;
    }
    our_page_input(in);
    if (g_page == Gameplay || g_page == Sound) game_page_run(0);   /* opened this frame */
    else our_page_draw();
    log_state(in);
}

} // namespace

void options_menu_init(BYTE *base)
{
    g_base = base;
    game_draw_image = (draw_image_t)(base + (0x42A3A0 - 0x400000));
    game_draw_text = (draw_text_t)(base + (0x428E20 - 0x400000));
    game_get_image = (get_image_t)(base + (0x428AB0 - 0x400000));
    game_page = (page_t)(base + (0x432170 - 0x400000));
}

void *options_menu_page() { return (void *)options_page; }
void *options_menu_filter_image() { return (void *)filter_image; }
void *options_menu_filter_text() { return (void *)filter_text; }

/* The game's drawing calls inside its Options page routine (0x432170 to 0x432C74). */
const DWORD kOptionsImageCalls[17] = { 0x43218D, 0x4321E2, 0x43223D, 0x432298, 0x4322EF, 0x432348, 0x4323A3, 0x4323FE, 0x432459,
                                       0x4324B0, 0x43250B, 0x432566, 0x4325C1, 0x43261C, 0x432672, 0x4326CD, 0x4326F6 };
const DWORD kOptionsTextCalls[13] = { 0x43271C, 0x432743, 0x43276A, 0x432791, 0x4327B8, 0x4327E2, 0x432816, 0x432840, 0x432860,
                                      0x432883, 0x4328A8, 0x4328CD, 0x4328FD };
