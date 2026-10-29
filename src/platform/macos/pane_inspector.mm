#include "pane_inspector.hpp"
#import <QuartzCore/QuartzCore.h>

// The owned surface draws its own semantic background. NSWindow's backing
// otherwise sits outside content-view rasterization, leaving exported captures
// transparent and making their text illegible against arbitrary viewer colors.
@interface STPaneInspectorSurface : NSView
@end
@implementation STPaneInspectorSurface
- (BOOL)isOpaque {return YES;}
- (void)drawRect:(NSRect)dirtyRect {
    [NSColor.windowBackgroundColor setFill];NSRectFill(dirtyRect);
}
@end

@implementation STPaneInspector
- (instancetype)initWithSnapshot:(const stormtty::PaneContextPresentation&)snapshot {
    NSPanel* panel=[[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,580,440)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    self=[super initWithWindow:panel];if(!self)return nil;
    panel.title=@"Pane context";panel.releasedWhenClosed=NO;
    panel.titleVisibility=NSWindowTitleHidden;panel.titlebarAppearsTransparent=YES;
    panel.backgroundColor=NSColor.windowBackgroundColor;
    NSView* surface=[[STPaneInspectorSurface alloc] initWithFrame:panel.contentView.bounds];
    panel.contentView=surface;
    NSTextField* heading=[NSTextField labelWithString:@"Pane context"];
    heading.font=[NSFont systemFontOfSize:22 weight:NSFontWeightSemibold];
    heading.frame=NSMakeRect(24,378,532,30);[surface addSubview:heading];
    NSTextField* subtitle=[NSTextField labelWithString:@"A snapshot of the active terminal session"];
    subtitle.font=[NSFont systemFontOfSize:12];subtitle.textColor=NSColor.secondaryLabelColor;
    subtitle.frame=NSMakeRect(24,351,532,20);[surface addSubview:subtitle];
    _scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(24,70,532,265)];
    _scroll.hasVerticalScroller=YES;_scroll.borderType=NSNoBorder;
    _scroll.wantsLayer=YES;_scroll.layer.cornerRadius=8;
    _details=[[NSTextView alloc] initWithFrame:_scroll.contentView.bounds];
    // Labels remain inert text, never filesystem, URL, or command authority.
    _details.editable=NO;_details.selectable=YES;_details.richText=NO;
    _details.automaticLinkDetectionEnabled=NO;_details.automaticDataDetectionEnabled=NO;
    _details.font=[NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    _details.textColor=NSColor.labelColor;_details.backgroundColor=NSColor.textBackgroundColor;
    _details.textContainerInset=NSMakeSize(14,14);
    _details.verticallyResizable=YES;_details.horizontallyResizable=NO;
    _details.autoresizingMask=NSViewWidthSizable;
    _details.textContainer.widthTracksTextView=YES;
    _details.textContainer.containerSize=NSMakeSize(_scroll.contentSize.width,CGFLOAT_MAX);
    _details.maxSize=NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);
    _details.string=[[NSString alloc] initWithBytes:snapshot.detail.data() length:snapshot.detail.size() encoding:NSUTF8StringEncoding] ?: @"Unavailable";
    [_details setAccessibilityLabel:@"Pane context details"];
    _scroll.documentView=_details;[surface addSubview:_scroll];
    NSTextField* hint=[NSTextField labelWithString:@"Read-only · Reopen to refresh"];
    hint.font=[NSFont systemFontOfSize:11];hint.textColor=NSColor.secondaryLabelColor;
    hint.frame=NSMakeRect(24,27,390,18);[surface addSubview:hint];
    _closeButton=[NSButton buttonWithTitle:@"Close" target:self action:@selector(closeInspector:)];
    _closeButton.bezelStyle=NSBezelStyleRounded;_closeButton.frame=NSMakeRect(464,20,92,32);
    _closeButton.keyEquivalent=@"\033";_closeButton.keyEquivalentModifierMask=0;[_closeButton setAccessibilityLabel:@"Close pane context"];
    [surface addSubview:_closeButton];panel.initialFirstResponder=_details;
    return self;
}
- (void)presentForWindow:(NSWindow*)parent completion:(void (^)(void))completion {
    self.window.appearance=parent.effectiveAppearance;
    // Completion retains the controller until AppKit finishes its attached sheet.
    [parent beginSheet:self.window completionHandler:^(NSModalResponse response){
        (void)response;[self.window orderOut:nil];if(completion)completion();
    }];
}
- (void)closeInspector:(id)sender {
    (void)sender;NSWindow* parent=self.window.sheetParent;
    if(parent)[parent endSheet:self.window returnCode:NSModalResponseOK];
}
@end
