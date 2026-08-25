include(FetchContent)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

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
target_link_libraries(safeedge_rt_tests PRIVATE
  safeedge::rt
  safeedge::warnings
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,safeedge::rt_alloc_guard>"
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
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,safeedge::rt_alloc_guard>"
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
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,safeedge::rt_alloc_guard>"
  GTest::gtest_main)
gtest_discover_tests(safeedge_ipc_tests
  PROPERTIES LABELS unit TIMEOUT 300
  DISCOVERY_TIMEOUT 60)
