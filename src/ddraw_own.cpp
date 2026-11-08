/* Our own DirectDraw ([Proxy] OwnDirectDraw=1).
 *
 * The game uses a small part of DirectDraw 1: a primary surface with a flip chain of back buffers, 640x400
 * off-screen pages for the scrolling sky, Lock/Unlock (it keeps the surface pointers after unlocking), GetDC for
 * a pixel-format test, colour-fill and copy blits, and Flip. This file implements that part with plain
 * system-memory surfaces (DIB sections, RGB565) and shows each flipped frame in the game window ourselves,
 * so Windows' DirectDraw, exclusive mode and display-mode changes are no longer involved.
 *
 * Presentation in this build is GDI (StretchDIBits), scaled to 4:3 and centred, as a stepping stone; Direct3D 11
 * replaces it in 0.0.9.
 *
 * Flip copies the back buffer to the screen and into the primary's memory; the back buffer's memory never moves,
 * because the game keeps writing through the pointer it got from Lock.
 */
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include <set>
#include "proxy.h"
#include "ddraw_own.h"
#include "hardening.h"
#include "trace.h"

namespace {

constexpr DWORD kCapsPrimary = 0x8A38;   /* as reported by Windows' software DirectDraw for this game */
constexpr DWORD kCapsBack1   = 0x081C;
constexpr DWORD kCapsBackN   = 0x0818;
constexpr DWORD kSysMem      = DDSCAPS_SYSTEMMEMORY;

OwnDDConfig g_cfg;

void fill_pixel_format(DDPIXELFORMAT &pf)
{
    memset(&pf, 0, sizeof pf);
    pf.dwSize = sizeof pf;
    pf.dwFlags = DDPF_RGB;
    pf.dwRGBBitCount = 16;
    pf.dwRBitMask = 0xF800; pf.dwGBitMask = 0x07E0; pf.dwBBitMask = 0x001F;
}

struct Bmi565 { BITMAPINFOHEADER h; DWORD masks[3]; };

void make_bmi(Bmi565 &b, int w, int h)
{
    memset(&b, 0, sizeof b);
    b.h.biSize = sizeof b.h; b.h.biWidth = w; b.h.biHeight = -h; b.h.biPlanes = 1;
    b.h.biBitCount = 16; b.h.biCompression = BI_BITFIELDS;
    b.masks[0] = 0xF800; b.masks[1] = 0x07E0; b.masks[2] = 0x001F;
}

class OwnDD;
OwnDD *g_current;   /* the DirectDraw object presenting now */

class OwnSurface final : public IDirectDrawSurface {
public:
    OwnSurface(OwnDD *dd, int w, int h, DWORD caps);
    ~OwnSurface();
    bool ok() const { return bits_ != nullptr; }

    /* IUnknown */
    STDMETHOD(QueryInterface)(REFIID riid, void **out) override;
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&refs_); }
    STDMETHOD_(ULONG, Release)() override;
    /* IDirectDrawSurface */
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(AddOverlayDirtyRect)(LPRECT) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(Blt)(LPRECT dst, LPDIRECTDRAWSURFACE src, LPRECT srcrc, DWORD flags, LPDDBLTFX fx) override;
    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcrc, DWORD trans) override;
    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(EnumAttachedSurfaces)(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) override;
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE override_target, DWORD flags) override;
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS caps, LPDIRECTDRAWSURFACE *out) override;
    STDMETHOD(GetBltStatus)(DWORD) override { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDSCAPS caps) override { if (!caps) return DDERR_INVALIDPARAMS; caps->dwCaps = caps_; return DD_OK; }
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER *c) override { if (c) *c = nullptr; return DDERR_NOCLIPPERATTACHED; }
    STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
    STDMETHOD(GetDC)(HDC *dc) override;
    STDMETHOD(GetFlipStatus)(DWORD) override { return DD_OK; }
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) override { return DDERR_NOTAOVERLAYSURFACE; }
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE *p) override { if (p) *p = nullptr; return DDERR_NOPALETTEATTACHED; }
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT pf) override { if (!pf) return DDERR_INVALIDPARAMS; fill_pixel_format(*pf); return DD_OK; }
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC d) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(IsLost)() override { return DD_OK; }
    STDMETHOD(Lock)(LPRECT rc, LPDDSURFACEDESC d, DWORD flags, HANDLE ev) override;
    STDMETHOD(ReleaseDC)(HDC dc) override;
    STDMETHOD(Restore)() override { return DD_OK; }
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER) override { return DD_OK; }
    STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
    STDMETHOD(SetOverlayPosition)(LONG, LONG) override { return DDERR_NOTAOVERLAYSURFACE; }
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(Unlock)(LPVOID) override;
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) override { return DDERR_NOTAOVERLAYSURFACE; }
    STDMETHOD(UpdateOverlayDisplay)(DWORD) override { return DDERR_NOTAOVERLAYSURFACE; }
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE) override { return DDERR_NOTAOVERLAYSURFACE; }

    int width() const { return w_; }
    int height() const { return h_; }
    int pitch() const { return pitch_; }
    BYTE *bits() const { return bits_; }
    WORD *row(int y) const { return (WORD *)(bits_ + y * pitch_); }
    bool is_primary() const { return (caps_ & DDSCAPS_PRIMARYSURFACE) != 0; }
    void attach(OwnSurface *next) { next_ = next; }
    OwnSurface *next() const { return next_; }

private:
    void after_write_to_primary();

    LONG refs_ = 1;
    OwnDD *dd_;
    int w_, h_, pitch_;
    DWORD caps_;
    BYTE *bits_ = nullptr;
    HBITMAP dib_ = nullptr;
    HDC dc_ = nullptr;
    HGDIOBJ dc_old_ = nullptr;
    int locks_ = 0;
    OwnSurface *next_ = nullptr;     /* the next surface in a flip chain (primary -> back buffer 1 -> 2 ...) */
    bool has_src_key_ = false;
    DDCOLORKEY src_key_ = {};
};

class OwnDD final : public IDirectDraw {
    friend void ::own_display_config(const DisplayConfig &d);
public:
    STDMETHOD(QueryInterface)(REFIID riid, void **out) override;
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&refs_); }
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Compact)() override { return DD_OK; }
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER *, IUnknown *) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE *, IUnknown *) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE *out, IUnknown *outer) override;
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE *) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(EnumDisplayModes)(DWORD flags, LPDDSURFACEDESC d, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override;
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(FlipToGDISurface)() override { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDCAPS drv, LPDDCAPS hel) override;
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC d) override;
    STDMETHOD(GetFourCCCodes)(LPDWORD n, LPDWORD) override { if (n) *n = 0; return DD_OK; }
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE *s) override;
    STDMETHOD(GetMonitorFrequency)(LPDWORD f) override { if (f) *f = 60; return DD_OK; }
    STDMETHOD(GetScanLine)(LPDWORD) override { return DDERR_UNSUPPORTED; }
    STDMETHOD(GetVerticalBlankStatus)(WINBOOL *v) override { if (v) *v = TRUE; return DD_OK; }
    STDMETHOD(Initialize)(GUID *) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(RestoreDisplayMode)() override { return DD_OK; }
    STDMETHOD(SetCooperativeLevel)(HWND h, DWORD flags) override;
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp) override;
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) override { return DD_OK; }

    void present(const OwnSurface *s, bool vsync);
    void primary_gone(OwnSurface *s) { if (primary_ == s) primary_ = nullptr; }
    int mode_w() const { return mode_w_; }
    int mode_h() const { return mode_h_; }

private:
    LONG refs_ = 1;
    HWND hwnd_ = nullptr;
    int mode_w_ = 640, mode_h_ = 480;
    OwnSurface *primary_ = nullptr;
    LONG presents_ = 0;
    Presenter *presenter_ = nullptr;
    int lost_ = 0;   /* R2-9: Direct3D 11 devices lost so far */
    bool presenter_failed_ = false;
};

/* ---------- surface ---------- */

static std::set<const void *> g_surfaces;   /* every live OwnSurface (own_surface_bits checks against it) */

OwnSurface::OwnSurface(OwnDD *dd, int w, int h, DWORD caps) : dd_(dd), w_(w), h_(h), caps_(caps)
{
    g_surfaces.insert(static_cast<IDirectDrawSurface *>(this));
    Bmi565 b; make_bmi(b, w, h);
    void *p = nullptr;
    dib_ = CreateDIBSection(nullptr, (BITMAPINFO *)&b, DIB_RGB_COLORS, &p, nullptr, 0);
    pitch_ = ((w * 2) + 3) & ~3;
    bits_ = (BYTE *)p;
    if (bits_) memset(bits_, 0, (size_t)pitch_ * h);
}

OwnSurface::~OwnSurface()
{
    g_surfaces.erase(static_cast<IDirectDrawSurface *>(this));
    if (dc_) { SelectObject(dc_, dc_old_); DeleteDC(dc_); }
    if (dib_) DeleteObject(dib_);
    if (is_primary()) dd_->primary_gone(this);
}

STDMETHODIMP OwnSurface::QueryInterface(REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectDrawSurface)) { *out = this; AddRef(); return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) OwnSurface::Release()
{
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) {
        if (is_primary()) {   /* the flip chain goes with the primary, as in DirectDraw */
            OwnSurface *s = next_;
            while (s) { OwnSurface *n = s->next_; s->next_ = nullptr; s->Release(); s = n; }
        }
        delete this;
    }
    return (ULONG)(r < 0 ? 0 : r);
}

STDMETHODIMP OwnSurface::GetSurfaceDesc(LPDDSURFACEDESC d)
{
    if (!d || d->dwSize < sizeof(DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = w_; d->dwHeight = h_; d->lPitch = pitch_;
    d->ddsCaps.dwCaps = caps_;
    fill_pixel_format(d->ddpfPixelFormat);
    if (is_primary()) {
        int n = 0; for (OwnSurface *s = next_; s; s = s->next_) n++;
        if (n) { d->dwFlags |= DDSD_BACKBUFFERCOUNT; d->dwBackBufferCount = n; }
    }
    if (has_src_key_) { d->dwFlags |= DDSD_CKSRCBLT; d->ddckCKSrcBlt = src_key_; }
    return DD_OK;
}

STDMETHODIMP OwnSurface::Lock(LPRECT rc, LPDDSURFACEDESC d, DWORD, HANDLE)
{
    if (!d) return DDERR_INVALIDPARAMS;
    GdiFlush();                       /* GDI drawing through GetDC must be in memory first */
    HRESULT r = GetSurfaceDesc(d);
    if (FAILED(r)) return r;
    BYTE *p = bits_;
    if (rc) p += rc->top * pitch_ + rc->left * 2;
    d->lpSurface = p;
    d->dwFlags |= DDSD_LPSURFACE;
    locks_++;
    return DD_OK;
}

STDMETHODIMP OwnSurface::Unlock(LPVOID)
{
    if (locks_ <= 0) return DDERR_NOTLOCKED;
    locks_--;
    if (is_primary()) after_write_to_primary();
    return DD_OK;
}

STDMETHODIMP OwnSurface::GetDC(HDC *out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    if (dc_) return DDERR_DCALREADYCREATED;
    dc_ = CreateCompatibleDC(nullptr);
    if (!dc_) return DDERR_CANTCREATEDC;
    dc_old_ = SelectObject(dc_, dib_);
    *out = dc_;
    return DD_OK;
}

STDMETHODIMP OwnSurface::ReleaseDC(HDC dc)
{
    if (!dc_ || dc != dc_) return DDERR_NODC;
    GdiFlush();
    SelectObject(dc_, dc_old_);
    DeleteDC(dc_);
    dc_ = nullptr;
    if (is_primary()) after_write_to_primary();
    return DD_OK;
}

STDMETHODIMP OwnSurface::SetColorKey(DWORD flags, LPDDCOLORKEY key)
{
    if (flags & DDCKEY_SRCBLT) {
        has_src_key_ = key != nullptr;
        if (key) src_key_ = *key;
        return DD_OK;
    }
    return DDERR_UNSUPPORTED;
}

STDMETHODIMP OwnSurface::GetColorKey(DWORD flags, LPDDCOLORKEY key)
{
    if (!key) return DDERR_INVALIDPARAMS;
    if ((flags & DDCKEY_SRCBLT) && has_src_key_) { *key = src_key_; return DD_OK; }
    return DDERR_NOCOLORKEY;
}

STDMETHODIMP OwnSurface::GetAttachedSurface(LPDDSCAPS caps, LPDIRECTDRAWSURFACE *out)
{
    if (!caps || !out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if ((caps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP)) && next_) {
        next_->AddRef();
        *out = next_;
        return DD_OK;
    }
    return DDERR_NOTFOUND;
}

STDMETHODIMP OwnSurface::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    if (next_) {
        DDSURFACEDESC d; d.dwSize = sizeof d; next_->GetSurfaceDesc(&d);
        next_->AddRef();
        cb(next_, &d, ctx);
    }
    return DD_OK;
}

/* Copy or colour-fill a rectangle; source and destination are both RGB565. */
static void clip_rect(RECT &r, int w, int h)
{
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > w) r.right = w;
    if (r.bottom > h) r.bottom = h;
}

STDMETHODIMP OwnSurface::Blt(LPRECT dstp, LPDIRECTDRAWSURFACE srcp, LPRECT srcrcp, DWORD flags, LPDDBLTFX fx)
{
    RECT dr = dstp ? *dstp : RECT{ 0, 0, w_, h_ };
    if (flags & DDBLT_COLORFILL) {
        if (!fx) return DDERR_INVALIDPARAMS;
        clip_rect(dr, w_, h_);
        WORD c = (WORD)fx->dwFillColor;
        for (int y = dr.top; y < dr.bottom; y++) {
            WORD *p = row(y) + dr.left;
            for (int x = dr.left; x < dr.right; x++) *p++ = c;
        }
        if (is_primary()) after_write_to_primary();
        return DD_OK;
    }
    if (!srcp) return DDERR_INVALIDPARAMS;
    OwnSurface *src = static_cast<OwnSurface *>(srcp);
    RECT sr = srcrcp ? *srcrcp : RECT{ 0, 0, src->w_, src->h_ };
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top, sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return DD_OK;
    bool key = false; WORD keylo = 0, keyhi = 0;
    if (flags & DDBLT_KEYSRCOVERRIDE) { if (!fx) return DDERR_INVALIDPARAMS; key = true; keylo = (WORD)fx->ddckSrcColorkey.dwColorSpaceLowValue; keyhi = (WORD)fx->ddckSrcColorkey.dwColorSpaceHighValue; }
    else if ((flags & DDBLT_KEYSRC) && src->has_src_key_) { key = true; keylo = (WORD)src->src_key_.dwColorSpaceLowValue; keyhi = (WORD)src->src_key_.dwColorSpaceHighValue; }
    if (keyhi < keylo) keyhi = keylo;
    for (int y = 0; y < dh; y++) {
        int ty = dr.top + y; if (ty < 0 || ty >= h_) continue;
        int syy = sr.top + (int)((long long)y * sh / dh); if (syy < 0 || syy >= src->h_) continue;
        WORD *d = row(ty); const WORD *s = src->row(syy);
        for (int x = 0; x < dw; x++) {
            int tx = dr.left + x; if (tx < 0 || tx >= w_) continue;
            int sx = sr.left + (int)((long long)x * sw / dw); if (sx < 0 || sx >= src->w_) continue;
            WORD v = s[sx];
            if (key && v >= keylo && v <= keyhi) continue;
            d[tx] = v;
        }
    }
    if (is_primary()) after_write_to_primary();
    return DD_OK;
}

STDMETHODIMP OwnSurface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE srcp, LPRECT srcrcp, DWORD trans)
{
    if (!srcp) return DDERR_INVALIDPARAMS;
    OwnSurface *src = static_cast<OwnSurface *>(srcp);
    RECT sr = srcrcp ? *srcrcp : RECT{ 0, 0, src->w_, src->h_ };
    RECT dr = { (LONG)x, (LONG)y, (LONG)x + (sr.right - sr.left), (LONG)y + (sr.bottom - sr.top) };
    DWORD flags = 0;
    if (trans & DDBLTFAST_SRCCOLORKEY) flags |= DDBLT_KEYSRC;
    return Blt(&dr, srcp, &sr, flags, nullptr);
}

STDMETHODIMP OwnSurface::Flip(LPDIRECTDRAWSURFACE, DWORD)
{
    if (!is_primary() || !next_) return DDERR_NOTFLIPPABLE;
    OwnSurface *back = next_;
    frame_capture(back->bits_, back->w_, back->h_, back->pitch_);
    for (int y = 0; y < h_ && y < back->h_; y++) memcpy(row(y), back->row(y), (size_t)(w_ < back->w_ ? w_ : back->w_) * 2);
    dd_->present(this, true);
    return DD_OK;
}

void OwnSurface::after_write_to_primary() { dd_->present(this, false); }

bool surface_bits(const void *surface, const BYTE **bits, int *w, int *h, int *pitch)
{
    if (!surface || !g_surfaces.count(surface)) return false;
    const OwnSurface *s = static_cast<const OwnSurface *>((const IDirectDrawSurface *)surface);
    if (!s->bits()) return false;
    *bits = s->bits(); *w = s->width(); *h = s->height(); *pitch = s->pitch();
    return true;
}

/* ---------- DirectDraw object ---------- */

STDMETHODIMP OwnDD::QueryInterface(REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectDraw)) { *out = this; AddRef(); return S_OK; }
    *out = nullptr;
    plog("own ddraw: QueryInterface for an interface we do not provide");
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) OwnDD::Release()
{
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) { plog("own ddraw: released after %ld frames shown", presents_); if (g_current == this) g_current = nullptr; delete presenter_; delete this; }
    return (ULONG)(r < 0 ? 0 : r);
}

STDMETHODIMP OwnDD::SetCooperativeLevel(HWND h, DWORD flags)
{
    hwnd_ = h;
    plog("own ddraw: SetCooperativeLevel(hwnd=%p, flags=0x%lX)", (void *)h, flags);
    proxy_on_cooperative_level(h, flags);
    return DD_OK;
}

STDMETHODIMP OwnDD::SetDisplayMode(DWORD w, DWORD h, DWORD bpp)
{
    /* Same answers as Windows' software DirectDraw gave the game: no 320-wide modes, 16-bit 640x480 and 640x400. */
    HRESULT r = (bpp == 16 && w == 640 && (h == 480 || h == 400)) ? DD_OK : (HRESULT)0x80004001;
    if (r == DD_OK) { mode_w_ = (int)w; mode_h_ = (int)h; }
    plog("own ddraw: SetDisplayMode(%lux%lux%lu) = 0x%08lX (no real mode change)", w, h, bpp, (unsigned long)r);
    return r;
}

STDMETHODIMP OwnDD::GetDisplayMode(LPDDSURFACEDESC d)
{
    if (!d) return DDERR_INVALIDPARAMS;
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d->dwWidth = mode_w_; d->dwHeight = mode_h_; d->lPitch = mode_w_ * 2; d->dwRefreshRate = 60;
    fill_pixel_format(d->ddpfPixelFormat);
    return DD_OK;
}

STDMETHODIMP OwnDD::EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID ctx, LPDDENUMMODESCALLBACK cb)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    static const int modes[][2] = { { 640, 400 }, { 640, 480 } };
    for (auto &m : modes) {
        DDSURFACEDESC d; memset(&d, 0, sizeof d); d.dwSize = sizeof d;
        d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        d.dwWidth = m[0]; d.dwHeight = m[1]; d.lPitch = m[0] * 2; d.dwRefreshRate = 60;
        fill_pixel_format(d.ddpfPixelFormat);
        if (cb(&d, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}

STDMETHODIMP OwnDD::GetCaps(LPDDCAPS drv, LPDDCAPS hel)
{
    for (LPDDCAPS c : { drv, hel }) {
        if (!c) continue;
        DWORD size = c->dwSize ? c->dwSize : sizeof(DDCAPS);
        memset(c, 0, size);
        c->dwSize = size;
        c->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM;
        c->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        c->ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER |
                            DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY;
    }
    return DD_OK;
}

STDMETHODIMP OwnDD::GetGDISurface(LPDIRECTDRAWSURFACE *s)
{
    if (!s) return DDERR_INVALIDPARAMS;
    *s = primary_;
    if (!primary_) return DDERR_NOTFOUND;
    primary_->AddRef();
    return DD_OK;
}

STDMETHODIMP OwnDD::CreateSurface(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE *out, IUnknown *)
{
    if (!d || !out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
    OwnSurface *s = nullptr;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s = new OwnSurface(this, mode_w_, mode_h_, kCapsPrimary);
        if (!s->ok()) { delete s; return DDERR_OUTOFMEMORY; }
        int n = (d->dwFlags & DDSD_BACKBUFFERCOUNT) ? (int)d->dwBackBufferCount : 0;
        OwnSurface *prev = s;
        for (int i = 0; i < n; i++) {
            OwnSurface *b = new OwnSurface(this, mode_w_, mode_h_, i == 0 ? kCapsBack1 : kCapsBackN);
            if (!b->ok()) { delete b; s->Release(); return DDERR_OUTOFMEMORY; }
            prev->attach(b);
            prev = b;
        }
        primary_ = s;
        if (trace_enabled()) {
            trace_name_object(s, "own primary");
            int i = 1;
            for (OwnSurface *b = s; (b = b->next()) != nullptr; i++) { char nm[32]; snprintf(nm, sizeof nm, "own back buffer %d", i); trace_name_object(b, nm); }
            trace_surface(s);
        }
        plog("own ddraw: primary %dx%d with %d back buffers -> %p", mode_w_, mode_h_, n, (void *)s);
    } else {
        if (!(d->dwFlags & DDSD_WIDTH) || !(d->dwFlags & DDSD_HEIGHT) || !d->dwWidth || !d->dwHeight) return DDERR_INVALIDPARAMS;
        s = new OwnSurface(this, (int)d->dwWidth, (int)d->dwHeight, (caps & ~DDSCAPS_VIDEOMEMORY) | kSysMem);
        if (!s->ok()) { delete s; return DDERR_OUTOFMEMORY; }
        if (trace_enabled()) { char nm[48]; snprintf(nm, sizeof nm, "own surface %lux%lu", d->dwWidth, d->dwHeight); trace_name_object(s, nm); trace_surface(s); }
    }
    if ((d->dwFlags & DDSD_CKSRCBLT)) s->SetColorKey(DDCKEY_SRCBLT, &d->ddckCKSrcBlt);
    *out = s;
    return DD_OK;
}

void OwnDD::present(const OwnSurface *s, bool vsync)
{
    g_current = this;
    presents_++;
    if (!hwnd_ || !IsWindow(hwnd_) || IsIconic(hwnd_)) return;
    if (presenter_ && presenter_->lost()) {   /* R2-9: device removed or reset: a new presenter (GDI after 3 losses) */
        present_device_lost();                /* the new renderer lets go of the old device first */
        delete presenter_; presenter_ = nullptr;
        if (++lost_ >= 3) { plog("display: the Direct3D 11 device was lost %d times; using GDI", lost_); g_cfg.display.d3d11 = false; }
    }
    if (!presenter_) {
        if (g_cfg.display.d3d11) presenter_ = make_d3d11_presenter(hwnd_, g_cfg.display);
        if (!presenter_) {
            if (g_cfg.display.d3d11) plog("display: Direct3D 11 not available, using GDI");
            presenter_ = make_gdi_presenter(g_cfg.display);
        }
        plog("display: presenting with %s", presenter_->name());
    }
    presenter_->present(hwnd_, s->bits(), s->width(), s->height(), s->pitch(), vsync);
}

/* ---------- frame capture ---------- */

DWORD g_crc_table[256];
LONG g_frames;
int g_dumped;

void crc_init()
{
    for (DWORD i = 0; i < 256; i++) {
        DWORD c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
}

DWORD crc32(const BYTE *p, size_t n, DWORD c)
{
    c = ~c;
    while (n--) c = g_crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

void dump_bmp(const BYTE *bits, int w, int h, int pitch, LONG frame)
{
    char dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char *e = strrchr(dir, '\\'); if (e) e[1] = 0;
    strncat(dir, "logs\\frames", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, nullptr);
    snprintf(path, sizeof path, "%s\\frame_%06ld.bmp", dir, frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    Bmi565 b; make_bmi(b, w, h);
    int row = w * 2;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof b;
    fh.bfSize = fh.bfOffBits + row * h;
    b.h.biSizeImage = row * h;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&b, sizeof b, 1, f);
    for (int y = 0; y < h; y++) fwrite(bits + y * pitch, row, 1, f);
    fclose(f);
}

} // namespace

void own_display_config(const DisplayConfig &d)
{
    g_cfg.display = d;
    if (g_current && g_current->presenter_) g_current->presenter_->set_config(d);
}



void frame_capture_config(const OwnDDConfig &cfg)
{
    g_cfg = cfg;
    crc_init();
}

void frame_capture(const BYTE *bits, int w, int h, int pitch)
{
    LONG n = InterlockedIncrement(&g_frames);
    health_note_frame();
#ifdef ULT_DEBUG
    crash_test_tick();
#endif
    if (!bits) return;
    if (g_cfg.frame_hash && n <= g_cfg.frame_hash) {
        DWORD c = 0;
        for (int y = 0; y < h; y++) c = crc32(bits + y * pitch, (size_t)w * 2, c);
        plog("frame %ld %dx%d crc %08lX", n, w, h, c);
    }
    if (g_cfg.frame_dump_every > 0 && n % g_cfg.frame_dump_every == 0 && g_dumped < g_cfg.frame_dump_max) {
        g_dumped++;
        dump_bmp(bits, w, h, pitch, n);
    }
}

HRESULT own_directdraw_create(LPDIRECTDRAW *out, const OwnDDConfig &cfg)
{
    g_cfg = cfg;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new OwnDD();
    return DD_OK;
}

bool own_surface_bits(const void *surface, const BYTE **bits, int *w, int *h, int *pitch)
{
    return surface_bits(surface, bits, w, h, pitch);
}
