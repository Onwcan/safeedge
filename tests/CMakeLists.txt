include(FetchContent)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# GNU-compatible ELF linkers can wrap allocator references in the test binary
# and its static dependencies. Keep this target local to tests: shipped targets
# continue linking the normal C++-only guard, with no C allocator interposition.
set(SAFEEDGE_TEST_ALLOC_GUARD safeedge::rt_alloc_guard)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_library(safeedge_test_alloc_guard STATIC
    ${PROJECT_SOURCE_DIR}/src/rt/no_alloc_guard_hooks.cpp
    c_alloc_guard.cpp)
  target_compile_definitions(safeedge_test_alloc_guard
    PRIVATE SAFEEDGE_TEST_WRAP_C_ALLOCATIONS)
  target_link_libraries(safeedge_test_alloc_guard
    PUBLIC safeedge::rt
    PRIVATE safeedge::warnings)
  target_link_options(safeedge_test_alloc_guard INTERFACE
    "LINKER:--wrap=malloc"
    "LINKER:--wrap=calloc"
    "LINKER:--wrap=realloc"
    "LINKER:--wrap=free")
  set(SAFEEDGE_TEST_ALLOC_GUARD safeedge_test_alloc_guard)
endif()

add_executable(safeedge_tests
  test_spsc_ring.cpp)
target_link_libraries(safeedge_tests
  PRIVATE safeedge::concurrent safeedge::warnings GTest::gtest_main)

# The concurrency tests spin two threads against each other for hundreds of
# thousands of iterations. Under TSan that is slow but it is exactly the point,
# so the discovery and per-test timeouts are generous.
gtest_discover_tests(safeedge_tests
  PROPERTIES LABELS unit TIMEOUT 300
  DISCOVERY_TIMEOUT 60)

# The real-time tests link the allocation guard with WHOLE_ARCHIVE.
#
# Without it the linker would pull in only those object files that resolve an
# undefined symbol -- and a global operator new replacement resolves nothing,
# so the object would be dropped and the guard would silently not exist. The
# tests would then pass by observing no violations, which is the worst possible
# failure mode for a guard. WHOLE_ARCHIVE forces it in.
add_executable(safeedge_rt_tests
  test_latency_histogram.cpp
  test_no_alloc_guard.cpp
  test_cyclic_executor.cpp)
if(TARGET safeedge_test_alloc_guard)
  target_sources(safeedge_rt_tests PRIVATE test_c_alloc_guard.cpp)
endif()
target_link_libraries(safeedge_rt_tests PRIVATE
  safeedge::rt
  safeedge::warnings
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,${SAFEEDGE_TEST_ALLOC_GUARD}>"
  GTest::gtest_main)
gtest_discover_tests(safeedge_rt_tests
  PROPERTIES LABELS unit TIMEOUT 300
  DISCOVERY_TIMEOUT 60)

add_executable(safeedge_safety_tests
  test_crc32.cpp
  test_black_channel.cpp
  test_safety_state_machine.cpp
  test_dual_channel.cpp)
target_link_libraries(safeedge_safety_tests PRIVATE
  safeedge::safety
  safeedge::rt
  safeedge::warnings
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,${SAFEEDGE_TEST_ALLOC_GUARD}>"
  GTest::gtest_main)
gtest_discover_tests(safeedge_safety_tests
  PROPERTIES LABELS unit TIMEOUT 300
  DISCOVERY_TIMEOUT 60)

add_executable(safeedge_ipc_tests
  test_shared_memory.cpp
  test_seqlock_slot.cpp)
target_link_libraries(safeedge_ipc_tests PRIVATE
  safeedge::ipc
  safeedge::rt
  safeedge::warnings
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,${SAFEEDGE_TEST_ALLOC_GUARD}>"
  GTest::gtest_main)
gtest_discover_tests(safeedge_ipc_tests
  PROPERTIES LABELS unit TIMEOUT 300
  DISCOVERY_TIMEOUT 60)

add_executable(safeedge_edge_tests
  test_edge_service.cpp)
target_link_libraries(safeedge_edge_tests PRIVATE
  safeedge::edge
  safeedge::ipc
  safeedge::warnings
  GTest::gtest_main)
gtest_discover_tests(safeedge_edge_tests
  PROPERTIES LABELS unit TIMEOUT 300
  DISCOVERY_TIMEOUT 60)

# Only when the component is built. Guarding here rather than skipping at run
# time means the default build does not compile a protocol stack to run tests
# that would immediately be skipped.
if(SAFEEDGE_BUILD_OPCUA)
  add_executable(safeedge_opcua_tests
    test_opcua_address_space.cpp
    test_opcua_events.cpp
    test_opcua_security.cpp)
  target_link_libraries(safeedge_opcua_tests PRIVATE
    safeedge::opcua
    safeedge::warnings
    GTest::gtest_main)
  if(SAFEEDGE_UA_INCLUDES)
    target_include_directories(safeedge_opcua_tests SYSTEM PRIVATE ${SAFEEDGE_UA_INCLUDES})
  endif()
  gtest_discover_tests(safeedge_opcua_tests
    PROPERTIES LABELS unit TIMEOUT 300
    DISCOVERY_TIMEOUT 60)
endif()
