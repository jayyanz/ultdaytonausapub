# Release 2 test checklist

What was checked for 2.0.0, on Windows 11 with the game's version 1.1 (a 3840 x 2160 PC with an NVIDIA RTX 5090 and
a 1920 x 1080 PC with an AMD card), from the logs of each run, screenshots and the tester's own eyes, in the
development builds 1.1.3 to 1.1.21. Each check was also run under Wine during development where it could be, and
every build was checked against 64 reference frames captured from the original renderer (identical at 640 x 400).

## The new renderer
- [x] At 640 x 400 the new renderer's picture is the original's, pixel for pixel, on every reference frame
- [x] Every screen: races on all three courses, menus, car select (the bonus cars' own textures), results, attract
- [x] RESOLUTION 640X400, 2X, 3X, 4X and DISPLAY; following the picture's size and Alt+Enter
- [x] FILTER nearest to anisotropic on the 3D and the sky; ANTIALIAS off to 8x, no lines between polygons
- [x] MAPPING PC, SATURN, PERSPECTIVE in all four views (F5 to F8), at the game's draw distance and MAX
- [x] HUD SCALE 100, 75, 50 and HUD FILTER; the lap times in place on every lap; the in-car dashboard and map at
  the game's size
- [x] PRESET: CUSTOM for a mixed set, ENHANCED and ORIGINAL set every row; races in each
- [x] Settings from 1.0.0 kept, new ones on ENHANCED

## CRT, depth buffer, screenshots
- [x] CRT OFF, AUTO, SATURN, PC, 800P at 4K and 1080p (AUTO off at 1080p, as DOSBox Staging)
- [x] DEPTH BUFFER: depth pictures matching the 3D; ReShade 6.8 finds it (Generic Depth) and MXAO shows
- [x] Page Down screenshots at the picture's size, with the depth beside them

## Hardening and long session
- [x] Alt+Enter to a window and back, Alt+Tab out and back, mid-race
- [x] A graphics device loss (simulated in the debug build): display, renderer and depth buffer made again
- [x] 33.5 minutes (a race, then the attract mode) at 4K: memory cycling 115 to 192 MB with the course loads, no
  growth; handles, GDI and USER objects level; 0 fallbacks; clean exit
- [x] Frame pacing: menus a median of 17 ms, races 50 ms, the 95th percentile within 1 ms

## Open
- [ ] A thin dark line left of the rev counter with HUD SCALE below 100, seen only on the 1080p PC with an AMD card
  (issue for the next release)
