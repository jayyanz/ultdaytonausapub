# ultdaytonausa 1.0.0

The first release. It runs the 1996 PC version of Daytona USA (version 1.1) on Windows 11 the way it played,
full screen on today's monitors, with modern controllers, its CD music and its hidden extras on a page of their
own.

**You need your own copy of the game** (version 1.1, from the Expert Software re-release). Nothing from the game is
included: the installer copies everything from your own disc or disc image.

## Download

- `ultdaytonausa-setup.exe`: the installer. Installs the game from your own disc or `.cue` / `.bin` image, checks
  every file, rips the music to lossless FLAC and adds the files below. Portable: nothing outside the folder you
  choose.
- `ddraw.dll`: for a game you have installed already. Put it beside `DAYTONA.EXE`.
- `THIRD-PARTY-NOTICES.md`: the licences of the libraries included.

SHA-1:

| File | SHA-1 |
|---|---|
| `ddraw.dll` | 0fbe9b558a4c1597aec53518e3135db6f53f939b |
| `ultdaytonausa-setup.exe` | c9cfae62e0e2e6fae147baa83205059f4348a52c |

Both files can be rebuilt from the source of this release and give the same SHA-1s (see `README.md`).

## What you get

- The game full screen on any monitor through Direct3D 11, no display-mode change, at its own 16:10 shape or 4:3;
  Alt+Enter for a window.
- Gamepads, wheels and pedals, plugged in at any time, with every button set on Options > CONTROLS > BUTTONS.
- The Options screen in pages: GAMEPLAY, SOUND, DISPLAY, CONTROLS, CHEATS.
- CHEATS: bonus cars, the horses, mirror, Time Lap, karaoke (lyrics now in time), maniac, the secret billboard, the
  dancing statue, the afro team photo.
- Longer track draw distance (FAR, MAX), a pause menu, EXIT on the mode select screen, MPH or KM/H.
- The race music from your disc, played from FLAC.

The full list is in `CHANGELOG.md`.

## Known limits

- Windows 11 only.
- Version 1.1 of the game only. Any other version shows a message and runs unchanged.
- The game runs at its own speed, as the original: 20 frames a second in races.
- Install to a local drive: from a network drive the game loads slowly.
- Not signed: Windows may warn when you run the installer (More info, Run anyway).

Not affiliated with or endorsed by Sega. "Daytona USA" is used only to say which game this works with.
