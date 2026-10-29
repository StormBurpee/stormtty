#pragma once
#include "stormtty/pane_context.hpp"
#import <AppKit/AppKit.h>

@interface STPaneInspector : NSWindowController
@property(nonatomic,strong,readonly) NSTextView* details;
@property(nonatomic,strong,readonly) NSScrollView* scroll;
@property(nonatomic,strong,readonly) NSButton* closeButton;
- (instancetype)initWithSnapshot:(const stormtty::PaneContextPresentation&)snapshot;
- (void)presentForWindow:(NSWindow*)parent completion:(void (^)(void))completion;
- (void)closeInspector:(id)sender;
@end
