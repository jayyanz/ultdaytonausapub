/* The new renderer's list drawing (Release 2, R2-3): the game's display list drawn by Direct3D 11.

   R2-4 adds the SeeThrough outlines and the car shadows exactly (spans_exact.h).

   R2-3 draws at the game's own 640x400 with the 1996 PC quad mapping (pcmap.h) and the game's own texels, into
   the game's back buffer: the picture rows are uploaded (the sky is already in them), every record of the frame
   is drawn in the game's sorted order in one instanced draw, and the rows are read back. Everything the game
   draws after the list (needle, secondary list, fixed records, the HUD queues) then draws on top as before.

   Shared by the proxy and tools/replay. Reads the game's memory at its own addresses (texture descriptors and
   texels) through r2tex. */
#pragma once
#include <stdint.h>
#include <string>

#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11DepthStencilView;

namespace sky { struct Quad; }

namespace gpulist {

class Renderer {
public:
    ~Renderer();
    /* Creates its own device (hardware, else WARP), or uses the presenter's (shared_dev / shared_ctx, R2-5).
       driver: "hardware", "WARP" or "shared" after success. */
    bool init(std::string &err, bool allow_hardware = true, ID3D11Device *shared_dev = nullptr, ID3D11DeviceContext *shared_ctx = nullptr);

    /* R2-5: the high-resolution pass. w x h: its target (0 x 0: off). Every frame from begin() to end() then also
       draws the records at that size into a BGRA picture, alpha 0 where nothing was drawn. */
    bool set_hires(int w, int h);
    /* how far past its edges a quad covers, in 640x400 pixels (default 0.75: closes the cracks the game's rounding closes) */
    void set_widen(float w);
    /* texture filtering at high resolution: 0 nearest (the game's texels), 1 sharp bilinear, 2 bilinear,
       3 trilinear (default), 4 anisotropic (16x) */
    void set_filter(int f);
    /* edge antialiasing at high resolution: the coverage rule tested at 1 (off), 2, 4 (default) or 8 points of each
       pixel, the fraction covered blended as opacity; returns the samples in use */
    int set_msaa(int samples);
    int msaa() const;
    void set_verify_textures(bool on);   /* 1.1.17: check each texture's contents every frame (off the race screens) */
    void set_mapping(int m);   /* R2-7: 0 PC (the 1996 release's mapping), 1 Saturn (bilinear in the quad's parameters), 2 perspective-correct (persp.h) */
    /* R2-8 (1.1.18): the depth buffer for ReShade (the presenter's, D24S8): each frame cleared by begin() and filled with
       the quads' depths (those with captured depths, persp.h) in the game's order, the 3D at rect (x, y, w, h in the
       buffer's pixels); znear .. zfar (game units) become ReShade's 0 .. 1. nullptr: off. false: not available (no
       high-resolution pass, or the depth shader did not build: depth_error()). */
    bool set_depth(ID3D11DepthStencilView *dsv, const float rect[4], float znear, float zfar);
    const char *depth_error() const { return depth_error_.empty() ? nullptr : depth_error_.c_str(); }
    /* the sky at high resolution (1.1.9): this frame's copies of the game's sky (sky.h), drawn before the 3D from the
       panorama art (re-uploaded when version changes); n = 0: none (the game's own sky shows under the 3D) */
    void set_sky(const sky::Quad *q, int n, const uint32_t *art, uint32_t version);
    int hires_width() const;
    int hires_height() const;
    ID3D11ShaderResourceView *hires_view() const;     /* the high-resolution picture (BGRA, premultiplied) */
    ID3D11ShaderResourceView *picture_view() const;   /* the 640x400 picture as end() read it back (R16_UINT) */
    ID3D11ShaderResourceView *background_view() const; /* the 640x400 picture before the 3D, as begin() had it (R16_UINT) */
    /* the HUD step: the picture after the flush's tail (needle, CONDITION panel, course map), R16_UINT */
    void upload_tail(const uint16_t *rows, uint32_t pitch);
    ID3D11ShaderResourceView *tail_view() const;
    bool read_hires(std::vector<uint32_t> &bgra);     /* for tools: the high-resolution picture, top-down */
    /* for tools: keep a copy of the 640x400 picture end() read back (before the game's tail draws its HUD) */
    void keep_picture(bool on) { keep_picture_ = on; }
    const std::vector<uint16_t> &picture() const { return picture_; }
    const char *driver() const { return driver_; }

    /* rows: the back buffer at the picture's first row (640x400 RGB565), pitch in bytes. */
    void begin(const uint16_t *rows, uint32_t pitch);
    /* One display-list record (40 bytes at rec), as 0x454C10 would draw it; returns false if not drawn. */
    bool quad(uint32_t rec);
    /* A car shadow record (texture number 9), as 0x454B50 would draw it. */
    void shadow(uint32_t rec);
    /* A record drawn as the game's SeeThrough outline (0x4559D0): its four sides in its texture's first texel. */
    bool outline(uint32_t rec);
    /* Draws the frame and writes the picture rows back. false: device trouble (the caller falls back). */
    bool end(uint16_t *rows, uint32_t pitch);

    struct Stats { uint32_t quads, shadows, outlines, skipped, atlas_textures, atlas_resets; };
    const Stats &stats() const { return stats_; }

private:
    struct Impl;
    Impl *p_ = nullptr;
    const char *driver_ = "";
    std::string depth_error_;
    Stats stats_{};
    uint32_t pitch_ = 1280;
    bool keep_picture_ = false;
    std::vector<uint16_t> picture_;
    uint16_t outline_colour_ = 0;   /* 0x493620: the last record's first texel */
};

}  // namespace gpulist
