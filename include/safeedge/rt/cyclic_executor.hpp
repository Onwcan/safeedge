// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "safeedge/rt/latency_histogram.hpp"
#include "safeedge/rt/thread_config.hpp"

namespace safeedge::rt {

/// What to do when a cycle finishes after its successor was already due.
enum class OverrunPolicy : std::uint8_t {
  /// Advance the deadline past every missed cycle and resume on the next one,
  /// counting what was skipped. Correct for a control loop: running four
  /// cycles back to back to "catch up" produces a burst of stale setpoints,
  /// which is worse for the machine than a clean gap the supervisor can see.
  kSkipMissed,
  /// Run every cycle regardless of lateness. The loop then drifts behind real
  /// time, but no iteration is ever dropped. Correct when the callback is a
  /// counter or an integrator whose every step matters.
  kRunLate,
};

/// Passed to the callback each cycle.
struct CycleContext {
  /// Monotonically increasing, starting at 0. Not the same as elapsed time
  /// divided by period when cycles are skipped -- see `skipped_cycles`.
  std::uint64_t index{0};
  /// How late the wakeup was relative to the scheduled deadline, in
  /// nanoseconds. This is the platform tax: on a tuned PREEMPT_RT machine it
  /// is single-digit microseconds, on a general-purpose kernel it is not.
  std::int64_t wakeup_jitter_ns{0};
  std::int64_t period_ns{0};
};

struct ExecutorStats {
  std::uint64_t cycles_executed{0};
  /// Cycles whose callback was still running when the next deadline passed.
  std::uint64_t overruns{0};
  /// Deadlines that went by without a callback running, under kSkipMissed.
  std::uint64_t skipped_cycles{0};
  /// Allocations detected on the real-time path, if the guard was linked.
  std::uint64_t allocation_violations{0};

  /// Scheduled deadline to actual wakeup.
  LatencyHistogram wakeup_jitter;
  /// Callback entry to callback return.
  LatencyHistogram execution_time;
  /// Scheduled deadline to callback return: the figure that must fit inside
  /// the period. Jitter plus execution, measured rather than added, so a
  /// correlation between the two cannot hide.
  LatencyHistogram cycle_total;

  void reset() noexcept;
};

/// Non-owning callable reference.
///
/// Deliberately not std::function: assigning a lambda that captures more than
/// a couple of words pushes std::function past its small-object buffer and it
/// heap-allocates. That would happen at setup rather than in the loop, so it
/// would be survivable -- but a type that *can* allocate has no business in
/// the signature of a real-time API, because the next person to touch it will
/// not know that. Two pointers, no ownership, no allocation, ever.
class CycleCallback {
 public:
  template <typename Callable>
  CycleCallback(Callable& callable) noexcept  // NOLINT(google-explicit-constructor)
      : object_(&callable), invoke_([](void* obj, const CycleContext& ctx) {
          (*static_cast<Callable*>(obj))(ctx);
        }) {}

  void operator()(const CycleContext& ctx) const { invoke_(object_, ctx); }

 private:
  void* object_;
  void (*invoke_)(void*, const CycleContext&);
};

/// A fixed-period execution loop with absolute-deadline scheduling.
///
/// The scheduling property that matters
/// ------------------------------------
/// Each deadline is computed by adding the period to the *previous deadline*,
/// never to the current time, and the wait is `clock_nanosleep` with
/// TIMER_ABSTIME. The naive alternative -- sleep for `period` each iteration --
/// accumulates every wakeup delay into permanent drift: at 1 kHz with 50 us of
/// average lateness, the loop is a full second behind after twenty seconds and
/// nothing in it ever reports an error.
///
/// With absolute deadlines, a late wakeup is absorbed by the next interval and
/// shows up as jitter rather than drift. Long-run cadence stays exact.
class CyclicExecutor {
 public:
  struct Config {
    std::chrono::nanoseconds period{std::chrono::milliseconds(1)};
    ThreadConfig thread{};
    OverrunPolicy overrun_policy{OverrunPolicy::kSkipMissed};
    /// Wrap each callback invocation in a NoAllocScope. Costs an increment and
    /// a decrement; catches the class of defect that otherwise only appears
    /// under load in the field.
    bool guard_allocations{true};
  };

  explicit CyclicExecutor(Config config) noexcept;

  /// Applies the thread configuration to the calling thread and returns what
  /// actually took effect. Call before `run` and check the result: latency
  /// figures gathered without the scheduling you asked for describe the
  /// machine's mood, not the code.
  [[nodiscard]] ThreadConfigReport configureCallingThread() noexcept;

  /// Runs on the calling thread until `requestStop()` or until `max_cycles`
  /// have executed. `max_cycles == 0` means run until stopped.
  void run(const CycleCallback& callback, std::uint64_t max_cycles = 0) noexcept;

  /// Safe to call from any thread, including from inside the callback.
  void requestStop() noexcept { stop_requested_.store(true, std::memory_order_relaxed); }
  [[nodiscard]] bool stopRequested() const noexcept {
    return stop_requested_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] const ExecutorStats& stats() const noexcept { return stats_; }
  void resetStats() noexcept;

  [[nodiscard]] const Config& config() const noexcept { return config_; }

 private:
  Config config_;
  ExecutorStats stats_{};
  std::atomic<bool> stop_requested_{false};
};

}  // namespace safeedge::rt
