# Windows application builds include the pinned shell. Core/non-Windows builds
# never include this module and acquire no Python/download dependency.
option(STORMTTY_EXPERIMENTAL_COMPLETION_SEED "Opt in to native build-anchored completion seed and serialized initialization" OFF)
option(STORMTTY_BUNDLE_ZSH "Bundle pinned upstream Zsh and the managed preset with the Windows app" ON)
set(STORMTTY_DEVELOPER_SHELL_RUNTIME "" CACHE PATH "Optional already-staged Windows Zsh root; overrides automatic build preparation")
if(STORMTTY_BUNDLE_ZSH OR STORMTTY_DEVELOPER_SHELL_RUNTIME)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(_shell_stager "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage-developer-shell.py")
    set(_shell_lock "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell-bootstrap.lock.json")
    set(_shell_preset "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell-preset.zsh")
    set(_shell_prepare_command)
    if(STORMTTY_DEVELOPER_SHELL_RUNTIME)
        if(NOT IS_ABSOLUTE "${STORMTTY_DEVELOPER_SHELL_RUNTIME}")
            message(FATAL_ERROR "STORMTTY_DEVELOPER_SHELL_RUNTIME must be an absolute staged runtime root")
        endif()
        set(_shell_source "${STORMTTY_DEVELOPER_SHELL_RUNTIME}")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${_shell_stager}"
            --source "${_shell_source}" --lock "${_shell_lock}" --preset "${_shell_preset}"
            RESULT_VARIABLE _shell_result OUTPUT_VARIABLE _shell_output ERROR_VARIABLE _shell_error TIMEOUT 180)
        if(NOT _shell_result EQUAL 0)
            message(FATAL_ERROR "Configured shell runtime is invalid:\n${_shell_output}${_shell_error}")
        endif()
    else()
        set(STORMTTY_SHELL_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cache/shell" CACHE PATH "Checksum-verified bundled shell download cache")
        option(STORMTTY_SHELL_OFFLINE "Require cached shell archives; never download while building" OFF)
        set(STORMTTY_SHELL_ZSTD "" CACHE FILEPATH "Optional trusted host zstd executable (normally provisioned automatically on Windows)")
        set(_shell_source "${CMAKE_CURRENT_BINARY_DIR}/_deps/stormtty-shell")
        set(_shell_prepare_args --runtime "${_shell_source}" --cache "${STORMTTY_SHELL_CACHE}")
        if(STORMTTY_SHELL_OFFLINE)
            list(APPEND _shell_prepare_args --offline)
        endif()
        if(STORMTTY_SHELL_ZSTD)
            list(APPEND _shell_prepare_args --zstd "${STORMTTY_SHELL_ZSTD}")
        endif()
        set(_shell_prepare_command COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-bundled-shell.py" ${_shell_prepare_args})
    endif()
    # Bounded metadata inventories skip copying on no-op builds and repair
    # deleted outputs. Never put upstream "[.exe" into a CMake list (unbalanced
    # brackets alter list parsing). All file validation stays in the helper.
    set(_shell_stamp "${CMAKE_CURRENT_BINARY_DIR}/developer-shell-$<CONFIG>.stamp")
    add_custom_target(stormtty_developer_shell_runtime
        ${_shell_prepare_command}
        COMMAND "${Python3_EXECUTABLE}" "${_shell_stager}"
            --source "${_shell_source}" --lock "${_shell_lock}" --preset "${_shell_preset}"
            --destination "$<TARGET_FILE_DIR:stormtty>/runtimes/zsh" --stamp "${_shell_stamp}" --repair-if-needed
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell-tools.lock.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell-tools-sources.lock.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell-tools.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell-tools.zsh"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell-tools-zoxide.zsh"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell-tools-notices.py"
        COMMENT "Preparing the pinned bundled Zsh runtime and notices" VERBATIM)
    add_dependencies(stormtty stormtty_developer_shell_runtime)
    target_compile_definitions(stormtty PRIVATE STORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1)
    message(STATUS "Bundled Zsh enabled for fresh app launches; existing saved shell choices win. No launch-time downloads.")
    # This local install layout is not a public redistribution/source approval.
    install(CODE "execute_process(COMMAND [==[${Python3_EXECUTABLE}]==] [==[${_shell_stager}]==] --source [==[${_shell_source}]==] --lock [==[${_shell_lock}]==] --preset [==[${_shell_preset}]==] RESULT_VARIABLE result)\nif(NOT result EQUAL 0)\nmessage(FATAL_ERROR \"Bundled runtime validation failed; install stopped\")\nendif()")
    if(STORMTTY_EXPERIMENTAL_COMPLETION_SEED)
        if(CMAKE_CROSSCOMPILING OR NOT CMAKE_HOST_WIN32)
            message(FATAL_ERROR "Completion seed generation requires the exact native Windows runtime; cross-build activation is forbidden")
        endif()
        set(_seed_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/completion-seed")
        file(MAKE_DIRECTORY "${_seed_generated}")
        add_custom_target(stormtty_completion_seed_package
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-bundled-completion-seed.py"
                --runtime "$<TARGET_FILE_DIR:stormtty>/runtimes/zsh"
                --destination "$<TARGET_FILE_DIR:stormtty>/runtimes/zsh-seed"
                --trust-header "${_seed_generated}/stormtty_completion_seed_trust.hpp"
            BYPRODUCTS "${_seed_generated}/stormtty_completion_seed_trust.hpp"
            COMMENT "Generating or verifying exact native completion seed and compiled trust anchor" VERBATIM)
        add_dependencies(stormtty_completion_seed_package stormtty_developer_shell_runtime)
        add_dependencies(stormtty_settings_services stormtty_completion_seed_package)
        target_sources(stormtty_settings_services PRIVATE src/platform/windows/completion_seed.cpp)
        target_include_directories(stormtty_settings_services PRIVATE "${_seed_generated}")
        target_compile_definitions(stormtty_settings_services PUBLIC STORMTTY_EXPERIMENTAL_COMPLETION_SEED=1)
        target_link_libraries(stormtty_settings_services PRIVATE bcrypt)
        # Copy the SAME final tree used by generation, including its staging
        # receipt. Installing _shell_source instead would invalidate the anchor.
        install(DIRECTORY "$<TARGET_FILE_DIR:stormtty>/runtimes/zsh/" DESTINATION runtimes/zsh)
        install(DIRECTORY "$<TARGET_FILE_DIR:stormtty>/runtimes/zsh-seed/" DESTINATION runtimes/zsh-seed)
    else()
        install(DIRECTORY "${_shell_source}/" DESTINATION runtimes/zsh)
    endif()
endif()

if(STORMTTY_EXPERIMENTAL_COMPLETION_SEED AND NOT (STORMTTY_BUNDLE_ZSH OR STORMTTY_DEVELOPER_SHELL_RUNTIME))
    message(FATAL_ERROR "Completion seed option requires an exact staged bundled runtime")
endif()
