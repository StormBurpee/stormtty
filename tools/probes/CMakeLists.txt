cmake_minimum_required(VERSION 3.24)
project(StormTTYHostBackdropDiagnostic LANGUAGES CXX)
# Standalone native compile route for the same optional root-build target/source.
# No terminal engine, packaged shell, app gates, install rule or runtime downloads.
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Host backdrop diagnostic requires native MSVC and Windows SDK 10.0.22621+")
endif()
add_executable(stormtty_host_backdrop_probe host-backdrop-blur.cpp)
target_compile_features(stormtty_host_backdrop_probe PRIVATE cxx_std_20)
target_compile_options(stormtty_host_backdrop_probe PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_compile_definitions(stormtty_host_backdrop_probe PRIVATE UNICODE _UNICODE)
target_link_libraries(stormtty_host_backdrop_probe PRIVATE windowsapp CoreMessaging d3d11 dxgi d2d1 dxguid dwrite dwmapi user32 gdi32 advapi32 ole32)
