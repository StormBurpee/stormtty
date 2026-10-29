#include "core_test.hpp"
#include "../src/platform/macos/pane_inspector.hpp"
using namespace stormtty;
namespace {
bool wait_for(bool (^condition)(void)) {
    NSDate* deadline=[NSDate dateWithTimeIntervalSinceNow:3];
    while(!condition()&&deadline.timeIntervalSinceNow>0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    return condition();
}
}
int main(){@autoreleasepool{return run_core_tests("native AppKit pane inspector",[]{
    [NSApplication sharedApplication];
    ShellContext shell;shell.display_directory="/owned/project";shell.repository_root="/owned";
    shell.git=GitHeadKind::detached;shell.head="a1b2c3d";
    auto snapshot=make_pane_context({"Editor\nInjected", "Fixture", "/stale", "/configured", &shell, false, true});
    STPaneInspector* inspector=[[STPaneInspector alloc] initWithSnapshot:snapshot];
    NSTextView* text=inspector.details;
    CHECK([inspector.window.title isEqualToString:@"Pane context"]);
    CHECK(inspector.scroll.hasVerticalScroller);CHECK(text.selectable);CHECK(!text.editable);CHECK(!text.richText);
    CHECK(!text.automaticLinkDetectionEnabled);CHECK(!text.automaticDataDetectionEnabled);
    CHECK([text.accessibilityLabel isEqualToString:@"Pane context details"]);
    CHECK([text.string containsString:@"Title: Editor[U+000A]Injected"]);
    CHECK([text.string containsString:@"Observed directory: /owned/project"]);
    CHECK([text.string containsString:@"Configured start folder: /configured"]);
    CHECK([text.string containsString:@"Detached HEAD: a1b2c3d"]);
    CHECK([text.string containsString:@"State: Exited (retained output)"]);
    CHECK(![text.string containsString:@"/stale"]);
    snapshot.detail="changed after creation";CHECK(![text.string containsString:@"changed after creation"]);
    [text setSelectedRange:NSMakeRange(0,5)];CHECK(text.selectedRange.length==5);
    CHECK([inspector.closeButton.keyEquivalent isEqualToString:@"\033"]);
    STPaneInspector* unavailable=[[STPaneInspector alloc] initWithSnapshot:make_pane_context({"Terminal","Profile",{},"/start"})];
    CHECK([unavailable.details.string containsString:@"Observed directory: Unavailable"]);
    CHECK([unavailable.details.string containsString:@"Configured start folder: /start"]);
    PaneContextPresentation longSnapshot;
    for(int i=0;i<100;++i)longSnapshot.detail+="Long context line /owned/path/not-an-action\n";
    STPaneInspector* longInspector=[[STPaneInspector alloc] initWithSnapshot:longSnapshot];
    NSTextView* longText=longInspector.details;
    [longText.layoutManager ensureLayoutForTextContainer:longText.textContainer];
    CHECK([longText.layoutManager usedRectForTextContainer:longText.textContainer].size.height>265);
    CHECK(longText.textContainer.widthTracksTextView);CHECK(!longText.horizontallyResizable);
    NSWindow* parent=[[NSWindow alloc] initWithContentRect:NSMakeRect(100,100,800,600)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    parent.releasedWhenClosed=NO;
    NSTextField* focus=[[NSTextField alloc] initWithFrame:NSMakeRect(20,20,200,24)];
    [parent.contentView addSubview:focus];[parent makeKeyAndOrderFront:nil];
    for(NSString* appearance in @[NSAppearanceNameAqua,NSAppearanceNameDarkAqua]){
        parent.appearance=[NSAppearance appearanceNamed:appearance];
        auto display=make_pane_context({"Build · release", "Developer shell", "/stale", "/configured", &shell, false, true});
        STPaneInspector* shown=[[STPaneInspector alloc] initWithSnapshot:display];
        __block BOOL completed=NO;
        [shown presentForWindow:parent completion:^{[parent makeFirstResponder:focus];completed=YES;}];
        CHECK(wait_for(^bool{return parent.attachedSheet==shown.window&&shown.window.visible;}));
        CHECK(shown.window.sheetParent==parent);
        CHECK([shown.window.effectiveAppearance.name isEqualToString:appearance]);
        // Rasterize only our AppKit-owned view, not the desktop/compositor.
        NSView* surface=shown.window.contentView;[surface layoutSubtreeIfNeeded];[surface displayIfNeeded];
        NSBitmapImageRep* bitmap=[surface bitmapImageRepForCachingDisplayInRect:surface.bounds];CHECK(bitmap!=nil);
        [surface cacheDisplayInRect:surface.bounds toBitmapImageRep:bitmap];
        NSData* png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];CHECK(png.length>0);
        NSString* file=[appearance isEqualToString:NSAppearanceNameAqua]?@"pane-context-light.png":@"pane-context-dark.png";
        CHECK([png writeToFile:file atomically:YES]);
        // Owned control keyboard equivalent dispatch, never global key injection.
        NSEvent* escape=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:shown.window.windowNumber context:nil characters:@"\033" charactersIgnoringModifiers:@"\033" isARepeat:NO keyCode:53];
        CHECK([shown.closeButton performKeyEquivalent:escape]);
        CHECK(wait_for(^bool{return completed&&parent.attachedSheet==nil;}));
        CHECK(parent.firstResponder==focus.currentEditor || parent.firstResponder==focus);
    }
    [parent orderOut:nil];
});}}
