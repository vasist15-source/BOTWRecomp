// Mouse and keyboard play: the captured mouse as the right stick, the mouse buttons and wheel as the
// inputs the controls mapping gives them (input_map.h: Mapping::mouse), and the quick select steps.
//
// Plain C++ (no AppKit) so it can be unit-tested (runtime/tools/input_map_test.cpp). The hosts feed it
// through mods::mouse_add / mouse_button / mouse_wheel (mods/mouse.mm, platform/mouse_sdl.cpp, only
// while the pointer is captured) and apply() runs at the end of every input read (mods::filter_pad).
//
// Mouse camera: the game turns its camera at a rate set by the right stick, so the mouse's speed
// (points per second, smoothed over about 30 ms) becomes the stick's push: Mapping::mouse_sensitivity
// 1 is a full push at 320 points/s. A push smaller than Mapping::mouse_floor is raised to it, so slow
// movement still gets past the game's own stick dead zone.
//
// Quick select (the game's item bar): holding a D-pad direction shows the runes (↑), weapons (→) or
// shields (←), and the right stick steps through them; letting go equips. One step (a wheel notch, or
// the Quick select keys) holds the D-pad (→, weapons, unless a D-pad direction is already held: then
// that bar) and flicks the right stick once; the D-pad is let go 0.6 s after the last step.
#pragma once
#include <cstdint>

#include "input.h"
#include "input_map.h"

namespace mouse_look {

// any thread (the hosts' event handlers)
void motion(float dx, float dy);  // points moved while captured, dy > 0: down
void button(int b, bool down);    // input_map::Mouse button (kMouseLeft..kMouseForward)
void wheel(float dy);             // wheel lines (dy > 0: up / away from the player), trackpads in fractions
void quick_step(int dir);         // -1 previous, +1 next (the Quick select keys or controller inputs)
void release_all();               // the pointer was released: buttons up, movement dropped

// the input read (game thread): adds the mouse to `s` at time `now` (seconds, any steady clock)
void apply(input::PadState& s, const input_map::Mapping& m, double now);

// The game's menus, as far as the input shows them: + or − opens the menu (the inventory or the
// map), B or the button that opened it again closes it (the other one switches screens). The port
// cannot see the game's own menu state, so this follows the presses. While it is open (and
// Mapping::mouse_menu_nav) the mouse moves the highlight instead of the camera: in the inventory one
// left-stick flick per kNavStep points, on the map the left stick as the camera's right stick; left
// click is A, right click B, the wheel L / R. take_menu_event() returns and clears the last change.
enum MenuEvent { kMenuNone, kMenuOpened, kMenuClosed };
int take_menu_event();

}  // namespace mouse_look
