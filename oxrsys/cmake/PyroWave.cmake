# SPDX-License-Identifier: MPL-2.0
#
# ${OXRSYS_PYROWAVE_TARGET}: PyroWave's C API (MIT), linked statically with the Granite subset its
# checkout_granite.sh pins, for the Windows and Linux runtime and the Android client.

# The target this file builds: oxrsys_pyrowave, unless the includer names another (the macOS app
# builds the Vulkan library as xrpilot_pyrowave_vk beside the runtime's Metal oxrsys_pyrowave).
if(NOT DEFINED OXRSYS_PYROWAVE_TARGET)
    set(OXRSYS_PYROWAVE_TARGET oxrsys_pyrowave)
endif()
include(FetchContent)

FetchContent_Declare(
    pyrowave
    GIT_REPOSITORY https://github.com/V-Sekai-fire/pyrowave.git
    GIT_TAG 2a0db384be579e0f92639fc30259a6f8d3dc4eab
    SOURCE_SUBDIR none
)
FetchContent_Declare(
    granite
    # Themaister/Granite 1b2d1801 plus one commit that lets util/timer.cpp build on macOS.
    GIT_REPOSITORY https://github.com/V-Sekai-fire/Granite.git
    GIT_TAG 0e9b26e2d0e6cc9b5ab1184782b0635571080c8e
    GIT_SUBMODULES third_party/volk third_party/khronos/vulkan-headers
    SOURCE_SUBDIR none
)
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
if(NOT TARGET granite-vulkan)
    add_subdirectory(${granite_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/granite-build EXCLUDE_FROM_ALL)
endif()

add_library(${OXRSYS_PYROWAVE_TARGET} STATIC
    ${pyrowave_SOURCE_DIR}/pyrowave_encoder.cpp
    ${pyrowave_SOURCE_DIR}/pyrowave_decoder.cpp
    ${pyrowave_SOURCE_DIR}/pyrowave_common.cpp
    ${pyrowave_SOURCE_DIR}/pyrowave_c.cpp
    ${granite_SOURCE_DIR}/video/scaler.cpp
)
target_include_directories(${OXRSYS_PYROWAVE_TARGET}
    PUBLIC ${pyrowave_SOURCE_DIR}
    PRIVATE ${pyrowave_SOURCE_DIR}/shaders ${granite_SOURCE_DIR}/video
)
target_link_libraries(${OXRSYS_PYROWAVE_TARGET} PUBLIC granite-vulkan granite-math)
# Upstream's default PYROWAVE_FP32_MATH: FP32 arithmetic, reduced-range storage.
target_compile_definitions(${OXRSYS_PYROWAVE_TARGET} PRIVATE PYROWAVE_PRECISION=1)
set_target_properties(${OXRSYS_PYROWAVE_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
if(MSVC)
    target_compile_options(${OXRSYS_PYROWAVE_TARGET} PRIVATE /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4309 /wd4005)
endif()
