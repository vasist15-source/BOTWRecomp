// The aspect ratio sites tools/botw_aspect_patches.py puts into the translated game (answered by
// runtime/src/aspect.cpp). At 16:9 each returns the game's own value.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
double botw_aspect_view(double aspect);  // a camera's viewport aspect -> the one to project with
double botw_aspect_fovy(double fovy, uint32_t viewport);  // its vertical field of view (radians)
double botw_aspect_scaled(double aspect);  // a viewport aspect times picture aspect / (16/9) (inventory 3D)
double botw_aspect_grass(void);          // grass culling factor (the game's: 1.0)
void botw_aspect_tag_projection(void);   // the next 16-float vertex uniform upload is a layout projection
// HUD and menus wider than 16:9 (runtime/src/aspect_hud.cpp)
void botw_hud_screen(uint32_t name);                 // a screen is being created
void botw_hud_pane(uint32_t pane, uint32_t size);    // a pane was built from its layout
double botw_hud_anim(double value, uint32_t pane, uint32_t component);  // an animated transform value
void botw_hud_map(uint32_t open);                    // the map opens (1) or closes (0)
void botw_hud_projection(uint32_t copy);             // a pane-based projection's parameters were copied
#ifdef __cplusplus
}
#endif
