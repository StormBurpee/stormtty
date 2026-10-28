# Native macOS, official Apple SDK + Homebrew LLVM/libc++ 20 toolchain.
# Usage: -DCMAKE_TOOLCHAIN_FILE=cmake/MacOSLLVM.cmake
#        -DSTORMTTY_LLVM_ROOT="$(brew --prefix llvm@20)"
# All standard-library flags are set before project()/compiler detection. This
# file does not install tools, alter xcode-select, set experimental macros, or
# download an SDK. App distribution must package/audit this non-system runtime.
include_guard(GLOBAL)
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "MacOSLLVM.cmake requires native macOS with an official Apple SDK")
endif()
set(STORMTTY_LLVM_ROOT "" CACHE PATH "Homebrew llvm@20 prefix; pass brew --prefix llvm@20")
if(NOT STORMTTY_LLVM_ROOT)
    message(FATAL_ERROR "Pass -DSTORMTTY_LLVM_ROOT=\"$(brew --prefix llvm@20)\"; no compiler/runtime path is guessed")
endif()
get_filename_component(STORMTTY_LLVM_ROOT "${STORMTTY_LLVM_ROOT}" ABSOLUTE)
set(STORMTTY_LLVM_ROOT "${STORMTTY_LLVM_ROOT}" CACHE PATH "Homebrew llvm@20 prefix" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES STORMTTY_LLVM_ROOT)
foreach(required bin/clang bin/clang++ include/c++/v1/stop_token lib/c++/libc++.1.dylib lib/unwind/libunwind.dylib)
    if(NOT EXISTS "${STORMTTY_LLVM_ROOT}/${required}")
        message(FATAL_ERROR "Incomplete LLVM 20 compiler/header/runtime installation: ${STORMTTY_LLVM_ROOT}/${required}")
    endif()
endforeach()
execute_process(COMMAND "${STORMTTY_LLVM_ROOT}/bin/clang++" --version
    RESULT_VARIABLE _version_result OUTPUT_VARIABLE _version ERROR_VARIABLE _version_error)
if(NOT _version_result EQUAL 0 OR NOT _version MATCHES "clang version 20\\.")
    message(FATAL_ERROR "StormTTY's macOS toolchain is pinned to clang/libc++ major 20: ${_version} ${_version_error}")
endif()
if(NOT CMAKE_OSX_SYSROOT)
    execute_process(COMMAND /usr/bin/xcrun --sdk macosx --show-sdk-path
        RESULT_VARIABLE _sdk_result OUTPUT_VARIABLE _sdk OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE _sdk_error)
    if(NOT _sdk_result EQUAL 0 OR NOT EXISTS "${_sdk}/System/Library/Frameworks/AppKit.framework")
        message(FATAL_ERROR "An official Xcode/Command Line Tools macOS SDK is required: ${_sdk_error}")
    endif()
    set(CMAKE_OSX_SYSROOT "${_sdk}" CACHE PATH "Official xcrun macOS SDK")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "StormTTY API baseline; bundled runtime may require a newer OS")
endif()
set(CMAKE_C_COMPILER "${STORMTTY_LLVM_ROOT}/bin/clang" CACHE FILEPATH "LLVM 20 C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${STORMTTY_LLVM_ROOT}/bin/clang++" CACHE FILEPATH "LLVM 20 C++ compiler" FORCE)
set(CMAKE_OBJCXX_COMPILER "${STORMTTY_LLVM_ROOT}/bin/clang++" CACHE FILEPATH "Same LLVM 20 Objective-C++ compiler" FORCE)
set(_standard_flags "--no-default-config -stdlib=libc++ -nostdinc++ -isystem \"${STORMTTY_LLVM_ROOT}/include/c++/v1\"")
string(APPEND CMAKE_CXX_FLAGS_INIT " ${_standard_flags}")
string(APPEND CMAKE_OBJCXX_FLAGS_INIT " ${_standard_flags}")
# The matching runtime must win at link AND load. Merely selecting new headers
# while linking Apple's old /usr/lib/libc++.1.dylib is not a valid fix.
set(_runtime_flags "-nostdlib++ -L\"${STORMTTY_LLVM_ROOT}/lib/c++\" -L\"${STORMTTY_LLVM_ROOT}/lib/unwind\" -Wl,-rpath,\"${STORMTTY_LLVM_ROOT}/lib/c++\" -Wl,-rpath,\"${STORMTTY_LLVM_ROOT}/lib/unwind\" -lc++ -lunwind")
foreach(kind EXE SHARED MODULE)
    string(APPEND CMAKE_${kind}_LINKER_FLAGS_INIT " ${_runtime_flags}")
endforeach()
