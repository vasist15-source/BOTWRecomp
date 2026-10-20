// Mouse capture for mouse and keyboard play (macOS). With the Controls tab's mouse controls on
// (input_map::Mapping::mouse_look), a click into the TV window captures the pointer: it is hidden and
// detached from the cursor position, its movement turns the camera and its buttons and wheel are the
// inputs the controls give them (mouse_look.h). Esc, a lone Cmd tap, the settings overlay, a dialog,
// leaving the window or the game's menus (+ / −, mouse_look::take_menu_event) release it; the
// overlay closing and the menu closing (+ / − / B) capture it again if it was captured. While the
// pointer is free over the game it is the Triforce pointer (triforce_cursor.mm).
// WWHD_NO_HOST_INPUT ignores the real mouse (test runs; WWHD_TEST_MOUSE injects movement).
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>  // CACurrentMediaTime
#include "../overlay/overlay.h"
#include <Carbon/Carbon.h>  // kVK_Escape

#include <atomic>

#include "mods.h"
#include "runtime.h"
#include "../input_map.h"
#include "../motion/motion.h"
#include "../mouse_look.h"

namespace gfx { bool drc_overlay_hit(void* window, double x, double y); }  // gfx/display.mm

namespace mods { void show_free_pointer(); }

// receives the TV picture's cursor updates (mouse_init)
@interface TriforcePointer : NSResponder
+ (instancetype)shared;
@end
@implementation TriforcePointer
+ (instancetype)shared {
    static TriforcePointer* p = [TriforcePointer new];
    return p;
}
- (void)cursorUpdate:(NSEvent*)e { mods::show_free_pointer(); }
- (void)mouseEntered:(NSEvent*)e { mods::show_free_pointer(); }
- (void)mouseExited:(NSEvent*)e { [[NSCursor arrowCursor] set]; }
@end

namespace mods {
NSCursor* triforce_cursor();  // triforce_cursor.mm
namespace {
NSWindow* __weak g_tv;
std::atomic<bool> g_captured{false};
double g_released_at = -10;   // when the pointer was last released (the overlay resumes a capture it ended)
bool g_menu_released = false; // released because the game's menu opened

bool in_tv(NSEvent* e) { return g_tv && e.window == g_tv; }
bool in_tv_content(NSEvent* e) { return in_tv(e) && NSPointInRect(e.locationInWindow, g_tv.contentView.frame); }
// the pointer over the game, while it is free (a captured one is hidden)
void show_pointer() {
    if (!g_tv || g_captured.load() || !NSApp.isActive) return;
    const NSPoint p = [g_tv mouseLocationOutsideOfEventStream];
    if (NSPointInRect(p, g_tv.contentView.frame)) [triforce_cursor() set];
}

bool gyro_capture_active();
void capture() {
    if (g_captured.exchange(true)) return;
    if (!gyro_capture_active()) {  // else the pointer is captured and hidden already
        CGAssociateMouseAndMouseCursorPosition(false);
        [NSCursor hide];
    }
    LOG("[mods] mouse captured (Esc or a Cmd tap releases)");
}
void release_now() {
    if (!g_captured.exchange(false)) return;
    g_released_at = CACurrentMediaTime();
    if (!gyro_capture_active()) {  // else the gyro's capture keeps the pointer until the aim ends
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
    }
    mouse_look::release_all();
    show_pointer();
    LOG("[mods] mouse released");
}
// capture again on our own (overlay or game menu closed): only into the active game window
void recapture() {
    if (g_tv && g_tv.isKeyWindow && NSApp.isActive && mouse_camera() && !overlay::captures() && !getenv("WWHD_NO_HOST_INPUT"))
        capture();
}
// once per input timer tick (main thread): the overlay and the game's menus
void update_capture() {
    static bool was_open = false, resume = false;
    const bool open = overlay::captures();
    if (open && !was_open) resume = g_captured.load() || CACurrentMediaTime() - g_released_at < 0.5;
    if (!open && was_open && resume) {
        resume = false;
        recapture();
    }
    was_open = open;
    switch (mouse_look::take_menu_event()) {
    case mouse_look::kMenuOpened:
        if (g_captured.load() && input_map::current().mouse_menu_release) {
            release_now();
            g_menu_released = true;
        }
        break;
    case mouse_look::kMenuClosed:
        if (g_menu_released) recapture();
        g_menu_released = false;
        break;
    default: break;
    }
}
}  // namespace

bool mouse_captured() { return g_captured.load(std::memory_order_relaxed); }
void show_free_pointer() { show_pointer(); }

// the mouse as a gyro (motion/motion.h): while the game aims the pointer is captured, so that Steam
// Input's gyro-to-mouse (or the mouse) never stops at the screen's edge; released when the aim ends.
// Main thread, from the input timer (gfx/input.mm).
static bool g_gyro_capture = false;
namespace { bool gyro_capture_active() { return g_gyro_capture; } }
void update_gyro_mouse() {
    update_capture();
    const bool want = g_tv && motion::mouse_drives_gyro() && !overlay::captures() && g_tv.isKeyWindow && NSApp.isActive &&
                      !getenv("WWHD_NO_HOST_INPUT");
    if (want && !g_gyro_capture) {
        g_gyro_capture = true;
        if (!g_captured.load()) {
            CGAssociateMouseAndMouseCursorPosition(false);
            [NSCursor hide];
            LOG("[gyro] mouse captured while the game aims");
        }
    } else if (!want && g_gyro_capture) {
        g_gyro_capture = false;
        if (!g_captured.load()) {
            CGAssociateMouseAndMouseCursorPosition(true);
            [NSCursor unhide];
        }
    }
}

void mouse_release() {
    if ([NSThread isMainThread]) release_now();
    else dispatch_async(dispatch_get_main_queue(), ^{ release_now(); });
}

bool host_key_down(uint16_t code) {
    if (code == kVK_Escape && mouse_captured()) {
        g_menu_released = false;
        mouse_release();
        return true;
    }
    return false;
}

void mouse_init(void* tv_window) {
    g_tv = (__bridge NSWindow*)tv_window;
    if (getenv("WWHD_NO_HOST_INPUT")) return;
    g_tv.acceptsMouseMovedEvents = YES;
    NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
                       NSEventMaskOtherMouseDragged | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                       NSEventMaskRightMouseDown | NSEventMaskRightMouseUp | NSEventMaskOtherMouseDown |
                       NSEventMaskOtherMouseUp | NSEventMaskScrollWheel;
    // a lone Cmd tap releases the pointer (Cmd with a key, e.g. the save states' Cmd+1, does not)
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged | NSEventMaskKeyDown handler:^NSEvent*(NSEvent* e) {
        static bool tap = false;
        static double down_at = 0;
        if (e.type == NSEventTypeKeyDown) {
            tap = false;
        } else if (e.modifierFlags & NSEventModifierFlagCommand) {
            tap = (e.keyCode == kVK_Command || e.keyCode == kVK_RightCommand) &&
                  !(e.modifierFlags & (NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagOption));
            down_at = CACurrentMediaTime();
        } else if (tap) {
            tap = false;
            if (CACurrentMediaTime() - down_at < 0.6 && mouse_captured()) {
                g_menu_released = false;
                release_now();
            }
        }
        return e;
    }];
    [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* e) {
        if (overlay::captures()) {  // the settings overlay (or the text prompt) has the mouse (gfx/overlay_appkit.mm)
            if (e.type == NSEventTypeMouseMoved && in_tv_content(e)) show_pointer();
            return e;
        }
        if (e.type == NSEventTypeScrollWheel) {
            if (mouse_camera() && mouse_captured()) {
                // the wheel's physical direction (natural scrolling flips the deltas); a notch is
                // about 1 line, trackpads send many small precise deltas and keep going after the swipe
                if (e.momentumPhase != NSEventPhaseNone) return nil;
                float dy = (float)e.scrollingDeltaY;
                if (e.isDirectionInvertedFromDevice) dy = -dy;
                if (e.hasPreciseScrollingDeltas) dy /= 10.0f;
                mouse_wheel(dy);
                return nil;
            }
            return e;
        }
        // the mouse as a gyro: movement in the game window (motion.h keeps it only while the game aims)
        if ((e.type == NSEventTypeMouseMoved || e.type == NSEventTypeLeftMouseDragged || e.type == NSEventTypeRightMouseDragged ||
             e.type == NSEventTypeOtherMouseDragged) && (in_tv(e) || mouse_captured() || g_gyro_capture)) {
            motion::mouse_motion((float)e.deltaX, (float)e.deltaY);
            if (motion::mouse_drives_gyro()) return nil;
        }
        if (!mouse_captured() && e.type == NSEventTypeMouseMoved && in_tv_content(e)) show_pointer();
        if (!mouse_camera()) return e;
        if (!mouse_captured()) {
            // a click into the game picture captures the pointer (the title bar stays usable)
            // (except on the GamePad picture-in-picture, where a click is a touch)
            if (e.type == NSEventTypeLeftMouseDown && in_tv_content(e) &&
                !gfx::drc_overlay_hit((__bridge void*)g_tv, e.locationInWindow.x, e.locationInWindow.y)) {
                g_menu_released = false;
                capture();
                return nil;
            }
            return e;
        }
        switch (e.type) {
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            mouse_add((float)e.deltaX, (float)e.deltaY);  // deltaY > 0: down
            break;
        case NSEventTypeLeftMouseDown: mouse_button(input_map::kMouseLeft, true); break;
        case NSEventTypeLeftMouseUp: mouse_button(input_map::kMouseLeft, false); break;
        case NSEventTypeRightMouseDown: mouse_button(input_map::kMouseRight, true); break;
        case NSEventTypeRightMouseUp: mouse_button(input_map::kMouseRight, false); break;
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:  // button 2 middle, 3 back, 4 forward
            if (e.buttonNumber >= 2 && e.buttonNumber <= 4)
                mouse_button(input_map::kMouseMiddle + (int)e.buttonNumber - 2, e.type == NSEventTypeOtherMouseDown);
            break;
        default: break;
        }
        return nil;
    }];
    // leaving the game (other app, other window) releases the pointer
    [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidResignActiveNotification
                                                      object:nil queue:nil usingBlock:^(NSNotification*) { release_now(); }];
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification
                                                      object:g_tv queue:nil usingBlock:^(NSNotification*) { release_now(); }];
    // the free pointer over the game is the Triforce one: on entering the picture and whenever AppKit
    // puts the arrow back (cursor updates), as well as on every move (the monitor above)
    NSTrackingArea* area = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingCursorUpdate | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:[TriforcePointer shared]
            userInfo:nil];
    [g_tv.contentView addTrackingArea:area];
}

}  // namespace mods
