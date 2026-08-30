// SPDX-License-Identifier: Apache-2.0
#include "safeedge/rt/cyclic_executor.hpp"

#include <cerrno>
#include <ctime>

#include "safeedge/rt/no_alloc_guard.hpp"

namespace safeedge::rt {
namespace {

constexpr std::int64_t kNanosPerSecond = 1'000'000'000;

timespec nowMonotonic() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts;
}

std::int64_t toNanos(const timespec& ts) noexcept {
  return (static_cast<std::int64_t>(ts.tv_sec) * kNanosPerSecond) +
         static_cast<std::int64_t>(ts.tv_nsec);
}

timespec fromNanos(std::int64_t nanos) noexcept {
  timespec ts{};
  ts.tv_sec = static_cast<std::time_t>(nanos / kNanosPerSecond);
  ts.tv_nsec = static_cast<long>(nanos % kNanosPerSecond);
  return ts;
}

/// Sleeps until an absolute point on CLOCK_MONOTONIC.
///
/// TIMER_ABSTIME rather than a relative sleep is the whole scheduling design:
/// a relative sleep restarts its countdown from whenever the thread happened
/// to wake, so every delay is permanently added to the schedule. An absolute
/// deadline absorbs the delay instead.
///
/// EINTR is retried rather than treated as a wakeup. A signal arriving mid-wait
/// would otherwise make the loop run early, which reads as negative jitter and
/// corrupts the histogram with a value that is not a latency at all.
void sleepUntil(const timespec& deadline) noexcept {
  while (true) {
    const int rc = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
    if (rc == 0) {
      return;
    }
    if (rc != EINTR) {
      return;  // nothing useful to do; the jitter measurement will show it
    }
  }
}

}  // namespace

void ExecutorStats::reset() noexcept {
  cycles_executed = 0;
  overruns = 0;
  skipped_cycles = 0;
  allocation_violations = 0;
  wakeup_jitter.reset();
  execution_time.reset();
  cycle_total.reset();
}

CyclicExecutor::CyclicExecutor(Config config) noexcept : config_(config) {}

// This is intentionally non-const to preserve the installed library's original
// ABI. Changing only the cv-qualification changes the exported C++ symbol.
// NOLINTNEXTLINE(readability-make-member-function-const)
ThreadConfigReport CyclicExecutor::configureCallingThread() noexcept {
  return applyThreadConfig(config_.thread);
}

void CyclicExecutor::resetStats() noexcept { stats_.reset(); }

void CyclicExecutor::run(const CycleCallback& callback,
                         std::uint64_t max_cycles) noexcept {
  const auto period_ns = static_cast<std::int64_t>(config_.period.count());
  if (period_ns <= 0) {
    return;
  }

  stop_requested_.store(false, std::memory_order_relaxed);

  const std::uint64_t violations_at_start = allocationReport().violations;

  // The deadline sequence originates here and is only ever advanced by exact
  // multiples of the period. Nothing downstream reads the clock to decide when
  // the next cycle is due, which is what makes the cadence drift-free.
  // @satisfies REQ-RT-002
  std::int64_t deadline_ns = toNanos(nowMonotonic()) + period_ns;

  for (std::uint64_t index = 0; max_cycles == 0 || index < max_cycles; ++index) {
    if (stop_requested_.load(std::memory_order_relaxed)) {
      break;
    }

    const timespec deadline = fromNanos(deadline_ns);
    sleepUntil(deadline);

    const std::int64_t woke_ns = toNanos(nowMonotonic());
    const std::int64_t jitter_ns = woke_ns - deadline_ns;
    stats_.wakeup_jitter.record(jitter_ns);

    CycleContext context;
    context.index = index;
    context.wakeup_jitter_ns = jitter_ns;
    context.period_ns = period_ns;

    std::int64_t finished_ns = 0;
    if (config_.guard_allocations) {
      // @satisfies REQ-RT-001
      const NoAllocScope no_alloc;
      callback(context);
      finished_ns = toNanos(nowMonotonic());
    } else {
      callback(context);
      finished_ns = toNanos(nowMonotonic());
    }

    stats_.execution_time.record(finished_ns - woke_ns);
    stats_.cycle_total.record(finished_ns - deadline_ns);
    ++stats_.cycles_executed;

    // Advance to the next scheduled deadline.
    deadline_ns += period_ns;

    // @satisfies REQ-RT-003
    if (finished_ns >= deadline_ns) {
      // The cycle ran past the point at which its successor was already due.
      ++stats_.overruns;
      if (config_.overrun_policy == OverrunPolicy::kSkipMissed) {
        // Skip forward to the next deadline that is still in the future,
        // counting what was missed. Running the skipped cycles back to back
        // would emit a burst of stale setpoints -- worse for a machine than a
        // clean gap the supervisor can see and react to.
        while (deadline_ns <= finished_ns) {
          deadline_ns += period_ns;
          ++stats_.skipped_cycles;
        }
      }
      // Under kRunLate the deadline is left where it is, so the next iteration
      // does not wait at all and the loop drifts behind real time by design.
    }
  }

  stats_.allocation_violations = allocationReport().violations - violations_at_start;
}

}  // namespace safeedge::rt
