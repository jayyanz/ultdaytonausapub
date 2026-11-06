/* The CHEATS page (jayyanz, 5 Nov 2026, build 0.0.23): the game's own cheats switched from the Options screen
 * instead of button codes. Each switch sets the game's own flag, or feeds the game the press its own code waits for;
 * nothing is reimplemented. Research in exe-findings.md ("Cheats in full").
 *
 *   CHEATS UNLOCK  NO: cheats with an earning rule in the game's code (the bonus cars and horses) are available once
 *                  earned; YES: everything at once. Cheats with no earning rule in the code are always available.
 *   BONUS CARS     PC MODE car select: the cars earned by a top-three finish on each course (6, 8, 10 cars)
 *   HORSE          PC MODE: cars 11 and 12, earned by winning all three courses
 *   HORSE 2        PC MODE: cars 13 and 14. The game's own rule asks for an ENDURANCE win driving HORSE 2 itself, so it
 *                  could never be earned; as decided by jayyanz it is earned by an ENDURANCE win driving the HORSE.
 *   MIRROR         PC MODE: every course mirrored (the game's Start held at course select)
 *   TIME LAP       every race a Time Lap (the game's Start held at the transmission choice)
 *   KARAOKE        ARCADE MODE, GAME MODE NORMAL: the karaoke track with lyrics (the game's Up held at course select)
 *   MANIAC         extra-hard rivals, an M on the race display (the game's Up, Up, Down, Down, Left, Right, Left, Right,
 *                  A, B, C on the SEGA / AM2 logos sets byte 0x11B708F; nothing clears it)
 *   BILLBOARD      Advanced course: the hidden billboard (the game's Up, Down, Left, Right, A, B, C in a race)
 *   STATUE         Expert course: the Jeffry statue dances while the car is stopped beside it (the game's X presses
 *                  there; its code also asks for the car's speed, [car]+8, to be 0)
 *   AFRO           (0.0.24) the AM2 team photo of the ending (PC_END.BIN, also PC_OVER.BIN) with afros: byte 0x11B708E,
 *                  which the game sets from a code typed on the ending in PC MODE at VERY HARD (Left x 15, then C) or
 *                  from A, F, R, O entered in its goal sequence (letters 0, 5, 17, 14)
 *
 * The old ways in are removed (jayyanz): the title-screen button codes, DAYTONA.INI [Config] Command, and the presses
 * above when the switch is off.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "proxy.h"
#include "cheats.h"

namespace {

BYTE *g_base;
char g_ini[MAX_PATH];
template <typename T> T &at(DWORD va) { return *(T *)(g_base + (va - 0x400000)); }

using intfn = int (__cdecl *)();
using voidfn = void (__cdecl *)();
intfn game_car_count;      /* 0x44E280 */
voidfn game_select;        /* 0x44DC40: course and car select (screen mode 13) */
voidfn game_expert_object; /* 0x4141E0 */

constexpr DWORD kHeld = 0xC583E0, kPressed = 0xC583E2;
constexpr DWORD kPcMode = 0xC58458;      /* byte: 1 PC MODE, 0 ARCADE MODE */
constexpr DWORD kSubState = 0x4C4FC0;    /* select screen step: 0 course select opening, 1 course select, 9 car select */
constexpr DWORD kCar = 0x11D5A74;        /* car chosen (10, 11 HORSE; 12, 13 HORSE 2) */
constexpr DWORD kTop3 = 0x11D5A7A, kWins = 0x11D5A88;   /* earned: course bits (1, 2, 4); kWins 8: HORSE 2 */
constexpr DWORD kCommand = 0x48AC04;     /* DAYTONA.INI [Config] Command */
constexpr WORD kStart = 0x0800, kUp = 0x1000, kX = 0x0040;

bool g_val[CHEAT_COUNT];
BYTE g_horse;             /* read by the race-end code instead of 0x11B7A6C (HORSE 2 chosen): any horse chosen */
WORD g_statue;            /* read by the Expert scenery code instead of the held buttons */
int g_statue_frames;
BYTE g_afro;              /* read instead of 0x11B708E: the afro team photo */
constexpr DWORD kAfroReads[] = { 0x417BC7, 0x41DB3C, 0x44020F, 0x4402A9, 0x440392 };   /* movsx edx,byte [0x11B708E] */

const char *const kKeys[CHEAT_COUNT] = { "Unlock", "BonusCars", "Horse", "Horse2", "Mirror", "TimeLap", "Karaoke", "Maniac", "Billboard", "Statue", "Afro" };
const bool kDefault[CHEAT_COUNT] = { false, true, true, true, false, false, false, false, false, false, false };

bool on(int c) { return g_val[c] && cheat_available(c); }

/* ---------- cars ---------- */

int __cdecl car_count()
{
    int earned = game_car_count();               /* 4, 6, 8, 10, 12 or 14 from the game's own bits */
    int avail = g_val[CHEAT_UNLOCK] ? 14 : earned;
    int n = 4;
    if (on(CHEAT_BONUS_CARS)) n = avail < 10 ? avail : 10;
    if (on(CHEAT_HORSE) && avail >= 12) n = 12;      /* the cars are in order: a horse brings the bonus cars with it */
    if (on(CHEAT_HORSE2) && avail >= 14) n = 14;
    plog("cheats: car select with %d cars (earned %d%s)", n, earned, g_val[CHEAT_UNLOCK] ? ", unlock on" : "");
    return n;
}

/* ---------- course and car select: the presses the game's own codes wait for ---------- */

void __cdecl select_screen()
{
    WORD held = at<WORD>(kHeld), pressed = at<WORD>(kPressed);
    WORD h = held, p = pressed;
    int sub = at<int>(kSubState);
    bool pc = at<BYTE>(kPcMode) != 0;
    if (sub == 0 || sub == 1) {
        if (pc) { h &= ~kStart; p &= ~kStart; if (on(CHEAT_MIRROR)) h |= kStart; }
        else { h &= ~kUp; if (on(CHEAT_KARAOKE)) h |= kUp; }
    } else if (sub == 9) {
        h &= ~kStart;
        if (on(CHEAT_TIME_LAP)) h |= kStart;
    }
    at<WORD>(kHeld) = h; at<WORD>(kPressed) = p;
    at<BYTE>(0x11B708F) = on(CHEAT_MANIAC);   /* read when the race sets up its rivals */
    game_select();
    at<WORD>(kHeld) = held; at<WORD>(kPressed) = pressed;
    g_horse = at<int>(kCar) >= 10;
}

/* the SEGA / AM2 logos code (0x42F755 calls the game's sequence matcher 0x41DDF0): never matches now */
int __cdecl no_code(const void *, void *, int) { return 0; }

/* ---------- Advanced: the billboard ---------- */

void __cdecl billboard()
{
    if (!on(CHEAT_BILLBOARD) || at<WORD>(0x482100)) return;
    /* what the game's own code does once its sequence is complete (0x414197 to 0x4141C9) */
    DWORD idx = at<WORD>(0xC0D75A);
    at<WORD>(0x482100) = 1;
    at<WORD>(0x482104) = 0;
    at<DWORD>(0xBFD664 + idx * 24) = at<DWORD>(0xC59F58);
    at<WORD>(0xBFD674 + idx * 24) = 0x460;
}

/* ---------- Expert: the statue ---------- */

void __cdecl expert_object()
{
    /* an X press every 16 frames while the switch is on: the game turns the model one step a press (ARCADE MODE), or
       starts its own steps (PC MODE), only while the car is beside it */
    g_statue = on(CHEAT_STATUE) && (++g_statue_frames & 8) ? kX : 0;
    game_expert_object();
}

/* ---------- karaoke: the lyric clock (0.0.24) ---------- */

/* The lyric drawer 0x435D60 reads its clock from 0x494FB8 (20ths of a second since the song started), which the PC
   game resets when the song starts and never advances (exe-findings.md). It now reads g_lyric, set before each call
   from the music actually playing; without our CD audio emulation, from the time the game noted at the song's start. */
int g_lyric;
voidfn game_lyrics;        /* 0x435D60 */
constexpr DWORD kLyricCalls[] = { 0x44A9B1, 0x44ABF0, 0x44AC73, 0x44AD71, 0x44AE14, 0x44AF1E, 0x44B3D2, 0x44B4A1 };

void __cdecl lyrics()
{
    int t = cda_track_ticks20();
    if (t < 0) {
        double start = at<double>(0x494FB0);   /* timeGetTime x 1e6, noted by the play routine 0x457320 */
        double ms = ((double)timeGetTime() * 1e6 - start) / 1e6;
        t = ms > 0 ? (int)(ms / 50) : 0;
    }
    g_lyric = t;
    game_lyrics();
}

bool redirect(DWORD site, DWORD target, void *fn, void **orig)
{
    BYTE *p = g_base + (site - 0x400000);
    if (p[0] != 0xE8 || (DWORD)(site + 5 + *(LONG *)(p + 1)) != target) return false;
    *orig = g_base + (target - 0x400000);
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(LONG *)(p + 1) = (LONG)((BYTE *)fn - (p + 5));
    VirtualProtect(p, 5, old, &old);
    return true;
}

bool expect(DWORD site, const BYTE *code, int n)
{
    return !memcmp(g_base + (site - 0x400000), code, n);
}

void write(DWORD site, const void *bytes, int n)
{
    BYTE *p = g_base + (site - 0x400000);
    DWORD old;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
}

bool operand(DWORD site, const BYTE *op, int oplen, DWORD old_addr, const void *new_addr)
{
    BYTE *p = g_base + (site - 0x400000);
    if (memcmp(p, op, oplen) || *(DWORD *)(p + oplen) != old_addr) return false;
    DWORD a = (DWORD)new_addr;
    write(site + oplen, &a, 4);
    return true;
}

void install_lyrics()
{
    static const BYTE moveax[] = { 0xA1, 0xB8, 0x4F, 0x49, 0x00 };   /* 0x435D80: mov eax,[0x494FB8] */
    bool ok = expect(0x435D80, moveax, sizeof moveax);
    for (DWORD c : kLyricCalls) {
        BYTE *p = g_base + (c - 0x400000);
        ok = ok && p[0] == 0xE8 && (DWORD)(c + 5 + *(LONG *)(p + 1)) == 0x435D60;
    }
    if (!ok) { plog("karaoke: unexpected code, lyric clock NOT fixed"); return; }
    for (DWORD c : kLyricCalls) redirect(c, 0x435D60, (void *)lyrics, (void **)&game_lyrics);
    operand(0x435D80, moveax, 1, 0x494FB8, &g_lyric);
    plog("karaoke: lyric clock follows the music (%s)", cda_available() ? "CD audio emulation" : "time since the song started");
}

} // namespace

bool cheat_get(int c) { return g_val[c]; }

void cheat_set(int c, bool v)
{
    g_val[c] = v;
    g_afro = on(CHEAT_AFRO);
    WritePrivateProfileStringA("Cheats", kKeys[c], v ? (c == CHEAT_UNLOCK ? "Yes" : "On") : (c == CHEAT_UNLOCK ? "No" : "Off"), g_ini);
    plog("cheats: %s %s", kKeys[c], v ? "on" : "off");
}

bool cheats_installed() { return g_base != nullptr; }

bool cheat_available(int c)
{
    if (!g_base) return false;
    if (c == CHEAT_UNLOCK || g_val[CHEAT_UNLOCK]) return true;
    switch (c) {
    case CHEAT_BONUS_CARS: return at<BYTE>(kTop3) != 0;
    case CHEAT_HORSE: return (at<BYTE>(kWins) & 7) == 7;
    case CHEAT_HORSE2: return (at<BYTE>(kWins) & 8) != 0;
    default: return true;   /* no earning rule in the game's code */
    }
}

void cheats_after_settings_loaded()
{
    if (!g_base) return;
    if (at<int>(kCommand)) plog("cheats: DAYTONA.INI [Config] Command ignored (the CHEATS page replaces it)");
    at<int>(kCommand) = 0;
}

void cheats_install(BYTE *base, const char *proxy_ini)
{
    g_base = base;
    lstrcpynA(g_ini, proxy_ini, MAX_PATH);
    for (int c = 0; c < CHEAT_COUNT; c++) {
        char v[16];
        GetPrivateProfileStringA("Cheats", kKeys[c], "", v, sizeof v, g_ini);
        g_val[c] = v[0] ? (!lstrcmpiA(v, "On") || !lstrcmpiA(v, "Yes") || !lstrcmpiA(v, "1")) : kDefault[c];
    }
    install_lyrics();   /* its own check: the lyric clock is fixed even if the cheats cannot be installed */
    /* every site checked first: all or none, so another build of the exe keeps the game as it was */
    static const BYTE title[] = { 0x66, 0x81, 0xF9, 0xA8, 0xAA, 0x75, 0x0E };   /* 0x414E1A: cmp cx,0xAAA8 / jne */
    static const BYTE movbl[] = { 0x8A, 0x1D, 0x6C, 0x7A, 0x1B, 0x01 };        /* 0x44B236: mov bl,[0x11B7A6C] */
    static const BYTE movax[] = { 0x66, 0xA1, 0xE0, 0x83, 0xC5, 0x00 };        /* 0x41420D: mov ax,[0xC583E0] */
    static const BYTE movsx[] = { 0x0F, 0xBE, 0x15, 0x8E, 0x70, 0x1B, 0x01 };   /* movsx edx,byte [0x11B708E] */
    auto afro_ok = [&] { for (DWORD s : kAfroReads) if (!expect(s, movsx, sizeof movsx)) return false; return true; };
    auto call_to = [&](DWORD site, DWORD target) {
        BYTE *p = g_base + (site - 0x400000);
        return p[0] == 0xE8 && (DWORD)(site + 5 + *(LONG *)(p + 1)) == target;
    };
    if (!expect(0x414E1A, title, sizeof title) || !expect(0x44B236, movbl, sizeof movbl) || !expect(0x41420D, movax, sizeof movax) ||
        !afro_ok() || !call_to(0x44E011, 0x44E280) || !call_to(0x42F755, 0x41DDF0) || !call_to(0x44A6E0, 0x44DC40) || !call_to(0x414044, 0x414150) || !call_to(0x414139, 0x4141E0)) {
        plog("cheats: unexpected code, NOT applied (the game's cheats stay as they were)");
        g_base = nullptr;
        return;
    }
    static const BYTE skip[] = { 0xEB, 0x32 };   /* title codes: jmp 0x414E4E, past both */
    write(0x414E1A, skip, 2);
    void *unused;
    redirect(0x44E011, 0x44E280, (void *)car_count, (void **)&game_car_count);
    redirect(0x42F755, 0x41DDF0, (void *)no_code, &unused);
    redirect(0x44A6E0, 0x44DC40, (void *)select_screen, (void **)&game_select);
    operand(0x44B236, movbl, 2, 0x11B7A6C, &g_horse);
    redirect(0x414044, 0x414150, (void *)billboard, &unused);
    redirect(0x414139, 0x4141E0, (void *)expert_object, (void **)&game_expert_object);
    operand(0x41420D, movax, 2, kHeld, &g_statue);
    for (DWORD s : kAfroReads) operand(s, movsx, 3, 0x11B708E, &g_afro);
    g_afro = on(CHEAT_AFRO);
    FlushInstructionCache(GetCurrentProcess(), base + 0x10000, 0x50000);
    plog("cheats: CHEATS page installed; Unlock=%s BonusCars=%d Horse=%d Horse2=%d Mirror=%d TimeLap=%d Karaoke=%d Maniac=%d Billboard=%d Statue=%d Afro=%d",
         g_val[0] ? "Yes" : "No", g_val[1], g_val[2], g_val[3], g_val[4], g_val[5], g_val[6], g_val[7], g_val[8], g_val[9], g_val[10]);
}
