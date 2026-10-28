#import <AppKit/AppKit.h>
#include "stormtty/native_window.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

int main(int argc,char** argv) {
    bool smoke=false;
    for(int i=1;i<argc;++i)if(std::string_view(argv[i])=="--smoke")smoke=true;
    @autoreleasepool {
        try {
            stormtty::NativeRunOptions options;options.smoke_test=smoke;
            std::filesystem::path path;bool required=false;
            for(int i=1;i<argc;++i){const std::string_view arg=argv[i];
                if(arg=="--smoke")continue;
                if(arg=="--capture"&&i+1<argc){options.capture_path=argv[++i];continue;}
                if(arg=="--config"&&i+1<argc){path=argv[++i];required=true;continue;}
                if(arg=="--help"){
                    std::puts("StormTTY [--config /absolute/path]\nCI: stormtty --smoke [--capture /path/frame.png]\nCmd+T: new tab; Cmd+W: close pane; Cmd+Shift+P: commands\nCmd+C/V: copy/paste; Ctrl+C remains terminal interrupt.\nNative macOS support requires macOS 11+ and a Metal device.");return 0;
                }
                throw std::invalid_argument("Unknown or incomplete option; use --help");
            }
            if(smoke&&required)throw std::invalid_argument("Smoke mode never loads user configuration or shell profiles");
            if(!smoke&&!options.capture_path.empty())throw std::invalid_argument("--capture requires explicit --smoke mode");
            auto config=stormtty::default_config(stormtty::Platform::macos);
            if(!smoke&&path.empty()){
                // Foundation supplies the current user's home; do not assume a
                // hard-coded /Users name or mutate existing configuration.
                path=std::filesystem::path(NSHomeDirectory().UTF8String)/"Library/Application Support/StormTTY/config.stormtty";
            }
            if(!path.empty()&&(required||std::filesystem::exists(path))){
                auto loaded=stormtty::load_config(path,stormtty::Platform::macos);
                if(!loaded){std::string error="Configuration rejected without applying partial settings:\n";
                    for(const auto& item:loaded.errors)error+="Line "+std::to_string(item.line)+": "+item.message+"\n";
                    throw std::invalid_argument(error);}
                config=std::move(*loaded.config);
            }
            return stormtty::run_native_application(std::move(config),std::move(options));
        }catch(const std::exception& e){
            std::fprintf(stderr,"StormTTY startup: %s\n",e.what());
            if(!smoke){[NSApplication sharedApplication];NSAlert* alert=[NSAlert new];alert.messageText=@"StormTTY could not start";
                alert.informativeText=[[NSString alloc] initWithBytes:e.what() length:std::char_traits<char>::length(e.what()) encoding:NSUTF8StringEncoding] ?: @"Unknown error";
                [alert runModal];}return 1;
        }catch(...){std::fprintf(stderr,"StormTTY: unexpected macOS startup failure\n");return 1;}
    }
}
