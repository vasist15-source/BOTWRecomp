// Mouse camera, mouse buttons / wheel and quick select steps (see mouse_look.h).
#include "mouse_look.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace mouse_look {
namespace {
constexpr double kTau = 0.025;          // mouse speed smoothing (s)
constexpr float kPointsPerPush = 800;   // points/s for a full stick push at sensitivity 1
constexpr float kTail = 0.03f;          // below this push the floor fades out (no drift after the mouse stops)
constexpr double kPulse = 0.10;         // wheel notch on a button: held this long
constexpr double kQuickLead = 0.20;     // quick select: D-pad held before the first stick flick
constexpr double kQuickFlick = 0.10;    // stick flick, then as long at rest
constexpr double kQuickLinger = 0.60;   // D-pad let go this long after the last flick (equips)
constexpr int kQuickMax = 4;            // steps queued at most (trackpad momentum)

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
std::atomic<int> g_menu_event{kMenuNone};

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
    auto axis = [](float& x, float add) { x = std::clamp(x + add, -1.0f, 1.0f); };
    axis(s.lx, v[kLRight] - v[kLLeft]);
    axis(s.ly, v[kLUp] - v[kLDown]);
    axis(s.rx, v[kRRight] - v[kRLeft]);
    axis(s.ry, v[kRUp] - v[kRDown]);

    // mouse speed -> right stick
    if (dt > 1e-4) {
        const float a = (float)(1.0 - std::exp(-dt / kTau));
        g_vx += ((float)(dx / dt) - g_vx) * a;
        g_vy += ((float)(dy / dt) - g_vy) * a;
    } else {  // two reads at once: the movement waits for the next
        std::lock_guard<std::mutex> lk(g_mu);
        g_dx += dx, g_dy += dy;
    }
    const float gain = m.mouse_sensitivity / kPointsPerPush;
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

    // the game's menus (+ / −), from the presses
    const uint32_t pressed = s.buttons & ~g_prev_buttons;
    g_prev_buttons = s.buttons;
    const uint32_t menu = pressed & (input::kPlus | input::kMinus);
    if (!g_menu_button && menu) {
        g_menu_button = menu & input::kPlus ? input::kPlus : input::kMinus;
        g_menu_event = kMenuOpened;
    } else if (g_menu_button && ((pressed & input::kB) || (menu & g_menu_button))) {
        g_menu_button = 0;
        g_menu_event = kMenuClosed;
    }
}

int take_menu_event() { return g_menu_event.exchange(kMenuNone); }
void menu_reset() { g_menu_button = 0; }

}  // namespace mouse_look
