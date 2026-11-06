/* Longer track draw distance (0.0.17): our own course-draw routine. See draw_distance.cpp. */
#pragma once
#include <windows.h>

/* 0 = the game's own Clip Size (NORMAL / WIDE / VAST), 1 = FAR, 2 = MAX */
enum class DrawDistance { Game = 0, Far = 1, Max = 2 };

bool draw_distance_install(BYTE *game_base, DrawDistance level, int stats_log, int far_radius, int max_radius);
DrawDistance draw_distance_get();
void draw_distance_set(DrawDistance level);   /* live, from the Options DISPLAY page */
int draw_distance_radius(DrawDistance level); /* in cells of 32 units */
