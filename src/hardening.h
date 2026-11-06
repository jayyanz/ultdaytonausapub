/* Release hardening (0.0.16): exe version check, crash reports, settings file migration, health log.
   See hardening.cpp. */
#pragma once
#include <windows.h>
#include <string>

/* DAYTONA.EXE v1.1 (Expert Software re-release), the only version this build supports. */
constexpr const char *kSupportedExeSha1 = "28ee8057e75ac7b5187519d255c8a1622df8de33";
constexpr DWORD kSupportedExeSize = 729600;

enum class ExeCheck { Supported, OtherVersion, OtherProgram };
/* Checks the running program. sha1 receives its SHA-1 (hex) when it was computed. */
ExeCheck exe_check(char sha1[41]);
/* The message for a DAYTONA.EXE of another version (shown once, from the game's first DirectDrawCreate). */
void exe_unsupported_message(const char *sha1);

/* Brings ddraw_proxy.ini up to this build's layout: every setting the player has is kept, new settings get
   their defaults and comments, per-controller sections are kept, the old file is kept as
   ddraw_proxy.ini.before-<version>. Writes the default file if there is none. Returns a log line or "". */
std::string settings_migrate(const char *ini_path, const char *version);
/* The default ddraw_proxy.ini of this build (the release or debug variant). */
const char *settings_default_ini();

/* Crash reports: logs\crash_<date>_<time>.txt (exception, registers, stack, log tail) and .dmp (minidump). */
void crash_init(const char *gamedir, const char *log_path, BYTE *game_base);
void crash_set_window(HWND hwnd);
void crash_preload();   /* load dbghelp.dll outside DllMain, before it is needed */
/* Replacement for the game's import of UnhandledExceptionFilter (its C runtime calls it for a crash in the game). */
LONG WINAPI crash_unhandled_exception_filter(EXCEPTION_POINTERS *ep);
#ifdef ULT_DEBUG
void crash_test_config(int seconds);   /* [Test] CrashTest: crash on the game thread after this many seconds */
void crash_test_tick();                /* called on every frame shown, on the game thread */
#endif

/* Health line in the log once a minute: memory, handles, GDI and USER objects, frames shown. */
void health_start();
void health_note_frame();
