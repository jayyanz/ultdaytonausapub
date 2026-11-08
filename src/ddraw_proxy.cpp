/*
 * Daytona USA (PC, 1996, v1.1) - ddraw.dll proxy
 *
 * Drop next to DAYTONA.EXE. Forwards DirectDrawCreate to the system ddraw.dll and
 * applies fixes in memory, so every original game file stays untouched:
 *
 *   SkipCDCheck          No "Please insert DAYTONA USA CD." loop; MUSIC.BIN loads from
 *                        the game folder. If a disc is mounted, detection still runs.
 *   ForceEmulation       DirectDraw software emulation (HEL), the equivalent of Windows 98
 *                        "Hardware acceleration: None".
 *   (menu)               The Windows menu bar is removed (0.0.17); Alt / F10 do not open it.
 *   HideLoadingDialog    "[Daytona USA] Loading..." box not shown in full screen.
 *   CDAudioEmulation     Emulates the CD audio device; music plays from Music\\TrackNN.flac (lossless).
 *   MusicVolume          Music level 0-100 (percent) for the emulated CD audio.
 *   PersistSettings      Saves the settings the game forgets (Motion, BG Disp, SeeThrough, keyboard keys,
 *                        joystick button and axis assignment) to DAYTONA.INI on exit and restores them.
 *   FixReadOnlySaves     Clears a read-only attribute on DAYTONA.INI / DAYTONA.DAT so saves can succeed.
 *   Log                  Writes logs\\ddraw_proxy_<date>_<time>.log in the game folder (one file per run).
 *
 * Settings: ddraw_proxy.ini, section [Proxy], 1 = on, 0 = off. All default to on.
 * Version: see ../VERSION and ../CHANGELOG.md (PROXY_VERSION_STR comes from build.sh).
 */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include "proxy.h"
#include "present.h"
#include "trace.h"
#include "ddraw_own.h"
#include "input_sdl.h"
#include "options_menu.h"
#include "main_menu.h"
#include "cheats.h"
#include "hardening.h"
#include "draw_distance.h"
#include "pause_menu.h"
#ifdef ULT_RENDER_CAPTURE   /* the render capture: debug build, private source only (build_mingw.sh) */
#include "render/render_capture.h"
#endif
#include "render/renderer3d.h"
#ifndef PROXY_VERSION_STR
#define PROXY_VERSION_STR "dev"
#endif

/* ---------- config / logging ---------- */
static char g_dir[MAX_PATH];
static int cfg_skipcd = 1, cfg_emul = 1, cfg_loaddlg = 1, cfg_log = 1, cfg_cda = 1, cfg_musvol = 100, cfg_persist = 1, cfg_fixro = 1, cfg_trace = 0, cfg_owndd = 1, cfg_origopts = 0;
static OwnDDConfig g_ddcfg;
static InputConfig g_incfg;
static char g_proxy_ini[MAX_PATH];
static char g_log_path[MAX_PATH];
static ExeCheck g_exe = ExeCheck::OtherProgram;   /* Supported: all fixes; otherwise DirectDrawCreate goes straight to Windows */
static char g_exe_sha1[41];
#ifdef ULT_DEBUG
static int cfg_racerate = 20, cfg_ratelog = 0, cfg_crashtest = 0;   /* [Test], debug build only */
#ifdef ULT_RENDER_CAPTURE
static RenderCaptureConfig cfg_rcap;                                 /* [Test] RenderCapture* (Release 2) */
#endif
#endif
static int cfg_ddlog, cfg_ddfar, cfg_ddmax;                          /* [Test] DrawDistanceLog, FarRadius, MaxRadius */
static DrawDistance cfg_drawdist = DrawDistance::Game;
static Renderer3D cfg_r3d = Renderer3D::New;                       /* [Display] Renderer3D (Release 2) */
static SeeThroughLook cfg_stlook = SeeThroughLook::Textured;      /* [Display] SeeThroughLook (Release 2) */
static int cfg_res3d = 0;                                          /* [Display] Resolution3D (Release 2): 0 Display, 1 640x400, 2-4 */
static int cfg_filter = 3;                                         /* [Display] TextureFilter (Release 2) */
static const char *const kFilterNames[5] = { "Nearest", "Sharp", "Bilinear", "Trilinear", "Anisotropic" };
static bool cfg_depth = false;                                     /* [Display] DepthBuffer (R2-8): Off, On */
static int cfg_mapping = 2;                                        /* [Display] Mapping (Release 2, R2-7): 0 PC, 1 Saturn, 2 Perspective (the default, ENHANCED) */
static int cfg_msaa = 4;                                           /* [Display] Antialiasing (Release 2): samples */
static int cfg_hud_pct = 100, cfg_hud_filter = 1;                   /* [Display] HudScale, HudFilter (Release 2) */
static const char *const kHudFilterNames[3] = { "Nearest", "Sharp", "Bilinear" };
static const char *const kCrtNames[5] = { "Off", "Auto", "Saturn", "PC", "800p" };   /* [Display] Crt (R2-6) */
static bool cfg_kmh;                                                 /* [Gameplay] Speed=KMH */
static FILE *g_log;
static CRITICAL_SECTION g_logcs;

void plog(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap; SYSTEMTIME t; GetLocalTime(&t);
    EnterCriticalSection(&g_logcs);
    fprintf(g_log, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_logcs);
}

static void set_paths(HMODULE self)
{
    char *p;
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    if ((p = strrchr(g_dir, '\\'))) p[1] = 0;
    snprintf(g_proxy_ini, sizeof g_proxy_ini, "%sddraw_proxy.ini", g_dir);
}

static void load_config(HMODULE self)
{
    char ini[MAX_PATH];
    set_paths(self);
    snprintf(ini, sizeof ini, "%s", g_proxy_ini);
    cfg_skipcd  = GetPrivateProfileIntA("Proxy", "SkipCDCheck", 1, ini);
    cfg_emul    = GetPrivateProfileIntA("Proxy", "ForceEmulation", 1, ini);
    cfg_loaddlg = GetPrivateProfileIntA("Proxy", "HideLoadingDialog", 1, ini);
    cfg_log     = GetPrivateProfileIntA("Proxy", "Log", 1, ini);
    cfg_cda     = GetPrivateProfileIntA("Proxy", "CDAudioEmulation", 1, ini);
    cfg_musvol  = GetPrivateProfileIntA("Proxy", "MusicVolume", 100, ini);
    cfg_persist = GetPrivateProfileIntA("Proxy", "PersistSettings", 1, ini);
    cfg_origopts = GetPrivateProfileIntA("Proxy", "OriginalOptionsMenu", 0, ini);
    cfg_fixro   = GetPrivateProfileIntA("Proxy", "FixReadOnlySaves", 1, ini);
    cfg_owndd   = GetPrivateProfileIntA("Proxy", "OwnDirectDraw", 1, ini);
#ifdef ULT_DEBUG
    cfg_racerate = GetPrivateProfileIntA("Test", "RaceLogicRate", 20, ini);
    cfg_ratelog  = GetPrivateProfileIntA("Test", "RateLog", 0, ini);
    cfg_crashtest = GetPrivateProfileIntA("Test", "CrashTest", 0, ini);
#ifdef ULT_RENDER_CAPTURE
    cfg_rcap.every     = GetPrivateProfileIntA("Test", "RenderCapture", 0, ini);
    cfg_rcap.max       = GetPrivateProfileIntA("Test", "RenderCaptureMax", 50, ini);
    cfg_rcap.key_every = GetPrivateProfileIntA("Test", "RenderCaptureKeyframe", 10, ini);
    cfg_rcap.hotkey    = GetPrivateProfileIntA("Test", "RenderCaptureHotkey", 0, ini);
    cfg_rcap.requests  = GetPrivateProfileIntA("Test", "RenderCaptureRequests", 0, ini);
#endif
    cfg_trace    = GetPrivateProfileIntA("Test", "TraceCalls", 0, ini);
    g_ddcfg.frame_hash       = GetPrivateProfileIntA("Test", "FrameHash", 0, ini);
    g_ddcfg.frame_dump_every = GetPrivateProfileIntA("Test", "FrameDumpEvery", 0, ini);
    g_ddcfg.frame_dump_max   = GetPrivateProfileIntA("Test", "FrameDumpMax", 100, ini);
#endif   /* release (1.0.0): no [Test] options are read at all */
    {
        char v[32];
        DisplayConfig &d = g_ddcfg.display;
        GetPrivateProfileStringA("Display", "Mode", "Fullscreen", v, sizeof v, ini);
        d.windowed = !lstrcmpiA(v, "Windowed");
        d.window_scale = GetPrivateProfileIntA("Display", "WindowScale", 2, ini);
        if (d.window_scale < 1) d.window_scale = 1;
        GetPrivateProfileStringA("Display", "Scaling", "SharpBilinear", v, sizeof v, ini);
        d.scaling = !lstrcmpiA(v, "Nearest") ? Scaling::Nearest : !lstrcmpiA(v, "Integer") ? Scaling::Integer : Scaling::SharpBilinear;
        GetPrivateProfileStringA("Display", "Picture", "16:10", v, sizeof v, ini);   /* "Auto" (0.0.10 to 0.0.14): now 16:10 */
        d.picture = !lstrcmpiA(v, "4:3") || !lstrcmpiA(v, "Fill") ? Picture::Fill : !lstrcmpiA(v, "Frame") ? Picture::Frame : Picture::Native;
        d.monitor = GetPrivateProfileIntA("Display", "Monitor", 0, ini);
        d.pause_in_background = GetPrivateProfileIntA("Display", "PauseInBackground", 1, ini) != 0;
        d.vsync = GetPrivateProfileIntA("Display", "VSync", 1, ini) != 0;
        GetPrivateProfileStringA("Display", "Renderer", "D3D11", v, sizeof v, ini);
        d.d3d11 = lstrcmpiA(v, "GDI") != 0;
        GetPrivateProfileStringA("Gameplay", "Speed", "MPH", v, sizeof v, ini);
        cfg_kmh = !lstrcmpiA(v, "KMH") || !lstrcmpiA(v, "KM/H");
        GetPrivateProfileStringA("Display", "DrawDistance", "Game", v, sizeof v, ini);
        cfg_drawdist = !lstrcmpiA(v, "Max") ? DrawDistance::Max : !lstrcmpiA(v, "Far") ? DrawDistance::Far : DrawDistance::Game;
        GetPrivateProfileStringA("Display", "Renderer3D", "New", v, sizeof v, ini);
        cfg_r3d = !lstrcmpiA(v, "Original") ? Renderer3D::Original : Renderer3D::New;
        GetPrivateProfileStringA("Display", "SeeThroughLook", "Textured", v, sizeof v, ini);
        cfg_stlook = !lstrcmpiA(v, "Outline") ? SeeThroughLook::Outline : SeeThroughLook::Textured;
        GetPrivateProfileStringA("Display", "Resolution3D", "Display", v, sizeof v, ini);
        cfg_res3d = !lstrcmpiA(v, "640x400") ? 1 : !lstrcmpiA(v, "2x") ? 2 : !lstrcmpiA(v, "3x") ? 3 : !lstrcmpiA(v, "4x") ? 4 : 0;
        GetPrivateProfileStringA("Display", "TextureFilter", "Trilinear", v, sizeof v, ini);
        cfg_filter = 3;
        for (int i = 0; i < 5; i++) if (!lstrcmpiA(v, kFilterNames[i])) cfg_filter = i;
        GetPrivateProfileStringA("Display", "Antialiasing", "4x", v, sizeof v, ini);
        cfg_msaa = !lstrcmpiA(v, "Off") ? 1 : !lstrcmpiA(v, "2x") ? 2 : !lstrcmpiA(v, "8x") ? 8 : 4;
        GetPrivateProfileStringA("Display", "Mapping", "Perspective", v, sizeof v, ini);
        cfg_mapping = !lstrcmpiA(v, "Saturn") ? 1 : !lstrcmpiA(v, "PC") ? 0 : 2;
        GetPrivateProfileStringA("Display", "DepthBuffer", "Off", v, sizeof v, ini);
        cfg_depth = !lstrcmpiA(v, "On");
        cfg_hud_pct = (int)GetPrivateProfileIntA("Display", "HudScale", 100, ini);
        GetPrivateProfileStringA("Display", "HudFilter", "Sharp", v, sizeof v, ini);
        for (int i = 0; i < 3; i++) if (!lstrcmpiA(v, kHudFilterNames[i])) cfg_hud_filter = i;
        GetPrivateProfileStringA("Display", "Crt", "Off", v, sizeof v, ini);
        {
            int crt = !lstrcmpiA(v, "Arcade") ? 1 : 0;   /* 1.1.11's name for AUTO */
            for (int i = 0; i < 5; i++) if (!lstrcmpiA(v, kCrtNames[i])) crt = i;
            present_set_crt(crt);
        }
#ifdef ULT_DEBUG
        cfg_ddlog = GetPrivateProfileIntA("Test", "DrawDistanceLog", 0, ini);
        cfg_ddfar = GetPrivateProfileIntA("Test", "FarRadius", 0, ini);
        cfg_ddmax = GetPrivateProfileIntA("Test", "MaxRadius", 0, ini);
#endif
    }
    if (cfg_log) {
        char lp[MAX_PATH]; SYSTEMTIME t; GetLocalTime(&t);
        snprintf(lp, sizeof lp, "%slogs", g_dir);
        CreateDirectoryA(lp, NULL);
        snprintf(lp, sizeof lp, "%slogs\\ddraw_proxy_%04d%02d%02d_%02d%02d%02d.log", g_dir,
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        g_log = fopen(lp, "w");
        if (g_log) snprintf(g_log_path, sizeof g_log_path, "%s", lp);
    }
#ifdef ULT_DEBUG
    plog("Daytona USA ddraw proxy v%s (debug build: test options on)", PROXY_VERSION_STR);
#else
    plog("Daytona USA ddraw proxy v%s (release build)", PROXY_VERSION_STR);
#endif
    plog("ddraw proxy loaded. OwnDirectDraw=%d SkipCDCheck=%d ForceEmulation=%d MenuRemoved=1 HideLoadingDialog=%d CDAudioEmulation=%d MusicVolume=%d PersistSettings=%d FixReadOnlySaves=%d",
         cfg_owndd, cfg_skipcd, cfg_emul, cfg_loaddlg, cfg_cda, cfg_musvol, cfg_persist, cfg_fixro);
#ifdef ULT_DEBUG
    plog("test settings: RaceLogicRate=%d RateLog=%d CrashTest=%d TraceCalls=%d FrameHash=%d FrameDumpEvery=%d FrameDumpMax=%d", cfg_racerate, cfg_ratelog,
         cfg_crashtest, cfg_trace, g_ddcfg.frame_hash, g_ddcfg.frame_dump_every, g_ddcfg.frame_dump_max);
#endif
    {
        const DisplayConfig &d = g_ddcfg.display;
        plog("display settings: Mode=%s WindowScale=%d Scaling=%s Picture=%s VSync=%d Renderer=%s Monitor=%d PauseInBackground=%d",
             d.windowed ? "Windowed" : "Fullscreen", d.window_scale,
             d.scaling == Scaling::Nearest ? "Nearest" : d.scaling == Scaling::Integer ? "Integer" : "SharpBilinear",
             d.picture == Picture::Fill ? "4:3" : d.picture == Picture::Frame ? "Frame" : "16:10", d.vsync, d.d3d11 ? "D3D11" : "GDI",
             d.monitor, d.pause_in_background);
    }
    {
        char v[32];
        g_incfg.enabled = GetPrivateProfileIntA("Controller", "Enable", 1, ini) != 0;
        GetPrivateProfileStringA("Controller", "Layout", "Modern", v, sizeof v, ini);
        g_incfg.layout = !lstrcmpiA(v, "PS1") || !lstrcmpiA(v, "Classic") ? PadLayout::PS1 : !lstrcmpiA(v, "Off") ? PadLayout::Off : PadLayout::Modern;
        g_incfg.dead_zone = GetPrivateProfileIntA("Controller", "DeadZone", 8, ini);
        g_incfg.trigger_threshold = GetPrivateProfileIntA("Controller", "TriggerThreshold", 30, ini);
        g_incfg.keyboard_with_pad = GetPrivateProfileIntA("Controller", "KeyboardWithPad", 1, ini) != 0;
        snprintf(g_incfg.ini, sizeof g_incfg.ini, "%s", ini);
        char sc[MAX_PATH] = "";
#ifdef ULT_DEBUG
        GetPrivateProfileStringA("Test", "VirtualPad", "", sc, sizeof sc, ini);
#endif
        if (sc[0]) snprintf(g_incfg.test_script, sizeof g_incfg.test_script, "%s%s", strchr(sc, ':') ? "" : g_dir, sc);
        plog("controller settings: Enable=%d Layout=%s DeadZone=%d TriggerThreshold=%d KeyboardWithPad=%d%s%s", g_incfg.enabled,
             g_incfg.layout == PadLayout::PS1 ? "Classic" : g_incfg.layout == PadLayout::Off ? "Off" : "Modern",
             g_incfg.dead_zone, g_incfg.trigger_threshold, g_incfg.keyboard_with_pad, sc[0] ? " VirtualPad=" : "", sc);
    }
    frame_capture_config(g_ddcfg);
}

/* ---------- memory patching ---------- */
static BYTE *g_base;   /* DAYTONA.EXE image base */

static int patch(DWORD va, const void *expect, const void *repl, size_t n, const char *what)
{
    BYTE *p = g_base + (va - 0x400000);
    DWORD old;
    if (memcmp(p, expect, n) != 0) { plog("patch %s @%08lX: bytes differ, NOT applied", what, va); return 0; }
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, repl, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    plog("patch %s @%08lX: applied", what, va);
    return 1;
}

/* Coming back to the game (WM_ACTIVATEAPP 1, 0x439880) the game rebuilds its display (0x43AFE0: display mode, flip
   chain, the eight background pages read again from their BMP files), as DirectDraw needed after losing exclusive full
   screen. Our own DirectDraw never loses its surfaces, so the call is skipped: Alt+Tab is instant from any drive. */
static void apply_no_rebuild_patch(void)
{
    static const BYTE old_[] = { 0xE8, 0x43, 0x17, 0x00, 0x00 };   /* call 0x43AFE0 */
    static const BYTE new_[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    patch(0x439898, old_, new_, sizeof old_, "no display rebuild on return");
    /* Alt pressed (WM_SYSKEYDOWN, 0x4395A0): the game readied the flip chain for its Windows menu (0x41A820 releases and
       creates the primary and back buffer again), which showed one black frame. The menu is gone (0.0.17). */
    static const BYTE alt_old[] = { 0xE8, 0x27, 0x12, 0xFE, 0xFF };   /* call 0x41A820 */
    patch(0x4395F4, alt_old, new_, sizeof alt_old, "no flip-chain reset on Alt");
}

static void apply_cd_patch(void)
{
    /* CheckCD 0x43A740: CD-not-found branch -> return 1 (drive index stays -1) */
    static const BYTE cd_old[] = { 0x6A,0x35, 0x50, 0x68,0xCC,0x62,0x48,0x00 };
    static const BYTE cd_new[] = { 0xB8,0x01,0x00,0x00,0x00, 0x5E, 0xC3, 0x90 };
    /* MUSIC.BIN path format string */
    static const char mus_old[23] = "%c:\\GAME\\ETC\\MUSIC.BIN";
    static const char mus_new[23] = "MUSIC.BIN";
    patch(0x43A75B, cd_old, cd_new, sizeof cd_old, "CheckCD");
    patch(0x48C9F4, mus_old, mus_new, sizeof mus_old, "MUSIC.BIN path");
}

/* ---------- IAT hooking of DAYTONA.EXE ---------- */
static int hook_iat_raw(const char *dll, const char *func, void *fn)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dd->VirtualAddress);
    for (; imp->Name; imp++) {
        if (lstrcmpiA((char *)(g_base + imp->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(g_base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(g_base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, func)) continue;
            if (trace_retarget((void *)iat->u1.Function, fn, NULL)) return 1;   /* our hook goes behind the trace stub */
            if ((void *)iat->u1.Function == fn) return 1;          /* already hooked */
            if (iat->u1.Function == names->u1.AddressOfData) return 0; /* not bound yet */
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            iat->u1.Function = (ULONG_PTR)fn;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            plog("hooked %s!%s", dll, func);
            return 1;
        }
    }
    plog("hook %s!%s: import not found", dll, func);
    return 0;
}
template <typename F> static int hook_iat(const char *dll, const char *func, F fn) { return hook_iat_raw(dll, func, fnptr(fn)); }

/* ---------- full-screen state, menu and loading dialog ---------- */
static HWND  g_hwnd;           /* game window passed to SetCooperativeLevel */
static int   g_fullscreen;
static HMENU g_gamemenu;       /* menu the game believes is attached */
static int   g_inmenu;         /* menu temporarily shown (Alt) */
static HWND  g_loaddlg;
static WNDPROC g_origproc;

static BOOL  (WINAPI *real_SetMenu)(HWND, HMENU);
static HMENU (WINAPI *real_GetMenu)(HWND);
static HWND  (WINAPI *real_CreateDialogParamA)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
static BOOL  (WINAPI *real_ShowWindow)(HWND, int);
static BOOL  (WINAPI *real_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static void  (WINAPI *real_OutputDebugStringA)(LPCSTR);
static int   (WINAPI *real_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
static void  (WINAPI *real_PostQuitMessage)(int);
static BOOL  (WINAPI *real_ClipCursor)(const RECT *);

static void sync_menu_checks(HMENU m);
/* The Windows menu bar is removed (jayyanz 1 Nov 2026, 0.0.17): never attached to the game's window, and Alt / F10
   do not open it. The game keeps its menu handle for its own tick marks; its commands are sent by the Options pages. */
static int menu_hidden_mode(void) { return g_hwnd != 0; }

static void describe_window(HWND w, char *out, int n)
{
    char title[96] = "", cls[64] = ""; DWORD pid = 0;
    if (w) { GetWindowTextA(w, title, sizeof title); GetClassNameA(w, cls, sizeof cls); GetWindowThreadProcessId(w, &pid); }
    snprintf(out, n, "%p class='%s' title='%s' pid=%lu%s", (void *)w, cls, title, pid,
             pid == GetCurrentProcessId() ? " (this game)" : "");
}

static const char *msgname(UINT m)
{
    switch (m) {
    case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP"; case WM_ACTIVATE: return "WM_ACTIVATE";
    case WM_SETFOCUS: return "WM_SETFOCUS"; case WM_KILLFOCUS: return "WM_KILLFOCUS";
    case WM_SIZE: return "WM_SIZE"; case WM_SYSCOMMAND: return "WM_SYSCOMMAND";
    case WM_CLOSE: return "WM_CLOSE"; case WM_DESTROY: return "WM_DESTROY";
    case WM_DISPLAYCHANGE: return "WM_DISPLAYCHANGE"; case WM_ENTERMENULOOP: return "WM_ENTERMENULOOP";
    case WM_EXITMENULOOP: return "WM_EXITMENULOOP"; case WM_SHOWWINDOW: return "WM_SHOWWINDOW";
    case WM_NCACTIVATE: return "WM_NCACTIVATE"; case WM_QUERYNEWPALETTE: return "WM_QUERYNEWPALETTE";
    }
    return NULL;
}

static void toggle_window_mode(HWND h);
static void ensure_game_placement(HWND h);
static LRESULT CALLBACK game_subproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    trace_message(m);
    if (cfg_owndd && h == g_hwnd) {
        static DWORD alt_enter_at;
        if (m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29))) {
            if (!(l & (1 << 30))) toggle_window_mode(h);   /* ignore key repeat */
            alt_enter_at = GetTickCount();
            return 0;
        }
        if (m == WM_SYSCHAR && w == VK_RETURN) return 0;  /* no beep */
        if (m == WM_ACTIVATEAPP && w) ensure_game_placement(h);   /* back from minimised or a stray size */
        /* screen saver and monitor sleep stay off while the game runs (as with a full-screen DirectX game) */
        if (m == WM_SYSCOMMAND && ((w & 0xFFF0) == SC_SCREENSAVE || (w & 0xFFF0) == SC_MONITORPOWER)) return 0;
        /* PauseInBackground=0: the game keeps running (and its music playing) when it loses focus */
        if (m == WM_ACTIVATEAPP && !w && !g_ddcfg.display.pause_in_background) {
            static int n; if (n++ < 3) plog("lost focus: game kept running (PauseInBackground=0)");
            return CallWindowProcA(g_origproc, h, m, TRUE, l);
        }
        /* releasing Alt after Alt+Enter must not open the menu bar */
        if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU && l == 0 && alt_enter_at && GetTickCount() - alt_enter_at < 3000) {
            alt_enter_at = 0;
            return 0;
        }
    }
    const char *nm = msgname(m);
    if (nm) {
        char other[256] = "";
        if ((m == WM_ACTIVATEAPP || m == WM_ACTIVATE || m == WM_NCACTIVATE) && !LOWORD(w)) {
            char fg[200]; describe_window(GetForegroundWindow(), fg, sizeof fg);
            snprintf(other, sizeof other, "  foreground now: %s", fg);
        } else if (m == WM_KILLFOCUS) {
            char fw[200]; describe_window((HWND)w, fw, sizeof fw);
            snprintf(other, sizeof other, "  focus to: %s", fw);
        }
        plog("msg %s w=0x%lX l=0x%lX%s", nm, (unsigned long)w, (unsigned long)l, other);
    }
    if (m == WM_INITMENUPOPUP && cfg_persist) sync_menu_checks(g_gamemenu ? g_gamemenu : real_GetMenu(h));
    if (menu_hidden_mode() && h == g_hwnd) {
        if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU) {
            static int n; if (n++ < 3) plog("Alt / F10: no menu (the Windows menu is removed)");
            return 0;
        } else if (m == WM_EXITMENULOOP && g_inmenu) {
            LRESULT r = CallWindowProcA(g_origproc, h, m, w, l);
            g_inmenu = 0;
            real_SetMenu(h, NULL);
            plog("menu hidden");
            return r;
        }
    }
    return CallWindowProcA(g_origproc, h, m, w, l);
}

static BOOL WINAPI hk_SetMenu(HWND h, HMENU m)
{
    if (h == g_hwnd) g_gamemenu = m;
    if (menu_hidden_mode() && h == g_hwnd && !g_inmenu) {
        plog("SetMenu(%p) by game kept hidden (full screen)", (void *)m);
        return real_SetMenu(h, NULL);
    }
    return real_SetMenu(h, m);
}

static HMENU WINAPI hk_GetMenu(HWND h)
{
    if (menu_hidden_mode() && h == g_hwnd) return g_gamemenu;
    return real_GetMenu(h);
}

static HWND WINAPI hk_CreateDialogParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
{
    HWND d = real_CreateDialogParamA(inst, tmpl, parent, proc, lp);
    if (IS_INTRESOURCE(tmpl)) {
        plog("CreateDialogParamA(template 0x%X) -> %p", (unsigned)(ULONG_PTR)tmpl, (void *)d);
        if ((ULONG_PTR)tmpl == 0x72) g_loaddlg = d;
    }
    return d;
}

static int hide_this_dialog(HWND h)
{
    return cfg_loaddlg && g_fullscreen && h && h == g_loaddlg && IsWindow(h);
}

/* If the hidden loading box has taken activation, hand it straight back to the game window,
   otherwise DirectDraw treats it as the full-screen window losing focus and minimises it. */
static void keep_game_active(void)
{
    HWND a = GetActiveWindow();
    if (g_hwnd && a == g_loaddlg) {
        SetActiveWindow(g_hwnd);
        SetFocus(g_hwnd);
        plog("loading dialog had taken activation; returned to game window");
    }
}

static BOOL WINAPI hk_ShowWindow(HWND h, int cmd)
{
    if (hide_this_dialog(h) && cmd != SW_HIDE) {
        static int n;
        if (n++ < 3) plog("loading dialog ShowWindow(%d) suppressed", cmd);
        BOOL r = real_ShowWindow(h, SW_HIDE);
        keep_game_active();
        return r;
    }
    /* Started from a shortcut or launcher set to "Minimised": the game's main window is shown normally instead, as
       our own full screen or window is placed on it (a minimised start left a 237 x 39 window, 5 Nov 2026). */
    if (cfg_owndd && (cmd == SW_SHOWMINIMIZED || cmd == SW_MINIMIZE || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE
                      || cmd == SW_SHOWDEFAULT)) {
        char cls[32] = "";
        GetClassNameA(h, cls, sizeof cls);
        if (!lstrcmpA(cls, "DaytonaClass")) {
            static int n;
            if (cmd != SW_SHOWDEFAULT || IsIconic(h)) { if (n++ < 3) plog("game window ShowWindow(%d): shown normally", cmd); }
            BOOL r = real_ShowWindow(h, SW_SHOWNORMAL);
            if (IsIconic(h)) real_ShowWindow(h, SW_RESTORE);
            return r;
        }
    }
    return real_ShowWindow(h, cmd);
}

static int g_own_fs;   /* our own DirectDraw is showing the game full screen (borderless) */

struct MonitorPick { int want, n; HMONITOR found; };
static BOOL CALLBACK pick_monitor(HMONITOR m, HDC, LPRECT, LPARAM lp)
{
    MonitorPick *p = (MonitorPick *)lp;
    if (++p->n == p->want) { p->found = m; return FALSE; }
    return TRUE;
}

/* The monitor the game is shown on: [Display] Monitor=N (1 = first display) or, with 0, the one the window is on. */
static HMONITOR game_monitor(HWND h)
{
    static int logged;
    if (g_ddcfg.display.monitor > 0) {
        MonitorPick p{ g_ddcfg.display.monitor, 0, nullptr };
        EnumDisplayMonitors(nullptr, nullptr, pick_monitor, (LPARAM)&p);
        if (p.found) return p.found;
        if (!logged++) plog("display: Monitor=%d not found (%d displays), using the current one", g_ddcfg.display.monitor, p.n);
    }
    return MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
}

static RECT monitor_rect(HWND h)
{
    MONITORINFO mi; mi.cbSize = sizeof mi;
    RECT r = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (GetMonitorInfoA(game_monitor(h), &mi)) r = mi.rcMonitor;
    return r;
}

static int g_own_windowed;    /* our own DirectDraw is showing the game in a normal window */

/* The game hides the mouse cursor with ShowCursor. In a window the cursor stays visible, so the player can use the
   title bar and other programs; in full screen the game's own choice applies. */
static int (WINAPI *real_ShowCursor)(BOOL);
static int g_game_cursor;           /* the game's ShowCursor counter (0 = visible, below 0 = hidden) */
static int g_real_cursor;           /* our calls' net effect on Windows' counter */
static void sync_cursor()
{
    if (!real_ShowCursor) return;
    int want = (g_own_windowed || g_game_cursor >= 0) ? 0 : -1;
    while (g_real_cursor < want) { real_ShowCursor(TRUE); g_real_cursor++; }
    while (g_real_cursor > want) { real_ShowCursor(FALSE); g_real_cursor--; }
}
static int WINAPI hk_ShowCursor(BOOL show)
{
    g_game_cursor += show ? 1 : -1;
    sync_cursor();
    return g_game_cursor;
}

static BOOL WINAPI hk_ClipCursor(const RECT *r)
{
    /* in a window the mouse must stay free to leave it */
    return real_ClipCursor(g_own_windowed ? nullptr : r);
}

static BOOL WINAPI hk_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f)
{
    if (g_own_fs && h == g_hwnd && !(f & SWP_NOSIZE && f & SWP_NOMOVE)) {
        if (g_own_windowed) {
            f |= SWP_NOSIZE | SWP_NOMOVE;   /* the player's window: the game does not move or size it */
        } else {
            /* full screen is our borderless window covering the monitor; keep it there */
            RECT m = monitor_rect(h);
            x = m.left; y = m.top; cx = m.right - m.left; cy = m.bottom - m.top;
            f &= ~(SWP_NOSIZE | SWP_NOMOVE);
        }
    }
    if (hide_this_dialog(h)) {
        /* game centres the box with SetWindowPos(..., SWP_NOSIZE), which also activates it */
        f |= SWP_NOACTIVATE | SWP_NOZORDER;
        if (f & SWP_SHOWWINDOW) f = (f & ~SWP_SHOWWINDOW) | SWP_HIDEWINDOW;
        BOOL r = real_SetWindowPos(h, after, x, y, cx, cy, f);
        keep_game_active();
        return r;
    }
    return real_SetWindowPos(h, after, x, y, cx, cy, f);
}

static void WINAPI hk_OutputDebugStringA(LPCSTR s)
{
    if (s) {
        char buf[512]; size_t n;
        lstrcpynA(buf, s, sizeof buf);
        n = strlen(buf); while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        plog("game: %s", buf);
    }
    real_OutputDebugStringA(s);
}

static int WINAPI hk_MessageBoxA(HWND h, LPCSTR t, LPCSTR c, UINT f)
{
    plog("MessageBox '%s': %s", c ? c : "", t ? t : "");
    int r = real_MessageBoxA(h, t, c, f);
    plog("MessageBox returned %d", r);
    return r;
}

static void WINAPI hk_PostQuitMessage(int code)
{
    plog("game called PostQuitMessage(%d)", code);
    real_PostQuitMessage(code);
}


static void hook_vtable_raw(void **vt, int idx, void *fn, void **orig);
template <typename F> static void hook_vtable(void **vt, int idx, F fn, void **orig) { hook_vtable_raw(vt, idx, fnptr(fn), orig); }
/* ---------- file, MCI (CD audio) and DirectSound diagnostics ---------- */
#include <mmsystem.h>
#include <dsound.h>

static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static MCIERROR (WINAPI *real_mciSendCommandA)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
static BOOL (WINAPI *real_mciGetErrorStringA)(MCIERROR, LPSTR, UINT);
static HRESULT (WINAPI *real_DirectSoundCreate)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);

static HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tmpl)
{
    HANDLE h = real_CreateFileA(name, acc, share, sa, disp, fl, tmpl);
    DWORD e = GetLastError();
    if (h == INVALID_HANDLE_VALUE) plog("CreateFileA('%s', %s) FAILED error %lu", name ? name : "", (acc & GENERIC_WRITE) ? "write" : "read", e);
    else plog("CreateFileA('%s', %s) ok", name ? name : "", (acc & GENERIC_WRITE) ? "write" : "read");
    SetLastError(e);
    return h;
}

static const char *mci_name(UINT m)
{
    switch (m) {
    case MCI_OPEN: return "OPEN"; case MCI_CLOSE: return "CLOSE"; case MCI_PLAY: return "PLAY";
    case MCI_SEEK: return "SEEK"; case MCI_STOP: return "STOP"; case MCI_PAUSE: return "PAUSE";
    case MCI_SET: return "SET"; case MCI_STATUS: return "STATUS"; case MCI_RESUME: return "RESUME";
    case MCI_INFO: return "INFO"; case MCI_GETDEVCAPS: return "GETDEVCAPS";
    }
    return "?";
}

static MCIERROR WINAPI hk_mciSendCommandA(MCIDEVICEID dev, UINT msg, DWORD_PTR flags, DWORD_PTR param)
{
    int emu = cda_is_open_request(msg, flags, param) || cda_is_device(dev);
    MCIERROR r = emu ? cda_command(dev, msg, flags, param) : real_mciSendCommandA(dev, msg, flags, param);
    char extra[160] = "", err[128] = "";
    if (msg == MCI_OPEN && param) {
        MCI_OPEN_PARMSA *o = (MCI_OPEN_PARMSA *)param;
        snprintf(extra, sizeof extra, " type='%s' element='%s' -> device %u",
                 (flags & MCI_OPEN_TYPE) && o->lpstrDeviceType && !IS_INTRESOURCE(o->lpstrDeviceType) ? o->lpstrDeviceType : "",
                 (flags & MCI_OPEN_ELEMENT) && o->lpstrElementName ? o->lpstrElementName : "", (unsigned)o->wDeviceID);
    } else if (msg == MCI_PLAY && param) {
        MCI_PLAY_PARMS *p = (MCI_PLAY_PARMS *)param;
        snprintf(extra, sizeof extra, " from=%s0x%lX to=%s0x%lX", (flags & MCI_FROM) ? "" : "(unset)", p->dwFrom,
                 (flags & MCI_TO) ? "" : "(unset)", p->dwTo);
    } else if (msg == MCI_STATUS && param) {
        MCI_STATUS_PARMS *p = (MCI_STATUS_PARMS *)param;
        snprintf(extra, sizeof extra, " item=0x%lX track=%lu -> 0x%lX", p->dwItem, p->dwTrack, (unsigned long)p->dwReturn);
    } else if (msg == MCI_SET && param) {
        MCI_SET_PARMS *p = (MCI_SET_PARMS *)param;
        snprintf(extra, sizeof extra, " timeformat=%lu", p->dwTimeFormat);
    }
    if (r && real_mciGetErrorStringA) { real_mciGetErrorStringA(r, err, sizeof err); }
    {
        static int n_status;
        if (msg != MCI_STATUS || r || n_status++ < 40)
            plog("MCI%s %s dev=%u flags=0x%lX%s = %lu%s%s", emu ? " (emulated)" : "", mci_name(msg), (unsigned)dev, (unsigned long)flags, extra,
                 (unsigned long)r, r ? " " : "", err);
    }
    return r;
}

/* DirectSound: per-vtable originals so primary and secondary buffers can differ */
#define DS_MAXVT 4
static void **dsb_vt[DS_MAXVT]; static void *dsb_orig[DS_MAXVT][21]; static int dsb_nvt;
static void **dsb_orig_for(void *obj)
{
    void **vt = *(void ***)obj; int i;
    for (i = 0; i < dsb_nvt; i++) if (dsb_vt[i] == vt) return dsb_orig[i];
    return NULL;
}
enum { B_PLAY, B_STOP, B_LOCK, B_SETVOL, B_N };
static const char *b_names[B_N] = { "Play", "Stop", "Lock", "SetVolume" };
static volatile LONG b_calls[B_N], b_fails[B_N]; static LONG b_flog[B_N]; static DWORD b_last;
static void dsb_note(int i, HRESULT r, void *buf)
{
    InterlockedIncrement(&b_calls[i]);
    if (FAILED(r)) { InterlockedIncrement(&b_fails[i]); if (b_flog[i] < 15) { b_flog[i]++; plog("SoundBuffer::%s(%p) FAILED 0x%08lX", b_names[i], buf, r); } }
    DWORD now = GetTickCount();
    if (now - b_last >= 1000) {
        char b[200]; int n = 0, k; b_last = now;
        for (k = 0; k < B_N; k++) {
            LONG c = InterlockedExchange(&b_calls[k], 0), f = InterlockedExchange(&b_fails[k], 0);
            if (c) n += snprintf(b + n, sizeof b - n, " %s=%ld", b_names[k], c);
            if (f) n += snprintf(b + n, sizeof b - n, "(fail %ld)", f);
        }
        plog("sound per second:%s", n ? b : " none");
    }
}
typedef HRESULT (WINAPI *BPlay_t)(IDirectSoundBuffer *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *BStop_t)(IDirectSoundBuffer *);
typedef HRESULT (WINAPI *BLock_t)(IDirectSoundBuffer *, DWORD, DWORD, LPVOID *, LPDWORD, LPVOID *, LPDWORD, DWORD);
typedef HRESULT (WINAPI *BVol_t)(IDirectSoundBuffer *, LONG);
typedef HRESULT (WINAPI *BFmt_t)(IDirectSoundBuffer *, LPCWAVEFORMATEX);
static HRESULT WINAPI hb_Play(IDirectSoundBuffer *b, DWORD r1, DWORD pr, DWORD f)
{ void **o = dsb_orig_for(b); HRESULT r = ((BPlay_t)o[12])(b, r1, pr, f); dsb_note(B_PLAY, r, b); return r; }
static HRESULT WINAPI hb_Stop(IDirectSoundBuffer *b)
{ void **o = dsb_orig_for(b); HRESULT r = ((BStop_t)o[18])(b); dsb_note(B_STOP, r, b); return r; }
static HRESULT WINAPI hb_Lock(IDirectSoundBuffer *b, DWORD off, DWORD n, LPVOID *p1, LPDWORD n1, LPVOID *p2, LPDWORD n2, DWORD f)
{ void **o = dsb_orig_for(b); HRESULT r = ((BLock_t)o[11])(b, off, n, p1, n1, p2, n2, f); dsb_note(B_LOCK, r, b); return r; }
static HRESULT WINAPI hb_SetVolume(IDirectSoundBuffer *b, LONG v)
{
    static int n; void **o = dsb_orig_for(b); HRESULT r = ((BVol_t)o[15])(b, v); dsb_note(B_SETVOL, r, b);
    if (n++ < 20) plog("SoundBuffer::SetVolume(%p, %ld) = 0x%08lX", (void *)b, v, r);
    return r;
}
static HRESULT WINAPI hb_SetFormat(IDirectSoundBuffer *b, LPCWAVEFORMATEX w)
{
    void **o = dsb_orig_for(b); HRESULT r = ((BFmt_t)o[14])(b, w);
    if (w) plog("SoundBuffer::SetFormat(%p, tag=%u ch=%u %lu Hz %u-bit) = 0x%08lX", (void *)b, w->wFormatTag, w->nChannels, w->nSamplesPerSec, w->wBitsPerSample, r);
    return r;
}
static void dsb_hook(IDirectSoundBuffer *b)
{
    void **vt = *(void ***)b; DWORD old; int i;
    for (i = 0; i < dsb_nvt; i++) if (dsb_vt[i] == vt) return;
    if (dsb_nvt >= DS_MAXVT) return;
    i = dsb_nvt++; dsb_vt[i] = vt; memcpy(dsb_orig[i], vt, sizeof dsb_orig[i]);
    VirtualProtect(vt, 21 * sizeof(void *), PAGE_READWRITE, &old);
    vt[11] = fnptr(hb_Lock); vt[12] = fnptr(hb_Play); vt[14] = fnptr(hb_SetFormat); vt[15] = fnptr(hb_SetVolume); vt[18] = fnptr(hb_Stop);
    VirtualProtect(vt, 21 * sizeof(void *), old, &old);
    plog("sound buffer vtable %p hooked", (void *)vt);
}

typedef HRESULT (WINAPI *DSCSB_t)(IDirectSound *, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN);
typedef HRESULT (WINAPI *DSSCL_t)(IDirectSound *, HWND, DWORD);
static DSCSB_t real_DSCSB; static DSSCL_t real_DSSCL;
static HRESULT WINAPI hd_CreateSoundBuffer(IDirectSound *ds, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER *b, LPUNKNOWN o)
{
    static int n;
    HRESULT r = real_DSCSB(ds, d, b, o);
    if (d && (n++ < 120 || FAILED(r))) {
        if (d->lpwfxFormat)
            plog("IDirectSound::CreateSoundBuffer(flags=0x%lX bytes=%lu tag=%u ch=%u %lu Hz %u-bit) = 0x%08lX",
                 d->dwFlags, d->dwBufferBytes, d->lpwfxFormat->wFormatTag, d->lpwfxFormat->nChannels,
                 d->lpwfxFormat->nSamplesPerSec, d->lpwfxFormat->wBitsPerSample, r);
        else
            plog("IDirectSound::CreateSoundBuffer(flags=0x%lX bytes=%lu, no format%s) = 0x%08lX",
                 d->dwFlags, d->dwBufferBytes, (d->dwFlags & DSBCAPS_PRIMARYBUFFER) ? ", PRIMARY" : "", r);
    }
    if (SUCCEEDED(r) && b && *b) dsb_hook(*b);
    return r;
}
static HRESULT WINAPI hd_SetCooperativeLevel(IDirectSound *ds, HWND h, DWORD lvl)
{
    HRESULT r = real_DSSCL(ds, h, lvl);
    plog("IDirectSound::SetCooperativeLevel(hwnd=%p, level=%lu) = 0x%08lX", (void *)h, lvl, r);
    return r;
}
static HRESULT WINAPI hk_DirectSoundCreate(LPCGUID g, LPDIRECTSOUND *ds, LPUNKNOWN o)
{
    HRESULT r = real_DirectSoundCreate(g, ds, o);
    plog("DirectSoundCreate(guid=%p) = 0x%08lX", (void *)g, r);
    if (SUCCEEDED(r) && ds && *ds) {
        void **vt = *(void ***)*ds;
        hook_vtable(vt, 3, hd_CreateSoundBuffer, (void **)&real_DSCSB);
        hook_vtable(vt, 6, hd_SetCooperativeLevel, (void **)&real_DSSCL);
        DSCAPS c; memset(&c, 0, sizeof c); c.dwSize = sizeof c;
        if (SUCCEEDED(IDirectSound_GetCaps(*ds, &c)))
            plog("DirectSound caps: flags=0x%lX hw buffers=%lu free=%lu", c.dwFlags, c.dwMaxHwMixingAllBuffers, c.dwFreeHwMixingAllBuffers);
    }
    return r;
}


/* ---------- settings persistence (PersistSettings) ----------
 * The game's own save (0x439D20, run at shutdown) writes only Device, Window, SetWindowPos and ClipLevel.
 * It never saves Motion ([Config] Buffer, global 0x481D80), BG Disp (0x48ABD4), SeeThrough (0x48AC08) or the
 * joystick button/axis assignment, and only saves the keyboard keys from the Key Assign dialog.
 * We redirect the calls to the save (0x437E74) and load (0x439E80, called at 0x437F65) routines through our
 * own functions: run the game's code, then save / restore the rest. Joystick settings use the game's own
 * [Joystick] format and loader (0x412760, enabled by [Sections] Joystick=Enable).
 */
#define VA(x) ((BYTE *)(g_base + ((DWORD)(x) - 0x400000)))
typedef void (__cdecl *voidfn_t)(void);
static voidfn_t game_save, game_load;
static BOOL (WINAPI *real_WritePrivateProfileStringA)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);

static const char *game_ini(void) { return *(const char **)VA(0x48ABA4); }   /* ".\\DAYTONA.INI" */

static void ini_put(const char *sec, const char *key, const char *val)
{
    if (!real_WritePrivateProfileStringA(sec, key, val, game_ini()))
        plog("settings: write [%s] %s=%s FAILED, error %lu", sec, key, val ? val : "(delete)", GetLastError());
}
static void ini_puti(const char *sec, const char *key, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); ini_put(sec, key, b); }

static void save_extras(void)
{
    char full[MAX_PATH], *fp;
    int i, buf = *(int *)VA(0x481D80);
    const char **fnames = (const char **)VA(0x481FE8);          /* NO USE, ACCELERATE, BRAKE, ... (15) */
    const DWORD *pads = (const DWORD *)VA(0x482028);              /* {name, slot, mask} ... {0} */
    const int *slots = (const int *)VA(0x488C20);
    const char **keynames = (const char **)VA(0x48AC38);          /* LEFT RIGHT UP DOWN ACCEL BREAK V1-V4 */
    const WORD *keys = (const WORD *)VA(0xC58430);
    char kb[200]; int n = 0;

    if (buf == 2 || buf == 3) ini_puti("Config", "Buffer", buf);
    for (i = 0; i < 10; i++) { ini_puti("Input", keynames[i], keys[i]); n += snprintf(kb + n, sizeof kb - n, " %s=%u", keynames[i], keys[i]); }
    ini_put("Sections", "Joystick", "Enable");
    for (i = 0; pads[i * 3]; i++) {
        int f = slots[pads[i * 3 + 1]];
        if (f < 0 || f > 14) f = 0;
        ini_put("Joystick", (const char *)pads[i * 3], fnames[f]);
    }
    ini_puti("Joystick", "AXIS_UD", *(short *)VA(0x490638));
    ini_puti("Joystick", "AXIS_LR", *(short *)VA(0x49063A));
    ini_puti("Screen", "BGDisp", *(int *)VA(0x48ABD4));
    ini_puti("Screen", "SeeThrough", *(int *)VA(0x48AC08));
    GetFullPathNameA(game_ini(), MAX_PATH, full, &fp);
    plog("settings saved to %s: ClipLevel=%d Motion(Buffer)=%d BGDisp=%d SeeThrough=%d AXIS_UD=%d AXIS_LR=%d",
         full, *(int *)VA(0x48AC30), buf, *(int *)VA(0x48ABD4), *(int *)VA(0x48AC08), *(short *)VA(0x490638), *(short *)VA(0x49063A));
    plog("settings saved: keys%s", kb);
    n = 0;
    for (i = 0; pads[i * 3]; i++) { int f = slots[pads[i * 3 + 1]]; n += snprintf(kb + n, sizeof kb - n, " %s=%s", (const char *)pads[i * 3], fnames[(f < 0 || f > 14) ? 0 : f]); }
    plog("settings saved: buttons%s", kb);
}

/* Clean shutdown, from the game's own clean-up (0x437E50, also run after a crash in the game): our threads
   are stopped and the audio device closed while the process is still whole, not left to process exit. */
static void proxy_shutdown(void)
{
    static LONG done;
    if (InterlockedExchange(&done, 1)) return;
    DWORD t0 = GetTickCount();
    cda_shutdown();
    input_shutdown(true);
#ifdef ULT_RENDER_CAPTURE
    render_capture_shutdown();
#endif
    renderer3d_shutdown();
    plog("clean shutdown: music and controller threads stopped (%lu ms)", GetTickCount() - t0);
}

static void __cdecl hk_game_save(void)
{
    plog("game is saving its settings (shutdown)");
    game_save();
    if (cfg_persist) save_extras();
}

/* The game's clean-up closes the CD audio device (0x457110) right after saving: shut down after that. */
static voidfn_t game_cd_close;
static void __cdecl hk_game_cd_close(void)
{
    game_cd_close();
    proxy_shutdown();
}

static void __cdecl hk_game_load(void)
{
    char full[MAX_PATH], *fp;
    int v;
    input_apply_layout(game_ini(), g_proxy_ini);
    game_load();
    input_after_settings_loaded();
    cheats_after_settings_loaded();
    GetFullPathNameA(game_ini(), MAX_PATH, full, &fp);
    if (cfg_persist) {
        if ((v = GetPrivateProfileIntA("Screen", "BGDisp", -1, game_ini())) >= 0) *(int *)VA(0x48ABD4) = v ? 1 : 0;
        if ((v = GetPrivateProfileIntA("Screen", "SeeThrough", -1, game_ini())) >= 0) *(int *)VA(0x48AC08) = v ? 1 : 0;
    }
    plog("settings loaded from %s: ClipLevel=%d Motion(Buffer)=%d BGDisp=%d SeeThrough=%d Device=%d",
         full, *(int *)VA(0x48AC30), *(int *)VA(0x481D80), *(int *)VA(0x48ABD4), *(int *)VA(0x48AC08), *(WORD *)VA(0x48ABB0));
}

static int redirect_call_raw(DWORD site, DWORD expect_target, void *fn, voidfn_t *orig, const char *what)
{
    BYTE *p = VA(site); DWORD old; LONG rel;
    if (p[0] != 0xE8 || (DWORD)(site + 5 + *(LONG *)(p + 1)) != expect_target) { plog("redirect %s @%08lX: unexpected code, NOT applied", what, site); return 0; }
    *orig = (voidfn_t)VA(expect_target);
    rel = (LONG)((BYTE *)fn - (p + 5));
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(LONG *)(p + 1) = rel;
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    plog("redirect %s @%08lX: applied", what, site);
    return 1;
}


#ifdef ULT_DEBUG
/* ---------- frame-rate test ([Test] RaceLogicRate, RateLog) ----------
 * Table 0x4870C0 holds "vblanks per frame" per screen mode (1 = 60 Hz menus, 3 = 20 Hz races). The same
 * value indexes the scheduler's tick period (0x469A68: 16666 / 33333 / 50000 us), so rewriting the race
 * entries from 3 to 2 or 1 runs race logic at 30 or 60 Hz. RateLog samples the player car four times a
 * second so speed versus distance travelled can be compared between rates.
 */
static void apply_race_rate(void)
{
    BYTE *t = (BYTE *)VA(0x4870C0), nv; int i, n = 0; DWORD old;
    if (cfg_racerate == 20) return;
    nv = cfg_racerate == 60 ? 1 : cfg_racerate == 30 ? 2 : 0;
    if (!nv) { plog("RaceLogicRate=%d not supported (use 20, 30 or 60)", cfg_racerate); return; }
    VirtualProtect(t, 24, PAGE_READWRITE, &old);
    for (i = 0; i < 24; i++) if (t[i] == 3) { t[i] = nv; n++; }
    VirtualProtect(t, 24, old, &old);
    plog("RaceLogicRate=%d: %d race screen entries changed from 3 to %d vblanks per frame", cfg_racerate, n, nv);
}

static DWORD WINAPI rate_logger(LPVOID a)
{
    DWORD t0 = GetTickCount(), lastframes = 0, lastt = 0;
    int lastmode = -1;
    (void)a;
    for (;;) {
        Sleep(250);
        int mode = *(int *)VA(0x482230);
        DWORD frames = *(DWORD *)VA(0xC58384), now = GetTickCount() - t0;
        BYTE *car = *(BYTE **)VA(0x11B7A5C);
        if (mode != lastmode) { plog("rate: screen mode %d", mode); lastmode = mode; }
        if (car && !IsBadReadPtr(car, 0x100)) {
            double fps = (now > lastt) ? (frames - lastframes) * 1000.0 / (now - lastt) : 0;
            plog("rate: t=%6.2f s logic_fps=%5.1f speed=%5d pos=%11d %11d %11d time_left=%5d vblanks/frame=%d",
                 now / 1000.0, fps, *(int *)(car + 0xE4), *(int *)(car + 0x10), *(int *)(car + 0x14), *(int *)(car + 0x18),
                 *(short *)VA(0xC592D2), *(int *)VA(0xC0E090));
            plog("rate: input device=%u pad=%04x analogue_flag=%u accel_byte=%u steer=%04x mask_accel=%04x mask_brake=%04x",
                 *(WORD *)VA(0x48ABB0), *(WORD *)VA(0xC583E0), *(WORD *)VA(0xC583E6), *(BYTE *)VA(0xC58400),
                 *(WORD *)VA(0xC583E4), *(WORD *)VA(0x11B7268), *(WORD *)VA(0x11B727C));
        }
        lastframes = frames; lastt = now;
    }
    return 0;
}

#endif

template <typename F> static int redirect_call(DWORD site, DWORD expect_target, F fn, voidfn_t *orig, const char *what) { return redirect_call_raw(site, expect_target, fnptr(fn), orig, what); }

/* The Options screen in pages (options_menu.cpp): its page routine and the 30 drawing calls inside it.
   All sites are checked first and changed only if every one is the expected call. */
static void install_options_menu(void)
{
    struct Site { DWORD at, target; void *fn; } sites[31];
    int n = 0;
    sites[n++] = { 0x4320A8, 0x432170, options_menu_page() };
    for (DWORD a : kOptionsImageCalls) sites[n++] = { a, 0x42A3A0, options_menu_filter_image() };
    for (DWORD a : kOptionsTextCalls) sites[n++] = { a, 0x428E20, options_menu_filter_text() };
    for (int i = 0; i < n; i++) {
        BYTE *p = VA(sites[i].at);
        if (p[0] != 0xE8 || (DWORD)(sites[i].at + 5 + *(LONG *)(p + 1)) != sites[i].target) {
            plog("options screen: unexpected code at %08lX, original Options screen kept", sites[i].at);
            return;
        }
    }
    options_menu_init(g_base);
    for (int i = 0; i < n; i++) {
        BYTE *p = VA(sites[i].at); DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        *(LONG *)(p + 1) = (LONG)((BYTE *)sites[i].fn - (p + 5));
        VirtualProtect(p, 5, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), VA(0x432000), 0x1000);
    plog("options screen: in pages (Gameplay, Sound, Display, Controls, Cheats), %d calls redirected", n);
}

/* The game's settings save (shutdown) and load (start-up): our settings, the pad layout and the clean shutdown
   hang on these, so they are installed whatever PersistSettings says (it only decides the extra settings). */
static void install_settings_hooks(void)
{
    redirect_call(0x437E74, 0x439D20, hk_game_save, &game_save, "settings save");
    redirect_call(0x437F65, 0x439E80, hk_game_load, &game_load, "settings load");
    redirect_call(0x437E79, 0x457110, hk_game_cd_close, &game_cd_close, "clean-up (CD audio close)");
}

/* Menu tick marks for the settings we restore (the game only sets them when you click) */
static void sync_menu_checks(HMENU m)
{
    int buf = *(int *)VA(0x481D80);
    if (!m) return;
    if (buf == 2 || buf == 3) {
        CheckMenuItem(m, 0x1D1, MF_BYCOMMAND | (buf == 2 ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(m, 0x1D2, MF_BYCOMMAND | (buf == 3 ? MF_CHECKED : MF_UNCHECKED));
    }
    CheckMenuItem(m, 0x1D3, MF_BYCOMMAND | (*(int *)VA(0x48ABD4) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, 0x1D4, MF_BYCOMMAND | (*(int *)VA(0x48AC08) ? MF_CHECKED : MF_UNCHECKED));
}

/* Files copied from a CD keep the read-only attribute, which silently blocks every save */
static void fix_readonly(const char *name)
{
    char p[MAX_PATH]; DWORD a;
    snprintf(p, sizeof p, "%s%s", g_dir, name);
    a = GetFileAttributesA(p);
    if (a == INVALID_FILE_ATTRIBUTES) { plog("%s: not found", name); return; }
    if (!(a & FILE_ATTRIBUTE_READONLY)) { plog("%s: writable", name); return; }
    if (!cfg_fixro) { plog("%s: READ-ONLY, saves to it will fail (FixReadOnlySaves=0)", name); return; }
    if (SetFileAttributesA(p, a & ~FILE_ATTRIBUTE_READONLY)) plog("%s: was read-only, attribute cleared so settings can be saved", name);
    else plog("%s: READ-ONLY and the attribute could not be cleared (error %lu)", name, GetLastError());
}

static BOOL WINAPI hk_WritePrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR val, LPCSTR file)
{
    BOOL r = real_WritePrivateProfileStringA(sec, key, val, file);
    DWORD e = GetLastError();
    if (val && *val) plog("game writes %s [%s] %s=%s%s", file ? file : "", sec ? sec : "", key ? key : "", val, r ? "" : "  FAILED");
    if (!r) plog("   write failed, error %lu", e);
    SetLastError(e);
    return r;
}

static void resolve_real(void)
{
    HMODULE u = GetModuleHandleA("user32.dll"), k = GetModuleHandleA("kernel32.dll");
    set_fn(real_SetMenu, GetProcAddress(u, "SetMenu"));
    set_fn(real_GetMenu, GetProcAddress(u, "GetMenu"));
    set_fn(real_CreateDialogParamA, GetProcAddress(u, "CreateDialogParamA"));
    set_fn(real_ShowWindow, GetProcAddress(u, "ShowWindow"));
    set_fn(real_SetWindowPos, GetProcAddress(u, "SetWindowPos"));
    set_fn(real_ClipCursor, GetProcAddress(u, "ClipCursor"));
    set_fn(real_ShowCursor, GetProcAddress(u, "ShowCursor"));
    set_fn(real_OutputDebugStringA, GetProcAddress(k, "OutputDebugStringA"));
    set_fn(real_MessageBoxA, GetProcAddress(u, "MessageBoxA"));
    set_fn(real_PostQuitMessage, GetProcAddress(u, "PostQuitMessage"));
    set_fn(real_CreateFileA, GetProcAddress(k, "CreateFileA"));
    set_fn(real_WritePrivateProfileStringA, GetProcAddress(k, "WritePrivateProfileStringA"));
    HMODULE w = GetModuleHandleA("winmm.dll");
    if (w) {
        set_fn(real_mciSendCommandA, GetProcAddress(w, "mciSendCommandA"));
        set_fn(real_mciGetErrorStringA, GetProcAddress(w, "mciGetErrorStringA"));
    }
    HMODULE d = GetModuleHandleA("dsound.dll");
    if (d) set_fn(real_DirectSoundCreate, GetProcAddress(d, "DirectSoundCreate"));
}

static void install_user32_hooks(void)
{
    hook_iat("USER32.dll", "SetMenu", hk_SetMenu);
    hook_iat("USER32.dll", "GetMenu", hk_GetMenu);
    hook_iat("USER32.dll", "CreateDialogParamA", hk_CreateDialogParamA);
    hook_iat("USER32.dll", "ShowWindow", hk_ShowWindow);
    hook_iat("USER32.dll", "SetWindowPos", hk_SetWindowPos);
    if (cfg_owndd && real_ClipCursor) hook_iat("USER32.dll", "ClipCursor", hk_ClipCursor);
    if (cfg_owndd && real_ShowCursor) hook_iat("USER32.dll", "ShowCursor", hk_ShowCursor);
    if (input_active()) {
        hook_iat("WINMM.dll", "joyGetNumDevs", input_joyGetNumDevs);
        hook_iat("WINMM.dll", "joyGetDevCapsA", input_joyGetDevCapsA);
        hook_iat("WINMM.dll", "joyGetPosEx", input_joyGetPosEx);
    }
    hook_iat("KERNEL32.dll", "UnhandledExceptionFilter", crash_unhandled_exception_filter);
    if (real_mciSendCommandA && (g_log || cda_available())) hook_iat("WINMM.dll", "mciSendCommandA", hk_mciSendCommandA);
    if (g_log) {
        hook_iat("KERNEL32.dll", "OutputDebugStringA", hk_OutputDebugStringA);
        hook_iat("USER32.dll", "MessageBoxA", hk_MessageBoxA);
        hook_iat("USER32.dll", "PostQuitMessage", hk_PostQuitMessage);
        if (real_CreateFileA) hook_iat("KERNEL32.dll", "CreateFileA", hk_CreateFileA);
        hook_iat("KERNEL32.dll", "WritePrivateProfileStringA", hk_WritePrivateProfileStringA);
        if (real_DirectSoundCreate) hook_iat("DSOUND.dll", "DirectSoundCreate", hk_DirectSoundCreate);
    }
}

static void enter_fullscreen(HWND h)
{
    g_hwnd = h; g_fullscreen = 1;
    if (h && !g_origproc) g_origproc = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)game_subproc);
    if (h) {
        HMENU m = real_GetMenu(h);
        if (m) g_gamemenu = m;
        real_SetMenu(h, NULL);
        plog("full screen: menu %p detached", (void *)g_gamemenu);
    }
    if (cfg_loaddlg && g_loaddlg && IsWindow(g_loaddlg)) real_ShowWindow(g_loaddlg, SW_HIDE);
}

static void leave_fullscreen(HWND h)
{
    g_fullscreen = 0; g_inmenu = 0;
    if (h) g_hwnd = h;
    if (h && !g_origproc) g_origproc = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)game_subproc);
    plog("windowed mode");
}

/* Called by our own DirectDraw. Full screen without exclusive mode or a display-mode change:
   the game's borderless window is sized to cover its monitor and frames are scaled into it. */
static void apply_window_mode(HWND h, bool windowed)
{
    g_own_windowed = windowed;
    if (IsIconic(h)) {   /* started minimised (a shortcut or launcher set to "Minimised"): come back as the game */
        real_ShowWindow(h, SW_RESTORE);
        plog("display: window was minimised, restored");
    }
    if (windowed && real_ClipCursor) real_ClipCursor(nullptr);
    sync_cursor();
    if (!windowed) {
        SetWindowLongA(h, GWL_STYLE, (GetWindowLongA(h, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
        RECT m = monitor_rect(h);
        real_SetWindowPos(h, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        plog("display: full screen (borderless) %ldx%ld at %ld,%ld", m.right - m.left, m.bottom - m.top, m.left, m.top);
        return;
    }
    DWORD style = (GetWindowLongA(h, GWL_STYLE) & ~WS_POPUP) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    SetWindowLongA(h, GWL_STYLE, style);
    MONITORINFO mi; mi.cbSize = sizeof mi;
    RECT work = { 0, 0, 1024, 768 };
    if (GetMonitorInfoA(game_monitor(h), &mi)) work = mi.rcWork;
    int k = g_ddcfg.display.window_scale, cw, ch;
    RECT r;
    for (;; k--) {
        cw = 640 * (k > 0 ? k : 1); ch = 480 * (k > 0 ? k : 1);
        r = { 0, 0, cw, ch };
        AdjustWindowRect(&r, style, FALSE);
        if (k <= 1 || (r.right - r.left <= work.right - work.left && r.bottom - r.top <= work.bottom - work.top)) break;
    }
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = work.left + ((work.right - work.left) - ww) / 2, y = work.top + ((work.bottom - work.top) - wh) / 2;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    real_SetWindowPos(h, HWND_NOTOPMOST, x, y, ww, wh, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    plog("display: windowed, client %dx%d", cw, ch);
}

/* Display settings from the options screen: applied at once and saved to ddraw_proxy.ini. */
DisplayConfig proxy_display_get() { return g_ddcfg.display; }

/* MPH / KM/H from the Options GAMEPLAY page, saved in [Gameplay] Speed */
void proxy_speed_set(bool kmh)
{
    speed_kmh_set(kmh);
    WritePrivateProfileStringA("Gameplay", "Speed", kmh ? "KMH" : "MPH", g_proxy_ini);
}

/* RENDERER from the Options DISPLAY page, saved in [Display] Renderer3D */
void proxy_renderer3d_set(Renderer3D mode)
{
    if (mode == renderer3d_get()) return;
    renderer3d_set(mode);
    WritePrivateProfileStringA("Display", "Renderer3D", mode == Renderer3D::New ? "New" : "Original", g_proxy_ini);
}

/* SEETHRU LOOK from the Options DISPLAY page, saved in [Display] SeeThroughLook */
void proxy_seethrough_look_set(SeeThroughLook look)
{
    if (look == renderer3d_seethrough_get()) return;
    renderer3d_seethrough_set(look);
    WritePrivateProfileStringA("Display", "SeeThroughLook", look == SeeThroughLook::Outline ? "Outline" : "Textured", g_proxy_ini);
}

/* RESOLUTION from the Options DISPLAY page, saved in [Display] Resolution3D */
void proxy_resolution3d_set(int res)
{
    if (res == renderer3d_resolution_get()) return;
    renderer3d_resolution_set(res);
    static const char *const names[5] = { "Display", "640x400", "2x", "3x", "4x" };
    WritePrivateProfileStringA("Display", "Resolution3D", names[res >= 0 && res <= 4 ? res : 0], g_proxy_ini);
}

/* FILTER from the Options DISPLAY page, saved in [Display] TextureFilter */
void proxy_filter_set(int f)
{
    if (f == renderer3d_filter_get()) return;
    renderer3d_filter_set(f);
    WritePrivateProfileStringA("Display", "TextureFilter", kFilterNames[f >= 0 && f <= 4 ? f : 3], g_proxy_ini);
}

/* ANTIALIAS from the Options DISPLAY page, saved in [Display] Antialiasing */
/* MAPPING from the Options DISPLAY page, saved in [Display] Mapping */
/* DEPTH BUFFER from the Options DISPLAY page, saved in [Display] DepthBuffer */
void proxy_depth_set(bool on)
{
    if (on == renderer3d_depth_get()) return;
    renderer3d_depth_set(on);
    WritePrivateProfileStringA("Display", "DepthBuffer", on ? "On" : "Off", g_proxy_ini);
}

void proxy_mapping_set(int m)
{
    if (m == renderer3d_mapping_get()) return;
    renderer3d_mapping_set(m);
    static const char *const names[3] = { "PC", "Saturn", "Perspective" };
    WritePrivateProfileStringA("Display", "Mapping", names[renderer3d_mapping_get()], g_proxy_ini);
}

void proxy_msaa_set(int samples)
{
    if (samples == renderer3d_msaa_get()) return;
    renderer3d_msaa_set(samples);
    const int s = renderer3d_msaa_get();
    WritePrivateProfileStringA("Display", "Antialiasing", s == 1 ? "Off" : s == 2 ? "2x" : s == 8 ? "8x" : "4x", g_proxy_ini);
}

/* HUD SCALE and HUD FILTER from the Options DISPLAY page, saved in [Display] HudScale and HudFilter */
void proxy_hud_scale_set(int pct)
{
    if (pct == renderer3d_hud_scale_get()) return;
    renderer3d_hud_scale_set(pct);
    char v[8]; wsprintfA(v, "%d", renderer3d_hud_scale_get());
    WritePrivateProfileStringA("Display", "HudScale", v, g_proxy_ini);
}

void proxy_hud_filter_set(int f)
{
    if (f == renderer3d_hud_filter_get()) return;
    renderer3d_hud_filter_set(f);
    WritePrivateProfileStringA("Display", "HudFilter", kHudFilterNames[renderer3d_hud_filter_get()], g_proxy_ini);
}

/* CRT from the Options DISPLAY page, saved in [Display] Crt */
void proxy_crt_set(int mode)
{
    present_set_crt(mode);
    WritePrivateProfileStringA("Display", "Crt", kCrtNames[present_crt()], g_proxy_ini);
}

/* FAR / MAX from the Options DISPLAY page, saved in [Display] DrawDistance */
void proxy_draw_distance_set(DrawDistance level)
{
    if (level == draw_distance_get()) return;
    draw_distance_set(level);
    WritePrivateProfileStringA("Display", "DrawDistance", level == DrawDistance::Max ? "Max" : level == DrawDistance::Far ? "Far" : "Game", g_proxy_ini);
}

void proxy_display_set(const DisplayConfig &d)
{
    DisplayConfig old = g_ddcfg.display;
    g_ddcfg.display = d;
    own_display_config(d);
    HWND h = *(HWND *)VA(0xC58488);
    if (d.windowed != old.windowed) {
        if (g_own_fs && h && (bool)g_own_windowed != d.windowed) apply_window_mode(h, d.windowed);
        WritePrivateProfileStringA("Display", "Mode", d.windowed ? "Windowed" : "Fullscreen", g_proxy_ini);
    }
    if (d.scaling != old.scaling)
        WritePrivateProfileStringA("Display", "Scaling", d.scaling == Scaling::Nearest ? "Nearest" : d.scaling == Scaling::Integer ? "Integer" : "SharpBilinear", g_proxy_ini);
    if (d.picture != old.picture)
        WritePrivateProfileStringA("Display", "Picture", d.picture == Picture::Fill ? "4:3" : d.picture == Picture::Frame ? "Frame" : "16:10", g_proxy_ini);
    if (d.vsync != old.vsync) WritePrivateProfileStringA("Display", "VSync", d.vsync ? "1" : "0", g_proxy_ini);
    if (h) InvalidateRect(h, nullptr, TRUE);   /* the area around the picture is redrawn for the new placement */
}

/* Alt+Enter: switch between full screen and a window. */
/* Back in the game: if the window was minimised or left at another size (the game started minimised), put it back
   as full screen or as the game window, as the settings say. */
static void ensure_game_placement(HWND h)
{
    if (!g_own_fs || !h) return;
    bool wrong = IsIconic(h);
    if (!wrong && !g_own_windowed) {
        RECT m = monitor_rect(h), r;
        GetWindowRect(h, &r);
        wrong = !EqualRect(&m, &r);
    }
    if (!wrong) return;
    plog("display: game window was minimised or resized, put back");
    apply_window_mode(h, g_own_windowed);
}

static void toggle_window_mode(HWND h)
{
    if (!g_own_fs) return;
    apply_window_mode(h, !g_own_windowed);
    g_ddcfg.display.windowed = g_own_windowed;   /* so the options screen shows the current mode */
}

void proxy_on_cooperative_level(HWND h, DWORD flags)
{
    crash_set_window(h);
    if (flags & DDSCL_FULLSCREEN) {
        enter_fullscreen(h);
        bool first = !g_own_fs;
        g_own_fs = 1;
        apply_window_mode(h, first ? g_ddcfg.display.windowed : (bool)g_own_windowed);
    } else {
        g_own_fs = 0;
        leave_fullscreen(h);
    }
}

/* ---------- IDirectDrawSurface vtable hooks (diagnostics) ---------- */
enum { S_BLT, S_BLTFAST, S_FLIP, S_LOCK, S_UNLOCK, S_GETDC, S_SETPAL, S_RESTORE, S_N };
static const char *s_names[S_N] = { "Blt", "BltFast", "Flip", "Lock", "Unlock", "GetDC", "SetPalette", "Restore" };
static volatile LONG s_calls[S_N], s_fails[S_N];
static LONG s_faillogged[S_N];
static DWORD s_lastsum;
static void *s_real[36];

static void surf_note(int i, HRESULT r, void *surf)
{
    InterlockedIncrement(&s_calls[i]);
    if (FAILED(r)) {
        InterlockedIncrement(&s_fails[i]);
        if (s_faillogged[i] < 15) { s_faillogged[i]++; plog("Surface::%s(%p) FAILED 0x%08lX", s_names[i], surf, r); }
    }
    DWORD now = GetTickCount();
    if (now - s_lastsum >= 1000) {
        char b[400]; int n = 0, k;
        s_lastsum = now;
        for (k = 0; k < S_N; k++) {
            LONG c = InterlockedExchange(&s_calls[k], 0), f = InterlockedExchange(&s_fails[k], 0);
            if (c) n += snprintf(b + n, sizeof b - n, " %s=%ld", s_names[k], c);
            if (f) n += snprintf(b + n, sizeof b - n, "(fail %ld)", f);
        }
        plog("per second:%s  active=%d", n ? b : " no surface calls", *(int *)(g_base + (0x48ABDC - 0x400000)));
    }
}

typedef HRESULT (WINAPI *Blt_t)(IDirectDrawSurface *, LPRECT, IDirectDrawSurface *, LPRECT, DWORD, LPDDBLTFX);
typedef HRESULT (WINAPI *BltFast_t)(IDirectDrawSurface *, DWORD, DWORD, IDirectDrawSurface *, LPRECT, DWORD);
typedef HRESULT (WINAPI *Flip_t)(IDirectDrawSurface *, IDirectDrawSurface *, DWORD);
typedef HRESULT (WINAPI *Lock_t)(IDirectDrawSurface *, LPRECT, LPDDSURFACEDESC, DWORD, HANDLE);
typedef HRESULT (WINAPI *Unlock_t)(IDirectDrawSurface *, LPVOID);
typedef HRESULT (WINAPI *GetDC_t)(IDirectDrawSurface *, HDC *);
typedef HRESULT (WINAPI *SetPal_t)(IDirectDrawSurface *, IDirectDrawPalette *);
typedef HRESULT (WINAPI *Restore_t)(IDirectDrawSurface *);
typedef HRESULT (WINAPI *GetAttached_t)(IDirectDrawSurface *, LPDDSCAPS, LPDIRECTDRAWSURFACE *);

static HRESULT WINAPI hs_Blt(IDirectDrawSurface *s, LPRECT d, IDirectDrawSurface *src, LPRECT sr, DWORD f, LPDDBLTFX fx)
{ HRESULT r = ((Blt_t)s_real[5])(s, d, src, sr, f, fx); surf_note(S_BLT, r, s); return r; }
static HRESULT WINAPI hs_BltFast(IDirectDrawSurface *s, DWORD x, DWORD y, IDirectDrawSurface *src, LPRECT sr, DWORD f)
{ HRESULT r = ((BltFast_t)s_real[7])(s, x, y, src, sr, f); surf_note(S_BLTFAST, r, s); return r; }
static int g_mode_h = 480;
static HRESULT WINAPI hs_Flip(IDirectDrawSurface *s, IDirectDrawSurface *t, DWORD f)
{
    /* the frame being shown, through the game's own back-buffer pointer and pitch (for comparison with our DirectDraw) */
    BYTE *bb = *(BYTE **)(g_base + (0xC584F8 - 0x400000));
    WORD pitch = *(WORD *)(g_base + (0x4D4F34 - 0x400000));
    if (bb && pitch) frame_capture(bb, 640, g_mode_h, pitch);
    HRESULT r = ((Flip_t)s_real[11])(s, t, f); surf_note(S_FLIP, r, s); return r;
}
static int s_lockinfo;
static HRESULT WINAPI hs_Lock(IDirectDrawSurface *s, LPRECT rc, LPDDSURFACEDESC d, DWORD f, HANDLE e)
{
    HRESULT r = ((Lock_t)s_real[25])(s, rc, d, f, e); surf_note(S_LOCK, r, s);
    if (SUCCEEDED(r) && d && s_lockinfo < 6) {
        s_lockinfo++;
        plog("Lock(%p) %lux%lu pitch=%ld bpp=%lu masks R=%04lX G=%04lX B=%04lX caps=0x%lX", (void *)s,
             d->dwWidth, d->dwHeight, d->lPitch, d->ddpfPixelFormat.dwRGBBitCount,
             d->ddpfPixelFormat.dwRBitMask, d->ddpfPixelFormat.dwGBitMask, d->ddpfPixelFormat.dwBBitMask,
             d->ddsCaps.dwCaps);
    }
    return r;
}
static HRESULT WINAPI hs_Unlock(IDirectDrawSurface *s, LPVOID p)
{ HRESULT r = ((Unlock_t)s_real[32])(s, p); surf_note(S_UNLOCK, r, s); return r; }
static HRESULT WINAPI hs_GetDC(IDirectDrawSurface *s, HDC *dc)
{ HRESULT r = ((GetDC_t)s_real[17])(s, dc); surf_note(S_GETDC, r, s); return r; }
static HRESULT WINAPI hs_SetPalette(IDirectDrawSurface *s, IDirectDrawPalette *p)
{ HRESULT r = ((SetPal_t)s_real[31])(s, p); surf_note(S_SETPAL, r, s); return r; }
static HRESULT WINAPI hs_Restore(IDirectDrawSurface *s)
{ HRESULT r = ((Restore_t)s_real[27])(s); surf_note(S_RESTORE, r, s); plog("Surface::Restore(%p) = 0x%08lX", (void *)s, r); return r; }


static HRESULT WINAPI hs_GetAttachedSurface(IDirectDrawSurface *s, LPDDSCAPS caps, LPDIRECTDRAWSURFACE *out)
{
    HRESULT r = ((GetAttached_t)s_real[12])(s, caps, out);
    if (SUCCEEDED(r) && out && *out && trace_enabled()) {
        char nm[64]; snprintf(nm, sizeof nm, "attached to %p, caps 0x%lX", (void *)s, caps ? caps->dwCaps : 0);
        trace_name_object(*out, nm);
    }
    return r;
}

static void hook_surface_vtable(IDirectDrawSurface *s)
{
    static void **hooked_vt;
    void **vt = *(void ***)s;
    if (vt == hooked_vt) return;
    if (hooked_vt) { plog("surface %p uses a different vtable %p (not hooked)", (void *)s, (void *)vt); return; }
    hooked_vt = vt;
    hook_vtable(vt, 5, hs_Blt, &s_real[5]);
    hook_vtable(vt, 7, hs_BltFast, &s_real[7]);
    hook_vtable(vt, 11, hs_Flip, &s_real[11]);
    hook_vtable(vt, 12, hs_GetAttachedSurface, &s_real[12]);
    hook_vtable(vt, 17, hs_GetDC, &s_real[17]);
    hook_vtable(vt, 25, hs_Lock, &s_real[25]);
    hook_vtable(vt, 27, hs_Restore, &s_real[27]);
    hook_vtable(vt, 31, hs_SetPalette, &s_real[31]);
    hook_vtable(vt, 32, hs_Unlock, &s_real[32]);
    plog("surface vtable %p hooked", (void *)vt);
}

/* ---------- IDirectDraw vtable hooks (logging + full-screen tracking) ---------- */
typedef HRESULT (WINAPI *SCL_t)(IDirectDraw *, HWND, DWORD);
typedef HRESULT (WINAPI *SDM_t)(IDirectDraw *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *CS_t)(IDirectDraw *, LPDDSURFACEDESC, LPDIRECTDRAWSURFACE *, IUnknown *);
static SCL_t real_SCL; static SDM_t real_SDM; static CS_t real_CS;

static HRESULT WINAPI hk_SetCooperativeLevel(IDirectDraw *dd, HWND h, DWORD f)
{
    HRESULT r = real_SCL(dd, h, f);
    plog("IDirectDraw::SetCooperativeLevel(hwnd=%p, flags=0x%lX) = 0x%08lX", (void *)h, f, r);
    if (SUCCEEDED(r)) {
        if (f & DDSCL_FULLSCREEN) enter_fullscreen(h); else leave_fullscreen(h);
    }
    return r;
}

static HRESULT WINAPI hk_SetDisplayMode(IDirectDraw *dd, DWORD w, DWORD h, DWORD bpp)
{
    HRESULT r = real_SDM(dd, w, h, bpp);
    if (SUCCEEDED(r)) g_mode_h = (int)h;
    plog("IDirectDraw::SetDisplayMode(%lux%lux%lu) = 0x%08lX", w, h, bpp, r);
    return r;
}

static HRESULT WINAPI hk_CreateSurface(IDirectDraw *dd, LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE *s, IUnknown *o)
{
    HRESULT r = real_CS(dd, d, s, o);
    if (d) plog("IDirectDraw::CreateSurface(flags=0x%lX caps=0x%lX %lux%lu backbuffers=%lu) = 0x%08lX -> %p",
                d->dwFlags, d->ddsCaps.dwCaps,
                (d->dwFlags & DDSD_WIDTH) ? d->dwWidth : 0, (d->dwFlags & DDSD_HEIGHT) ? d->dwHeight : 0,
                (d->dwFlags & DDSD_BACKBUFFERCOUNT) ? d->dwBackBufferCount : 0, r, (s && SUCCEEDED(r)) ? (void *)*s : NULL);
    if (SUCCEEDED(r) && s && *s) {
        hook_surface_vtable(*s);
        if (trace_enabled()) {
            char nm[96];
            snprintf(nm, sizeof nm, "%s %lux%lu caps 0x%lX", (d && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) ? "primary" : "surface",
                     (d && (d->dwFlags & DDSD_WIDTH)) ? d->dwWidth : 0, (d && (d->dwFlags & DDSD_HEIGHT)) ? d->dwHeight : 0,
                     d ? d->ddsCaps.dwCaps : 0);
            trace_name_object(*s, nm);
            trace_surface(*s);
        }
        static int pf_logged;
        if (d && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) && !pf_logged++) {
            DDPIXELFORMAT pf; DDSCAPS caps;
            memset(&pf, 0, sizeof pf); pf.dwSize = sizeof pf;
            if (SUCCEEDED(IDirectDrawSurface_GetPixelFormat(*s, &pf)))
                plog("primary pixel format: flags=0x%lX bpp=%lu R=%04lX G=%04lX B=%04lX",
                     pf.dwFlags, pf.dwRGBBitCount, pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask);
            if (SUCCEEDED(IDirectDrawSurface_GetCaps(*s, &caps))) plog("primary caps: 0x%lX", caps.dwCaps);
        }
    }
    return r;
}

static void hook_vtable_raw(void **vt, int idx, void *fn, void **orig)
{
    DWORD old;
    if (trace_retarget(vt[idx], fn, orig)) return;   /* our hook goes behind the trace stub */
    if (vt[idx] == fn) return;
    *orig = vt[idx];
    VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &old);
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
}

/* ---------- export ---------- */
typedef HRESULT (WINAPI *DDC_t)(GUID *, LPDIRECTDRAW *, IUnknown *);
static DDC_t real_DDC;
static DWORD (WINAPI *real_GetVersion)(void);
static DWORD WINAPI hk_GetVersion_unsupported(void)
{
    exe_unsupported_message(g_exe_sha1);
    return real_GetVersion();
}

static HRESULT system_directdraw_create(GUID *guid, LPDIRECTDRAW *dd, IUnknown *outer)
{
    if (!real_DDC) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        lstrcatA(path, "\\ddraw.dll");
        HMODULE m = LoadLibraryA(path);
        if (m) set_fn(real_DDC, GetProcAddress(m, "DirectDrawCreate"));
        plog("system ddraw: %s -> %p", path, (void *)real_DDC);
        if (!real_DDC) return DDERR_GENERIC;
    }
    return real_DDC(guid, dd, outer);
}

extern "C" HRESULT WINAPI proxy_DirectDrawCreate(GUID *guid, LPDIRECTDRAW *dd, IUnknown *outer)
{
    if (g_exe != ExeCheck::Supported) {
        /* another program or another version of the game: no changes at all */
        if (g_exe == ExeCheck::OtherVersion) exe_unsupported_message(g_exe_sha1);
        HRESULT r = system_directdraw_create(guid, dd, outer);
        plog("DirectDrawCreate passed to Windows unchanged = 0x%08lX", r);
        return r;
    }
    crash_preload();
    health_start();
    if (cfg_owndd) {
        install_user32_hooks();
        HRESULT r = own_directdraw_create(dd, g_ddcfg);
        plog("DirectDrawCreate(guid=%p) -> our own DirectDraw = 0x%08lX", (void *)guid, r);
        if (SUCCEEDED(r)) { trace_name_object(*dd, "own DirectDraw"); trace_directdraw(*dd); }
        return r;
    }
    install_user32_hooks();     /* idempotent; re-applied once all imports are certainly bound */
    GUID *use = guid;
    if (cfg_emul && guid == NULL) use = (GUID *)DDCREATE_EMULATIONONLY;
    HRESULT r = system_directdraw_create(use, dd, outer);
    plog("DirectDrawCreate(guid=%p%s) = 0x%08lX", (void *)guid,
         use == (GUID *)DDCREATE_EMULATIONONLY ? " -> EMULATIONONLY" : "", r);
    if (SUCCEEDED(r) && dd && *dd) {
        void **vt = *(void ***)*dd;
        hook_vtable(vt, 20, hk_SetCooperativeLevel, (void **)&real_SCL);
        hook_vtable(vt, 21, hk_SetDisplayMode, (void **)&real_SDM);
        hook_vtable(vt, 6, hk_CreateSurface, (void **)&real_CS);
        trace_name_object(*dd, "DirectDraw");
        trace_directdraw(*dd);
    }
    return r;
}

/* Per-monitor DPI awareness, so Windows display scaling (125 %, 150 % ...) does not blur or misplace the game:
   our window sizes and the swap chain are then in real pixels. Must happen before the game makes its window. */
static void set_dpi_awareness()
{
    HMODULE u = GetModuleHandleA("user32.dll");
    BOOL (WINAPI *set_ctx)(HANDLE) = nullptr;
    set_fn(set_ctx, GetProcAddress(u, "SetProcessDpiAwarenessContext"));
    if (set_ctx && set_ctx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) { plog("display: per-monitor DPI aware (v2)"); return; }
    BOOL (WINAPI *set_aware)() = nullptr;
    set_fn(set_aware, GetProcAddress(u, "SetProcessDPIAware"));
    if (set_aware && set_aware()) plog("display: DPI aware (system)");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logcs);
        g_base = (BYTE *)GetModuleHandleA(NULL);
        g_exe = exe_check(g_exe_sha1);
        if (g_exe == ExeCheck::OtherProgram) return TRUE;   /* not the game: nothing at all, no log */
        set_paths(inst);
        std::string migrated;
        if (g_exe == ExeCheck::Supported) migrated = settings_migrate(g_proxy_ini, PROXY_VERSION_STR);
        load_config(inst);
        if (g_exe != ExeCheck::Supported) {
            plog("DAYTONA.EXE is not v1.1 (SHA-1 %s, expected %s): nothing changed, DirectDraw goes straight to Windows",
                 g_exe_sha1[0] ? g_exe_sha1 : "unreadable", kSupportedExeSha1);
            /* the message comes at the game's first call into Windows (its C runtime's GetVersion), before
               anything of the game shows; not from here (no message boxes while Windows loads DLLs) */
            set_fn(real_GetVersion, GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetVersion"));
            if (real_GetVersion) hook_iat("KERNEL32.dll", "GetVersion", hk_GetVersion_unsupported);
            return TRUE;
        }
        plog("DAYTONA.EXE v1.1 (SHA-1 %s)", g_exe_sha1);
        if (!migrated.empty()) plog("%s", migrated.c_str());
        crash_init(g_dir, g_log_path, g_base);
        resolve_real();
        if (cfg_owndd) set_dpi_awareness();
        if (cfg_skipcd) apply_cd_patch();
        if (cfg_owndd) apply_no_rebuild_patch();
        input_init(g_incfg, g_base);
        input_install_bindings();
        if (cfg_cda) cda_init(g_dir, cfg_musvol); else plog("cdaudio: emulation disabled in ddraw_proxy.ini");
        fix_readonly("DAYTONA.INI");
        fix_readonly("DAYTONA.DAT");
        install_settings_hooks();
        if (!cfg_origopts) install_options_menu();
        draw_distance_install(g_base, cfg_drawdist, cfg_ddlog, cfg_ddfar, cfg_ddmax);
        pause_menu_install(g_base);
        main_menu_install(g_base, cfg_kmh);
        if (!cfg_origopts) cheats_install(g_base, g_proxy_ini);
#ifdef ULT_DEBUG
        apply_race_rate();
        if (cfg_ratelog) CloseHandle(CreateThread(NULL, 0, rate_logger, NULL, 0, NULL));
        crash_test_config(cfg_crashtest);
#endif
#ifdef ULT_RENDER_CAPTURE
        render_capture_install(g_base, g_dir, g_exe_sha1, cfg_rcap);
#endif
        renderer3d_install(g_base, cfg_r3d, cfg_stlook, cfg_res3d, cfg_filter, cfg_msaa, cfg_hud_pct, cfg_hud_filter);
        renderer3d_mapping_set(cfg_mapping);
        renderer3d_depth_set(cfg_depth);
        install_user32_hooks();
        trace_init(g_base, cfg_trace && g_log);
        if (cfg_owndd) SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);   /* no sleep while playing */
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_exe != ExeCheck::Supported) { if (g_log) fclose(g_log); return TRUE; }
        trace_shutdown();
        input_shutdown(false);
        if (cfg_owndd) SetThreadExecutionState(ES_CONTINUOUS);
        plog("unloading");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
