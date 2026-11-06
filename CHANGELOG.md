# Changelog

Each release's files are on the Releases page, with their SHA-1s. Builds are reproducible from the source of the
same version (see `README.md`, Building from source).

## 1.0.0 (Release 1)

The first public release: the 1996 PC version of Daytona USA (v1.1) on Windows 11, kept as it plays, with:

- **Display:** our own DirectDraw (no exclusive mode, no change of display mode) shown through Direct3D 11, full
  screen on any monitor or in a window (Alt+Enter); the 640 x 400 picture at 16:10 (default), stretched to 4:3, or
  in the game's 4:3 frame; scaling sharp, nearest or whole multiples; GDI as a fallback; high-DPI aware; no screen
  saver or sleep during play; pauses when switched away, with no stall or reload on return.
- **Controllers:** gamepads, wheels, pedals and joysticks through SDL3, plugged in or out at any time; keyboard and
  pad take turns, the last one used drives; per-device settings for wheels.
- **Options pages:** the game's Options screen rearranged into GAMEPLAY, SOUND, DISPLAY, CONTROLS and CHEATS pages
  in the game's style; every change applies at once and is saved.
- **CONTROLS / BUTTONS:** a key and a pad input for each of 16 functions, MODERN and CLASSIC layouts, gears 1 to 4
  and views 1 to 4 on any button, VIEW CHANGE going round all four views.
- **CHEATS:** the game's hidden extras on switches (bonus cars, horse, horse 2, mirror, Time Lap, karaoke, maniac,
  the Advanced billboard, the Expert statue, the afro team photo), with CHEATS UNLOCK to earn them first or have
  them all at once. The old button codes are removed.
- **Fixes to the game:** karaoke lyrics now follow the song (the original never advanced them); settings the game
  forgot (Motion, BG Disp, SeeThrough, keys, joystick setup) are kept; read-only settings files are fixed; the
  initials countdown runs at its proper speed; one letter per press when entering initials; keyboard steering
  works after a pad has driven; the CD check is skipped.
- **Additions:** track draw distance FAR and MAX (off by default); a pause menu (CONTINUE, RESTART RACE, EXIT TO
  TITLE); EXIT on the mode select screen; MPH or KM/H; the Windows menu bar removed.
- **CD music:** played from lossless FLAC rips of the player's own disc.
- **Installer:** installs from the player's own disc or `.cue` / `.bin` image, checks every game file against
  version 1.1, rips and verifies the music; portable (nothing outside the game folder).
- **Reliability:** a check that the game is version 1.1 (any other version runs unchanged), crash reports with a
  minidump, a clean shutdown, a settings file brought up to date on each new version with the player's settings
  kept, and a log for every run.

The release build has no test options; the debug build (`debug\ddraw.dll`, in the source build only) has them.

## Development builds

Release 1 was built in 24 development builds, 0.0.1 to 0.0.24 (30 October to 5 November 2026), each tested on
Windows 11. They were not published.
