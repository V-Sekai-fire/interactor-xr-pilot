# SPDX-License-Identifier: Apache-2.0 OR MIT
cmake_minimum_required(VERSION 3.24)
project(xr_pilot VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(XRPILOT_BUILD_APP "Build the xr-pilot window (needs a display and Vulkan)" ON)

include(FetchContent)

# The OXRSys wire protocol and the Lean-authored yuv420_to_rgbx kernel, read from oxrsys at a pinned commit.
FetchContent_Declare(
    oxrsys
    GIT_REPOSITORY https://github.com/V-Sekai-fire/interactor-oxrsys.git
    GIT_TAG 7e325940e6ba3be7de65b3dbb5d52006350b4994
    SOURCE_SUBDIR none
)
# PyroWave's C API (MIT) and the Granite subset it pins, at the commits oxrsys uses.
FetchContent_Declare(
    pyrowave
    GIT_REPOSITORY https://github.com/V-Sekai-fire/pyrowave.git
    GIT_TAG 89f7e47d4abbf650c91fae766728af866c5e32a0
    SOURCE_SUBDIR none
)
FetchContent_Declare(
    granite
    GIT_REPOSITORY https://github.com/Themaister/Granite.git
    GIT_TAG 1b2d1801d2910fb09ebcded2f0bb3a3a781103b5
    GIT_SUBMODULES third_party/volk third_party/khronos/vulkan-headers
    SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(oxrsys)
set(XRPILOT_PROTOCOL_INCLUDE ${oxrsys_SOURCE_DIR}/common/protocol/include)

add_library(xrpilot_core STATIC
    core/src/Agent.cpp
    core/src/Commands.cpp
    core/src/FrameAssembler.cpp
    core/src/HumanInput.cpp
    core/src/Json.cpp
    core/src/Png.cpp
    core/src/Rotation.cpp
    core/src/SpanLog.cpp
    core/src/Sparkline.cpp
)
target_include_directories(xrpilot_core PUBLIC core/include ${XRPILOT_PROTOCOL_INCLUDE})

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
                      json.malformed-rejected spans.lines-parse spans.begin-end-and-children spans.repeats-duck spans.nested-fold-and-expand rotation.euler-orders-match-axis-products
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

if(NOT XRPILOT_BUILD_APP)
    return()
endif()

FetchContent_MakeAvailable(pyrowave granite)
set(GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER OFF CACHE BOOL "" FORCE)
set(GRANITE_SHADER_COMPILER_OPTIMIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_POSITION_INDEPENDENT ON CACHE BOOL "" FORCE)
set(GRANITE_SHIPPING ON CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SPIRV_CROSS OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SYSTEM_HANDLES OFF CACHE BOOL "" FORCE)
set(GRANITE_RENDERER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_FOSSILIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_PLATFORM "null" CACHE STRING "" FORCE)
add_subdirectory(${granite_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/granite-build EXCLUDE_FROM_ALL)

add_library(xrpilot_pyrowave STATIC
    ${pyrowave_SOURCE_DIR}/pyrowave_encoder.cpp
    ${pyrowave_SOURCE_DIR}/pyrowave_decoder.cpp
    ${pyrowave_SOURCE_DIR}/pyrowave_common.cpp
    ${pyrowave_SOURCE_DIR}/pyrowave_c.cpp
    ${granite_SOURCE_DIR}/video/scaler.cpp
)
target_include_directories(xrpilot_pyrowave
    PUBLIC ${pyrowave_SOURCE_DIR}
    PRIVATE ${pyrowave_SOURCE_DIR}/shaders ${granite_SOURCE_DIR}/video
)
target_link_libraries(xrpilot_pyrowave PUBLIC granite-vulkan granite-math)
target_compile_definitions(xrpilot_pyrowave PRIVATE PYROWAVE_PRECISION=1)
if(MSVC)
    target_compile_options(xrpilot_pyrowave PRIVATE /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4309 /wd4005)
endif()

# panelspun (SDL3 + ThorVG), subtreed; one copy of the Vulkan headers for every target.
set(PANELSPUN_VULKAN_HEADERS_DIR ${granite_SOURCE_DIR}/third_party/khronos/vulkan-headers/include CACHE PATH "" FORCE)
set(PANELSPUN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PANELSPUN_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(PANELSPUN_SDL_TRAY ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/panelspun)

add_library(xrpilot_gpu STATIC core/src/GpuDecoder.cpp)
target_include_directories(xrpilot_gpu PUBLIC core/include PRIVATE ${oxrsys_SOURCE_DIR}/kernels/simulator/spirv)
target_link_libraries(xrpilot_gpu PUBLIC xrpilot_pyrowave)
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
