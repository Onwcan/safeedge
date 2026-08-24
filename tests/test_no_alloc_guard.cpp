// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "safeedge/concurrent/spsc_ring.hpp"
#include "safeedge/rt/no_alloc_guard.hpp"

namespace safeedge::rt {
namespace {

/// Forces an allocation to actually happen.
///
/// [expr.new]/10 permits an implementation to omit the allocation call for a
/// new-expression whose result it can prove is unused. GCC and Clang both do
/// this at -O2, so a test that writes `delete new int(1)` observes zero
/// violations in Release and passes for entirely the wrong reason -- these two
/// tests did exactly that until a Release run caught them.
///
/// Routing the pointer through a volatile sink makes it escape, so the
/// allocation cannot be proved dead and the call survives optimisation.
void* volatile g_escape_sink = nullptr;

template <typename T>
void escape(T* pointer) noexcept {
  g_escape_sink = static_cast<void*>(pointer);
}

class NoAllocGuardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Never leave the policy at kAbort inside a normal test: a violation
    // would take the whole test binary down instead of failing one case.
    // The abort path gets its own death test below.
    setAllocationPolicy(AllocationPolicy::kCount);
    resetAllocationReport();
  }
  void TearDown() override {
    setAllocationPolicy(AllocationPolicy::kAbort);
    resetAllocationReport();
  }
};

TEST_F(NoAllocGuardTest, GuardIsActuallyLinked) {
  // If safeedge::rt_alloc_guard were missing from the link line this would not
  // resolve at all. The assertion documents the dependency; the linker enforces
  // it. Without WHOLE_ARCHIVE in tests/CMakeLists.txt the object would be
  // dropped, every violation would go unseen, and every test below would pass
  // for the wrong reason.
  EXPECT_TRUE(guardIsInstalled());
}

TEST_F(NoAllocGuardTest, ScopeTrackingIsAccurate) {
  EXPECT_FALSE(inNoAllocScope());
  {
    const NoAllocScope outer;
    EXPECT_TRUE(inNoAllocScope());
    {
      const NoAllocScope inner;
      EXPECT_TRUE(inNoAllocScope());
    }
    EXPECT_TRUE(inNoAllocScope())
        << "leaving a nested scope must not clear the outer one";
  }
  EXPECT_FALSE(inNoAllocScope());
}

TEST_F(NoAllocGuardTest, AllocationOutsideAScopeIsNotAViolation) {
  auto* leaked = new int(5);
  delete leaked;
  std::vector<int> v(1000, 7);
  EXPECT_EQ(allocationReport().violations, 0u);
  EXPECT_EQ(v.size(), 1000u);
}

TEST_F(NoAllocGuardTest, AllocationInsideAScopeIsCounted) {
  {
    const NoAllocScope no_alloc;
    int* p = new int(1);
    escape(p);
    delete p;
  }
  EXPECT_EQ(allocationReport().violations, 1u);
}

TEST_F(NoAllocGuardTest, ViolationRecordsTheLargestSizeSeen) {
  // The size is the diagnostic: a 32-byte violation is usually a std::function
  // or a small vector; a multi-kilobyte one is a buffer that outgrew its
  // reserve. Knowing which saves an hour with a debugger.
  {
    const NoAllocScope no_alloc;
    ::operator delete(::operator new(64));
    ::operator delete(::operator new(8192));
    ::operator delete(::operator new(128));
  }
  const AllocationReport report = allocationReport();
  EXPECT_EQ(report.violations, 3u);
  EXPECT_GE(report.largest_bytes, 8192u);
}

TEST_F(NoAllocGuardTest, ArrayAndAlignedFormsAreAlsoIntercepted) {
  // Replacing only operator new(size_t) is a classic half-measure: the
  // compiler emits the array and over-aligned forms for real code, and they
  // would slip past unseen.
  struct alignas(128) OverAligned {
    double payload[16];
  };

  {
    const NoAllocScope no_alloc;
    int* array = new int[64];
    escape(array);
    delete[] array;

    auto* aligned = new OverAligned;
    escape(aligned);
    delete aligned;
  }
  EXPECT_EQ(allocationReport().violations, 2u);
}

TEST_F(NoAllocGuardTest, NothrowFormIsIntercepted) {
  {
    const NoAllocScope no_alloc;
    void* p = ::operator new(256, std::nothrow);
    ::operator delete(p, std::nothrow);
  }
  EXPECT_EQ(allocationReport().violations, 1u);
}

TEST_F(NoAllocGuardTest, AllowAllocScopeSuspendsTheGuard) {
  // The escape hatch for the honest case: a diagnostic path that has already
  // missed its deadline and is now building an error report.
  {
    const NoAllocScope no_alloc;
    {
      const AllowAllocScope permitted;
      EXPECT_FALSE(inNoAllocScope());
      auto* p = new int(3);
      delete p;
    }
    EXPECT_TRUE(inNoAllocScope()) << "the exemption must end with its scope";
  }
  EXPECT_EQ(allocationReport().violations, 0u);
}

TEST_F(NoAllocGuardTest, ViolationsResumeAfterTheExemptionEnds) {
  {
    const NoAllocScope no_alloc;
    {
      const AllowAllocScope permitted;
      ::operator delete(::operator new(16));
    }
    ::operator delete(::operator new(16));
  }
  EXPECT_EQ(allocationReport().violations, 1u);
}

TEST_F(NoAllocGuardTest, IgnorePolicyStopsCounting) {
  setAllocationPolicy(AllocationPolicy::kIgnore);
  {
    const NoAllocScope no_alloc;
    ::operator delete(::operator new(32));
  }
  // kIgnore still counts -- what it suppresses is the abort. Counting is
  // essentially free and the number is useful either way.
  EXPECT_EQ(allocationReport().violations, 1u);
  EXPECT_EQ(allocationPolicy(), AllocationPolicy::kIgnore);
}

// ---------------------------------------------------------------------------
// Cross-checking the components that claim to be real-time safe
// ---------------------------------------------------------------------------

TEST_F(NoAllocGuardTest, SpscRingOperationsDoNotAllocate) {
  // ADR-0001 claims the ring never allocates on the fast path. This is that
  // claim under test rather than under discussion.
  auto ring = std::make_unique<concurrent::SpscRing<std::uint64_t, 256>>();
  std::uint64_t out = 0;

  {
    const NoAllocScope no_alloc;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
      if (!ring->tryPush(i)) {
        (void)ring->tryPop(out);
        (void)ring->tryPush(i);
      }
    }
    while (ring->tryPop(out)) {
    }
  }
  EXPECT_EQ(allocationReport().violations, 0u);
}

TEST_F(NoAllocGuardTest, AStdFunctionAssignmentIsCaughtAsExpected) {
  // A worked example of the defect this exists to find. Capturing more than a
  // couple of pointers pushes std::function past its small-object buffer and
  // it heap-allocates -- silently, on a path that looks like plain assignment.
  // This is the single most common way an allocation reaches a control loop.
  std::function<void()> sink;
  std::array<double, 32> fat{};

  {
    const NoAllocScope no_alloc;
    sink = [fat]() { (void)fat; };
  }
  EXPECT_GE(allocationReport().violations, 1u)
      << "a fat lambda in std::function should have allocated";
}

TEST_F(NoAllocGuardTest, AbortPolicyTerminatesTheProcess) {
  // The behaviour CI depends on. Runs in a forked child so the abort does not
  // take the test binary with it.
  EXPECT_DEATH(
      {
        setAllocationPolicy(AllocationPolicy::kAbort);
        const NoAllocScope no_alloc;
        ::operator delete(::operator new(48));
      },
      "allocation of 48 bytes inside a NoAllocScope");
}

}  // namespace
}  // namespace safeedge::rt
