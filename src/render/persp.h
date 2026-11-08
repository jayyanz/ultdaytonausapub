/* R2-7: the records' true depths for the perspective-correct mapping. See persp.cpp. */
#pragma once
#include <windows.h>
#include <stdint.h>

namespace persp {
bool install(BYTE *base);                  /* hooks the five emitters (all checked first, all or none) */
/* the depths of record rec's corners A to D, if captured and current: 2 for the mapping and the depth buffer, 1 for
   the depth buffer only (a corner moved onto the near plane by the clipping emitters: the depth is right there, the
   texture mapping is not), 0 none */
int lookup(uint32_t rec, float z[4]);
bool range(float &znear, float &zmin, float &zmax);   /* 1.1.18: the near plane and the depths seen so far */
void stats(unsigned &captured, unsigned &near_plane, unsigned &checked, unsigned &matched);   /* checks: debug build */
}
