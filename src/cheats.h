/* The CHEATS page: the game's own cheats switched from the Options screen. See cheats.cpp. */
#pragma once
#include <windows.h>

enum { CHEAT_UNLOCK, CHEAT_BONUS_CARS, CHEAT_HORSE, CHEAT_HORSE2, CHEAT_MIRROR, CHEAT_TIME_LAP, CHEAT_KARAOKE,
       CHEAT_MANIAC, CHEAT_BILLBOARD, CHEAT_STATUE, CHEAT_AFRO, CHEAT_COUNT };

void cheats_install(BYTE *game_base, const char *proxy_ini);
void cheats_after_settings_loaded();   /* drops DAYTONA.INI [Config] Command */
bool cheats_installed();
bool cheat_get(int c);
void cheat_set(int c, bool on);       /* applies at once, saved in ddraw_proxy.ini [Cheats] */
bool cheat_available(int c);          /* earned, or CHEATS UNLOCK YES, or no earning rule in the game's code */
