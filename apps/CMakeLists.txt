# The Gray-Scott demonstrator (docs/gray-scott.md).

add_library(gridlane_gray_scott STATIC gray_scott/kernel.cpp gray_scott/pipeline.cpp)
target_include_directories(gridlane_gray_scott PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gray_scott)
target_compile_features(gridlane_gray_scott PUBLIC cxx_std_17)
target_link_libraries(gridlane_gray_scott PUBLIC gridlane::gridlane)
# The specification fixes the order of every floating point operation; a
# fused multiply-add would round differently from the reference.
target_compile_options(gridlane_gray_scott PRIVATE -ffp-contract=off)
gridlane_set_warnings(gridlane_gray_scott)
gridlane_enable_sanitizers(gridlane_gray_scott)
gridlane_enable_coverage(gridlane_gray_scott)

add_executable(gridlane_gray_scott_serial gray_scott/serial_main.cpp)
target_link_libraries(gridlane_gray_scott_serial PRIVATE gridlane_gray_scott)
gridlane_set_warnings(gridlane_gray_scott_serial)
gridlane_enable_sanitizers(gridlane_gray_scott_serial)
gridlane_enable_coverage(gridlane_gray_scott_serial)

foreach(program IN ITEMS worker coordinator)
  add_executable(gridlane_${program} gray_scott/${program}_main.cpp)
  target_link_libraries(gridlane_${program} PRIVATE gridlane_gray_scott gridlane::control_${program})
  gridlane_set_warnings(gridlane_${program})
  gridlane_enable_sanitizers(gridlane_${program})
  gridlane_enable_coverage(gridlane_${program})
endforeach()
