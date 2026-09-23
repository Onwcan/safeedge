// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <string>

#include "safeedge/rt/no_alloc_guard.hpp"

namespace safeedge::rt {
namespace {

// The volatile function pointers force the named allocator to be called even
// under Release/LTO. The escaped result also prevents dead-allocation elision.
static auto* volatile g_malloc = &std::malloc;
static auto* volatile g_calloc = &std::calloc;
static auto* volatile g_realloc = &std::realloc;
static auto* volatile g_free = &std::free;
static void* volatile g_escape_sink = nullptr;

class CAllocGuardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    setAllocationPolicy(AllocationPolicy::kCount);
    resetAllocationReport();
  }
  void TearDown() override {
    setAllocationPolicy(AllocationPolicy::kAbort);
    resetAllocationReport();
  }
};

TEST_F(CAllocGuardTest, DirectMallocIsCounted) {
  void* pointer = nullptr;
  {
    const NoAllocScope no_alloc;
    pointer = g_malloc(73);
    g_escape_sink = pointer;
  }
  EXPECT_NE(pointer, nullptr);
  EXPECT_EQ(allocationReport().violations, 1u);
  EXPECT_EQ(allocationReport().largest_bytes, 73u);
  g_free(pointer);
}

TEST_F(CAllocGuardTest, CallocIsCountedAndStillZeroInitializes) {
  unsigned char* pointer = nullptr;
  {
    const NoAllocScope no_alloc;
    pointer = static_cast<unsigned char*>(g_calloc(7, 11));
    g_escape_sink = pointer;
  }
  ASSERT_NE(pointer, nullptr);
  EXPECT_EQ(allocationReport().violations, 1u);
  EXPECT_EQ(allocationReport().largest_bytes, 77u);
  for (std::size_t i = 0; i < 77; ++i) {
    EXPECT_EQ(pointer[i], 0u);
  }
  g_free(pointer);
}

TEST_F(CAllocGuardTest, ReallocIsCountedAndPreservesTheAllocation) {
  auto* original = static_cast<unsigned char*>(g_malloc(1));
  ASSERT_NE(original, nullptr);
  original[0] = 42;
  unsigned char* resized = nullptr;
  {
    const NoAllocScope no_alloc;
    resized = static_cast<unsigned char*>(g_realloc(original, 129));
    g_escape_sink = resized;
  }
  EXPECT_EQ(allocationReport().violations, 1u);
  EXPECT_EQ(allocationReport().largest_bytes, 129u);
  EXPECT_NE(resized, nullptr);
  if (resized != nullptr) {
    EXPECT_EQ(resized[0], 42u);
    g_free(resized);
  } else {
    g_free(original);
  }
}

TEST_F(CAllocGuardTest, FreeIsForwardedWithoutAnAllocationViolation) {
  void* pointer = g_malloc(32);
  ASSERT_NE(pointer, nullptr);
  {
    const NoAllocScope no_alloc;
    g_free(pointer);
    g_free(nullptr);
  }
  EXPECT_EQ(allocationReport().violations, 0u);
}

TEST_F(CAllocGuardTest, OutsideAScopeAndAllowedAllocationsAreNotViolations) {
  void* pointer = g_malloc(8);
  ASSERT_NE(pointer, nullptr);
  g_free(pointer);
  {
    const NoAllocScope no_alloc;
    {
      const AllowAllocScope allowed;
      pointer = g_calloc(2, 16);
      g_escape_sink = pointer;
      g_free(pointer);
    }
    pointer = g_malloc(31);
    g_escape_sink = pointer;
    g_free(pointer);
  }
  EXPECT_EQ(allocationReport().violations, 1u);
  EXPECT_EQ(allocationReport().largest_bytes, 31u);
}

TEST_F(CAllocGuardTest, CallocSizeCalculationSaturatesOnOverflow) {
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  // Abort before entering libc: sanitizers can terminate on huge requests,
  // while this test only needs to verify the wrapper's diagnostic arithmetic.
  EXPECT_DEATH(
      {
        setAllocationPolicy(AllocationPolicy::kAbort);
        const NoAllocScope no_alloc;
        g_escape_sink = g_calloc(maximum / 2 + 1, 2);
      },
      "allocation of " + std::to_string(maximum) + " bytes");
}

TEST_F(CAllocGuardTest, AbortPolicyCatchesDirectMalloc) {
  EXPECT_DEATH(
      {
        setAllocationPolicy(AllocationPolicy::kAbort);
        const NoAllocScope no_alloc;
        g_escape_sink = g_malloc(83);
      },
      "allocation of 83 bytes inside a NoAllocScope");
}

}  // namespace
}  // namespace safeedge::rt
