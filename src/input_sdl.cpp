/* Controllers through SDL3, and the CONTROLS mapping (0.0.18).
 *
 * The game reads one Windows joystick with the old WINMM API (joyGetNumDevs, joyGetDevCaps, joyGetPosEx) in its
 * "PC Handle" device mode: one axis for steering, one for up / down, and up to nine buttons laid out like a Saturn
 * pad (1 A, 2 B, 3 C, 4 X, 5 Y, 6 Z, 7 R, 8 L, 9 Start). This file answers those three calls from SDL3 gamepads
 * (Xbox, PlayStation, Switch and most others). A background thread polls SDL; the game thread only reads the latest
 * state. All connected pads drive joystick 1 together.
 *
 * Menus: the pad is a fixed Saturn pad (South C confirms, East B goes back, West X, North Y, LB L, RB R, Start,
 * Back A, D-pad and left stick move). The keyboard keeps the game's own reading.
 *
 * Races (0.0.18, jayyanz 1 Nov 2026): each of 16 functions has one key and one pad input, set on the CONTROLS page
 * (options_menu.cpp) and kept in ddraw_proxy.ini [Controls]. The game tests each function against a mask
 * ("(pad word & mask) != 0", table 0x488AF0, loaded by 0x431EC0). Every row of that table is given one fixed bit per
 * function (kRowWords), and during races the game's two readers (keyboard 0x445B50, joystick 0x445D10, called from
 * 0x446030) are wrapped: their pad word is rebuilt from the bindings, and the joystick's steering byte from the two
 * steering inputs. VIEW DOWN is not used (jayyanz); VIEW CHANGE is the game's VIEW UP.
 * The LAYOUT presets MODERN and CLASSIC (PS1 style) fill the table.
 *
 * Wheels, pedals, arcade sticks and other joysticks that SDL does not know as a gamepad (0.0.12) are read through SDL's
 * joystick layer: button n gives the game's own button n+1, the first axis steers, and pedals (axes found resting at
 * one end of their travel) accelerate and brake. A section [Joystick <name>] overrides the axes and buttons. In races
 * their buttons do what the game's Modern assignment did (C accelerate, B and Z brake, X / Y shift down / up,
 * R view change, Start).
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "proxy.h"
#include "input_sdl.h"

namespace {

/* Saturn buttons as the game numbers WINMM joystick buttons (bit = button - 1) */
enum Sat : DWORD { SA = 1u << 0, SB = 1u << 1, SC = 1u << 2, SX = 1u << 3, SY = 1u << 4, SZ = 1u << 5, SR = 1u << 6, SL = 1u << 7, SSTART = 1u << 8 };
constexpr int kButtons = 12;

/* The pad in menus: a fixed Saturn pad (and, with Layout=Off, the pad in races too, through the game's own
   button assignment in DAYTONA.INI). */
struct Layout {
    DWORD pad_to_sat[SDL_GAMEPAD_BUTTON_COUNT];  /* Saturn button(s) for each SDL gamepad button */
    DWORD lt_button;                           /* left trigger as a button (0 = not a button) */
    DWORD rt_button;                           /* right trigger as a button */
    bool y_from_stick;                         /* Y axis from the left stick (else the D-pad only) */
};

Layout make_menu_map(bool bindings)
{
    Layout l{};
    l.pad_to_sat[SDL_GAMEPAD_BUTTON_SOUTH] = SC;           /* confirm (and accelerate with Layout=Off) */
    l.pad_to_sat[SDL_GAMEPAD_BUTTON_EAST] = SB;            /* back */
    l.pad_to_sat[SDL_GAMEPAD_BUTTON_WEST] = SX;            /* clears a binding on the CONTROLS page */
    l.pad_to_sat[SDL_GAMEPAD_BUTTON_NORTH] = SY;
    l.pad_to_sat[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = SL;
    l.pad_to_sat[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = SR;
    l.pad_to_sat[SDL_GAMEPAD_BUTTON_START] = SSTART;
    l.pad_to_sat[SDL_GAMEPAD_BUTTON_BACK] = SA;
    l.lt_button = SZ;
    l.rt_button = SC;                                      /* RT on the title screen starts, as before */
    l.y_from_stick = bindings;                             /* races take steering from the bindings, not y */
    return l;
}

InputConfig g_cfg;
Layout g_layout;

/* Saturn button names used in [Pad ...] and [Joystick ...] sections */
DWORD sat_from_name(const char *v)
{
    static const struct { const char *n; DWORD b; } t[] = { { "A", SA }, { "B", SB }, { "C", SC }, { "X", SX }, { "Y", SY }, { "Z", SZ },
        { "L", SL }, { "R", SR }, { "Start", SSTART }, { "None", 0 } };
    for (auto &e : t) if (!lstrcmpiA(v, e.n)) return e.b;
    return 0xFFFFFFFF;
}

void section_name(char *out, size_t n, const char *kind, const char *dev)
{
    snprintf(out, n, "%s %s", kind, dev && *dev ? dev : "?");
    for (char *p = out; *p; p++) if (*p == '[' || *p == ']' || *p == '=' || *p == ';') *p = '_';
}

/* ---------- gamepads ---------- */
struct PadMap {
    SDL_JoystickID id;
    DWORD pad_to_sat[SDL_GAMEPAD_BUTTON_COUNT];
    DWORD lt_button, rt_button;
};
PadMap g_pads[8];

const char *const kPadKeys[] = { "South", "East", "West", "North", "Back", "Guide", "Start", "LeftStick", "RightStick",
                                 "LeftShoulder", "RightShoulder", "DpadUp", "DpadDown", "DpadLeft", "DpadRight" };

PadMap *pad_map(SDL_JoystickID id)
{
    for (auto &m : g_pads) if (m.id == id) return &m;
    return nullptr;
}

void pad_open(SDL_Gamepad *g, SDL_JoystickID id)
{
    PadMap *m = pad_map(0);
    if (!m) { plog("controller: more than 8 pads, ignoring the extra one"); return; }
    m->id = id;
    memcpy(m->pad_to_sat, g_layout.pad_to_sat, sizeof m->pad_to_sat);
    m->lt_button = g_layout.lt_button;
    m->rt_button = g_layout.rt_button;
    char sec[96], v[32];
    section_name(sec, sizeof sec, "Pad", SDL_GetGamepadName(g));
    int overrides = 0;
    for (int b = 0; b < (int)(sizeof kPadKeys / sizeof *kPadKeys); b++) {
        if (!GetPrivateProfileStringA(sec, kPadKeys[b], "", v, sizeof v, g_cfg.ini)) continue;
        DWORD sat = sat_from_name(v);
        if (sat == 0xFFFFFFFF) { plog("controller: [%s] %s=%s not understood (A B C X Y Z L R Start None)", sec, kPadKeys[b], v); continue; }
        m->pad_to_sat[b] = sat; overrides++;
    }
    const char *trig[2] = { "LeftTrigger", "RightTrigger" };
    for (int t = 0; t < 2; t++) {
        if (!GetPrivateProfileStringA(sec, trig[t], "", v, sizeof v, g_cfg.ini)) continue;
        DWORD sat = sat_from_name(v);
        if (sat == 0xFFFFFFFF) { plog("controller: [%s] %s=%s not understood (A B C X Y Z L R Start None)", sec, trig[t], v); continue; }
        if (t) m->rt_button = sat; else m->lt_button = sat;
        overrides++;
    }
    plog("controller: pad mapping section [%s]: %d override(s)", sec, overrides);
}

/* ---------- wheels, pedals and other joysticks ---------- */
constexpr int kMaxAxes = 8, kLearnMs = 400;
struct JoyMap {
    SDL_JoystickID id;
    SDL_Joystick *j;
    char sec[96];
    int naxes, nbuttons, nhats;
    int steer, accel, brake;          /* axis numbers, -1 none */
    bool auto_pedals;
    Sint16 rest[kMaxAxes];
    DWORD opened;
    bool learned;
    DWORD button_to_sat[32];
    int dead_zone;
};
JoyMap g_joys[4];

BYTE *g_base;
bool g_active;

/* state shared between the SDL thread and the game thread */
CRITICAL_SECTION g_cs;
/* Pad inputs a function can be bound to: SDL's gamepad buttons 0 to 14, then the triggers and the stick directions */
enum PadInput { P_SOUTH, P_EAST, P_WEST, P_NORTH, P_BACK, P_GUIDE, P_START, P_LSTICK, P_RSTICK, P_LB, P_RB,
                P_DUP, P_DDOWN, P_DLEFT, P_DRIGHT, P_LT, P_RT, P_LSL, P_LSR, P_LSU, P_LSD, P_RSL, P_RSR, P_RSU, P_RSD, kPadInputs };
enum Family { F_XBOX, F_PS, F_SWITCH };
struct State {
    bool connected; char name[64]; DWORD x, y, z, r, u, v, buttons, pov; int pads; bool active;
    int amt[kPadInputs];   /* how far each pad input is pressed, 0 to 32767 (all gamepads together) */
    DWORD held;            /* bit per pad input: pressed past its point (buttons, triggers at TriggerThreshold, sticks half way) */
    int family;            /* first gamepad: Xbox, PlayStation or Switch button names */
    DWORD wheel_sat;       /* Saturn buttons from wheels and other joysticks (pedals as C and B) */
    int wheel_x;           /* their steering, 0 to 65535, or -1 */
    DWORD dpad;            /* D-pads and hats: 1 left, 2 right */
};
State g_state;
HANDLE g_thread;
volatile LONG g_relayout;
volatile LONG g_stop;
volatile LONG g_started;

WORD *device_var() { return (WORD *)(g_base + (0x48ABB0 - 0x400000)); }
constexpr WORD kDevKeyboard = 8, kDevPcHandle = 0x20;
volatile LONG g_settings_loaded;

DWORD axis_from_stick(Sint16 v, int dead_zone)
{
    int d = 32767 * dead_zone / 100;
    int a = v;
    if (a > -d && a < d) a = 0;
    else if (a > 0) a = (int)((long long)(a - d) * 32767 / (32767 - d));
    else a = (int)((long long)(a + d) * 32768 / (32768 - d));
    long r = 32768 + a;
    if (r < 0) r = 0;
    if (r > 65535) r = 65535;
    return (DWORD)r;
}

/* ---------- test script: a virtual gamepad driven from a file ---------- */
struct ScriptStep { DWORD t; int control; int value; };
ScriptStep *g_script; int g_nscript;
SDL_JoystickID g_virtual;

int control_index(const char *s)
{
    static const char *const names[] = { "lx", "ly", "rx", "ry", "lt", "rt", "a", "b", "x", "y", "lb", "rb", "start", "back", "up", "down", "left", "right", "plug" };
    for (int i = 0; i < (int)(sizeof names / sizeof *names); i++) if (!strcmp(s, names[i])) return i;
    /* virtual wheel (a joystick, not a gamepad): wplug 0/1, wa0 to wa3 (axes), wb0 to wb9 (buttons) */
    if (!strcmp(s, "wplug")) return 100;
    if (s[0] == 'w' && s[1] == 'a' && s[2] >= '0' && s[2] <= '3' && !s[3]) return 110 + (s[2] - '0');
    if (s[0] == 'w' && s[1] == 'b' && s[2] >= '0' && s[2] <= '9' && !s[3]) return 120 + (s[2] - '0');
    return -1;
}

void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { plog("controller: test script %s not found", path); return; }
    char line[128];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned t; char c[16]; int v;
        if (line[0] == '#' || sscanf(line, "%u %15s %d", &t, c, &v) != 3) continue;
        int ci = control_index(c);
        if (ci < 0) continue;
        if (g_nscript == cap) { cap = cap ? cap * 2 : 64; g_script = (ScriptStep *)realloc(g_script, cap * sizeof *g_script); }
        g_script[g_nscript++] = { t, ci, v };
    }
    fclose(f);
    plog("controller: test script %s, %d steps", path, g_nscript);
}

void attach_virtual(SDL_Joystick *&vj);
SDL_Joystick *g_vwheel; SDL_JoystickID g_vwheel_id;
void wheel_script(const ScriptStep &st)
{
    if (st.control == 100) {
        if (!st.value && g_vwheel) { SDL_CloseJoystick(g_vwheel); g_vwheel = nullptr; SDL_DetachVirtualJoystick(g_vwheel_id); g_vwheel_id = 0; }
        else if (st.value && !g_vwheel) {
            SDL_VirtualJoystickDesc d;
            SDL_INIT_INTERFACE(&d);
            d.type = SDL_JOYSTICK_TYPE_WHEEL;
            d.naxes = 4; d.nbuttons = 10; d.nhats = 1;
            d.name = "Test script wheel";
            g_vwheel_id = SDL_AttachVirtualJoystick(&d);
            g_vwheel = g_vwheel_id ? SDL_OpenJoystick(g_vwheel_id) : nullptr;
            /* pedals at rest: up at the positive end */
            if (g_vwheel) for (int a = 1; a < 4; a++) SDL_SetJoystickVirtualAxis(g_vwheel, a, 32767);
            plog("controller: virtual test wheel %s", g_vwheel ? "attached" : SDL_GetError());
        }
        return;
    }
    if (!g_vwheel) return;
    if (st.control >= 120) SDL_SetJoystickVirtualButton(g_vwheel, st.control - 120, st.value != 0);
    else SDL_SetJoystickVirtualAxis(g_vwheel, st.control - 110, (Sint16)st.value);
}
void run_script(SDL_Joystick *&vj, DWORD elapsed, int &next)
{
    static const int axis_map[6] = { SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
                                     SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER };
    static const int button_map[12] = { SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
                                         SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_START,
                                         SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                         SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT };
    while (next < g_nscript && g_script[next].t <= elapsed) {
        const ScriptStep &s = g_script[next++];
        plog("controller: test script t=%lu control %d value %d", (unsigned long)s.t, s.control, s.value);
        if (s.control >= 100) { wheel_script(s); continue; }
        if (s.control == 18) {   /* plug 0 / plug 1: unplug or plug the virtual pad */
            if (!s.value && vj) { SDL_CloseJoystick(vj); vj = nullptr; SDL_DetachVirtualJoystick(g_virtual); g_virtual = 0; }
            else if (s.value && !vj) attach_virtual(vj);
            continue;
        }
        if (!vj) continue;
        /* triggers: the script gives 0 (released) to 32767 (full); the virtual device's raw axis runs from -32768 */
        if (s.control >= 4 && s.control < 6) SDL_SetJoystickVirtualAxis(vj, axis_map[s.control], (Sint16)(s.value * 2 - 32767 < -32768 ? -32768 : s.value * 2 - 32767));
        else if (s.control < 6) SDL_SetJoystickVirtualAxis(vj, axis_map[s.control], (Sint16)s.value);
        else SDL_SetJoystickVirtualButton(vj, button_map[s.control - 6], s.value != 0);
    }
}

void attach_virtual(SDL_Joystick *&vj)
{
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "Test script pad";
    g_virtual = SDL_AttachVirtualJoystick(&d);
    vj = g_virtual ? SDL_OpenJoystick(g_virtual) : nullptr;
    if (vj) {   /* triggers released */
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    }
    plog("controller: virtual test pad %s", vj ? "attached" : SDL_GetError());
}

/* ---------- the SDL thread ---------- */

JoyMap *joy_map(SDL_JoystickID id)
{
    for (auto &m : g_joys) if (m.id == id) return &m;
    return nullptr;
}

void joy_open(SDL_JoystickID id)
{
    JoyMap *m = joy_map(0);
    if (!m) { plog("controller: more than 4 joysticks, ignoring the extra one"); return; }
    SDL_Joystick *j = SDL_OpenJoystick(id);
    if (!j) { plog("controller: joystick open failed: %s", SDL_GetError()); return; }
    *m = JoyMap{};
    m->id = id; m->j = j;
    m->naxes = SDL_GetNumJoystickAxes(j); m->nbuttons = SDL_GetNumJoystickButtons(j); m->nhats = SDL_GetNumJoystickHats(j);
    section_name(m->sec, sizeof m->sec, "Joystick", SDL_GetJoystickName(j));
    static const char *const types[] = { "unknown", "gamepad", "wheel", "arcade stick", "flight stick", "dance pad", "guitar", "drum kit", "arcade pad", "throttle" };
    int t = SDL_GetJoystickType(j);
    plog("controller: connected joystick '%s' (%s, %d axes, %d buttons, %d hats), settings section [%s]",
         SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : "?", t >= 0 && t < 10 ? types[t] : "?", m->naxes, m->nbuttons, m->nhats, m->sec);
    /* the game's own reading of a Windows joystick: button n -> its button n+1 (A B C X Y Z R L Start) */
    for (int b = 0; b < 32; b++) m->button_to_sat[b] = b < 9 ? 1u << b : 0;
    char key[16], v[32];
    int overrides = 0;
    for (int b = 0; b < 32; b++) {
        snprintf(key, sizeof key, "Button%d", b);
        if (!GetPrivateProfileStringA(m->sec, key, "", v, sizeof v, g_cfg.ini)) continue;
        DWORD sat = sat_from_name(v);
        if (sat == 0xFFFFFFFF) { plog("controller: [%s] %s=%s not understood (A B C X Y Z L R Start None)", m->sec, key, v); continue; }
        m->button_to_sat[b] = sat; overrides++;
    }
    m->steer = GetPrivateProfileIntA(m->sec, "Steering", 0, g_cfg.ini);
    if (m->steer >= m->naxes) m->steer = -1;
    m->accel = GetPrivateProfileIntA(m->sec, "Accelerator", -2, g_cfg.ini);
    m->brake = GetPrivateProfileIntA(m->sec, "Brake", -2, g_cfg.ini);
    m->auto_pedals = m->accel == -2 && m->brake == -2;
    if (m->accel >= m->naxes || m->accel < -1) m->accel = -1;
    if (m->brake >= m->naxes || m->brake < -1) m->brake = -1;
    m->dead_zone = GetPrivateProfileIntA(m->sec, "DeadZone", 2, g_cfg.ini);
    m->opened = GetTickCount();
    plog("controller: [%s] steering axis %d, pedals %s, %d button override(s)", m->sec, m->steer,
         m->auto_pedals ? "found automatically" : "from settings", overrides);
}

/* Pedals rest at one end of their travel and a steering axis or stick at the centre. Shortly after connecting, every
   axis's resting value is taken; with no Accelerator / Brake set, the first two axes (other than steering) resting
   at an end are the accelerator and the brake. */
void joy_learn(JoyMap &m)
{
    if (m.learned || GetTickCount() - m.opened < kLearnMs) return;
    m.learned = true;
    char rests[160] = ""; size_t n = 0;
    for (int a = 0; a < m.naxes && a < kMaxAxes; a++) {
        m.rest[a] = SDL_GetJoystickAxis(m.j, a);
        n += snprintf(rests + n, sizeof rests - n, " %d", m.rest[a]);
        if (n >= sizeof rests) n = sizeof rests - 1;
    }
    if (m.auto_pedals) {
        m.accel = m.brake = -1;
        for (int a = 0; a < m.naxes && a < kMaxAxes; a++) {
            if (a == m.steer || abs(m.rest[a]) < 24000) continue;
            if (m.accel < 0) m.accel = a; else if (m.brake < 0) m.brake = a;
        }
    }   /* set by hand: the resting value still gives each pedal's direction */
    plog("controller: [%s] resting axis values:%s; accelerator axis %d, brake axis %d", m.sec, rests, m.accel, m.brake);
}

/* how far a pedal is pressed, 0 to 32767, from its resting end */
int pedal(const JoyMap &m, int axis)
{
    if (axis < 0 || axis >= kMaxAxes) return 0;
    int v = SDL_GetJoystickAxis(m.j, axis), r = m.rest[axis];
    int end = r < 0 ? -32768 : 32767;
    int travel = abs(r - end) < 16384 ? 65535 : abs(r - end);   /* rest near an end: full travel to the other end */
    int p = (int)((long long)abs(v - r) * 32767 / travel);
    return p > 32767 ? 32767 : p;
}

/* a stick axis past the dead zone, scaled to -32768 .. 32767 */
int dead_zoned(int a, int dead_zone)
{
    return (int)axis_from_stick((Sint16)a, dead_zone) - 32768;
}

int family_of(SDL_Gamepad *g)
{
    switch (SDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return F_PS;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return F_SWITCH;
    default: return F_XBOX;
    }
}

void read_pads(State &st)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    State s{};
    s.wheel_x = -1;
    s.x = s.y = s.z = s.r = s.u = s.v = 32768;
    s.pov = JOY_POVCENTERED;
    int lx = 0, ly = 0;
    bool dl = false, dr = false, du = false, dd = false;
    int dz = 32767 * g_cfg.dead_zone / 100, thr = 32767 * g_cfg.trigger_threshold / 100;
    for (int i = 0; i < n; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        PadMap *m = pad_map(ids[i]);
        if (!g || !m) continue;
        if (!s.connected) {
            snprintf(s.name, sizeof s.name, "%s", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "Gamepad");
            s.family = family_of(g);
        }
        s.connected = true;
        s.pads++;
        int ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        if (abs(ax) > abs(lx)) lx = ax;
        if (abs(ay) > abs(ly)) ly = ay;
        int rt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), lt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        if (m->rt_button && rt > thr) s.buttons |= m->rt_button;
        if (m->lt_button && lt > thr) s.buttons |= m->lt_button;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) {
                s.buttons |= m->pad_to_sat[b];
                s.active = true;
            }
        if (abs(ax) > dz || abs(ay) > dz || rt > thr || lt > thr) s.active = true;
        /* every input's amount, for the race bindings and the CONTROLS page */
        int a[kPadInputs] = {};
        for (int b = 0; b <= P_DRIGHT; b++) a[b] = SDL_GetGamepadButton(g, (SDL_GamepadButton)b) ? 32767 : 0;
        a[P_LT] = lt > 0 ? lt : 0; a[P_RT] = rt > 0 ? rt : 0;
        int sx = dead_zoned(ax, g_cfg.dead_zone), sy = dead_zoned(ay, g_cfg.dead_zone);
        int rx = dead_zoned(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX), g_cfg.dead_zone);
        int ry = dead_zoned(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY), g_cfg.dead_zone);
        a[P_LSL] = sx < 0 ? -sx : 0; a[P_LSR] = sx > 0 ? sx : 0; a[P_LSU] = sy < 0 ? -sy : 0; a[P_LSD] = sy > 0 ? sy : 0;
        a[P_RSL] = rx < 0 ? -rx : 0; a[P_RSR] = rx > 0 ? rx : 0; a[P_RSU] = ry < 0 ? -ry : 0; a[P_RSD] = ry > 0 ? ry : 0;
        for (int k = 0; k < kPadInputs; k++) {
            if (a[k] > 32767) a[k] = 32767;
            if (a[k] > s.amt[k]) s.amt[k] = a[k];
        }
        if (abs(rx) > 16384 || abs(ry) > 16384) s.active = true;
        dl |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        dr |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        du |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP);
        dd |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    }
    SDL_free(ids);
    for (int k = 0; k < kPadInputs; k++) {
        int point = k == P_LT || k == P_RT ? thr : k >= P_LSL ? 16384 : 0;
        if (s.amt[k] > point) s.held |= 1u << k;
    }
    s.x = axis_from_stick((Sint16)lx, g_cfg.dead_zone);

    /* wheels and other joysticks */
    int wsteer = 0; bool wheel_steering = false;
    for (auto &m : g_joys) {
        if (!m.j) continue;
        if (!s.connected) snprintf(s.name, sizeof s.name, "%s", SDL_GetJoystickName(m.j) ? SDL_GetJoystickName(m.j) : "Joystick");
        s.connected = true;
        s.pads++;
        joy_learn(m);
        if (!m.learned) continue;
        if (m.steer >= 0) {
            int v = SDL_GetJoystickAxis(m.j, m.steer);
            int wdz = 32767 * m.dead_zone / 100;
            if (abs(v) > abs(wsteer)) { wsteer = v; wheel_steering = true; }
            if (abs(v - m.rest[m.steer < kMaxAxes ? m.steer : 0]) > 32767 * 15 / 100 && abs(v) > wdz) s.active = true;
        }
        int pa = pedal(m, m.accel), pb = pedal(m, m.brake);
        if (pa > thr || pb > thr) s.active = true;
        if (pa > thr) { s.buttons |= SC; s.wheel_sat |= SC; }
        if (pb > thr) { s.buttons |= SB; s.wheel_sat |= SB; }
        for (int b = 0; b < m.nbuttons && b < 32; b++)
            if (SDL_GetJoystickButton(m.j, b)) { s.buttons |= m.button_to_sat[b]; s.wheel_sat |= m.button_to_sat[b]; s.active = true; }
        for (int h = 0; h < m.nhats; h++) {
            Uint8 hv = SDL_GetJoystickHat(m.j, h);
            dl |= (hv & SDL_HAT_LEFT) != 0; dr |= (hv & SDL_HAT_RIGHT) != 0;
            du |= (hv & SDL_HAT_UP) != 0; dd |= (hv & SDL_HAT_DOWN) != 0;
        }
    }
    if (wheel_steering && abs(wsteer) > abs(lx)) {
        int wdz = 0;
        for (auto &m : g_joys) if (m.j) { wdz = m.dead_zone; break; }
        s.x = axis_from_stick((Sint16)wsteer, wdz);
    }
    if (wheel_steering) {
        int wdz = 0;
        for (auto &m : g_joys) if (m.j) { wdz = m.dead_zone; break; }
        s.wheel_x = (int)axis_from_stick((Sint16)wsteer, wdz);
    }
    if (dl || dr || du || dd) s.active = true;

    /* The initials entry (screen mode 27) sets the letter from the steering position like a wheel; there the D-pad
       steps one letter at a time instead (main_menu.cpp), so it must not move the steering. */
    bool initials = *(int *)(g_base + (0x482230 - 0x400000)) == 27;
    s.dpad = (dl ? 1u : 0) | (dr ? 2u : 0);
    if (dl && !dr && !initials) s.x = 0;
    if (dr && !dl && !initials) s.x = 65535;
    if (g_layout.y_from_stick) s.y = axis_from_stick((Sint16)ly, g_cfg.dead_zone);
    if (du && !dd) s.y = 0;
    if (dd && !du) s.y = 65535;
    if (du) s.pov = JOY_POVFORWARD; else if (dd) s.pov = JOY_POVBACKWARD; else if (dl) s.pov = JOY_POVLEFT; else if (dr) s.pov = JOY_POVRIGHT;
    st = s;
}

/* ---------- the bindings (0.0.18) ---------- */

constexpr int kFns = INPUT_FUNCTIONS;
/* page order, as jayyanz's mock-ups (Mockups\controls) */
enum Fn { STEER_L, STEER_R, ACCEL, BRAKE, SHIFT_UP, SHIFT_DN, VIEW_CHANGE, VIEW1, VIEW2, VIEW3, VIEW4, GEAR1, GEAR2, GEAR3, GEAR4, START };
const char *const kFnLabels[kFns] = { "STEER LEFT", "STEER RIGHT", "ACCELERATE", "BRAKE", "SHIFT UP", "SHIFT DOWN", "VIEW CHANGE",
                                      "VIEW 1", "VIEW 2", "VIEW 3", "VIEW 4", "GEAR 1", "GEAR 2", "GEAR 3", "GEAR 4", "START" };
const char *const kFnKeys[kFns] = { "SteerLeft", "SteerRight", "Accelerate", "Brake", "ShiftUp", "ShiftDown", "ViewChange",
                                    "View1", "View2", "View3", "View4", "Gear1", "Gear2", "Gear3", "Gear4", "Start" };
/* each function's bit in the race pad word; steering uses the game's own LEFT / RIGHT bits */
const WORD kFnBits[kFns] = { 0x4000, 0x8000, 0x0200, 0x0100, 0x2000, 0x1000, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008,
                             0x0400, 0x0040, 0x0020, 0x0010, 0x0800 };
/* The function masks for every row of the game's table (words 1 to 14 of each 15-word row at 0x488AF0), and where
   0x431EC0 copies each. The race tests "(pad word & mask) != 0" with the mask read as a signed word, and the SHIFT
   tests also want it above 0, so no mask uses 0x8000. VIEW DOWN (word 7) is 0: never pressed. */
const WORD kRowWords[14] = { 0x0200, 0x0100, 0x0400, 0x0040, 0x0020, 0x0010, 0x0000, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008, 0x1000, 0x2000 };
const DWORD kLiveMasks[14] = { 0x11B7268, 0x11B727C, 0x11BDB20, 0x11BDB22, 0x11B7244, 0x11B7AA4, 0xC583AC, 0xC583B2,
                               0xC583BA, 0xC583B4, 0xC583B6, 0xC583B8, 0x11B7184, 0x11B7A6E };

const char *const kPadIni[kPadInputs] = { "South", "East", "West", "North", "Back", "Guide", "Start", "LeftStick", "RightStick",
    "LeftShoulder", "RightShoulder", "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "LeftTrigger", "RightTrigger",
    "LeftStickLeft", "LeftStickRight", "LeftStickUp", "LeftStickDown", "RightStickLeft", "RightStickRight", "RightStickUp", "RightStickDown" };
/* names on screen, in the font (capitals, digits, spaces), by the family of the pad plugged in */
const char *const kPadShown[3][kPadInputs] = {
    { "A", "B", "X", "Y", "VIEW", "GUIDE", "MENU", "LS", "RS", "LB", "RB", "DPAD UP", "DPAD DOWN", "DPAD LEFT", "DPAD RIGHT", "LT", "RT",
      "LSTICK LEFT", "LSTICK RIGHT", "LSTICK UP", "LSTICK DOWN", "RSTICK LEFT", "RSTICK RIGHT", "RSTICK UP", "RSTICK DOWN" },
    { "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "SELECT", "PS", "OPTIONS", "L3", "R3", "L1", "R1", "DPAD UP", "DPAD DOWN", "DPAD LEFT", "DPAD RIGHT", "L2", "R2",
      "LSTICK LEFT", "LSTICK RIGHT", "LSTICK UP", "LSTICK DOWN", "RSTICK LEFT", "RSTICK RIGHT", "RSTICK UP", "RSTICK DOWN" },
    { "B", "A", "Y", "X", "MINUS", "HOME", "PLUS", "LS", "RS", "L", "R", "DPAD UP", "DPAD DOWN", "DPAD LEFT", "DPAD RIGHT", "ZL", "ZR",
      "LSTICK LEFT", "LSTICK RIGHT", "LSTICK UP", "LSTICK DOWN", "RSTICK LEFT", "RSTICK RIGHT", "RSTICK UP", "RSTICK DOWN" },
};

/* keys: Windows virtual-key code and name (on screen and in the settings file) */
const struct { int vk; const char *name; } kKeys[] = {
    { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" }, { VK_UP, "UP" }, { VK_DOWN, "DOWN" }, { VK_RETURN, "ENTER" }, { VK_SPACE, "SPACE" },
    { VK_TAB, "TAB" }, { VK_BACK, "BACKSPACE" }, { VK_DELETE, "DELETE" }, { VK_INSERT, "INSERT" }, { VK_HOME, "HOME" }, { VK_END, "END" },
    { VK_PRIOR, "PAGE UP" }, { VK_NEXT, "PAGE DOWN" }, { VK_LSHIFT, "LSHIFT" }, { VK_RSHIFT, "RSHIFT" }, { VK_LCONTROL, "LCTRL" },
    { VK_RCONTROL, "RCTRL" }, { VK_LMENU, "LALT" }, { VK_RMENU, "RALT" }, { VK_CAPITAL, "CAPS LOCK" },
    { VK_NUMPAD0, "NUM 0" }, { VK_NUMPAD1, "NUM 1" }, { VK_NUMPAD2, "NUM 2" }, { VK_NUMPAD3, "NUM 3" }, { VK_NUMPAD4, "NUM 4" },
    { VK_NUMPAD5, "NUM 5" }, { VK_NUMPAD6, "NUM 6" }, { VK_NUMPAD7, "NUM 7" }, { VK_NUMPAD8, "NUM 8" }, { VK_NUMPAD9, "NUM 9" },
    { VK_MULTIPLY, "NUM MUL" }, { VK_ADD, "NUM ADD" }, { VK_SUBTRACT, "NUM SUB" }, { VK_DECIMAL, "NUM DOT" }, { VK_DIVIDE, "NUM DIV" },
    { VK_OEM_1, "SEMICOLON" }, { VK_OEM_PLUS, "EQUALS" }, { VK_OEM_COMMA, "COMMA" }, { VK_OEM_MINUS, "MINUS" }, { VK_OEM_PERIOD, "PERIOD" },
    { VK_OEM_2, "SLASH" }, { VK_OEM_3, "GRAVE" }, { VK_OEM_4, "LBRACKET" }, { VK_OEM_5, "BACKSLASH" }, { VK_OEM_6, "RBRACKET" },
    { VK_OEM_7, "QUOTE" },
};

void key_name(int vk, char *out, size_t n)
{
    if (vk <= 0) { snprintf(out, n, "NONE"); return; }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { snprintf(out, n, "%c", vk); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(out, n, "F%d", vk - VK_F1 + 1); return; }
    for (auto &k : kKeys) if (k.vk == vk) { snprintf(out, n, "%s", k.name); return; }
    snprintf(out, n, "KEY %d", vk);
}

int key_from_name(const char *s)
{
    if (!lstrcmpiA(s, "NONE") || !*s) return 0;
    for (int vk = 1; vk < 256; vk++) { char b[16]; key_name(vk, b, sizeof b); if (!lstrcmpiA(b, s)) return vk; }
    if (!strncmp(s, "KEY ", 4)) { int v = atoi(s + 4); if (v > 0 && v < 256) return v; }
    return -1;
}

int pad_from_name(const char *s)
{
    if (!lstrcmpiA(s, "None")) return -1;
    for (int i = 0; i < kPadInputs; i++) if (!lstrcmpiA(s, kPadIni[i])) return i;
    return -2;
}

struct Bindings { int key[kFns]; int pad[kFns]; };   /* key: virtual-key code, 0 none; pad: PadInput, -1 none */
Bindings g_bind;
bool g_bind_on;           /* Layout is not Off */

Bindings preset(PadLayout l)
{
    Bindings b;
    const int keys[kFns] = { VK_LEFT, VK_RIGHT, 'X', 'Z', VK_UP, VK_DOWN, 'V', VK_F5, VK_F6, VK_F7, VK_F8, '1', '2', '3', '4', VK_RETURN };
    memcpy(b.key, keys, sizeof keys);
    for (int f = 0; f < kFns; f++) b.pad[f] = -1;
    b.pad[STEER_L] = P_LSL; b.pad[STEER_R] = P_LSR; b.pad[VIEW_CHANGE] = P_RB; b.pad[START] = P_START;
    b.pad[SHIFT_UP] = P_NORTH;
    if (l == PadLayout::PS1) {   /* CLASSIC: as the PlayStation version, cross accelerates, square brakes */
        b.pad[ACCEL] = P_SOUTH; b.pad[BRAKE] = P_WEST; b.pad[SHIFT_DN] = P_EAST;
    } else {                     /* MODERN: the triggers */
        b.pad[ACCEL] = P_RT; b.pad[BRAKE] = P_LT; b.pad[SHIFT_DN] = P_WEST;
    }
    return b;
}

void save_bindings()
{
    char k[32], v[24];
    for (int f = 0; f < kFns; f++) {
        snprintf(k, sizeof k, "%sKey", kFnKeys[f]); key_name(g_bind.key[f], v, sizeof v);
        WritePrivateProfileStringA("Controls", k, v, g_cfg.ini);
        snprintf(k, sizeof k, "%sPad", kFnKeys[f]);
        WritePrivateProfileStringA("Controls", k, g_bind.pad[f] >= 0 ? kPadIni[g_bind.pad[f]] : "None", g_cfg.ini);
    }
}

void load_bindings()
{
    g_bind = preset(g_cfg.layout);
    char k[32], v[32];
    int read = 0;
    for (int f = 0; f < kFns; f++) {
        snprintf(k, sizeof k, "%sKey", kFnKeys[f]);
        if (GetPrivateProfileStringA("Controls", k, "", v, sizeof v, g_cfg.ini)) {
            int vk = key_from_name(v);
            if (vk < 0) plog("controls: [Controls] %s=%s not understood, kept the default", k, v); else { g_bind.key[f] = vk; read++; }
        }
        snprintf(k, sizeof k, "%sPad", kFnKeys[f]);
        if (GetPrivateProfileStringA("Controls", k, "", v, sizeof v, g_cfg.ini)) {
            int p = pad_from_name(v);
            if (p < -1) plog("controls: [Controls] %s=%s not understood, kept the default", k, v); else { g_bind.pad[f] = p; read++; }
        }
    }
    plog("controls: %d binding(s) from [Controls], the rest from the %s layout", read, g_cfg.layout == PadLayout::PS1 ? "CLASSIC" : "MODERN");
}

int mode() { return *(int *)(g_base + (0x482230 - 0x400000)); }
bool race_mode() { int m = mode(); return m == 15 || m == 17 || m == 21 || m == 23; }   /* countdown, race and their kin, not paused */

bool game_has_focus()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    return fg && (GetWindowThreadProcessId(fg, &pid), pid == GetCurrentProcessId());
}

/* every row of the game's function table, and the masks in force */
void apply_rows()
{
    WORD *t = (WORD *)(g_base + (0x488AF0 - 0x400000));
    for (int r = 0; r < 10; r++) for (int i = 0; i < 14; i++) t[r * 15 + 1 + i] = kRowWords[i];
    for (int i = 0; i < 14; i++) *(WORD *)(g_base + (kLiveMasks[i] - 0x400000)) = kRowWords[i];
}

void ensure_masks()
{
    for (int i = 0; i < 14; i++) {
        WORD *m = (WORD *)(g_base + (kLiveMasks[i] - 0x400000));
        if (*m != kRowWords[i]) { *m = kRowWords[i]; }
    }
}

State current_state();

/* wheels and joysticks in races: the game's Modern assignment */
WORD wheel_bits(DWORD sat)
{
    WORD w = 0;
    if (sat & SC) w |= kFnBits[ACCEL];
    if (sat & (SB | SZ)) w |= kFnBits[BRAKE];
    if (sat & SX) w |= kFnBits[SHIFT_DN];
    if (sat & SY) w |= kFnBits[SHIFT_UP];
    if (sat & SR) w |= kFnBits[VIEW_CHANGE];
    if (sat & SSTART) w |= kFnBits[START];
    return w;
}

/* GEAR 1 to 4. The game's gearbox (0x41B5BC, run only while word 0x11B7A9C is 0: manual cars) has two modes chosen by the SHIFT
   DOWN mask: above 0, SHIFT UP / DOWN step the gear; otherwise GEAR 1 to 4 pick it, and the steps do nothing. Every PC
   row used the steps, so GEAR 1 to 4 never worked there. Both are on the page (jayyanz), so the steps stay with the
   game and a GEAR press sets the gear itself, in the word the game's own GEAR code writes (car + 0xDE, 0 to 3). */
void direct_gears(WORD bits)
{
    static WORD prev;
    const WORD all = kFnBits[GEAR1] | kFnBits[GEAR2] | kFnBits[GEAR3] | kFnBits[GEAR4];
    WORD edge = bits & all & ~prev;
    prev = bits & all;
    if (!edge || *(short *)(g_base + (0x11B7A9C - 0x400000))) return;   /* automatic */
    BYTE *car = *(BYTE **)(g_base + (0x11B7A5C - 0x400000));
    if (!car || IsBadWritePtr(car + 0xDE, 2)) return;
    for (int g = 0; g < 4; g++)
        if (edge & kFnBits[GEAR1 + g]) { *(short *)(car + 0xDE) = (short)g; return; }
}

/* VIEW CHANGE wraps round (jayyanz, 1 Nov 2026). The game's view code (0x449B10) steps the view (0x4D5154, 0 to 3)
   up on a VIEW UP press only while it is below the limit at 0xC583B0, so it stopped at the last view. A press made at
   the last view is given to the game as VIEW 1 instead, for as long as it is held (the game acts on new presses). */
void view_wrap(WORD &bits)
{
    static bool prev, latched;
    bool now = (bits & kFnBits[VIEW_CHANGE]) != 0;
    if (now && !prev) {
        int view = *(int *)(g_base + (0x4D5154 - 0x400000));
        int last = *(signed char *)(g_base + (0xC583B0 - 0x400000));
        latched = view >= last;
    }
    prev = now;
    if (now && latched) bits = (WORD)((bits & ~kFnBits[VIEW_CHANGE]) | kFnBits[VIEW1]);
}

/* The game's joystick reader (0x445D10, cdecl): returns the pad word, writes the steering byte (0 right .. 255 left)
   and the up / down byte. In races, both rebuilt from the bindings. */
using pad_reader_t = DWORD (__cdecl *)(WORD *steer, WORD *updown);
using key_reader_t = DWORD (__cdecl *)();
pad_reader_t g_game_pad;
key_reader_t g_game_keys;

DWORD __cdecl pad_reader(WORD *steer, WORD *updown)
{
    DWORD w = g_game_pad(steer, updown);
    if (!g_bind_on || !race_mode()) return w;
    ensure_masks();
    State s = current_state();
    WORD bits = 0;
    int left = 0, right = 0;
    for (int f = 0; f < kFns; f++) {
        int in = g_bind.pad[f];
        if (in < 0) continue;
        if (f == STEER_L) left = s.amt[in];
        else if (f == STEER_R) right = s.amt[in];
        else if (s.held & (1u << in)) bits |= kFnBits[f];
    }
    bits |= wheel_bits(s.wheel_sat);
    int x = 32768 + right - left;
    if (s.wheel_x >= 0 && abs(s.wheel_x - 32768) > abs(x - 32768)) x = s.wheel_x;
    if (x < 0) x = 0;
    if (x > 65535) x = 65535;
    if (steer) *steer = (WORD)(~(x >> 8) & 0xFF);
    if (updown) *updown = 0x80;
    if (x > 0xC000) bits |= 0x8000; else if (x < 0x4000) bits |= 0x4000;
    direct_gears(bits);
    view_wrap(bits);
    return (w & 0xFFFF0000) | bits;
}

/* The game's keyboard reader (0x445B50): in races the pad word from the bound keys. */
DWORD __cdecl key_reader()
{
    DWORD w = g_game_keys();
    if (!g_bind_on || !race_mode()) return w;
    ensure_masks();
    if (!game_has_focus()) return w & 0xFFFF0000;
    WORD bits = 0;
    for (int f = 0; f < kFns; f++) {
        int vk = g_bind.key[f];
        if (vk > 0 && (GetAsyncKeyState(vk) & 0x8000)) bits |= kFnBits[f];
    }
    if ((bits & 0xC000) == 0xC000) bits &= ~0xC000;   /* both steering keys: straight on */
    direct_gears(bits);
    view_wrap(bits);
    return (w & 0xFFFF0000) | bits;
}

/* ---------- waiting for a key or button on the CONTROLS page ---------- */
struct Capture { bool on, pad, released; int fn; DWORD start; } g_cap;
constexpr DWORD kCaptureMs = 5000;

int first_key_down()
{
    for (int vk = 8; vk < 256; vk++) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_ESCAPE) continue;   /* left / right kinds instead */
        if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS) continue;
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    return 0;
}

void bind(int f, bool pad, int input)
{
    char was[24], now[24];
    for (int g = 0; g < kFns; g++) {   /* one function per key or button: the old one is left unbound */
        if (g == f || input <= (pad ? -1 : 0)) continue;
        if (pad ? g_bind.pad[g] == input : g_bind.key[g] == input) {
            if (pad) g_bind.pad[g] = -1; else g_bind.key[g] = 0;
            plog("controls: %s %s unbound (now used by %s)", kFnLabels[g], pad ? "pad" : "key", kFnLabels[f]);
        }
    }
    if (pad) {
        snprintf(was, sizeof was, "%s", g_bind.pad[f] >= 0 ? kPadIni[g_bind.pad[f]] : "None");
        g_bind.pad[f] = input;
        snprintf(now, sizeof now, "%s", input >= 0 ? kPadIni[input] : "None");
    } else {
        key_name(g_bind.key[f], was, sizeof was);
        g_bind.key[f] = input > 0 ? input : 0;
        key_name(g_bind.key[f], now, sizeof now);
    }
    plog("controls: %s %s %s -> %s", kFnLabels[f], pad ? "pad" : "key", was, now);
    save_bindings();
}

/* Any of the game's own keys (its 10 driving keys and Start key, WORDs from 0xC58430) or a bound key held while
   the game has the focus. Only the "held" bit is read, so the game's own key reading is not disturbed. */
bool keyboard_active()
{
    if (!game_has_focus()) return false;
    const WORD *keys = (const WORD *)(g_base + (0xC58430 - 0x400000));
    for (int i = 0; i < 11; i++)
        if (keys[i] && keys[i] < 256 && (GetAsyncKeyState(keys[i]) & 0x8000)) return true;
    if (g_bind_on) for (int f = 0; f < kFns; f++)
        if (g_bind.key[f] > 0 && (GetAsyncKeyState(g_bind.key[f]) & 0x8000)) return true;
    return false;
}

bool g_use_pad;   /* last input: pad (true) or keyboard */

/* The game keeps a "control type" (0xC58450); the function row in force is type + 5 x analogue: 0 for a joystick,
   1 for the keyboard, 2 to 4 the Saturn types of the game's own Key Configuration page (no longer shown). Its Key /
   Button Assign dialogs set 1 for the keyboard and 0 for a joystick and reload the masks; changing the device alone
   did not (0.0.11 bug). So the type is kept in step, loaded through the game's own 0x431EC0(type, analogue). Since
   0.0.18 every row holds the same masks (apply_rows), so this only keeps the game's own state tidy. */
void sync_control_type(bool pad)
{
    BYTE *type = g_base + (0xC58450 - 0x400000);
    BYTE want = pad ? 0 : 1;
    if (*type == want) return;
    *type = want;
    using load_row_t = void (__cdecl *)(int, int);
    load_row_t load_row = (load_row_t)(g_base + (0x431EC0 - 0x400000));
    load_row(want, pad ? 1 : 0);    /* PC Handle reads as analogue (flag 1), the keyboard as digital */
    plog("controller: game control type set to %u (%s)", want, pad ? "pad" : "keyboard");
}

/* The player car's steering mode, word +0x9E of the car at *(0x11B7A5C): 1 = analogue (reads the steering byte
   0xC583E4), 0 = digital (the LEFT / RIGHT bits). The game sets it when a race starts, from the device in use then,
   and reads it every tick (0x41BCD5 steering, 0x424B4F handling). Switching device mid-race left it as it was: a
   race started with the pad kept reading the analogue byte after the keyboard took over, which the keyboard path
   never fills, so the arrow keys did nothing (jayyanz, 1 Nov 2026). Kept in step with the device during races. */
void sync_car_steering(bool pad)
{
    int mode = *(int *)(g_base + (0x482230 - 0x400000));
    if (mode < 15 || mode > 23 || !(mode & 1)) return;           /* race screens only (not the attract demo, 3) */
    BYTE *car = *(BYTE **)(g_base + (0x11B7A5C - 0x400000));
    if (!car || IsBadWritePtr(car + 0x9E, 2)) return;
    volatile short *steer = (volatile short *)(car + 0x9E);
    short want = pad ? 1 : 0;
    if (*steer == want || (*steer != 0 && *steer != 1)) return;
    *steer = want;
    plog("controller: car steering set to %s for the %s (screen mode %d)", pad ? "analogue" : "digital", pad ? "pad" : "keyboard", mode);
}

void set_device(const State &s)
{
    if (!g_settings_loaded || g_cfg.layout == PadLayout::Off) return;
    bool pad;
    if (!s.connected) pad = false;
    else if (!g_cfg.keyboard_with_pad) pad = true;
    else if (s.active) pad = true;
    else if (keyboard_active()) pad = false;
    else pad = g_use_pad;
    g_use_pad = pad;
    WORD want = pad ? kDevPcHandle : kDevKeyboard;
    if (*device_var() != want) {
        *device_var() = want;
        plog("controller: game input device set to %s", pad ? "joystick (PC Handle)" : "keyboard");
    }
    sync_control_type(pad);
    sync_car_steering(pad);
}

DWORD WINAPI sdl_thread(LPVOID)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        plog("controller: SDL_Init failed: %s", SDL_GetError());
        return 0;
    }
    plog("controller: SDL %d.%d.%d ready, keyboard with a pad %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
         g_cfg.keyboard_with_pad ? "on (last input used wins)" : "off");
    SDL_Joystick *vj = nullptr;
    int next = 0;
    DWORD t0 = GetTickCount();
    if (g_nscript) attach_virtual(vj);
    bool was = false;
    char was_name[64] = "";
    int was_pads = 0;
    while (!g_stop) {
        SDL_Event e;
        SDL_PumpEvents();
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which);
                if (g) {
                    plog("controller: connected '%s' (%s)", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "?",
                         SDL_GetGamepadStringForType(SDL_GetGamepadType(g)) ? SDL_GetGamepadStringForType(SDL_GetGamepadType(g)) : "?");
                    pad_open(g, e.gdevice.which);
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                SDL_Gamepad *g = SDL_GetGamepadFromID(e.gdevice.which);
                plog("controller: disconnected");
                if (PadMap *m = pad_map(e.gdevice.which)) m->id = 0;
                if (g) SDL_CloseGamepad(g);
            } else if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
                if (!SDL_IsGamepad(e.jdevice.which)) joy_open(e.jdevice.which);
            } else if (e.type == SDL_EVENT_JOYSTICK_REMOVED) {
                if (JoyMap *m = joy_map(e.jdevice.which)) {
                    plog("controller: joystick [%s] disconnected", m->sec);
                    SDL_CloseJoystick(m->j);
                    *m = JoyMap{};
                }
            }
        }
        if (InterlockedExchange(&g_relayout, 0)) {   /* layout changed: rebuild every pad's mapping */
            for (auto &m : g_pads) m.id = 0;
            int n = 0;
            SDL_JoystickID *ids = SDL_GetGamepads(&n);
            for (int i = 0; i < n; i++) if (SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i])) pad_open(g, ids[i]);
            SDL_free(ids);
        }
        if (g_nscript) run_script(vj, GetTickCount() - t0, next);
        State s;
        read_pads(s);
        EnterCriticalSection(&g_cs);
        g_state = s;
        LeaveCriticalSection(&g_cs);
        if (s.connected != was || s.pads != was_pads || strcmp(s.name, was_name)) {
            was = s.connected; was_pads = s.pads; strcpy(was_name, s.name);
            plog("controller: %d controller(s) driving joystick 1%s%s", s.pads, s.pads ? ", first: " : "", s.name);
        }
        set_device(s);
        Sleep(2);
    }
    if (vj) SDL_CloseJoystick(vj);
    if (g_vwheel) SDL_CloseJoystick(g_vwheel);
    SDL_Quit();
    return 0;
}

State current_state()
{
    if (!g_active) { State s{}; s.wheel_x = -1; return s; }
    EnterCriticalSection(&g_cs);
    State s = g_state;
    LeaveCriticalSection(&g_cs);
    return s;
}

bool patch_call(DWORD site, DWORD target, void *fn)
{
    BYTE *p = g_base + (site - 0x400000);
    if (p[0] != 0xE8 || (DWORD)(site + 5 + *(LONG *)(p + 1)) != target) return false;
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(LONG *)(p + 1) = (LONG)((BYTE *)fn - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

void start()
{
    if (InterlockedExchange(&g_started, 1)) return;
    g_thread = CreateThread(nullptr, 0, sdl_thread, nullptr, 0, nullptr);
}

} // namespace

void input_init(const InputConfig &cfg, BYTE *game_base)
{
    g_cfg = cfg;
    g_base = game_base;
    g_bind_on = cfg.layout != PadLayout::Off;
    g_layout = make_menu_map(g_bind_on);
    if (g_bind_on) load_bindings();
    g_active = cfg.enabled;
    if (!g_active) return;
    InitializeCriticalSection(&g_cs);
    if (cfg.test_script[0]) load_script(cfg.test_script);
    /* The SDL thread starts on the game's first joystick call or after its settings are loaded,
       never from DllMain (Windows does not allow starting threads that load DLLs while DLLs are loading). */
}

/* the race readers: the calls from the game's input routine 0x446030 */
void input_install_bindings()
{
    if (!g_bind_on) { plog("controls: Layout=Off, the game's own button assignment is used"); return; }
    g_game_pad = (pad_reader_t)(g_base + (0x445D10 - 0x400000));
    g_game_keys = (key_reader_t)(g_base + (0x445B50 - 0x400000));
    BYTE *s1 = g_base + (0x446087 - 0x400000), *s2 = g_base + (0x4460A9 - 0x400000), *s3 = g_base + (0x446094 - 0x400000);
    auto ok = [](BYTE *p, DWORD site, DWORD target) { return p[0] == 0xE8 && (DWORD)(site + 5 + *(LONG *)(p + 1)) == target; };
    if (!ok(s1, 0x446087, 0x445D10) || !ok(s2, 0x4460A9, 0x445D10) || !ok(s3, 0x446094, 0x445B50)) {
        plog("controls: unexpected code in the game's input routine, bindings NOT applied");
        g_bind_on = false;
        return;
    }
    patch_call(0x446087, 0x445D10, (void *)pad_reader);
    patch_call(0x4460A9, 0x445D10, (void *)pad_reader);
    patch_call(0x446094, 0x445B50, (void *)key_reader);
    plog("controls: race bindings installed (keyboard and joystick readers)");
}

bool input_active() { return g_active; }

UINT WINAPI input_joyGetNumDevs()
{
    start();
    return 16;
}

MMRESULT WINAPI input_joyGetDevCapsA(UINT_PTR id, LPJOYCAPSA c, UINT size)
{
    start();
    if (!c || size < sizeof(JOYCAPSA)) return MMSYSERR_INVALPARAM;
    if (id >= 16) return MMSYSERR_NODRIVER;
    EnterCriticalSection(&g_cs);
    State s = g_state;
    LeaveCriticalSection(&g_cs);
    if (id != 0 || !s.connected) return JOYERR_UNPLUGGED;
    memset(c, 0, sizeof *c);
    c->wMid = 0x45E; c->wPid = 0x28E;
    snprintf(c->szPname, sizeof c->szPname, "%s", s.name[0] ? s.name : "Gamepad");
    c->wXmax = c->wYmax = c->wZmax = c->wRmax = c->wUmax = c->wVmax = 65535;
    c->wNumButtons = kButtons; c->wMaxButtons = 32;
    c->wNumAxes = 6; c->wMaxAxes = 6;
    c->wPeriodMin = 10; c->wPeriodMax = 1000;
    c->wCaps = JOYCAPS_HASZ | JOYCAPS_HASR | JOYCAPS_HASU | JOYCAPS_HASV | JOYCAPS_HASPOV | JOYCAPS_POV4DIR;
    snprintf(c->szRegKey, sizeof c->szRegKey, "ultdaytonausa");
    snprintf(c->szOEMVxD, sizeof c->szOEMVxD, "SDL3");
    return JOYERR_NOERROR;
}

MMRESULT WINAPI input_joyGetPosEx(UINT id, LPJOYINFOEX ji)
{
    start();
    if (!ji || ji->dwSize < sizeof(JOYINFOEX)) return MMSYSERR_INVALPARAM;
    if (id >= 16) return MMSYSERR_NODRIVER;
    EnterCriticalSection(&g_cs);
    State s = g_state;
    LeaveCriticalSection(&g_cs);
    if (id != 0 || !s.connected) return JOYERR_UNPLUGGED;
    DWORD flags = ji->dwFlags;
    ji->dwXpos = s.x; ji->dwYpos = s.y; ji->dwZpos = s.z; ji->dwRpos = s.r; ji->dwUpos = s.u; ji->dwVpos = s.v;
    ji->dwButtons = s.buttons;
    DWORD n = 0; for (DWORD b = s.buttons; b; b &= b - 1) n++;
    ji->dwButtonNumber = n;
    ji->dwPOV = s.pov;
    ji->dwFlags = flags;
    return JOYERR_NOERROR;
}

void input_apply_layout(const char *, const char *) {}   /* 0.0.18: the game's joystick settings are no longer written */

void input_after_settings_loaded()
{
    if (g_bind_on) { apply_rows(); plog("controls: function masks set in every row of the game's table"); }
    if (!g_active) return;
    InterlockedExchange(&g_settings_loaded, 1);
    start();
    State st = current_state();
    g_use_pad = st.connected;
    set_device(st);
}

void input_shutdown(bool wait)
{
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    if (!wait) return;   /* from DllMain at process exit: the thread is already gone or cannot run */
    if (WaitForSingleObject(g_thread, 2000) != WAIT_OBJECT_0) plog("controller: thread did not stop within 2 s");
    CloseHandle(g_thread);
    g_thread = nullptr;
}

InputConfig input_get_config() { return g_cfg; }

static void save_int(const char *key, int v)
{
    char b[16]; snprintf(b, sizeof b, "%d", v);
    WritePrivateProfileStringA("Controller", key, b, g_cfg.ini);
}

void input_set_config(const InputConfig &c)
{
    InputConfig old = g_cfg;
    g_cfg.dead_zone = c.dead_zone;
    g_cfg.trigger_threshold = c.trigger_threshold;
    g_cfg.keyboard_with_pad = c.keyboard_with_pad;
    if (c.dead_zone != old.dead_zone) save_int("DeadZone", c.dead_zone);
    if (c.trigger_threshold != old.trigger_threshold) save_int("TriggerThreshold", c.trigger_threshold);
    if (c.keyboard_with_pad != old.keyboard_with_pad) save_int("KeyboardWithPad", c.keyboard_with_pad);
}

/* ---------- the CONTROLS page ---------- */

bool input_bindings_on() { return g_bind_on; }
const char *input_function_label(int f) { return f >= 0 && f < kFns ? kFnLabels[f] : ""; }

void input_binding_name(int f, bool pad, char *out, size_t n)
{
    if (f < 0 || f >= kFns) { snprintf(out, n, "%s", ""); return; }
    if (!pad) { key_name(g_bind.key[f], out, n); return; }
    int in = g_bind.pad[f];
    snprintf(out, n, "%s", in >= 0 ? kPadShown[current_state().family][in] : "NONE");
}

const char *input_pad_button_name(int which)
{
    static const int k[] = { P_SOUTH, P_EAST, P_WEST };
    return kPadShown[current_state().family][k[which]];
}

void input_bind_clear(int f, bool pad) { if (f >= 0 && f < kFns) bind(f, pad, pad ? -1 : 0); }

void input_apply_preset(PadLayout l)
{
    if (l == PadLayout::Off) return;
    g_bind = preset(l);
    g_cfg.layout = l;
    WritePrivateProfileStringA("Controller", "Layout", l == PadLayout::PS1 ? "Classic" : "Modern", g_cfg.ini);
    save_bindings();
    plog("controls: %s layout put in the table", l == PadLayout::PS1 ? "CLASSIC" : "MODERN");
}

int input_preset_shown()
{
    /* the controller column decides (jayyanz, 1 Nov 2026: CUSTOM once the controller mapping is changed) */
    const PadLayout kPresets[2] = { PadLayout::Modern, PadLayout::PS1 };
    for (PadLayout l : kPresets) {
        Bindings b = preset(l);
        if (!memcmp(b.pad, g_bind.pad, sizeof b.pad)) return l == PadLayout::PS1 ? 1 : 0;
    }
    return 2;
}

void input_capture_start(int f, bool pad)
{
    g_cap = Capture{ true, pad, false, f, GetTickCount() };
    plog("controls: waiting for a %s for %s", pad ? "pad button" : "key", kFnLabels[f]);
}

DWORD input_dpad_left_right() { return current_state().dpad; }

int input_capture_seconds_left()
{
    DWORD gone = GetTickCount() - g_cap.start;
    return gone >= kCaptureMs ? 0 : (int)((kCaptureMs - gone + 999) / 1000);
}

void input_reset_defaults() { input_apply_preset(g_cfg.layout == PadLayout::Off ? PadLayout::Modern : g_cfg.layout); }

bool input_capturing(int *f, bool *pad)
{
    if (f) *f = g_cap.fn;
    if (pad) *pad = g_cap.pad;
    return g_cap.on;
}

/* Called once a frame while waiting. Esc cancels; 5 seconds with no press gives up. The key or button that started
   the wait (and any other) must be released first. */
void input_capture_poll()
{
    if (!g_cap.on) return;
    bool fg = game_has_focus();
    if (GetTickCount() - g_cap.start > kCaptureMs) { g_cap.on = false; plog("controls: no press within 5 s, %s unchanged", kFnLabels[g_cap.fn]); return; }
    if (fg && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) { g_cap.on = false; plog("controls: cancelled, %s unchanged", kFnLabels[g_cap.fn]); return; }
    if (g_cap.pad) {
        DWORD held = current_state().held;
        if (!g_cap.released) { if (!held) g_cap.released = true; return; }
        for (int k = 0; k < kPadInputs; k++)
            if (held & (1u << k)) { g_cap.on = false; bind(g_cap.fn, true, k); return; }
    } else {
        int vk = fg ? first_key_down() : 0;
        if (!g_cap.released) { if (!vk) g_cap.released = true; return; }
        if (vk) { g_cap.on = false; bind(g_cap.fn, false, vk); }
    }
}
