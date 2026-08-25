# libFuzzer targets. Clang only -- GCC has no -fsanitize=fuzzer -- so this
# subdirectory is opt-in rather than part of the default build. A developer on
# GCC still gets the mutation-based robustness test in the normal suite, which
# covers the same property less thoroughly.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(STATUS "safeedge: fuzzers require Clang; skipping")
  return()
endif()

add_executable(fuzz_telegram_decode fuzz_telegram_decode.cpp)
target_link_libraries(fuzz_telegram_decode PRIVATE safeedge::safety safeedge::warnings)
target_compile_options(fuzz_telegram_decode PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_telegram_decode PRIVATE -fsanitize=fuzzer,address,undefined)
