// Controller rumble on the AppKit host: rumble.h's motor level, played on every connected controller
// that has haptics (GameController.framework's GCDeviceHaptics, Core Haptics), or through a DualShock
// 4's own effects report. Adapted from nxnative's dm/host/haptics_apple.m and the findings in its
// docs/reference/services.md ("Controller haptic engines without advanced players", "A haptic engine
// that stops under a call raises", "DualShock 4 rumble through its effects report").
//
// Threads: every engine, player and HID device belongs to one serial queue. A timer on it reads the
// level 60 times a second while some motor runs (rumble::host_active) and sleeps otherwise; the game's
// requests wake it (rumble::set_host_wake). It runs apart from the main thread, so a stalled event
// loop cannot leave a motor running: the Pro Controller motor counts as off once the game stops
// refreshing it and a GamePad pattern ends by itself (rumble.h).
//
// The motors are still while the settings overlay is open (the game sees no input then), while the
// app is not active, and for good from the moment the process ends (quit, exit, restart, crash).
//
// Routes, per controller:
//   - one Core Haptics engine on GCHapticsLocalityHandles (Default when the controller does not
//     list it, or when that engine cannot be made or started). The game drives one motor here, so
//     both handles always play the same level: this is the route nxnative merges a split one into
//     on an Xbox controller, whose left and right handle engines override each other without
//     advanced players.
//   - an advanced (looping) player first; when the controller's haptics server offers none
//     (gamecontrollerd on Xbox and DualShock 4-mode pads: NSCocoaErrorDomain 4097, and the engine is
//     unusable afterwards), a new engine with plain players that play pulses of kPulse seconds that
//     end on their own. A plain player stopped in the middle of its event left an Xbox controller's
//     motors running after the process was gone, so a stop only silences the playing pulses.
//   - every Core Haptics call is inside an @try: a call on an engine that stopped on its own raises
//     an NSException instead of returning an error. A raise drops that engine; the next effect
//     makes a new one after a backoff.
//   - a controller GameController calls a DualShock 4 (Sony 054C / 05C4, 09CC, 0BA0, which includes
//     pads that present themselves as one) is driven through its HID effects report when such a HID
//     device is free, as SDL does: Core Haptics failed on every locality on such pads.
// WWHD_LOG_RUMBLE=1 logs the routes and every level sent.
#import <AppKit/AppKit.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDDevice.h>
#import <IOKit/hid/IOHIDKeys.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>

#include "../input.h"
#include "../overlay/overlay.h"
#include "../rumble.h"
#include "../runtime.h"

namespace {
constexpr double kTick = 1.0 / 60;      // the level is read this often while a motor runs
constexpr double kPulse = 0.5;          // plain players: one pulse
constexpr double kPulseLead = 0.15;     // the next pulse is made this long before the last one ends
constexpr double kIdleRelease = 1.0;    // an engine silent this long is stopped (warm for nearby hits)

dispatch_queue_t g_queue;
dispatch_source_t g_timer;
std::atomic<bool> g_sleeping{true};     // the timer is suspended (it starts so)
std::atomic<int> g_outputs{0};          // controllers with a route (has_rumble)
std::atomic<bool> g_active{false};      // the app is active (focus)
std::atomic<bool> g_quit{false};        // the process ends: nothing plays again
unsigned g_errors;                       // queue only

double now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
bool limit_log() { return ++g_errors <= 8 || g_errors % 64 == 0; }

// ---- DualShock 4 effects report: 0x11 over Bluetooth (78 bytes, CRC32 over 0xA2 and the first 74
// bytes; flags 0xC4 and rumble-only 0x01, so the light bar is left alone), 0x05 over USB (32 bytes)
struct Ds4 {
    IOHIDDeviceRef dev = nullptr;
    uint64_t id = 0;  // its registry entry: two handles on one device are not CFEqual
    bool bluetooth = false;
    uint8_t sent = 0;
};
uint32_t ds4_crc(const uint8_t* p, size_t n) {
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}
bool ds4_send(Ds4& o, uint8_t level) {
    uint8_t r[78] = {};
    size_t n;
    // the game has one motor here: the strong (left) and weak (right) motors both run at its level
    if (o.bluetooth) {
        r[0] = 0x11; r[1] = 0xC4; r[3] = 0x01; r[6] = level; r[7] = level; n = 78;
        uint8_t c[75] = {0xA2};
        memcpy(c + 1, r, 74);
        uint32_t crc = ds4_crc(c, sizeof c);
        r[74] = crc; r[75] = crc >> 8; r[76] = crc >> 16; r[77] = crc >> 24;
    } else {
        r[0] = 0x05; r[1] = 0x01; r[4] = level; r[5] = level; n = 32;
    }
    IOReturn e = IOHIDDeviceSetReport(o.dev, kIOHIDReportTypeOutput, r[0], r, n);
    if (e != kIOReturnSuccess) {
        if (limit_log()) LOG("[rumble] host: DualShock 4 effects report: IOReturn 0x%x", e);
        return false;
    }
    o.sent = level;
    return true;
}
}  // namespace

@interface BotwRumble : NSObject
@property(nonatomic) GCController* controller;
@property(nonatomic) GCDeviceHaptics* haptics;
@property(nonatomic, copy) NSString* name;
@property(nonatomic) NSArray<GCHapticsLocality>* localities;  // tried in order: an engine that cannot be made or started moves on
@property(nonatomic) NSUInteger localityIndex;
@property(nonatomic) CHHapticEngine* engine;
@property(nonatomic) id<CHHapticPatternPlayer> pattern;
@property(nonatomic) id<CHHapticPatternPlayer> previous;  // plain players: the pulse before the scheduled one, still playing
@property(nonatomic) BOOL simple;                       // no advanced players: plain players play the effect in pulses
@property(nonatomic) NSTimeInterval pulseEnd;            // engine time at which the last scheduled pulse ends
@property(nonatomic) float level;                        // the level last applied (the next pulse starts at it)
@property(nonatomic) uint64_t generation;
@property(nonatomic) uint64_t silenceGeneration;
@property(nonatomic) double retryAfter;
@property(nonatomic) unsigned failures;
@end

@implementation BotwRumble {
@public
    Ds4 _ds4;
}

- (NSString*)target {
    return [NSString stringWithFormat:@"%@ (%@)", self.name, self.localities[self.localityIndex]];
}

// Core Haptics raises when a call reaches an engine that is no longer running, and offers no running
// state to ask first (the engine stops on its own at any time; its stopped handler is only queued).
- (void)guarded:(const char*)stage work:(void (^)(void))work {
    @try {
        work();
    } @catch (NSException* x) {
        [self stoppedEngine:x stage:stage];
    }
}
// like a failed start, except that neither the engine nor its players are called again (silencing a
// player raises once more); nothing plays from an engine that is not running
- (void)stoppedEngine:(NSException*)x stage:(const char*)stage {
    if (limit_log())
        LOG("[rumble] host: %s %s on a stopped engine (%s: %s)", self.target.UTF8String, stage, x.name.UTF8String ?: "exception",
            x.reason.UTF8String ?: "no reason");
    self.generation++;
    self.silenceGeneration++;
    self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {};
    self.engine.resetHandler = ^{};
    self.engine = nil; self.pattern = nil; self.previous = nil; self.level = 0;
    [self backoff];
}
- (void)backoff {
    self.failures = MIN(self.failures + 1, 4u);
    self.retryAfter = now() + (1u << (self.failures - 1));
}

- (void)stop {
    if (_ds4.dev) {
        if (_ds4.sent) ds4_send(_ds4, 0);
        self.level = 0;
        return;
    }
    [self guarded:"stop" work:^{ [self halt]; }];
}
// stop's work, for callers already inside guarded:work:
- (void)halt {
    self.generation++;
    self.silenceGeneration++;
    self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {};
    self.engine.resetHandler = ^{};
    self.level = 0;
    if (self.simple) {
        // never cut a pulse: silence the playing ones and stop the engine once the last has ended
        NSArray* zero = @[ [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:0 relativeTime:0] ];
        [self.previous sendParameters:zero atTime:CHHapticTimeImmediate error:nil];
        [self.pattern sendParameters:zero atTime:CHHapticTimeImmediate error:nil];
        self.pattern = nil; self.previous = nil;
        NSTimeInterval left = self.engine ? self.pulseEnd - self.engine.currentTime : 0;
        if (left > 0) {
            __weak BotwRumble* weak = self;
            CHHapticEngine* engine = self.engine;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((left + 0.05) * NSEC_PER_SEC)), g_queue, ^{
                BotwRumble* c = weak;
                if (c && c.engine == engine && c.pattern) return;  // a new effect took the engine meanwhile
                @try {
                    [engine stopWithCompletionHandler:nil];
                } @catch (NSException* x) {
                    if (c && c.engine == engine) [c stoppedEngine:x stage:"stop engine"];
                }
            });
            return;
        }
    } else {
        [self.pattern stopAtTime:CHHapticTimeImmediate error:nil];
        self.pattern = nil;
    }
    [self.engine stopWithCompletionHandler:nil];  // the engine stays: the next effect restarts it
}

- (void)report:(NSError*)e stage:(const char*)stage {
    if (!limit_log()) return;
    LOG("[rumble] host: %s %s: %s (%s %ld)", self.target.UTF8String, stage, e.localizedDescription.UTF8String ?: "engine unavailable",
        e.domain.UTF8String ?: "no error", (long)e.code);
}
// a player or parameter failure: retried on the same locality after the backoff, with a new engine
// (a failed helper connection is not reused)
- (void)failed:(NSError*)e stage:(const char*)stage {
    [self report:e stage:stage];
    [self halt];
    self.engine = nil;
    [self backoff];
}
// an engine that cannot be made or started: the next locality, if there is one
- (void)engineFailed:(NSError*)e stage:(const char*)stage {
    [self failed:e stage:stage];
    if (self.localityIndex + 1 < self.localities.count) {
        self.localityIndex++;
        LOG("[rumble] host: %s: retrying on locality %s", self.name.UTF8String, self.localities[self.localityIndex].UTF8String);
    }
}
- (BOOL)startEngine {
    NSError* error = nil;
    if (!self.engine) {
        self.engine = [self.haptics createEngineWithLocality:self.localities[self.localityIndex]];
        if (!self.engine) {
            [self engineFailed:nil stage:"create engine"];
            return NO;
        }
        self.engine.playsHapticsOnly = YES;
        self.engine.autoShutdownEnabled = NO;
    }
    __weak BotwRumble* weak = self;
    const uint64_t generation = ++self.generation;
    // stopped by the system (the helper's exit, sleep) or reset: drop the effect, start again on the
    // next level change after a short wait
    void (^reset)(void) = ^{
        dispatch_async(g_queue, ^{
            BotwRumble* c = weak;
            if (!c || c.generation != generation) return;
            if (rumble::log_enabled()) LOG("[rumble] host: %s: engine stopped or reset by the system", c.target.UTF8String);
            [c stop];
            c.retryAfter = now() + 0.5;
        });
    };
    self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason) { reset(); };
    self.engine.resetHandler = reset;
    // startup can itself fail or reset the service: the handlers go in before it is contacted
    if (![self.engine startAndReturnError:&error]) {
        [self engineFailed:error stage:"start engine"];
        return NO;
    }
    return YES;
}

static CHHapticPattern* simple_pattern(float level, NSError** error) {
    CHHapticEvent* event = [[CHHapticEvent alloc]
        initWithEventType:CHHapticEventTypeHapticContinuous
               parameters:@[
                   [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1],
                   [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5]
               ]
             relativeTime:0
                 duration:kPulse];
    // the level is the pattern's own initial control: a plain player has no mute to hide its first
    // instant at full intensity
    return [[CHHapticPattern alloc]
        initWithEvents:@[ event ]
            parameters:@[ [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:level relativeTime:0] ]
                 error:error];
}
// the next pulse at the current level, from where the last one ends, and the renewal of the one
// after it while the effect lasts
- (BOOL)pulse:(NSError**)error {
    CHHapticPattern* p = simple_pattern(self.level, error);
    id<CHHapticPatternPlayer> next = p ? [self.engine createPlayerWithPattern:p error:error] : nil;
    NSTimeInterval at = MAX(self.engine.currentTime, self.pulseEnd);
    if (!next || ![next startAtTime:at error:error]) return NO;
    self.previous = self.pattern;
    self.pattern = next;
    self.pulseEnd = at + kPulse;
    __weak BotwRumble* weak = self;
    const uint64_t generation = self.generation;
    NSTimeInterval wait = MAX(0, self.pulseEnd - kPulseLead - self.engine.currentTime);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * NSEC_PER_SEC)), g_queue, ^{
        BotwRumble* c = weak;
        if (!c || c.generation != generation || c.pattern != next || c.level == 0) return;  // stopped, replaced or silent
        [c guarded:"pulse" work:^{
            NSError* e = nil;
            if (![c pulse:&e]) [c failed:e stage:"pulse"];
        }];
    });
    return YES;
}

- (void)apply:(float)level {
    if (_ds4.dev) {
        const uint8_t b = (uint8_t)lrintf(std::clamp(level, 0.f, 1.f) * 255);
        self.level = level;
        if (b != _ds4.sent) ds4_send(_ds4, b);
        return;
    }
    [self guarded:"apply" work:^{ [self applyLevel:level]; }];
}
- (void)applyLevel:(float)level {
    const uint64_t silence = ++self.silenceGeneration;
    NSError* error = nil;
    self.level = level;
    if (!self.pattern) {
        if (level == 0) return;
        if (now() < self.retryAfter) {  // backing off after a failure: the tick asks again
            self.level = 0;
            return;
        }
        if (![self startEngine]) return;
        if (!self.simple) {
            CHHapticEvent* event = [[CHHapticEvent alloc]
                initWithEventType:CHHapticEventTypeHapticContinuous
                       parameters:@[
                           [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1],
                           [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5]
                       ]
                     relativeTime:0
                         duration:30];
            CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:&error];
            id<CHHapticAdvancedPatternPlayer> advanced = pattern ? [self.engine createAdvancedPlayerWithPattern:pattern error:&error] : nil;
            if (advanced) {
                advanced.loopEnabled = YES;
                advanced.isMuted = YES;
                self.pattern = advanced;
            } else if (pattern) {
                // the controller's haptics server (gamecontrollerd) offers no advanced players and the
                // request fails over XPC; the refusal leaves that engine unusable (a plain player or a
                // restart on it fails with -4810), so plain players play from a new engine
                LOG("[rumble] host: %s has no advanced players (%s %ld): plain players", self.target.UTF8String,
                    error.domain.UTF8String ?: "no error", (long)error.code);
                error = nil;
                [self halt];
                self.engine = nil;
                self.simple = YES;
                self.level = level;
                if (![self startEngine]) return;
            }
        }
        if (self.simple) {
            self.pulseEnd = 0;
            if (![self pulse:&error]) {
                [self failed:error stage:"start pulse"];
                return;
            }
        }
        if (!self.pattern) {
            [self failed:error stage:"create player"];
            return;
        }
        if (!self.simple && ![self.pattern startAtTime:CHHapticTimeImmediate error:&error]) {
            [self failed:error stage:"start player"];
            return;
        }
    } else if (self.simple && level > 0 && self.pulseEnd <= self.engine.currentTime) {
        if (![self pulse:&error]) {  // the pulses ran out while the effect was silent
            [self failed:error stage:"start pulse"];
            return;
        }
    }
    // the controls live outside the loop: the loop boundary never replays an initial intensity
    NSArray* parameters = @[ [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:level relativeTime:0] ];
    [self.previous sendParameters:parameters atTime:CHHapticTimeImmediate error:nil];  // still playing until the scheduled pulse starts
    if (![self.pattern sendParameters:parameters atTime:CHHapticTimeImmediate error:&error]) {
        [self failed:error stage:"send parameters"];
        return;
    }
    self.failures = 0;
    if (!self.simple) ((id<CHHapticAdvancedPatternPlayer>)self.pattern).isMuted = NO;
    if (level == 0) {
        // a warm player for nearby hits, then the hardware is released; never times out an effect
        __weak BotwRumble* weak = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kIdleRelease * NSEC_PER_SEC)), g_queue, ^{
            BotwRumble* c = weak;
            if (c && c.silenceGeneration == silence) [c stop];
        });
    }
}
@end

namespace {
NSMutableArray<BotwRumble*>* g_channels;  // queue only

// the first DualShock 4 HID device no other channel drives; the registry is asked at each connect
bool ds4_take(BotwRumble* ch) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey);
    if (!match) return false;
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), (__bridge CFNumberRef) @0x054C);
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return false;  // takes match
    bool found = false;
    for (io_service_t s; !found && (s = IOIteratorNext(it)); IOObjectRelease(s)) {
        uint64_t id = 0;
        if (IORegistryEntryGetRegistryEntryID(s, &id) != KERN_SUCCESS) continue;
        bool taken = false;  // GameController does not name a controller's HID device: two may get each other's
        for (BotwRumble* o in g_channels) taken |= o->_ds4.dev && o->_ds4.id == id;
        IOHIDDeviceRef dev = taken ? nullptr : IOHIDDeviceCreate(kCFAllocatorDefault, s);
        if (!dev) continue;
        const int pid = [(__bridge NSNumber*)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductIDKey)) intValue];
        // DualShock 4 v1, v2, the USB wireless adapter
        if ((pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0) && IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
            ch->_ds4.dev = dev;
            ch->_ds4.id = id;
            ch->_ds4.bluetooth = [(__bridge NSString*)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey)) isEqualToString:@"Bluetooth"];
            found = true;
            LOG("[rumble] host: %s: rumble through the DualShock 4 effects report (%s, product 0x%04x)", ch.name.UTF8String,
                ch->_ds4.bluetooth ? "Bluetooth" : "USB", pid);
        } else {
            CFRelease(dev);
        }
    }
    IOObjectRelease(it);
    return found;
}

void ds4_release(BotwRumble* ch) {
    Ds4& o = ch->_ds4;
    if (!o.dev) return;
    if (o.sent) ds4_send(o, 0);
    IOHIDDeviceClose(o.dev, kIOHIDOptionsTypeNone);
    CFRelease(o.dev);
    o = Ds4{};
}

void wake() {
    if (g_outputs.load(std::memory_order_relaxed) == 0 || g_quit.load(std::memory_order_relaxed)) return;
    if (g_sleeping.exchange(false)) dispatch_async(g_queue, ^{ dispatch_resume(g_timer); });
}

void tick() {
    float level = 0;
    if (!g_quit && g_active && !overlay::is_open()) level = std::clamp(rumble::host_level((uint64_t)(kTick * 1e6)), 0.f, 1.f);
    bool any = false;
    for (BotwRumble* c in g_channels) {
        if (fabsf(c.level - level) > 0.004f) {
            if (rumble::log_enabled()) LOG("[rumble] host: %s motor %.2f", c.name.UTF8String, level);
            [c apply:level];
        }
        any |= c.level > 0;
    }
    if (any || g_quit) return;
    // nothing plays: sleep until the game asks again. The flag goes up first, so a request that
    // comes in between either finds it up (and resumes the timer after this) or is seen here.
    g_sleeping = true;
    if (rumble::host_active() && g_sleeping.exchange(false)) return;  // still running: keep ticking
    dispatch_suspend(g_timer);
}

void add_controller(GCController* c) {
    for (BotwRumble* o in g_channels)
        if (o.controller == c) return;
    BotwRumble* ch = [BotwRumble new];
    ch.controller = c;
    ch.name = [NSString stringWithFormat:@"%@", c.vendorName ?: c.productCategory ?: @"controller"];
    if ([c.productCategory isEqualToString:GCProductCategoryDualShock4] && ds4_take(ch)) {
        ch.localities = @[ @"DualShock 4 report" ];
    } else {
        GCDeviceHaptics* h = c.haptics;
        NSSet<GCHapticsLocality>* have = h.supportedLocalities;
        if (!h || !have.count) {
            LOG("[rumble] host: %s has no haptics: no rumble", ch.name.UTF8String);
            return;
        }
        ch.haptics = h;
        ch.localities = [have containsObject:GCHapticsLocalityHandles] ? @[ GCHapticsLocalityHandles, GCHapticsLocalityDefault ]
                                                                       : @[ GCHapticsLocalityDefault ];
        LOG("[rumble] host: %s (%s): Core Haptics on %s (localities %s)", ch.name.UTF8String, c.productCategory.UTF8String ?: "?",
            ch.localities[0].UTF8String,
            [[have.allObjects sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@", "].UTF8String);
    }
    [g_channels addObject:ch];
    g_outputs = (int)g_channels.count;
    if (rumble::host_active()) wake();  // an effect that already runs
}

void remove_controller(GCController* c) {
    for (BotwRumble* o in [g_channels copy]) {
        if (o.controller != c) continue;
        [o stop];
        ds4_release(o);
        [g_channels removeObject:o];
    }
    g_outputs = (int)g_channels.count;
}
}  // namespace

namespace input {

bool has_rumble() { return g_outputs.load(std::memory_order_relaxed) > 0; }

// Any thread, also the crash handler: silences every motor and waits for it (up to 0.3 s), then
// nothing plays again. Plain players are silenced, not cut (see the top of this file).
void stop_rumble_now() {
    if (g_quit.exchange(true) || !g_queue) return;
    if (dispatch_get_specific(&g_queue)) return;  // on the queue itself (a crash there): it cannot run
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(g_queue, ^{
        for (BotwRumble* c in g_channels) [c stop];
        dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC));
}

// main thread, from init()
void init_rumble() {
    g_queue = dispatch_queue_create("botw.rumble", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(g_queue, &g_queue, &g_queue, nullptr);
    g_channels = [NSMutableArray array];
    g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_queue);  // created suspended (g_sleeping)
    dispatch_source_set_timer(g_timer, DISPATCH_TIME_NOW, (uint64_t)(kTick * NSEC_PER_SEC), 2 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_timer, ^{ @autoreleasepool { tick(); } });
    g_active = NSApp.active;
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        g_active = false;  // the running tick silences the motors
    }];
    [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        g_active = true;
        dispatch_async(g_queue, ^{ if (rumble::host_active()) wake(); });
    }];
    [nc addObserverForName:NSApplicationWillTerminateNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        stop_rumble_now();
    }];
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification* n) {
        GCController* c = n.object;
        if (c) dispatch_async(g_queue, ^{ @autoreleasepool { add_controller(c); } });
    }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification* n) {
        GCController* c = n.object;
        if (c) dispatch_async(g_queue, ^{ @autoreleasepool { remove_controller(c); } });
    }];
    for (GCController* c in [GCController controllers]) dispatch_async(g_queue, ^{ @autoreleasepool { add_controller(c); } });
    rumble::set_host_wake(wake);
    atexit(stop_rumble_now);  // std::exit: the game's exit()
}

}  // namespace input
