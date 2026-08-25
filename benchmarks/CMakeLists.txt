# Benchmarks are ordinary executables rather than a test-framework target: the
# output is a report to be read and pasted into the README, not a pass/fail
# gate. Performance regressions are caught by comparing successive reports,
# which is a judgement call, not an assertion.
add_executable(bench_spsc_ring bench_spsc_ring.cpp)
target_link_libraries(bench_spsc_ring PRIVATE safeedge::concurrent safeedge::warnings)

# Always optimise benchmarks, even in a Debug configure. A latency number
# measured at -O0 describes the compiler, not the code.
target_compile_options(bench_spsc_ring PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O2>)

add_custom_target(run_benchmarks
  COMMAND bench_spsc_ring
  COMMAND bench_transports
  DEPENDS bench_spsc_ring bench_transports
  COMMENT "Running safeedge benchmarks")

add_executable(bench_transports bench_transports.cpp)
target_link_libraries(bench_transports PRIVATE safeedge::ipc safeedge::warnings)
target_compile_options(bench_transports PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O2>)
