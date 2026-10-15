#import <AppKit/AppKit.h>
#include "rex/ui/metal/metal_startup_wait.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdio>
static unsigned pumps = 0;
extern "C" void SDL_PumpEvents(void) { ++pumps; }
int main() {
 @autoreleasepool {
  [NSApplication sharedApplication];
  NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,640,480) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
  [window makeKeyAndOrderFront:nil];
  auto count = window.contentView.subviews.count;
  std::atomic<bool> complete{false};
  std::thread worker([&] { std::this_thread::sleep_for(std::chrono::milliseconds(900)); complete.store(true); });
  [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:window.windowNumber context:nil subtype:0 data1:0 data2:0] atStart:YES];
  auto result = rex::ui::metal::WaitForMetalStartup([&]{return complete.load();});
  worker.join();
  assert(result.overlay_shown && result.iterations > 0 && result.events > 0 && pumps > 0);
  assert(window.contentView.subviews.count == count);
  std::atomic<bool> warm{false};
  std::thread warm_worker([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); warm.store(true); });
  auto quick = rex::ui::metal::WaitForMetalStartup([&]{return warm.load();});
  warm_worker.join();
  assert(!quick.overlay_shown && quick.iterations > 0);
  assert(window.contentView.subviews.count == count);
  auto fast = rex::ui::metal::WaitForMetalStartup([]{return true;});
  assert(!fast.overlay_shown && fast.iterations == 0);
  puts("PASS: delayed overlay, no overlay within grace, native events, SDL pumping, cleanup, immediate completion");
  [window orderOut:nil];
 }
}
