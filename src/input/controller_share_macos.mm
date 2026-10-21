// fable_2 - Share button via GameController (see controller_share_macos.h).
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "controller_share_macos.h"

// AlwaysReceive is deprecated in the macOS 27 SDK (Apple prefers Disabled, our
// default); it stays available as the FABLE2_SHARE_GESTURE=receive option.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace fable2::padkeys::gcshare {
namespace {

LogFn g_log = nullptr;
ShareFn g_share = nullptr;
NSHashTable<GCController*>* g_hooked = nil;  // weak: controllers already set up
// Poll fallback: Share elements -> last pressed state seen by the poll / by the handler.
NSMapTable<GCControllerButtonInput*, NSNumber*>* g_polled = nil;
NSMapTable<GCControllerButtonInput*, NSNumber*>* g_handled = nil;
NSMapTable<GCControllerButtonInput*, NSString*>* g_names = nil;
NSTimer* g_poll = nil;
int g_wanted = 2;

void Log(NSString* text) {
  if (g_log) g_log(text.UTF8String);
}

const char* StateName(GCSystemGestureState s) API_AVAILABLE(macos(11.0)) {
  switch (s) {
    case GCSystemGestureStateEnabled: return "Enabled (system takes the press)";
    case GCSystemGestureStateAlwaysReceive: return "AlwaysReceive (system and app)";
    case GCSystemGestureStateDisabled: return "Disabled (app only)";
  }
  return "?";
}

// FABLE2_SHARE_GESTURE: "disabled" (default), "receive", "leave".
int WantedState() {
  const char* v = std::getenv("FABLE2_SHARE_GESTURE");
  if (v && std::strcmp(v, "receive") == 0) return 1;  // AlwaysReceive
  if (v && std::strcmp(v, "leave") == 0) return -1;
  return 2;  // Disabled
}

GCControllerButtonInput* ShareElement(GCController* c) API_AVAILABLE(macos(11.0)) {
  GCControllerButtonInput* share = nil;
  if (@available(macOS 12.0, *)) {
    share = c.physicalInputProfile.buttons[GCInputButtonShare];
    if (!share && [c.extendedGamepad isKindOfClass:[GCXboxGamepad class]])
      share = ((GCXboxGamepad*)c.extendedGamepad).buttonShare;
  }
  if (!share) share = c.physicalInputProfile.buttons[@"Button Share"];
  return share;
}

void Hook(GCController* c) API_AVAILABLE(macos(11.0)) {
  if (!c || [g_hooked containsObject:c]) return;
  [g_hooked addObject:c];
  NSString* name = c.vendorName ?: @"?";
  GCControllerButtonInput* share = ShareElement(c);
  if (!share) {
    Log([NSString stringWithFormat:
                      @"[pad-hotkeys] GameController: \"%@\" (%@, profile %@) has no Share element",
                      name, c.productCategory, NSStringFromClass([c.physicalInputProfile class])]);
    return;
  }
  const GCSystemGestureState before = share.preferredSystemGestureState;
  const int wanted = WantedState();
  if (wanted >= 0 && share.isBoundToSystemGesture)
    share.preferredSystemGestureState = GCSystemGestureState(wanted);
  Log([NSString stringWithFormat:
                    @"[pad-hotkeys] GameController: \"%@\" (%@, profile %@) Share element: bound to system "
                    @"gesture=%s, gesture state %s -> %s (FABLE2_SHARE_GESTURE=%s)",
                    name, c.productCategory, NSStringFromClass([c.physicalInputProfile class]),
                    share.isBoundToSystemGesture ? "yes" : "no", StateName(before),
                    StateName(share.preferredSystemGestureState),
                    wanted == 1 ? "receive" : wanted < 0 ? "leave" : "disabled"]);
  // Other buttons macOS reserves for itself on this pad (logged once, so a play log shows them).
  NSMutableArray<NSString*>* gestures = [NSMutableArray array];
  for (NSString* key in c.physicalInputProfile.buttons) {
    GCControllerButtonInput* b = c.physicalInputProfile.buttons[key];
    if (b.isBoundToSystemGesture) [gestures addObject:[NSString stringWithFormat:@"%@=%s", key,
                                                          StateName(b.preferredSystemGestureState)]];
  }
  Log([NSString stringWithFormat:@"[pad-hotkeys] GameController: \"%@\" gesture-bound buttons: %@; app active=%d, "
                                 @"background monitoring=%d",
                                 name, gestures.count ? [gestures componentsJoinedByString:@", "] : @"none",
                                 NSApp.isActive, GCController.shouldMonitorBackgroundEvents]);
  [g_names setObject:name forKey:share];
  [g_polled setObject:@(share.isPressed) forKey:share];
  share.pressedChangedHandler = ^(GCControllerButtonInput* element, float, BOOL pressed) {
    [g_handled setObject:@(pressed) forKey:element];
    if (g_share) g_share(name.UTF8String, pressed, NSApp.isActive, "handler");
  };
  // SDL opens the pad a little later and sets every gesture-bound button to
  // Disabled; log what it ends up as (and re-apply AlwaysReceive if asked).
  __weak GCControllerButtonInput* weak_share = share;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    GCControllerButtonInput* s = weak_share;
    if (!s) return;
    if (wanted == 1 && s.isBoundToSystemGesture &&
        s.preferredSystemGestureState != GCSystemGestureStateAlwaysReceive)
      s.preferredSystemGestureState = GCSystemGestureStateAlwaysReceive;
    Log([NSString stringWithFormat:@"[pad-hotkeys] GameController: \"%@\" Share gesture state after "
                                   @"2 s: %s; Share handler %s",
                                   name, StateName(s.preferredSystemGestureState),
                                   s.pressedChangedHandler ? "installed" : "MISSING"]);
  });
}

// 30 Hz on the main run loop: (1) a Share press the handler did not report (seen with
// SDL's polling path too: neither saw the press) still reaches the action, via "poll";
// (2) a gesture state that went back to Enabled (SDL sets Enabled when it closes a pad
// and Disabled when it opens one) is set again, so the system does not take the press.
void Poll() API_AVAILABLE(macos(11.0)) {
  for (GCControllerButtonInput* share in g_names.keyEnumerator.allObjects) {
    NSString* name = [g_names objectForKey:share];
    if (g_wanted >= 0 && share.isBoundToSystemGesture &&
        share.preferredSystemGestureState != GCSystemGestureState(g_wanted)) {
      const GCSystemGestureState was = share.preferredSystemGestureState;
      share.preferredSystemGestureState = GCSystemGestureState(g_wanted);
      Log([NSString stringWithFormat:@"[pad-hotkeys] GameController: \"%@\" Share gesture state was %s; set "
                                     @"back to %s",
                                     name, StateName(was), StateName(share.preferredSystemGestureState)]);
    }
    const BOOL pressed = share.isPressed;
    NSNumber* last = [g_polled objectForKey:share];
    if (last && last.boolValue == pressed) continue;
    [g_polled setObject:@(pressed) forKey:share];
    NSNumber* handled = [g_handled objectForKey:share];
    if (handled && handled.boolValue == pressed) continue;  // the handler already reported this edge
    [g_handled setObject:@(pressed) forKey:share];
    if (g_share) g_share(name.UTF8String, pressed, NSApp.isActive, "poll");
  }
}

}  // namespace

void MonitorBackgroundEvents(bool on) {
  if (@available(macOS 11.3, *)) GCController.shouldMonitorBackgroundEvents = on;
}

void Install(LogFn log, ShareFn share) {
  if (g_log) return;
  g_log = log;
  g_share = share;
  if (@available(macOS 11.0, *)) {
    g_hooked = [NSHashTable weakObjectsHashTable];
    g_polled = [NSMapTable weakToStrongObjectsMapTable];
    g_handled = [NSMapTable weakToStrongObjectsMapTable];
    g_names = [NSMapTable weakToStrongObjectsMapTable];
    g_wanted = WantedState();
    [[NSNotificationCenter defaultCenter]
        addObserverForName:GCControllerDidConnectNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
                  Hook((GCController*)note.object);
                }];
    NSArray<GCController*>* now = [GCController controllers];
    for (GCController* c in now) Hook(c);
    g_poll = [NSTimer timerWithTimeInterval:1.0 / 30.0 repeats:YES block:^(NSTimer*) { Poll(); }];
    [[NSRunLoop mainRunLoop] addTimer:g_poll forMode:NSRunLoopCommonModes];
    Log([NSString stringWithFormat:@"[pad-hotkeys] GameController Share watch installed (%lu "
                                   @"controller(s) now; handler + 30 Hz poll)",
                                   (unsigned long)now.count]);
  } else {
    Log(@"[pad-hotkeys] GameController Share watch needs macOS 11+");
  }
}

}  // namespace fable2::padkeys::gcshare
