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

# OPC UA is off by default, and that is a decision rather than caution.
#
# open62541 is a protocol stack: a session layer, two encoders, a certificate
# handler and a subscription engine. It builds in a couple of minutes, which is
# a couple of minutes every contributor pays for a component most of them are
# not touching. More importantly the failsafe runtime does not link it -- the
# OPC UA server is a separate binary in a separate process, so a fault anywhere
# in that stack cannot reach the control loop, and safeedged keeps its 1.51 MB
# scratch image.
option(SAFEEDGE_BUILD_OPCUA      "Build the OPC UA server component"    OFF)

# Encryption is ON whenever the component is built.
#
# A switch exists because the crypto backend is another dependency to build, not
# because running without it is a supported posture. An OPC UA server reachable
# on a plant network with SecurityPolicy#None is a server anyone on that network
# can read and impersonate.
option(SAFEEDGE_OPCUA_ENCRYPTION "Build the OPC UA component with mbedTLS"  ON)
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

# --- opcua component -------------------------------------------------------
#
# Its own library and its own binary, linked against open62541. safeedged links
# neither, which is the point: a fault in a protocol stack must not be able to
# reach a loop that is holding a machine in a safe state.
if(SAFEEDGE_BUILD_OPCUA)
  include(FetchContent)

  if(SAFEEDGE_OPCUA_ENCRYPTION)
    # mbedTLS rather than OpenSSL because it builds from source with no system
    # packages, which is what lets this component be built on a machine with no
    # install rights. Two settings are not optional:
    #
    #   DISABLE_PACKAGE_CONFIG_AND_INSTALL defaults to ON for a subproject, which
    #   leaves mbedTLS's targets out of any export set -- and open62541's own
    #   install(EXPORT) then fails because it references them. Configure dies
    #   with "requires target mbedtls that is not in any export set", which does
    #   not obviously mean "turn on mbedTLS's install rules".
    #
    #   MBEDTLS_FATAL_WARNINGS is mbedTLS's own -Werror. GCC 15 added
    #   -Wunterminated-string-initialization, which fires inside mbedTLS 3.6.2,
    #   so the dependency fails to compile on a new enough compiler. Somebody
    #   else's warning bar is not this project's to satisfy.
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(mbedtls
      GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
      GIT_TAG        v3.6.2
      GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(mbedtls)

    set(UA_ENABLE_ENCRYPTION "MBEDTLS" CACHE STRING "" FORCE)
    set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
    set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
    set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
  endif()

  set(UA_ENABLE_AMALGAMATION OFF CACHE BOOL "" FORCE)
  set(UA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(UA_ENABLE_SUBSCRIPTIONS ON CACHE BOOL "" FORCE)
  set(UA_FORCE_WERROR OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(open62541
    GIT_REPOSITORY https://github.com/open62541/open62541.git
    GIT_TAG        v1.4.12
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(open62541)

  # open62541's headers are included as SYSTEM, so this project's warning bar
  # does not apply to them. Without this, -Wextra -Werror fails the build on
  # `unused parameter 'lock'` inside open62541's own generated config.h -- a
  # diagnostic about somebody else's code that nobody here can act on. Holding a
  # third-party header to your own standard produces exactly one outcome:
  # somebody turns the warning off globally.
  get_target_property(SAFEEDGE_UA_INCLUDES open62541::open62541
                      INTERFACE_INCLUDE_DIRECTORIES)

  add_library(safeedge_opcua src/opcua/address_space.cpp src/opcua/security.cpp)
  add_library(safeedge::opcua ALIAS safeedge_opcua)
  if(SAFEEDGE_UA_INCLUDES)
    target_include_directories(safeedge_opcua SYSTEM PUBLIC ${SAFEEDGE_UA_INCLUDES})
  endif()
  target_include_directories(safeedge_opcua
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(safeedge_opcua
    PUBLIC open62541::open62541
    PRIVATE safeedge::warnings)
  set_target_properties(safeedge_opcua PROPERTIES
    OUTPUT_NAME safeedge-opcua
    EXPORT_NAME opcua
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

  # The client side, and a measurement. Ships as a tool rather than a test
  # because it needs a running server and a real transition to observe -- which
  # is a thing a person does, not a thing CI does in three seconds.
  add_executable(safeedge_opcua_probe src/apps/safeedge_opcua_probe.cpp)
  set_target_properties(safeedge_opcua_probe PROPERTIES OUTPUT_NAME safeedge-opcua-probe)
  target_link_libraries(safeedge_opcua_probe PRIVATE
    safeedge::opcua
    safeedge::warnings)
  if(SAFEEDGE_UA_INCLUDES)
    target_include_directories(safeedge_opcua_probe SYSTEM PRIVATE ${SAFEEDGE_UA_INCLUDES})
  endif()

  add_executable(safeedge_opcua_server src/apps/safeedge_opcua.cpp)
  set_target_properties(safeedge_opcua_server PROPERTIES OUTPUT_NAME safeedge-opcua)
  if(SAFEEDGE_UA_INCLUDES)
    target_include_directories(safeedge_opcua_server SYSTEM PRIVATE ${SAFEEDGE_UA_INCLUDES})
  endif()
  target_link_libraries(safeedge_opcua_server PRIVATE
    safeedge::opcua
    safeedge::ipc
    safeedge::warnings)
endif()

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
