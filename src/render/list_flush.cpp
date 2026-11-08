/* The game's list flush with the new renderer. See list_flush.h and docs/renderer-findings.md ("List flush"). */
#include "list_flush.h"
#include <string.h>

namespace r2flush {

namespace {

constexpr uint32_t kListFlush = 0x455650, kBackBuffer = 0xC584F8, kPitch = 0x4D4F34, kRowOffset = 0x48ABE8;
constexpr uint32_t kScreenMode = 0x482230, kCount = 0x4931A8, kOrder = 0xBD9E00, kRecords = 0xC464B0;
constexpr uint32_t kFlushFlag = 0xC464A4, kDecodeOnTheFly = 0x431D40, kRaceFlags = 0x11B7AA8, kSeeThrough = 0x48AC08;
constexpr uint32_t kCourse = 0x11B7288;
constexpr uint32_t kSeeThroughTables[3] = { 0x493630, 0x493780, 0x4938E0 };
constexpr uint32_t kBeforeNeedle = 0x42CE20, kNeedle = 0x4552E0, kSecondary = 0x4114F0, kFixed = 0x431BB0;

template <typename T> T rd(uint32_t va) { T v; memcpy(&v, (const void *)(uintptr_t)va, sizeof v); return v; }
typedef void (__cdecl *fn0)(void);
typedef void (__cdecl *fn1)(uint32_t);

}  // namespace

void game_flush() { ((fn0)(uintptr_t)kListFlush)(); }

/* 0x454C10: with SeeThrough on, on the race screens, the records whose texture number the course's table flags
   are drawn as outlines instead of textured. */
bool seethrough_outline(uint32_t rec)
{
    if (!rd<uint32_t>(kSeeThrough) || !(rd<uint32_t>(kRaceFlags) & 0x202A8000)) return false;
    const uint32_t num = rd<uint16_t>(rec + 6);
    if (num > 0x300) return false;
    const uint16_t course = rd<uint16_t>(kCourse);
    return course < 3 && rd<uint8_t>(kSeeThroughTables[course] + num) != 0;
}

bool flush(gpulist::Renderer &r, bool outlines)
{
    uint8_t *bb = (uint8_t *)(uintptr_t)rd<uint32_t>(kBackBuffer);
    const uint32_t pitch = rd<uint16_t>(kPitch);
    if (!bb || pitch < 1280) return false;
    uint16_t *rows = (uint16_t *)(bb + rd<uint32_t>(kRowOffset));
    r.begin(rows, pitch);
    *(uint32_t *)(uintptr_t)kFlushFlag = 0;
    const bool on_the_fly = rd<uint32_t>(kScreenMode) == 0x1B;
    const uint32_t n = rd<uint32_t>(kCount);
    for (uint32_t i = 0; i < n && i < 1800; i++) {
        const uint32_t rec = kRecords + 40 * rd<uint32_t>(kOrder + 4 * i);
        if (on_the_fly) { ((fn1)(uintptr_t)kDecodeOnTheFly)(rec); r.quad(rec); continue; }
        if (rd<uint16_t>(rec + 6) == 9) r.shadow(rec);
        else if (outlines && seethrough_outline(rec)) r.outline(rec);
        else r.quad(rec);
    }
    if (!r.end(rows, pitch)) return false;
    /* then the game's own tail drawing, unchanged */
    if (rd<uint32_t>(kRaceFlags) & 0x202A8000) {
        if (!rd<uint32_t>(kSeeThrough)) ((fn0)(uintptr_t)kBeforeNeedle)();
        ((fn0)(uintptr_t)kNeedle)();
    }
    ((fn0)(uintptr_t)kSecondary)();
    ((fn0)(uintptr_t)kFixed)();
    r.upload_tail(rows, pitch);   /* the picture after the tail (needle, CONDITION panel, course map): the HUD's split */
    return true;
}

bool race_screen() { return (rd<uint32_t>(kRaceFlags) & 0x202A8000) != 0; }

}  // namespace r2flush
