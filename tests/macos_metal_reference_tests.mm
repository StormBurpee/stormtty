// Independent reference path: no StormTTY renderer, host, session or capture code.
// Apple's MTKView contract: obtain currentRenderPassDescriptor, encode/end,
// register presentDrawable on the command buffer, then commit.
// https://developer.apple.com/documentation/metalkit/mtkview
// https://developer.apple.com/documentation/metal/mtldrawable/presentedtime
#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#include <atomic>
#include <memory>
#include <cstdio>

namespace {
struct Observation {
    std::atomic<unsigned> submitted{}, scheduled{}, completed{}, callbacks{}, positive{}, zero{}, gpu_errors{}, unavailable{}, encode_errors{};
    std::atomic<double> last_positive{};
};
}

@interface MetalReference : NSObject <NSApplicationDelegate, MTKViewDelegate> {
@public
    std::shared_ptr<Observation> observations_;
    NSWindow* window_;
    MTKView* view_;
    id<MTLCommandQueue> queue_;
    NSTimer* timer_;
    NSDate* deadline_;
    bool timed_out_;
    bool setup_failed_;
}
- (void)poll;
@end

@implementation MetalReference
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    queue_=[device newCommandQueue];
    if(!device || !queue_) {setup_failed_=true;[self poll];return;}
    window_=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,480,320)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable
        backing:NSBackingStoreBuffered defer:NO];
    window_.title=@"Independent MetalKit presentation reference";
    window_.releasedWhenClosed=NO;
    view_=[[MTKView alloc] initWithFrame:NSMakeRect(0,0,480,320) device:device];
    view_.clearColor=MTLClearColorMake(0.12,0.22,0.34,1.0);
    view_.colorPixelFormat=MTLPixelFormatBGRA8Unorm;
    view_.preferredFramesPerSecond=60;
    view_.delegate=self;
    window_.contentView=view_;
    [window_ center];[window_ makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
    deadline_=[NSDate dateWithTimeIntervalSinceNow:12.0];
    timer_=[NSTimer scheduledTimerWithTimeInterval:0.05 target:self selector:@selector(poll) userInfo:nil repeats:YES];
}
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {(void)view;(void)size;}
- (void)drawInMTKView:(MTKView*)view {
    @autoreleasepool {
        const auto observations=observations_;
        if(observations->submitted.load()>=120 || observations->positive.load()>0) {view.paused=YES;return;}
        MTLRenderPassDescriptor* descriptor=view.currentRenderPassDescriptor;
        id<CAMetalDrawable> drawable=view.currentDrawable;
        if(!descriptor || !drawable) {++observations->unavailable;return;}
        id<MTLCommandBuffer> command=[queue_ commandBuffer];
        id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:descriptor];
        if(!command || !encoder) {++observations->encode_errors;return;}
        [encoder endEncoding];
        [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
            const double timestamp=presented.presentedTime;
            ++observations->callbacks;
            if(timestamp>0) {observations->last_positive.store(timestamp);++observations->positive;}
            else ++observations->zero;
        }];
        [command addScheduledHandler:^(id<MTLCommandBuffer> scheduled) {(void)scheduled;++observations->scheduled;}];
        [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            if(completed.status==MTLCommandBufferStatusError || completed.error)++observations->gpu_errors;
            ++observations->completed;
        }];
        ++observations->submitted;
        [command presentDrawable:drawable];
        [command commit];
    }
}
- (void)poll {
    const auto observations=observations_;
    const bool positive=observations && observations->positive.load()>0;
    if(positive)view_.paused=YES;
    const bool drained=observations && observations->completed.load()==observations->submitted.load();
    timed_out_=deadline_ && deadline_.timeIntervalSinceNow<=0;
    if(!setup_failed_ && !(positive&&drained) && !timed_out_)return;
    [timer_ invalidate];timer_=nil;view_.paused=YES;
    [NSApp stop:nil];
    // Wake NSApp.run after stop; this is a private application event, not input.
    NSEvent* wake=[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:wake atStart:NO];
}
@end

int main() {@autoreleasepool {
    [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    MetalReference* probe=[MetalReference new];probe->observations_=std::make_shared<Observation>();NSApp.delegate=probe;
    [NSApp run];
    const auto o=probe->observations_;
    const bool passed=o && !probe->setup_failed_ && !probe->timed_out_ && o->submitted.load()>0 && o->positive.load()>0 &&
        o->last_positive.load()>0 && o->gpu_errors.load()==0 && o->encode_errors.load()==0 && o->completed.load()==o->submitted.load();
    NSScreen* screen=probe->window_.screen;
    const auto display=static_cast<CGDirectDisplayID>([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);
    NSDictionary* evidence=@{
        @"reference":@"Standalone MTKView clear + command-buffer present + commit; true NSApp.run",
        @"passed":@(passed),@"timed_out":@(probe->timed_out_),@"setup_failed":@(probe->setup_failed_),
        @"submitted":@(o?o->submitted.load():0),@"scheduled":@(o?o->scheduled.load():0),@"completed":@(o?o->completed.load():0),
        @"callbacks":@(o?o->callbacks.load():0),@"positive_presentations":@(o?o->positive.load():0),@"zero_time_callbacks":@(o?o->zero.load():0),
        @"last_positive_time":@(o?o->last_positive.load():0),@"gpu_errors":@(o?o->gpu_errors.load():0),@"encode_errors":@(o?o->encode_errors.load():0),
        @"drawable_unavailable":@(o?o->unavailable.load():0),@"device":probe->view_.device.name?:@"unavailable",
        @"registry_id":@(probe->view_.device.registryID),@"window_visible":@(probe->window_.visible),@"window_key":@(probe->window_.keyWindow),
        @"app_active":@(NSApp.active),@"miniaturized":@(probe->window_.miniaturized),@"view_hidden":@(probe->view_.hidden),
        @"occlusion_state":@(probe->window_.occlusionState),@"display_online":@(screen && CGDisplayIsOnline(display)),
        @"display_active":@(screen && CGDisplayIsActive(display)),@"physical_ui_verified":@NO,
        @"interpretation":@"Zero presentedTime means not presented or dropped; does not establish the cause. No StormTTY qualification implied."};
    NSError* error=nil;NSData* json=[NSJSONSerialization dataWithJSONObject:evidence options:NSJSONWritingPrettyPrinted error:&error];
    const bool written=json && [json writeToFile:@"macos-metal-reference.json" atomically:YES];
    if(json)std::fprintf(stdout,"%s\n",[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
    else std::fprintf(stderr,"Unable to serialize reference evidence: %s\n",error.localizedDescription.UTF8String);
    probe->view_.delegate=nil;[probe->window_ orderOut:nil];NSApp.delegate=nil;
    return passed&&written?0:2;
}}
