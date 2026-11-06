/* ultdaytonausa installer.
 *
 * Installs Daytona USA (PC, v1.1) from the player's own disc or .cue / .bin image:
 *   1. checks that the disc is v1.1 (DAYTONA.EXE SHA-1) and copies the game files (GAME\MINI, GAME\FULL,
 *      GAME\ETC\MUSIC.BIN), each checked against the v1.1 checksums; the player's own DAYTONA.INI and DAYTONA.DAT
 *      are kept when installing over an earlier install;
 *   2. rips the CD audio (Red Book) to lossless FLAC in Music\ with cdaudio.toc, each track verified;
 *   3. installs the ddraw.dll proxy and its default settings (an existing ddraw_proxy.ini is kept);
 * Portable (jayyanz, 5 Nov 2026): everything stays in the game folder. No registry entries, no entry in Windows'
 * installed apps, no uninstaller and no shortcuts. To remove the game, delete its folder. What the installers of
 * 0.0.14 to 0.0.21 left outside the folder is removed by tools\ultdaytonausa-cleanup.cmd.
 * No game data or music is in this program: everything comes from the player's disc.
 *
 * Command line: /source <file.cue | D:>  /dest <folder>  [/silent] [/norip]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include "disc.h"
#include "rip.h"
#include "game_files.h"

#ifndef PROXY_VERSION_STR
#define PROXY_VERSION_STR "dev"
#endif

namespace {

const wchar_t kAppName[] = L"Daytona USA (ultdaytonausa)";
const wchar_t kManifest[] = L"ultdaytonausa-install.txt";
const wchar_t kSetupCopy[] = L"ultdaytonausa-setup.exe";          /* left in the game folder by 0.0.14 to 0.0.16 */
constexpr int kResDdraw = 101, kResIni = 102;

std::wstring fmt(const wchar_t *f, ...)
{
    wchar_t b[1024];
    va_list a; va_start(a, f); vswprintf(b, 1024, f, a); va_end(a);
    return b;
}

std::wstring version() { std::string v = PROXY_VERSION_STR; return std::wstring(v.begin(), v.end()); }

std::wstring self_path()
{
    wchar_t b[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    return b;
}

bool file_exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring sha1_hex(const void *data, size_t n)
{
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    UCHAR out[20] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0);
            BCryptFinishHash(h, out, 20, 0);
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    std::wstring s;
    for (UCHAR c : out) s += fmt(L"%02x", c);
    return s;
}

std::wstring widen(const char *s) { return std::wstring(s, s + strlen(s)); }

bool write_file(const std::wstring &path, const void *data, size_t n)
{
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);   /* files copied from a CD are read-only */
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data, (DWORD)n, &put, nullptr) && put == n;
    CloseHandle(h);
    return ok;
}

bool make_dirs(const std::wstring &dir)
{
    int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

bool resource(int id, const void *&data, DWORD &size)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    data = g ? LockResource(g) : nullptr;
    size = SizeofResource(nullptr, r);
    return data != nullptr;
}

/* ---------- the install ---------- */

struct Options {
    std::wstring source, dest;
    bool rip = true, silent = false;
};

struct Sink {
    std::function<void(const std::wstring &)> log;
    std::function<void(double)> progress;
    std::atomic<bool> *cancel = nullptr;
};

class Installer {
public:
    Installer(const Options &o, Sink &s) : o_(o), s_(s) {}

    bool run()
    {
        log_file_ = o_.dest + L"\\ultdaytonausa-install.log";
        SYSTEMTIME st; GetLocalTime(&st);
        say(fmt(L"---- ultdaytonausa installer %ls, %04d-%02d-%02d %02d:%02d ----", version().c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute));
        std::wstring err;
        std::unique_ptr<Disc> disc = open_source(err);
        if (!disc) return fail(err);
        say(L"Source: " + disc->describe());
        int audio = 0;
        for (const Track &t : disc->tracks()) audio += t.audio;
        say(fmt(L"Disc layout: %d track(s), %d of them CD audio", (int)disc->tracks().size(), audio));

        /* v1.1 only (decisions log): checked before anything is written */
        std::vector<uint8_t> exe;
        if (!disc->read_file(L"GAME\\MINI\\DAYTONA.EXE", exe, err)) return fail(L"DAYTONA.EXE was not found on the disc (" + err + L"). Is this the Daytona USA PC disc?");
        if (sha1_hex(exe.data(), exe.size()) != widen(kGameFiles[find_file("MINI", "DAYTONA.EXE")].sha1))
            return fail(L"This disc's DAYTONA.EXE is not version 1.1 (the Expert Software re-release of 1997). This installer supports version 1.1 only.");
        say(L"DAYTONA.EXE is version 1.1");

        if (!make_dirs(o_.dest)) return fail(L"Cannot create the folder " + o_.dest);
        if (!copy_game(*disc)) return false;
        if (!install_proxy()) return false;
        if (o_.rip) {
            say(L"Ripping the CD music to FLAC (lossless). This takes a few minutes" + std::wstring(disc->needs_double_read() ? L"; every track is read twice and compared." : L"."));
            RipCallbacks cb;
            cb.log = [&](const std::wstring &l) { say(l); };
            cb.progress = [&](double d) { progress(0.15 + 0.8 * d); };
            cb.cancelled = [&] { return s_.cancel && s_.cancel->load(); };
            if (!rip_audio(*disc, o_.dest + L"\\Music", cb, err)) return fail(L"Music: " + err);
            for (const Track &t : disc->tracks()) if (t.audio && t.length) installed_.push_back(fmt(L"Music\\Track%02d.flac", t.number));
            installed_.push_back(L"Music\\cdaudio.toc");
        } else {
            say(L"Music not ripped (chosen): the game plays without music until it is (run the installer again).");
        }
        progress(0.96);
        if (!finish()) return false;
        progress(1.0);
        say(L"Done. Daytona USA is installed in " + o_.dest);
        return true;
    }

private:
    std::unique_ptr<Disc> open_source(std::wstring &err)
    {
        std::wstring s = o_.source;
        if (s.size() >= 2 && s.size() <= 3 && s[1] == L':') return open_drive(towupper(s[0]), err);
        std::wstring lower = s;
        for (auto &c : lower) c = towlower(c);
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".bin") == 0) {   /* a .bin given: its .cue beside it */
            std::wstring cue = s.substr(0, s.size() - 4) + L".cue";
            if (file_exists(cue)) s = cue;
        }
        if (s.size() > 4 && (lower.compare(lower.size() - 4, 4, L".cue") == 0 || file_exists(s))) return open_cue(s, err);
        err = L"Choose a CD drive or a .cue file (the .cue / .bin disc image).";
        return nullptr;
    }

    static int find_file(const char *dir, const char *name)
    {
        for (int i = 0; i < (int)(sizeof kGameFiles / sizeof *kGameFiles); i++)
            if (!strcmp(kGameFiles[i].dir, dir) && !strcmp(kGameFiles[i].name, name)) return i;
        return 0;
    }

    bool copy_game(Disc &disc)
    {
        const int n = (int)(sizeof kGameFiles / sizeof *kGameFiles);
        int kept = 0;
        uint64_t total = 0, done = 0;
        for (const GameFile &f : kGameFiles) total += f.size;
        say(fmt(L"Copying %d game files and checking each against the version 1.1 checksums", n));
        for (const GameFile &f : kGameFiles) {
            if (s_.cancel && s_.cancel->load()) return fail(L"Cancelled.");
            std::wstring name = widen(f.name), dst = o_.dest + L"\\" + name;
            /* the player's settings and records from an earlier install */
            if ((name == L"DAYTONA.INI" || name == L"DAYTONA.DAT") && file_exists(dst)) {
                SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
                installed_.push_back(name);
                kept++;
                done += f.size;
                continue;
            }
            std::wstring src = L"GAME\\" + widen(f.dir) + L"\\" + name, err;
            std::vector<uint8_t> data;
            bool ok = false;
            for (int attempt = 0; attempt < 2 && !ok; attempt++)
                ok = disc.read_file(src, data, err) && data.size() == f.size && sha1_hex(data.data(), data.size()) == widen(f.sha1);
            if (!ok) return fail(err.empty() ? src + L" does not match the version 1.1 checksum (a read error, or a different release of the game)" : err);
            if (!write_file(dst, data.data(), data.size())) return fail(L"Cannot write " + dst);
            installed_.push_back(name);
            done += f.size;
            progress(0.13 * (double)done / (double)total);
        }
        say(fmt(L"Game files: %d copied and checked", n - kept) + (kept ? fmt(L", your own DAYTONA.INI / DAYTONA.DAT kept (%d)", kept) : L""));
        return true;
    }

    bool install_proxy()
    {
        const void *dll, *ini; DWORD dll_n, ini_n;
        if (!resource(kResDdraw, dll, dll_n) || !resource(kResIni, ini, ini_n)) return fail(L"The installer is damaged (its ddraw.dll is missing).");
        if (!write_file(o_.dest + L"\\ddraw.dll", dll, dll_n)) return fail(L"Cannot write ddraw.dll (is the game running?)");
        installed_.push_back(L"ddraw.dll");
        std::wstring ini_path = o_.dest + L"\\ddraw_proxy.ini";
        if (file_exists(ini_path)) {
            write_file(o_.dest + L"\\ddraw_proxy.default.ini", ini, ini_n);
            installed_.push_back(L"ddraw_proxy.default.ini");
            say(L"ultdaytonausa " + version() + L" installed (ddraw.dll); your ddraw_proxy.ini kept, the new defaults are in ddraw_proxy.default.ini");
        } else {
            if (!write_file(ini_path, ini, ini_n)) return fail(L"Cannot write " + ini_path);
            say(L"ultdaytonausa " + version() + L" installed (ddraw.dll, ddraw_proxy.ini)");
        }
        installed_.push_back(L"ddraw_proxy.ini");
        progress(0.15);
        return true;
    }

    bool finish()
    {
        /* Portable: nothing outside the game folder. The installer copy 0.0.14 to 0.0.16 left in the folder is
           removed; what they and 0.0.17 to 0.0.21 left elsewhere is for ultdaytonausa-cleanup.cmd. */
        std::wstring me = self_path(), old = o_.dest + L"\\" + kSetupCopy;
        if (lstrcmpiW(me.c_str(), old.c_str()) && file_exists(old) && DeleteFileW(old.c_str())) say(L"Removed the installer copy left in the game folder by an earlier build");
        installed_.push_back(L"ultdaytonausa-install.log");
        /* the list of what was installed, for reference */
        std::wstring m = L"; ultdaytonausa " + version() + L": files installed in this folder. Portable: nothing was written outside it;\r\n"
                         L"; to remove the game, delete this folder.\r\n";
        for (auto &f : installed_) m += f + L"\r\n";
        m += std::wstring(kManifest) + L"\r\n";
        std::string utf8(WideCharToMultiByte(CP_UTF8, 0, m.c_str(), -1, nullptr, 0, nullptr, nullptr), 0);
        WideCharToMultiByte(CP_UTF8, 0, m.c_str(), -1, &utf8[0], (int)utf8.size(), nullptr, nullptr);
        if (!write_file(o_.dest + L"\\" + kManifest, utf8.data(), utf8.size() - 1)) return fail(L"Cannot write the install list");
        say(L"Portable install: nothing was written outside " + o_.dest + L" (no registry entries, no shortcuts)");
        return true;
    }

    void say(const std::wstring &l)
    {
        s_.log(l);
        std::string u(WideCharToMultiByte(CP_UTF8, 0, l.c_str(), -1, nullptr, 0, nullptr, nullptr), 0);
        WideCharToMultiByte(CP_UTF8, 0, l.c_str(), -1, &u[0], (int)u.size(), nullptr, nullptr);
        u.back() = '\r'; u += '\n';
        log_text_ += u;
        HANDLE h = CreateFileW(log_file_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {   /* the folder may not exist yet: written in full once it does */
            if (!logged_all_) { DWORD p; WriteFile(h, log_text_.data(), (DWORD)log_text_.size(), &p, nullptr); logged_all_ = true; }
            else { DWORD p; WriteFile(h, u.data(), (DWORD)u.size(), &p, nullptr); }
            CloseHandle(h);
        }
    }
    bool fail(const std::wstring &e) { say(L"Not installed: " + e); return false; }
    void progress(double d) { s_.progress(d); }

    Options o_;
    Sink &s_;
    std::vector<std::wstring> installed_;
    std::wstring log_file_;
    std::string log_text_;
    bool logged_all_ = false;
};

/* ---------- the window ---------- */

enum { IdDriveRadio = 10, IdDrive, IdImageRadio, IdImage, IdImageBrowse, IdDest, IdDestBrowse, IdRip,
       IdProgress, IdLog, IdInstall, IdClose, IdPlay };
constexpr UINT WmLog = WM_APP + 1, WmProgress = WM_APP + 2, WmDone = WM_APP + 3;

HWND g_wnd;
HFONT g_font, g_bold;
std::atomic<bool> g_cancel{false};
bool g_busy;
int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }

HWND item(int id) { return GetDlgItem(g_wnd, id); }
std::wstring text_of(int id)
{
    int n = GetWindowTextLengthW(item(id));
    std::wstring s(n + 1, 0);
    GetWindowTextW(item(id), &s[0], n + 1);
    s.resize(n);
    return s;
}

HWND add(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") || !wcscmp(cls, L"LISTBOX") ? WS_EX_CLIENTEDGE : 0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}

void append_log(const std::wstring &l)
{
    HWND e = item(IdLog);
    int n = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, n, n);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)(l + L"\r\n").c_str());
}

void set_source_mode(bool drive)
{
    CheckRadioButton(g_wnd, IdDriveRadio, IdImageRadio, drive ? IdDriveRadio : IdImageRadio);
    EnableWindow(item(IdDrive), drive);
    EnableWindow(item(IdImage), !drive);
    EnableWindow(item(IdImageBrowse), !drive);
}

void browse_image()
{
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW of{ sizeof of };
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"Disc image (*.cue)\0*.cue\0All files\0*.*\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH * 2;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT;
    if (GetOpenFileNameW(&of)) SetWindowTextW(item(IdImage), file);
}

void browse_dest()
{
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void **)&d))) return;
    DWORD f; d->GetOptions(&f); d->SetOptions(f | FOS_PICKFOLDERS | FOS_DONTADDTORECENT);
    if (SUCCEEDED(d->Show(g_wnd))) {
        IShellItem *it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                std::wstring s = p;
                if (s.size() < 14 || _wcsicmp(s.c_str() + s.size() - 12, L"\\Daytona USA")) s += L"\\Daytona USA";
                SetWindowTextW(item(IdDest), s.c_str());
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}

void start_install()
{
    Options o;
    bool drive = IsDlgButtonChecked(g_wnd, IdDriveRadio) == BST_CHECKED;
    if (drive) {
        int sel = (int)SendMessageW(item(IdDrive), CB_GETCURSEL, 0, 0);
        wchar_t b[16] = L"";
        if (sel >= 0) SendMessageW(item(IdDrive), CB_GETLBTEXT, sel, (LPARAM)b);
        o.source = std::wstring(b).substr(0, 2);
    } else {
        o.source = text_of(IdImage);
    }
    o.dest = text_of(IdDest);
    while (!o.dest.empty() && (o.dest.back() == L'\\' || o.dest.back() == L' ')) o.dest.pop_back();
    o.rip = IsDlgButtonChecked(g_wnd, IdRip) == BST_CHECKED;
    if (o.source.empty()) { MessageBoxW(g_wnd, L"Choose your Daytona USA disc: a CD drive or a .cue disc image.", kAppName, MB_ICONINFORMATION); return; }
    if (o.dest.size() < 3 || o.dest[1] != L':') { MessageBoxW(g_wnd, L"Choose the folder to install to.", kAppName, MB_ICONINFORMATION); return; }
    g_busy = true;
    g_cancel = false;
    for (int id = IdDriveRadio; id <= IdRip; id++) EnableWindow(item(id), FALSE);
    EnableWindow(item(IdInstall), FALSE);
    ShowWindow(item(IdPlay), SW_HIDE);
    SetWindowTextW(item(IdClose), L"Cancel");
    SetWindowTextW(item(IdLog), L"");
    std::thread([o] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Sink s;
        s.log = [](const std::wstring &l) { PostMessageW(g_wnd, WmLog, 0, (LPARAM)new std::wstring(l)); };
        s.progress = [](double d) { PostMessageW(g_wnd, WmProgress, (WPARAM)(d * 1000), 0); };
        s.cancel = &g_cancel;
        Installer in(o, s);
        bool ok = in.run();
        CoUninitialize();
        PostMessageW(g_wnd, WmDone, ok, 0);
    }).detach();
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IdDriveRadio: set_source_mode(true); break;
        case IdImageRadio: set_source_mode(false); break;
        case IdImageBrowse: browse_image(); break;
        case IdDestBrowse: browse_dest(); break;
        case IdInstall: start_install(); break;
        case IdPlay: {
            std::wstring dir = text_of(IdDest);
            ShellExecuteW(h, nullptr, (dir + L"\\DAYTONA.EXE").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
            break;
        }
        case IdClose:
            if (g_busy) { g_cancel = true; append_log(L"Cancelling..."); }
            else DestroyWindow(h);
            break;
        }
        return 0;
    case WmLog: { std::wstring *s = (std::wstring *)l; append_log(*s); delete s; return 0; }
    case WmProgress: SendMessageW(item(IdProgress), PBM_SETPOS, w, 0); return 0;
    case WmDone:
        g_busy = false;
        SetWindowTextW(item(IdClose), L"Close");
        if (w) { ShowWindow(item(IdPlay), SW_SHOW); MessageBoxW(h, L"Daytona USA is installed.", kAppName, MB_ICONINFORMATION); }
        else {
            for (int id = IdDriveRadio; id <= IdRip; id++) EnableWindow(item(id), TRUE);
            set_source_mode(IsDlgButtonChecked(h, IdDriveRadio) == BST_CHECKED);
            EnableWindow(item(IdInstall), TRUE);
        }
        return 0;
    case WM_CLOSE:
        if (g_busy) { g_cancel = true; return 0; }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int run_window()
{
    INITCOMMONCONTROLSEX ic{ sizeof ic, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
    HDC dc = GetDC(nullptr); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW nm{ sizeof nm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
    g_font = CreateFontIndirectW(&nm.lfMessageFont);
    nm.lfMessageFont.lfWeight = FW_BOLD;
    nm.lfMessageFont.lfHeight = nm.lfMessageFont.lfHeight * 4 / 3;
    g_bold = CreateFontIndirectW(&nm.lfMessageFont);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"ultdaytonausa_setup";
    RegisterClassW(&wc);
    RECT r = { 0, 0, S(600), S(520) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowW(wc.lpszClassName, (L"Daytona USA installer (ultdaytonausa " + version() + L")").c_str(), style,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    add(L"STATIC", L"Daytona USA (PC, version 1.1)", 0, 16, 12, 560, 26, 0, g_bold);
    add(L"STATIC", L"Installs the game from your own disc or disc image, rips its music to lossless FLAC and adds the "
                   L"ultdaytonausa update (Windows 11 display, controllers, options). Nothing of the game is in this installer: "
                   L"everything comes from your disc.", 0, 16, 40, 568, 46, 0);
    add(L"BUTTON", L"Your disc", BS_GROUPBOX, 12, 90, 576, 82, 0);
    add(L"BUTTON", L"CD drive", BS_AUTORADIOBUTTON | WS_GROUP, 24, 110, 130, 22, IdDriveRadio);
    HWND cb = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 160, 109, 120, 200, IdDrive);
    add(L"BUTTON", L"Disc image (.cue)", BS_AUTORADIOBUTTON, 24, 138, 130, 22, IdImageRadio);
    add(L"EDIT", L"", ES_AUTOHSCROLL, 160, 137, 330, 24, IdImage);
    add(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 498, 136, 80, 26, IdImageBrowse);
    add(L"BUTTON", L"Install to", BS_GROUPBOX, 12, 178, 576, 56, 0);
    std::wstring def = L"C:\\Games\\Daytona USA";
    add(L"EDIT", def.c_str(), ES_AUTOHSCROLL, 24, 200, 466, 24, IdDest);
    add(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 498, 199, 80, 26, IdDestBrowse);
    add(L"BUTTON", L"Rip the CD music to FLAC (the game's music plays from these files)", BS_AUTOCHECKBOX, 16, 242, 560, 22, IdRip);
    add(L"STATIC", L"Portable: everything stays in this folder (no shortcuts, no registry). To remove the game, delete the folder.", 0, 16, 268, 568, 20, 0);
    CheckDlgButton(g_wnd, IdRip, BST_CHECKED);
    HWND pb = add(PROGRESS_CLASSW, L"", 0, 12, 296, 576, 18, IdProgress);
    SendMessageW(pb, PBM_SETRANGE32, 0, 1000);
    add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, 12, 320, 576, 156, IdLog);
    add(L"BUTTON", L"Play", BS_PUSHBUTTON, 296, 484, 92, 28, IdPlay);
    ShowWindow(item(IdPlay), SW_HIDE);
    add(L"BUTTON", L"Install", BS_DEFPUSHBUTTON, 396, 484, 92, 28, IdInstall);
    add(L"BUTTON", L"Close", BS_PUSHBUTTON, 496, 484, 92, 28, IdClose);
    std::vector<wchar_t> drives = cd_drives();
    for (wchar_t d : drives) { wchar_t s[8] = { d, L':', 0 }; SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)s); }
    if (!drives.empty()) SendMessageW(cb, CB_SETCURSEL, 0, 0);
    set_source_mode(!drives.empty());
    if (drives.empty()) EnableWindow(item(IdDriveRadio), FALSE);
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(g_wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    PWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Options o;
    bool cmdline = false, uninst = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        for (auto &c : a) c = towlower(c);
        auto next = [&] { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"/source") { o.source = next(); cmdline = true; }
        else if (a == L"/dest") { o.dest = next(); cmdline = true; }
        else if (a == L"/silent") o.silent = true;
        else if (a == L"/norip") o.rip = false;
        else if (a == L"/uninstall") uninst = true;
    }
    LocalFree(argv);
    int rc;
    if (uninst) {   /* from 0.0.22 there is no uninstaller: Windows' entry left by an earlier build may still call this */
        if (!o.silent) MessageBoxW(nullptr, L"This build of Daytona USA is portable and has no uninstaller.\n\n"
                                            L"To remove the game, delete its folder. To remove what earlier installers left "
                                            L"(the installed apps entry and shortcuts), run ultdaytonausa-cleanup.cmd.", kAppName, MB_ICONINFORMATION);
        rc = 1;
    }
    else if (cmdline && o.silent) {
        while (!o.dest.empty() && o.dest.back() == L'\\') o.dest.pop_back();
        Sink s;
        s.log = [](const std::wstring &) {};   /* the install log file has every line */
        s.progress = [](double) {};
        Installer in(o, s);
        rc = in.run() ? 0 : 1;
    } else {
        rc = run_window();
    }
    CoUninitialize();
    return rc;
}
