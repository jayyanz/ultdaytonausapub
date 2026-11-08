/* Game textures for the new renderer (Release 2, R2-2): the game's 4-bit Saturn textures decoded to
   premultiplied RGBA8 with a mip chain, ready to upload. Shared by the proxy and tools/replay; no Direct3D here.

   Reads the game's memory at its own addresses (DAYTONA.EXE at 0x400000, as in the game process and in the
   replayer). Addresses and layout: docs/investigation/renderer.md section 2, docs/renderer-findings.md.

   Textures are keyed by (texel pointer, width, height), the way the game draws them, so the flip-book descriptors
   that swap their pointer each frame need no special case. A texture the game cut to 127 texels is decoded
   again at its full size from VRAM when that decode, cut the game's way, gives the game's own texels. */
#pragma once
#include <stdint.h>
#include <map>
#include <memory>
#include <vector>

namespace r2tex {

constexpr uint16_t kKey565 = 0x0020;   /* the game's transparent texel (cannot occur as a colour) */

/* One mip level: premultiplied RGBA8 (R in the low byte), width*height. */
struct Level { uint32_t width = 0, height = 0; std::vector<uint32_t> px; };

struct Texture {
    uint32_t ptr = 0, game_w = 0, game_h = 0;   /* key: the game's texel pointer and decoded size */
    bool keyed = false;      /* has transparent texels */
    bool full_size = false;  /* decoded from VRAM at the Saturn size instead of the game's 127 cut */
    bool scroll = false;     /* the 80x40 scroll buffer: contents change every frame */
    uint32_t hash = 0;       /* of the level-0 texels, for spotting changes */
    std::vector<Level> mips; /* level 0 first */
};

/* What a descriptor (24 bytes at 0xBFD660) says. */
struct Descriptor {
    uint32_t src;        /* VRAM address in 8-byte units */
    uint32_t ptr;        /* decoded RGB565 texels, or 0 = decoded on the fly per draw */
    uint32_t sat_w, sat_h;  /* Saturn size from CMDSIZE */
    uint32_t w, h;       /* decoded size */
    uint32_t colr;       /* CMDCOLR: palette in 8-byte units from the colour table base */
};
uint32_t descriptor_count();
Descriptor descriptor(uint32_t index);
int descriptor_for(uint32_t bank, uint32_t number);   /* through the index map 0xC0C090, -1 if none */

/* Is this VRAM address in the current course's keyed list. */
bool keyed_by_course(uint32_t src);

/* Decodes a 4bpp VRAM texture to RGB565 (RGB555 when the game runs in that mode), giving the same texels as the
   game. max_side = 127 to match the game's size limit; 0 = full size. */
void decode_vram(uint32_t src, uint32_t colr, uint32_t sat_w, uint32_t sat_h, bool keyed, uint32_t max_side,
                 std::vector<uint16_t> &out, uint32_t &w, uint32_t &h);

/* Game texel to premultiplied RGBA8. */
uint32_t rgba_from_game(uint16_t v, bool mode555);
bool game_mode555();

/* Premultiplied box-filtered mip chain down to 1x1 from level 0 (already in mips[0]). */
void build_mips(Texture &t);

/* All textures the game has decoded (descriptors with a texel pointer), built per course load and kept up to
   date per frame (scroll buffer, flip-book pointers). */
class TextureSet {
public:
    struct Stats { uint32_t descriptors = 0, textures = 0, full_size = 0, full_size_rejected = 0, on_the_fly = 0, scroll = 0; };
    /* Rebuild when the descriptor table changed (course or screen load); refresh the scroll buffer. Returns true
       when textures were (re)built. */
    bool update();
    const Texture *find(uint32_t ptr, uint32_t w, uint32_t h) const;
    /* Descriptors with no texel pointer (screen mode 0x1B) are decoded from VRAM on use; keyed per record. */
    const Texture *on_the_fly(const Descriptor &d, bool keyed);
    const Stats &stats() const { return stats_; }
    const std::map<uint64_t, std::unique_ptr<Texture>> &all() const { return tex_; }
    void clear() { tex_.clear(); fly_.clear(); sig_ = 0; }

private:
    static uint64_t key(uint32_t ptr, uint32_t w, uint32_t h) { return (uint64_t)ptr << 20 | (uint64_t)(w & 1023) << 10 | (h & 1023); }
    Texture *add(const Descriptor &d);
    std::map<uint64_t, std::unique_ptr<Texture>> tex_;
    std::map<uint64_t, std::unique_ptr<Texture>> fly_;
    uint64_t sig_ = 0;
    Stats stats_;
};

}  // namespace r2tex
