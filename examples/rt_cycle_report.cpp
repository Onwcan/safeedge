// SPDX-License-Identifier: Apache-2.0
//
// Runs the cyclic executor at a requested rate and reports what the platform
// actually delivered.
//
// The report leads with measurement conditions rather than with numbers,
// because on a host that could not obtain SCHED_FIFO the numbers describe the
// machine's mood and not this code. Printing them without that context would be
// the most flattering and least honest thing this program could do.
//
//   ./rt_cycle_report [frequency_hz] [seconds]

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "safeedge/rt/cyclic_executor.hpp"
#include "safeedge/rt/no_alloc_guard.hpp"
#include "safeedge/rt/thread_config.hpp"

namespace {

using namespace safeedge::rt;

bool isVirtualised() {
  std::ifstream version("/proc/version");
  if (!version) {
    return false;
  }
  std::string line;
  std::getline(version, line);
  return line.find("microsoft") != std::string::npos ||
         line.find("Microsoft") != std::string::npos ||
         line.find("WSL") != std::string::npos;
}

const char* policyName(int policy) {
  switch (policy) {
    case 0:
      return "SCHED_OTHER";
    case 1:
      return "SCHED_FIFO";
    case 2:
      return "SCHED_RR";
    default:
      return "unknown";
  }
}

void printHistogram(const char* label, const LatencyHistogram& histogram) {
  std::printf("| %-22s | %7" PRId64 " | %7" PRId64 " | %8" PRId64 " | %9" PRId64
              " | %10" PRId64 " |\n",
              label, histogram.min(), histogram.p50(), histogram.p99(), histogram.p999(),
              histogram.max());
}

}  // namespace

int main(int argc, char** argv) {
  const long frequency_hz = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 1000;
  const long seconds = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 5;
  if (frequency_hz <= 0 || seconds <= 0) {
    std::fprintf(stderr, "usage: %s [frequency_hz] [seconds]\n", argv[0]);
    return 2;
  }

  const auto period = std::chrono::nanoseconds(1'000'000'000LL / frequency_hz);
  const auto cycles = static_cast<std::uint64_t>(frequency_hz * seconds);

  CyclicExecutor::Config config;
  config.period = period;
  config.overrun_policy = OverrunPolicy::kSkipMissed;
  config.guard_allocations = true;
  config.thread.policy = SchedulingPolicy::kFifo;
  config.thread.priority = 80;
  config.thread.cpu = 2;
  config.thread.lock_memory = true;
  config.thread.prefault_stack = true;

  CyclicExecutor executor(config);
  const ThreadConfigReport thread_report = executor.configureCallingThread();

  setAllocationPolicy(AllocationPolicy::kCount);
  resetAllocationReport();

  std::printf("# safeedge cyclic executor report\n\n");
  std::printf("## Measurement conditions\n\n");
  std::printf("- Requested: %ld Hz for %ld s (%" PRIu64 " cycles, %" PRId64
              " ns period)\n",
              frequency_hz, seconds, cycles, static_cast<std::int64_t>(period.count()));
  std::printf("- Scheduling requested: SCHED_FIFO priority 80\n");
  std::printf("- Scheduling obtained:  %s priority %d  -> %s\n",
              policyName(thread_report.effective_policy),
              thread_report.effective_priority,
              thread_report.scheduling_applied ? "GRANTED" : "DENIED");
  std::printf("- CPU pinning:          %s\n",
              thread_report.affinity_applied ? "verified" : "NOT APPLIED");
  std::printf("- Memory locked:        %s\n",
              thread_report.memory_locked ? "yes (mlockall)" : "NO");
  std::printf("- Stack prefaulted:     %s\n",
              thread_report.stack_prefaulted ? "yes" : "no");
  std::printf("- Allocation guard:     %s\n",
              guardIsInstalled() ? "installed" : "NOT LINKED");
  std::printf("- Virtualised host:     %s\n", isVirtualised() ? "YES" : "no");
  if (!thread_report.fullyApplied()) {
    std::printf("- Diagnostics:          %s\n", thread_report.what());
  }

  const bool trustworthy = thread_report.scheduling_applied &&
                           thread_report.affinity_applied && !isVirtualised();
  if (!trustworthy) {
    std::printf("\n");
    std::printf("> The figures below are NOT a characterisation of this code.\n");
    std::printf(
        "> Without real-time scheduling on a non-virtualised host, the tail is\n");
    std::printf(
        "> dominated by other threads and by the hypervisor. Treat the median as\n");
    std::printf(
        "> indicative and everything above p99 as a measurement of the platform.\n");
    std::printf(
        "> Run this on tuned bare-metal Linux before quoting any number from it.\n");
  }
  std::printf("\n");

  // The work itself: a stand-in for a control law. Trivial and constant-time,
  // so that what is measured is the loop and not the payload.
  std::uint64_t accumulator = 0;
  auto control_step = [&accumulator](const CycleContext& ctx) {
    accumulator += ctx.index ^ static_cast<std::uint64_t>(ctx.wakeup_jitter_ns);
  };

  executor.run(CycleCallback(control_step), cycles);
  const ExecutorStats& stats = executor.stats();

  std::printf("## Results\n\n");
  std::printf(
      "| Measurement (ns)       |     min |     p50 |      p99 |     p99.9 |        max "
      "|\n");
  std::printf(
      "|------------------------|---------|---------|----------|-----------|------------|"
      "\n");
  printHistogram("wakeup jitter", stats.wakeup_jitter);
  printHistogram("callback execution", stats.execution_time);
  printHistogram("deadline to complete", stats.cycle_total);

  std::printf("\n");
  std::printf("- Cycles executed:       %" PRIu64 "\n", stats.cycles_executed);
  std::printf(
      "- Overruns:              %" PRIu64 " (%.4f%%)\n", stats.overruns,
      100.0 * static_cast<double>(stats.overruns) /
          static_cast<double>(stats.cycles_executed == 0 ? 1 : stats.cycles_executed));
  std::printf("- Deadlines skipped:     %" PRIu64 "\n", stats.skipped_cycles);
  std::printf("- Allocation violations: %" PRIu64 "%s\n", stats.allocation_violations,
              stats.allocation_violations == 0 ? "" : "  <-- DEFECT ON THE RT PATH");
  std::printf("- Mean wakeup jitter:    %.0f ns\n", stats.wakeup_jitter.mean());

  if (accumulator == 0xFFFFFFFFFFFFFFFFULL) {
    std::fputs("", stderr);  // keep the optimiser honest
  }
  return 0;
}
