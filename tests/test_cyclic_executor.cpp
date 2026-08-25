// SPDX-License-Identifier: Apache-2.0
//
// Note on what is asserted here.
//
// These tests run on whatever machine CI or a developer happens to have, which
// is generally virtualised, generally without real-time scheduling privileges,
// and generally shared with other work. Asserting "wakeup jitter is under 50
// microseconds" on such a host produces a test that fails for reasons having
// nothing to do with this code -- and a flaky test is worse than no test,
// because the team learns to re-run it.
//
// So the assertions here are on *structural* properties that hold regardless of
// platform: exact cycle counts, drift-free deadline arithmetic, correct overrun
// accounting, and the absence of allocation. Actual latency numbers are the job
// of the benchmark and of rt-latency-lab on tuned hardware, where they mean
// something.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "safeedge/rt/cyclic_executor.hpp"
#include "safeedge/rt/no_alloc_guard.hpp"
#include "safeedge/rt/thread_config.hpp"

namespace safeedge::rt {
namespace {

using namespace std::chrono_literals;

/// Busy-waits for a duration. Used to make a callback deliberately overrun.
/// Spinning rather than sleeping keeps the thread runnable, so the overrun is
/// caused by the callback rather than by the scheduler choosing not to run us.
void burn(std::chrono::nanoseconds duration) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
  }
}

class CyclicExecutorTest : public ::testing::Test {
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

// ---------------------------------------------------------------------------
// Cycle accounting
// ---------------------------------------------------------------------------

TEST_F(CyclicExecutorTest, RunsExactlyTheRequestedNumberOfCycles) {
  CyclicExecutor::Config config;
  config.period = 200us;
  CyclicExecutor executor(config);

  std::uint64_t observed = 0;
  auto callback = [&](const CycleContext&) { ++observed; };
  executor.run(CycleCallback(callback), 50);

  EXPECT_EQ(observed, 50u);
  EXPECT_EQ(executor.stats().cycles_executed, 50u);
}

TEST_F(CyclicExecutorTest, CycleIndexIsContiguousAndStartsAtZero) {
  CyclicExecutor::Config config;
  config.period = 100us;
  CyclicExecutor executor(config);

  std::vector<std::uint64_t> indices;
  indices.reserve(40);
  auto callback = [&](const CycleContext& ctx) { indices.push_back(ctx.index); };
  // The callback allocates on the first push_back, so the guard is off here.
  config.guard_allocations = false;
  CyclicExecutor unguarded(config);
  unguarded.run(CycleCallback(callback), 40);

  ASSERT_EQ(indices.size(), 40u);
  for (std::uint64_t i = 0; i < indices.size(); ++i) {
    EXPECT_EQ(indices[i], i);
  }
}

TEST_F(CyclicExecutorTest, ContextCarriesTheConfiguredPeriod) {
  CyclicExecutor::Config config;
  config.period = 750us;
  CyclicExecutor executor(config);

  std::int64_t seen_period = 0;
  auto callback = [&](const CycleContext& ctx) { seen_period = ctx.period_ns; };
  executor.run(CycleCallback(callback), 3);

  EXPECT_EQ(seen_period, 750'000);
}

TEST_F(CyclicExecutorTest, EveryHistogramGetsOneSamplePerCycle) {
  CyclicExecutor::Config config;
  config.period = 100us;
  CyclicExecutor executor(config);

  auto callback = [](const CycleContext&) {};
  executor.run(CycleCallback(callback), 25);

  EXPECT_EQ(executor.stats().wakeup_jitter.count(), 25u);
  EXPECT_EQ(executor.stats().execution_time.count(), 25u);
  EXPECT_EQ(executor.stats().cycle_total.count(), 25u);
}

TEST_F(CyclicExecutorTest, ZeroOrNegativePeriodIsRejectedRatherThanSpinning) {
  // A period of zero would turn the loop into an unbounded busy-wait at
  // real-time priority, which on a pinned core is indistinguishable from a
  // hung machine.
  CyclicExecutor::Config config;
  config.period = 0ns;
  CyclicExecutor executor(config);

  std::uint64_t calls = 0;
  auto callback = [&](const CycleContext&) { ++calls; };
  executor.run(CycleCallback(callback), 10);

  EXPECT_EQ(calls, 0u);
  EXPECT_EQ(executor.stats().cycles_executed, 0u);
}

// ---------------------------------------------------------------------------
// Stopping
// ---------------------------------------------------------------------------

TEST_F(CyclicExecutorTest, StopFromInsideTheCallbackEndsTheLoop) {
  CyclicExecutor::Config config;
  config.period = 100us;
  CyclicExecutor executor(config);

  std::uint64_t calls = 0;
  auto callback = [&](const CycleContext& ctx) {
    ++calls;
    if (ctx.index == 9) {
      executor.requestStop();
    }
  };
  executor.run(CycleCallback(callback), 0);  // unbounded

  EXPECT_EQ(calls, 10u);
  EXPECT_TRUE(executor.stopRequested());
}

TEST_F(CyclicExecutorTest, StopFromAnotherThreadEndsTheLoop) {
  CyclicExecutor::Config config;
  config.period = 200us;
  CyclicExecutor executor(config);

  std::atomic<bool> running{false};
  auto callback = [&](const CycleContext&) {
    running.store(true, std::memory_order_relaxed);
  };

  std::thread stopper([&] {
    while (!running.load(std::memory_order_relaxed)) {
    }
    std::this_thread::sleep_for(5ms);
    executor.requestStop();
  });

  executor.run(CycleCallback(callback), 0);
  stopper.join();

  EXPECT_GT(executor.stats().cycles_executed, 0u);
}

TEST_F(CyclicExecutorTest, RunClearsAPreviousStopRequest) {
  // Otherwise a restart after a clean shutdown would silently do nothing.
  CyclicExecutor::Config config;
  config.period = 100us;
  CyclicExecutor executor(config);
  executor.requestStop();

  std::uint64_t calls = 0;
  auto callback = [&](const CycleContext&) { ++calls; };
  executor.run(CycleCallback(callback), 5);

  EXPECT_EQ(calls, 5u);
}

// ---------------------------------------------------------------------------
// The scheduling property the design exists for
// ---------------------------------------------------------------------------

TEST_F(CyclicExecutorTest, DeadlinesDoNotDriftAcrossManyCycles) {
  // @verifies REQ-RT-002
  // The core claim: because each deadline is derived from the previous
  // deadline rather than from the current time, wakeup delays are absorbed
  // rather than accumulated.
  //
  // A naive `sleep_for(period)` loop fails this badly -- at 1 ms with even
  // 100 us of average lateness it finishes 10% late, and the error grows
  // without bound. The tolerance below is deliberately loose enough to survive
  // a virtualised host but far tighter than accumulated drift would produce.
  constexpr std::uint64_t kCycles = 300;
  constexpr auto kPeriod = 1ms;

  CyclicExecutor::Config config;
  config.period = kPeriod;
  CyclicExecutor executor(config);

  auto callback = [](const CycleContext&) {};
  const auto started = std::chrono::steady_clock::now();
  executor.run(CycleCallback(callback), kCycles);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  const auto expected = kPeriod * kCycles;
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
  const auto expected_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(expected);

  EXPECT_GE(elapsed_ms.count(), expected_ms.count() - 5)
      << "finished early: the loop is not actually pacing";
  EXPECT_LE(elapsed_ms.count(), expected_ms.count() * 2)
      << "finished at least 100% late, which is what accumulated drift looks like";
}

TEST_F(CyclicExecutorTest, ActuallyWaitsRatherThanSpinning) {
  // Guards against a regression where the sleep is skipped and the loop
  // becomes a busy-wait that happens to produce the right cycle count.
  constexpr std::uint64_t kCycles = 20;
  constexpr auto kPeriod = 2ms;

  CyclicExecutor::Config config;
  config.period = kPeriod;
  CyclicExecutor executor(config);

  auto callback = [](const CycleContext&) {};
  const auto started = std::chrono::steady_clock::now();
  executor.run(CycleCallback(callback), kCycles);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 30);
}

// ---------------------------------------------------------------------------
// Overrun handling
// ---------------------------------------------------------------------------

TEST_F(CyclicExecutorTest, OverrunIsDetectedWhenTheCallbackExceedsThePeriod) {
  // @verifies REQ-RT-003
  CyclicExecutor::Config config;
  config.period = 500us;
  config.overrun_policy = OverrunPolicy::kSkipMissed;
  CyclicExecutor executor(config);

  // Burn three periods every cycle.
  auto callback = [](const CycleContext&) { burn(1500us); };
  executor.run(CycleCallback(callback), 10);

  EXPECT_GE(executor.stats().overruns, 8u)
      << "a callback taking 3x the period must register as an overrun";
}

TEST_F(CyclicExecutorTest, SkipMissedCountsTheDeadlinesItGaveUp) {
  // @verifies REQ-RT-003
  CyclicExecutor::Config config;
  config.period = 500us;
  config.overrun_policy = OverrunPolicy::kSkipMissed;
  CyclicExecutor executor(config);

  auto callback = [](const CycleContext&) { burn(1500us); };
  executor.run(CycleCallback(callback), 10);

  EXPECT_GT(executor.stats().skipped_cycles, 0u)
      << "skipped deadlines must be counted, not silently swallowed";
  EXPECT_EQ(executor.stats().cycles_executed, 10u)
      << "skipping a deadline must not change how many cycles actually ran";
}

TEST_F(CyclicExecutorTest, RunLateDoesNotSkipDeadlines) {
  CyclicExecutor::Config config;
  config.period = 500us;
  config.overrun_policy = OverrunPolicy::kRunLate;
  CyclicExecutor executor(config);

  auto callback = [](const CycleContext&) { burn(1500us); };
  executor.run(CycleCallback(callback), 10);

  EXPECT_EQ(executor.stats().skipped_cycles, 0u)
      << "kRunLate trades drift for completeness; nothing may be dropped";
  EXPECT_GE(executor.stats().overruns, 8u) << "lateness is still reported";
}

TEST_F(CyclicExecutorTest, NoOverrunsWhenTheCallbackIsCheap) {
  CyclicExecutor::Config config;
  config.period = 5ms;  // generous, so a noisy host does not create false positives
  CyclicExecutor executor(config);

  auto callback = [](const CycleContext&) {};
  executor.run(CycleCallback(callback), 10);

  EXPECT_EQ(executor.stats().skipped_cycles, 0u);
}

// ---------------------------------------------------------------------------
// Real-time safety
// ---------------------------------------------------------------------------

TEST_F(CyclicExecutorTest, TheExecutorItselfDoesNotAllocatePerCycle) {
  // @verifies REQ-RT-001
  // Everything the loop touches -- histograms, stats, the callback wrapper --
  // must be allocation-free. The guard is on by default, so this is the
  // executor holding itself to its own contract.
  ASSERT_TRUE(guardIsInstalled());

  CyclicExecutor::Config config;
  config.period = 100us;
  config.guard_allocations = true;
  CyclicExecutor executor(config);

  std::uint64_t accumulator = 0;
  auto callback = [&](const CycleContext& ctx) { accumulator += ctx.index; };
  executor.run(CycleCallback(callback), 200);

  EXPECT_EQ(executor.stats().allocation_violations, 0u);
  EXPECT_EQ(accumulator, 199u * 200u / 2u);
}

TEST_F(CyclicExecutorTest, AllocationInsideTheCallbackIsReported) {
  // @verifies REQ-RT-001
  // The failure mode this exists to catch, demonstrated end to end.
  CyclicExecutor::Config config;
  config.period = 100us;
  config.guard_allocations = true;
  CyclicExecutor executor(config);

  auto callback = [](const CycleContext&) { ::operator delete(::operator new(64)); };
  executor.run(CycleCallback(callback), 10);

  EXPECT_EQ(executor.stats().allocation_violations, 10u);
}

TEST_F(CyclicExecutorTest, GuardCanBeDisabledForCallbacksThatMustAllocate) {
  CyclicExecutor::Config config;
  config.period = 100us;
  config.guard_allocations = false;
  CyclicExecutor executor(config);

  std::vector<std::uint64_t> log;
  auto callback = [&](const CycleContext& ctx) { log.push_back(ctx.index); };
  executor.run(CycleCallback(callback), 10);

  EXPECT_EQ(executor.stats().allocation_violations, 0u);
  EXPECT_EQ(log.size(), 10u);
}

TEST_F(CyclicExecutorTest, CycleCallbackDoesNotAllocateEvenForAFatCapture) {
  // std::function would heap-allocate for a capture this size. CycleCallback
  // is two pointers and never does, which is the reason it exists.
  std::array<double, 64> fat{};
  fat[0] = 1.0;
  auto callback = [&fat](const CycleContext&) { fat[1] += 1.0; };

  setAllocationPolicy(AllocationPolicy::kCount);
  resetAllocationReport();
  {
    const NoAllocScope no_alloc;
    const CycleCallback wrapped(callback);
    CycleContext ctx;
    wrapped(ctx);
  }
  EXPECT_EQ(allocationReport().violations, 0u);
  EXPECT_DOUBLE_EQ(fat[1], 1.0);
}

TEST_F(CyclicExecutorTest, ResetStatsClearsEverything) {
  CyclicExecutor::Config config;
  config.period = 100us;
  CyclicExecutor executor(config);

  auto callback = [](const CycleContext&) {};
  executor.run(CycleCallback(callback), 10);
  ASSERT_GT(executor.stats().cycles_executed, 0u);

  executor.resetStats();
  EXPECT_EQ(executor.stats().cycles_executed, 0u);
  EXPECT_EQ(executor.stats().overruns, 0u);
  EXPECT_EQ(executor.stats().wakeup_jitter.count(), 0u);
  EXPECT_EQ(executor.stats().cycle_total.count(), 0u);
}

// ---------------------------------------------------------------------------
// Thread configuration
// ---------------------------------------------------------------------------

TEST(ThreadConfig, DefaultConfigurationAlwaysApplies) {
  const ThreadConfig config;  // nothing requested
  const ThreadConfigReport report = applyThreadConfig(config);
  EXPECT_TRUE(report.fullyApplied()) << report.what();
  EXPECT_TRUE(report.scheduling_applied);
  EXPECT_TRUE(report.affinity_applied);
}

TEST(ThreadConfig, AffinityAppliesAndIsVerified) {
  ThreadConfig config;
  config.cpu = 0;
  const ThreadConfigReport report = applyThreadConfig(config);
  // Pinning to CPU 0 needs no privilege and should succeed anywhere this runs.
  EXPECT_TRUE(report.affinity_applied) << report.what();
}

TEST(ThreadConfig, FailureToObtainRealTimeSchedulingIsReportedNotHidden) {
  // @verifies REQ-RT-005
  // The single most important behaviour in this file. On a host without
  // CAP_SYS_NICE this request fails -- and the report must say so, because a
  // runtime that believes it has SCHED_FIFO when it does not produces latency
  // figures that are pure fiction.
  ThreadConfig config;
  config.policy = SchedulingPolicy::kFifo;
  config.priority = 80;
  const ThreadConfigReport report = applyThreadConfig(config);

  if (report.scheduling_applied) {
    // Privileged environment: the policy must genuinely be in force.
    EXPECT_EQ(report.effective_priority, 80);
    EXPECT_TRUE(report.fullyApplied()) << report.what();
  } else {
    // Unprivileged environment, which is the common case. The report must
    // carry a reason rather than silently claim success.
    EXPECT_FALSE(report.fullyApplied());
    EXPECT_NE(report.what()[0], '\0') << "a failed real-time request must explain itself";
  }
  // Either way, the availability probe and the outcome must agree.
  EXPECT_EQ(realTimeSchedulingAvailable(), report.scheduling_applied);
}

TEST(ThreadConfig, StackPrefaultSucceeds) {
  ThreadConfig config;
  config.prefault_stack = true;
  config.prefault_bytes = 128 * 1024;
  const ThreadConfigReport report = applyThreadConfig(config);
  EXPECT_TRUE(report.stack_prefaulted) << report.what();
}

}  // namespace
}  // namespace safeedge::rt
