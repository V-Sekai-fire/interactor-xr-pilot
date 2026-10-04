# SPDX-License-Identifier: Apache-2.0 OR MIT
cmake_minimum_required(VERSION 3.24)
project(panelspun VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PANELSPUN_BUILD_TESTS "Build the panelspun tests" ${PROJECT_IS_TOP_LEVEL})
option(PANELSPUN_BUILD_DEMO "Build panelspun-demo" ${PROJECT_IS_TOP_LEVEL})
option(PANELSPUN_GPU_TESTS "Register demo tests that need a display and a Vulkan device" OFF)
set(PANELSPUN_VULKAN_HEADERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/src/video/khronos
    CACHE PATH "Directory holding vulkan/vulkan_core.h")

# SDL3, static, with only the video, events and Vulkan surface paths.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
foreach(sub AUDIO GPU RENDER CAMERA JOYSTICK HAPTIC HIDAPI POWER SENSOR DIALOG TRAY OPENGL OPENGLES)
  set(SDL_${sub} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL_VIDEO ON CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

include(cmake/thorvg.cmake)

set(PANELSPUN_FONT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/inter/Inter-Regular.ttf)
set(PANELSPUN_FONT_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/inter_regular_ttf.cpp)
add_custom_command(
  OUTPUT ${PANELSPUN_FONT_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${PANELSPUN_FONT} -DOUTPUT=${PANELSPUN_FONT_CPP} -DSYMBOL=inter_regular_ttf
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${PANELSPUN_FONT} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  VERBATIM)

add_library(panelspun STATIC
  src/split_tree.cpp
  src/font.cpp
  src/vulkan_presenter.cpp
  src/widgets.cpp
  src/window.cpp
  ${PANELSPUN_FONT_CPP}
)
target_include_directories(panelspun PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${PANELSPUN_VULKAN_HEADERS_DIR})
target_compile_definitions(panelspun PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(panelspun PUBLIC SDL3::SDL3-static PRIVATE thorvg)
if(MSVC)
  target_compile_options(panelspun PRIVATE /W4 /permissive-)
else()
  target_compile_options(panelspun PRIVATE -Wall -Wextra)
endif()

if(PANELSPUN_BUILD_DEMO)
  add_executable(panelspun-demo WIN32 examples/demo/main.cpp)
  target_link_libraries(panelspun-demo PRIVATE panelspun)
endif()

if(PANELSPUN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
