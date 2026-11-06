/* The game's Options screen rearranged into pages. See options_menu.cpp. */
#pragma once
#include <windows.h>

void options_menu_init(BYTE *game_base);
void *options_menu_page();            /* replaces the game's page routine 0x432170 (call at 0x4320A8) */
void *options_menu_filter_image();    /* replace the drawing calls listed below while the game's routine runs */
void *options_menu_filter_text();
extern const DWORD kOptionsImageCalls[17];
extern const DWORD kOptionsTextCalls[13];
