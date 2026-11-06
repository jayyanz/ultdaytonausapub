/* API trace for the logging pass ([Test] TraceCalls=1).
 *
 * Counts every call the game makes through its import table (USER32, GDI32, WINMM, MSACM32, DDRAW,
 * DSOUND and a short list of KERNEL32 functions) and through the DirectDraw object and surface
 * interfaces, with the calling address and the first argument (the object for interface methods,
 * for example the key code for GetAsyncKeyState). A summary is written for every visit to a game
 * screen (screen mode), and a full report when the game exits.
 *
 * Each traced entry point gets a small machine-code stub that records the call and then jumps on to
 * whatever the entry pointed at before, so the game's behaviour is unchanged. Our own typed hooks may
 * already sit behind a stub; trace_retarget() keeps the chain intact when a hook is (re)installed.
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "proxy.h"
#include "trace.h"

namespace {

constexpr int kMaxEntries = 512;
constexpr int kStubSize = 32;
constexpr int kCallers = 8;
constexpr int kArgs = 16;

enum class Kind { Import, Method };

struct Slot { volatile LONG used; void *value; volatile LONG count; };

struct Entry {
    void *orig;                 /* where the stub jumps: read by the stub through an absolute pointer */
    char name[64];
    Kind kind;
    volatile LONG count;
    LONG visit_base;
    Slot callers[kCallers];
    Slot args[kArgs];
    volatile LONG args_overflow;
    bool keypoll;               /* GetAsyncKeyState: argument is a virtual-key code */
};

Entry g_entries[kMaxEntries];
int g_nentries;
BYTE *g_stubs;
BYTE *g_base;
bool g_on;
CRITICAL_SECTION g_cs;

/* per-visit state, reset by the reporter */
volatile LONG g_vkeys[256];
volatile LONG g_msgs[0x400];
volatile LONG g_msg_other;

/* joystick observations per visit */
struct JoyVisit {
    volatile LONG calls, fails;
    DWORD flags_or;
    DWORD flags_seen[8]; int nflags;
    DWORD minv[6], maxv[6]; bool any;
    DWORD buttons_or, pov_seen[8]; int npov;
    int ids_mask;
};
JoyVisit g_joy;
CRITICAL_SECTION g_joycs;

struct Named { void *ptr; char name[96]; };
Named g_named[64]; int g_nnamed;

int screen_mode() { return *(int *)(g_base + (0x482230 - 0x400000)); }
DWORD logic_frames() { return *(DWORD *)(g_base + (0xC58384 - 0x400000)); }
int input_device() { return *(WORD *)(g_base + (0x48ABB0 - 0x400000)); }

void note_slot(Slot *s, int n, void *v, volatile LONG *overflow)
{
    for (int i = 0; i < n; i++) {
        if (s[i].used && s[i].value == v) { InterlockedIncrement(&s[i].count); return; }
        if (!s[i].used) {
            if (InterlockedCompareExchange(&s[i].used, 2, 0) == 0) {
                s[i].value = v; s[i].count = 1; InterlockedExchange(&s[i].used, 1); return;
            }
            while (s[i].used == 2) {}
            if (s[i].value == v) { InterlockedIncrement(&s[i].count); return; }
        }
    }
    if (overflow) InterlockedIncrement(overflow);
}

void __cdecl trace_record(int id, void *ret, void *arg1)
{
    DWORD le = GetLastError();
    Entry &e = g_entries[id];
    InterlockedIncrement(&e.count);
    note_slot(e.callers, kCallers, ret, nullptr);
    note_slot(e.args, kArgs, arg1, &e.args_overflow);
    if (e.keypoll) InterlockedIncrement(&g_vkeys[(DWORD_PTR)arg1 & 0xFF]);
    SetLastError(le);
}

void *make_stub(int id)
{
    BYTE *p = g_stubs + id * kStubSize;
    Entry &e = g_entries[id];
    BYTE *q = p;
    *q++ = 0x60;                                            /* pushad */
    *q++ = 0xFF; *q++ = 0x74; *q++ = 0x24; *q++ = 0x24;     /* push [esp+24h]  (first argument) */
    *q++ = 0xFF; *q++ = 0x74; *q++ = 0x24; *q++ = 0x24;     /* push [esp+24h]  (return address) */
    *q++ = 0x68; *(DWORD *)q = (DWORD)id; q += 4;           /* push id */
    *q++ = 0xE8; *(LONG *)q = (LONG)((BYTE *)fnptr(trace_record) - (q + 4)); q += 4;   /* call trace_record */
    *q++ = 0x83; *q++ = 0xC4; *q++ = 0x0C;                  /* add esp, 12 */
    *q++ = 0x61;                                            /* popad */
    *q++ = 0xFF; *q++ = 0x25; *(void ***)q = &e.orig; q += 4;   /* jmp [e.orig] */
    return p;
}

int new_entry(const char *name, Kind kind, void *orig)
{
    if (g_nentries >= kMaxEntries) return -1;
    int id = g_nentries++;
    Entry &e = g_entries[id];
    memset(&e, 0, sizeof e);
    snprintf(e.name, sizeof e.name, "%s", name);
    e.kind = kind;
    e.orig = orig;
    e.keypoll = !strcmp(name, "USER32!GetAsyncKeyState");
    return id;
}

bool is_stub(void *p) { return g_stubs && (BYTE *)p >= g_stubs && (BYTE *)p < g_stubs + kMaxEntries * kStubSize; }
Entry &stub_entry(void *p) { return g_entries[((BYTE *)p - g_stubs) / kStubSize]; }

void write_ptr(void **where, void *v)
{
    DWORD old;
    VirtualProtect(where, sizeof(void *), PAGE_READWRITE, &old);
    *where = v;
    VirtualProtect(where, sizeof(void *), old, &old);
}

/* ---------- joystick detail hooks ---------- */
UINT (WINAPI *real_joyGetDevCapsA)(UINT_PTR, LPJOYCAPSA, UINT);
MMRESULT (WINAPI *real_joyGetPosEx)(UINT, LPJOYINFOEX);

UINT WINAPI hk_joyGetDevCapsA(UINT_PTR id, LPJOYCAPSA c, UINT n)
{
    UINT r = real_joyGetDevCapsA(id, c, n);
    static UINT last_r[16] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                               0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    UINT k = (UINT)id & 15;
    if (last_r[k] != r) {
        last_r[k] = r;
        if (r == JOYERR_NOERROR && c)
            plog("trace: joyGetDevCaps(%u) '%s' axes %u of %u, buttons %u, caps 0x%X (Z %d R %d U %d V %d POV %d), "
                 "X %u-%u Y %u-%u Z %u-%u R %u-%u U %u-%u V %u-%u",
                 (unsigned)id, c->szPname, c->wNumAxes, c->wMaxAxes, c->wNumButtons, c->wCaps,
                 !!(c->wCaps & JOYCAPS_HASZ), !!(c->wCaps & JOYCAPS_HASR), !!(c->wCaps & JOYCAPS_HASU),
                 !!(c->wCaps & JOYCAPS_HASV), !!(c->wCaps & JOYCAPS_HASPOV),
                 c->wXmin, c->wXmax, c->wYmin, c->wYmax, c->wZmin, c->wZmax, c->wRmin, c->wRmax,
                 c->wUmin, c->wUmax, c->wVmin, c->wVmax);
        else
            plog("trace: joyGetDevCaps(%u) = %u", (unsigned)id, r);
    }
    return r;
}

MMRESULT WINAPI hk_joyGetPosEx(UINT id, LPJOYINFOEX ji)
{
    DWORD asked = ji ? ji->dwFlags : 0;
    MMRESULT r = real_joyGetPosEx(id, ji);
    EnterCriticalSection(&g_joycs);
    g_joy.calls++;
    g_joy.ids_mask |= 1 << (id & 15);
    g_joy.flags_or |= asked;
    bool seen = false;
    for (int i = 0; i < g_joy.nflags; i++) if (g_joy.flags_seen[i] == asked) seen = true;
    if (!seen && g_joy.nflags < 8) g_joy.flags_seen[g_joy.nflags++] = asked;
    if (r != JOYERR_NOERROR) g_joy.fails++;
    else if (ji) {
        DWORD v[6] = { ji->dwXpos, ji->dwYpos, ji->dwZpos, ji->dwRpos, ji->dwUpos, ji->dwVpos };
        for (int i = 0; i < 6; i++) {
            if (!g_joy.any || v[i] < g_joy.minv[i]) g_joy.minv[i] = v[i];
            if (!g_joy.any || v[i] > g_joy.maxv[i]) g_joy.maxv[i] = v[i];
        }
        g_joy.any = true;
        g_joy.buttons_or |= ji->dwButtons;
        bool pseen = false;
        for (int i = 0; i < g_joy.npov; i++) if (g_joy.pov_seen[i] == ji->dwPOV) pseen = true;
        if (!pseen && g_joy.npov < 8) g_joy.pov_seen[g_joy.npov++] = ji->dwPOV;
    }
    LeaveCriticalSection(&g_joycs);
    return r;
}

/* ---------- import table ---------- */
const char *const kKernel32[] = {
    "CreateFileA", "ReadFile", "SetFilePointer", "GetFileSize", "CloseHandle", "_lopen", "_lclose",
    "GetPrivateProfileStringA", "WritePrivateProfileStringA", "SetPriorityClass", "GlobalAlloc", "LoadLibraryA",
};

bool traced_import(const char *dll, const char *fn)
{
    if (!lstrcmpiA(dll, "KERNEL32.dll")) {
        for (const char *k : kKernel32) if (!strcmp(k, fn)) return true;
        return false;
    }
    return !lstrcmpiA(dll, "USER32.dll") || !lstrcmpiA(dll, "GDI32.dll") || !lstrcmpiA(dll, "WINMM.dll") ||
           !lstrcmpiA(dll, "MSACM32.dll") || !lstrcmpiA(dll, "DDRAW.dll") || !lstrcmpiA(dll, "DSOUND.dll");
}

void wrap_imports()
{
    auto *dos = (IMAGE_DOS_HEADER *)g_base;
    auto *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    auto &dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto *imp = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dd.VirtualAddress);
    int n = 0;
    for (; imp->Name; imp++) {
        const char *dll = (const char *)(g_base + imp->Name);
        auto *names = (IMAGE_THUNK_DATA *)(g_base + imp->OriginalFirstThunk);
        auto *iat = (IMAGE_THUNK_DATA *)(g_base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char *fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData))->Name;
            if (!traced_import(dll, fn)) continue;
            void **slot = (void **)&iat->u1.Function;
            if (is_stub(*slot)) continue;
            if (!lstrcmpiA(dll, "WINMM.dll") && !strcmp(fn, "joyGetDevCapsA")) {
                set_fn(real_joyGetDevCapsA, *slot); write_ptr(slot, fnptr(hk_joyGetDevCapsA));
            } else if (!lstrcmpiA(dll, "WINMM.dll") && !strcmp(fn, "joyGetPosEx")) {
                set_fn(real_joyGetPosEx, *slot); write_ptr(slot, fnptr(hk_joyGetPosEx));
            }
            char nm[64];
            snprintf(nm, sizeof nm, "%.*s!%s", (int)(strchr(dll, '.') ? strchr(dll, '.') - dll : strlen(dll)), dll, fn);
            CharUpperBuffA(nm, (DWORD)(strchr(nm, '!') - nm));
            int id = new_entry(nm, Kind::Import, *slot);
            if (id < 0) break;
            write_ptr(slot, make_stub(id));
            n++;
        }
    }
    plog("trace: %d imports traced", n);
}

const char *const kDDraw[] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
    "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode",
    "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus",
    "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
};
const char *const kSurface[] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect", "Blt", "BltBatch",
    "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip",
    "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus",
    "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
    "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock",
    "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder",
};

void **g_vtables[16]; int g_nvtables;

void wrap_vtable(void **vt, const char *iface, const char *const *names, int n)
{
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nvtables; i++) if (g_vtables[i] == vt) { LeaveCriticalSection(&g_cs); return; }
    if (g_nvtables < 16) g_vtables[g_nvtables++] = vt;
    for (int i = 0; i < n; i++) {
        if (is_stub(vt[i])) continue;
        char nm[64]; snprintf(nm, sizeof nm, "%s::%s", iface, names[i]);
        int id = new_entry(nm, Kind::Method, vt[i]);
        if (id < 0) break;
        write_ptr(&vt[i], make_stub(id));
    }
    LeaveCriticalSection(&g_cs);
    plog("trace: %s vtable %p traced", iface, (void *)vt);
}

const char *object_name(void *p)
{
    for (int i = 0; i < g_nnamed; i++) if (g_named[i].ptr == p) return g_named[i].name;
    return nullptr;
}

const char *msg_name(int m)
{
    switch (m) {
    case WM_ACTIVATEAPP: return "ACTIVATEAPP"; case WM_ACTIVATE: return "ACTIVATE"; case WM_SETFOCUS: return "SETFOCUS";
    case WM_KILLFOCUS: return "KILLFOCUS"; case WM_PAINT: return "PAINT"; case WM_ERASEBKGND: return "ERASEBKGND";
    case WM_TIMER: return "TIMER"; case WM_KEYDOWN: return "KEYDOWN"; case WM_KEYUP: return "KEYUP";
    case WM_CHAR: return "CHAR"; case WM_SYSKEYDOWN: return "SYSKEYDOWN"; case WM_SYSKEYUP: return "SYSKEYUP";
    case WM_MOUSEMOVE: return "MOUSEMOVE"; case WM_LBUTTONDOWN: return "LBUTTONDOWN"; case WM_LBUTTONUP: return "LBUTTONUP";
    case WM_RBUTTONDOWN: return "RBUTTONDOWN"; case WM_SETCURSOR: return "SETCURSOR"; case WM_COMMAND: return "COMMAND";
    case WM_SYSCOMMAND: return "SYSCOMMAND"; case WM_INITMENU: return "INITMENU"; case WM_INITMENUPOPUP: return "INITMENUPOPUP";
    case WM_ENTERMENULOOP: return "ENTERMENULOOP"; case WM_EXITMENULOOP: return "EXITMENULOOP"; case WM_NCHITTEST: return "NCHITTEST";
    case WM_NCACTIVATE: return "NCACTIVATE"; case WM_GETMINMAXINFO: return "GETMINMAXINFO"; case WM_WINDOWPOSCHANGING: return "WINDOWPOSCHANGING";
    case WM_WINDOWPOSCHANGED: return "WINDOWPOSCHANGED"; case WM_MOVE: return "MOVE"; case WM_SIZE: return "SIZE";
    case WM_SHOWWINDOW: return "SHOWWINDOW"; case WM_DISPLAYCHANGE: return "DISPLAYCHANGE"; case WM_CLOSE: return "CLOSE";
    case WM_DESTROY: return "DESTROY"; case WM_QUERYNEWPALETTE: return "QUERYNEWPALETTE"; case WM_PALETTECHANGED: return "PALETTECHANGED";
    case WM_DEVICECHANGE: return "DEVICECHANGE"; case WM_INPUT: return "INPUT"; case WM_MOUSEWHEEL: return "MOUSEWHEEL";
    case MM_MCINOTIFY: return "MM_MCINOTIFY"; case WM_GETICON: return "GETICON"; case WM_IME_SETCONTEXT: return "IME_SETCONTEXT";
    case WM_IME_NOTIFY: return "IME_NOTIFY"; case WM_ENTERIDLE: return "ENTERIDLE"; case WM_MENUSELECT: return "MENUSELECT";
    }
    return nullptr;
}

/* ---------- reporting ---------- */
int g_visit_mode = -999;
DWORD g_visit_start, g_visit_frames0;
int g_visits;

void report_visit(DWORD now)
{
    if (g_visit_mode == -999) return;
    double secs = (now - g_visit_start) / 1000.0;
    DWORD frames = logic_frames() - g_visit_frames0;
    plog("trace: visit %d, screen mode %d, %.1f s, %lu logic frames, input device %d", g_visits, g_visit_mode, secs,
         (unsigned long)frames, input_device());
    struct Row { int id; LONG d; } rows[kMaxEntries]; int nr = 0;
    for (int i = 0; i < g_nentries; i++) {
        LONG c = g_entries[i].count, d = c - g_entries[i].visit_base;
        g_entries[i].visit_base = c;
        if (d > 0) rows[nr++] = { i, d };
    }
    for (int a = 1; a < nr; a++) for (int b = a; b > 0 && rows[b].d > rows[b - 1].d; b--) { Row t = rows[b]; rows[b] = rows[b - 1]; rows[b - 1] = t; }
    char line[700]; int n = 0;
    for (int i = 0; i < nr; i++) {
        int w = snprintf(line + n, sizeof line - n, "%s%s %ld", n ? ", " : "", g_entries[rows[i].id].name, rows[i].d);
        if (secs > 0.5 && rows[i].d > secs * 2) w += snprintf(line + n + w, sizeof line - n - w, " (%.1f/s)", rows[i].d / secs);
        n += w;
        if (n > 560) { plog("trace:   calls: %s", line); n = 0; }
    }
    if (n) plog("trace:   calls: %s", line);
    if (!nr) plog("trace:   calls: none");
    n = 0;
    for (int k = 0; k < 256; k++) {
        LONG c = InterlockedExchange(&g_vkeys[k], 0);
        if (c) n += snprintf(line + n, sizeof line - n, " 0x%02X x%ld", k, c);
        if (n > 600) break;
    }
    if (n) plog("trace:   keys polled (GetAsyncKeyState):%s", line);
    n = 0;
    for (int m = 0; m < 0x400; m++) {
        LONG c = InterlockedExchange(&g_msgs[m], 0);
        if (!c) continue;
        const char *nm = msg_name(m);
        n += nm ? snprintf(line + n, sizeof line - n, " %s %ld", nm, c) : snprintf(line + n, sizeof line - n, " 0x%X %ld", m, c);
        if (n > 600) break;
    }
    LONG o = InterlockedExchange(&g_msg_other, 0);
    if (o) n += snprintf(line + n, sizeof line - n, " other %ld", o);
    if (n) plog("trace:   window messages:%s", line);
    EnterCriticalSection(&g_joycs);
    if (g_joy.calls) {
        char fl[100] = ""; int fn = 0;
        for (int i = 0; i < g_joy.nflags; i++) fn += snprintf(fl + fn, sizeof fl - fn, " 0x%lX", g_joy.flags_seen[i]);
        char pv[100] = ""; int pn = 0;
        for (int i = 0; i < g_joy.npov; i++) pn += snprintf(pv + pn, sizeof pv - pn, " %lu", g_joy.pov_seen[i]);
        plog("trace:   joystick: %ld reads (%ld failed), ids mask 0x%X, flags asked:%s", g_joy.calls, g_joy.fails, g_joy.ids_mask, fl);
        if (g_joy.any)
            plog("trace:   joystick ranges seen: X %lu-%lu Y %lu-%lu Z %lu-%lu R %lu-%lu U %lu-%lu V %lu-%lu, buttons 0x%lX, POV:%s",
                 g_joy.minv[0], g_joy.maxv[0], g_joy.minv[1], g_joy.maxv[1], g_joy.minv[2], g_joy.maxv[2],
                 g_joy.minv[3], g_joy.maxv[3], g_joy.minv[4], g_joy.maxv[4], g_joy.minv[5], g_joy.maxv[5], g_joy.buttons_or, pv);
    }
    memset(&g_joy, 0, sizeof g_joy);
    LeaveCriticalSection(&g_joycs);
}

void report_final()
{
    plog("trace: ---- full report ----");
    for (int i = 0; i < g_nentries; i++) {
        Entry &e = g_entries[i];
        if (!e.count) continue;
        char cl[300] = ""; int n = 0;
        for (auto &s : e.callers) if (s.used == 1) n += snprintf(cl + n, sizeof cl - n, " %p x%ld", s.value, s.count);
        char ar[600] = ""; int m = 0;
        for (auto &s : e.args) if (s.used == 1) {
            const char *nm = e.kind == Kind::Method ? object_name(s.value) : nullptr;
            m += nm ? snprintf(ar + m, sizeof ar - m, " %p(%s) x%ld", s.value, nm, s.count)
                    : snprintf(ar + m, sizeof ar - m, " 0x%lX x%ld", (unsigned long)(DWORD_PTR)s.value, s.count);
            if (m > 520) break;
        }
        plog("trace: %s %ld | called from:%s | %s:%s%s", e.name, e.count, cl,
             e.kind == Kind::Method ? "objects" : "first argument", ar, e.args_overflow ? " (more values not listed)" : "");
    }
    for (int i = 0; i < g_nnamed; i++) plog("trace: object %p = %s", g_named[i].ptr, g_named[i].name);
}

DWORD WINAPI reporter(LPVOID)
{
    for (;;) {
        Sleep(100);
        DWORD now = GetTickCount();
        int mode = screen_mode();
        if (mode != g_visit_mode || now - g_visit_start >= 30000) {
            EnterCriticalSection(&g_cs);
            report_visit(now);
            if (mode != g_visit_mode) g_visits++;
            g_visit_mode = mode; g_visit_start = now; g_visit_frames0 = logic_frames();
            LeaveCriticalSection(&g_cs);
        }
    }
}

} // namespace

void trace_init(BYTE *base, int enabled)
{
    g_base = base;
    g_on = enabled != 0;
    if (!g_on) return;
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_joycs);
    g_stubs = (BYTE *)VirtualAlloc(nullptr, kMaxEntries * kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stubs) { g_on = false; plog("trace: no memory for stubs, tracing off"); return; }
    memset(g_stubs, 0xCC, kMaxEntries * kStubSize);
    wrap_imports();
    CloseHandle(CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr));
}

int trace_enabled() { return g_on; }

void trace_directdraw(void *dd) { if (g_on && dd) wrap_vtable(*(void ***)dd, "IDirectDraw", kDDraw, 23); }
void trace_surface(void *s) { if (g_on && s) wrap_vtable(*(void ***)s, "IDirectDrawSurface", kSurface, 36); }

void trace_name_object(void *p, const char *name)
{
    if (!g_on || !p) return;
    EnterCriticalSection(&g_cs);
    int i;
    for (i = 0; i < g_nnamed; i++) if (g_named[i].ptr == p) break;
    if (i == g_nnamed && g_nnamed < 64) g_nnamed++;
    if (i < 64) { g_named[i].ptr = p; snprintf(g_named[i].name, sizeof g_named[i].name, "%s", name); }
    LeaveCriticalSection(&g_cs);
}

void trace_message(UINT m)
{
    if (!g_on) return;
    if (m < 0x400) InterlockedIncrement(&g_msgs[m]); else InterlockedIncrement(&g_msg_other);
}

int trace_retarget(void *current, void *fn, void **orig_out)
{
    if (!g_on || !is_stub(current)) return 0;
    Entry &e = stub_entry(current);
    if (e.orig != fn) { if (orig_out) *orig_out = e.orig; e.orig = fn; }
    return 1;
}

void trace_shutdown()
{
    if (!g_on) return;
    EnterCriticalSection(&g_cs);
    report_visit(GetTickCount());
    report_final();
    LeaveCriticalSection(&g_cs);
}
