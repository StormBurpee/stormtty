#include "stormtty/native_window.hpp"
#include "stormtty/renderer.hpp"
#include "stormtty/session.hpp"
#include "platform.hpp"
#include "input.hpp"
#include "menu_shortcuts.hpp"
#include "pane_inspector.hpp"
#import <AppKit/AppKit.h>
#import <QuartzCore/CATransaction.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace stormtty::macos { class Host; }
@interface STChromeView : NSView {
@public __weak NSButton* selectedTab;NSColor* selectionColor;
}
@end

@interface STTerminalView : NSView <NSTextInputClient> {
@public
    std::weak_ptr<stormtty::macos::Host> host;
    NSMutableAttributedString* marked;
    NSRange markedSelection;
    NSEvent* typingEvent;
    BOOL composingEvent;
    std::set<unsigned short> pressedKeys;
}
@end
@interface STWindowDelegate : NSObject <NSWindowDelegate,NSApplicationDelegate,NSMenuItemValidation> {
@public std::weak_ptr<stormtty::macos::Host> host;
}
- (void)executeCommand:(id)sender;
- (void)selectTab:(id)sender;
- (void)displayPreferencesChanged:(NSNotification*)note;
@end

namespace stormtty::macos {
namespace {
NSString* ns(std::string_view text) { return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] ?: @"\uFFFD"; }
std::string utf8(NSString* value) {
    if(!value)return {};NSData* data=[value dataUsingEncoding:NSUTF8StringEncoding];
    return data?std::string(static_cast<const char*>(data.bytes),data.length):std::string{};
}
std::string plain_title(std::string_view input) {
    // No terminal control characters, bidirectional overrides or unbounded title.
    NSString* source=ns(input);NSMutableString* clean=[NSMutableString string];
    for(NSUInteger i=0;i<source.length&&clean.length<96;++i){const unichar c=[source characterAtIndex:i];
        if(c>=32&&c!=127&&!(c>=0x202a&&c<=0x202e)&&!(c>=0x2066&&c<=0x2069)) [clean appendFormat:@"%C",c];}
    return utf8(clean);
}
Color color(std::string_view value,Color fallback) {
    if(value.size()!=7||value.front()!='#')return fallback;
    auto digit=[](char c)->unsigned{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:0;};
    return {std::uint8_t(digit(value[1])*16+digit(value[2])),std::uint8_t(digit(value[3])*16+digit(value[4])),std::uint8_t(digit(value[5])*16+digit(value[6])),255};
}
std::uint16_t modifiers(NSEventModifierFlags flags) {
    std::uint16_t value=0;if(flags&NSEventModifierFlagShift)value|=Modifier::shift;
    if(flags&NSEventModifierFlagControl)value|=Modifier::control;if(flags&NSEventModifierFlagOption)value|=Modifier::alt;
    if(flags&NSEventModifierFlagCommand)value|=Modifier::super;if(flags&NSEventModifierFlagCapsLock)value|=Modifier::caps_lock;return value;
}
KeyChord chord(NSEvent* event) {
    KeyChord result;const auto flags=event.modifierFlags;
    if(flags&NSEventModifierFlagCommand)result.modifiers=result.modifiers|CommandModifier::command;
    if(flags&NSEventModifierFlagControl)result.modifiers=result.modifiers|CommandModifier::control;
    if(flags&NSEventModifierFlagOption)result.modifiers=result.modifiers|CommandModifier::alt;
    if(flags&NSEventModifierFlagShift)result.modifiers=result.modifiers|CommandModifier::shift;
    switch(physical_key(event.keyCode)) {
    case Key::tab:result.key="TAB";break;case Key::enter:result.key="ENTER";break;
    case Key::left:result.key="LEFT";break;case Key::right:result.key="RIGHT";break;case Key::up:result.key="UP";break;case Key::down:result.key="DOWN";break;
    case Key::escape:result.key="ESCAPE";break;case Key::backspace:result.key="BACKSPACE";break;case Key::del:result.key="DELETE";break;
    default:result.key=utf8(event.charactersIgnoringModifiers.uppercaseString);break;
    }return result;
}
std::string frame_text(FramePtr frame,std::size_t limit=65536,bool selected=false) {
    std::string result;if(!frame)return result;
    for(const auto& row:frame->lines){if(!row)continue;bool any=false;
        for(const auto& cell:row->cells){if(!cell.span||(selected&&!cell.selected))continue;
            const std::string_view text=cell.grapheme.empty()?std::string_view(" "):cell.grapheme;
            if(result.size()+text.size()+1>limit)return result;result+=text;any=true;}
        if(any)result+='\n';}return result;
}
}

class Host final : public std::enable_shared_from_this<Host> {
public:
    Config config;NativeRunOptions options;
    NSWindow* window;STTerminalView* view;STChromeView* chrome_view;NSTitlebarAccessoryViewController* tab_accessory;NSVisualEffectView* backdrop;STWindowDelegate* delegate;
    std::unique_ptr<Renderer> renderer;
    Workspace workspace;Layout layout;
    std::map<SessionId,std::shared_ptr<Session>> sessions;
    std::map<SessionId,TerminalSize> sent_sizes;
    std::vector<NSButton*> tab_buttons;
    NSButton* add_button;NSButton* menu_button;
    std::uint64_t next_session{1},chrome{1};
    double scale{1};std::uint32_t pixel_width{},pixel_height{};
    bool closing{},finished{},close_authorized{},confirming_close{},reduce_motion{},cursor_on{true},blink_scheduled{},retry_scheduled{};
    bool smoke_input{},smoke_seen{},smoke_presented{},smoke_capture_pending{},smoke_force_present{};
    bool appearance_pending{},resize_pending{},update_active{},update_again{};
    std::chrono::steady_clock::time_point smoke_present_deadline{};
    int exit_code{};std::atomic<bool> wake_queued{false};
    NodeId dragged_divider;PaneId selection_pane;std::uint16_t selection_x{},selection_y{};
    std::string message;std::uint64_t accessibility_generation{};
    struct Input {std::function<SessionSendResult()> send;std::size_t bytes{};};
    std::deque<Input> pending;std::size_t pending_bytes{};
    explicit Host(Config c,NativeRunOptions o):config(std::move(c)),options(std::move(o)) {}
    ~Host(){for(auto& [id,s]:sessions){(void)id;s->request_close();}}
    std::shared_ptr<Session> active() const {
        auto p=workspace.pane(workspace.active_pane());if(!p)return {};
        auto found=sessions.find(p->session);return found==sessions.end()?nullptr:found->second;
    }
    double header_height()const {return std::max(0.,[view convertRect:window.contentLayoutRect fromView:nil].origin.y);}
    Rect viewport()const {const double top=header_height()+14;return {16,top,std::max(1.,view.bounds.size.width-32),std::max(1.,view.bounds.size.height-top-16)};}
    LayoutMetrics layout_metrics()const {return {1./scale,8,96,64};}
    void initialize() {
        install_macos_png_decoder();
        delegate=[STWindowDelegate new];delegate->host=weak_from_this();
        window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1120,720)
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable|NSWindowStyleMaskFullSizeContentView
            backing:NSBackingStoreBuffered defer:NO];
        window.title=@"StormTTY";window.titleVisibility=NSWindowTitleHidden;window.titlebarAppearsTransparent=YES;
        window.releasedWhenClosed=NO;window.delegate=delegate;window.minSize=NSMakeSize(400,260);
        window.tabbingMode=NSWindowTabbingModeDisallowed;window.collectionBehavior=NSWindowCollectionBehaviorFullScreenPrimary;
        // Native unified toolbar centers the actual AppKit traffic lights. We do
        // not move, draw, hit-test or replace the system window buttons.
        NSToolbar* toolbar=[[NSToolbar alloc] initWithIdentifier:@"StormTTY.IntegratedTitlebar"];
        toolbar.showsBaselineSeparator=NO;toolbar.displayMode=NSToolbarDisplayModeIconOnly;window.toolbar=toolbar;
        if(@available(macOS 11.0,*))window.toolbarStyle=NSWindowToolbarStyleUnifiedCompact;
        backdrop=[[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0,0,1120,720)];
        backdrop.material=NSVisualEffectMaterialUnderWindowBackground;backdrop.blendingMode=NSVisualEffectBlendingModeBehindWindow;
        backdrop.state=NSVisualEffectStateFollowsWindowActiveState;backdrop.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
        window.contentView=backdrop;
        view=[[STTerminalView alloc] initWithFrame:backdrop.bounds];view->host=weak_from_this();view->marked=[NSMutableAttributedString new];
        view->markedSelection=NSMakeRange(0,0);view.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
        RendererOptions rendering;rendering.font={config.font.family,"en-US",float(config.font.size_dip),config.font.ligatures};
        rendering.background_opacity=static_cast<float>(config.appearance.background_opacity);
        // Establish layer hosting before AppKit can implicitly layer-back this
        // view through its parent hierarchy.
        renderer=std::make_unique<Renderer>((__bridge void*)view,rendering);
        [backdrop addSubview:view];
        chrome_view=[[STChromeView alloc] initWithFrame:NSMakeRect(0,0,1000,38)];
        tab_accessory=[NSTitlebarAccessoryViewController new];tab_accessory.layoutAttribute=NSLayoutAttributeRight;tab_accessory.view=chrome_view;
        [window addTitlebarAccessoryViewController:tab_accessory];
        add_button=[NSButton buttonWithTitle:@"+" target:delegate action:@selector(executeCommand:)];add_button.identifier=@"tab.new";
        add_button.bordered=NO;add_button.font=[NSFont systemFontOfSize:22 weight:NSFontWeightLight];add_button.toolTip=@"New tab (⌘T)";
        [add_button setAccessibilityLabel:@"New terminal tab"];[chrome_view addSubview:add_button];
        menu_button=[NSButton buttonWithTitle:@"⋯" target:delegate action:@selector(executeCommand:)];menu_button.identifier=@"app.command_palette";
        menu_button.bordered=NO;menu_button.font=[NSFont systemFontOfSize:20];[menu_button setAccessibilityLabel:@"Terminal commands"];[chrome_view addSubview:menu_button];
        make_menu();NSApp.delegate=delegate;
        [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:delegate selector:@selector(displayPreferencesChanged:)
            name:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification object:nil];
        appearance();create_tab();[window center];[window makeKeyAndOrderFront:nil];[window makeFirstResponder:view];
        [NSApp activateIgnoringOtherApps:YES];resize();
        if(options.smoke_test){smoke_present_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);const auto weak=weak_from_this();dispatch_after(dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC),dispatch_get_main_queue(),^{
            if(auto h=weak.lock();h&&!h->finished&&!h->closing&&!h->smoke_capture_pending)h->fail("macOS smoke timed out before process/frame/present/exit evidence");});}
    }
    void make_menu() {
        NSMenu* bar=[NSMenu new];NSMenuItem* app=[NSMenuItem new];[bar addItem:app];NSMenu* app_menu=[NSMenu new];app.submenu=app_menu;
        [app_menu addItemWithTitle:@"About StormTTY" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
        [app_menu addItem:[NSMenuItem separatorItem]];[app_menu addItemWithTitle:@"Hide StormTTY" action:@selector(hide:) keyEquivalent:@"h"];
        [app_menu addItemWithTitle:@"Quit StormTTY" action:@selector(terminate:) keyEquivalent:@"q"];
        NSMenuItem* terminal=[NSMenuItem new];terminal.title=@"Terminal";[bar addItem:terminal];NSMenu* actions=[NSMenu new];terminal.submenu=actions;
        for(const auto& command:command_registry()) {
            if(!implemented(command.id))continue;
            NSMenuItem* item=[[NSMenuItem alloc] initWithTitle:ns(command.label) action:@selector(executeCommand:) keyEquivalent:@""];
            item.identifier=ns(command.id);item.target=delegate;apply_menu_shortcut(item,command.id,config.bindings);[actions addItem:item];
        }
        NSMenuItem* window_item=[NSMenuItem new];window_item.title=@"Window";[bar addItem:window_item];
        NSMenu* windows=[NSMenu new];window_item.submenu=windows;
        [windows addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [windows addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
        [windows addItem:[NSMenuItem separatorItem]];[windows addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
        NSApp.windowsMenu=windows;NSApp.mainMenu=bar;
    }
    static bool implemented(std::string_view id) {
        return id=="app.command_palette"||id=="tab.new"||id=="tab.close"||id=="tab.next"||id=="tab.previous"||id=="tab.rename"||
            id=="tab.move_left"||id=="tab.move_right"||id=="pane.split_right"||id=="pane.split_down"||id=="pane.close"||
            id.starts_with("pane.focus_")||id=="pane.zoom"||id=="pane.new_tab"||id.starts_with("pane.resize_")||id=="pane.equalize"||
            id=="edit.copy"||id=="edit.paste"||id=="window.fullscreen"||id=="settings.open"||id=="terminal.find"||id=="pane.inspect";
    }
    void appearance() {
        if(smoke_capture_pending){appearance_pending=true;return;}
        const auto system=[NSWorkspace sharedWorkspace];
        const bool opaque=system.accessibilityDisplayShouldReduceTransparency||system.accessibilityDisplayShouldIncreaseContrast||config.appearance.backdrop==BackdropPreference::none;
        reduce_motion=config.appearance.reduced_motion==ReducedMotionPreference::enabled||
            (config.appearance.reduced_motion==ReducedMotionPreference::system&&system.accessibilityDisplayShouldReduceMotion);
        // These shared preferences select native Mac materials, not Windows
        // Mica/Acrylic implementations. Accessibility policy always wins.
        NSVisualEffectMaterial material=NSVisualEffectMaterialUnderWindowBackground;
        switch(config.appearance.backdrop){
        case BackdropPreference::mica:material=NSVisualEffectMaterialWindowBackground;break;
        case BackdropPreference::automatic:case BackdropPreference::acrylic:case BackdropPreference::none:break;
        }
        backdrop.material=opaque?NSVisualEffectMaterialWindowBackground:material;
        backdrop.state=opaque?NSVisualEffectStateInactive:NSVisualEffectStateFollowsWindowActiveState;
        const auto bg=color(config.theme.background,{23,25,29,255});
        const double luminance=.2126*bg.r+.7152*bg.g+.0722*bg.b;
        window.appearance=[NSAppearance appearanceNamed:luminance<128?NSAppearanceNameDarkAqua:NSAppearanceNameAqua];
        window.opaque=opaque;window.backgroundColor=opaque?[NSColor colorWithSRGBRed:bg.r/255. green:bg.g/255. blue:bg.b/255. alpha:1]:NSColor.clearColor;
        if(renderer)renderer->set_background_opacity(opaque?1.f:float(config.appearance.background_opacity));cursor_on=true;++chrome;wake();
    }
    ProcessOptions process_options()const {
        ProcessOptions process;
        if(options.smoke_test) {
            process.executable="/bin/sh";process.arguments={"-c",
                "printf '\033[2J\033[H\033[1;36mStormTTY macOS\033[0m\r\nCoreText + Metal / real POSIX PTY\r\nSTORMTTY_MAC_READY\r\n'; "
                "IFS= read -r line; if [ \"$line\" = stormtty-smoke-input ]; then printf '\033[32mSTORMTTY_MAC_OK\033[0m\r\n'; exit 0; else exit 19; fi"};
            process.environment={{"ENV",std::nullopt},{"BASH_ENV",std::nullopt},{"ZDOTDIR",std::nullopt},{"PROMPT_COMMAND",std::nullopt}};
            process.working_directory="/";
        } else {
            const auto profile=std::find_if(config.profiles.begin(),config.profiles.end(),[&](const Profile& p){return p.id==config.default_profile;});
            if(profile==config.profiles.end())throw std::runtime_error("Default shell profile is missing");
            if(profile->executable.empty()||profile->executable.front()!='/')throw std::runtime_error("macOS profile executable must be an absolute path; no shell interpolation or PATH search");
            process.executable=profile->executable;process.arguments=profile->arguments;process.working_directory=profile->working_directory;
            for(const auto& [key,value]:profile->environment)process.environment.push_back({key,value});
        }
        process.environment.push_back({"TERM",std::string("xterm-256color")});process.environment.push_back({"COLORTERM",std::string("truecolor")});
        return process;
    }
    SessionId start_session() {
        const SessionId id{next_session++};TerminalOptions terminal;terminal.scrollback_lines=config.scrollback_lines;terminal.resize_pull_scrollback=true;
        terminal.default_foreground=color(config.theme.foreground,{236,238,242,255});
        terminal.ansi.emplace();
        for (std::size_t i=0;i<16;++i) (*terminal.ansi)[i]=color(config.theme.ansi[i],{0,0,0,255});
        terminal.default_background=color(config.theme.background,{23,25,29,255});
        const auto metrics=renderer->metrics();terminal.size.cell_width=metrics.width;terminal.size.cell_height=metrics.height;
        auto weak=weak_from_this();auto session=Session::start(process_options(),terminal,[weak]{if(auto h=weak.lock())h->wake();});
        sessions.emplace(id,std::move(session));return id;
    }
    void create_tab() {
        const auto id=start_session();auto created=workspace.create_tab(id,config.default_profile);
        if(!created){sessions.at(id)->request_close();throw std::runtime_error(created.error);}
        tabs();resize();
    }
    void tabs() {
        for(NSButton* button:tab_buttons)[button removeFromSuperview];tab_buttons.clear();
        const auto& state=workspace.state();const double available=std::max(120.,chrome_view.bounds.size.width-84.);
        const double width=std::clamp(available/std::max(std::size_t(1),state.tabs.size()),80.,210.);
        double x=0;const double y=std::max(0.,(chrome_view.bounds.size.height-30)/2);chrome_view->selectedTab=nil;
        for(const auto& tab:state.tabs) {
            std::string title=tab.title;
            if(title.empty()){auto pane=workspace.pane(tab.active_pane);if(pane){auto s=sessions.find(pane->session);if(s!=sessions.end())title=plain_title(s->second->info().title);}}
            if(title.empty())title="Terminal";
            NSButton* button=[NSButton buttonWithTitle:ns(title) target:delegate action:@selector(selectTab:)];button.tag=static_cast<NSInteger>(tab.id.value);
            button.bordered=NO;button.font=[NSFont systemFontOfSize:12 weight:tab.id==state.active_tab?NSFontWeightSemibold:NSFontWeightRegular];
            button.contentTintColor=tab.id==state.active_tab?NSColor.labelColor:NSColor.secondaryLabelColor;
            button.frame=NSMakeRect(x,y,width-4,30);button.toolTip=ns(title);[button setAccessibilityLabel:[@"Terminal tab: " stringByAppendingString:ns(title)]];
            [button setAccessibilityValue:tab.id==state.active_tab?@"Selected":@""];[chrome_view addSubview:button];tab_buttons.push_back(button);if(tab.id==state.active_tab)chrome_view->selectedTab=button;x+=width;
            if(x>chrome_view.bounds.size.width-100)break; // Remaining tabs remain reachable with Ctrl+Tab/menu.
        }
        add_button.frame=NSMakeRect(std::min(x+4,chrome_view.bounds.size.width-76.),y,30,30);menu_button.frame=NSMakeRect(chrome_view.bounds.size.width-38,y,30,30);
        const auto accent=color(config.theme.accent,{167,196,210,255});chrome_view->selectionColor=[NSColor colorWithSRGBRed:accent.r/255. green:accent.g/255. blue:accent.b/255. alpha:1];chrome_view.needsDisplay=YES;
        ++chrome;
    }
    void resize() {
        if(smoke_capture_pending){resize_pending=true;return;}
        if(!renderer||!view)return;
        scale=window.backingScaleFactor>0?window.backingScaleFactor:1;
        // Derive the reserved traffic-light area from AppKit, not Windows or
        // guessed caption metrics. Accessory width is the supported mutable axis.
        double controls_right=0;for(NSWindowButton which:{NSWindowCloseButton,NSWindowMiniaturizeButton,NSWindowZoomButton}){
            NSButton* button=[window standardWindowButton:which];if(button)controls_right=std::max(controls_right,NSMaxX([button.superview convertRect:button.frame toView:nil]));}
        [chrome_view setFrameSize:NSMakeSize(std::max(180.,view.bounds.size.width-controls_right-32),chrome_view.frame.size.height)];
        pixel_width=static_cast<std::uint32_t>(std::max(0.,std::round(view.bounds.size.width*scale)));
        pixel_height=static_cast<std::uint32_t>(std::max(0.,std::round(view.bounds.size.height*scale)));
        renderer->resize(pixel_width,pixel_height,float(scale*96));
        layout=workspace.layout(workspace.active_tab(),viewport(),layout_metrics());
        const auto metrics=renderer->metrics();
        for(const auto& p:layout.panes) {
            TerminalSize size;size.columns=static_cast<std::uint16_t>(std::clamp(std::floor(p.bounds.width*scale/metrics.width),1.,1000.));
            size.rows=static_cast<std::uint16_t>(std::clamp(std::floor(p.bounds.height*scale/metrics.height),1.,1000.));
            size.cell_width=metrics.width;size.cell_height=metrics.height;
            if(sent_sizes.contains(p.session)&&sent_sizes.at(p.session)==size)continue;
            if(auto found=sessions.find(p.session);found!=sessions.end()){
                auto session=found->second;enqueue([session,size]{return session->request_resize(size);},sizeof(size));sent_sizes[p.session]=size;
            }
        }
        tabs();[view.inputContext invalidateCharacterCoordinates];wake();
    }
    void wake() noexcept {
        if(wake_queued.exchange(true))return;
        auto weak=weak_from_this();dispatch_async(dispatch_get_main_queue(),^{if(auto h=weak.lock()){h->wake_queued.store(false);h->guard([&]{h->update();});}});
    }
    template<class F> void guard(F&& action) noexcept {
        try {action();}catch(const std::exception& e){fail(e.what());}catch(...){fail("Unexpected macOS host failure");}
    }
    void fail(std::string reason) noexcept {
        try {
            if(finished)return;message=std::move(reason);exit_code=1;
            std::fprintf(stderr,"StormTTY macOS: %s\n",message.c_str());
            if(!options.smoke_test&&!closing){NSAlert* alert=[NSAlert new];alert.messageText=@"StormTTY could not continue";alert.informativeText=ns(message);[alert runModal];}
            begin_close(true);
        }catch(...){exit_code=1;finish();}
    }
    void enqueue(std::function<SessionSendResult()> send,std::size_t bytes=64) {
        if(closing)return;
        if(bytes>1024u*1024u-pending_bytes){message="Input queue is full; this new action was not sent.";NSBeep();++chrome;wake();return;}
        pending.push_back({std::move(send),bytes});pending_bytes+=bytes;flush_input();
    }
    void flush_input() {
        while(!pending.empty()) {
            const auto result=pending.front().send();if(result==SessionSendResult::would_block)return;
            if(result!=SessionSendResult::accepted){message=result==SessionSendResult::too_large?"Input was too large and was not sent.":"Session has exited; input was not sent.";++chrome;}
            pending_bytes-=pending.front().bytes;pending.pop_front();
        }
    }
    void text(std::string value,NSEvent* event,bool composition) {
        auto session=active();if(!session||value.empty())return;const auto bytes=value.size();
        if(event&&!composition){KeyInput key;key.key=physical_key(event.keyCode);key.action=event.isARepeat?KeyAction::repeat:KeyAction::press;
            key.modifiers=modifiers(event.modifierFlags);key.consumed_modifiers=key.modifiers&(Modifier::shift|Modifier::alt);key.text=std::move(value);
            enqueue([session,key=std::move(key)]{return session->key(key);},bytes+sizeof(key));
        }else enqueue([session,value=std::move(value)]{return session->text(value);},bytes);
        cursor_on=true;wake();
    }
    void key(NSEvent* event,Key override_key=Key::unidentified,KeyAction action=KeyAction::press) {
        auto session=active();if(!session)return;KeyInput input;input.key=override_key==Key::unidentified?physical_key(event.keyCode):override_key;
        input.action=action==KeyAction::press&&event.isARepeat?KeyAction::repeat:action;input.modifiers=modifiers(event.modifierFlags);
        NSString* unshifted=event.charactersIgnoringModifiers;if(unshifted.length==1)input.unshifted_codepoint=[unshifted characterAtIndex:0];
        enqueue([session,input]{return session->key(input);});cursor_on=true;wake();
    }
    CommandContext context()const {
        CommandContext result;result.supports_pane_context=true;result.tabs=workspace.state().tabs.size();result.panes=workspace.pane_order(workspace.active_tab()).size();
        result.total_panes=workspace.state().panes.size();auto tab=workspace.tab(workspace.active_tab());result.zoomed=tab&&tab->zoomed_pane.has_value();
        if(auto session=active()){const auto info=session->info();result.session_running=info.state==SessionState::running||info.state==SessionState::starting;
            if(auto frame=session->frame())for(const auto& row:frame->lines)if(row)for(const auto& cell:row->cells)result.has_selection|=cell.selected;}
        result.clipboard_has_text=[[NSPasteboard generalPasteboard] availableTypeFromArray:@[NSPasteboardTypeString]]!=nil;return result;
    }
    bool command_key(NSEvent* event) {
        auto found=resolve_command({chord(event),view.hasMarkedText,false,true},config.bindings,context(),Platform::macos);
        if(!found)return false;if(!implemented(*found)){message="This command is not implemented on macOS yet.";++chrome;wake();return true;}
        command(*found);return true;
    }
    bool confirm_close(NSString* title) {
        if(options.smoke_test)return true;
        if(confirming_close)return false;
        confirming_close=true;struct Reset {bool& flag;~Reset(){flag=false;}} reset{confirming_close};
        bool running=false;for(const auto& [id,s]:sessions){(void)id;if(!s->closed()){running=true;break;}}
        if(!running)return true;
        NSAlert* alert=[NSAlert new];alert.messageText=title;alert.informativeText=@"This closes the terminal session and may stop processes running in it.";
        [alert addButtonWithTitle:@"Cancel"];[alert addButtonWithTitle:@"Close"];
        return [alert runModal]==NSAlertSecondButtonReturn;
    }
    void reap_removed() {
        std::set<SessionId> retained;for(const auto& [id,pane]:workspace.state().panes){(void)id;retained.insert(pane.session);}
        for(auto& [id,session]:sessions)if(!retained.contains(id))session->request_close();
    }
    void command(std::string_view id) {
        if(closing||smoke_capture_pending||window.attachedSheet)return;
        if(!command_availability(id,context()).enabled)return;
        if(id=="tab.new"){create_tab();return;}
        if(id=="pane.split_right"||id=="pane.split_down"){
            auto session=start_session();auto result=workspace.split(workspace.active_pane(),id=="pane.split_right"?Direction::right:Direction::down,session,config.default_profile);
            if(!result){sessions.at(session)->request_close();throw std::runtime_error(result.error);}
        }else if(id=="pane.close"||id=="tab.close"){
            if(!confirm_close(id=="pane.close"?@"Close this terminal pane?":@"Close this terminal tab?"))return;
            if(id=="pane.close")workspace.close_pane(workspace.active_pane());else workspace.close_tab(workspace.active_tab());reap_removed();
            if(workspace.state().tabs.empty()){begin_close(true);return;}
        }else if(id=="tab.next"||id=="tab.previous"||id=="tab.move_left"||id=="tab.move_right"){
            const auto& tabs=workspace.state().tabs;auto current=std::find_if(tabs.begin(),tabs.end(),[&](const Tab& t){return t.id==workspace.active_tab();});
            const auto index=static_cast<std::size_t>(current-tabs.begin());const bool previous=id=="tab.previous"||id=="tab.move_left";
            const auto next=previous?(index+tabs.size()-1)%tabs.size():(index+1)%tabs.size();
            if(id.starts_with("tab.move_"))workspace.reorder_tab(workspace.active_tab(),next);else workspace.activate_tab(tabs[next].id);
        }else if(id=="pane.zoom")workspace.toggle_zoom();
        else if(id=="pane.new_tab")workspace.move_pane_to_new_tab(workspace.active_pane());
        else if(id.starts_with("pane.focus_")){
            const auto direction=id.ends_with("left")?Direction::left:id.ends_with("right")?Direction::right:id.ends_with("up")?Direction::up:Direction::down;
            workspace.focus_direction(direction,viewport(),layout_metrics());
        }else if(id.starts_with("pane.resize_")||id=="pane.equalize"){
            const auto pane=std::find_if(layout.panes.begin(),layout.panes.end(),[&](const auto& p){return p.pane==workspace.active_pane();});
            if(pane!=layout.panes.end())for(auto it=layout.dividers.rbegin();it!=layout.dividers.rend();++it){
                if(!it->split_bounds.contains({pane->bounds.x+pane->bounds.width/2,pane->bounds.y+pane->bounds.height/2}))continue;
                if(id=="pane.equalize"){workspace.equalize_split(it->node);break;}
                const bool horizontal=id.ends_with("left")||id.ends_with("right");if((it->axis==Axis::horizontal)!=horizontal)continue;
                const double amount=(id.ends_with("left")||id.ends_with("up"))?-.05:.05;
                workspace.resize_split(it->node,it->applied_ratio+amount,viewport(),layout_metrics());break;
            }
        }else if(id=="edit.copy"){
            if(auto session=active()){auto weak=weak_from_this();enqueue([session,weak]{return session->copy_selection([weak](std::string value){
                dispatch_async(dispatch_get_main_queue(),^{if(auto h=weak.lock();h&&!h->closing){auto board=[NSPasteboard generalPasteboard];[board clearContents];[board setString:ns(value) forType:NSPasteboardTypeString];}});
            });});}return;
        }else if(id=="edit.paste"){
            auto session=active();NSString* value=[[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];if(!session||!value)return;
            if([value lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>1024u*1024u){message="Paste exceeds the 1 MiB limit.";++chrome;wake();return;}
            auto bytes=utf8(value);bool unsafe=!TerminalCore::paste_is_safe(bytes)||bytes.find('\n')!=std::string::npos||bytes.find('\r')!=std::string::npos;
            if(unsafe){NSAlert* alert=[NSAlert new];alert.messageText=@"Paste commands or control characters?";
                alert.informativeText=@"This text contains multiple lines or terminal controls and could execute commands. Only continue if you trust its source.";
                [alert addButtonWithTitle:@"Cancel"];[alert addButtonWithTitle:@"Paste"];
                if([alert runModal]!=NSAlertSecondButtonReturn)return;}
            const auto size=bytes.size();enqueue([session,bytes=std::move(bytes),unsafe]{return session->paste(bytes,unsafe);},size);return;
        }else if(id=="window.fullscreen"){[window toggleFullScreen:nil];return;}
        else if(id=="tab.rename"){
            NSAlert* alert=[NSAlert new];alert.messageText=@"Rename terminal tab";NSTextField* field=[[NSTextField alloc] initWithFrame:NSMakeRect(0,0,300,24)];
            field.stringValue=ns(workspace.tab(workspace.active_tab())->title);alert.accessoryView=field;[alert addButtonWithTitle:@"Rename"];[alert addButtonWithTitle:@"Cancel"];
            if([alert runModal]==NSAlertFirstButtonReturn)workspace.rename_tab(workspace.active_tab(),plain_title(utf8(field.stringValue)));
        }else if(id=="app.command_palette"){
            NSMenu* menu=[NSMenu new];for(const auto& command:command_registry())if(implemented(command.id)&&command.id!="app.command_palette"){
                NSMenuItem* item=[[NSMenuItem alloc] initWithTitle:ns(command.label) action:@selector(executeCommand:) keyEquivalent:@""];item.target=delegate;item.identifier=ns(command.id);
                apply_menu_shortcut(item,command.id,config.bindings);
                item.enabled=command_availability(command.id,context()).enabled;[menu addItem:item];}
            [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(view.bounds.size.width-30,44) inView:view];return;
        }else if(id=="pane.inspect"){
            // Do not steal AppKit marked text or nest a modal inspector. Capture
            // metadata once, without polling the shell or touching PTY ownership.
            if(!menu_commands_available(view.hasMarkedText,closing,NSApp.modalWindow!=nil))return;
            const auto* pane=workspace.pane(workspace.active_pane());const auto session=active();
            if(!pane||!session)return;
            const auto info=session->info();
            const auto profile=std::find_if(config.profiles.begin(),config.profiles.end(),[&](const Profile& p){return p.id==pane->profile;});
            const std::string_view label=profile==config.profiles.end()?std::string_view(pane->profile):std::string_view(profile->label);
            const std::string_view start=options.smoke_test?std::string_view("/"):profile==config.profiles.end()?std::string_view{}:std::string_view(profile->working_directory);
            // This host launches ordinary profiles, not the Windows managed shell.
            const auto snapshot=make_pane_context({info.title,label,info.working_directory,start,
                info.shell_context?&*info.shell_context:nullptr,false,
                info.state==SessionState::exited||info.state==SessionState::failed});
            STPaneInspector* inspector=[[STPaneInspector alloc] initWithSnapshot:snapshot];
            auto weak=weak_from_this();
            [inspector presentForWindow:window completion:^{if(auto h=weak.lock())
                h->guard([&]{if(!h->closing)[h->window makeFirstResponder:h->view];});}];
            return;
        }else if(id=="terminal.find"){
            NSAlert* alert=[NSAlert new];alert.messageText=@"Find in visible terminal";NSTextField* field=[[NSTextField alloc] initWithFrame:NSMakeRect(0,0,300,24)];
            alert.accessoryView=field;[alert addButtonWithTitle:@"Find"];[alert addButtonWithTitle:@"Cancel"];
            if([alert runModal]==NSAlertFirstButtonReturn){const auto needle=utf8(field.stringValue);auto session=active();
                message=session&&!needle.empty()&&frame_text(session->frame()).find(needle)!=std::string::npos?"Match found in visible terminal text.":"No visible match. Scrollback-wide search is not implemented.";++chrome;wake();}return;
        }else if(id=="settings.open"){
            NSAlert* alert=[NSAlert new];alert.messageText=@"StormTTY settings";
            alert.informativeText=@"Declarative configuration: ~/Library/Application Support/StormTTY/config.stormtty\n\nRestart after editing, or launch with --config /absolute/path. In-app editing and live reload are not available yet.";[alert runModal];return;
        }
        focus_changed();resize();
    }
    void focus_changed() {
        for(const auto& [id,session]:sessions){if(session->closed())continue;const auto pane=workspace.pane(workspace.active_pane());const bool active=pane&&pane->session==id&&window.keyWindow;
            enqueue([session,active]{return session->focus(active);});}
        cursor_on=true;++chrome;[view.inputContext discardMarkedText];[view unmarkText];[window makeFirstResponder:view];wake();
    }
    void select_tab(std::uint64_t id){workspace.activate_tab(TabId{id});focus_changed();resize();}
    void mouse(NSEvent* event,MouseAction action) {
        const NSPoint position=[view convertPoint:event.locationInWindow fromView:nil];const Point point{position.x,position.y};
        if(action==MouseAction::press){
            if(position.y<header_height()){[window performWindowDragWithEvent:event];return;}
            auto hit=hit_test(layout,point);
            if(hit.kind==HitKind::divider){dragged_divider=hit.divider;if(event.clickCount==2){workspace.equalize_split(hit.divider);dragged_divider={};resize();}return;}
            if(hit.kind==HitKind::pane){workspace.focus(hit.pane);focus_changed();}
        }
        if(dragged_divider){if(action==MouseAction::release)dragged_divider={};else{
            auto divider=std::find_if(layout.dividers.begin(),layout.dividers.end(),[&](const auto& d){return d.node==dragged_divider;});
            if(divider!=layout.dividers.end()){const bool horizontal=divider->axis==Axis::horizontal;
                const double length=horizontal?divider->split_bounds.width:divider->split_bounds.height;
                const double offset=horizontal?point.x-divider->split_bounds.x:point.y-divider->split_bounds.y;
                workspace.resize_split(divider->node,offset/std::max(1.,length),viewport(),layout_metrics());resize();}}return;}
        auto place=std::find_if(layout.panes.begin(),layout.panes.end(),[&](const auto& p){return p.pane==workspace.active_pane();});
        auto session=active();if(place==layout.panes.end()||!session)return;auto frame=session->frame();if(!frame)return;
        const double x=(point.x-place->bounds.x)*scale,y=(point.y-place->bounds.y)*scale;
        if(frame->mouse_reporting&&!(event.modifierFlags&NSEventModifierFlagShift)){
            MouseInput input{action,event.buttonNumber==1?MouseButton::right:event.buttonNumber==2?MouseButton::middle:MouseButton::left,modifiers(event.modifierFlags),x,y,action!=MouseAction::release};
            enqueue([session,input]{return session->mouse(input);});return;
        }
        const auto cx=static_cast<std::uint16_t>(std::clamp(std::floor(x/std::max(1u,unsigned(frame->cell_width))),0.,double(std::max(1u,unsigned(frame->columns))-1)));
        const auto cy=static_cast<std::uint16_t>(std::clamp(std::floor(y/std::max(1u,unsigned(frame->cell_height))),0.,double(std::max(1u,unsigned(frame->rows))-1)));
        if(action==MouseAction::press){selection_pane=place->pane;selection_x=cx;selection_y=cy;enqueue([session]{return session->clear_selection();});}
        else if(selection_pane==place->pane){const auto sx=selection_x,sy=selection_y;const bool rectangle=event.modifierFlags&NSEventModifierFlagOption;
            enqueue([session,sx,sy,cx,cy,rectangle]{return session->select(sx,sy,cx,cy,rectangle);});if(action==MouseAction::release)selection_pane={};}
    }
    void scroll(NSEvent* event) {
        auto session=active();if(!session)return;auto frame=session->frame();if(!frame)return;
        if(frame->mouse_reporting&&!(event.modifierFlags&NSEventModifierFlagShift)){
            auto place=std::find_if(layout.panes.begin(),layout.panes.end(),[&](const auto& p){return p.pane==workspace.active_pane();});if(place==layout.panes.end())return;
            const auto position=[view convertPoint:event.locationInWindow fromView:nil];
            MouseInput input{MouseAction::press,event.scrollingDeltaY>0?MouseButton::wheel_up:MouseButton::wheel_down,modifiers(event.modifierFlags),(position.x-place->bounds.x)*scale,(position.y-place->bounds.y)*scale,false};
            if(event.scrollingDeltaY!=0)enqueue([session,input]{return session->mouse(input);});
        }else {const double delta=event.hasPreciseScrollingDeltas?event.scrollingDeltaY/8:event.scrollingDeltaY;
            const auto rows=static_cast<std::int64_t>(std::round(delta));if(rows)enqueue([session,rows]{return session->scroll(rows);});}
    }
    NSRect cursor_rect()const {
        auto session=active();auto frame=session?session->frame():nullptr;
        auto placement=std::find_if(layout.panes.begin(),layout.panes.end(),[&](const auto& p){return p.pane==workspace.active_pane();});
        if(!frame||placement==layout.panes.end())return NSMakeRect(16,60,8,18);
        return NSMakeRect(placement->bounds.x+frame->cursor.column*frame->cell_width/scale,placement->bounds.y+frame->cursor.row*frame->cell_height/scale,
            std::max(1.,frame->cell_width/scale),std::max(1.,frame->cell_height/scale));
    }
    void update() {
        if(finished)return;
        if(update_active){update_again=true;return;}
        update_active=true;
        struct UpdateScope {Host& host;~UpdateScope(){host.update_active=false;if(std::exchange(host.update_again,false))host.wake();}} scope{*this};
        flush_input();
        if(closing){if(smoke_capture_pending)return;bool closed=true;for(const auto& [id,s]:sessions){(void)id;closed&=s->closed();}if(closed)finish();return;}
        if(smoke_capture_pending)return;
        if(std::exchange(appearance_pending,false))appearance();
        if(std::exchange(resize_pending,false))resize();
        std::set<SessionId> retained;for(const auto& [id,pane]:workspace.state().panes){(void)id;retained.insert(pane.session);}
        for(auto it=sessions.begin();it!=sessions.end();)if(!retained.contains(it->first)&&it->second->closed()){sent_sizes.erase(it->first);it=sessions.erase(it);}else ++it;
        if(options.smoke_test){auto session=active();if(!session)throw std::runtime_error("Smoke session missing");auto info=session->info();
            if(info.state==SessionState::failed)throw std::runtime_error(info.error);
            auto content=frame_text(session->frame());
            if(!smoke_input&&content.find("STORMTTY_MAC_READY")!=std::string::npos){smoke_input=true;
                enqueue([session]{return session->text("stormtty-smoke-input");});KeyInput enter;enter.key=Key::enter;enqueue([session,enter]{return session->key(enter);});}
            smoke_seen=content.find("STORMTTY_MAC_OK")!=std::string::npos;
            if(session->closed()&&(!info.exit_code||*info.exit_code!=0||!smoke_seen))throw std::runtime_error("Smoke child exited without successful input/terminal marker");
        }
        bool titles_changed=false;
        for(std::size_t i=0;i<tab_buttons.size()&&i<workspace.state().tabs.size();++i){const auto& tab=workspace.state().tabs[i];std::string title=tab.title;
            if(title.empty()){auto pane=workspace.pane(tab.active_pane);if(pane){auto found=sessions.find(pane->session);if(found!=sessions.end())title=plain_title(found->second->info().title);}}
            if(title.empty())title="Terminal";if(utf8(tab_buttons[i].title)!=title){titles_changed=true;break;}}
        if(titles_changed)tabs();
        WindowScene scene;scene.force_redraw=smoke_force_present;scene.background=color(config.theme.background,{23,25,29,255});scene.chrome_generation=chrome;
        const auto accent=color(config.theme.accent,{167,196,210,255});bool blinks=false;
        for(const auto& placement:layout.panes){auto found=sessions.find(placement.session);if(found==sessions.end())continue;
            auto frame=found->second->frame();if(!frame)continue;const auto& bounds=placement.bounds;
            scene.panes.push_back({placement.session.value,{float(bounds.x*scale),float(bounds.y*scale),float(bounds.width*scale),float(bounds.height*scale)},frame,placement.pane==workspace.active_pane()&&window.keyWindow,reduce_motion||cursor_on,reduce_motion||cursor_on});
            blinks|=frame->cursor.visible&&frame->cursor.blinking;
            for(const auto& row:frame->lines)if(row)for(const auto& cell:row->cells)blinks|=cell.style.blink;
            if(placement.pane==workspace.active_pane()&&layout.panes.size()>1)scene.primitives.push_back({{float(bounds.x*scale),float((bounds.y-5)*scale),float(std::min(28.,bounds.width)*scale),float(2*scale)},accent});
            const auto info=found->second->info();if(!info.error.empty()&&message!=plain_title(info.error)){message=plain_title(info.error);scene.chrome_generation=++chrome;}
        }
        for(const auto& divider:layout.dividers){const auto& r=divider.bounds;scene.primitives.push_back({{float(r.x*scale),float(r.y*scale),float(r.width*scale),float(r.height*scale)},{62,67,75,255}});}
        // The material is confined to the default canvas/chrome. Glyphs and
        // explicit application backgrounds always remain independently opaque.
        if(header_height()>0)scene.primitives.push_back({{0,float((header_height()-1)*scale),float(pixel_width),float(scale)},{54,59,66,255}});
        if(view.hasMarkedText){const auto r=cursor_rect();const auto width=std::max(80.,std::min(400.,double(view->marked.length)*config.font.size_dip));
            scene.primitives.push_back({{float(r.origin.x*scale),float(r.origin.y*scale),float(width*scale),float(r.size.height*scale)},{43,49,57,255},{},0,0,ChromePrimitiveKind::rectangle,false,{ChromeLayer::transient}});
            scene.texts.push_back({{float(r.origin.x*scale),float(r.origin.y*scale),float(width*scale),float(r.size.height*scale)},utf8(view->marked.string),{236,238,242,255},float(config.font.size_dip),false,"Menlo",ChromeTextAlignment::leading,{ChromeLayer::transient}});}
        if(!message.empty())scene.texts.push_back({{float(16*scale),float(pixel_height-22*scale),float(pixel_width-32*scale),float(18*scale)},message,{222,192,146,255},11,false,"Segoe UI Variable Text",ChromeTextAlignment::leading,{ChromeLayer::toast}});
        auto result=renderer->render(scene);
        if(result==RenderResult::busy&&!retry_scheduled){retry_scheduled=true;auto weak=weak_from_this();dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC/60),dispatch_get_main_queue(),^{if(auto h=weak.lock()){h->retry_scheduled=false;h->wake();}});}
        if(result==RenderResult::presented){smoke_force_present=false;if(auto s=active()){auto frame=s->frame();if(frame&&frame->generation!=accessibility_generation){accessibility_generation=frame->generation;NSAccessibilityPostNotification(view,NSAccessibilityValueChangedNotification);}}
            if(smoke_seen)smoke_presented=true;}
        if(!reduce_motion&&blinks&&window.keyWindow&&(window.occlusionState&NSWindowOcclusionStateVisible)&&!blink_scheduled){blink_scheduled=true;auto weak=weak_from_this();dispatch_after(dispatch_time(DISPATCH_TIME_NOW,600*NSEC_PER_MSEC),dispatch_get_main_queue(),^{
            if(auto h=weak.lock()){h->blink_scheduled=false;if(!h->closing){h->cursor_on=!h->cursor_on;h->wake();}}});}
        if(options.smoke_test&&smoke_presented&&active()->closed()&&!smoke_capture_pending){
            smoke_capture_pending=true;
            // Readback waits for the actual Metal command buffer only on a
            // background thread, never in an AppKit callback. Renderer lifetime
            // remains owned by Host; capture_pending gates every UI renderer
            // operation until this worker reports completion on the main queue.
            // Give the real main run loop a committed, attached layer before
            // asking a background worker to observe its presentation callback.
            [view layoutSubtreeIfNeeded];[window displayIfNeeded];[CATransaction flush];
            const auto screen=window.screen;NSNumber* screen_number=screen.deviceDescription[@"NSScreenNumber"];
            const auto display=static_cast<CGDirectDisplayID>(screen_number.unsignedIntValue);
            std::ostringstream context;
            context<<"windowVisible="<<bool([window isVisible])<<" key="<<bool([window isKeyWindow])<<" appActive="<<bool([NSApp isActive])
                <<" miniaturized="<<bool([window isMiniaturized])<<" occlusion="<<static_cast<unsigned long long>(window.occlusionState)
                <<" viewHidden="<<bool([view isHiddenOrHasHiddenAncestor])<<" screens="<<NSScreen.screens.count
                <<" hasWindowScreen="<<(screen!=nil)<<" displayOnline="<<(screen?CGDisplayIsOnline(display):0)
                <<" displayActive="<<(screen?CGDisplayIsActive(display):0)<<" windowFrame="<<utf8(NSStringFromRect(window.frame))
                <<" viewBounds="<<utf8(NSStringFromRect(view.bounds));
            const auto visibility=context.str();std::fprintf(stdout,"macOS smoke presentation context: %s\n",visibility.c_str());std::fflush(stdout);
            auto self=shared_from_this();
            const auto capture_width=pixel_width,capture_height=pixel_height;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
                @autoreleasepool { try {auto pixels=self->renderer->read_pixels();if(pixels.empty())throw std::runtime_error("Smoke Metal framebuffer is empty");
                    if(!self->options.capture_path.empty())write_macos_capture(self->options.capture_path,pixels,capture_width,capture_height);
                    dispatch_async(dispatch_get_main_queue(),^{self->guard([&]{self->smoke_capture_pending=false;std::fprintf(stdout,"PASS macOS AppKit + POSIX PTY + Session + TerminalCore + CoreText + Metal framebuffer + exit\n");self->begin_close(true);self->wake();});});
                }catch(const std::exception& e){const auto error=std::string(e.what());
                    dispatch_async(dispatch_get_main_queue(),^{self->guard([&]{
                        // Dropped first drawables do not establish a displayed
                        // frame. The completed worker releases its exclusive
                        // readback phase before another main-run-loop present.
                        // Request another real present within the observation
                        // deadline; never accept a zero timestamp or fake/skip.
                        self->smoke_capture_pending=false;
                        if(error.starts_with("Metal drawable was not observed presented onscreen")&&std::chrono::steady_clock::now()<self->smoke_present_deadline&&!self->closing){
                            std::fprintf(stderr,"macOS smoke re-presentation after observer timeout: %s; %s\n",error.c_str(),visibility.c_str());
                            self->smoke_presented=false;self->smoke_force_present=true;
                            // Do not synchronously redisplay/order the window
                            // inside a readback completion callback. Its native
                            // layout/delegate callbacks may be reentrant. The
                            // existing visible window gets the next real Metal
                            // present from an ordinary main-loop wake instead.
                            // self is a reference captured by the enclosing C++
                            // guard lambda. A nested escaping Block would copy
                            // that reference, not retain its shared_ptr owner.
                            // A local value gives Blocks an owning C++ capture.
                            const auto retry_host=self;
                            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),dispatch_get_main_queue(),^{retry_host->wake();});
                        }else{self->fail(error+"; "+visibility);self->wake();}
                    });});
                }catch(...){dispatch_async(dispatch_get_main_queue(),^{self->smoke_capture_pending=false;self->fail("Unexpected macOS smoke readback failure");self->wake();});} }
            });
        }
    }
    void begin_close(bool authorized=false) {
        if(closing||finished)return;
        // Finish the read-only sheet before opening a separate close prompt.
        if(window.attachedSheet&&!authorized){[window.attachedSheet makeKeyAndOrderFront:nil];return;}
        if(!authorized&&!confirm_close(@"Close StormTTY and its terminal sessions?"))return;
        closing=true;close_authorized=true;pending.clear();pending_bytes=0;
        if(window.attachedSheet)[window endSheet:window.attachedSheet returnCode:NSModalResponseCancel];
        for(auto& [id,s]:sessions){(void)id;s->request_close();}[window orderOut:nil];wake();
    }
    void finish() noexcept {
        if(finished)return;finished=true;[[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:delegate];
        [NSApp stop:nil];[NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
    }
};
}

@implementation STChromeView
- (BOOL)isFlipped{return YES;}
- (NSView*)hitTest:(NSPoint)point{NSView* hit=[super hitTest:point];return hit==self?nil:hit;}
- (void)drawRect:(NSRect)dirty{(void)dirty;if(selectedTab){[selectionColor setFill];const auto r=selectedTab.frame;NSRectFill(NSMakeRect(r.origin.x+12,self.bounds.size.height-3,std::max(0.,r.size.width-24),2));}}
@end

@implementation STTerminalView
- (BOOL)isFlipped{return YES;}
- (BOOL)acceptsFirstResponder{return YES;}
- (BOOL)acceptsFirstMouse:(NSEvent*)event{(void)event;return YES;}
- (BOOL)mouseDownCanMoveWindow{return NO;}
- (BOOL)performKeyEquivalent:(NSEvent*)event{auto h=host.lock();if(!h)return NO;BOOL handled=NO;h->guard([&]{handled=h->command_key(event);});return handled;}
- (void)keyDown:(NSEvent*)event{if(auto h=host.lock())h->guard([&]{
    if(h->command_key(event))return;typingEvent=event;composingEvent=self.hasMarkedText;
    if(!self.hasMarkedText&&(stormtty::macos::direct_key(stormtty::macos::physical_key(event.keyCode))||(event.modifierFlags&(NSEventModifierFlagControl|NSEventModifierFlagCommand)))){pressedKeys.insert(event.keyCode);h->key(event);}
    else [self interpretKeyEvents:@[event]];typingEvent=nil;
});}
- (void)keyUp:(NSEvent*)event{if(!pressedKeys.erase(event.keyCode))return;if(auto h=host.lock();h&&!self.hasMarkedText)h->guard([&]{h->key(event,stormtty::Key::unidentified,stormtty::KeyAction::release);});}
- (void)insertText:(id)value replacementRange:(NSRange)range{(void)range;if(auto h=host.lock())h->guard([&]{
    NSString* text=[value isKindOfClass:NSAttributedString.class]?[value string]:value;
    if([text lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>1024u*1024u){h->message="Committed text exceeds the 1 MiB input limit.";++h->chrome;h->wake();NSBeep();return;}
    const bool composition=composingEvent||self.hasMarkedText;[marked setAttributedString:[[NSAttributedString alloc] initWithString:@""]];markedSelection=NSMakeRange(0,0);
    if(typingEvent&&!composition)pressedKeys.insert(typingEvent.keyCode);
    h->text(stormtty::macos::utf8(text),typingEvent,composition);[self.inputContext invalidateCharacterCoordinates];++h->chrome;h->wake();
});}
- (void)setMarkedText:(id)value selectedRange:(NSRange)selection replacementRange:(NSRange)replacement{if(auto h=host.lock())h->guard([&]{
    NSAttributedString* text=[value isKindOfClass:NSAttributedString.class]?value:[[NSAttributedString alloc] initWithString:value];
    if(text.length>4096){NSBeep();return;}
    if(replacement.location!=NSNotFound&&replacement.location<=marked.length&&replacement.length<=marked.length-replacement.location){
        if(marked.length-replacement.length+text.length>4096){NSBeep();return;}
        [marked replaceCharactersInRange:replacement withAttributedString:text];}
    else [marked setAttributedString:text];
    markedSelection=NSMakeRange(std::min(selection.location,marked.length),std::min(selection.length,marked.length-std::min(selection.location,marked.length)));
    composingEvent=YES;[self.inputContext invalidateCharacterCoordinates];++h->chrome;h->wake();
});}
- (void)unmarkText{[marked setAttributedString:[[NSAttributedString alloc] initWithString:@""]];markedSelection=NSMakeRange(0,0);if(auto h=host.lock()){++h->chrome;h->wake();}}
- (BOOL)hasMarkedText{return marked.length>0;}
- (NSRange)markedRange{return marked.length?NSMakeRange(0,marked.length):NSMakeRange(NSNotFound,0);}
- (NSRange)selectedRange{return marked.length?markedSelection:NSMakeRange(0,0);}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText{return @[NSUnderlineStyleAttributeName,NSMarkedClauseSegmentAttributeName];}
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual{
    if(range.location==NSNotFound||range.location>marked.length){if(actual)*actual=NSMakeRange(NSNotFound,0);return nil;}
    range.length=std::min(range.length,marked.length-range.location);if(actual)*actual=range;return [marked attributedSubstringFromRange:range];
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point{(void)point;return self.hasMarkedText?markedSelection.location:NSNotFound;}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual{
    if(actual)*actual=range;auto h=host.lock();const auto rect=h?h->cursor_rect():NSMakeRect(0,0,1,18);
    return [self.window convertRectToScreen:[self convertRect:rect toView:nil]];
}
- (void)doCommandBySelector:(SEL)selector{if(auto h=host.lock())h->guard([&]{
    stormtty::Key key=stormtty::Key::unidentified;
    if(selector==@selector(insertNewline:))key=stormtty::Key::enter;else if(selector==@selector(insertTab:))key=stormtty::Key::tab;
    else if(selector==@selector(deleteBackward:))key=stormtty::Key::backspace;else if(selector==@selector(cancelOperation:)){[self unmarkText];key=stormtty::Key::escape;}
    else if(selector==@selector(moveLeft:))key=stormtty::Key::left;else if(selector==@selector(moveRight:))key=stormtty::Key::right;
    else if(selector==@selector(moveUp:))key=stormtty::Key::up;else if(selector==@selector(moveDown:))key=stormtty::Key::down;
    if(typingEvent){pressedKeys.insert(typingEvent.keyCode);h->key(typingEvent,key);}
});}
- (void)mouseDown:(NSEvent*)event{if(auto h=host.lock())h->guard([&]{h->mouse(event,stormtty::MouseAction::press);});}
- (void)mouseDragged:(NSEvent*)event{if(auto h=host.lock())h->guard([&]{h->mouse(event,stormtty::MouseAction::motion);});}
- (void)mouseUp:(NSEvent*)event{if(auto h=host.lock())h->guard([&]{h->mouse(event,stormtty::MouseAction::release);});}
- (void)rightMouseDown:(NSEvent*)event{[self mouseDown:event];}
- (void)rightMouseUp:(NSEvent*)event{[self mouseUp:event];}
- (void)otherMouseDown:(NSEvent*)event{[self mouseDown:event];}
- (void)otherMouseUp:(NSEvent*)event{[self mouseUp:event];}
- (void)scrollWheel:(NSEvent*)event{if(auto h=host.lock())h->guard([&]{h->scroll(event);});}
- (BOOL)isAccessibilityElement{return YES;}
- (NSString*)accessibilityRole{return NSAccessibilityTextAreaRole;}
- (NSString*)accessibilityLabel{return @"Terminal output and input";}
- (NSString*)accessibilityValue{auto h=host.lock();auto session=h?h->active():nullptr;auto frame=session?session->frame():nullptr;
    try{if(frame&&frame->cursor.password_input)return @"Secure terminal input";return stormtty::macos::ns(stormtty::macos::frame_text(frame));}catch(...){return @"Terminal output unavailable";}}
- (NSString*)accessibilitySelectedText{auto h=host.lock();auto session=h?h->active():nullptr;try{auto frame=session?session->frame():nullptr;if(frame&&frame->cursor.password_input)return @"";return stormtty::macos::ns(stormtty::macos::frame_text(frame,65536,true));}catch(...){return @"";}}
- (NSInteger)accessibilityNumberOfCharacters{
    id value=[self accessibilityValue];
    return [value isKindOfClass:NSString.class]?static_cast<NSInteger>([(NSString*)value length]):0;
}
- (NSRange)accessibilityVisibleCharacterRange{
    id value=[self accessibilityValue];
    return NSMakeRange(0,[value isKindOfClass:NSString.class]?[(NSString*)value length]:0);
}
- (BOOL)isAccessibilityFocused{return self.window.firstResponder==self;}
@end

@implementation STWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender{(void)sender;if(auto h=host.lock())h->guard([&]{h->begin_close();});return NO;}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender{(void)sender;if(auto h=host.lock())h->guard([&]{h->begin_close();});return NSTerminateCancel;}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender{(void)sender;return NO;}
- (void)windowDidResize:(NSNotification*)note{(void)note;if(auto h=host.lock())h->guard([&]{h->resize();});}
- (void)windowDidChangeBackingProperties:(NSNotification*)note{[self windowDidResize:note];}
- (void)windowDidBecomeKey:(NSNotification*)note{(void)note;if(auto h=host.lock())h->guard([&]{h->focus_changed();});}
- (void)windowDidResignKey:(NSNotification*)note{[self windowDidBecomeKey:note];}
- (void)windowDidChangeOcclusionState:(NSNotification*)note{(void)note;if(auto h=host.lock())h->wake();}
- (void)executeCommand:(id)sender{if(auto h=host.lock())h->guard([&]{h->command(stormtty::macos::utf8([sender identifier]));});}
- (void)selectTab:(id)sender{if(auto h=host.lock())h->guard([&]{h->select_tab(static_cast<std::uint64_t>([sender tag]));});}
- (BOOL)validateMenuItem:(NSMenuItem*)item{auto h=host.lock();if(!h)return NO;BOOL valid=NO;h->guard([&]{valid=stormtty::macos::menu_commands_available(h->view.hasMarkedText,h->closing,NSApp.modalWindow!=nil||h->window.attachedSheet!=nil)&&stormtty::command_availability(stormtty::macos::utf8(item.identifier),h->context()).enabled;});return valid;}
- (void)displayPreferencesChanged:(NSNotification*)note{(void)note;if(auto h=host.lock())h->guard([&]{h->appearance();});}
@end

namespace stormtty {
int run_native_application(Config config,NativeRunOptions options) {
    @autoreleasepool {
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        auto host=std::make_shared<macos::Host>(std::move(config),std::move(options));host->initialize();[NSApp run];
        [host->window orderOut:nil];NSApp.delegate=nil;host->window.delegate=nil;
        return host->exit_code;
    }
}
}
