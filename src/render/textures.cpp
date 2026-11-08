/* Game textures to premultiplied RGBA8 (Release 2, R2-2). See textures.h.
   Built with SSE2 doubles (build scripts): the decoder's texel stepping must round like the game's x87 at
   53-bit precision, which IEEE double arithmetic does exactly. */
#include "textures.h"
#include <string.h>

namespace r2tex {

namespace {

constexpr uint32_t kDescTable = 0xBFD660, kDescCount = 0xC0C080, kIndexMap = 0xC0C090, kVramPtr = 0xBDD650;
constexpr uint32_t kColourBase = 0x4935F8, kMaxSideVar = 0x493624, kMode555 = 0x48ABEC, kCourse = 0x11B7288;
constexpr uint32_t kCacheCursor = 0xBDBA20, kScrollBuffer = 0xC0A700;
constexpr uint32_t kKeyedLists[3] = { 0x493230, 0x493308, 0x4934B0 };
constexpr uint32_t kMaxDescriptors = 4096;

template <typename T> inline T rd(uint32_t va) { T v; memcpy(&v, (const void *)(uintptr_t)va, sizeof v); return v; }
inline const uint8_t *mem(uint32_t va) { return (const uint8_t *)(uintptr_t)va; }

uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* x87 FISTP after the game's _ftol: truncation toward zero (all values here are >= 0) */
inline int32_t ftol(double v) { return (int32_t)v; }

void level0_from_game(Texture &t, const uint16_t *src, uint32_t w, uint32_t h, bool mode555)
{
    t.mips.assign(1, Level{});
    Level &l = t.mips[0];
    l.width = w; l.height = h; l.px.resize((size_t)w * h);
    bool key = false;
    for (size_t i = 0; i < l.px.size(); i++) {
        if (src[i] == kKey565) key = true;
        l.px[i] = rgba_from_game(src[i], mode555);
    }
    t.keyed = t.keyed || key;
    t.hash = fnv(src, (size_t)w * h * 2);
}

}  // namespace

uint32_t descriptor_count()
{
    uint32_t n = rd<uint32_t>(kDescCount);
    return n > kMaxDescriptors ? kMaxDescriptors : n;
}

Descriptor descriptor(uint32_t i)
{
    uint32_t a = kDescTable + 24 * i;
    uint16_t size = rd<uint16_t>(a + 8);
    return { rd<uint32_t>(a), rd<uint32_t>(a + 4), (uint32_t)(size >> 8) * 8, (uint32_t)(size & 0xFF),
             rd<uint16_t>(a + 12), rd<uint16_t>(a + 14), rd<uint16_t>(a + 20) };
}

int descriptor_for(uint32_t bank, uint32_t number)
{
    if (bank > 255 || number > 0x300) return -1;   /* numbers above 0x300 are not drawn */
    uint32_t i = rd<uint16_t>(kIndexMap + 2 * (bank * 64 + number));
    return i < descriptor_count() ? (int)i : -1;
}

bool keyed_by_course(uint32_t src)
{
    uint16_t course = rd<uint16_t>(kCourse);
    uint32_t list = kKeyedLists[course == 2 ? 2 : course == 1 ? 1 : 0];
    for (int i = 0; i < 4096; i++) {
        uint32_t v = rd<uint32_t>(list + 4 * i);
        if (v == 0xFFFFFFFFu) return false;
        if (v == src) return true;
    }
    return false;
}

bool game_mode555() { return rd<uint32_t>(kMode555) != 0; }

void decode_vram(uint32_t src, uint32_t colr, uint32_t sat_w, uint32_t sat_h, bool keyed, uint32_t max_side,
                 std::vector<uint16_t> &out, uint32_t &w, uint32_t &h)
{
    const uint32_t vram = rd<uint32_t>(kVramPtr);
    const uint8_t *tex = mem(vram + src * 8);
    const uint8_t *pal = mem(vram + (colr + rd<uint32_t>(kColourBase)) * 8);
    const bool m555 = game_mode555();
    w = sat_w; h = sat_h;
    if (max_side) {
        if (w >= 128) w = 127;
        if (h >= 128) h = 127;
        uint32_t cap = rd<uint32_t>(kMaxSideVar);
        if ((int32_t)cap < (int32_t)w) w = cap;
        if ((int32_t)cap < (int32_t)h) h = cap;
    }
    out.resize((size_t)w * h);
    const double step_x = (double)(int32_t)sat_w / (double)(int32_t)w, step_y = (double)(int32_t)sat_h / (double)(int32_t)h;
    double v = 0;
    uint16_t *o = out.data();
    for (uint32_t y = 0; y < h; y++, v += step_y) {
        const int32_t row = ftol(v) * (int32_t)sat_w / 2;
        double u = 0;
        for (uint32_t x = 0; x < w; x++, u += step_x) {
            const int32_t sx = ftol(u);
            const uint8_t b = tex[row + sx / 2];
            const uint16_t idx = (sx & 1) ? (b & 15) : (b >> 4);
            if (!idx && keyed) { *o++ = kKey565; continue; }
            uint16_t c; memcpy(&c, pal + 2 * idx, 2);
            const uint16_t r = c & 31, g = (c >> 5) & 31, bl = (c >> 10) & 31;
            *o++ = m555 ? (uint16_t)(r << 10 | (g | 0xFC00) << 5 | bl) : (uint16_t)(r << 11 | g << 6 | bl);
        }
    }
}

uint32_t rgba_from_game(uint16_t v, bool mode555)
{
    if (mode555 ? !(v & 0x8000) : v == kKey565) return 0;   /* 555: the overlay's test is bit 15 clear */
    uint32_t r, g, b;
    if (mode555) { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; }
    else { r = v >> 11; g = (v >> 6) & 31; b = v & 31; }
    r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
    return r | g << 8 | b << 16 | 0xFF000000u;
}

void build_mips(Texture &t)
{
    t.mips.resize(1);
    while (t.mips.back().width > 1 || t.mips.back().height > 1) {
        const Level &s = t.mips.back();
        Level d;
        d.width = s.width > 1 ? s.width / 2 : 1; d.height = s.height > 1 ? s.height / 2 : 1;
        d.px.resize((size_t)d.width * d.height);
        for (uint32_t y = 0; y < d.height; y++)
            for (uint32_t x = 0; x < d.width; x++) {
                uint32_t sx0 = x * 2, sy0 = y * 2;
                uint32_t sx1 = sx0 + 1 < s.width ? sx0 + 1 : sx0, sy1 = sy0 + 1 < s.height ? sy0 + 1 : sy0;
                const uint32_t p[4] = { s.px[(size_t)sy0 * s.width + sx0], s.px[(size_t)sy0 * s.width + sx1],
                                        s.px[(size_t)sy1 * s.width + sx0], s.px[(size_t)sy1 * s.width + sx1] };
                uint32_t out = 0;
                for (int c = 0; c < 4; c++) {   /* premultiplied average, rounded */
                    uint32_t sum = 2;
                    for (int k = 0; k < 4; k++) sum += (p[k] >> (8 * c)) & 0xFF;
                    out |= (sum / 4) << (8 * c);
                }
                d.px[(size_t)y * d.width + x] = out;
            }
        t.mips.push_back(std::move(d));
    }
}

Texture *TextureSet::add(const Descriptor &d)
{
    auto t = std::make_unique<Texture>();
    t->ptr = d.ptr; t->game_w = d.w; t->game_h = d.h;
    t->scroll = d.ptr == kScrollBuffer;
    const bool m555 = game_mode555();
    t->keyed = keyed_by_course(d.src);
    level0_from_game(*t, (const uint16_t *)mem(d.ptr), d.w, d.h, m555);
    if (!t->scroll && (d.sat_w > d.w || d.sat_h > d.h)) {
        /* cut by the game: decode at full size when the game's cut of our decode is the game's own texels */
        std::vector<uint16_t> cut; uint32_t cw, ch;
        decode_vram(d.src, d.colr, d.sat_w, d.sat_h, t->keyed, 127, cut, cw, ch);
        if (cw == d.w && ch == d.h && !memcmp(cut.data(), mem(d.ptr), cut.size() * 2)) {
            std::vector<uint16_t> full; uint32_t fw, fh;
            decode_vram(d.src, d.colr, d.sat_w, d.sat_h, t->keyed, 0, full, fw, fh);
            level0_from_game(*t, full.data(), fw, fh, m555);
            t->full_size = true; stats_.full_size++;
        } else stats_.full_size_rejected++;
    }
    build_mips(*t);
    stats_.textures++;
    if (t->scroll) stats_.scroll++;
    Texture *p = t.get();
    tex_[key(d.ptr, d.w, d.h)] = std::move(t);
    return p;
}

bool TextureSet::update()
{
    const uint32_t n = descriptor_count();
    /* the table without its texel pointers (flip-books change those every frame), the count and the cache
       cursor: a different value means a course or screen load */
    uint64_t sig = fnv(&n, 4) ^ (uint64_t)rd<uint32_t>(kCacheCursor) << 32;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { uint8_t e[24]; memcpy(e, mem(kDescTable + 24 * i), 24); memset(e + 4, 0, 4); h = fnv(e, 24, h); }
    sig ^= h;
    bool rebuilt = false;
    if (sig != sig_) { tex_.clear(); fly_.clear(); stats_ = Stats{}; sig_ = sig; rebuilt = true; }
    stats_.descriptors = n;
    for (uint32_t i = 0; i < n; i++) {
        Descriptor d = descriptor(i);
        if (!d.ptr) { if (rebuilt) stats_.on_the_fly++; continue; }
        if (!d.w || !d.h || d.w > 1024 || d.h > 1024) continue;
        auto it = tex_.find(key(d.ptr, d.w, d.h));
        if (it == tex_.end()) { add(d); continue; }
        Texture &t = *it->second;
        if (t.scroll) {   /* refilled every frame by the game */
            uint32_t hh = fnv(mem(d.ptr), (size_t)d.w * d.h * 2);
            if (hh != t.hash) { level0_from_game(t, (const uint16_t *)mem(d.ptr), d.w, d.h, game_mode555()); build_mips(t); }
        }
    }
    return rebuilt;
}

const Texture *TextureSet::find(uint32_t ptr, uint32_t w, uint32_t h) const
{
    auto it = tex_.find(key(ptr, w, h));
    return it == tex_.end() ? nullptr : it->second.get();
}

const Texture *TextureSet::on_the_fly(const Descriptor &d, bool keyed)
{
    const uint32_t vram = rd<uint32_t>(kVramPtr);
    uint32_t hsh = fnv(mem(vram + d.src * 8), (size_t)d.sat_w * d.sat_h / 2);
    hsh = fnv(mem(vram + (d.colr + rd<uint32_t>(kColourBase)) * 8), 32, hsh);
    const uint64_t k = (uint64_t)hsh << 32 ^ (uint64_t)d.sat_w << 20 ^ (uint64_t)d.sat_h << 8 ^ (keyed ? 1 : 0);
    auto it = fly_.find(k);
    if (it != fly_.end()) return it->second.get();
    auto t = std::make_unique<Texture>();
    std::vector<uint16_t> full; uint32_t fw, fh;
    decode_vram(d.src, d.colr, d.sat_w, d.sat_h, keyed, 0, full, fw, fh);
    t->keyed = keyed; t->full_size = true; t->game_w = d.sat_w; t->game_h = d.sat_h;
    level0_from_game(*t, full.data(), fw, fh, game_mode555());
    build_mips(*t);
    Texture *p = t.get();
    fly_[k] = std::move(t);
    return p;
}

}  // namespace r2tex
