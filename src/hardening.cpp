/*
 * Release hardening (build 0.0.16).
 *
 *   exe check        The proxy changes DAYTONA.EXE v1.1 in memory at known addresses. Any other program or version
 *                    gets no changes at all: DirectDrawCreate goes straight to Windows, and DAYTONA.EXE of another
 *                    version gets a clear message once.
 *   crash reports    An unhandled exception (in the game or in our threads) writes logs\crash_<date>_<time>.txt
 *                    (exception, module and offset, registers, stack, the game's screen, the end of the log) and
 *                    a minidump beside it, then tells the player where they are. A crash in the game still runs
 *                    the game's own clean-up (its settings save) as before.
 *   settings         ddraw_proxy.ini is brought up to this build's layout on start (see settings_migrate).
 *   health log       Once a minute: memory, handles, GDI and USER objects, frames shown (for long-session tests).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <dbghelp.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
#include "proxy.h"
#include "hardening.h"
#ifndef PROXY_VERSION_STR
#define PROXY_VERSION_STR "dev"
#endif

/* ---------- exe check ---------- */

static bool sha1_file(const char *path, char out[41], DWORD *size)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    *size = GetFileSize(f, nullptr);
    HCRYPTPROV prov = 0; HCRYPTHASH hash = 0; bool ok = false;
    if (CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
        static BYTE buf[65536]; DWORD n; ok = true;
        while (ok && ReadFile(f, buf, sizeof buf, &n, nullptr) && n) ok = CryptHashData(hash, buf, n, 0) != 0;
        BYTE h[20]; DWORD hl = sizeof h;
        if (ok && CryptGetHashParam(hash, HP_HASHVAL, h, &hl, 0)) {
            for (int i = 0; i < 20; i++) sprintf(out + i * 2, "%02x", h[i]);
        } else ok = false;
    }
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(f);
    return ok;
}

ExeCheck exe_check(char sha1[41])
{
    char path[MAX_PATH]; DWORD size = 0;
    sha1[0] = 0;
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char *base = strrchr(path, '\\'); base = base ? base + 1 : path;
    bool named = !lstrcmpiA(base, "DAYTONA.EXE");
    if (!sha1_file(path, sha1, &size)) return named ? ExeCheck::OtherVersion : ExeCheck::OtherProgram;
    if (size == kSupportedExeSize && !strcmp(sha1, kSupportedExeSha1)) return ExeCheck::Supported;
    return named ? ExeCheck::OtherVersion : ExeCheck::OtherProgram;
}

void exe_unsupported_message(const char *sha1)
{
    static LONG shown;
    if (InterlockedExchange(&shown, 1)) return;
    char msg[1024];
    snprintf(msg, sizeof msg,
        "This DAYTONA.EXE is not version 1.1 (the Expert Software re-release), the only version this ddraw.dll "
        "(ultdaytonausa %s) supports.\n\n"
        "The game will start without any of its changes (display scaling, controllers, CD music from FLAC, "
        "the Options pages), as the original.\n\n"
        "To stop this message, remove ddraw.dll from the game folder, or install the game from a v1.1 disc with "
        "the ultdaytonausa installer.\n\n(SHA-1 of this DAYTONA.EXE: %s)", PROXY_VERSION_STR, sha1[0] ? sha1 : "unreadable");
    MessageBoxA(nullptr, msg, "Daytona USA (ultdaytonausa)", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
}

/* ---------- settings file migration ---------- */

#include "ini_default.gen.h"   /* kDefaultIni: src/ddraw_proxy.ini for this variant, made by the build */

const char *settings_default_ini() { return kDefaultIni; }

namespace {

struct Line { std::string text, key, value; bool is_key = false; };
struct Section { std::string name; std::vector<Line> lines; };   /* lines include the header (first) */

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool same(const std::string &a, const std::string &b) { return !lstrcmpiA(a.c_str(), b.c_str()); }

std::vector<Section> parse(const std::string &text)
{
    std::vector<Section> out(1);   /* out[0]: lines before the first section */
    std::vector<Line> pending;     /* comments and blank lines, attached to the next section header */
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        std::string raw = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        Line l; l.text = raw;
        std::string t = trim(raw);
        if (!t.empty() && t[0] == '[' && t.back() == ']') {
            Section s; s.name = trim(t.substr(1, t.size() - 2));
            s.lines = pending; pending.clear();
            s.lines.push_back(l);
            out.push_back(s);
            continue;
        }
        size_t eq = t.find('=');
        if (!t.empty() && t[0] != ';' && t[0] != '#' && eq != std::string::npos) {
            l.is_key = true; l.key = trim(t.substr(0, eq)); l.value = trim(t.substr(eq + 1));
            for (auto &p : pending) out.back().lines.push_back(p);
            pending.clear();
            out.back().lines.push_back(l);
        } else pending.push_back(l);
    }
    for (auto &p : pending) out.back().lines.push_back(p);
    return out;
}

const Line *find_key(const std::vector<Section> &ss, const std::string &sec, const std::string &key)
{
    for (auto &s : ss) if (same(s.name, sec)) for (auto &l : s.lines) if (l.is_key && same(l.key, key)) return &l;
    return nullptr;
}

/* Settings that are gone in this build: dropped from the player's file. The [Test] options are debug-build only:
   the release build drops them all (decisions log, 31 Oct 2026; 1.0.0 release candidate). */
bool retired(const std::string &sec, const std::string &key)
{
#ifndef ULT_DEBUG
    if (same(sec, "Test")) return true;   /* 1.0.0: the release build has no [Test] options at all */
#endif
    if (same(sec, "Proxy") && same(key, "HideMenuInFullscreen")) return true;   /* 0.0.17: the Windows menu is removed */
    if (same(sec, "Controller") && (same(key, "PadType") || same(key, "Applied"))) return true;   /* 0.0.18: the BUTTONS page */
    (void)sec; (void)key;
    return false;
}

/* Values whose spelling changed between builds. */
std::string upgrade_value(const std::string &sec, const std::string &key, const std::string &v)
{
    if (same(sec, "Display") && same(key, "Picture")) {
        if (same(v, "Auto")) return "16:10";   /* 0.0.10 to 0.0.14 */
        if (same(v, "Fill")) return "4:3";
    }
    return v;
}

bool read_file(const char *path, std::string &out)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = GetFileSize(f, nullptr), got = 0;
    out.assign(n, '\0');
    bool ok = n == 0 || (ReadFile(f, &out[0], n, &got, nullptr) && got == n);
    CloseHandle(f);
    return ok;
}

bool write_file(const char *path, const std::string &text)
{
    std::string tmp = std::string(path) + ".new";
    HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    if (ok) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);   /* a read-only copy from CD */
        ok = MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!ok) DeleteFileA(tmp.c_str());
    return ok;
}

std::string crlf(const std::string &s)
{
    std::string o; o.reserve(s.size() + s.size() / 20);
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) o += '\r';
        o += s[i];
    }
    return o;
}

} // namespace

std::string settings_migrate(const char *ini_path, const char *version)
{
    std::string tmpl = kDefaultIni, old;
    if (!read_file(ini_path, old)) {
        return write_file(ini_path, crlf(tmpl)) ? "settings: no ddraw_proxy.ini, default settings written"
                                                : "settings: no ddraw_proxy.ini and the default could not be written";
    }
    std::vector<Section> user = parse(old), def = parse(tmpl);
    const Line *uv = find_key(user, "Proxy", "SettingsVersion");
    if (uv && uv->value == version) return "";

    std::string out, dropped;
    int nkept = 0, nnew = 0, nextra = 0;
    for (auto &s : def) {
        size_t last_key = std::string::npos;   /* end of the section's last setting in out */
        for (auto &l : s.lines) {
            if (!l.is_key) { out += l.text + "\n"; continue; }
            const Line *u = find_key(user, s.name, l.key);
            if (same(s.name, "Proxy") && same(l.key, "SettingsVersion")) out += l.key + "=" + version + "\n";
            else if (u) { out += l.key + "=" + upgrade_value(s.name, l.key, u->value) + "\n"; nkept++; }
            else { out += l.text + "\n"; nnew++; }
            last_key = out.size();
        }
        /* settings the player's file has in this section and the default file does not (written by the proxy
           itself, such as [Controller] Applied, or from a newer build) are kept after the section's last setting;
           only retired settings go */
        std::string extra;
        for (auto &us : user) {
            if (!same(us.name, s.name) || s.name.empty()) continue;
            for (auto &l : us.lines) {
                if (!l.is_key || find_key(def, s.name, l.key)) continue;
                if (retired(s.name, l.key)) { dropped += " [" + s.name + "] " + l.key + "=" + l.value; continue; }
                extra += l.key + "=" + l.value + "\n"; nextra++;
            }
        }
        if (!extra.empty()) {
            if (last_key == std::string::npos) last_key = out.size();
            out.insert(last_key, extra);
        }
    }
    /* sections the default file does not have (per-controller [Pad ...] / [Joystick ...]) are kept as they are */
    int nsec = 0;
    for (size_t i = 1; i < user.size(); i++) {
        bool known = false;
        for (auto &s : def) if (same(s.name, user[i].name)) known = true;
        if (known) continue;
#ifndef ULT_DEBUG
        if (same(user[i].name, "Test")) {   /* 1.0.0: no test options in the release build, the whole section goes */
            for (auto &l : user[i].lines) if (l.is_key) dropped += " [Test] " + l.key + "=" + l.value;
            continue;
        }
#endif
        bool blank_before = out.size() >= 2 && out.compare(out.size() - 2, 2, "\n\n") == 0;
        bool blank_first = !user[i].lines.empty() && trim(user[i].lines[0].text).empty();
        if (!blank_before && !blank_first) out += "\n";
        for (auto &l : user[i].lines) out += l.text + "\n";
        nsec++;
    }

    char backup[MAX_PATH];
    snprintf(backup, sizeof backup, "%s.before-%s", ini_path, version);
    if (GetFileAttributesA(backup) == INVALID_FILE_ATTRIBUTES) CopyFileA(ini_path, backup, TRUE);
    char buf[2048];
    if (!write_file(ini_path, crlf(out))) {
        snprintf(buf, sizeof buf, "settings: ddraw_proxy.ini (from %s) could NOT be updated to %s; it is read as it is",
                 uv ? uv->value.c_str() : "a build before 0.0.16", version);
        return buf;
    }
    snprintf(buf, sizeof buf, "settings: ddraw_proxy.ini updated from %s to %s: %d settings kept, %d new with defaults, "
             "%d other settings kept, %d controller sections kept, old file kept as %s%s%s",
             uv ? uv->value.c_str() : "a build before 0.0.16", version, nkept, nnew, nextra, nsec, strrchr(backup, '\\') ? strrchr(backup, '\\') + 1 : backup,
             dropped.empty() ? "" : "; no longer used:", dropped.c_str());
    return buf;
}

/* ---------- crash reports ---------- */

namespace {

char g_crash_dir[MAX_PATH];
char g_log_path[MAX_PATH];
BYTE *g_game;
HWND g_win;
volatile LONG g_crashed;
using MiniDumpWriteDump_t = BOOL (WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                             PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDump_t g_minidump;

const char *exception_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "floating-point invalid operation";
    case EXCEPTION_FLT_OVERFLOW: return "floating-point overflow";
    case EXCEPTION_FLT_STACK_CHECK: return "floating-point stack check";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case 0xC0000409: return "stack buffer overrun / fail fast";
    case 0xC0000374: return "heap corruption";
    default: return "exception";
    }
}

/* "DAYTONA.EXE+0x12345" for an address inside a loaded module, else "" */
void where(DWORD addr, char *out, size_t n)
{
    HMODULE m = nullptr; char path[MAX_PATH];
    out[0] = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(ULONG_PTR)addr, &m) || !m) return;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi) || addr < (DWORD)(ULONG_PTR)mi.lpBaseOfDll ||
        addr >= (DWORD)(ULONG_PTR)mi.lpBaseOfDll + mi.SizeOfImage) return;
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return;
    const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fnptr(&where), &self);
    if (m == self) b = "ddraw.dll (ultdaytonausa)";
    if (m == (HMODULE)g_game) snprintf(out, n, "%s+0x%lX (0x%08lX in the original exe)", b, addr - (DWORD)(ULONG_PTR)m, addr - (DWORD)(ULONG_PTR)m + 0x400000);
    else snprintf(out, n, "%s+0x%lX", b, addr - (DWORD)(ULONG_PTR)m);
}

bool readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (BYTE *)p + n <= (BYTE *)mi.BaseAddress + mi.RegionSize;
}

struct Out {
    HANDLE f;
    __attribute__((format(printf, 2, 3))) void put(const char *fmt, ...)
    {
        char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        if (n < 0) return;
        if (n > (int)sizeof b - 1) n = sizeof b - 1;
        DWORD w; WriteFile(f, b, (DWORD)n, &w, nullptr);
    }
};

void write_report(EXCEPTION_POINTERS *ep, const char *thread_kind, char *txt_path, char *dmp_path)
{
    SYSTEMTIME t; GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    snprintf(txt_path, MAX_PATH, "%scrash_%s.txt", g_crash_dir, stamp);
    snprintf(dmp_path, MAX_PATH, "%scrash_%s.dmp", g_crash_dir, stamp);
    CreateDirectoryA(g_crash_dir, nullptr);

    EXCEPTION_RECORD *er = ep->ExceptionRecord; CONTEXT *c = ep->ContextRecord;
    char at[300]; where((DWORD)(ULONG_PTR)er->ExceptionAddress, at, sizeof at);
    plog("CRASH: %s (0x%08lX) at 0x%08lX %s, %s thread; report %s", exception_name(er->ExceptionCode), er->ExceptionCode,
         (DWORD)(ULONG_PTR)er->ExceptionAddress, at, thread_kind, txt_path);

    HANDLE f = CreateFileA(txt_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        Out o{f};
        o.put("Daytona USA crash report\r\nultdaytonausa ddraw.dll %s (%s build)\r\n", PROXY_VERSION_STR,
#ifdef ULT_DEBUG
              "debug"
#else
              "release"
#endif
        );
        o.put("Time: %04d-%02d-%02d %02d:%02d:%02d\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        OSVERSIONINFOA ov{}; ov.dwOSVersionInfoSize = sizeof ov;
        using RtlGetVersion_t = LONG (WINAPI *)(OSVERSIONINFOA *);
        RtlGetVersion_t rgv = nullptr; set_fn(rgv, GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion"));
        if (rgv) rgv(&ov);
        o.put("Windows: %lu.%lu build %lu\r\n", ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
        o.put("Thread: %s (id %lu)\r\n\r\n", thread_kind, GetCurrentThreadId());
        o.put("Exception: %s (0x%08lX)\r\nAddress: 0x%08lX %s\r\n", exception_name(er->ExceptionCode), er->ExceptionCode,
              (DWORD)(ULONG_PTR)er->ExceptionAddress, at);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            o.put("Access: %s 0x%08lX\r\n", er->ExceptionInformation[0] == 0 ? "read of" : er->ExceptionInformation[0] == 1 ? "write to" : "execution at",
                  (DWORD)er->ExceptionInformation[1]);
        o.put("\r\nRegisters:\r\n EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\r\n ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX\r\n EIP=%08lX EFLAGS=%08lX\r\n",
              c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags);
        if (g_game && readable(g_game + (0x482230 - 0x400000), 4))
            o.put("\r\nGame screen mode (0x482230): %d\r\n", *(int *)(g_game + (0x482230 - 0x400000)));

        o.put("\r\nCall chain (frame pointers, may be incomplete):\r\n");
        DWORD ebp = c->Ebp;
        for (int i = 0; i < 24 && ebp && readable((void *)(ULONG_PTR)ebp, 8); i++) {
            DWORD ret = ((DWORD *)(ULONG_PTR)ebp)[1], next = ((DWORD *)(ULONG_PTR)ebp)[0];
            char w2[300]; where(ret, w2, sizeof w2);
            o.put(" %2d  0x%08lX %s\r\n", i, ret, w2);
            if (next <= ebp) break;
            ebp = next;
        }
        o.put("\r\nStack (from ESP, values inside a module named):\r\n");
        DWORD *sp = (DWORD *)(ULONG_PTR)c->Esp;
        for (int i = 0; i < 96 && readable(sp + i, 4); i++) {
            char w2[300]; where(sp[i], w2, sizeof w2);
            if (w2[0]) o.put(" ESP+%03X  0x%08lX %s\r\n", i * 4, sp[i], w2);
        }
        o.put("\r\nModules:\r\n");
        HMODULE mods[256]; DWORD need = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
            for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 256; i++) {
                char p[MAX_PATH]; MODULEINFO mi{};
                GetModuleFileNameA(mods[i], p, MAX_PATH);
                GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
                o.put(" %08lX %08lX %s\r\n", (DWORD)(ULONG_PTR)mi.lpBaseOfDll, mi.SizeOfImage, p);
            }
        }
        /* the end of the proxy log */
        if (g_log_path[0]) {
            HANDLE lf = CreateFileA(g_log_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (lf != INVALID_HANDLE_VALUE) {
                static char tail[16384]; DWORD size = GetFileSize(lf, nullptr), n = 0;
                DWORD from = size > sizeof tail ? size - sizeof tail : 0;
                SetFilePointer(lf, (LONG)from, nullptr, FILE_BEGIN);
                ReadFile(lf, tail, sizeof tail, &n, nullptr);
                CloseHandle(lf);
                o.put("\r\nEnd of the log (%s):\r\n", g_log_path);
                DWORD w; WriteFile(f, tail, n, &w, nullptr);
            }
        }
        CloseHandle(f);
    }

    if (!g_minidump) {
        HMODULE d = LoadLibraryA("dbghelp.dll");
        if (d) set_fn(g_minidump, GetProcAddress(d, "MiniDumpWriteDump"));
    }
    if (g_minidump) {
        HANDLE df = CreateFileA(dmp_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (df != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
            BOOL ok = g_minidump(GetCurrentProcess(), GetCurrentProcessId(), df,
                                 (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory), &mei, nullptr, nullptr);
            CloseHandle(df);
            if (!ok) { DeleteFileA(dmp_path); dmp_path[0] = 0; }
        } else dmp_path[0] = 0;
    } else dmp_path[0] = 0;
    plog("CRASH: report written%s%s", dmp_path[0] ? ", minidump " : ", no minidump", dmp_path[0] ? dmp_path : "");
}

DWORD WINAPI message_thread(LPVOID m)
{
    ShowCursor(TRUE);
    MessageBoxA(nullptr, (const char *)m, "Daytona USA (ultdaytonausa)", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    return 0;
}

LONG handle(EXCEPTION_POINTERS *ep, const char *thread_kind)
{
    if (InterlockedExchange(&g_crashed, 1)) {
        /* a second exception while the game cleans up after the first: stop now */
        plog("CRASH: another %s (0x%08lX) during clean-up; closing", exception_name(ep->ExceptionRecord->ExceptionCode),
             ep->ExceptionRecord->ExceptionCode);
        TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    char txt[MAX_PATH], dmp[MAX_PATH];
    write_report(ep, thread_kind, txt, dmp);
    ClipCursor(nullptr);
    static char msg[1200];
    snprintf(msg, sizeof msg,
             "Daytona USA has stopped because of an error (%s).\n\nA crash report was saved:\n%s%s%s\n\n"
             "Please send %s with a short description of what you were doing. The game will now close.",
             exception_name(ep->ExceptionRecord->ExceptionCode), txt, dmp[0] ? "\n" : "", dmp,
             dmp[0] ? "both files" : "it");
    /* The message is shown from a thread of its own: the crashed thread waits and does not run the game's
       window code (a message box on the crashed thread would keep calling it). */
    HANDLE th = CreateThread(nullptr, 0, message_thread, msg, 0, nullptr);
    if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); }
    plog("CRASH: message closed, the game closes");
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI top_level_filter(EXCEPTION_POINTERS *ep) { return handle(ep, "other"); }

} // namespace

bool crash_hook_game_filter(BYTE *game_base);

/* WinMain (0x438180) runs the whole game inside __try { ... } __except(1): every crash in the game thread ends
   there, and the game used to exit without a word (its clean-up still runs). Its filter expression is the
   6-byte function 0x438539 (mov eax,1 / ret), called by the C runtime's _except_handler3 with EBP set to
   WinMain's frame, where [ebp-0x14] holds the EXCEPTION_POINTERS. The filter is replaced by a jump to a stub
   that passes them to crash_game_filter, which reports and returns 1 as before. */
extern "C" LONG __stdcall crash_game_filter(EXCEPTION_POINTERS *ep)
{
    if (IsDebuggerPresent()) return EXCEPTION_EXECUTE_HANDLER;
    return handle(ep, "game");
}
extern "C" void crash_game_filter_stub();
asm(".text\n"
    ".globl _crash_game_filter_stub\n"
    "_crash_game_filter_stub:\n"
    "    pushl -0x14(%ebp)\n"
    "    call _crash_game_filter@4\n"
    "    ret\n");

bool crash_hook_game_filter(BYTE *game_base)
{
    static const BYTE expect[6] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    BYTE *p = game_base + (0x438539 - 0x400000);
    if (memcmp(p, expect, sizeof expect)) { plog("crash reports: WinMain's exception filter not as expected, NOT hooked"); return false; }
    DWORD old;
    VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(LONG *)(p + 1) = (LONG)((BYTE *)crash_game_filter_stub - (p + 5));
    p[5] = 0x90;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    plog("crash reports: on (WinMain's exception filter and the unhandled-exception filter)");
    return true;
}

LONG WINAPI crash_unhandled_exception_filter(EXCEPTION_POINTERS *ep)
{
    if (IsDebuggerPresent()) return EXCEPTION_CONTINUE_SEARCH;
    /* the game's C runtime filter: EXCEPTION_EXECUTE_HANDLER runs the game's own clean-up (settings save), then exits */
    return handle(ep, "game");
}

void crash_init(const char *gamedir, const char *log_path, BYTE *game_base)
{
    snprintf(g_crash_dir, sizeof g_crash_dir, "%slogs\\", gamedir);
    snprintf(g_log_path, sizeof g_log_path, "%s", log_path ? log_path : "");
    g_game = game_base;
    SetUnhandledExceptionFilter(top_level_filter);
    crash_hook_game_filter(game_base);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
}

void crash_set_window(HWND hwnd) { g_win = hwnd; }

void crash_preload()
{
    if (g_minidump) return;
    HMODULE d = LoadLibraryA("dbghelp.dll");
    if (d) set_fn(g_minidump, GetProcAddress(d, "MiniDumpWriteDump"));
    if (!g_minidump) plog("crash reports: dbghelp.dll not available, reports without a minidump");
}

#ifdef ULT_DEBUG
static DWORD g_crash_at;
void crash_test_config(int seconds) { if (seconds > 0) g_crash_at = GetTickCount() + (DWORD)seconds * 1000; }
void crash_test_tick()
{
    if (!g_crash_at || (LONG)(GetTickCount() - g_crash_at) < 0) return;
    g_crash_at = 0;
    plog("CrashTest: crashing on purpose now (game thread)");
    static volatile ULONG_PTR bad = 0x10;
    *(volatile int *)bad = 1;   /* access violation at a low address */
}
#endif

/* ---------- health log ---------- */

static volatile LONG g_frames;
void health_note_frame() { InterlockedIncrement(&g_frames); }

static DWORD WINAPI health_thread(LPVOID)
{
    DWORD t0 = GetTickCount();
    LONG last = 0;
    for (;;) {
        Sleep(60000);
        PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb = sizeof pm;
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
        LONG f = g_frames;
        plog("health: %lu min, memory %.1f MB (private %.1f MB), handles %lu, GDI %lu, USER %lu, frames shown %ld (%.1f per second)",
             (GetTickCount() - t0) / 60000, pm.WorkingSetSize / 1048576.0, pm.PrivateUsage / 1048576.0, handles,
             GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS),
             f, (f - last) / 60.0);
        last = f;
    }
    return 0;
}

void health_start()
{
    static LONG started;
    if (InterlockedExchange(&started, 1)) return;
    CloseHandle(CreateThread(nullptr, 0, health_thread, nullptr, 0, nullptr));
}
