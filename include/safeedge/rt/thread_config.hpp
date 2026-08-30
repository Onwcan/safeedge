// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace safeedge::rt {

enum class SchedulingPolicy : std::uint8_t {
  /// Leave the thread on the default time-sharing scheduler.
  kDefault,
  /// SCHED_FIFO: runs until it blocks or yields, never preempted by anything
  /// of lower priority. What a control loop needs.
  kFifo,
  /// SCHED_RR: like FIFO but round-robins between threads of equal priority.
  kRoundRobin,
};

struct ThreadConfig {
  SchedulingPolicy policy{SchedulingPolicy::kDefault};
  /// 1..99 for FIFO/RR. Higher is more urgent. Deliberately not defaulted to
  /// 99: a runaway loop at the top priority can lock out kernel threads and
  /// make a machine unrecoverable without a power cycle.
  int priority{0};
  /// CPU to pin to, or -1 for no affinity change.
  int cpu{-1};
  /// mlockall(MCL_CURRENT | MCL_FUTURE). Prevents the loop from taking a page
  /// fault, which would mean a disk read inside a 1 ms cycle.
  bool lock_memory{false};
  /// Touch stack pages up front so they are resident before the loop starts.
  /// mlockall alone does not help with stack pages that have never been
  /// written -- they are not mapped yet, so there is nothing to lock.
  bool prefault_stack{false};
  std::size_t prefault_bytes{std::size_t{512} * 1024U};
};

/// What actually happened when the configuration was applied.
///
/// Every field is reported rather than assumed, because in the environments
/// this runtime ships to, requesting real-time scheduling routinely fails:
/// a container without CAP_SYS_NICE, an RLIMIT_RTPRIO of 0, or a virtualised
/// kernel that accepts the call and ignores it. A runtime that assumes it got
/// SCHED_FIFO and quietly did not is a runtime whose every latency figure is
/// a lie -- and that lie surfaces as an unexplained field failure, not as a
/// test failure.
struct ThreadConfigReport {
  bool scheduling_applied{false};
  bool affinity_applied{false};
  bool memory_locked{false};
  bool stack_prefaulted{false};

  /// Policy and priority actually in force after the attempt, read back from
  /// the kernel rather than echoed from the request.
  int effective_policy{0};
  int effective_priority{0};

  /// Human-readable reasons for anything that did not apply. Fixed storage:
  /// this may be produced on a thread that must not allocate.
  std::array<char, 512> diagnostics{};

  [[nodiscard]] bool fullyApplied() const noexcept;
  [[nodiscard]] const char* what() const noexcept { return diagnostics.data(); }
};

/// Applies `config` to the calling thread. Never throws; failures are reported
/// in the return value rather than raised, because partial application is a
/// normal and recoverable condition, not an exceptional one.
[[nodiscard]] ThreadConfigReport applyThreadConfig(const ThreadConfig& config) noexcept;

/// True if this process could plausibly obtain real-time scheduling. Cheap
/// pre-flight check so a caller can warn early rather than discover it after
/// collecting a run of meaningless measurements.
[[nodiscard]] bool realTimeSchedulingAvailable() noexcept;

}  // namespace safeedge::rt
