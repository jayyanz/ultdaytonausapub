/* The new renderer in the proxy (Release 2): [Display] Renderer3D=Original|New, the DISPLAY page's RENDERER row.
   The game's list flush (the call at 0x45523E) goes to r2flush::flush with a Direct3D 11 device of its own; the
   original software path stays, and is used if the new one cannot start or loses its device. */
#pragma once
#include <windows.h>

enum class Renderer3D { Original = 0, New = 1 };
/* How the new renderer draws the records SeeThrough shows see-through: [Display] SeeThroughLook, the DISPLAY page's
   SEETHRU LOOK row. Textured (default, decided 1 Nov 2026) or the game's Outline. */
enum class SeeThroughLook { Textured = 0, Outline = 1 };

bool renderer3d_install(BYTE *game_base, Renderer3D mode, SeeThroughLook look = SeeThroughLook::Textured, int resolution = 0, int filter = 3, int msaa = 4,
                        int hud_pct = 100, int hud_filter = 1);
Renderer3D renderer3d_get();
void renderer3d_set(Renderer3D mode);     /* live, from the Options DISPLAY page */
SeeThroughLook renderer3d_seethrough_get();
void renderer3d_seethrough_set(SeeThroughLook look);   /* live, from the Options DISPLAY page */
/* R2-5: the resolution the 3D is drawn at: 0 the display's own (the size the picture is shown at), 1 640x400 (as
   the game), 2 to 4 times 640x400. [Display] Resolution3D, the DISPLAY page's RESOLUTION row. */
int renderer3d_resolution_get();
void renderer3d_resolution_set(int res);
/* R2-5: texture filtering at high resolution: 0 nearest, 1 sharp bilinear, 2 bilinear, 3 trilinear (default),
   4 anisotropic. [Display] TextureFilter, the DISPLAY page's FILTER row. */
int renderer3d_filter_get();
void renderer3d_filter_set(int f);
/* R2-5: edge antialiasing at high resolution, coverage samples per pixel: 1 (off), 2, 4 (default) or 8. [Display] Antialiasing, the DISPLAY
   page's ANTIALIAS row. */
int renderer3d_msaa_get();
/* R2-7: how textures are spread over each quad at high resolution: 0 PC (the 1996 release's scanline mapping), 1 Saturn
   (bilinear in the quad's own parameters, as the Saturn's VDP1). [Display] Mapping; the DISPLAY page's MAPPING row. */
int renderer3d_mapping_get();
void renderer3d_mapping_set(int m);
/* R2-8 (1.1.18): a depth buffer for ReShade (present.h), filled from the quads' true depths (persp.h) with any MAPPING.
   [Display] DepthBuffer; the DISPLAY page's DEPTH BUFFER row. */
bool renderer3d_depth_get();
void renderer3d_depth_set(bool on);
void renderer3d_msaa_set(int samples);
/* R2-5 HUD step: the race HUD's size, 100, 75 or 50 (a whole number of screen pixels for each HUD pixel; each group
   shrinks towards its corner or edge) and
   its filter (0 nearest, 1 sharp bilinear, 2 bilinear). [Display] HudScale, HudFilter; rows HUD SCALE and HUD FILTER. */
int renderer3d_hud_scale_get();
void renderer3d_hud_scale_set(int pct);
int renderer3d_hud_filter_get();
void renderer3d_hud_filter_set(int f);
bool renderer3d_active();                 /* New and working */

/* The list flush itself (the render capture's end of frame calls it): the game's own path when original is true. */
void renderer3d_list_flush(bool original);
void renderer3d_shutdown();
