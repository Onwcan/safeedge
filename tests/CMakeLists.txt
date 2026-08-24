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
