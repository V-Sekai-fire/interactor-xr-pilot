# SPDX-License-Identifier: Apache-2.0 OR MIT
cmake_minimum_required(VERSION 3.24)
project(xr_pilot VERSION 0.1.0 LANGUAGES C CXX)

# compile_commands.json, copied to the repository root, where clang-tidy (the commit hook in
# .pre-commit-config.yaml) and editors find it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_custom_target(xrpilot_sync_compile_commands ALL
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_BINARY_DIR}/compile_commands.json
        -DOUTPUT_FILE=${CMAKE_SOURCE_DIR}/compile_commands.json
        -P ${CMAKE_SOURCE_DIR}/oxrsys/cmake/SyncCompileCommands.cmake
    COMMENT "Copying compile_commands.json to the repository root"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The version comes from the release tag; see cmake/XrPilotVersion.cmake.
include(cmake/XrPilotVersion.cmake)
set(XRPILOT_BASE_VERSION ${PROJECT_VERSION})
xrpilot_read_version(${CMAKE_CURRENT_SOURCE_DIR})
message(STATUS "XR Pilot ${XRPILOT_VERSION} (installer ${XRPILOT_MSI_VERSION})")
configure_file(core/include/xrpilot/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/xrpilot/Version.h @ONLY)

option(XRPILOT_BUILD_APP "Build the xr-pilot window (needs a display and Vulkan)" ON)

option(XRPILOT_BUILD_OXRSYS "Build the OXRSys runtime and SteamVR driver in oxrsys/ (needs Vulkan)" ${XRPILOT_BUILD_APP})

# The OXRSys wire protocol and the Lean-authored yuv420_to_rgbx kernel.
set(XRPILOT_OXRSYS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/oxrsys)
set(XRPILOT_PROTOCOL_INCLUDE ${XRPILOT_OXRSYS_DIR}/common/protocol/include)

add_library(xrpilot_core STATIC
    core/src/Agent.cpp
    core/src/Body.cpp
    core/src/Makima.cpp
    third_party/sinew-align/sinew_align.c
    core/src/CommandLoop.cpp
    core/src/Commands.cpp
    core/src/FrameAssembler.cpp
    core/src/HeadGizmo.cpp
    core/src/HumanInput.cpp
    core/src/Json.cpp
    core/src/Launch.cpp
    core/src/McpLink.cpp
    core/src/MouseLook.cpp
    core/src/UiTrace.cpp
    core/src/Pace.cpp
    core/src/Pump.cpp
    core/src/Png.cpp
    core/src/Rotation.cpp
    core/src/SpanLog.cpp
    core/src/StreamRecord.cpp
    core/src/TraceStore.cpp
    core/src/Theme.cpp
    third_party/sqlite/sqlite3.c
    core/src/Sparkline.cpp
)
target_include_directories(xrpilot_core PUBLIC core/include ${CMAKE_CURRENT_BINARY_DIR}/generated ${XRPILOT_PROTOCOL_INCLUDE} third_party/sqlite
    PRIVATE third_party/sinew-align)
target_compile_definitions(xrpilot_core PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_FOREIGN_KEYS=1)
if(WIN32)
    # The MCP token: BCryptGenRandom, and a user-only DACL on its file.
    target_link_libraries(xrpilot_core PUBLIC bcrypt advapi32)
endif()

add_library(xrpilot_net STATIC core/src/Client.cpp)
target_link_libraries(xrpilot_net PUBLIC xrpilot_core)
if(WIN32)
    target_link_libraries(xrpilot_net PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(xrpilot_net PUBLIC Threads::Threads)
endif()

enable_testing()
add_test(NAME version.tag-ladder COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/version.cmake)
add_executable(xrpilot-core-tests core/tests/main.cpp)
target_link_libraries(xrpilot-core-tests PRIVATE xrpilot_net)
foreach(name IN ITEMS assembler.complete-frame assembler.fec-recovers-one-loss assembler.two-losses-drop
                      assembler.render-pose-joins-frame assembler.stale-packet-never-joins-a-frame
                      png.round-trip png.corrupt-crc-rejected
                      agent.trigger-reaches-packet agent.release-clears-input agent.absent-hands-clear-flags
                      commands.malformed-rejected commands.unknown-rejected commands.state-reports-head
                      commands.json-string-escapes json.round-trip-keeps-order-and-numbers json.set-and-erase
                      json.malformed-rejected spans.lines-parse spans.begin-end-and-children spans.repeats-duck spans.nested-fold-and-expand traces.etnf-decider-rows traces.rectgtn-time traces.null-check-control body.reach-within-the-arm body.reach-past-the-arm-is-clamped body.feet-planted-in-the-world body.feet-unplanted-slide rotation.euler-orders-match-axis-products
                      rotation.quaternion-round-trip rotation.non-rotations-refused
                      commands.head-takes-a-rotation-matrix commands.state-reports-runtime-fov
                      agent.headset-samples-keep-their-time agent.unsampled-head-sends-every-tick
                      makima.holds-flat-and-does-not-overshoot hand.hops-make-one-monotone-motion hand.momentum-carries-through-a-hop
                      client.stays-with-a-streaming-runtime
                      launch.plain-start-opens-the-pilot launch.second-start-shows-the-first
                      launch.flags-keep-their-meaning launch.mcp-flags client.local-runtime-wins
                      mcp.token-is-random-and-kept mcp.client-config-names-the-port-and-token
                      client.local-address-is-recognised client.second-pilot-is-refused
                      ui.marks-take-no-frame ui.spans-export-as-lines ui.sample-run
                      mouse.look-turns-evenly mouse.look-lands-whole
                      commands.loop-normalizes-lines bridge.wakes-the-tray-and-waits bridge.no-pilot-is-an-error
                      bridge.detach-waits-for-the-command
                      tracks.pick-keeps-the-rest-of-the-file tracks.pick-adds-what-is-missing tracks.named-not-numbered)
    add_test(NAME ${name} COMMAND xrpilot-core-tests ${name})
endforeach()
# The 15.6 ms tick it guards against is Windows'; elsewhere a throttled shell (background QoS) fails it.
foreach(name IN ITEMS pace.ticks-evenly-at-90hz)
    if(NOT WIN32)
        continue()
    endif()
    add_test(NAME ${name} COMMAND xrpilot-core-tests ${name})
endforeach()
# Installing OXRSys into the per-user folders, MPL-2.0 like the tray it serves; no SDL, so CI tests it.
# Exports a trace's UI spans for the Lean UI model's checker (lake exe ui_check).
add_executable(xrpilot-ui-trace tools/ui_trace.cpp)
target_link_libraries(xrpilot-ui-trace PRIVATE xrpilot_core)

add_library(xrpilot_install STATIC tray/src/Install.cpp tray/src/SteamVrSettings.cpp)
target_include_directories(xrpilot_install PUBLIC tray/include)
target_link_libraries(xrpilot_install PUBLIC xrpilot_core)
add_executable(xrpilot-install-tests tray/tests/install.cpp)
target_link_libraries(xrpilot-install-tests PRIVATE xrpilot_install)
foreach(name IN ITEMS install.complete-source-installs install.files-identical-to-source
                      install.changed-source-is-recopied install.missing-driver-writes-nothing
                      install.missing-runtime-refused install.stale-manifest-is-not-an-install
                      steamvr.swap-forces-oxrsys-with-multiple-drivers steamvr.hand-back-restores-the-desk)
    add_test(NAME ${name} COMMAND xrpilot-install-tests ${name})
endforeach()

# Frame-sync reliability trials, and the same trials over a planted index-only pose match that must fail.
add_executable(xrpilot-sync-trials core/tests/sync_trials.cpp)
target_link_libraries(xrpilot-sync-trials PRIVATE xrpilot_core)
add_executable(xrpilot-sync-trials-planted core/tests/sync_trials.cpp core/src/FrameAssembler.cpp)
target_include_directories(xrpilot-sync-trials-planted PRIVATE core/include ${XRPILOT_PROTOCOL_INCLUDE})
target_compile_definitions(xrpilot-sync-trials-planted PRIVATE XRPILOT_PLANT_INDEX_ONLY)
find_package(Threads REQUIRED)
target_link_libraries(xrpilot-sync-trials PRIVATE Threads::Threads)
target_link_libraries(xrpilot-sync-trials-planted PRIVATE Threads::Threads)
add_test(NAME sync.no-wrong-pairing COMMAND xrpilot-sync-trials 2000000 4)
add_test(NAME sync.control-index-only-fails COMMAND xrpilot-sync-trials-planted 2000000 4)
set_tests_properties(sync.control-index-only-fails PROPERTIES WILL_FAIL TRUE)
add_test(NAME sync.control-unposed-fails COMMAND xrpilot-sync-trials 2000000 4 1 --plant-unposed)
set_tests_properties(sync.control-unposed-fails PROPERTIES WILL_FAIL TRUE)

add_executable(xrpilot-record-tests core/tests/record.cpp)
target_link_libraries(xrpilot-record-tests PRIVATE xrpilot_core)
target_compile_definitions(xrpilot-record-tests PRIVATE RECORD_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
foreach(name IN ITEMS record.round-trip record.damage-is-named)
    add_test(NAME ${name} COMMAND xrpilot-record-tests ${name})
endforeach()

add_executable(xrpilot-human-tests core/tests/human.cpp)
target_link_libraries(xrpilot-human-tests PRIVATE xrpilot_core)
foreach(name IN ITEMS human.gizmo-straight-up-and-down human.gizmo-arcs-measure-the-angles human.tracking-period-follows-refresh human.walk-on-sender-never-repeats human.wheel-sets-walk-speed human.shift-wheel-sets-hand-reach human.walk-moves-head-shift-moves-hand human.mouse-look-not-inverted human.touch-keys
                      human.controllers-flag human.hands-aim-at-gaze human.agent-hand-overrides-body
                      agent.packet-carries-the-controller-pose-its-tip-aims-from human.seated-lowers-head-and-hands
                      human.gizmo-axes-right-handed human.gizmo-front-faces-ccw human.theme-contrast human.theme-appearance
                      sparkline.deltas-over-ten-seconds sparkline.fault-colour-follows-its-sample
                      sparkline.p90-scale-clips-a-spike sparkline.model-runs sparkline.zero-clears-the-base
                      sparkline.ticks-move-every-sample sparkline.pump-samples-without-frames)
    add_test(NAME ${name} COMMAND xrpilot-human-tests ${name})
endforeach()

if(XRPILOT_BUILD_OXRSYS)
    add_subdirectory(oxrsys)
endif()

# Plausible-witness-dag property and falsification tests over the pure maths; witness-cpp is header-only.
add_executable(xrpilot-witness-tests core/tests/test_witness.cpp)
target_link_libraries(xrpilot-witness-tests PRIVATE xrpilot_core)
target_include_directories(xrpilot-witness-tests PRIVATE third_party/witness-cpp/include)
foreach(name IN ITEMS witness.unit-concrete-witnesses witness.euler-orthonormal witness.quaternion-orthonormal
                      witness.euler-round-trip witness.quaternion-round-trip witness.multiply-closed
                      witness.reach-lands witness.reach-clamped witnessdag.ablation-coverage)
    add_test(NAME ${name} COMMAND xrpilot-witness-tests ${name})
endforeach()

# xr-pilot-frame: the client for a standalone Linux headset such as the Steam Frame, built on the headset.
option(XRPILOT_BUILD_FRAME "Build xr-pilot-frame, the OpenXR client for a standalone Linux headset" OFF)
if(XRPILOT_BUILD_FRAME)
    if(NOT TARGET oxrsys_pyrowave)
        include(${XRPILOT_OXRSYS_DIR}/cmake/PyroWave.cmake)
    endif()
    include(FetchContent)
    if(NOT TARGET openxr_loader)
        FetchContent_Declare(OpenXR GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
                             GIT_TAG release-1.1.57 GIT_SHALLOW ON)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(OpenXR)
    endif()
    if(NOT TARGET xrpilot_gpu)
        add_library(xrpilot_gpu STATIC core/src/GpuDecoder.cpp)
        target_include_directories(xrpilot_gpu PUBLIC core/include PRIVATE ${XRPILOT_OXRSYS_DIR}/kernels/simulator/spirv)
        target_link_libraries(xrpilot_gpu PUBLIC oxrsys_pyrowave)
        target_compile_definitions(xrpilot_gpu PUBLIC VK_NO_PROTOTYPES)
    endif()
    add_executable(xr-pilot-frame frame/main.cpp)
    target_include_directories(xr-pilot-frame PRIVATE ${openxr_SOURCE_DIR}/include)
    target_link_libraries(xr-pilot-frame PRIVATE xrpilot_net xrpilot_gpu openxr_loader)
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

if(NOT TARGET xrpilot_gpu)
    add_library(xrpilot_gpu STATIC core/src/GpuDecoder.cpp)
    target_include_directories(xrpilot_gpu PUBLIC core/include PRIVATE ${XRPILOT_OXRSYS_DIR}/kernels/simulator/spirv)
    target_link_libraries(xrpilot_gpu PUBLIC oxrsys_pyrowave)
endif()

# A kept trace frame through scene-linear RGB (for OpenEXR) and back into PyroWave.
add_executable(trace_roundtrip tools/trace_roundtrip.cpp)
target_link_libraries(trace_roundtrip PRIVATE xrpilot_gpu xrpilot_core)
target_compile_definitions(xrpilot_gpu PUBLIC VK_NO_PROTOTYPES)

# The subtreed CineForm SDK adds frames2cfhd and pyro2cfhd; an empty path leaves them out.
set(XRPILOT_CINEFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cineform-sdk" CACHE PATH
    "CineForm SDK for frames2cfhd and pyro2cfhd")
if(XRPILOT_CINEFORM_DIR)
    set(CMAKE_DISABLE_FIND_PACKAGE_OpenMP ON)
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BUILD_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(BUILD_SEPARATED OFF CACHE BOOL "" FORCE)
    add_subdirectory(${XRPILOT_CINEFORM_DIR} cineform-sdk EXCLUDE_FROM_ALL)
    if(NOT MSVC)
        target_compile_options(CFHDCodecStatic PRIVATE -w
            $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types>)
    endif()
    add_executable(frames2cfhd tools/frames2cfhd.cpp)
    target_include_directories(frames2cfhd PRIVATE ${XRPILOT_CINEFORM_DIR}/Common)
    target_link_libraries(frames2cfhd PRIVATE CFHDCodecStatic)
    add_executable(pyro2cfhd tools/pyro2cfhd.cpp)
    target_include_directories(pyro2cfhd PRIVATE ${XRPILOT_CINEFORM_DIR}/Common)
    target_link_libraries(pyro2cfhd PRIVATE CFHDCodecStatic xrpilot_gpu xrpilot_core)
endif()

# The OXRSys tray, MPL-2.0 like the Home tray it is ported from.
add_library(xrpilot_tray STATIC tray/src/Tray.cpp)
target_include_directories(xrpilot_tray PUBLIC tray/include)
target_link_libraries(xrpilot_tray PUBLIC xrpilot_install panelspun)

# A GUI program, so a launch from the shell or a shortcut starts the tray and no console; the
# MCP server still talks to it over the stdin and stdout pipes it hands over.
add_executable(xr-pilot WIN32 app/main.cpp)
if(WIN32)
    # Explorer's Details tab shows the version.
    string(REPLACE "." "," XRPILOT_RC_VERSION "${XRPILOT_MSI_VERSION},0")
    configure_file(app/xr-pilot.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/xr-pilot.rc @ONLY)
    target_sources(xr-pilot PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/xr-pilot.rc)
endif()
target_link_libraries(xr-pilot PRIVATE xrpilot_net xrpilot_gpu xrpilot_tray panelspun)
add_custom_command(TARGET xr-pilot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/tray/resources $<TARGET_FILE_DIR:xr-pilot>/resources)

# The Windows MSI: `cpack -G WIX` in the build tree, per-user, laid out as the tray expects (bin\, runtime\,
# driver\oxrsys\ under the install root). Only this component is packed, not the subprojects' install rules.
if(WIN32 AND XRPILOT_BUILD_OXRSYS)
    set(XRPILOT_DRIVER_ROOT ${CMAKE_CURRENT_BINARY_DIR}/oxrsys/driver/oxrsys)
    install(TARGETS xr-pilot RUNTIME DESTINATION bin COMPONENT xrpilot)
    # The MCP server the tray runs, which `lake build xr_pilot_mcp` in lean/ makes before cpack packs it.
    set(XRPILOT_MCP_EXE ${CMAKE_CURRENT_SOURCE_DIR}/lean/.lake/build/bin/xr_pilot_mcp.exe CACHE FILEPATH
        "xr_pilot_mcp for the MSI")
    install(PROGRAMS ${XRPILOT_MCP_EXE} DESTINATION bin COMPONENT xrpilot)
    install(DIRECTORY tray/resources/ DESTINATION bin/resources COMPONENT xrpilot)
    install(FILES $<TARGET_FILE:oxrsys_runtime> DESTINATION runtime COMPONENT xrpilot)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/package/oxrsys-runtime.json
        "{\"file_format_version\": \"1.0.0\", \"runtime\": {\"name\": \"OXRSys Runtime\", \"library_path\": \".\\liboxrsys-runtime.dll\"}}")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/package/oxrsys-runtime.json DESTINATION runtime COMPONENT xrpilot)
    install(FILES $<TARGET_FILE:driver_oxrsys> DESTINATION driver/oxrsys/bin/win64 COMPONENT xrpilot)
    install(FILES ${XRPILOT_DRIVER_ROOT}/driver.vrdrivermanifest DESTINATION driver/oxrsys COMPONENT xrpilot)
    install(DIRECTORY ${XRPILOT_DRIVER_ROOT}/resources DESTINATION driver/oxrsys COMPONENT xrpilot)

    file(READ LICENSE-APACHE XRPILOT_LICENSE_APACHE)
    file(READ LICENSE-MIT XRPILOT_LICENSE_MIT)
    file(READ oxrsys/LICENSE XRPILOT_LICENSE_MPL)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/package/LICENSE.txt
        "XR Pilot is Apache-2.0 OR MIT. oxrsys/ (the runtime, the driver and the tray) is MPL-2.0.\n\n"
        "${XRPILOT_LICENSE_APACHE}\n\n${XRPILOT_LICENSE_MIT}\n\n${XRPILOT_LICENSE_MPL}")
    install(FILES LICENSE-APACHE LICENSE-MIT DESTINATION licenses COMPONENT xrpilot)
    install(FILES oxrsys/LICENSE DESTINATION licenses RENAME LICENSE-MPL-2.0 COMPONENT xrpilot)
    set_property(INSTALL "bin/xr-pilot.exe" PROPERTY CPACK_START_MENU_SHORTCUTS "XR Pilot")

    set(CPACK_GENERATOR WIX)
    set(CPACK_COMPONENTS_ALL xrpilot)
    set(CPACK_COMPONENTS_ALL_IN_ONE_PACKAGE ON)
    set(CPACK_PACKAGE_NAME "XR Pilot")
    set(CPACK_PACKAGE_VENDOR "V-Sekai")
    # XR Pilot's own version, not OXRSys's (always 1.2.0), so each release upgrades the install before it.
    set(CPACK_PACKAGE_VERSION ${XRPILOT_MSI_VERSION})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Install OXRSys, pick the OpenXR runtime, and see and drive what an app shows")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Programs/XR Pilot")
    set(CPACK_PACKAGE_FILE_NAME "xr-pilot-${XRPILOT_VERSION}-x64")
    set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_BINARY_DIR}/package/LICENSE.txt)
    set(CPACK_WIX_VERSION 4)
    set(CPACK_WIX_ARCHITECTURE x64)
    set(CPACK_WIX_INSTALL_SCOPE perUser)
    set(CPACK_WIX_UPGRADE_GUID "562DAE0D-E212-40C9-9AEA-684B142610A5")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER .)
    set(CPACK_WIX_COMPONENT_KEYS_REGISTRY_PATH "Software/V-Sekai/XR Pilot/Components")
    include(CPack)
endif()
