# Release 1 test checklist

What was checked for 1.0.0, on Windows 11 with the game's version 1.1, an Xbox Series X controller and the
keyboard, from the logs of each run and the tester's own eyes. Each check was also run under Wine during
development where it could be. Optional checks not run are listed at the end.

## Start-up, picture and window
- [x] Full screen with no display-mode change; the 640 x 400 picture at 16:10 (3840 x 2160 and 1920 x 1080)
- [x] PICTURE 16:10, 4:3 and FRAME change at once; SCALING SHARP, NEAREST and INTEGER
- [x] Alt+Enter to a window and back; in a window the mouse is free, in full screen hidden
- [x] SCREEN WINDOW / FULL SCREEN from the DISPLAY page, kept after a restart
- [x] Alt alone and F10 do nothing (no menu bar, no black frame)
- [x] Alt+Tab and the Windows key: the game pauses and carries on, no stall or reload on return
- [x] Started from a shortcut set to Minimised: comes up full screen, and again after minimising
- [x] With BG DISP and SEETHROUGH off the sky is clean; with them on, the game's own outline modes

## Long session and exit
- [x] The settings file brought up to date by each new build, every setting kept, the old file kept beside it
- [x] Clean exit ("clean shutdown" in the log) and an immediate restart
- [x] 31 minutes in the attract mode, then 50 minutes with races: memory level (65 to 68 MB), handles, GDI and USER
  objects level, the screen kept on

## Sound and music
- [x] Sound effects and race music from the FLAC rips; Music Test on the SOUND page

## Options pages
- [x] Every page with the keyboard and with the pad; the pages look like part of the game
- [x] CLIP SIZE, BG DISP, SEETHROUGH and draw distance kept after a restart

## Controllers
- [x] The pad found and used at the first press, also when plugged in while the game runs
- [x] Right trigger accelerates, left trigger brakes; the trigger point feels right
- [x] Manual gears; the keyboard takes over mid-race and steers, then the pad again
- [x] A wireless pad switched off and on again (as unplugging)
- [x] A race with the CLASSIC layout

## CONTROLS / BUTTONS page
- [x] Set, clear and reuse pad buttons and keys; a reused button leaves its old row on NONE
- [x] The 5-second give-up; Esc and B go back one page; DEFAULTS; LAYOUT MODERN, CLASSIC, CUSTOM
- [x] Gears 1 to 4 and views 1 to 4 on buttons, working in a race; the table kept after a restart
- [x] Readable over the pad picture, the button names match the pad

## Pause menu
- [x] CONTINUE, RESTART RACE (not stopping paused), EXIT TO TITLE, with the pad and the keyboard

## Draw distance
- [x] FAR and MAX on all three courses: further view, no problems, steady 20 frames a second in races

## Mode select, initials, speed
- [x] EXIT with its YES / NO question (NO lit first, B is NO), closing the game cleanly
- [x] VIEW CHANGE goes round all four views
- [x] Initials: one letter per D-pad or key press, holding repeats, the stick as before; the countdown at about one
  a second
- [x] SPEED KM/H on the race display and on the car select screen

## Installer
- [x] Install from a disc image: every game file checked against version 1.1, 21 music tracks ripped and verified,
  the game plays with music
- [x] Portable: nothing written outside the game folder (no registry, installed-apps entry, uninstaller or
  shortcuts); the installed files checked one by one

## CHEATS page
- [x] CHEATS UNLOCK NO: the bonus cars and horses LOCKED until earned; YES: 14 cars, a race as the horse
- [x] MIRROR, TIME LAP, MANIAC (the M on the race display)
- [x] KARAOKE on all three courses, the lyrics in time with the singing
- [x] BILLBOARD on Advanced ("COMING SOON"); STATUE on Expert (it dances when the car stops beside it)
- [x] The page (12 rows) looks right; the line at the bottom is readable

## Not run
- Optional, not run: a second monitor, Windows display scaling above 100 %, ReShade, the logging pass, the crash
  report with the debug build (checked under Wine), a steering wheel, installing from a real CD.
- Not checked on Windows: the old button codes doing nothing; the afro team photo in the ending (no race reached
  the ending; the two photos were checked in the game's picture files).
