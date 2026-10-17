#include "../mods/manager.h"
// Menu bar: app menu (Quit, which asks first while a game is in progress: quit_prompt.mm), a Window
// menu (Close Window on the TV window quits the same way) and a Graphics menu to switch fixes and
// enhancements while playing. Each option also has a single-key shortcut in the game window.
#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>  // kVK_* key codes
#include "../input.h"
#include "../platform/host.h"
#include <sys/stat.h>
#include <ctime>

#include "../interp.h"
#include "../savestate.h"
#include "../screenshot.h"
#include "../input_map.h"
#include "../overlay/hostui.h"
#include "../overlay/overlay.h"
#include "../mods/code_mods.h"
#include "../crashrec.h"
#include "../aspect.h"
#include "renderer.h"
#ifdef WWHD_HAS_VULKAN
#include "vulkan/settings.h"  // Presentation (present mode), kept with the other graphics options
#endif
#include "runtime.h"

// graphics options and capture go to the renderer in use (Metal or Vulkan: renderer.h)
namespace gfx {
bool drc_window_available();
bool drc_window_shown();
void show_drc_window(bool on);
void install_display_menu(NSMenu* bar);
void set_host_setting(const char* key, const std::string& value);  // display.mm
}  // namespace gfx

// internal resolution steps (Graphics menu; R cycles)
static const float kResScales[] = {1.0f, 1.5f, 2.0f, 3.0f};
static float g_res_shown = 0;  // last value set from the UI (res_scale() lags a frame)
static float current_res_scale() { return g_res_shown ? g_res_shown : render::res_scale(); }
static void set_res(float f) { g_res_shown = f; render::set_res_scale(f); }
static void cycle_res() {
    float cur = current_res_scale();
    size_t n = sizeof kResScales / sizeof *kResScales, i = 0;
    while (i < n && kResScales[i] <= cur + 0.01f) i++;
    set_res(kResScales[i % n]);
}



namespace gx2 { uint64_t flips_presented(); bool uncapped(); int fps_target(); void set_fps_target(int fps); }
#include "../mods/mods.h"
#include "../overlay/overlay.h"
namespace ax { void start_sound_trace(const char* path, double seconds); }
namespace gfx { bool menu_hotkey(uint16_t code); NSMenuItem* controls_menu_item(); /* controls_ui.mm */ void install_overlay_input(); }

static NSWindow* g_tv;
static double g_fps = 0;  // frames presented per second, measured over the last half second
static NSString* const kTitle = @"The Legend of Zelda: Breath of the Wild (recompiled)";

// Graphics options are kept across launches (macOS user defaults, domain "wwhd"); an option's WWHD_*
// environment variable overrides the saved value for that run and is not saved. Scripted test runs
// (WWHD_NO_HOST_INPUT) neither read nor write them.
static const bool g_prefs = getenv("WWHD_NO_HOST_INPUT") == nullptr;
static bool g_prefs_loaded = false;  // nothing is saved before the saved values were applied
static bool env_set(std::initializer_list<const char*> env) {
    for (const char* e : env)
        if (getenv(e)) return true;
    return false;
}
// Saved graphics options: NSUserDefaults, or in portable mode a plist in the folder (portable.txt).
static NSString* portable_prefs_path() {
    return host::portable() ? @((host::portable_user_dir() + "/graphics.plist").c_str()) : nil;
}
static NSMutableDictionary* g_portable_prefs;
static id pref(NSString* key) {
    if (NSString* p = portable_prefs_path()) {
        if (!g_portable_prefs)
            g_portable_prefs = [[NSDictionary dictionaryWithContentsOfFile:p] mutableCopy] ?: [NSMutableDictionary new];
        return g_portable_prefs[key];
    }
    return [NSUserDefaults.standardUserDefaults objectForKey:key];
}
static void set_pref(NSString* key, id value) {
    if (portable_prefs_path()) {
        pref(key);  // loads the file
        g_portable_prefs[key] = value;
    } else {
        [NSUserDefaults.standardUserDefaults setObject:value forKey:key];
    }
}
static void load_prefs() {
    g_prefs_loaded = true;
    if (!g_prefs) return;
    auto saved = [&](NSString* key, std::initializer_list<const char*> env) { return !env_set(env) && pref(key) != nil; };
    if (saved(@"resScale", {"WWHD_RES_SCALE"})) set_res([pref(@"resScale") floatValue]);
    if (saved(@"aniso", {"WWHD_ANISO"})) render::set_aniso([pref(@"aniso") boolValue]);
    if (saved(@"fxaa", {"WWHD_FXAA"})) render::set_fxaa([pref(@"fxaa") boolValue]);
    // frame rate (BotW): 30 (the game's own), 60, or 0 (no cap). The Wind Waker interpolation modes
    // (fps60, interpFps) do not apply to this game and are not loaded.
    if (saved(@"botwFps", {"WWHD_FPS"})) gx2::set_fps_target([pref(@"botwFps") intValue]);
#ifdef WWHD_HAS_VULKAN
    if (saved(@"vkPresentMode", {"WWHD_VK_PRESENT_MODE"})) gfxvk::set_present_mode([pref(@"vkPresentMode") intValue]);
#endif
}
static void save_prefs() {
    if (!g_prefs || !g_prefs_loaded) return;
    if (!env_set({"WWHD_RES_SCALE"})) set_pref(@"resScale", @(current_res_scale()));
    if (!env_set({"WWHD_ANISO"})) set_pref(@"aniso", @(render::aniso()));
    if (!env_set({"WWHD_FXAA"})) set_pref(@"fxaa", @(render::fxaa()));
    if (!env_set({"WWHD_FPS"})) set_pref(@"botwFps", @(gx2::fps_target()));
#ifdef WWHD_HAS_VULKAN
    if (!env_set({"WWHD_VK_PRESENT_MODE"})) set_pref(@"vkPresentMode", @(gfxvk::present_mode()));
#endif
    if (NSString* p = portable_prefs_path()) [g_portable_prefs writeToFile:p atomically:YES];
}

// the TV title summarises the active options so a key press is visible without opening the menu;
// it starts with the renderer in use (and notes a fallback or a choice waiting for a restart).
// Every option change ends here, so this also saves them
static void update_title() {
    save_prefs();
    NSString* res = current_res_scale() != 1.0f ? [NSString stringWithFormat:@" \u00b7 %gx res", current_res_scale()] : @"";
    NSString* rnd = @(render::api_name(render::active()));
    if (render::active() != render::requested())
        rnd = [rnd stringByAppendingFormat:@" (%s unavailable)", render::api_name(render::requested())];
    else if (render::restart_pending())
        rnd = [rnd stringByAppendingFormat:@" (%s after restart)", render::api_name(render::preferred())];
    // the middle dots of the optional parts are NSStrings: %s would read UTF-8 as Mac Roman ("¬∑")
    NSString* t = [NSString stringWithFormat:@"%@ \u2014 %@ \u00b7 %.0f fps%@ \u00b7 AF: %s%@%@", kTitle, rnd, g_fps, res,
                                             render::aniso() ? "16x" : "game",
                                             interp::mode() ? [@" \u00b7 " stringByAppendingString:@(interp::mode_name())] : @"", render::fxaa() ? @" \u00b7 FXAA" : @""];
    if (gx2::uncapped()) t = [t stringByAppendingString:@" \u00b7 UNCAPPED (debug)"];
    std::string msg = ss::last_message();  // save state confirmations
    if (!msg.empty()) t = [NSString stringWithFormat:@"%@ \u2014 %@ \u2014 %s", kTitle, rnd, msg.c_str()];
    if (getenv("WWHD_LOG_TITLE") && ![t isEqualToString:g_tv.title]) {  // tests: the window title as it changes
        NSString* noFps = [t stringByReplacingOccurrencesOfString:[NSString stringWithFormat:@"%.0f fps", g_fps] withString:@"N fps"];
        static NSString* last;
        if (![noFps isEqualToString:last]) LOG("[display] title: %s", noFps.UTF8String);
        last = noFps;
    }
    [g_tv setTitle:t];
}

// Graphics > Renderer: saved for the next start; offers to restart right away
static void choose_renderer(render::Api a) {
    if (a == render::preferred()) return;
    render::set_preferred(a);
    update_title();
    if (a == render::active()) return;  // back to the renderer in use: nothing to restart
    if (getenv("WWHD_NO_HOST_INPUT")) return;  // test runs: no dialogs
    NSAlert* alert = [NSAlert new];
    alert.messageText = [NSString stringWithFormat:@"The game will use %s after a restart.", render::api_name(a)];
    alert.informativeText = @"Restart now? Progress since your last save (in-game save or save state) is lost.";
    [alert addButtonWithTitle:@"Restart Now"];
    [alert addButtonWithTitle:@"Later"];
    [alert beginSheetModalForWindow:g_tv completionHandler:^(NSModalResponse r) {
        if (r != NSAlertFirstButtonReturn) return;
        if (!render::restart()) {
            NSAlert* e = [NSAlert new];
            e.messageText = @"The game could not restart itself.";
            e.informativeText = @"Quit and start it again to switch the renderer.";
            [e beginSheetModalForWindow:g_tv completionHandler:nil];
        }
    }];
}

// Save States menu: items are rebuilt each time it opens (slot time and area)
@interface WWStateMenu : NSObject <NSMenuDelegate>
@end
@implementation WWStateMenu
- (void)save:(NSMenuItem*)item { ss::request_save((int)item.tag); }
- (void)load:(NSMenuItem*)item { ss::request_load((int)item.tag); }
- (void)toggleCrashRecovery:(NSMenuItem*)item { crashrec::set_enabled(!crashrec::enabled()); }
- (void)toggleFullStates:(NSMenuItem*)item { ss::set_full_states(!ss::full_states()); }
- (void)loadAuto:(NSMenuItem*)item { crashrec::request_load((int)item.tag); }
- (void)menuNeedsUpdate:(NSMenu*)m {
    [m removeAllItems];
    ss::SlotInfo info[ss::kSlots + 1];
    for (int i = 1; i <= ss::kSlots; i++) info[i] = ss::slot_info(i);
    auto label = [&](int i) -> NSString* {
        const ss::SlotInfo& s = info[i];
        if (!s.used) return @"empty";
        NSString* d = [NSString stringWithFormat:@"%s%s%s%s", s.when.c_str(), s.area.empty() ? "" : " · ", s.area.c_str(),
                                                    s.portable ? "" : " · full"];
        if (!s.controller.empty()) d = [d stringByAppendingFormat:@" · %s", s.controller.c_str()];
        return s.compatible ? d : [d stringByAppendingString:@" (incompatible)"];
    };
    for (int i = 1; i <= ss::kSlots; i++) {
        NSMenuItem* it = [m addItemWithTitle:[NSString stringWithFormat:@"Save to slot %d  (%@)", i, label(i)] action:@selector(save:) keyEquivalent:@""];
        it.target = self;
        it.tag = i;
        it.toolTip = [NSString stringWithFormat:@"Shortcut in game: Shift+F%d", i];
    }
    [m addItem:[NSMenuItem separatorItem]];
    for (int i = 1; i <= ss::kSlots; i++) {
        NSMenuItem* it = [m addItemWithTitle:[NSString stringWithFormat:@"Load slot %d  (%@)", i, label(i)] action:@selector(load:) keyEquivalent:@""];
        it.target = self;
        it.tag = i;
        it.enabled = info[i].used && info[i].compatible;
        it.toolTip = i == 1 ? @"In game: F1 (or \u2318,) opens the settings overlay (Saves)" : [NSString stringWithFormat:@"Shortcut in game: F%d", i];
    }
    // crash recovery (crashrec.cpp): automatic states every few minutes + recorded input
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem* cr = [m addItemWithTitle:[NSString stringWithFormat:@"Crash Recovery (automatic state every %d min)",
                                                                    (crashrec::interval_seconds() + 30) / 60]
                                  action:@selector(toggleCrashRecovery:) keyEquivalent:@""];
    cr.target = self;
    cr.state = crashrec::enabled() ? NSControlStateValueOn : NSControlStateValueOff;
    cr.toolTip = @"Saves the game into automatic states in the background and records the controller input since the "
                 @"latest one. After a crash, the crash log in captures/ says how to load it and replay the input. "
                 @"Saving freezes the game for a moment.";
    for (int i = 1; i <= crashrec::kAutoSlots; i++) {
        crashrec::AutoInfo a = crashrec::auto_info(i);
        NSString* d = a.used ? [NSString stringWithFormat:@"%s%s%s", a.when.c_str(), a.area.empty() ? "" : " · ", a.area.c_str()] : @"empty";
        NSMenuItem* it = [m addItemWithTitle:[NSString stringWithFormat:@"Load automatic state %d  (%@)", i, d] action:@selector(loadAuto:) keyEquivalent:@""];
        it.target = self;
        it.tag = i;
        it.enabled = a.used;
    }
}
@end
static WWStateMenu* g_state_menu;

@interface WWGraphicsMenu : NSObject <NSMenuItemValidation>
@end

@implementation WWGraphicsMenu
- (void)setBloom:(NSMenuItem*)item { render::set_bloom_strength(item.tag / 100.0f); update_title(); }
- (void)setAO:(NSMenuItem*)item { render::set_ao_mode((int)item.tag); update_title(); }
- (void)toggleAniso:(NSMenuItem*)item { render::set_aniso(!render::aniso()); update_title(); }
- (void)capture:(NSMenuItem*)item { render::request_capture(); }
- (void)screenshot:(NSMenuItem*)item { screenshot::request(); }
- (void)openScreenshots:(NSMenuItem*)item { hostui::open_folder(screenshot::dir()); }
- (void)toggleScreenshotGamePad:(NSMenuItem*)item { screenshot::set_gamepad_too(!screenshot::gamepad_too()); }
- (void)openSettings:(NSMenuItem*)item { overlay::set_open(!overlay::is_open()); }  // Cmd+, toggles
- (void)setRenderer:(NSMenuItem*)item { choose_renderer((render::Api)item.tag); }
- (void)recordSound:(NSMenuItem*)item { gfx::menu_hotkey(kVK_ANSI_9); }
- (void)setController:(NSMenuItem*)item {
    input::set_pro_controller(item.tag == 1);
    gfx::set_host_setting("proController", item.tag == 1 ? "1" : "0");  // as the settings overlay saves it
}
- (void)setFaceLayout:(NSMenuItem*)item {
    // issue #78: which host face buttons drive the Wii U's A/B/X/Y (the same as the Controls window)
    input_map::Mapping m = input_map::current();
    if (item.tag == 2) input_map::set_face_auto(m, true);
    else input_map::apply_face_layout(m, item.tag == 1 ? input_map::FaceLayout::kLabels : input_map::FaceLayout::kPosition);
    input_map::set_current(m);
}
- (void)setFps:(NSMenuItem*)item { gx2::set_fps_target((int)item.tag); update_title(); }
- (void)toggleInterp:(NSMenuItem*)item { interp::set_mode(interp::mode() == item.tag ? 0 : (int)item.tag); update_title(); }
- (void)toggleInterpFps:(NSMenuItem*)item { interp::toggle_fps((int)item.tag); update_title(); }
- (void)toggleDrcWindow:(NSMenuItem*)item { gfx::show_drc_window(!gfx::drc_window_shown()); }
- (void)toggleFxaa:(NSMenuItem*)item { render::set_fxaa(!render::fxaa()); update_title(); }
- (void)toggleHires:(NSMenuItem*)item { render::set_ao_hires(!render::ao_hires()); update_title(); }
- (void)setRes:(NSMenuItem*)item { set_res(kResScales[item.tag]); update_title(); }
- (void)setAspect:(NSMenuItem*)item {
    aspect::set_mode((int)item.tag);
    gfx::set_host_setting("aspectMode", std::to_string((int)item.tag));  // as the settings overlay saves it
    update_title();
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(setRes:))
        item.state = fabsf(kResScales[item.tag] - current_res_scale()) < 0.01f ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(setAspect:)) item.state = item.tag == aspect::mode() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(setAO:)) {
        item.state = item.tag == render::ao_mode() ? NSControlStateValueOn : NSControlStateValueOff;
        return render::feature_available(render::kFeatureAO);
    }
    if (item.action == @selector(setRenderer:)) {
        render::Api a = (render::Api)item.tag;
        item.state = a == render::preferred() ? NSControlStateValueOn : NSControlStateValueOff;
        NSString* name = a == render::Api::Vulkan ? @"    Vulkan (MoltenVK)" : @"    Metal";
        item.title = a == render::active() ? [name stringByAppendingString:@"  \u2014 in use"] : name;
        if (!render::compiled(a)) return NO;
        return YES;
    }
    if (item.action == @selector(setController:))
        item.state = (item.tag == 1) == input::pro_controller() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(setFaceLayout:)) {
        input_map::FaceLayout fl = input_map::face_layout(input_map::current());
        bool on = fl == (item.tag == 1 ? input_map::FaceLayout::kLabels : input_map::FaceLayout::kPosition);
        item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    }
    if (item.action == @selector(setFps:)) item.state = gx2::fps_target() == item.tag ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleInterp:)) item.state = interp::mode() == item.tag ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleInterpFps:))
        item.state = interp::mode() == 1 && interp::fps() == item.tag ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleDrcWindow:)) {
        item.state = gfx::drc_window_shown() ? NSControlStateValueOn : NSControlStateValueOff;
        return gfx::drc_window_available();
    }
    if (item.action == @selector(setBloom:))
        item.state = fabsf(render::bloom_strength() * 100.0f - item.tag) < 0.5f ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleFxaa:)) {
        item.state = render::fxaa() ? NSControlStateValueOn : NSControlStateValueOff;
        return render::feature_available(render::kFeatureFXAA);
    }
    if (item.action == @selector(toggleHires:)) {
        item.state = render::ao_hires() ? NSControlStateValueOn : NSControlStateValueOff;
        return render::feature_available(render::kFeatureAOHires);
    }
    if (item.action == @selector(toggleAniso:)) {
        item.state = render::aniso() ? NSControlStateValueOn : NSControlStateValueOff;
        return render::feature_available(render::kFeatureAniso);
    }
    if (item.action == @selector(capture:)) return render::feature_available(render::kFeatureCapture);
    if (item.action == @selector(screenshot:)) {
        // the Screenshot binding's key (Controls), shown in the title: it can be rebound
        int k = input_map::current().keys[input_map::kScreenshot][0];
        if (k == input_map::kNoKey) k = input_map::current().keys[input_map::kScreenshot][1];
        item.title = k == input_map::kNoKey ? @"Take Screenshot"
                                            : [NSString stringWithFormat:@"Take Screenshot (%s)", input_map::key_label(k).c_str()];
    }
    if (item.action == @selector(toggleScreenshotGamePad:))
        item.state = screenshot::gamepad_too() ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end

static WWGraphicsMenu* g_target;

// Menu items backed by two blocks: `get` gives the check mark, `set` is called with !get() on a click.
// Used by the Gameplay menu: one line per option (see install_menu).
@interface WWBlockItem : NSObject <NSMenuItemValidation>
@property(copy) BOOL (^get)(void);
@property(copy) void (^set)(BOOL);
@end
@implementation WWBlockItem
- (void)act:(NSMenuItem*)item { self.set(!self.get()); update_title(); }
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    item.state = self.get() ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end
static NSMutableArray<WWBlockItem*>* g_block_items;  // menu items hold their targets weakly

// add a checkable item: toggle(menu, @"Title", ^{ return state(); }, ^(BOOL on) { set_state(on); })
static NSMenuItem* toggle(NSMenu* m, NSString* title, BOOL (^get)(void), void (^set)(BOOL), NSString* tip = @"") {
    WWBlockItem* t = [WWBlockItem new];
    t.get = get;
    t.set = set;
    if (!g_block_items) g_block_items = [NSMutableArray new];
    [g_block_items addObject:t];
    NSMenuItem* it = [m addItemWithTitle:title action:@selector(act:) keyEquivalent:@""];
    it.target = t;
    if (tip.length) it.toolTip = tip;
    return it;
}

static NSMenuItem* add(NSMenu* m, NSString* title, SEL action, NSString* key, NSInteger tag = 0) {
    NSMenuItem* it = [m addItemWithTitle:title action:action keyEquivalent:@""];
    it.target = g_target;
    it.tag = tag;
    // shown for reference; the game window's key monitor handles the actual key (no Cmd needed)
    if (key.length) it.toolTip = [NSString stringWithFormat:@"Shortcut in game: %@", key];
    return it;
}

namespace mods { bool climb_enabled(); void set_climb_enabled(bool on); }  // mods/climb.cpp

namespace gfx {

void install_menu(NSWindow* tv) {
    g_tv = tv;
    g_target = [WWGraphicsMenu new];
    NSMenu* bar = [NSMenu new];

    NSMenuItem* appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* app = [NSMenu new];
    // the settings overlay (overlay/overlay.h), at the place and with the shortcut macOS apps use;
    // F1 also works (Fn+F1 unless the top row sends standard function keys)
    NSMenuItem* settings = [app addItemWithTitle:@"Settings\u2026" action:@selector(openSettings:) keyEquivalent:@","];
    settings.target = g_target;
    settings.toolTip = @"In-game settings overlay over the picture (F1, or \u2318, in the game window)";
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:@"Quit Breath of the Wild" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = app;

    NSMenuItem* gfxItem = [bar addItemWithTitle:@"Graphics" action:nil keyEquivalent:@""];
    NSMenu* g = [[NSMenu alloc] initWithTitle:@"Graphics"];
    [g addItemWithTitle:@"Internal resolution (R cycles)" action:nil keyEquivalent:@""].enabled = NO;
    add(g, @"    1x (1280x720, as on Wii U)", @selector(setRes:), @"R", 0);
    add(g, @"    1.5x (1920x1080)", @selector(setRes:), @"R", 1);
    add(g, @"    2x (2560x1440)", @selector(setRes:), @"R", 2);
    add(g, @"    3x (3840x2160)", @selector(setRes:), @"R", 3);
    [g addItem:[NSMenuItem separatorItem]];
    add(g, @"16x anisotropic filtering (N)", @selector(toggleAniso:), @"N");
    add(g, @"Edge smoothing, FXAA (8)", @selector(toggleFxaa:), @"8");
    add(g, @"Frame rate: 30 fps (original)", @selector(setFps:), @"", 30);
    add(g, @"Frame rate: 60 fps (6)", @selector(setFps:), @"6", 60);
    add(g, @"Frame rate: uncapped (7)", @selector(setFps:), @"7", 0);
    [g addItem:[NSMenuItem separatorItem]];
    add(g, @"Take Screenshot (F10)", @selector(screenshot:), @"").toolTip =
        @"Saves the TV picture as a PNG (internal resolution, without the settings overlay) in the screenshots folder. "
        @"The key can be changed in Input > Controls (Screenshot).";
    add(g, @"    Open Screenshots Folder", @selector(openScreenshots:), @"");
    add(g, @"Capture frame for debugging (P)", @selector(capture:), @"P").toolTip =
        render::active() == render::Api::Vulkan ? @"Shortcut in game: P. Vulkan: the TV, GamePad and window pictures (the draw log is Metal only)"
                                                 : @"Shortcut in game: P";
    add(g, @"Record 3 s of sound activity (9)", @selector(recordSound:), @"9");
    gfxItem.submenu = g;

    NSMenuItem* inItem = [bar addItemWithTitle:@"Input" action:nil keyEquivalent:@""];
    NSMenu* in = [[NSMenu alloc] initWithTitle:@"Input"];
    [in addItemWithTitle:@"Keyboard and controllers act as" action:nil keyEquivalent:@""].enabled = NO;
    add(in, @"    Wii U GamePad", @selector(setController:), @"", 0);
    add(in, @"    Wii U Pro Controller", @selector(setController:), @"", 1);
    [in addItemWithTitle:@"Face buttons drive the Wii U's A/B/X/Y" action:nil keyEquivalent:@""].enabled = NO;
    add(in, @"    By position (Nintendo)", @selector(setFaceLayout:), @"", 0).toolTip =
        @"The bottom face button is B and the right one is A (Nintendo layout)";
    add(in, @"    By label (Xbox)", @selector(setFaceLayout:), @"", 1).toolTip =
        @"The button named A is A: on an Xbox pad A accepts/acts and B goes back (issue #78). "
        @"Rewrites the A/B/X/Y controller bindings only";
    add(in, @"    Automatic", @selector(setFaceLayout:), @"", 2).toolTip =
        @"Follows the labels printed on the pad that was plugged in first: by label on an Xbox pad, "
        @"by position on a Nintendo one";
    [in addItem:[NSMenuItem separatorItem]];
    [in addItem:gfx::controls_menu_item()];
    inItem.submenu = in;
    install_display_menu(bar);  // Display: full screen, scaling, GamePad screen mode (display.mm)

    // Save States: 5 slots (savestate.cpp); Shift+F1..F5 save, F1..F5 load
    NSMenuItem* ssItem = [bar addItemWithTitle:@"Save States" action:nil keyEquivalent:@""];
    NSMenu* sm = [[NSMenu alloc] initWithTitle:@"Save States"];
    sm.autoenablesItems = NO;
    g_state_menu = [WWStateMenu new];
    sm.delegate = g_state_menu;
    ssItem.submenu = sm;

    // Window: the standard items. Close Window (Cmd+W) on the TV window quits, asking first while a
    // game is in progress (quit_prompt.mm); on the GamePad window it only hides it
    NSMenuItem* winItem = [bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""];
    NSMenu* wm = [[NSMenu alloc] initWithTitle:@"Window"];
    [wm addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [wm addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [wm addItem:[NSMenuItem separatorItem]];
    [wm addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
    winItem.submenu = wm;
    NSApp.windowsMenu = wm;  // macOS lists the open windows below

    NSApp.mainMenu = bar;
    install_overlay_input();  // settings overlay (F1): mouse in the TV window (overlay_appkit.mm)
    update_title();
    // apply the saved options once the renderer is settled: the menu is installed with the windows,
    // before a Vulkan start can still fail and fall back to Metal; the main queue runs after that
    dispatch_async(dispatch_get_main_queue(), ^{
        load_prefs();
        update_title();
    });
    // live frame rate: presented frames over the last half second
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) {
        static uint64_t last = gx2::flips_presented();
        static CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        uint64_t n = gx2::flips_presented();
        CFAbsoluteTime t = CFAbsoluteTimeGetCurrent();
        if (t > t0) g_fps = (double)(n - last) / (t - t0);
        last = n;
        t0 = t;
        update_title();
    }];
}

// settings overlay (overlay_appkit.mm): the same internal resolution value and title / saved options
float menu_res_scale() { return current_res_scale(); }
void menu_set_res_scale(float f) { set_res(f); }
void menu_options_changed() { update_title(); }

// single-key shortcuts from the game window; true if the key was used
bool menu_hotkey(uint16_t code) {
    switch (code) {
    case kVK_ANSI_N: if (render::feature_available(render::kFeatureAniso)) render::set_aniso(!render::aniso()); break;
    case kVK_ANSI_6: gx2::set_fps_target(gx2::fps_target() == 60 ? 30 : 60); break;
    case kVK_ANSI_7: gx2::set_fps_target(gx2::fps_target() == 0 ? 30 : 0); break;
    case kVK_ANSI_8: if (render::feature_available(render::kFeatureFXAA)) render::set_fxaa(!render::fxaa()); break;
    case kVK_ANSI_R: cycle_res(); break;
    case kVK_ANSI_P: case kVK_F12: render::request_capture(); return true;
    case kVK_F1: case kVK_F2: case kVK_F3: case kVK_F4: case kVK_F5: {
        int slot = code == kVK_F1 ? 1 : code == kVK_F2 ? 2 : code == kVK_F3 ? 3 : code == kVK_F4 ? 4 : 5;
        if ([NSEvent modifierFlags] & NSEventModifierFlagShift) ss::request_save(slot);
        else if (slot != 1) ss::request_load(slot);  // F1: the settings overlay (overlay_appkit.mm)
        return true;
    }
    case kVK_ANSI_9: {
        // 3 s of sound activity (voice starts), without the frame capture's stall
        char path[96];
        time_t t = time(nullptr);
        mkdir("captures", 0755);
        strftime(path, sizeof path, "captures/%Y%m%d-%H%M%S-sound.log", localtime(&t));
        ax::start_sound_trace(path, 3.0);
        return true;
    }
    default: return false;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ update_title(); });
    return true;
}

}  // namespace gfx
