include_guard(GLOBAL)
include(FetchContent)

## gridlane_fetch_test_dependencies()
#
# Fetches GoogleTest/GoogleMock, used by tests/ only.
# Pinned to an exact tag for reproducible builds.
function(gridlane_fetch_test_dependencies)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE
    SYSTEM)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endfunction()

## gridlane_fetch_dependencies()
#
# The layers gridlane composes, each pinned to an exact revision. For local
# work on a layer, point CMake at a checkout instead, e.g.
#   -DFETCHCONTENT_SOURCE_DIR_PUSHSEG=../pushseg
function(gridlane_fetch_dependencies)
  FetchContent_Declare(
    pushseg
    GIT_REPOSITORY https://github.com/oguzhankynk/pushseg.git
    GIT_TAG a6fc1999d06e1909c7a882468ff4be99f2293192
    SYSTEM)
  FetchContent_Declare(
    pairring
    GIT_REPOSITORY https://github.com/oguzhankynk/pairring.git
    GIT_TAG 639a8a3758270c184f9d2c032c878ac9c9df9209
    SYSTEM)
  # Only the runtime headers of wireforge are used. SOURCE_SUBDIR names a
  # directory without a CMakeLists.txt, so the repository is downloaded but
  # not built.
  FetchContent_Declare(
    wireforge
    GIT_REPOSITORY https://github.com/oguzhankynk/wireforge.git
    GIT_TAG v0.5.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR runtime/include
    SYSTEM)
  # Instrument the layers like gridlane itself: ThreadSanitizer, for one,
  # reports false races when part of the program is not instrumented.
  set(PUSHSEG_SANITIZER "${GRIDLANE_SANITIZER}" CACHE STRING "" FORCE)
  set(PAIRRING_SANITIZER "${GRIDLANE_SANITIZER}" CACHE STRING "" FORCE)
  FetchContent_MakeAvailable(pushseg pairring wireforge)

  if(NOT TARGET wireforge::runtime)
    add_library(gridlane_wireforge_runtime INTERFACE)
    add_library(wireforge::runtime ALIAS gridlane_wireforge_runtime)
    target_include_directories(gridlane_wireforge_runtime SYSTEM INTERFACE
      $<BUILD_INTERFACE:${wireforge_SOURCE_DIR}/runtime/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(gridlane_wireforge_runtime INTERFACE cxx_std_17)
    # Installed with gridlane: the runtime headers are part of its interface.
    install(TARGETS gridlane_wireforge_runtime EXPORT gridlaneTargets)
    install(DIRECTORY "${wireforge_SOURCE_DIR}/runtime/include/wireforge"
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  endif()
endfunction()
