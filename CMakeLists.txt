# thebigs2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(thebigs2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Cross builds (Windows from Linux, packaging/windows/) can't run the target's
# rexglue.exe for codegen; use the host SDK's. The generated code is the same.
if(CMAKE_CROSSCOMPILING)
    find_program(REXGLUE_HOST_TOOL rexglue
        HINTS "$ENV{HOME}/rexsdk-install/bin" NO_CMAKE_FIND_ROOT_PATH)
    if(NOT REXGLUE_HOST_TOOL)
        message(FATAL_ERROR "Cross build: set REXGLUE_HOST_TOOL to a host rexglue binary")
    endif()
    set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION_RELEASE "${REXGLUE_HOST_TOOL}")
endif()

# Sources
set(THEBIGS2_SOURCES
    src/main.cpp
    src/hooks.cpp
    src/settings.cpp
    src/main_menu_entry.cpp
    src/online_debug.cpp
    src/settings_menu.cpp
    src/ui_theme.cpp
    src/imgui_gamepad.cpp
    src/frame_stats.cpp
    src/first_launch.cpp
    src/render_tweaks.cpp
)

# RenderDoc (F8 frame capture) has no macOS build; its header stops with "Unknown platform".
if(NOT APPLE)
    list(APPEND THEBIGS2_SOURCES src/renderdoc_capture.cpp)
endif()

if(WIN32)
    add_executable(thebigs2 WIN32 ${THEBIGS2_SOURCES} packaging/windows/thebigs2.rc)
    # The resource compiler doesn't report the .ico it embeds, so a new icon wouldn't rebuild the .rc on its own.
    set_source_files_properties(packaging/windows/thebigs2.rc PROPERTIES
        OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/thebigs2.ico)
    target_link_libraries(thebigs2 PRIVATE ws2_32)  # online_debug.cpp: own-IP check for the GameConn port swap
else()
    add_executable(thebigs2 ${THEBIGS2_SOURCES})
endif()

# SDL3 is linked into librexruntime; we only need its headers (window mode,
# file picker).
if(NOT TARGET SDL3::Headers)
    find_package(SDL3 CONFIG QUIET)
endif()
if(TARGET SDL3::Headers)
    target_link_libraries(thebigs2 PRIVATE SDL3::Headers)
elseif(DEFINED REXSDK_DIR AND EXISTS "${REXSDK_DIR}/thirdparty/sdl3/include")
    target_include_directories(thebigs2 PRIVATE "${REXSDK_DIR}/thirdparty/sdl3/include")
else()
    message(FATAL_ERROR "SDL3 headers not found (they ship with the ReXGlue SDK)")
endif()

# A user's disc image is accepted only if its default.xex is the one this
# build was recompiled from.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game/default.xex")
    file(MD5 "${CMAKE_CURRENT_SOURCE_DIR}/game/default.xex" TB2_XEX_MD5)
    target_compile_definitions(thebigs2 PRIVATE TB2_XEX_MD5="${TB2_XEX_MD5}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/game/default.xex")
endif()

# Render call tracing (debug build option).
# Wraps every function in the XDK D3D library and the engine's renderlib with a
# call counter; F8 records a frame. Off by default (adds ~2,000 wrappers).
option(TB2_RENDER_TRACE "Build with render call tracing (F8)" OFF)
if(TB2_RENDER_TRACE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_trace_cpp "${CMAKE_CURRENT_BINARY_DIR}/render_trace_hooks.cpp")
    set(_codegen_stamp "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp")
    add_custom_command(
        OUTPUT "${_trace_cpp}"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_render_trace.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/generated/default" "${_trace_cpp}"
                d3d:823A6400-823DE000 renderlib:82460000-8249B800
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_render_trace.py" "${_codegen_stamp}"
        COMMENT "Generating render trace wrappers"
        VERBATIM)
    target_sources(thebigs2 PRIVATE src/render_trace.cpp "${_trace_cpp}")
    target_compile_definitions(thebigs2 PRIVATE TB2_RENDER_TRACE=1)
endif()

# Function-entry tracing for reverse engineering (TB2_FN_TRACE=1 at run time logs functions run per AutoKey tap).
option(TB2_FN_TRACE "Build with function entry trace wrappers for the UI range" OFF)
set(TB2_FN_TRACE_RANGE "82200000-82360000" CACHE STRING "Guest address range (lo-hi hex) wrapped by TB2_FN_TRACE")
if(TB2_FN_TRACE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_fn_cpp "${CMAKE_CURRENT_BINARY_DIR}/fn_trace_hooks.cpp")
    add_custom_command(
        OUTPUT "${_fn_cpp}"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_fn_trace.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/generated/default" "${_fn_cpp}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/online_debug.cpp" ${TB2_FN_TRACE_RANGE}
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_fn_trace.py" "${CMAKE_CURRENT_SOURCE_DIR}/src/online_debug.cpp"
        COMMENT "Generating function trace wrappers" VERBATIM)
    target_sources(thebigs2 PRIVATE "${_fn_cpp}")
endif()

# Put the ONLINE entry back on the main menu (the game's own OnlineMenu; online
# play needs the launcher's Host/Join env, see thebigs2_gui.py online_env()).
option(TB2_ONLINE_MENU "Add an ONLINE entry that opens the game's online menu" ON)
if(TB2_ONLINE_MENU)
    target_compile_definitions(thebigs2 PRIVATE TB2_ONLINE_MENU=1)
endif()

# GPU emulation is opt-in on the development branch; ship the Xenos plugin.
rexglue_setup_target(thebigs2 GPU_PLUGINS xenos)

# Online play is lockstep: every machine must compute bit-identical floats. Never fuse a*b+c into one FMA
# (clang does so by default on arm64, e.g. Apple Silicon); explicit std::fma calls stay correctly rounded.
foreach(_t thebigs2 thebigs2_recomp)
    if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
    endif()
endforeach()

# Keep the executable loadable on glibc older than the build machine's, e.g.
# SteamOS (see src/glibc_compat.h), including the recompiled sources.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    foreach(_t thebigs2 thebigs2_recomp)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE
                "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/glibc_compat.h")
        endif()
    endforeach()
endif()

# The SDK sets a $ORIGIN rpath on Linux but (unlike Windows/macOS) never stages
# librexruntime.so / libTracyClient.so next to the binary, so it won't start.
if(UNIX AND NOT APPLE)
    foreach(_lib rex::runtime rexruntime rex::TracyClient TracyClient)
        if(TARGET ${_lib})
            add_custom_command(TARGET thebigs2 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_lib}> $<TARGET_FILE_DIR:thebigs2>
                VERBATIM)
        endif()
    endforeach()
endif()
