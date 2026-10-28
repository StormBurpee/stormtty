// Standalone diagnostic, not StormTTY's native acceptance test.
// See macos-presentation-probe.md for build instructions and evidence limits.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CoreGraphics.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>

namespace {
constexpr unsigned maximum_frames = 256;
struct Frame {
    std::atomic<bool> scheduled{false}, completed{false}, callback{false};
    std::atomic<double> presented_time{0};
};
struct Evidence {
    std::array<Frame, maximum_frames> frames;
    std::atomic<unsigned> in_flight{0}, gpu_errors{0};
};
using Clock = std::chrono::steady_clock;
}

@interface PresentationView : NSView
@end
@implementation PresentationView
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)wantsUpdateLayer { return YES; }
// Rendering is explicitly paced by the diagnostic timer, not AppKit repainting.
- (void)updateLayer {}
@end

@interface PresentationProbe : NSObject <NSApplicationDelegate> {
@public
    int result;
    bool finished;
    unsigned submitted, unavailable;
    Clock::time_point started;
    std::shared_ptr<Evidence> evidence;
    NSWindow* window;
    PresentationView* view;
    CAMetalLayer* metal_layer;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    NSTimer* timer;
}
- (void)tick:(NSTimer*)sender;
- (void)finish:(int)code reason:(const char*)reason;
@end

@implementation PresentationProbe
- (void)finish:(int)code reason:(const char*)reason {
    if (finished) return;
    finished = true;
    result = code;
    [timer invalidate];
    unsigned scheduled = 0, completed = 0, callbacks = 0, positive = 0, matching = 0;
    for (unsigned i = 0; i < submitted; ++i) {
        const auto& frame = evidence->frames[i];
        const bool done = frame.completed.load();
        const double time = frame.presented_time.load();
        const bool shown = std::isfinite(time) && time > 0;
        scheduled += frame.scheduled.load(); completed += done;
        callbacks += frame.callback.load(); positive += shown; matching += shown && done;
        std::printf("FRAME id=%u scheduled=%d completed=%d callback=%d presentedTime=%.9f\n",
                    i + 1, int(frame.scheduled.load()), int(done), int(frame.callback.load()), time);
    }
    NSScreen* screen = window.screen;
    NSNumber* screen_number = screen.deviceDescription[@"NSScreenNumber"];
    const auto display = static_cast<CGDirectDisplayID>(screen_number.unsignedIntValue);
    std::printf("CONTEXT visible=%d key=%d appActive=%d occlusion=%llu screens=%lu "
                "screenPresent=%d displayOnline=%d displayActive=%d layerAttached=%d "
                "layerBounds=%s drawableSize=%.0fx%.0f\n",
                int(window.visible), int(window.keyWindow), int(NSApp.active),
                static_cast<unsigned long long>(window.occlusionState),
                static_cast<unsigned long>(NSScreen.screens.count), int(screen != nil),
                screen ? int(CGDisplayIsOnline(display)) : 0,
                screen ? int(CGDisplayIsActive(display)) : 0, int(view.window == window),
                NSStringFromRect(NSRectFromCGRect(metal_layer.bounds)).UTF8String,
                metal_layer.drawableSize.width, metal_layer.drawableSize.height);
    std::printf("RESULT %s exit=%d reason=%s elapsed=%.3f submitted=%u scheduled=%u completed=%u "
                "callbacks=%u positive=%u matching=%u unavailable=%u gpuErrors=%u\n",
                code == 0 ? "PASS" : code == 2 ? "INCONCLUSIVE" : "ERROR", code, reason,
                std::chrono::duration<double>(Clock::now() - started).count(), submitted,
                scheduled, completed, callbacks, positive, matching, unavailable,
                evidence->gpu_errors.load());
    std::puts("Scope: standalone CAMetalLayer presentation evidence only; not StormTTY acceptance, "
              "physical display inspection, performance, or a diagnosis of a headless environment.");
    std::fflush(stdout);
    [NSApp stop:nil];
    // stop: takes effect after an event; wake the ordinary AppKit run loop.
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
        location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
        subtype:0 data1:0 data2:0] atStart:NO];
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    started = Clock::now();
    @try {
        device = MTLCreateSystemDefaultDevice();
        queue = [device newCommandQueue];
        if (!device || !queue) { [self finish:1 reason:"Metal device/queue unavailable"]; return; }
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 320)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed = NO;
        window.title = @"StormTTY isolated presentation diagnostic";
        window.opaque = YES;
        view = [[PresentationView alloc] initWithFrame:NSMakeRect(0, 0, 480, 320)];
        view.wantsLayer = YES;
        metal_layer = (CAMetalLayer*)view.layer;
        if (![metal_layer isKindOfClass:CAMetalLayer.class]) {
            [self finish:1 reason:"AppKit did not create a CAMetalLayer"]; return;
        }
        metal_layer.device = device;
        metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        metal_layer.framebufferOnly = YES;
        metal_layer.opaque = YES;
        metal_layer.presentsWithTransaction = NO;
        metal_layer.displaySyncEnabled = YES;
        metal_layer.allowsNextDrawableTimeout = YES;
        metal_layer.maximumDrawableCount = 3;
        window.contentView = view;
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [view layoutSubtreeIfNeeded];
        const NSRect pixels = [view convertRectToBacking:view.bounds];
        metal_layer.contentsScale = window.backingScaleFactor;
        metal_layer.drawableSize = pixels.size;
        std::printf("START os=%s device=%s observationSeconds=12 frameIntervalSeconds=0.1 "
                    "maximumOutstandingGPU=1 presentsWithTransaction=0\n",
                    NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String,
                    device.name.UTF8String);
        std::fflush(stdout);
        // A normal run-loop turn after launch attaches and commits the layer tree.
        timer = [NSTimer timerWithTimeInterval:0.1 target:self selector:@selector(tick:)
            userInfo:nil repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    } @catch (NSException* exception) {
        std::fprintf(stderr, "SETUP_EXCEPTION %s\n", exception.reason.UTF8String);
        [self finish:1 reason:"AppKit/Metal setup exception"];
    }
}

- (void)tick:(NSTimer*)sender {
    (void)sender;
    if (finished) return;
    @autoreleasepool { @try {
        if (evidence->gpu_errors.load()) { [self finish:1 reason:"GPU command failed"]; return; }
        for (unsigned i = 0; i < submitted; ++i) {
            const auto& frame = evidence->frames[i];
            const double time = frame.presented_time.load();
            if (frame.completed.load() && std::isfinite(time) && time > 0) {
                [self finish:0 reason:"positive presentedTime and matching GPU completion"]; return;
            }
        }
        if (Clock::now() - started >= std::chrono::seconds(12)) {
            [self finish:2 reason:"presentation evidence unavailable within observation window"]; return;
        }
        if (evidence->in_flight.load() || submitted == maximum_frames) return;
        id<CAMetalDrawable> drawable = [metal_layer nextDrawable];
        if (!drawable) { ++unavailable; return; }
        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (!command) { [self finish:1 reason:"command buffer allocation failed"]; return; }
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = (submitted % 2 == 0)
            ? MTLClearColorMake(0.1, 0.6, 0.9, 1) : MTLClearColorMake(0.9, 0.3, 0.1, 1);
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) { [self finish:1 reason:"render encoder allocation failed"]; return; }
        [encoder endEncoding];
        const unsigned index = submitted++;
        const auto state = evidence; // Blocks own callback state beyond delegate teardown.
        [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            state->frames[index].presented_time.store(shown.presentedTime);
            state->frames[index].callback.store(true);
        }];
        [command addScheduledHandler:^(id<MTLCommandBuffer> buffer) {
            (void)buffer; state->frames[index].scheduled.store(true);
        }];
        [command addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
            if (buffer.status == MTLCommandBufferStatusCompleted)
                state->frames[index].completed.store(true);
            else {
                state->gpu_errors.fetch_add(1);
                std::fprintf(stderr, "GPU_ERROR frame=%u status=%lu errorCode=%ld\n",
                    index + 1, static_cast<unsigned long>(buffer.status),
                    static_cast<long>(buffer.error.code));
            }
            state->in_flight.fetch_sub(1);
        }];
        [command presentDrawable:drawable];
        evidence->in_flight.fetch_add(1);
        [command commit];
        // Autorelease pool promptly releases drawable references. No GPU wait on AppKit.
    } @catch (NSException* exception) {
        std::fprintf(stderr, "RENDER_EXCEPTION %s\n", exception.reason.UTF8String);
        [self finish:1 reason:"AppKit/Metal render exception"];
    } }
}
@end

int main(int argc, const char* argv[]) {
    if (argc != 1) { std::fprintf(stderr, "Usage: %s (no options)\n", argv[0]); return 1; }
    @autoreleasepool { @try {
        try {
            [NSApplication sharedApplication];
            PresentationProbe* probe = [PresentationProbe new];
            probe->result = 1;
            probe->evidence = std::make_shared<Evidence>();
            NSApp.delegate = probe;
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp run];
            [probe->timer invalidate];
            [probe->window orderOut:nil];
            NSApp.delegate = nil;
            return probe->result;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "RESULT ERROR exit=1 exception=%s\n", error.what()); return 1;
        }
    } @catch (NSException* exception) {
        std::fprintf(stderr, "RESULT ERROR exit=1 exception=%s\n", exception.reason.UTF8String); return 1;
    } }
}
