# ultdaytonausa

A modern Windows 11 layer for the 1996 PC version of Daytona USA (Sega). It keeps the original game exactly as it
plays, and fixes what stops it running well today: the display, controllers, saving settings and the CD music.

It works as a `ddraw.dll` placed beside the game's own `DAYTONA.EXE`. The game loads it in place of the DirectDraw
system file, and it changes the game while it runs. The game's own files are never modified.

**You need your own copy of the game.** Nothing from the game is included here or in the releases: no program,
pictures, sound, music or data. The installer copies everything from your own disc or disc image.

Not affiliated with or endorsed by Sega. "Daytona USA" is used only to say which game this works with.

## Requirements

- **Windows 11** (64-bit). Other versions of Windows are not supported.
- **Daytona USA for Windows, version 1.1** (the Expert Software re-release; `DAYTONA.EXE`, 729,600 bytes). With any
  other version a message says so, and the game then runs without any changes.
- Your own game disc, or an image of it as `.cue` and `.bin` (the image must include the music tracks).
- A graphics card with Direct3D 11 (any card from the last fifteen years).

## Installing

1. Download `ultdaytonausa-setup.exe` from the [Releases](../../releases) page.
2. Run it. Windows may say it is unrecognised, as it is not signed: choose **More info**, then **Run anyway**.
3. Choose your source: **Disc image (.cue)** or **CD drive**. Choose the folder to install to (for example
   `C:\Games\Daytona USA`) and press **Install**.
4. The installer checks that the game is version 1.1, copies the game files and checks each one, rips the music
   tracks to lossless FLAC (each track verified), and adds `ddraw.dll` with its settings file.
5. Press **Play**, or start `DAYTONA.EXE` from the install folder.

The install is portable: everything stays in the folder you chose. Nothing is written to the registry, there is no
entry in Windows' installed apps, no uninstaller and no shortcuts. To remove it, delete the folder.

Install to a local drive. Running the game from a network drive makes loading very slow.

**Already have the game installed?** Copy `ddraw.dll` from the release into the game folder (beside
`DAYTONA.EXE`). The game folder must not be read-only. For the race music, the tracks must be in `Music\` as
`Track02.flac` to `Track22.flac` with `cdaudio.toc`; the installer makes these, so the installer is the easier way.

## What is included

- **Display:** the game runs full screen on any monitor with no change of display mode, through Direct3D 11.
  The 640 x 400 picture is shown at its own 16:10 shape by default, or stretched to 4:3, or in the game's whole
  4:3 frame. Three scaling choices (sharp, nearest, whole multiples). Alt+Enter switches between full screen and a
  window. Sharp on high-DPI screens, no screen saver during play, the game pauses when you switch away.
- **Controllers:** Xbox, PlayStation, Switch and most other gamepads, plus wheels, pedals and arcade sticks.
  Plug in or unplug while playing. The keyboard and a pad can take turns: the last one used drives.
- **Options pages:** the game's own Options screen, rearranged into pages in the game's style: GAMEPLAY, SOUND,
  DISPLAY, CONTROLS and CHEATS. Every change applies at once and is kept.
- **CONTROLS / BUTTONS:** set a key and a pad button for each of 16 functions (steering, accelerator, brake, shift
  up and down, view change, gears 1 to 4, views 1 to 4, start). Two ready layouts: MODERN (triggers to drive) and
  CLASSIC (the PlayStation layout).
- **CHEATS:** the game's own hidden extras, switched on and off from a page instead of button codes: bonus cars,
  the horse and horse 2, mirror mode, Time Lap, karaoke (with its lyrics now in time with the song), maniac rivals,
  the Advanced course's secret billboard, the Expert course's dancing statue, and the afro team photo. CHEATS
  UNLOCK NO (default): the bonus cars and horses come once earned; YES: all at once.
- **Track draw distance:** two extra CLIP SIZE settings, FAR and MAX, draw the track further ahead. Off by default.
- **Pause menu:** CONTINUE, RESTART RACE and EXIT TO TITLE.
- **Smaller fixes:** settings the game forgot are kept between runs; EXIT on the mode select screen; one letter
  per press when entering your initials, with the countdown at its proper speed; MPH or KM/H; no stall when
  returning to the game; no black flash on Alt; the Windows menu bar removed.
- **CD music:** played from lossless FLAC rips of your disc, so no disc is needed in the drive.
- **Logs and crash reports:** each run writes a log in `logs\`; if the game ever stops with an error, a report is
  written there too.

## Settings and troubleshooting

Almost everything is set in the game's Options pages. The settings are kept in `ddraw_proxy.ini` in the game
folder, with a comment for every setting. When a new version starts for the first time it brings the file up to
date, keeping your settings, and leaves the old file beside it as `ddraw_proxy.ini.before-<version>`.

If something goes wrong:

| Problem | Try |
|---|---|
| Something in the new Options pages misbehaves | `[Proxy] OriginalOptionsMenu=1` brings back the game's original Options screen |
| Black screen or no picture | `[Display] Renderer=GDI` (slower, without Direct3D 11) |
| Display trouble that GDI does not fix | `[Proxy] OwnDirectDraw=0` goes back to Windows' own DirectDraw |
| A controller misbehaves | `[Controller] Enable=0` turns the controller support off (keyboard only) |
| The keyboard takes over from your pad | `[Controller] KeyboardWithPad=0` |
| A wheel's pedals or buttons are wrong | a `[Joystick <name>]` section (examples in the file; the log names your device) |
| No music | check `Music\` has `Track02.flac` to `Track22.flac` and `cdaudio.toc`; or `[Proxy] CDAudioEmulation=0` and the disc in the drive |
| Settings not kept | make sure the game folder is not read-only |
| Back to your old settings | rename the newest `ddraw_proxy.ini.before-<version>` to `ddraw_proxy.ini` |

When reporting a problem, include the newest log from `logs\`, and the two `crash_` files if there are any.

## Building from source

On Linux (Ubuntu or Debian) with MinGW-w64:

```
sudo apt-get install g++-mingw-w64-i686-win32 cmake ninja-build git
sh build/build_mingw.sh
```

This fetches SDL3 3.2.24 and libFLAC 1.4.3 from their official repositories, then builds `dist/<version>/ddraw.dll`
(release), `dist/<version>/debug/ddraw.dll` (debug, with test options) and `dist/<version>/ultdaytonausa-setup.exe`.
Builds are reproducible: the same source gives the same files, so the SHA-1s in `SHA1.txt` can be checked against
the ones published with each release.

## Credits and licences

Our own code and documents are released under **CC0 1.0** (public domain dedication): see `LICENSE`.

Built with, and included in the release files:

- **SDL3** (Simple DirectMedia Layer), zlib licence, including HIDAPI under its BSD-style licence
- **libFLAC** (Xiph.Org Foundation), BSD licence
- **dr_flac** by David Reid, public domain or MIT No Attribution
- the **MinGW-w64** runtime and the **GCC** runtime libraries (under the GCC Runtime Library Exception)

Their licence texts are in `THIRD-PARTY-NOTICES.md`.

Daytona USA is a trademark of Sega. This project is not affiliated with or endorsed by Sega, and includes nothing
from the game.
