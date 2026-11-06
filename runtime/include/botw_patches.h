// Cemu graphic packs' code patches built into the game code (tools/botw_code_mods.py writes them into
// build/gen: the patched instructions in their code_NNN.c, the code caves, their table and the manifest in
// code_patches.c). The translated game reads each code mod's switch where the mod changes an instruction
// and the values of its presets in the instructions that use them; runtime/src/mods/code_patches.cpp
// sets both (Settings > Mods). With a switch off, the game runs its own instructions.
#pragma once
#include <stdint.h>

#include "ppc.h"

#ifdef __cplusplus
extern "C" {
#endif
#define BOTW_PATCH_MAX_MODS 64      /* tools/botw_code_mods.py MAX_MODS */
#define BOTW_PATCH_MAX_VALUES 1024  /* tools/botw_code_mods.py MAX_VALUES */
extern uint8_t botw_patch_switch[BOTW_PATCH_MAX_MODS];     // per code mod: 0 the game's code, 1 the mod's
extern uint32_t botw_patch_values[BOTW_PATCH_MAX_VALUES];  // instruction immediates from the presets
static inline int botw_patch_on(int mod) { return __atomic_load_n(&botw_patch_switch[mod], __ATOMIC_RELAXED); }
static inline uint32_t botw_patch_value(int slot) { return __atomic_load_n(&botw_patch_values[slot], __ATOMIC_RELAXED); }

typedef struct { uint32_t addr; PpcFunc fn; } BotwPatchFunc;
extern const BotwPatchFunc g_botw_patch_funcs[];  // the code caves' functions, at their guest addresses
extern const unsigned g_botw_patch_func_count;
extern const char g_botw_patch_manifest[];        // JSON: the packs built in (code_patches.h)
#ifdef __cplusplus
}
#endif
