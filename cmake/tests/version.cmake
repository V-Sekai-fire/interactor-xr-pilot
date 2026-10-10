# SPDX-License-Identifier: Apache-2.0 OR MIT
# cmake -P cmake/tests/version.cmake: the release tag maps to a display version and an MSI version that
# rises along vX.Y.Z-dev.N -> beta.N -> rc.N -> vX.Y.Z, so each installer upgrades the one before.
include(${CMAKE_CURRENT_LIST_DIR}/../XrPilotVersion.cmake)

set(failures 0)
function(expect describe display msi)
    xrpilot_version_from_describe("${describe}" got_display got_msi)
    if(NOT got_display STREQUAL display OR NOT got_msi STREQUAL msi)
        message(SEND_ERROR "${describe}: got ${got_display} / ${got_msi}, want ${display} / ${msi}")
    endif()
endfunction()

expect("v0.1.0-dev.6-0-gce35acd" "0.1.0-dev.6" "0.1.6")
expect("v0.1.0-dev.6-3-gd3abdcc" "0.1.0-dev.6+3.gd3abdcc" "0.1.6")
expect("v0.1.0-dev.6-3-gd3abdcc-dirty" "0.1.0-dev.6+3.gd3abdcc.dirty" "0.1.6")
expect("v0.1.0-beta.1-0-gaaaaaaa" "0.1.0-beta.1" "0.1.251")
expect("v0.1.0-rc.2-0-gaaaaaaa" "0.1.0-rc.2" "0.1.502")
expect("v0.1.0-0-gaaaaaaa" "0.1.0" "0.1.750")
expect("v0.1.1-dev.1-0-gaaaaaaa" "0.1.1-dev.1" "0.1.1001")
expect("v1.0.0-0-gaaaaaaa" "1.0.0" "1.0.750")
# No tag (a source archive): the project version, marked as unreleased.
expect("" "0.1.0-unreleased" "0.1.0")

# Each step of the ladder installs over the one before.
set(ladder "v0.1.0-dev.5-0-ga" "v0.1.0-dev.6-0-ga" "v0.1.0-beta.1-0-ga" "v0.1.0-rc.1-0-ga" "v0.1.0-0-ga"
           "v0.1.1-dev.1-0-ga" "v0.2.0-dev.1-0-ga" "v1.0.0-0-ga")
set(previous "")
foreach(tag IN LISTS ladder)
    xrpilot_version_from_describe("${tag}" d m)
    if(previous AND NOT m VERSION_GREATER previous)
        message(SEND_ERROR "${tag} -> ${m} does not rise above ${previous}")
    endif()
    set(previous "${m}")
endforeach()
