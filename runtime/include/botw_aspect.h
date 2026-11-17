// The aspect ratio sites tools/botw_aspect_patches.py puts into the translated game (answered by
// runtime/src/aspect.cpp). At 16:9 each returns the game's own value.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
double botw_aspect_view(double aspect);  // a camera's viewport aspect -> the one to project with
double botw_aspect_fovy(double fovy, uint32_t viewport);  // its vertical field of view (radians)
double botw_aspect_grass(void);          // grass culling factor (the game's: 1.0)
void botw_aspect_tag_projection(void);   // the next 16-float vertex uniform upload is a layout projection
#ifdef __cplusplus
}
#endif
