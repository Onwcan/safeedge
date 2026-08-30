// SPDX-License-Identifier: Apache-2.0
#include "safeedge/rt/thread_config.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace safeedge::rt {
namespace {

/// Appends to a fixed buffer without allocating. Silently truncates rather
/// than growing -- diagnostics must never be the reason a real-time thread
/// touches the allocator.
void appendDiagnostic(std::array<char, 512>& buffer, const char* text) noexcept {
  const std::size_t used = std::strlen(buffer.data());
  const std::size_t remaining = buffer.size() - used - 1;
  if (remaining == 0) {
    return;
  }
  if (used > 0 && remaining > 2) {
    buffer[used] = ';';
    buffer[used + 1] = ' ';
    std::strncpy(buffer.data() + used + 2, text, remaining - 2);
  } else {
    std::strncpy(buffer.data() + used, text, remaining);
  }
  buffer[buffer.size() - 1] = '\0';
}

void appendErrno(std::array<char, 512>& buffer, const char* prefix, int error) noexcept {
  char line[192];
  // Keep this path allocation-free and thread-safe. The numeric errno remains
  // unambiguous and can be decoded without calling strerror's shared buffer.
  static_cast<void>(
      std::snprintf(line, sizeof(line), "%s failed (errno %d)", prefix, error));
  appendDiagnostic(buffer, line);
}

#if defined(__linux__)
int toPosixPolicy(SchedulingPolicy policy) noexcept {
  switch (policy) {
    case SchedulingPolicy::kFifo:
      return SCHED_FIFO;
    case SchedulingPolicy::kRoundRobin:
      return SCHED_RR;
    case SchedulingPolicy::kDefault:
      break;
  }
  return SCHED_OTHER;
}
#endif

}  // namespace

bool ThreadConfigReport::fullyApplied() const noexcept { return diagnostics[0] == '\0'; }

bool realTimeSchedulingAvailable() noexcept {
#if defined(__linux__)
  // RLIMIT_RTPRIO of 0 means no real-time priority may be requested at all,
  // which is the default in many container runtimes.
  rlimit limit{};
  if (::getrlimit(RLIMIT_RTPRIO, &limit) == 0 && limit.rlim_cur > 0) {
    return true;
  }
  // CAP_SYS_NICE would also permit it, and root always can. Rather than parse
  // capabilities, probe: ask for the lowest real-time priority and put it back.
  const int previous_policy = ::sched_getscheduler(0);
  sched_param probe{};
  probe.sched_priority = 1;
  if (::sched_setscheduler(0, SCHED_FIFO, &probe) == 0) {
    sched_param restore{};
    restore.sched_priority = 0;
    (void)::sched_setscheduler(0, previous_policy < 0 ? SCHED_OTHER : previous_policy,
                               &restore);
    return true;
  }
  return false;
#else
  return false;
#endif
}

ThreadConfigReport applyThreadConfig(const ThreadConfig& config) noexcept {
  ThreadConfigReport report;
  report.diagnostics[0] = '\0';

#if !defined(__linux__)
  appendDiagnostic(report.diagnostics,
                   "real-time thread configuration is only implemented for Linux");
  return report;
#else

  // --- scheduling policy and priority -------------------------------------
  bool set_call_succeeded = false;
  if (config.policy == SchedulingPolicy::kDefault) {
    report.scheduling_applied = true;
    set_call_succeeded = true;
  } else {
    sched_param param{};
    param.sched_priority = config.priority;
    const int policy = toPosixPolicy(config.policy);
    const int rc = ::pthread_setschedparam(::pthread_self(), policy, &param);
    if (rc == 0) {
      report.scheduling_applied = true;
      set_call_succeeded = true;
    } else {
      appendErrno(report.diagnostics, "pthread_setschedparam", rc);
    }
  }

  // @satisfies REQ-RT-005
  // Read back what is actually in force. Requesting SCHED_FIFO and getting
  // SCHED_OTHER is the failure this whole struct exists to make visible, and
  // on some virtualised kernels the call succeeds while the policy does not
  // change -- so trusting the return code alone is not enough.
  {
    int actual_policy = 0;
    sched_param actual_param{};
    if (::pthread_getschedparam(::pthread_self(), &actual_policy, &actual_param) == 0) {
      report.effective_policy = actual_policy;
      report.effective_priority = actual_param.sched_priority;
      // Only meaningful when the set call reported success. Adding it after a
      // call that already failed with EPERM produces a contradictory report --
      // "failed" followed by "did not take effect despite a successful call" --
      // which is worse than saying nothing, because it makes the reader doubt
      // the part that was true.
      if (set_call_succeeded && config.policy != SchedulingPolicy::kDefault &&
          actual_policy != toPosixPolicy(config.policy)) {
        report.scheduling_applied = false;
        appendDiagnostic(
            report.diagnostics,
            "scheduling policy did not take effect despite a successful call");
      }
    }
  }

  // --- CPU affinity --------------------------------------------------------
  if (config.cpu < 0) {
    report.affinity_applied = true;
  } else {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(config.cpu), &set);
    const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    if (rc != 0) {
      appendErrno(report.diagnostics, "pthread_setaffinity_np", rc);
    } else {
      // Verify rather than assume, for the same reason as above.
      cpu_set_t check;
      CPU_ZERO(&check);
      if (::pthread_getaffinity_np(::pthread_self(), sizeof(check), &check) == 0 &&
          CPU_ISSET(static_cast<std::size_t>(config.cpu), &check) &&
          CPU_COUNT(&check) == 1) {
        report.affinity_applied = true;
      } else {
        appendDiagnostic(report.diagnostics, "cpu affinity did not take effect");
      }
    }
  }

  // --- memory locking ------------------------------------------------------
  if (!config.lock_memory || ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
    report.memory_locked = true;
  } else {
    appendErrno(report.diagnostics, "mlockall", errno);
  }

  // --- stack prefault ------------------------------------------------------
  if (!config.prefault_stack) {
    report.stack_prefaulted = true;
  } else {
    // Touch one byte per page down the stack so the pages are mapped and
    // resident before the loop starts. mlockall cannot lock a page that has
    // never been written, because it does not exist yet -- so a loop that
    // grows its stack on cycle 1000 takes a page fault there regardless.
    //
    // volatile prevents the compiler from eliding stores it can prove are
    // never read.
    const long page = ::sysconf(_SC_PAGESIZE);
    const std::size_t page_size = page > 0 ? static_cast<std::size_t>(page) : 4096U;
    const std::size_t pages = config.prefault_bytes / page_size;
    if (pages > 0 && pages < 65536) {
      volatile char scratch[65536];  // NOLINT(cppcoreguidelines-avoid-c-arrays)
      const std::size_t touch =
          pages * page_size < sizeof(scratch) ? pages * page_size : sizeof(scratch);
      for (std::size_t offset = 0; offset < touch; offset += page_size) {
        scratch[offset] = 0;
      }
      report.stack_prefaulted = true;
    } else {
      appendDiagnostic(report.diagnostics, "prefault_bytes out of supported range");
    }
  }

  return report;
#endif
}

}  // namespace safeedge::rt
