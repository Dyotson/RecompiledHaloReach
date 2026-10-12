# reach - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(reach LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Builds handed a finished generated/ (CI building from the private copy of the codegen
# output) have no default.xex to run codegen on.
option(REACH_SKIP_CODEGEN "Compile generated/ as it is, never run codegen" OFF)
if(REACH_SKIP_CODEGEN AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
    message(FATAL_ERROR "REACH_SKIP_CODEGEN is on but generated/default has no codegen output")
endif()

# Codegen runs the patched SDK overlay's rexglue when there is one: tools/build_rexglue_sdk.sh
# builds it from patches/rexglue-sdk/, and some patches change the generated code (0025:
# Xenon's dot products give QNaN on overflow).
set(REACH_REXGLUE "$ENV{HOME}/rexglue-sdk-patched/0.10.0.24/linux-amd64/bin/rexglue"
    CACHE FILEPATH "Patched rexglue to run codegen with instead of the SDK's")

# generated/rexglue.cmake lists the generated sources at configure time, so on a fresh
# checkout (no generated/default yet) codegen runs here once; otherwise the first build
# would run it but compile none of its output.
if(NOT CMAKE_CROSSCOMPILING AND NOT REXSDK_DIR AND
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
    find_package(rexglue 0.10.0 REQUIRED CONFIG)
    get_target_property(_rexglue rex::rexglue LOCATION)
    if(EXISTS "${REACH_REXGLUE}")
        set(_rexglue "${REACH_REXGLUE}")
    endif()
    message(STATUS "First configure: running codegen (${_rexglue}), this takes a while")
    execute_process(
        COMMAND "${_rexglue}" codegen "${CMAKE_CURRENT_SOURCE_DIR}/reach_manifest.toml"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()

include(generated/rexglue.cmake)

# A cross build (tools/build_windows.sh) compiles the generated code the native build
# made; it must not run codegen itself: the SDK's rexglue for the target cannot run here,
# and regenerating would rewrite generated/ under the native build. The build-time
# codegen step stays (rexglue.cmake orders the generated sources after it) but runs
# true instead of rexglue.
if((CMAKE_CROSSCOMPILING OR REACH_SKIP_CODEGEN) AND TARGET rex::rexglue)
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION /usr/bin/true IMPORTED_LOCATION_RELEASE /usr/bin/true)
    message(STATUS "Codegen is skipped: compiling generated/ as it is")
elseif(TARGET rex::rexglue AND EXISTS "${REACH_REXGLUE}")
    get_target_property(_configs rex::rexglue IMPORTED_CONFIGURATIONS)
    set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION "${REACH_REXGLUE}")
    foreach(_config IN LISTS _configs)
        set_target_properties(rex::rexglue PROPERTIES
            IMPORTED_LOCATION_${_config} "${REACH_REXGLUE}")
    endforeach()
    # A new tool reruns codegen (its depfile lists only the inputs).
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp"
        APPEND DEPENDS "${REACH_REXGLUE}")
    message(STATUS "Codegen tool: ${REACH_REXGLUE}")
elseif(TARGET rex::rexglue)
    message(WARNING "Patched rexglue not found at ${REACH_REXGLUE}; codegen uses the SDK's, "
                    "whose dot products differ from Xenon's. Run tools/build_rexglue_sdk.sh.")
endif()

# Sources
set(REACH_SOURCES
    src/main.cpp
    src/debug/frame_dump.cpp
    src/debug/frame_stats.cpp
    src/debug/gamestate_trace.cpp
    src/debug/sync_diff.cpp
    src/debug/sync_trace.cpp
    src/debug/texture_trace.cpp
    src/debug/tick_audit.cpp
    src/hooks/frame_rate.cpp
    src/hooks/frame_rate_net.cpp
    src/hooks/mouse_look.cpp
    src/hooks/offline_challenges.cpp
    src/hooks/switch_tailcalls.cpp
    src/kernel/critical_section.cpp
    src/kernel/file_delete_on_close.cpp
    src/kernel/file_size_refresh.cpp
    src/kernel/object_refs.cpp
    src/kernel/identity.cpp
    src/kernel/input_autopress.cpp
    src/kernel/live_xmsg.cpp
    src/kernel/live_lsp.cpp
    src/kernel/live_tcp.cpp
    src/kernel/net.cpp
    src/kernel/xam_enumerate.cpp
    src/kernel/xam_keyboard.cpp
    src/kernel/xam_signin.cpp
    src/kernel/xecrypt_rsa.cpp
    src/platform/achievement_toast.cpp
    src/platform/sdk_import.cpp
    src/platform/socket.cpp
    src/platform/vulkan_hooks.cpp
    src/input/kbm.cpp
)

if(WIN32)
    add_executable(reach WIN32 ${REACH_SOURCES})
    target_compile_definitions(reach PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(reach PRIVATE ws2_32)
else()
    add_executable(reach ${REACH_SOURCES})
endif()

rexglue_setup_target(reach GPU_PLUGINS xenos)

# src/platform/vulkan_hooks.cpp includes the SDK's Vulkan headers, which include
# <renderdoc_app.h> from this interface target and the X11/Wayland client headers
# (stand-ins in src/platform/wsi_stubs, so no window-system packages are needed).
if(TARGET rex::renderdoc)
    target_link_libraries(reach PRIVATE rex::renderdoc)
endif()
if(UNIX AND NOT APPLE)
    set_source_files_properties(src/platform/vulkan_hooks.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/wsi_stubs")
endif()

# The SDK's SPIR-V translator takes the texture-fetch exponent bias from the
# wrong fetch-constant word, which scales Reach's textures by 1/256 (black
# title menu). tools/build_rexglue_sdk.sh builds a patched plugin; this copies
# it over the SDK one that rexglue_setup_target() stages next to the binary.
set(REACH_XENOS_PLUGIN "$ENV{HOME}/rexglue-sdk-patched/0.10.0.24/linux-amd64/lib/librexgpu-xenos.so"
    CACHE FILEPATH "Patched librexgpu-xenos.so to ship instead of the SDK's")
if(UNIX AND NOT APPLE)
    if(EXISTS "${REACH_XENOS_PLUGIN}")
        add_custom_command(TARGET reach POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${REACH_XENOS_PLUGIN}" "$<TARGET_FILE_DIR:reach>/librexgpu-xenos.so"
            VERBATIM)
    else()
        message(WARNING "Patched GPU plugin not found at ${REACH_XENOS_PLUGIN}; "
                        "textures will render dark. Run tools/build_rexglue_sdk.sh.")
    endif()
endif()

# The SDK runtime dlopen()s guest-DLL modules by bare target name on Linux
# (no "lib" prefix, no ".so" suffix), so emit the libraries under that name.
if(UNIX AND NOT APPLE)
    foreach(_module reach_waveShell-Xbox reach_wavesLibDLL reach_L360 reach_Q10)
        if(TARGET ${_module})
            set_target_properties(${_module} PROPERTIES PREFIX "" SUFFIX "")
        endif()
    endforeach()
endif()

# Guest-DLL -> guest-DLL import thunks (codegen only implements kernel/XAM ones).
# Regenerate with tools/gen_xdll_thunks.py after re-running codegen.
if(TARGET reach_L360)
    target_sources(reach_L360 PRIVATE src/xdll/l360_thunks.cpp)
endif()
if(TARGET reach_Q10)
    target_sources(reach_Q10 PRIVATE src/xdll/q10_thunks.cpp)
endif()
