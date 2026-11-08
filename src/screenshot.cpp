/* Screenshots (Release 2, jayyanz 7 Nov 2026): Page Down saves the picture as shown (the new renderer's resolution,
 * the HUD and any CRT filter included) as a PNG in the game folder's `screenshots` folder.
 *
 * The presenter copies the picture's pixels on the game thread (a copy only); a worker thread encodes the PNG with
 * Windows' own encoder (WIC) and writes it, so the game does not wait for the file. One screenshot at a time: a press
 * while one is still being written is ignored.
 */
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "proxy.h"
#include "screenshot.h"

namespace {

/* GUIDs defined here, so no extra import library is needed */
const GUID kClsidWicFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
const GUID kIidWicFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
const GUID kContainerPng = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
const GUID kFormat24bppBGR = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c } };

volatile LONG g_busy = 0;
bool g_key_down = false;

struct Shot { std::vector<BYTE> bgr; int w, h; char path[MAX_PATH]; std::vector<BYTE> depth; char depth_path[MAX_PATH]; };

template <typename T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

bool encode_png(const Shot &s, HRESULT &why)
{
    IWICImagingFactory *f = nullptr; IWICStream *st = nullptr; IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr; IPropertyBag2 *props = nullptr;
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, s.path, -1, wpath, MAX_PATH);
    WICPixelFormatGUID fmt = kFormat24bppBGR;
    HRESULT r = CoCreateInstance(kClsidWicFactory, nullptr, CLSCTX_INPROC_SERVER, kIidWicFactory, (void **)&f);
    if (SUCCEEDED(r)) r = f->CreateStream(&st);
    if (SUCCEEDED(r)) r = st->InitializeFromFilename(wpath, GENERIC_WRITE);
    if (SUCCEEDED(r)) r = f->CreateEncoder(kContainerPng, nullptr, &enc);
    if (SUCCEEDED(r)) r = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(r)) r = enc->CreateNewFrame(&fr, &props);
    if (SUCCEEDED(r)) r = fr->Initialize(props);
    if (SUCCEEDED(r)) r = fr->SetSize(s.w, s.h);
    if (SUCCEEDED(r)) r = fr->SetPixelFormat(&fmt);
    if (SUCCEEDED(r) && memcmp(&fmt, &kFormat24bppBGR, sizeof fmt)) r = E_FAIL;
    if (SUCCEEDED(r)) r = fr->WritePixels(s.h, s.w * 3, (UINT)s.bgr.size(), (BYTE *)s.bgr.data());
    if (SUCCEEDED(r)) r = fr->Commit();
    if (SUCCEEDED(r)) r = enc->Commit();
    release(props); release(fr); release(enc); release(st); release(f);
    why = r;
    return SUCCEEDED(r);
}

DWORD WINAPI writer(void *arg)
{
    Shot *s = (Shot *)arg;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT why = S_OK;
    if (encode_png(*s, why)) plog("screenshot: %s (%dx%d)", s->path, s->w, s->h);
    else { plog("screenshot: could not write %s (0x%08lX)", s->path, (unsigned long)why); DeleteFileA(s->path); }
    if (!s->depth.empty()) {   /* 1.1.18: the depth buffer as ReShade reads it, near black, far white */
        s->bgr.swap(s->depth); lstrcpyA(s->path, s->depth_path);
        if (encode_png(*s, why)) plog("screenshot: %s (the depth buffer)", s->path);
        else { plog("screenshot: could not write %s (0x%08lX)", s->path, (unsigned long)why); DeleteFileA(s->path); }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    delete s;
    InterlockedExchange(&g_busy, 0);
    return 0;
}

} // namespace

bool screenshot_wanted(HWND hwnd)
{
#ifdef ULT_DEBUG
    {   /* tests (debug build): ULT_SHOT_FRAMES=n,m,... takes a screenshot at those frames shown (no keyboard focus needed) */
        static unsigned frames = 0; static const char *list = getenv("ULT_SHOT_FRAMES");
        frames++;
        if (list && g_busy == 0) {
            for (const char *q = list; *q;) {
                if ((unsigned)strtoul(q, nullptr, 10) == frames) return true;
                while (*q && *q != ',') q++;
                if (*q == ',') q++;
            }
        }
    }
#endif
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    const bool ours = fg && (fg == hwnd || (GetWindowThreadProcessId(fg, &pid), pid == GetCurrentProcessId()));
    const bool down = ours && (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
    const bool pressed = down && !g_key_down;
    g_key_down = down;
    return pressed && g_busy == 0;
}

void screenshot_save(const BYTE *bits, int w, int h, int pitch, bool rgb565, const BYTE *grey, int grey_pitch)
{
    if (w <= 0 || h <= 0 || InterlockedExchange(&g_busy, 1)) return;
    Shot *s = new Shot;
    s->w = w; s->h = h;
    s->bgr.resize((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        BYTE *o = &s->bgr[(size_t)y * w * 3];
        if (rgb565) {
            const WORD *q = (const WORD *)(bits + (size_t)y * pitch);
            for (int x = 0; x < w; x++) {
                const unsigned v = q[x], r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                o[3 * x] = (BYTE)((b << 3) | (b >> 2)); o[3 * x + 1] = (BYTE)((g << 2) | (g >> 4)); o[3 * x + 2] = (BYTE)((r << 3) | (r >> 2));
            }
        } else {
            const BYTE *q = bits + (size_t)y * pitch;
            for (int x = 0; x < w; x++) { o[3 * x] = q[4 * x]; o[3 * x + 1] = q[4 * x + 1]; o[3 * x + 2] = q[4 * x + 2]; }
        }
    }
    /* <game folder>\screenshots\daytona_YYYYMMDD_HHMMSS_mmm.png */
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, dir, MAX_PATH);
    while (n > 0 && dir[n - 1] != '\\' && dir[n - 1] != '/') n--;
    dir[n] = 0;
    lstrcatA(dir, "screenshots");
    CreateDirectoryA(dir, nullptr);
    SYSTEMTIME t; GetLocalTime(&t);
    _snprintf(s->path, MAX_PATH, "%s\\daytona_%04d%02d%02d_%02d%02d%02d_%03d.png", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
              t.wSecond, t.wMilliseconds);
    s->path[MAX_PATH - 1] = 0;
    if (grey) {
        s->depth.resize((size_t)w * h * 3);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) memset(&s->depth[((size_t)y * w + x) * 3], grey[(size_t)y * grey_pitch + x], 3);
        lstrcpynA(s->depth_path, s->path, MAX_PATH - 10);
        char *dot = strrchr(s->depth_path, '.'); if (dot) *dot = 0;
        lstrcatA(s->depth_path, "_depth.png");
    }
    HANDLE th = CreateThread(nullptr, 0, writer, s, 0, nullptr);
    if (th) CloseHandle(th);
    else { plog("screenshot: no thread"); delete s; InterlockedExchange(&g_busy, 0); }
}
