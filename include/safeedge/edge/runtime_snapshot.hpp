// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace safeedge::edge {

/// What the real-time loop publishes for anything outside it to look at.
///
/// Why a flat POD and not a reference to the live objects
/// -----------------------------------------------------
/// The metrics endpoint runs on an HTTP thread with no deadline. If it read the
/// executor's statistics and the supervisor's state directly, it would be
/// reading structures the real-time thread is concurrently writing, and the
/// choice would be between a lock on the deadline path -- unacceptable -- and a
/// data race.
///
/// So the real-time thread publishes this snapshot into an `ipc::SeqlockSlot`
/// once per cycle, and the HTTP thread reads whatever the latest complete one
/// is. The publish is wait-free, the read never blocks the writer, and a
/// scrape that arrives mid-write simply retries. Metrics are exactly the case
/// a seqlock is for: only the newest value has any meaning, and a scraper that
/// misses three cycles has lost nothing.
///
/// Trivially copyable and free of pointers, so it can also live in a shared
/// memory region when the metrics exporter is moved to its own process.
struct RuntimeSnapshot {
  std::uint64_t cycles_executed{0};
  std::uint64_t overruns{0};
  std::uint64_t skipped_cycles{0};
  std::uint64_t allocation_violations{0};

  /// Wakeup jitter, nanoseconds.
  std::int64_t jitter_p50_ns{0};
  std::int64_t jitter_p99_ns{0};
  std::int64_t jitter_p999_ns{0};
  std::int64_t jitter_max_ns{0};

  /// Callback execution time, nanoseconds.
  std::int64_t execution_p50_ns{0};
  std::int64_t execution_p99_ns{0};
  std::int64_t execution_max_ns{0};

  /// `safety::SafetyState` as an integer, so this header does not drag the
  /// whole safety component into the metrics path.
  std::uint32_t safety_state{0};
  /// `safety::FaultReason` as an integer.
  std::uint32_t fault_reason{0};
  std::uint32_t torque_permitted{0};
  std::uint32_t fault_latched{0};

  /// Safety telegrams accepted and rejected since start.
  std::uint64_t telegrams_accepted{0};
  std::uint64_t telegrams_rejected{0};

  /// Whether the runtime obtained the real-time scheduling it asked for.
  /// Exported so an operator can see from Grafana that the latency figures on
  /// the same dashboard are not trustworthy -- the single most useful thing
  /// this snapshot carries, and the one most often left out.
  // @satisfies REQ-EDGE-003
  std::uint32_t realtime_scheduling_granted{0};

  std::int64_t uptime_ns{0};
};

}  // namespace safeedge::edge
