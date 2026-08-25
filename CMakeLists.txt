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
option(SAFEEDGE_BUILD_FUZZERS    "Build libFuzzer targets (Clang only)" OFF)
option(SAFEEDGE_BUILD_EXAMPLES   "Build example programs" ${SAFEEDGE_IS_TOP_LEVEL})

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

# rt -- the deterministic execution layer: latency accounting, real-time thread
# configuration, and the cyclic executor itself.
add_library(safeedge_rt
  src/rt/latency_histogram.cpp
  src/rt/no_alloc_guard.cpp
  src/rt/thread_config.cpp
  src/rt/cyclic_executor.cpp)
add_library(safeedge::rt ALIAS safeedge_rt)
target_include_directories(safeedge_rt
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(safeedge_rt
  PUBLIC  safeedge::concurrent Threads::Threads
  PRIVATE safeedge::warnings)
set_target_properties(safeedge_rt PROPERTIES
  OUTPUT_NAME safeedge-rt
  EXPORT_NAME rt
  VERSION   ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# rt_alloc_guard -- globally replaces the operator new family so that an
# allocation inside a NoAllocScope aborts.
#
# Its own target, and never a dependency of safeedge::rt, because linking it
# changes allocation behaviour for the entire binary. That has to be an
# explicit decision by whoever produces the executable, not something a library
# imposes on its consumers. Tests and the demo link it; a production image
# would not have to.
add_library(safeedge_rt_alloc_guard STATIC
  src/rt/no_alloc_guard_hooks.cpp)
add_library(safeedge::rt_alloc_guard ALIAS safeedge_rt_alloc_guard)
target_include_directories(safeedge_rt_alloc_guard
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Depends on safeedge::rt for the guard state it reports into. The dependency
# runs this way round on purpose: rt owns the policy and counters and is
# self-contained without the hooks, so linking the hooks is additive.
target_link_libraries(safeedge_rt_alloc_guard
  PUBLIC  safeedge::rt
  PRIVATE safeedge::warnings)
set_target_properties(safeedge_rt_alloc_guard PROPERTIES
  OUTPUT_NAME safeedge-rt-alloc-guard
  EXPORT_NAME rt_alloc_guard)

# safety -- the failsafe layer: black-channel telegram and the supervisor that
# acts on it. Kept a separate component so it can be reviewed, and eventually
# argued for, in isolation from the transport and executor code around it.
add_library(safeedge_safety
  src/safety/crc32.cpp
  src/safety/black_channel.cpp
  src/safety/safety_state_machine.cpp
  src/safety/dual_channel.cpp)
add_library(safeedge::safety ALIAS safeedge_safety)
target_include_directories(safeedge_safety
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(safeedge_safety PRIVATE safeedge::warnings)
set_target_properties(safeedge_safety PROPERTIES
  OUTPUT_NAME safeedge-safety
  EXPORT_NAME safety
  VERSION   ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# ipc -- the transport layer: shared memory and the primitives that live in it.
add_library(safeedge_ipc
  src/ipc/shared_memory.cpp)
add_library(safeedge::ipc ALIAS safeedge_ipc)
target_include_directories(safeedge_ipc
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(safeedge_ipc
  PUBLIC  safeedge::concurrent
  PRIVATE safeedge::warnings)
set_target_properties(safeedge_ipc PROPERTIES
  OUTPUT_NAME safeedge-ipc
  EXPORT_NAME ipc
  VERSION   ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# shm_open lives in libc on glibc 2.34 and later, and in librt before that.
# Linking it only when it exists keeps the build working on both without
# forcing every consumer to know which they have.
find_library(SAFEEDGE_LIBRT rt)
if(SAFEEDGE_LIBRT)
  target_link_libraries(safeedge_ipc PUBLIC ${SAFEEDGE_LIBRT})
endif()

# edge -- the service wrapper: observability endpoints and metric rendering.
# Everything here runs off the deadline path by construction.
add_library(safeedge_edge
  src/edge/http_server.cpp
  src/edge/metrics.cpp)
add_library(safeedge::edge ALIAS safeedge_edge)
target_include_directories(safeedge_edge
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(safeedge_edge
  PUBLIC  safeedge::ipc Threads::Threads
  PRIVATE safeedge::warnings)
set_target_properties(safeedge_edge PROPERTIES
  OUTPUT_NAME safeedge-edge
  EXPORT_NAME edge
  VERSION   ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# safeedged -- the runtime as a service. Statically linked so the container
# image needs no runtime libraries at all; see ADR-0009.
add_executable(safeedged src/apps/safeedged.cpp)
target_link_libraries(safeedged PRIVATE
  safeedge::edge
  safeedge::rt
  safeedge::safety
  safeedge::ipc
  safeedge::warnings
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,safeedge::rt_alloc_guard>")

if(SAFEEDGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SAFEEDGE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# Conditional on its own option, not on being top level. The container build
# copies only what safeedged needs, so examples/ is not present there and an
# unconditional add_subdirectory would fail the image build.
if(SAFEEDGE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(SAFEEDGE_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS safeedge_concurrent safeedge_rt safeedge_rt_alloc_guard
                safeedge_safety safeedge_ipc safeedge_edge safeedge_warnings
        EXPORT safeedgeTargets
        LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR})
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
