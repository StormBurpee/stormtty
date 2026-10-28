# Included only for a Windows application target. Acquisition is build-only;
# availability never promotes the persisted/default System provider.
option(STORMTTY_BUNDLE_CONPTY "Bundle the pinned native AMD64 ConPTY preview (System remains default)" ON)
if(STORMTTY_BUNDLE_CONPTY)
    if(CMAKE_CROSSCOMPILING OR NOT CMAKE_HOST_WIN32)
        message(FATAL_ERROR "Bundled ConPTY needs native AMD64 Windows trust verification. Use -DSTORMTTY_BUNDLE_CONPTY=OFF for cross-builds; no trust bypass is provided.")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]|aarch64" OR CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]|Win32")
        message(FATAL_ERROR "Bundled ConPTY preview supports native AMD64 only. Use -DSTORMTTY_BUNDLE_CONPTY=OFF for unsupported architectures.")
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(STORMTTY_CONPTY_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cache/conpty" CACHE PATH "Checksum-verified pinned ConPTY download cache")
    option(STORMTTY_CONPTY_OFFLINE "Require cached ConPTY archive; never download while building" OFF)
    set(_conpty_prepare "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-bundled-conpty.py")
    set(_conpty_args --cache "${STORMTTY_CONPTY_CACHE}")
    if(STORMTTY_CONPTY_OFFLINE)
        list(APPEND _conpty_args --offline)
    endif()
    set(_conpty_relative "runtimes/conpty/1.25.260930003/x64")
    add_custom_target(stormtty_console_runtime
        COMMAND "${Python3_EXECUTABLE}" "${_conpty_prepare}"
            --destination "$<TARGET_FILE_DIR:stormtty>/${_conpty_relative}" ${_conpty_args}
        COMMENT "Verifying and preparing the pinned private ConPTY preview and notices (System remains default)"
        VERBATIM)
    add_dependencies(stormtty stormtty_console_runtime)
    target_compile_definitions(stormtty PRIVATE STORMTTY_HAS_BUNDLED_CONPTY_RUNTIME=1)
    # Install validates the exact staged artifacts with Windows trust again.
    install(CODE "execute_process(COMMAND [==[${Python3_EXECUTABLE}]==] [==[${_conpty_prepare}]==] --destination [==[$<TARGET_FILE_DIR:stormtty>/${_conpty_relative}]==] --cache [==[${STORMTTY_CONPTY_CACHE}]==] --verify-only RESULT_VARIABLE result)\nif(NOT result EQUAL 0)\nmessage(FATAL_ERROR \"Private ConPTY validation failed; install stopped\")\nendif()")
    install(DIRECTORY "$<TARGET_FILE_DIR:stormtty>/${_conpty_relative}/" DESTINATION "${_conpty_relative}")
    message(STATUS "Private ConPTY preview will be bundled; System remains the default. Native AMD64 only; no launch-time downloads.")
endif()
