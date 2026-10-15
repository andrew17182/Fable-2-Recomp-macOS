#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <SDL3/SDL_events.h>

#include "rex/ui/metal/metal_startup_wait.h"

namespace rex::ui::metal {
MetalStartupWaitResult WaitForMetalStartup(const std::function<bool()>& complete) {
  MetalStartupWaitResult result;
  @autoreleasepool {
    NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow;
    if (!window) {
      for (NSWindow* candidate in NSApp.windows) {
        if (candidate.visible && candidate.contentView.bounds.size.width > 200) {
          window = candidate;
          break;
        }
      }
    }
    // Warm starts finish in well under the grace period; showing the overlay
    // then only flashes it for a frame, so wait before creating it.
    constexpr NSTimeInterval kOverlayGraceSeconds = 0.5;
    NSDate* overlay_deadline =
        [NSDate dateWithTimeIntervalSinceNow:kOverlayGraceSeconds];
    NSView* overlay = nil;
    auto show_overlay = [&] {
      overlay = [[NSView alloc] initWithFrame:window.contentView.bounds];
      overlay.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      overlay.wantsLayer = YES;
      overlay.layer.backgroundColor =
          [NSColor colorWithCalibratedRed:0.075 green:0.085 blue:0.095 alpha:1].CGColor;
      [overlay setAccessibilityLabel:@"Loading Fable II"];

      NSTextField* title = [NSTextField labelWithString:@"Loading Fable II"];
      title.font = [NSFont systemFontOfSize:25 weight:NSFontWeightMedium];
      title.textColor = NSColor.whiteColor;
      title.alignment = NSTextAlignmentCenter;
      title.translatesAutoresizingMaskIntoConstraints = NO;
      [overlay addSubview:title];
      NSTextField* detail = [NSTextField labelWithString:@"Getting ready…"];
      detail.font = [NSFont systemFontOfSize:14];
      detail.textColor = [NSColor colorWithCalibratedWhite:0.7 alpha:1];
      detail.translatesAutoresizingMaskIntoConstraints = NO;
      [overlay addSubview:detail];
      NSProgressIndicator* spinner = [[[NSProgressIndicator alloc] init] autorelease];
      spinner.style = NSProgressIndicatorStyleSpinning;
      spinner.controlSize = NSControlSizeRegular;
      spinner.translatesAutoresizingMaskIntoConstraints = NO;
      [overlay addSubview:spinner];
      [NSLayoutConstraint activateConstraints:@[
        [title.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
        [title.centerYAnchor constraintEqualToAnchor:overlay.centerYAnchor constant:-22],
        [detail.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
        [detail.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:10],
        [spinner.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
        [spinner.topAnchor constraintEqualToAnchor:detail.bottomAnchor constant:22]
      ]];
      [window.contentView addSubview:overlay positioned:NSWindowAbove relativeTo:nil];
      [spinner startAnimation:nil];
      [overlay layoutSubtreeIfNeeded];
      [window displayIfNeeded];
      result.overlay_shown = true;
    };

    while (!complete()) {
      @autoreleasepool {
        if (window && !overlay && overlay_deadline.timeIntervalSinceNow <= 0) {
          show_overlay();
        }
        // Cocoa animation/input stays alive; SDL retains queued window/input
        // events for its regular loop once startup finishes. Quit requests go
        // through the SDK's existing synchronous event watcher.
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.008]
            inMode:NSDefaultRunLoopMode dequeue:YES];
        if (event) {
          [NSApp sendEvent:event];
          ++result.events;
        }
        SDL_PumpEvents();
        [NSApp updateWindows];
        ++result.iterations;
      }
    }
    [overlay removeFromSuperview];
    [overlay release];
    [window displayIfNeeded];
  }
  return result;
}

void HideAppForBackgroundWork() {
  @autoreleasepool {
    for (NSWindow* window in NSApp.windows) {
      [window orderOut:nil];
    }
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  }
}
}  // namespace rex::ui::metal
