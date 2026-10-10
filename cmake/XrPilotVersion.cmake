# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# XR Pilot's version comes from its release tag (vX.Y.Z-dev.N -> beta.N -> rc.N -> vX.Y.Z).
# The display version is the tag, plus commits past it and the commit when the build is not the tag.
# Windows Installer compares only the first three fields, each the third at most 65535, so the MSI
# version folds the pre-release stage into the third: PATCH*1000 + stage*250 + N, with dev 0, beta 1,
# rc 2 and the release 3. 0.1.0-dev.6 installs as 0.1.6, 0.1.0 as 0.1.750, and every step upgrades the last.

set(XRPILOT_BASE_VERSION 0.1.0)

function(xrpilot_version_from_describe describe out_display out_msi)
    string(REGEX MATCH "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(-(dev|beta|rc)\\.([0-9]+))?-([0-9]+)-g([0-9a-f]+)(-dirty)?$"
           matched "${describe}")
    if(NOT matched)
        string(REPLACE "." ";" base "${XRPILOT_BASE_VERSION}")
        list(GET base 0 major)
        list(GET base 1 minor)
        set(${out_display} "${XRPILOT_BASE_VERSION}-unreleased" PARENT_SCOPE)
        set(${out_msi} "${major}.${minor}.0" PARENT_SCOPE)
        return()
    endif()
    set(major ${CMAKE_MATCH_1})
    set(minor ${CMAKE_MATCH_2})
    set(patch ${CMAKE_MATCH_3})
    set(stage "${CMAKE_MATCH_5}")
    set(number "${CMAKE_MATCH_6}")
    set(ahead ${CMAKE_MATCH_7})
    set(commit ${CMAKE_MATCH_8})
    set(dirty "${CMAKE_MATCH_9}")

    set(display "${major}.${minor}.${patch}")
    if(stage)
        string(APPEND display "-${stage}.${number}")
    endif()
    if(ahead GREATER 0 OR dirty)
        string(APPEND display "+${ahead}.g${commit}")
        if(dirty)
            string(APPEND display ".dirty")
        endif()
    endif()

    if(stage STREQUAL "dev")
        set(rank 0)
    elseif(stage STREQUAL "beta")
        set(rank 1)
    elseif(stage STREQUAL "rc")
        set(rank 2)
    else()
        set(rank 3)
        set(number 0)
    endif()
    if(number GREATER 249 OR patch GREATER 64)
        message(FATAL_ERROR "XR Pilot version ${display} does not fit an MSI version")
    endif()
    math(EXPR build "${patch} * 1000 + ${rank} * 250 + ${number}")
    set(${out_display} "${display}" PARENT_SCOPE)
    set(${out_msi} "${major}.${minor}.${build}" PARENT_SCOPE)
endfunction()

# Reads the version of the checkout at source_dir into XRPILOT_VERSION and XRPILOT_MSI_VERSION.
function(xrpilot_read_version source_dir)
    set(describe "")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --match "v[0-9]*" --long --dirty
                        WORKING_DIRECTORY ${source_dir} OUTPUT_VARIABLE describe ERROR_QUIET
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    xrpilot_version_from_describe("${describe}" display msi)
    set(XRPILOT_VERSION "${display}" PARENT_SCOPE)
    set(XRPILOT_MSI_VERSION "${msi}" PARENT_SCOPE)
endfunction()
