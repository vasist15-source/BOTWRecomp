// Pausing the game while the port's menu is open (Settings > Display > Pause the game while the menu is
// open, on by default). The overlay says it wants the pause (overlay.cpp); the game's frame thread parks at
// its next GX2SwapScanBuffers and shows the last picture again ~60 times a second so the menu keeps drawing
// (gx2_core.cpp); while it is parked the AX frame thread runs no audio frames, so voices, music and the
// game's sound threads stop where they are (hle/ax.cpp) and go on from there on resume.
#pragma once
#include <atomic>

namespace game_pause {
inline std::atomic<bool> wanted{false};  // overlay: the menu is open and pausing is on (any thread)
inline std::atomic<bool> held{false};    // the game's frame thread is parked at its swap
}  // namespace game_pause
