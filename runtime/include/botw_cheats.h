// Cheats switched in Settings > Cheats (runtime/src/mods/cheats.cpp). The translated game reads the
// switches at the sites tools/botw_cheat_patches.py names; with a switch off, a site runs the game's
// own instruction.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    BOTW_CHEAT_ARROWS,          // shooting keeps the arrow count (while two or more are left)
    BOTW_CHEAT_WEAPONS,         // swords, spears and bows lose no durability
    BOTW_CHEAT_SHIELDS,         // shields lose no durability
    BOTW_CHEAT_REVALI,          // champion abilities: uses are not counted down
    BOTW_CHEAT_DARUK,
    BOTW_CHEAT_URBOSA,
    BOTW_CHEAT_MIPHA,           // recharged as soon as it is used
    BOTW_CHEAT_HEARTS,          // damage does not lower the player's health
    BOTW_CHEAT_STAMINA,         // stamina is not used up
    BOTW_CHEAT_CYCLE_FUEL,      // Master Cycle Zero fuel is not used up
    BOTW_CHEAT_CYCLE_ANYWHERE,  // Master Cycle Zero can be summoned everywhere
    BOTW_CHEAT_AMIIBO,          // each amiibo can be read again the same day
    BOTW_CHEAT_COUNT
};
extern uint8_t botw_cheats[BOTW_CHEAT_COUNT];  // 1 = on; any thread may switch them
static inline int botw_cheat(int id) { return __atomic_load_n(&botw_cheats[id], __ATOMIC_RELAXED); }
// a weapon actor's durability loss: its type (+0x938) 4 is a shield; 0-3 are swords, spears and bows
static inline int botw_cheat_unbreakable(uint32_t weapon_type) {
    return botw_cheat(weapon_type == 4 ? BOTW_CHEAT_SHIELDS : BOTW_CHEAT_WEAPONS);
}
#ifdef __cplusplus
}
#endif
