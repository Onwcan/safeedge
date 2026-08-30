// SPDX-License-Identifier: Apache-2.0
#include "safeedge/edge/metrics.hpp"

#include <cinttypes>
#include <cstdio>

namespace safeedge::edge {
namespace {

void appendMetric(std::string& out, const char* name, const char* help, const char* type,
                  double value) {
  char line[512];
  static_cast<void>(std::snprintf(line, sizeof(line),
                                  "# HELP %s %s\n# TYPE %s %s\n%s %.6g\n", name, help,
                                  name, type, name, value));
  out += line;
}

void appendLabelled(std::string& out, const char* name, const char* help,
                    const char* type, const char* label_name, const char* label_value,
                    double value) {
  char line[512];
  static_cast<void>(std::snprintf(
      line, sizeof(line), "# HELP %s %s\n# TYPE %s %s\n%s{%s=\"%s\"} %.6g\n", name, help,
      name, type, name, label_name, label_value, value));
  out += line;
}

/// Quantiles are exported as a Prometheus *summary* rather than a histogram.
///
/// A histogram exports bucket counts and lets the server compute quantiles,
/// which is right when you need to aggregate across many instances. A summary
/// exports quantiles the client already computed, which is right here: the
/// quantiles come from a fixed-bucket histogram inside the real-time loop, and
/// re-deriving them server-side would only add error on top of the bucketing
/// error already present.
///
/// It also makes the limitation visible. Summary quantiles cannot be averaged
/// across instances, and a dashboard that tries will be wrong -- which is true
/// of this data regardless of how it is exported, so the format may as well say
/// so.
void appendQuantiles(std::string& out, const char* name, const char* help,
                     std::int64_t p50, std::int64_t p99, std::int64_t p999,
                     std::int64_t maximum) {
  char header[256];
  static_cast<void>(std::snprintf(header, sizeof(header),
                                  "# HELP %s %s\n# TYPE %s summary\n", name, help, name));
  out += header;

  char line[256];
  static_cast<void>(
      std::snprintf(line, sizeof(line), "%s{quantile=\"0.5\"} %" PRId64 "\n", name, p50));
  out += line;
  static_cast<void>(std::snprintf(line, sizeof(line),
                                  "%s{quantile=\"0.99\"} %" PRId64 "\n", name, p99));
  out += line;
  static_cast<void>(std::snprintf(line, sizeof(line),
                                  "%s{quantile=\"0.999\"} %" PRId64 "\n", name, p999));
  out += line;
  static_cast<void>(
      std::snprintf(line, sizeof(line), "%s_max %" PRId64 "\n", name, maximum));
  out += line;
}

}  // namespace

std::string renderPrometheus(const RuntimeSnapshot& snapshot, bool snapshot_is_fresh) {
  std::string out;
  out.reserve(4096);

  out +=
      "# safeedge runtime metrics\n"
      "# Scraped from a seqlock snapshot published by the real-time loop; the\n"
      "# scrape never blocks that loop and never takes a lock on its path.\n";

  appendMetric(out, "safeedge_snapshot_fresh",
               "1 if the last scrape read a complete snapshot, 0 if it gave up retrying",
               "gauge", snapshot_is_fresh ? 1 : 0);

  // --- the honesty gauge ---------------------------------------------------
  // Exported first and deliberately prominent. Every latency figure below is
  // meaningless if this is 0, and a dashboard showing microsecond jitter
  // without showing this would be actively misleading.
  appendMetric(out, "safeedge_realtime_scheduling_granted",
               "1 if the runtime obtained the real-time scheduling it requested. "
               "When 0, every latency metric below describes the machine's load, not "
               "this code",
               "gauge", snapshot.realtime_scheduling_granted);

  appendMetric(out, "safeedge_cycles_total", "Control cycles executed", "counter",
               static_cast<double>(snapshot.cycles_executed));
  appendMetric(out, "safeedge_overruns_total",
               "Cycles that completed after their successor was already due", "counter",
               static_cast<double>(snapshot.overruns));
  appendMetric(out, "safeedge_skipped_cycles_total",
               "Deadlines passed without a cycle running", "counter",
               static_cast<double>(snapshot.skipped_cycles));
  appendMetric(out, "safeedge_allocation_violations_total",
               "Allocations detected on the real-time path. Any value above zero is a "
               "defect, not a warning",
               "counter", static_cast<double>(snapshot.allocation_violations));

  appendQuantiles(out, "safeedge_wakeup_jitter_nanoseconds",
                  "Scheduled deadline to actual wakeup", snapshot.jitter_p50_ns,
                  snapshot.jitter_p99_ns, snapshot.jitter_p999_ns,
                  snapshot.jitter_max_ns);
  appendQuantiles(out, "safeedge_execution_nanoseconds",
                  "Callback entry to callback return", snapshot.execution_p50_ns,
                  snapshot.execution_p99_ns, snapshot.execution_p99_ns,
                  snapshot.execution_max_ns);

  appendMetric(out, "safeedge_safety_state",
               "Supervisor state: 0=STO 1=SelfTest 2=Stopping 3=SOS 4=SLS 5=Operational",
               "gauge", snapshot.safety_state);
  appendMetric(out, "safeedge_fault_reason",
               "Latched fault reason, 0 when none. See safety_state_machine.hpp", "gauge",
               snapshot.fault_reason);
  appendMetric(out, "safeedge_torque_permitted", "1 when torque is permitted", "gauge",
               snapshot.torque_permitted);
  appendMetric(out, "safeedge_fault_latched",
               "1 while a fault is latched and awaiting acknowledgement", "gauge",
               snapshot.fault_latched);
  appendMetric(out, "safeedge_estop_asserted",
               "1 when an external emergency stop is demanding a stop", "gauge",
               snapshot.estop_asserted);
  appendMetric(out, "safeedge_safety_sequence",
               "Increments on every safety state transition; lets a consumer tell a "
               "repeated report from a new decision",
               "counter", static_cast<double>(snapshot.safety_sequence));
  // The transition *instant* is deliberately not a metric. It is a nanosecond
  // CLOCK_MONOTONIC value near 1e18, and this exposition format renders a
  // double with six significant digits -- the exported number would be wrong by
  // hundreds of millions of nanoseconds. The exact value is served by /safety,
  // as text, for consumers that need to compute against it. What belongs on a
  // dashboard is the age.
  appendMetric(out, "safeedge_safety_state_age_seconds",
               "How long the runtime has been in its current safety state", "gauge",
               static_cast<double>(snapshot.safety_state_age_ns) / 1e9);

  appendLabelled(out, "safeedge_telegrams_total", "Safety telegrams processed", "counter",
                 "result", "accepted", static_cast<double>(snapshot.telegrams_accepted));
  char line[256];
  static_cast<void>(std::snprintf(line, sizeof(line),
                                  "safeedge_telegrams_total{result=\"rejected\"} %.6g\n",
                                  static_cast<double>(snapshot.telegrams_rejected)));
  out += line;

  appendMetric(out, "safeedge_uptime_seconds", "Time since the runtime started", "gauge",
               static_cast<double>(snapshot.uptime_ns) / 1e9);
  return out;
}

}  // namespace safeedge::edge
