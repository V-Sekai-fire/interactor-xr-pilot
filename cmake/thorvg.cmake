# SPDX-License-Identifier: Apache-2.0 OR MIT
# ThorVG ships a meson build; this target compiles the same sources for the CPU engine
# with the SVG and TTF loaders, matching meson -Dengines=cpu -Dloaders=svg,ttf -Dthreads=false.
set(TVG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/thorvg)
set(TVG_SRC ${TVG_DIR}/src)

file(READ ${TVG_DIR}/meson.build TVG_MESON)
string(REGEX MATCH "version : '([0-9.]+)'" _ ${TVG_MESON})
set(TVG_VERSION ${CMAKE_MATCH_1})

set(TVG_CONFIG_DIR ${CMAKE_CURRENT_BINARY_DIR}/thorvg-config)
file(CONFIGURE OUTPUT ${TVG_CONFIG_DIR}/config.h CONTENT
"#pragma once
#define THORVG_VERSION_STRING \"${TVG_VERSION}\"
#define THORVG_CPU_ENGINE_SUPPORT 1
#define THORVG_PARTIAL_RENDER_SUPPORT 1
#define THORVG_SVG_LOADER_SUPPORT 1
#define THORVG_SFNT_LOADER_SUPPORT 1
#define THORVG_TTF_LOADER_SUPPORT 1
#define THORVG_FILE_IO_SUPPORT 1
#define WIN32_LEAN_AND_MEAN 1
" @ONLY)

file(GLOB TVG_SOURCES
  ${TVG_SRC}/common/*.cpp
  ${TVG_SRC}/renderer/*.cpp
  ${TVG_SRC}/renderer/cpu_engine/*.cpp
  ${TVG_SRC}/loaders/raw/*.cpp
  ${TVG_SRC}/loaders/svg/*.cpp
)
list(APPEND TVG_SOURCES
  ${TVG_SRC}/loaders/sfnt/tvgSfntLoader.cpp
  ${TVG_SRC}/loaders/sfnt/tvgSfntReader.cpp
  ${TVG_SRC}/loaders/sfnt/tvgTtfReader.cpp
)

add_library(thorvg STATIC ${TVG_SOURCES})
set_target_properties(thorvg PROPERTIES CXX_STANDARD 14 CXX_EXTENSIONS OFF)
target_include_directories(thorvg SYSTEM PUBLIC ${TVG_DIR}/inc)
target_include_directories(thorvg PRIVATE
  ${TVG_CONFIG_DIR}
  ${TVG_SRC}/common
  ${TVG_SRC}/renderer
  ${TVG_SRC}/renderer/cpu_engine
  ${TVG_SRC}/loaders/raw
  ${TVG_SRC}/loaders/svg
  ${TVG_SRC}/loaders/sfnt
)
target_compile_definitions(thorvg PUBLIC TVG_STATIC)
if(WIN32)
  target_compile_definitions(thorvg PRIVATE NOMINMAX)
endif()
if(MSVC)
  target_compile_options(thorvg PRIVATE /W0 /EHs-c- /GR-)
  target_compile_definitions(thorvg PRIVATE _HAS_EXCEPTIONS=0)
else()
  target_compile_options(thorvg PRIVATE -w -fno-exceptions -fno-rtti -fno-math-errno)
endif()
