// Mouse camera, mouse buttons / wheel and quick select steps (see mouse_look.h).
#include "mouse_look.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace mouse_look {
namespace {
constexpr double kTau = 0.025;          // mouse speed smoothing (s)
constexpr float kPointsPerPush = 320;   // points/s for a full stick push at sensitivity 1
constexpr float kTail = 0.03f;          // below this push the floor fades out (no drift after the mouse stops)
constexpr double kPulse = 0.10;         // wheel notch on a button: held this long
constexpr double kQuickLead = 0.20;     // quick select: D-pad held before the first stick flick
constexpr double kQuickFlick = 0.10;    // stick flick, then as long at rest
constexpr double kQuickLinger = 0.60;   // D-pad let go this long after the last flick (equips)
constexpr int kQuickMax = 4;            // steps queued at most (trackpad momentum)
constexpr float kNavStep = 70;          // menus: points of movement per highlight step at sensitivity 1
constexpr double kNavFlick = 0.07;      // menus: left stick flick per step, then as long at rest

std::mutex g_mu;
float g_dx = 0, g_dy = 0;               // movement not yet taken by apply
bool g_held[input_map::kMouseCount] = {};
float g_wheel = 0;                      // wheel lines not yet a notch
int g_notches[2] = {};                  // whole notches not yet applied: [0] up, [1] down
int g_quick = 0;                        // quick select steps queued (+ next, − previous)

// game thread (apply)
double g_last = -1;
float g_vx = 0, g_vy = 0;               // smoothed mouse speed, points/s
double g_pulse_until[input_map::kActionCount] = {};
bool g_quick_on = false;                // quick select holds the D-pad (or a held D-pad is the player's)
uint32_t g_quick_bit = 0;
double g_flick_until = 0, g_next_flick = 0, g_quick_release = 0;
float g_flick = 0;
uint32_t g_prev_buttons = 0;
std::atomic<uint32_t> g_menu_button{0};  // + or − that opened the menu, 0 = closed
uint32_t g_menu_screen = 0;             // + (inventory) or − (map): the last one pressed in the menu
float g_nav_x = 0, g_nav_y = 0;         // menus: movement not yet a highlight step
double g_nav_until = 0, g_nav_next = 0;
float g_nav_fx = 0, g_nav_fy = 0;       // the current flick
std::atomic<int> g_menu_event{kMenuNone};

// the game's menus (+ / −), from the presses
void track_menu(uint32_t buttons) {
    const uint32_t pressed = buttons & ~g_prev_buttons;
    g_prev_buttons = buttons;
    const uint32_t menu = pressed & (input::kPlus | input::kMinus);
    const uint32_t first = menu & input::kPlus ? input::kPlus : input::kMinus;
    if (!g_menu_button && menu) {
        g_menu_button = g_menu_screen = first;
        g_nav_x = g_nav_y = 0;
        g_menu_event = kMenuOpened;
    } else if (g_menu_button && ((pressed & input::kB) || (menu & g_menu_button))) {
        g_menu_button = 0;
        g_menu_event = kMenuClosed;
    } else if (g_menu_button && menu) {
        g_menu_screen = first;  // the other screen (inventory <-> map)
    }
}

void queue_quick(int dir) { g_quick = std::clamp(g_quick + dir, -kQuickMax, kQuickMax); }
bool is_quick(int a) { return a == input_map::kQuickPrev || a == input_map::kQuickNext; }
int quick_dir(int a) { return a == input_map::kQuickNext ? 1 : -1; }
}  // namespace

void motion(float dx, float dy) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_dx += dx;
    g_dy += dy;
}

void button(int b, bool down) {
    if (b < 0 || b >= input_map::kWheelUp) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (down && !g_held[b]) {
        const int a = input_map::current().mouse[b];
        if (is_quick(a)) queue_quick(quick_dir(a));
    }
    g_held[b] = down;
}

void wheel(float dy) {
    std::lock_guard<std::mutex> lk(g_mu);
    if ((g_wheel > 0) != (dy > 0)) g_wheel = 0;  // a turn the other way starts afresh
    g_wheel += dy;
    while (std::fabs(g_wheel) >= 1.0f) {
        const bool up = g_wheel > 0;
        g_wheel -= up ? 1.0f : -1.0f;
        const int a = input_map::current().mouse[up ? input_map::kWheelUp : input_map::kWheelDown];
        if (is_quick(a)) queue_quick(quick_dir(a));
        else g_notches[up ? 0 : 1]++;
    }
}

void quick_step(int dir) {
    std::lock_guard<std::mutex> lk(g_mu);
    queue_quick(dir > 0 ? 1 : -1);
}

void release_all() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_dx = g_dy = 0;
    g_wheel = 0;
    g_notches[0] = g_notches[1] = 0;
    std::fill(std::begin(g_held), std::end(g_held), false);
}

void apply(input::PadState& s, const input_map::Mapping& m, double now) {
    using namespace input_map;
    float dx, dy;
    bool held[kMouseCount];
    int notches[2], quick;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        dx = g_dx, dy = g_dy;
        g_dx = g_dy = 0;
        std::copy(std::begin(g_held), std::end(g_held), held);
        notches[0] = g_notches[0], notches[1] = g_notches[1];
        g_notches[0] = g_notches[1] = 0;
        quick = g_quick;  // taken one flick at a time below
    }
    double dt = g_last < 0 ? 1.0 / 60 : now - g_last;
    if (dt > 0.25) dt = 1.0 / 60;  // a pause (loading, the overlay): no burst from a stale speed
    g_last = now;

    auto axis = [](float& x, float add) { x = std::clamp(x + add, -1.0f, 1.0f); };
    // mouse speed, smoothed
    if (dt > 1e-4) {
        const float a = (float)(1.0 - std::exp(-dt / kTau));
        g_vx += ((float)(dx / dt) - g_vx) * a;
        g_vy += ((float)(dy / dt) - g_vy) * a;
    } else {  // two reads at once: the movement waits for the next
        std::lock_guard<std::mutex> lk(g_mu);
        g_dx += dx, g_dy += dy;
        dx = dy = 0;
    }
    const float gain = m.mouse_sensitivity / kPointsPerPush;

    if (m.mouse_menu_nav && g_menu_button) {
        // the game's menus: the mouse moves the highlight (inventory: one left-stick flick per step) or
        // the map's cursor (the left stick, as the camera's right stick), left click A, right click B,
        // the wheel L / R
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_quick = 0;
        }
        g_quick_on = false;
        if (held[kMouseLeft]) s.buttons |= input::kA;
        if (held[kMouseRight]) s.buttons |= input::kB;
        if (notches[0]) g_pulse_until[kL] = now + kPulse;
        if (notches[1]) g_pulse_until[kR] = now + kPulse;
        if (now < g_pulse_until[kL]) s.buttons |= input::kL;
        if (now < g_pulse_until[kR]) s.buttons |= input::kR;
        if (g_menu_screen == input::kMinus) {
            const float sx = std::clamp(g_vx * gain, -1.0f, 1.0f), sy = std::clamp(-g_vy * gain, -1.0f, 1.0f);
            axis(s.lx, std::fabs(sx) > 0.02f ? sx : 0);
            axis(s.ly, std::fabs(sy) > 0.02f ? sy : 0);
        } else {
            const float step = kNavStep / std::max(m.mouse_sensitivity, 0.05f);
            g_nav_x = std::clamp(g_nav_x + dx, -2 * step, 2 * step);
            g_nav_y = std::clamp(g_nav_y + dy, -2 * step, 2 * step);
            if (now >= g_nav_until && now >= g_nav_next) {
                g_nav_fx = g_nav_fy = 0;
                if (std::fabs(g_nav_x) >= step && std::fabs(g_nav_x) >= std::fabs(g_nav_y)) {
                    g_nav_fx = g_nav_x > 0 ? 1.0f : -1.0f;
                    g_nav_x -= g_nav_fx * step;
                    g_nav_y = 0;  // one direction at a time: no diagonal drift
                } else if (std::fabs(g_nav_y) >= step) {
                    g_nav_fy = g_nav_y > 0 ? -1.0f : 1.0f;  // mouse down: stick down
                    g_nav_y += g_nav_fy * step;
                    g_nav_x = 0;
                }
                if (g_nav_fx || g_nav_fy) {
                    g_nav_until = now + kNavFlick;
                    g_nav_next = g_nav_until + kNavFlick;
                }
            }
            if (now < g_nav_until) {
                axis(s.lx, g_nav_fx);
                axis(s.ly, g_nav_fy);
            }
        }
        track_menu(s.buttons);
        return;
    }
    g_nav_x = g_nav_y = 0;

    // mouse buttons and wheel notches -> actions (wheel: a short press per notch)
    float v[kActionCount] = {};
    for (int b = 0; b < kWheelUp; b++)
        if (held[b] && m.mouse[b] >= 0 && m.mouse[b] < kActionCount && !is_quick(m.mouse[b])) v[m.mouse[b]] = 1;
    for (int i = 0; i < 2; i++) {
        const int a = m.mouse[i ? kWheelDown : kWheelUp];
        if (notches[i] && a >= 0 && a < kActionCount) g_pulse_until[a] = now + kPulse;
    }
    for (int a = 0; a < kActionCount; a++)
        if (now < g_pulse_until[a]) v[a] = 1;
    for (int a = 0; a < kActionCount; a++)
        if (v[a]) s.buttons |= action_bit(a);
    axis(s.lx, v[kLRight] - v[kLLeft]);
    axis(s.ly, v[kLUp] - v[kLDown]);
    axis(s.rx, v[kRRight] - v[kRLeft]);
    axis(s.ry, v[kRUp] - v[kRDown]);

    // mouse speed -> right stick
    float sx = g_vx * gain, sy = -g_vy * gain;  // mouse up (dy < 0): stick up
    if (m.mouse_invert_y) sy = -sy;
    const float mag = std::hypot(sx, sy), floor = std::clamp(m.mouse_floor, 0.0f, 0.5f);
    if (mag > 0.005f) {  // below: at rest (4 points/s)
        const float push = mag >= kTail ? floor + (1 - floor) * std::min(mag, 1.0f) : mag / kTail * (floor + (1 - floor) * kTail);
        sx *= push / mag, sy *= push / mag;
    } else {
        sx = sy = 0;
    }

    // quick select: hold the D-pad, flick the right stick once per step, let go
    constexpr uint32_t kDpad = input::kUp | input::kDown | input::kLeft | input::kRight;
    if (quick && !g_quick_on) {
        g_quick_on = true;
        g_quick_bit = (s.buttons & kDpad) ? 0 : input::kRight;  // a bar the player holds open, or weapons
        g_next_flick = now + (g_quick_bit ? kQuickLead : 0.0);
        g_flick_until = 0;
    }
    if (g_quick_on) {
        s.buttons |= g_quick_bit;
        float flick = 0;
        if (now < g_flick_until) {
            flick = g_flick;
        } else if (now >= g_next_flick) {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_quick) {
                g_flick = g_quick > 0 ? 1.0f : -1.0f;
                g_quick -= (int)g_flick;
                g_flick_until = now + kQuickFlick;
                g_next_flick = g_flick_until + kQuickFlick;
                g_quick_release = g_flick_until + kQuickLinger;
                flick = g_flick;
            } else if (now >= g_quick_release) {
                g_quick_on = false;  // let go: the game equips what the bar shows
            }
        }
        s.rx = flick;  // the mouse and the camera keys wait while the bar is open
        s.ry = 0;
    } else {
        axis(s.rx, sx);
        axis(s.ry, sy);
    }

    track_menu(s.buttons);
}

int take_menu_event() { return g_menu_event.exchange(kMenuNone); }

}  // namespace mouse_look
