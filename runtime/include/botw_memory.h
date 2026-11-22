// More memory (Settings > Graphics > View distance, runtime.h mem::Layout): set once at boot, before the
// game runs. The translated game asks it at the heap-size sites of tools/botw_cheat_patches.py's
// MEMORY_SITES; off, each site runs the game's own instruction.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
extern int botw_more_memory_on;
static inline int botw_more_memory(void) { return botw_more_memory_on; }
#ifdef __cplusplus
}
#endif
