/* Presenting frames: Direct3D 11 (default) or GDI (fallback).
 *
 * Each frame the game shows (RGB565, normally 640x480 with the picture in the middle 400 rows) is converted to
 * BGRA, uploaded to a texture and drawn into the window's swap chain with one full-screen triangle. The pixel
 * shader does the scaling: nearest, sharp-bilinear (nearest-looking but without uneven pixel widths, smoothing only
 * the edges between source pixels) or integer (nearest, at the largest whole multiple that fits).
 *
 * The swap chain uses the flip model where Windows supports it (Windows 10 and 11), with one frame of latency, and
 * presents with vsync when the game flips, as a full-screen DirectDraw flip did. Direct3D 11 and the shader compiler
 * are loaded at run time, so a missing piece falls back to GDI instead of stopping the game.
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "proxy.h"
#include "present.h"

Picture resolve_picture(HWND hwnd, const DisplayConfig &cfg)
{
    (void)hwnd;
    return cfg.picture;
}

Placement place_picture(int cw, int ch, int fw, int fh, const DisplayConfig &cfg)
{
    Placement p{};
    int ph = fh, py = 0;
    if (cfg.picture != Picture::Frame && fw == 640 && fh == 480) { ph = 400; py = 40; }   /* the picture rows only */
    p.src_y = py; p.src_h = ph;
    if (cw <= 0 || ch <= 0) return p;
    int w, h;
    if (cfg.picture == Picture::Native) {
        /* square pixels at the source's own shape (640x400: 16:10) */
        int k = cw / fw < ch / ph ? cw / fw : ch / ph;
        if (cfg.scaling == Scaling::Integer && k >= 1) { w = fw * k; h = ph * k; }
        else { w = cw; h = (int)((long long)cw * ph / fw); if (h > ch) { h = ch; w = (int)((long long)ch * fw / ph); } }
        p.x = (cw - w) / 2; p.y = (ch - h) / 2; p.w = w; p.h = h;
        return p;
    }
    /* Frame (640x480, square pixels) and Fill (640x400 stretched): a 4:3 area */
    if (cfg.scaling == Scaling::Integer) {
        int k = cw / fw < ch / ph ? cw / fw : ch / ph;
        if (cfg.picture == Picture::Fill) {           /* whole multiples of the 400 rows; width follows 4:3 */
            k = ch / ph;
            while (k > 1 && ph * k * 4 / 3 > cw) k--;
        }
        if (k >= 1) {
            h = ph * k;
            w = (cfg.picture == Picture::Fill) ? h * 4 / 3 : fw * k;
            p.x = (cw - w) / 2; p.y = (ch - h) / 2; p.w = w; p.h = h;
            return p;
        }
    }
    w = cw; h = cw * 3 / 4;
    if (h > ch) { h = ch; w = ch * 4 / 3; }
    p.x = (cw - w) / 2; p.y = (ch - h) / 2; p.w = w; p.h = h;
    return p;
}

namespace {

/* ---------- GDI ---------- */

struct Bmi565 { BITMAPINFOHEADER h; DWORD masks[3]; };

class GdiPresenter final : public Presenter {
public:
    explicit GdiPresenter(const DisplayConfig &c) : cfg_(c) {}
    const char *name() const override { return "GDI"; }
    void present(HWND hwnd, const BYTE *bits, int w, int h, int pitch, bool) override
    {
        RECT cr; GetClientRect(hwnd, &cr);
        DisplayConfig c = cfg_; c.picture = resolve_picture(hwnd, cfg_);
        Placement p = place_picture(cr.right, cr.bottom, w, h, c);
        if (p.w <= 0) return;
        Bmi565 b{};
        b.h.biSize = sizeof b.h; b.h.biWidth = pitch / 2; b.h.biHeight = -p.src_h; b.h.biPlanes = 1;
        b.h.biBitCount = 16; b.h.biCompression = BI_BITFIELDS;
        b.masks[0] = 0xF800; b.masks[1] = 0x07E0; b.masks[2] = 0x001F;
        HDC dc = GetDC(hwnd);
        if (!dc) return;
        SetStretchBltMode(dc, cfg_.scaling == Scaling::SharpBilinear ? HALFTONE : COLORONCOLOR);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, p.x, p.y, p.w, p.h, 0, 0, w, p.src_h, bits + p.src_y * pitch, (BITMAPINFO *)&b, DIB_RGB_COLORS, SRCCOPY);
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bars[4] = { { 0, 0, cr.right, p.y }, { 0, p.y + p.h, cr.right, cr.bottom },
                         { 0, p.y, p.x, p.y + p.h }, { p.x + p.w, p.y, cr.right, p.y + p.h } };
        for (RECT &r : bars) if (r.right > r.left && r.bottom > r.top) FillRect(dc, &r, black);
        ReleaseDC(hwnd, dc);
    }
private:
    DisplayConfig cfg_;
public:
    void set_config(const DisplayConfig &c) override { cfg_ = c; }
};

/* ---------- Direct3D 11 ---------- */

const char kShader[] = R"(
Texture2D frame : register(t0);
SamplerState point_s : register(s0);
SamplerState linear_s : register(s1);
cbuffer params : register(b0) {
    float2 tex_size;    // texture size in texels
    float2 src_origin;  // first texel shown
    float2 src_size;    // texels shown
    float2 out_size;    // viewport size in pixels
    int mode;           // 0 sharp-bilinear, 1 nearest
    int3 pad;
};
struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
vs_out vs_main(uint id : SV_VertexID) {
    vs_out o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_main(vs_out i) : SV_Target {
    float2 texel = src_origin + i.uv * src_size;
    if (mode == 1)
        return float4(frame.Sample(point_s, texel / tex_size).rgb, 1);
    float2 scale = max(out_size / src_size, 1.0);
    float2 base = floor(texel - 0.5) + 0.5;
    float2 f = texel - 0.5 - floor(texel - 0.5);
    f = saturate((f - 0.5) * scale + 0.5);
    return float4(frame.Sample(linear_s, (base + f) / tex_size).rgb, 1);
}
)";

struct Params { float tex_size[2], src_origin[2], src_size[2], out_size[2]; int mode, pad[3]; };

template <typename T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

class D3D11Presenter final : public Presenter {
public:
    explicit D3D11Presenter(const DisplayConfig &c) : cfg_(c) {}
    ~D3D11Presenter() override
    {
        release(tex_); release(srv_); release(rtv_); release(cb_); release(ps_); release(vs_);
        release(samp_point_); release(samp_linear_); release(swap_); release(ctx_); release(dev_);
    }
    const char *name() const override { return flip_model_ ? "Direct3D 11 (flip model)" : "Direct3D 11"; }

    bool init(HWND hwnd)
    {
        HMODULE d3d = LoadLibraryA("d3d11.dll");
        if (!d3d) { plog("display: d3d11.dll not found"); return false; }
        PFN_D3D11_CREATE_DEVICE create = nullptr;
        set_fn(create, GetProcAddress(d3d, "D3D11CreateDevice"));
        if (!create) return false;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got;
        HRESULT r = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                           D3D11_SDK_VERSION, &dev_, &got, &ctx_);
        if (FAILED(r)) {
            r = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                       D3D11_SDK_VERSION, &dev_, &got, &ctx_);
            if (FAILED(r)) { plog("display: D3D11CreateDevice failed 0x%08lX", (unsigned long)r); return false; }
            plog("display: no hardware Direct3D 11 device, using WARP (software)");
        }
        if (!make_swap_chain(hwnd)) return false;
        if (!make_shaders()) return false;
        D3D11_SAMPLER_DESC sd{};
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        if (FAILED(dev_->CreateSamplerState(&sd, &samp_point_))) return false;
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        if (FAILED(dev_->CreateSamplerState(&sd, &samp_linear_))) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Params); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cb_))) return false;
        plog("display: %s, feature level %X.%X", name(), (got >> 12) & 0xF, (got >> 8) & 0xF);
        return true;
    }

    void present(HWND hwnd, const BYTE *bits, int w, int h, int pitch, bool vsync) override
    {
        RECT cr; GetClientRect(hwnd, &cr);
        if (cr.right <= 0 || cr.bottom <= 0 || IsIconic(hwnd)) return;
        if ((int)cr.right != bw_ || (int)cr.bottom != bh_) resize(cr.right, cr.bottom);
        if (!rtv_ || !upload(bits, w, h, pitch)) return;
        DisplayConfig c = cfg_; c.picture = resolve_picture(hwnd, cfg_);
        if (c.picture != last_picture_) { last_picture_ = c.picture; plog("display: picture %s", c.picture == Picture::Fill ? "4:3 (640x400 stretched to 4:3)" : c.picture == Picture::Frame ? "Frame (640x480 with the game's bars)" : "16:10 (640x400, square pixels)"); }
        Placement p = place_picture(bw_, bh_, w, h, c);
        const float black[4] = { 0, 0, 0, 1 };
        ctx_->ClearRenderTargetView(rtv_, black);
        if (p.w > 0) {
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                Params *q = (Params *)m.pData;
                *q = Params{ { (float)tw_, (float)th_ }, { 0, (float)p.src_y }, { (float)w, (float)p.src_h },
                             { (float)p.w, (float)p.h }, cfg_.scaling == Scaling::SharpBilinear ? 0 : 1, { 0, 0, 0 } };
                ctx_->Unmap(cb_, 0);
            }
            D3D11_VIEWPORT vp{ (float)p.x, (float)p.y, (float)p.w, (float)p.h, 0, 1 };
            ctx_->RSSetViewports(1, &vp);
            ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx_->IASetInputLayout(nullptr);
            ctx_->VSSetShader(vs_, nullptr, 0);
            ctx_->PSSetShader(ps_, nullptr, 0);
            ctx_->PSSetShaderResources(0, 1, &srv_);
            ID3D11SamplerState *ss[2] = { samp_point_, samp_linear_ };
            ctx_->PSSetSamplers(0, 2, ss);
            ctx_->PSSetConstantBuffers(0, 1, &cb_);
            ctx_->Draw(3, 0);
        }
        HRESULT r = swap_->Present(vsync && cfg_.vsync ? 1 : 0, 0);
        if (FAILED(r) && fails_++ < 5) plog("display: Present failed 0x%08lX", (unsigned long)r);
        if (flip_model_) ctx_->OMSetRenderTargets(0, nullptr, nullptr);   /* flip model unbinds the back buffer */
    }

private:
    bool make_swap_chain(HWND hwnd)
    {
        IDXGIDevice *dxdev = nullptr; IDXGIAdapter *ad = nullptr; IDXGIFactory *f = nullptr;
        if (FAILED(dev_->QueryInterface(IID_IDXGIDevice, (void **)&dxdev))) return false;
        dxdev->GetAdapter(&ad); dxdev->Release();
        if (!ad) return false;
        ad->GetParent(IID_IDXGIFactory, (void **)&f); ad->Release();
        if (!f) return false;
        RECT cr; GetClientRect(hwnd, &cr);
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferDesc.Width = cr.right > 0 ? cr.right : 640;
        d.BufferDesc.Height = cr.bottom > 0 ? cr.bottom : 480;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.OutputWindow = hwnd;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT r = f->CreateSwapChain(dev_, &d, &swap_);
        flip_model_ = SUCCEEDED(r);
        if (FAILED(r)) {
            d.BufferCount = 1;
            d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            r = f->CreateSwapChain(dev_, &d, &swap_);
        }
        f->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        f->Release();
        if (FAILED(r)) { plog("display: CreateSwapChain failed 0x%08lX", (unsigned long)r); return false; }
        IDXGIDevice1 *d1 = nullptr;
        if (SUCCEEDED(dev_->QueryInterface(IID_IDXGIDevice1, (void **)&d1))) { d1->SetMaximumFrameLatency(1); d1->Release(); }
        bw_ = d.BufferDesc.Width; bh_ = d.BufferDesc.Height;
        return make_rtv();
    }

    bool make_rtv()
    {
        ID3D11Texture2D *bb = nullptr;
        if (FAILED(swap_->GetBuffer(0, IID_ID3D11Texture2D, (void **)&bb))) return false;
        HRESULT r = dev_->CreateRenderTargetView(bb, nullptr, &rtv_);
        bb->Release();
        return SUCCEEDED(r);
    }

    void resize(int w, int h)
    {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        release(rtv_);
        HRESULT r = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(r)) { plog("display: ResizeBuffers(%d, %d) failed 0x%08lX", w, h, (unsigned long)r); return; }
        bw_ = w; bh_ = h;
        make_rtv();
        plog("display: output %dx%d", w, h);
    }

    bool make_shaders()
    {
        HMODULE c = LoadLibraryA("d3dcompiler_47.dll");
        if (!c) { plog("display: d3dcompiler_47.dll not found"); return false; }
        pD3DCompile compile = nullptr;
        set_fn(compile, GetProcAddress(c, "D3DCompile"));
        if (!compile) return false;
        ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
        HRESULT r = compile(kShader, sizeof kShader - 1, "present", nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vs, &err);
        if (SUCCEEDED(r)) r = compile(kShader, sizeof kShader - 1, "present", nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &ps, &err);
        if (FAILED(r)) {
            plog("display: shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
            release(err); release(vs); release(ps);
            return false;
        }
        bool ok = SUCCEEDED(dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) &&
                  SUCCEEDED(dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_));
        release(vs); release(ps); release(err);
        return ok;
    }

    bool upload(const BYTE *bits, int w, int h, int pitch)
    {
        if (!tex_ || w != tw_ || h != th_) {
            release(srv_); release(tex_);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tex_))) return false;
            if (FAILED(dev_->CreateShaderResourceView(tex_, nullptr, &srv_))) return false;
            tw_ = w; th_ = h;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        for (int y = 0; y < h; y++) {
            const WORD *s = (const WORD *)(bits + y * pitch);
            DWORD *d = (DWORD *)((BYTE *)m.pData + y * m.RowPitch);
            for (int x = 0; x < w; x++) {
                DWORD v = s[x];
                DWORD r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                d[x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
        }
        ctx_->Unmap(tex_, 0);
        return true;
    }

    DisplayConfig cfg_;
public:
    void set_config(const DisplayConfig &c) override { cfg_ = c; }
    ID3D11Device *dev_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    IDXGISwapChain *swap_ = nullptr;
    ID3D11RenderTargetView *rtv_ = nullptr;
    ID3D11Texture2D *tex_ = nullptr;
    ID3D11ShaderResourceView *srv_ = nullptr;
    ID3D11VertexShader *vs_ = nullptr;
    ID3D11PixelShader *ps_ = nullptr;
    ID3D11SamplerState *samp_point_ = nullptr, *samp_linear_ = nullptr;
    ID3D11Buffer *cb_ = nullptr;
    bool flip_model_ = false;
    int bw_ = 0, bh_ = 0, tw_ = 0, th_ = 0;
    int fails_ = 0;
    Picture last_picture_ = (Picture)-1;
};

} // namespace

Presenter *make_d3d11_presenter(HWND hwnd, const DisplayConfig &cfg)
{
    D3D11Presenter *p = new D3D11Presenter(cfg);
    if (p->init(hwnd)) return p;
    delete p;
    return nullptr;
}

Presenter *make_gdi_presenter(const DisplayConfig &cfg) { return new GdiPresenter(cfg); }
