include(GoogleTest)

add_executable(gridlane_unit_tests
  unit/test_glue.cpp
  unit/test_control.cpp
  unit/test_fabric.cpp
  unit/test_collective.cpp
  unit/test_gray_scott.cpp
  unit/test_version.cpp
)

target_link_libraries(gridlane_unit_tests PRIVATE gridlane::gridlane gridlane_gray_scott gridlane::control_coordinator
  gridlane::control_worker GTest::gtest GTest::gtest_main)

gridlane_set_warnings(gridlane_unit_tests)
gridlane_enable_sanitizers(gridlane_unit_tests)
gridlane_enable_coverage(gridlane_unit_tests)
gridlane_apply_sanitizer_test_wrapper(gridlane_unit_tests)

gtest_discover_tests(gridlane_unit_tests)

gridlane_add_coverage_target()
if(TARGET coverage)
  # The 'coverage' custom target has no implicit dependency on the test
  # binaries; without this, a clean build runs ctest before
  # gridlane_unit_tests exists and every test reports NOT_BUILT.
  add_dependencies(coverage gridlane_unit_tests)
  # The end-to-end tests run the demonstrator's programs.
  foreach(program gridlane_gray_scott_serial gridlane_worker gridlane_coordinator)
    if(TARGET ${program})
      add_dependencies(coverage ${program})
    endif()
  endforeach()
endif()

# The demonstrator against the Python reference (needs Python 3 with numpy).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} -c "import numpy"
    RESULT_VARIABLE gridlane_numpy_missing OUTPUT_QUIET ERROR_QUIET)
endif()
# Not under sanitizers: the comparison checks arithmetic, and the instrumented
# binary started by Python would need the sanitizer's own runtime setup.
if(Python3_FOUND AND NOT gridlane_numpy_missing AND NOT GRIDLANE_SANITIZER)
  add_test(NAME gray_scott.serial_vs_python
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/python/compare.py
      --exec $<TARGET_FILE:gridlane_gray_scott_serial>
      --width 64 --height 48 --steps 300 --every 100 --seed 7)

  # End to end: the coordinator and worker processes on this machine, compared
  # with the reference. An uneven layout over TCP, then the portability demo
  # (the same job deployed over TCP and over shared memory, fields equal).
  set(gridlane_apps_dir $<TARGET_FILE_DIR:gridlane_worker>)
  add_test(NAME gray_scott.e2e_uneven_tcp
    COMMAND ${PROJECT_SOURCE_DIR}/scripts/run_local.sh --bin ${gridlane_apps_dir}
      --layout 0:0:1,1:0:3,1:1:2 --sim 4 --analysis 2 --link tcp
      --width 96 --height 64 --steps 300 --every 100 --seed 3
      --out ${CMAKE_CURRENT_BINARY_DIR}/e2e/uneven --compare)
  add_test(NAME gray_scott.portability
    COMMAND ${PROJECT_SOURCE_DIR}/scripts/portability_demo.sh --bin ${gridlane_apps_dir}
      --width 96 --height 64 --steps 300
      --out ${CMAKE_CURRENT_BINARY_DIR}/e2e/portability)
  # Many busy processes: alone, so that timing-sensitive tests are not starved.
  set_tests_properties(gray_scott.e2e_uneven_tcp gray_scott.portability
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 300 LABELS e2e)
else()
  message(STATUS "gridlane: Python 3 with numpy not found; skipping the reference comparison")
endif()
