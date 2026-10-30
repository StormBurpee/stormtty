#include "core_test.hpp"
// Exercise the actual host, renderer, retained actors and native command path.
// This is not a substitute implementation of the footer or an AppKit mock.
#include "../src/platform/macos/window.mm"
#include <future>
using namespace stormtty;
namespace {
bool wait_for(const std::function<bool()>& condition,double seconds=5) {
    NSDate* deadline=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(!condition()&&deadline.timeIntervalSinceNow>0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    return condition();
}
void capture(const std::shared_ptr<macos::Host>& h,const char* path) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
    for(;;){
        h->smoke_force_present=true;h->update();[h->view layoutSubtreeIfNeeded];[h->window displayIfNeeded];[CATransaction flush];
        h->smoke_capture_pending=true;
        auto read=std::async(std::launch::async,[h]{return h->renderer->read_pixels();});
        const bool ready=wait_for([&]{return read.wait_for(std::chrono::seconds(0))==std::future_status::ready;},15);
        h->smoke_capture_pending=false;CHECK(ready);
        try{auto pixels=read.get();CHECK(!pixels.empty());write_macos_capture(path,pixels,h->pixel_width,h->pixel_height);return;}
        catch(const std::exception& e){
            if(!std::string_view(e.what()).starts_with("Metal drawable was not observed presented onscreen")||std::chrono::steady_clock::now()>=deadline)throw;
            h->wake();[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        }
    }
}
}
int main(){@autoreleasepool{return run_core_tests("native macOS context footer host",[]{
    [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    auto config=default_config(Platform::macos);config.profiles.clear();Profile profile;
    profile.id="footer-fixture";profile.label="Footer fixture";profile.executable="/bin/sh";profile.working_directory="/";
    profile.arguments={"-c","printf 'FOOTER_READY\\n'; IFS= read -r x; [ \"$x\" = context ] || exit 19; printf '\\033]7;file://localhost/owned/footer/a-long-project-folder/with-sibling-workspaces/and-complete-context-retained\\007FOOTER_CONTEXT\\n'; IFS= read -r x; if [ \"$x\" = finish ]; then printf 'FOOTER_INPUT_OK\\n'; exit 7; else exit 19; fi"};
    config.profiles.push_back(profile);config.default_profile=profile.id;
    auto h=std::make_shared<macos::Host>(config,NativeRunOptions{});
    struct Cleanup{std::shared_ptr<macos::Host> h;~Cleanup(){try{h->begin_close(true);wait_for([&]{h->guard([&]{h->update();});return h->finished;});}catch(...){}h->window.delegate=nil;NSApp.delegate=nil;}}cleanup{h};
    h->initialize();CHECK(wait_for([&]{return macos::frame_text(h->active()->frame()).find("FOOTER_READY")!=std::string::npos;}));
    auto actor=h->active();CHECK(h->footer_presentation().context.directory_source==PaneDirectorySource::configured);
    CHECK(h->footer_presentation().context.directory=="/");CHECK(h->footer_layout().bounds.height==26);
    CHECK(h->inspect_button.superview==h->backdrop);CHECK(h->inspect_button.superview!=h->view);
    const auto button_bounds=[h->view convertRect:h->inspect_button.frame fromView:h->backdrop];
    CHECK(near(button_bounds.origin.y,h->footer_layout().inspect.y));
    CHECK([h->inspect_button.accessibilityLabel isEqualToString:@"Inspect active terminal context"]);
    const auto grid=h->sent_sizes;const auto before=h->layout.panes;
    CHECK(actor->text("context\n")==SessionSendResult::accepted);
    CHECK(wait_for([&]{return macos::frame_text(actor->frame()).find("FOOTER_CONTEXT")!=std::string::npos;}));
    h->update();CHECK(h->sent_sizes==grid);CHECK(h->layout.panes.size()==before.size());CHECK(h->layout.panes.front().bounds==before.front().bounds);
    CHECK(h->footer_presentation().context.directory_source==PaneDirectorySource::observed);
    CHECK(h->footer_presentation().context.directory.find("/owned/footer")!=std::string::npos);
    CHECK(h->footer_presentation().context.head.empty());
    capture(h,"macos-footer-normal-metal.png");
    for(int iteration=0;iteration<2;++iteration){
        [h->inspect_button performClick:nil];CHECK(wait_for([&]{return h->window.attachedSheet!=nil;}));
        CHECK(h->active()==actor);CHECK(h->sent_sizes==grid);
        NSWindow* sheet=h->window.attachedSheet;
        [h->inspect_button performClick:nil];CHECK(h->window.attachedSheet==sheet);
        [h->window endSheet:sheet returnCode:NSModalResponseCancel];
        CHECK(wait_for([&]{return h->window.attachedSheet==nil&&h->window.firstResponder==h->view&&h->inspect_button.enabled;}));
    }
    h->command("pane.split_right");CHECK(h->layout.panes.size()==2);
    CHECK(wait_for([&]{return macos::frame_text(h->active()->frame()).find("FOOTER_READY")!=std::string::npos;}));
    const auto* original_pane=h->workspace.pane(before.front().pane);CHECK(original_pane!=nullptr);
    h->workspace.focus(original_pane->id);h->focus_changed();h->resize();CHECK(h->active()==actor);
    capture(h,"macos-footer-split-metal.png");
    const auto mouse=[&](stormtty::Point point,MouseAction action){
        const auto location=[h->view convertPoint:NSMakePoint(point.x,point.y) toView:nil];
        NSEvent* event=[NSEvent mouseEventWithType:(action==MouseAction::press?NSEventTypeLeftMouseDown:NSEventTypeLeftMouseUp) location:location modifierFlags:0 timestamp:0 windowNumber:h->window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
        h->mouse(event,action);
    };
    const auto divider=h->layout.dividers.front().bounds;const auto footer=h->footer_layout().bounds;
    mouse({divider.x+divider.width/2,divider.y+divider.height/2},MouseAction::press);CHECK(bool(h->dragged_divider));
    mouse({footer.width/2,footer.y+footer.height/2},MouseAction::release);CHECK(!h->dragged_divider);
    const auto pane=h->layout.panes.front().bounds;
    mouse({pane.x+4,pane.y+4},MouseAction::press);CHECK(bool(h->selection_pane));
    mouse({footer.width/2,footer.y+footer.height/2},MouseAction::release);CHECK(!h->selection_pane);
    mouse({footer.width/2,footer.y+footer.height/2},MouseAction::press);CHECK(h->footer_pointer_down);
    mouse({pane.x+4,pane.y+4},MouseAction::release);CHECK(!h->footer_pointer_down);CHECK(!h->selection_pane);CHECK(!h->terminal_pointer_down);
    const auto split_grid=h->sent_sizes;
    h->message="Fixture notice: context remains available through Inspect";h->update();CHECK(h->sent_sizes==split_grid);
    CHECK(h->inspect_button.toolTip.length>0);h->message.clear();
    [h->window setContentSize:NSMakeSize(400,300)];h->resize();h->update();
    CHECK(h->footer_layout().inspect.x+h->footer_layout().inspect.width<=h->view.bounds.size.width);
    CHECK(h->footer_layout().terminal.y+h->footer_layout().terminal.height<h->footer_layout().bounds.y);
    capture(h,"macos-footer-narrow-metal.png");
    // AppKit owns the button and titlebar; Metal readback does not include them.
    NSBitmapImageRep* button=[h->inspect_button bitmapImageRepForCachingDisplayInRect:h->inspect_button.bounds];CHECK(button!=nil);
    [h->inspect_button cacheDisplayInRect:h->inspect_button.bounds toBitmapImageRep:button];
    CHECK([[button representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@"macos-footer-inspect-appkit.png" atomically:YES]);
    CHECK(actor->text("finish\n")==SessionSendResult::accepted);CHECK(wait_for([&]{return actor->closed();}));h->update();
    CHECK(actor->info().exit_code==7);CHECK(macos::frame_text(actor->frame()).find("FOOTER_INPUT_OK")!=std::string::npos);
    CHECK(h->active()==actor);CHECK(h->footer_presentation().status=="Terminal exited · exit 7");
    capture(h,"macos-footer-exited-metal.png");
    for(int i=0;i<2;++i){
        [h->inspect_button performClick:nil];CHECK(wait_for([&]{return h->window.attachedSheet!=nil;}));
        [h->window endSheet:h->window.attachedSheet returnCode:NSModalResponseCancel];
        CHECK(wait_for([&]{return h->window.attachedSheet==nil&&h->inspect_button.enabled&&h->window.firstResponder==h->view;}));
        CHECK(h->active()==actor);
    }
});}}
