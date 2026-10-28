#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include "stormtty/native_window.hpp"
#include <filesystem>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
std::wstring widen(std::string_view text) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"StormTTY could not decode an error message.";
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
struct ComApartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment() { if (SUCCEEDED(status)) CoUninitialize(); }
};
struct Arguments {
    int count{};
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    ~Arguments() { if (values) LocalFree(values); }
};
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    bool smoke=false;
    try {
        Arguments args;
        if (!args.values) throw std::runtime_error("Could not read command-line arguments.");
        for (int i=1;i<args.count;++i) if (std::wstring_view(args.values[i])==L"--smoke" || std::wstring_view(args.values[i])==L"--smoke-layout" || std::wstring_view(args.values[i])==L"--smoke-modal") smoke=true;
        stormtty::NativeRunOptions options; options.smoke_test=smoke;
        // Must precede creation of any HWND, including startup error dialogs.
        // ERROR_ACCESS_DENIED means the executable manifest already set it.
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && GetLastError() != ERROR_ACCESS_DENIED)
            throw std::runtime_error("Per-monitor-v2 DPI awareness requires Windows 10 version 1703 or newer.");
        ComApartment apartment;
        if (FAILED(apartment.status)) throw std::runtime_error("Could not initialize the UI COM apartment.");
        auto config = stormtty::default_config(stormtty::Platform::windows);
        std::filesystem::path config_path;
        bool required = false;
        for (int i = 1; i < args.count; ++i) {
            const std::wstring argument(args.values[i]);
            if (argument == L"--config" && i + 1 < args.count) { config_path = args.values[++i]; required = true; }
            else if (argument == L"--smoke") {}
            else if (argument == L"--smoke-layout") options.smoke_layout=true;
            else if (argument == L"--smoke-modal") options.smoke_modal=true;
            else if (argument == L"--capture" && i+1<args.count) options.capture_path=args.values[++i];
            else if (argument == L"--help") {
                MessageBoxW(nullptr, L"StormTTY [--config path]\nCI: StormTTY --smoke [--capture frame.png]\nLayout CI: StormTTY --smoke-layout --capture layout.png\nModal CI: StormTTY --smoke-modal --capture modal.png\n\nCtrl+Shift+P opens commands.\nCtrl+Shift+T creates a tab.\nCtrl+Tab changes tabs.\nCtrl+Shift+V pastes.\nAlt+Space opens the Windows system menu.", L"StormTTY", MB_OK);
                return 0;
            } else throw std::runtime_error("Unknown or incomplete option. Use --help or --config path.");
        }
        if (smoke && required) throw std::runtime_error("Smoke mode does not load user configuration or profile scripts.");
        if (!smoke && !options.capture_path.empty()) throw std::runtime_error("--capture is available only with explicit --smoke mode.");
        if (config_path.empty() && !smoke) {
            const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
            if (count) {
                std::wstring directory(count, L'\0');
                const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", directory.data(), count);
                if (written && written < count) { directory.resize(written); config_path = std::filesystem::path(directory) / L"StormTTY" / L"config.stormtty"; }
            }
        }
        if (!config_path.empty() && (required || std::filesystem::exists(config_path))) {
            auto loaded = stormtty::load_config(config_path, stormtty::Platform::windows);
            if (!loaded) {
                std::string error = "Configuration rejected; no partial settings were applied.\n";
                for (const auto& item : loaded.errors) error += "Line " + std::to_string(item.line) + ": " + item.message + "\n";
                throw std::runtime_error(error);
            }
            config = std::move(*loaded.config);
        }
        return stormtty::run_native_application(std::move(config),std::move(options));
    } catch (const std::exception& exception) {
        if (smoke) std::fprintf(stderr,"Native smoke startup error: %s\n",exception.what());
        else MessageBoxW(nullptr, widen(exception.what()).c_str(), L"StormTTY startup error", MB_OK | MB_ICONERROR);
    } catch (...) {
        if (smoke) std::fprintf(stderr,"Native smoke startup failed unexpectedly.\n");
        else MessageBoxW(nullptr, L"An unexpected startup error occurred.", L"StormTTY", MB_OK | MB_ICONERROR);
    }
    return 1;
}
