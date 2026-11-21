// View distances of Settings > Graphics (runtime/src/mods/distance.cpp). The translated game reads them
// at the sites of tools/botw_cheat_patches.py's DISTANCE_SITES; a value of 0 runs the game's own
// instruction there. Any thread may change them.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    BOTW_DIST_ACTOR,           // the actor draw-distance multiplier (+0x13BC), in place of load balancing's
    BOTW_DIST_OBJECT,          // the object (map unit) distance multiplier
    BOTW_DIST_OBJECT_CULL,     // what the squared culling distance of objects is scaled by: 2 - object
    BOTW_DIST_TREE,            // the distance factor where trees turn into billboards (game: 0.5, lower: farther)
    BOTW_DIST_COUNT
};
extern float botw_distance[BOTW_DIST_COUNT];
static inline float botw_dist(int id) {
    float v;
    __atomic_load(&botw_distance[id], &v, __ATOMIC_RELAXED);
    return v;
}
static inline int botw_dist_on(int id) { return botw_dist(id) != 0.0f; }
#ifdef __cplusplus
}
#endif
