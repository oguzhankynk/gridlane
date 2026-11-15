cmake_minimum_required(VERSION 3.25)

project(gridlane
  VERSION 0.1.1
  DESCRIPTION "A layered, loosely coupled model for low-latency data distribution and messaging"
  LANGUAGES CXX)

# PREPEND: when this project is a subproject (FetchContent, add_subdirectory),
# its own modules must win over same-named modules of the parent project.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
find_package(Doxygen QUIET)

option(BUILD_SHARED_LIBS "Build gridlane as a shared library" OFF)
option(GRIDLANE_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(GRIDLANE_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
# Docs only for the top-level project: as a subproject, its "docs" targets
# would clash with the parent's.
if(PROJECT_IS_TOP_LEVEL AND DOXYGEN_FOUND)
  set(_gridlane_docs_default ON)
else()
  set(_gridlane_docs_default OFF)
endif()
option(GRIDLANE_BUILD_DOCS "Build the Doxygen API reference ('docs' / 'docs-check' targets)" ${_gridlane_docs_default})
option(GRIDLANE_ENABLE_CLANG_TIDY "Run clang-tidy on the gridlane target while building" OFF)
option(GRIDLANE_ENABLE_CPPCHECK "Run cppcheck on the gridlane target while building" OFF)
option(GRIDLANE_ENABLE_COVERAGE "Build gridlane and its tests with --coverage instrumentation" OFF)
option(GRIDLANE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(GRIDLANE_SANITIZER "" CACHE STRING
  "Semicolon-separated sanitizers to enable, e.g. 'address;undefined' or 'thread'")

include(CompilerWarnings)
include(Sanitizers)
include(Coverage)
include(ClangTidy)
include(Cppcheck)
include(Doxygen)
include(Dependencies)

gridlane_fetch_dependencies()
add_subdirectory(src)
add_subdirectory(apps)

if(GRIDLANE_BUILD_TESTS)
  enable_testing()
  gridlane_fetch_test_dependencies()
  add_subdirectory(tests)
endif()

if(GRIDLANE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(GRIDLANE_BUILD_DOCS)
  gridlane_add_doxygen_targets()
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS gridlane
  EXPORT gridlaneTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/gridlane DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT gridlaneTargets
  FILE gridlaneTargets.cmake
  NAMESPACE gridlane::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gridlane)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gridlaneConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/gridlaneConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gridlane)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/gridlaneConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/gridlaneConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/gridlaneConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gridlane)
