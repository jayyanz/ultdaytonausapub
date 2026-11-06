/* Controllers through SDL3, presented to the game as its own Windows (WINMM) joystick. See input_sdl.cpp. */
#pragma once
#include <windows.h>
#include <mmsystem.h>

enum class PadLayout { Off, Modern, PS1 };   /* PS1 is shown as CLASSIC (0.0.18) */
constexpr int INPUT_FUNCTIONS = 16;           /* rows of the CONTROLS page */

struct InputConfig {
    bool enabled = true;           /* [Controller] Enable */
    PadLayout layout = PadLayout::Modern;   /* [Controller] Layout: Modern, PS1, Off (the game's own settings untouched) */
    int dead_zone = 8;             /* [Controller] DeadZone: percent of stick travel ignored around the centre */
    int trigger_threshold = 30;    /* [Controller] TriggerThreshold: percent of travel at which a trigger used as a button presses */
    bool keyboard_with_pad = true; /* [Controller] KeyboardWithPad: the game's keys drive while a pad is connected (last input wins) */
    char ini[MAX_PATH] = "";       /* ddraw_proxy.ini, for the per-controller sections */
    char test_script[MAX_PATH] = "";   /* [Test] VirtualPad: script driving a virtual gamepad (testing without a pad) */
};

void input_init(const InputConfig &cfg, BYTE *game_base);
bool input_active();

/* Replacements for the game's WINMM joystick imports. */
UINT WINAPI input_joyGetNumDevs();
MMRESULT WINAPI input_joyGetDevCapsA(UINT_PTR id, LPJOYCAPSA caps, UINT size);
MMRESULT WINAPI input_joyGetPosEx(UINT id, LPJOYINFOEX ji);

/* Before the game reads DAYTONA.INI at start-up (0.0.18: nothing is written there any more). */
void input_apply_layout(const char *game_ini, const char *proxy_ini);
/* After the game has read its settings: the function masks, and the input device (joystick if a pad is connected). */
void input_after_settings_loaded();
void input_install_bindings();   /* the race readers; after input_init */
void input_shutdown(bool wait);  /* wait: stop the controller thread and wait for it (clean shutdown) */

/* The options screen: current settings, and a change applied at once (saved to ddraw_proxy.ini). */
InputConfig input_get_config();
void input_set_config(const InputConfig &c);

/* The CONTROLS page (game thread). Functions 0 to INPUT_FUNCTIONS-1 in page order. */
bool input_bindings_on();
const char *input_function_label(int f);
void input_binding_name(int f, bool pad, char *out, size_t n);   /* "NONE" when unbound; pad names follow the pad plugged in */
const char *input_pad_button_name(int which);                    /* 0 South, 1 East, 2 West, as the pad plugged in names them */
void input_bind_clear(int f, bool pad);
void input_apply_preset(PadLayout l);                            /* MODERN or CLASSIC into the table (both columns) */
int input_preset_shown();                                        /* 0 MODERN, 1 CLASSIC, 2 CUSTOM (controller column changed by hand) */
void input_capture_start(int f, bool pad);
bool input_capturing(int *f = nullptr, bool *pad = nullptr);
void input_capture_poll();
int input_capture_seconds_left();
DWORD input_dpad_left_right();                                  /* D-pads held: 1 left, 2 right */
void input_reset_defaults();                                    /* DEFAULTS: the table of the LAYOUT in use */
