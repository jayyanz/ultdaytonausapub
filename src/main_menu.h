/* Mode Select with EXIT, the initials entry one letter at a time, speed in mph or km/h. See main_menu.cpp. */
#pragma once
#include <windows.h>

void main_menu_install(BYTE *game_base, bool kmh);
bool speed_kmh_get();
void speed_kmh_set(bool kmh);   /* applies at once (the next frame of the race display) */
