# Compile-only Windows GNU gate from a Unix host. This is NOT an MSVC toolchain.
# Example:
# cmake -S . -B build/windows-gnu -G Ninja \
#   -DCMAKE_TOOLCHAIN_FILE=cmake/ZigWindowsGNU.cmake \
#   -DSTORMTTY_ZIG_EXECUTABLE=/path/to/zig \
#   -DSTORMTTY_GHOSTTY_ROOT=/path/to/ghostty-windows-gnu-out
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT CMAKE_HOST_UNIX)
    message(FATAL_ERROR "ZigWindowsGNU.cmake is a Unix-host compile gate; use native MSVC on Windows")
endif()
find_program(STORMTTY_ZIG_EXECUTABLE NAMES zig REQUIRED)
execute_process(COMMAND "${STORMTTY_ZIG_EXECUTABLE}" version
    OUTPUT_VARIABLE _stormtty_zig_version OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT _stormtty_zig_version STREQUAL "0.17.0")
    message(FATAL_ERROR "Expected Zig 0.17.0, found ${_stormtty_zig_version}")
endif()

set(STORMTTY_ZIG_CACHE_DIR "${CMAKE_BINARY_DIR}/zig-cache" CACHE PATH "Workspace-backed Zig cross-compiler cache")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES STORMTTY_ZIG_EXECUTABLE STORMTTY_ZIG_CACHE_DIR)
file(MAKE_DIRECTORY "${STORMTTY_ZIG_CACHE_DIR}/tmp" "${CMAKE_BINARY_DIR}/zig-tools")
# CMake's archive tools have no generic ARG1 property. Tiny generated wrappers
# preserve spaces and keep compiler/CRT temporary files off /tmp and $HOME.
string(REPLACE "'" "'\\''" _stormtty_zig_shell "${STORMTTY_ZIG_EXECUTABLE}")
string(REPLACE "'" "'\\''" _stormtty_cache_shell "${STORMTTY_ZIG_CACHE_DIR}")
foreach(_tool cc cxx ar ranlib)
    if(_tool STREQUAL "cc")
        set(_args "cc -target x86_64-windows-gnu")
    elseif(_tool STREQUAL "cxx")
        set(_args "c++ -target x86_64-windows-gnu")
    else()
        set(_args "${_tool}")
    endif()
    set(_wrapper "${CMAKE_BINARY_DIR}/zig-tools/zig-${_tool}")
    file(WRITE "${_wrapper}" "#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR='${_stormtty_cache_shell}'\nexport TMPDIR='${_stormtty_cache_shell}/tmp'\nexec '${_stormtty_zig_shell}' ${_args} \"$@\"\n")
    file(CHMOD "${_wrapper}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()
set(CMAKE_C_COMPILER "${CMAKE_BINARY_DIR}/zig-tools/zig-cc")
set(CMAKE_CXX_COMPILER "${CMAKE_BINARY_DIR}/zig-tools/zig-cxx")
set(CMAKE_AR "${CMAKE_BINARY_DIR}/zig-tools/zig-ar")
set(CMAKE_RANLIB "${CMAKE_BINARY_DIR}/zig-tools/zig-ranlib")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
