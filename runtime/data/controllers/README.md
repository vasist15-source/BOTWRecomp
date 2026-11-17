# Controller pictures (Settings > Controls)

The Controls page shows the connected controller. For these four it is a vector drawing, rendered 1000
pixels wide into the `.heic` files here by `tools/controller_images.py`. CMake puts each `.heic` into the
macOS executable (`__PADS,<name>`); `hostui::controller_picture` decodes it. Where the buttons are on each
picture is the `picture_def` table in `runtime/src/overlay/controls_view.cpp`, which the script prints.

| File | Controller | Source | Licence |
| --- | --- | --- | --- |
| `xbox.heic` | Xbox One controller | "Xbox One.svg" of the [8th Gen Console Vector Gamepad Collection](https://opengameart.org/content/8th-gen-console-vector-gamepad-collection) by TheHoodieGuy02, unchanged | CC0 |
| `dualshock4.heic` | DualShock 4 | "Dualshock 4.svg" of the same collection, unchanged | CC0 |
| `switchpro.svg`, `switchpro.heic` | Switch Pro Controller | made for this project in the collection's style, from its parts: a body and shoulders drawn in `tools/controller_images.py`, the sticks and D-pad of its Xbox One drawing, the letters and the Home icon of its Joy-Con drawing | CC0 |
| `dualsense.svg`, `dualsense.heic` | DualSense | made for this project from the collection's DualShock 4: a new body, a dark centre, plain symbols, no lettering or light bar, the mute button | CC0 |

The collection's author removed the console logos from it; the drawings carry none. The controllers' names
are trademarks of their owners (Microsoft, Sony, Nintendo); the pictures only show the player which
controller is connected.

No picture: the Steam Controller and generic pads. The page draws those itself (`host_def` in
`controls_view.cpp`), as it draws the Wii U GamePad and Pro Controller when no controller is connected.
