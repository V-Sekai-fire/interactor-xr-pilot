# SPDX-License-Identifier: Apache-2.0 OR MIT
add_executable(panelspun-tests main.cpp split_tree_test.cpp)
target_link_libraries(panelspun-tests PRIVATE panelspun)

set(PANELSPUN_TEST_CASES
  layout.single-panel-fills-bounds
  layout.three-panels-tile-without-overlap
  layout.tiling-check-rejects-overlap
  handles.hit-test-finds-split-and-misses-panel
  resize.drag-moves-split
  resize.drag-past-minimum-is-clamped
  resize.minimum-holds-when-window-shrinks
  resize.unknown-node-and-leaf-refused
  dock.each-side-places-panel
  dock.refuses-duplicate-missing-target-and-bad-id
  undock.remove-collapses-parent
  undock.removing-last-panel-is-refused
  construct.invalid-first-id-throws
  serialize.round-trip-preserves-layout
  serialize.format-is-line-based
  serialize.malformed-input-rejected
  serialize.deep-nesting-rejected
)
foreach(case IN LISTS PANELSPUN_TEST_CASES)
  add_test(NAME ${case} COMMAND panelspun-tests ${case})
endforeach()

# Every case the binary carries must be registered above, so none is silently skipped.
add_test(NAME registry.every-case-registered
  COMMAND ${CMAKE_COMMAND} -DTESTS=$<TARGET_FILE:panelspun-tests> "-DEXPECTED=${PANELSPUN_TEST_CASES}"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/registry.cmake)
add_test(NAME registry.control-missing-case-fails
  COMMAND ${CMAKE_COMMAND} -DTESTS=$<TARGET_FILE:panelspun-tests> "-DEXPECTED=layout.single-panel-fills-bounds"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/registry.cmake)
set_tests_properties(registry.control-missing-case-fails PROPERTIES WILL_FAIL TRUE)

add_test(NAME all-cases COMMAND panelspun-tests)

foreach(control control.failing-check-fails control.empty-case-fails control.unknown-case-name)
  add_test(NAME ${control} COMMAND panelspun-tests ${control})
  set_tests_properties(${control} PROPERTIES WILL_FAIL TRUE)
endforeach()

# These open a window and need a Vulkan device, so CI runners leave them off.
if(PANELSPUN_GPU_TESTS AND TARGET panelspun-demo)
  set(shot ${CMAKE_CURRENT_BINARY_DIR}/demo-smoke.bmp)
  add_test(NAME demo.vulkan-region-composited
    COMMAND panelspun-demo --frames 4 --screenshot ${shot} --check --validate)
  add_test(NAME demo.control-without-vulkan-region-fails
    COMMAND panelspun-demo --frames 4 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/demo-control.bmp --check --no-vulkan-region)
  set_tests_properties(demo.control-without-vulkan-region-fails PROPERTIES WILL_FAIL TRUE)
  add_test(NAME demo.all-features-device
    COMMAND panelspun-demo --frames 2 --all-features --check-features --validate)
  add_test(NAME demo.control-default-device-lacks-features
    COMMAND panelspun-demo --frames 2 --check-features)
  set_tests_properties(demo.control-default-device-lacks-features PROPERTIES WILL_FAIL TRUE)
  add_test(NAME demo.redraw-from-worker-thread COMMAND panelspun-demo --wake-check 3)
  add_test(NAME demo.control-no-redraw-without-wake COMMAND panelspun-demo --wake-check 3 --no-wake)
  set_tests_properties(demo.control-no-redraw-without-wake PROPERTIES WILL_FAIL TRUE)
  add_test(NAME demo.update-while-minimized COMMAND panelspun-demo --wake-check 3 --minimized)
  add_test(NAME demo.control-no-update-without-wake COMMAND panelspun-demo --wake-check 3 --minimized --no-wake)
  set_tests_properties(demo.control-no-update-without-wake PROPERTIES WILL_FAIL TRUE)
  add_test(NAME demo.input-reaches-focused-panel COMMAND panelspun-demo --input-check)
  add_test(NAME demo.control-keys-need-a-focus-click COMMAND panelspun-demo --input-check --no-focus-click)
  set_tests_properties(demo.control-keys-need-a-focus-click PROPERTIES WILL_FAIL TRUE)
  add_test(NAME demo.tick-wakes-idle-loop COMMAND panelspun-demo --tick-check 30)
  add_test(NAME demo.control-no-tick-stays-idle COMMAND panelspun-demo --tick-check 30 --no-tick)
  set_tests_properties(demo.control-no-tick-stays-idle PROPERTIES WILL_FAIL TRUE)
  add_test(NAME demo.polyline-drawn
    COMMAND panelspun-demo --frames 4 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/demo-line.bmp --check --polyline-check)
  add_test(NAME demo.control-no-polyline-fails
    COMMAND panelspun-demo --frames 4 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/demo-noline.bmp --check --polyline-check --no-polyline)
  set_tests_properties(demo.control-no-polyline-fails PROPERTIES WILL_FAIL TRUE)
  add_test(NAME demo.fingers-hold-two-controls COMMAND panelspun-demo --touch-check)
  add_test(NAME demo.control-single-touch-ignores-fingers COMMAND panelspun-demo --touch-check --single-touch)
  set_tests_properties(demo.control-single-touch-ignores-fingers PROPERTIES WILL_FAIL TRUE)
  if(PANELSPUN_SDL_GAMEPAD)
    add_test(NAME demo.gamepad-reported COMMAND panelspun-demo --gamepad-check)
    add_test(NAME demo.control-gamepads-off-reports-nothing COMMAND panelspun-demo --gamepad-check --no-gamepads)
    set_tests_properties(demo.control-gamepads-off-reports-nothing PROPERTIES WILL_FAIL TRUE)
  endif()
  add_test(NAME demo.locked-layout-holds COMMAND panelspun-demo --lock-check)
  add_test(NAME demo.control-unlocked-layout-moves COMMAND panelspun-demo --lock-check --unlocked)
  set_tests_properties(demo.control-unlocked-layout-moves PROPERTIES WILL_FAIL TRUE)
  set_tests_properties(demo.vulkan-region-composited demo.control-without-vulkan-region-fails
    demo.all-features-device demo.control-default-device-lacks-features
    demo.redraw-from-worker-thread demo.control-no-redraw-without-wake
    demo.update-while-minimized demo.control-no-update-without-wake
    demo.input-reaches-focused-panel demo.control-keys-need-a-focus-click
    demo.tick-wakes-idle-loop demo.control-no-tick-stays-idle
    demo.polyline-drawn demo.control-no-polyline-fails
    demo.locked-layout-holds demo.control-unlocked-layout-moves PROPERTIES RUN_SERIAL TRUE)
endif()
