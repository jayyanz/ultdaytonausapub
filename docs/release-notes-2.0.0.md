# ultdaytonausa 2.0.0

Release 2: the new renderer. The game's own 3D is drawn again by Direct3D 11 at your screen's resolution, from the
game's own polygons, textures and drawing order, while the game itself runs unchanged. Filtered textures, smooth
edges, perspective-correct texture mapping, a race HUD at the size you choose, an optional CRT look, and a depth
buffer for ReShade. Everything from 1.0.0 stays: the display, controllers, Options pages, cheats and CD music.

**You need your own copy of the game** (version 1.1, from the Expert Software re-release). Nothing from the game is
included: the installer copies everything from your own disc or disc image.

## Download

- `ultdaytonausa-setup.exe`: the installer. Installs the game from your own disc or `.cue` / `.bin` image, checks
  every file, rips the music to lossless FLAC and adds the files below. Portable: nothing outside the folder you
  choose.
- `ddraw.dll`: for a game you have installed already (with 1.0.0 or without). Put it beside `DAYTONA.EXE`; your
  settings are kept and brought up to date.
- `THIRD-PARTY-NOTICES.md`: the licences of the libraries and the shader included.

SHA-1:

| File | SHA-1 |
|---|---|
| `ddraw.dll` | 77fa7d8caa482c25c555c3b2e12046b15a079d1e |
| `ultdaytonausa-setup.exe` | 5eb4f2589f5a394696d5b86c21b2d2973ac4b306 |

Both files can be rebuilt from the source of this release and give the same SHA-1s (see `README.md`).

## What is new

- **PRESET** on Options > DISPLAY: **ENHANCED**, the default (the new renderer at your screen's resolution,
  perspective-correct mapping, trilinear filtering, 4x antialiasing, the race HUD at the game's size, sharp) or
  **ORIGINAL** (the 1996 look, the game's own renderer). Change any row and it shows CUSTOM.
- Each part on its own: RESOLUTION, FILTER, MAPPING (PC, SATURN, PERSPECTIVE), ANTIALIAS, HUD SCALE (100, 75, 50)
  and HUD FILTER. The sky is drawn at full resolution too.
- **CRT:** DOSBox Staging's CRT shader with its arcade presets: AUTO, SATURN (320 x 200), PC (640 x 400), 800P.
- **DEPTH BUFFER** for ReShade (ambient occlusion, depth of field, fog), working with ReShade's default settings.
- **Screenshots:** Page Down, into the game folder's `screenshots` folder.
- If the graphics driver resets, the picture comes back by itself.

The full list is in `CHANGELOG.md`.

## Known issues and limits

- On one 1080p PC with an AMD graphics card, a thin dark vertical line shows just left of the rev counter with HUD
  SCALE below 100. It does not show on an NVIDIA card and is believed to be an AMD driver difference; it is open in
  the issue list for the next release.
- The CRT filter's AUTO needs 3 screen pixels for each of the picture's lines, so on a 1080p screen AUTO stays off
  (as in DOSBox Staging); SATURN suits 1080p.
- The game still runs at its own speed: 20 frames a second in races, 60 in menus.
- At 4K, 8x antialiasing needs a recent graphics card; ENHANCED's 4x is lighter.
- Windows 11 only; version 1.1 of the game only; install to a local drive; not signed (More info, Run anyway).

Not affiliated with or endorsed by Sega. "Daytona USA" is used only to say which game this works with.
