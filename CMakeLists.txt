cmake_minimum_required(VERSION 3.24)

project(safeedge
  VERSION 0.1.0
  DESCRIPTION "Failsafe real-time runtime components for industrial edge devices"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(SAFEEDGE_IS_TOP_LEVEL ON)
else()
  set(SAFEEDGE_IS_TOP_LEVEL OFF)
endif()

option(SAFEEDGE_BUILD_TESTS      "Build unit tests"         ${SAFEEDGE_IS_TOP_LEVEL})
option(SAFEEDGE_BUILD_BENCHMARKS "Build latency benchmarks" ${SAFEEDGE_IS_TOP_LEVEL})
option(SAFEEDGE_ENABLE_WERROR    "Treat warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning policy
# ---------------------------------------------------------------------------
add_library(safeedge_warnings INTERFACE)
add_library(safeedge::warnings ALIAS safeedge_warnings)
target_compile_options(safeedge_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
    -Woverloaded-virtual -Wdouble-promotion -Wformat=2 -Wnull-dereference
    # A runtime that must not stall cannot afford a silently misaligned or
    # narrowed value in a hot path, so these are errors rather than notes.
    $<$<BOOL:${SAFEEDGE_ENABLE_WERROR}>:-Werror>>)

# GCC warns that std::hardware_destructive_interference_size is ABI-sensitive.
# We deliberately do not use it -- see ADR-0001 -- so the warning is moot, but
# the flag is kept here so a future reintroduction is a conscious decision.
target_compile_options(safeedge_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>)

# ---------------------------------------------------------------------------
# Components
#
# safeedge is built as a set of small, separately linkable components rather
# than one library: the safety supervisor must be reviewable in isolation from
# the transport and executor code that surrounds it.
# ---------------------------------------------------------------------------

# concurrent -- lock-free primitives shared by the executor and the transports.
# Header-only: the ring buffer is a template and inlining the fast path matters.
add_library(safeedge_concurrent INTERFACE)
add_library(safeedge::concurrent ALIAS safeedge_concurrent)
target_include_directories(safeedge_concurrent INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(safeedge_concurrent INTERFACE cxx_std_20)
set_target_properties(safeedge_concurrent PROPERTIES EXPORT_NAME concurrent)
set_target_properties(safeedge_warnings PROPERTIES EXPORT_NAME warnings)

find_package(Threads REQUIRED)
target_link_libraries(safeedge_concurrent INTERFACE Threads::Threads)

if(SAFEEDGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SAFEEDGE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS safeedge_concurrent safeedge_warnings
        EXPORT safeedgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT safeedgeTargets
        FILE safeedgeTargets.cmake
        NAMESPACE safeedge::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/safeedge)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/safeedgeConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/safeedgeConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/safeedgeConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/safeedge)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/safeedgeConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/safeedgeConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/safeedge)
