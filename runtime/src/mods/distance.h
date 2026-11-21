// View distances (Settings > Graphics > View distance): the Cemu Draw Distance mod's choices as live
// sliders. The translated game reads botw_distance[] (botw_distance.h) at the sites of
// tools/botw_cheat_patches.py DISTANCE_SITES; grass density is a game data word written here; texture
// detail is a LOD bias the Metal renderer adds to compressed textures (gfx/metal_draw.mm).
// Every choice applies at once and is saved in the host settings (drawActors, drawObjects, drawTrees,
// drawGrass, textureDetail). The defaults are the game's own: its code runs unchanged.
//   WWHD_DISTANCE=off: the game's own for this run (saved choices are kept, not applied)
#pragma once

namespace mods::distance {

// slider ranges (the pack's presets that need no Extended Memory pack)
constexpr float kActorMin = 0.5f, kActorMax = 1.5f;    // 1: the game's own (with its load balancing)
constexpr float kObjectMin = 0.75f, kObjectMax = 1.25f;  // 1: the game's own; below 0.75 events break
constexpr int kTreeLevels = 6, kTreeDefault = 2;         // Potato .. Extreme
constexpr int kGrassLevels = 6, kGrassDefault = 2;
constexpr int kTextureMin = -3, kTextureMax = 2;         // levels; negative: sharper in the distance
extern const char* const kTreeNames[kTreeLevels];
extern const char* const kGrassNames[kGrassLevels];

// main thread, after the game's code is loaded, before the game runs: the saved choices
void startup();

float actors();   // multiplier
float objects();  // multiplier
int trees();      // level
int grass();      // level
int texture_detail();  // LOD levels added (0: none)
float texture_bias();  // what the renderer adds to compressed textures (any thread)

// any thread; save: also write the host setting (posted to the main thread)
void set_actors(float v, bool save);
void set_objects(float v, bool save);
void set_trees(int level, bool save);
void set_grass(int level, bool save);
void set_texture_detail(int levels, bool save);
void reset();  // all back to the game's own (saved)
bool disabled();  // WWHD_DISTANCE=off

}  // namespace mods::distance
