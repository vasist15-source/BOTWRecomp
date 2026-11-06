#pragma once
#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

#include "input_map.h"

namespace overlay {

// Hands one controller back to the game after the settings overlay or the game's text prompt had the
// input (ported from upstream ZeldaWWHDRecomp #153, 003465b4 and 7fa8823a). The inputs the device holds
// while the overlay has the input (the button that confirmed the prompt, B, Home or Select that closed
// the menu) stay away from the game until each one is released; every other input reaches the game at
// once.
//
// Before, one latch kept the whole game input, keyboard included, until every input of every
// controller read below 0.3. An input that rests above that (a trigger or stick that does not return to
// zero, an input the game does not even use) kept the game without input until the controller was
// unplugged, and a button held on one pad also held back the same button on another.
//
// The host's input pump (gfx/input.mm) gives each device's values (input_map pad order) to a gate on
// every poll, also while the overlay has the input, and the game the gated values. A device that
// connects gets a new gate, so it holds nothing back. (platform/input_sdl.cpp, the SDL host of
// Vulkan-only builds, which ours are not, does not gate yet: upstream's commits have that half.)
class ReleaseGate {
public:
    // input_map::controller_state presses a game button above this (also the analog inputs it maps
    // to buttons): an input at or below it is released
    static constexpr float kHeld = 0.5f;

    // captured: the overlay or the text prompt has the input now (overlay::captures())
    void filter(float values[input_map::kPadCount], bool captured) {
        for (int p = 0; p < input_map::kPadCount; p++) {
            const bool held = values[p] > kHeld;
            if (captured) held_back_[p] = held_back_[p] || held;  // the overlay's input
            else held_back_[p] = held_back_[p] && held;           // released: the next press is the game's
            if (captured || held_back_[p]) values[p] = 0;
        }
    }
    // an input still kept from the game (diagnostics, tests)
    bool holds_back() const { return std::any_of(std::begin(held_back_), std::end(held_back_), [](bool b) { return b; }); }
    bool holds_back(int pad) const { return pad >= 0 && pad < input_map::kPadCount && held_back_[pad]; }

private:
    bool held_back_[input_map::kPadCount] = {};
};

// One gate per connected device (the AppKit host keys them by GCController). combine() gates each
// device's values, then combines the devices (strongest input wins) for the game; raw gets the same
// combination without the gates (the overlay's own navigation reads it). A device missing from a poll
// loses its gate: when it comes back (reconnected) it holds nothing back.
using PadValues = std::array<float, input_map::kPadCount>;
template <class Key>
class DeviceGates {
public:
    void combine(const std::vector<std::pair<Key, PadValues>>& devices, bool captured,
                 float raw[input_map::kPadCount], float game[input_map::kPadCount]) {
        std::fill(raw, raw + input_map::kPadCount, 0.0f);
        std::fill(game, game + input_map::kPadCount, 0.0f);
        std::map<Key, ReleaseGate> kept;
        for (const auto& [key, values] : devices) {
            PadValues gated = values;
            ReleaseGate& gate = kept[key];
            if (auto old = gates_.find(key); old != gates_.end()) gate = old->second;
            gate.filter(gated.data(), captured);
            for (int p = 0; p < input_map::kPadCount; p++) {
                raw[p] = std::max(raw[p], values[p]);
                game[p] = std::max(game[p], gated[p]);
            }
        }
        gates_.swap(kept);
    }
    size_t size() const { return gates_.size(); }
    bool holds_back(const Key& key, int pad) const {
        auto i = gates_.find(key);
        return i != gates_.end() && i->second.holds_back(pad);
    }

private:
    std::map<Key, ReleaseGate> gates_;
};

}  // namespace overlay
