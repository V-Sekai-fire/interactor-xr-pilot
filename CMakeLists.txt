# SPDX-License-Identifier: Apache-2.0 OR MIT
cmake_minimum_required(VERSION 3.24)
project(xr_pilot VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(XRPILOT_BUILD_APP "Build the xr-pilot window (needs a display and Vulkan)" ON)

option(XRPILOT_BUILD_OXRSYS "Build the OXRSys runtime and SteamVR driver in oxrsys/ (needs Vulkan)" ${XRPILOT_BUILD_APP})

# The OXRSys wire protocol and the Lean-authored yuv420_to_rgbx kernel.
set(XRPILOT_OXRSYS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/oxrsys)
set(XRPILOT_PROTOCOL_INCLUDE ${XRPILOT_OXRSYS_DIR}/common/protocol/include)

add_library(xrpilot_core STATIC
    core/src/Agent.cpp
    core/src/Body.cpp
    third_party/sinew-align/sinew_align.c
    core/src/Commands.cpp
    core/src/FrameAssembler.cpp
    core/src/HumanInput.cpp
    core/src/Json.cpp
    core/src/Png.cpp
    core/src/Rotation.cpp
    core/src/SpanLog.cpp
    core/src/TraceStore.cpp
    third_party/sqlite/sqlite3.c
    core/src/Sparkline.cpp
)
target_include_directories(xrpilot_core PUBLIC core/include ${XRPILOT_PROTOCOL_INCLUDE} third_party/sqlite
    PRIVATE third_party/sinew-align)
target_compile_definitions(xrpilot_core PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_FOREIGN_KEYS=1)

add_library(xrpilot_net STATIC core/src/Client.cpp)
target_link_libraries(xrpilot_net PUBLIC xrpilot_core)
if(WIN32)
    target_link_libraries(xrpilot_net PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(xrpilot_net PUBLIC Threads::Threads)
endif()

enable_testing()
add_executable(xrpilot-core-tests core/tests/main.cpp)
target_link_libraries(xrpilot-core-tests PRIVATE xrpilot_core)
foreach(name IN ITEMS assembler.complete-frame assembler.fec-recovers-one-loss assembler.two-losses-drop
                      png.round-trip png.corrupt-crc-rejected
                      agent.trigger-reaches-packet agent.release-clears-input agent.absent-hands-clear-flags
                      commands.malformed-rejected commands.unknown-rejected commands.state-reports-head
                      commands.json-string-escapes json.round-trip-keeps-order-and-numbers json.set-and-erase
                      json.malformed-rejected spans.lines-parse spans.begin-end-and-children spans.repeats-duck spans.nested-fold-and-expand traces.etnf-maskscore-rows traces.rectgtn-time traces.null-check-control body.reach-within-the-arm body.reach-past-the-arm-is-clamped body.feet-planted-in-the-world body.feet-unplanted-slide rotation.euler-orders-match-axis-products
                      rotation.quaternion-round-trip rotation.non-rotations-refused
                      commands.head-takes-a-rotation-matrix commands.state-reports-runtime-fov)
    add_test(NAME ${name} COMMAND xrpilot-core-tests ${name})
endforeach()
# Installing OXRSys into the per-user folders, MPL-2.0 like the tray it serves; no SDL, so CI tests it.
add_library(xrpilot_install STATIC tray/src/Install.cpp)
target_include_directories(xrpilot_install PUBLIC tray/include)
target_link_libraries(xrpilot_install PUBLIC xrpilot_core)
add_executable(xrpilot-install-tests tray/tests/install.cpp)
target_link_libraries(xrpilot-install-tests PRIVATE xrpilot_install)
foreach(name IN ITEMS install.complete-source-installs install.files-identical-to-source
                      install.changed-source-is-recopied install.missing-driver-writes-nothing
                      install.missing-runtime-refused install.stale-manifest-is-not-an-install)
    add_test(NAME ${name} COMMAND xrpilot-install-tests ${name})
endforeach()

add_executable(xrpilot-human-tests core/tests/human.cpp)
target_link_libraries(xrpilot-human-tests PRIVATE xrpilot_core)
foreach(name IN ITEMS human.walk-moves-head-shift-moves-hand human.mouse-look-not-inverted human.touch-keys
                      human.controllers-flag human.hands-aim-at-gaze human.agent-hand-overrides-body
                      human.seated-lowers-head-and-hands
                      sparkline.deltas-over-ten-seconds sparkline.fault-colour-follows-its-sample
                      sparkline.p90-scale-clips-a-spike)
    add_test(NAME ${name} COMMAND xrpilot-human-tests ${name})
endforeach()

if(XRPILOT_BUILD_OXRSYS)
    add_subdirectory(oxrsys)
endif()

if(NOT XRPILOT_BUILD_APP)
    return()
endif()

# The app shares the runtime's PyroWave (MIT) and Granite subset.
if(NOT TARGET oxrsys_pyrowave)
    include(${XRPILOT_OXRSYS_DIR}/cmake/PyroWave.cmake)
endif()
include(FetchContent)
FetchContent_GetProperties(granite)

# panelspun (SDL3 + ThorVG), subtreed; one copy of the Vulkan headers for every target.
set(PANELSPUN_VULKAN_HEADERS_DIR ${granite_SOURCE_DIR}/third_party/khronos/vulkan-headers/include CACHE PATH "" FORCE)
set(PANELSPUN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PANELSPUN_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(PANELSPUN_SDL_TRAY ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/panelspun)

add_library(xrpilot_gpu STATIC core/src/GpuDecoder.cpp)
target_include_directories(xrpilot_gpu PUBLIC core/include PRIVATE ${XRPILOT_OXRSYS_DIR}/kernels/simulator/spirv)
target_link_libraries(xrpilot_gpu PUBLIC oxrsys_pyrowave)

# A kept trace frame through scene-linear RGB (for OpenEXR) and back into PyroWave.
add_executable(trace_roundtrip tools/trace_roundtrip.cpp)
target_link_libraries(trace_roundtrip PRIVATE xrpilot_gpu xrpilot_core)
target_compile_definitions(xrpilot_gpu PUBLIC VK_NO_PROTOTYPES)

# The OXRSys tray, MPL-2.0 like the Home tray it is ported from.
add_library(xrpilot_tray STATIC tray/src/Tray.cpp)
target_include_directories(xrpilot_tray PUBLIC tray/include)
target_link_libraries(xrpilot_tray PUBLIC xrpilot_install panelspun)

# A GUI program, so a launch from the shell or the tray opens the pilot window and no console; the
# MCP server still talks to it over the stdin and stdout pipes it hands over.
add_executable(xr-pilot WIN32 app/main.cpp)
target_link_libraries(xr-pilot PRIVATE xrpilot_net xrpilot_gpu xrpilot_tray panelspun)
add_custom_command(TARGET xr-pilot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/tray/resources $<TARGET_FILE_DIR:xr-pilot>/resources)
