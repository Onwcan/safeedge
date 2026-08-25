// SPDX-License-Identifier: Apache-2.0
//
// safeedged -- the runtime as a long-lived service.
//
// Runs the cyclic executor with the safety supervisor in the loop, exchanges
// safety telegrams over the black channel, and exposes liveness, readiness and
// Prometheus metrics on an HTTP port.
//
// Written to be PID 1 in a container, which changes two things most daemons get
// to ignore:
//
//   * PID 1 has no default signal dispositions. SIGTERM does not terminate it
//     unless a handler is installed. A container that ignores SIGTERM makes
//     every `docker stop` a ten-second wait followed by SIGKILL, and SIGKILL
//     means the runtime never gets to drive the machine to a safe state.
//
//   * PID 1 is expected to reap orphans. This process forks nothing, so there
//     are none -- worth stating, because the usual answer is to add an init
//     shim, and adding one that is not needed is its own kind of cargo cult.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "safeedge/edge/http_server.hpp"
#include "safeedge/edge/metrics.hpp"
#include "safeedge/edge/runtime_snapshot.hpp"
#include "safeedge/ipc/seqlock_slot.hpp"
#include "safeedge/rt/cyclic_executor.hpp"
#include "safeedge/rt/no_alloc_guard.hpp"
#include "safeedge/safety/black_channel.hpp"
#include "safeedge/safety/dual_channel.hpp"

namespace {

using namespace safeedge;

/// Set from a signal handler, so it must be sig_atomic_t and nothing more.
/// std::atomic<bool> happens to work on every implementation here, but the
/// standard only guarantees this type.
volatile std::sig_atomic_t g_shutdown_requested = 0;

// @satisfies REQ-EDGE-001
extern "C" void onSignal(int) { g_shutdown_requested = 1; }

/// Single-line JSON to stdout.
///
/// Structured because a container's logs are collected by machine, and a human
/// reading them is the exception rather than the rule. Unbuffered because a
/// process killed mid-flush loses whatever was still in the buffer, and the
/// last few lines before a crash are the ones anyone actually wants.
void logEvent(const char* level, const char* message, const char* detail = nullptr) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  if (detail != nullptr) {
    std::printf(R"({"ts":%lld,"level":"%s","msg":"%s","detail":"%s"})"
                "\n",
                static_cast<long long>(now), level, message, detail);
  } else {
    std::printf(R"({"ts":%lld,"level":"%s","msg":"%s"})"
                "\n",
                static_cast<long long>(now), level, message);
  }
  std::fflush(stdout);
}

long environmentLong(const char* name, long fallback) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  return (end != nullptr && *end == '\0') ? parsed : fallback;
}

/// Self-contained health probe, so the container image needs no shell.
///
/// A `scratch` image has no curl, no wget and no /bin/sh, which means the usual
/// `HEALTHCHECK CMD curl -f ...` cannot run. The alternatives are to abandon
/// the minimal base -- pulling in a whole userland to run one HTTP GET -- or to
/// teach the binary to probe itself. This is the second.
///
/// Deliberately tiny: connect, send a request line, look for "200" in the
/// first bytes of the response. It is not a general HTTP client and does not
/// need to be.
int runHealthcheck(std::uint16_t port) {
  const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    return 1;
  }

  timeval timeout{};
  timeout.tv_sec = 2;
  (void)::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(socket_fd);
    return 1;
  }

  const char* request =
      "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  const std::size_t length = std::strlen(request);
  if (::write(socket_fd, request, length) != static_cast<ssize_t>(length)) {
    ::close(socket_fd);
    return 1;
  }

  std::array<char, 64> response{};
  const ssize_t received = ::read(socket_fd, response.data(), response.size() - 1);
  ::close(socket_fd);
  if (received <= 0) {
    return 1;
  }
  return std::strstr(response.data(), " 200 ") != nullptr ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const auto metrics_port =
      static_cast<std::uint16_t>(environmentLong("SAFEEDGE_METRICS_PORT", 9100));

  // Health probe mode. Checked before anything is started, because this
  // invocation must do nothing except ask the *other* process how it is.
  if (argc > 1 && std::strcmp(argv[1], "--healthcheck") == 0) {
    return runHealthcheck(metrics_port);
  }

  // Handlers before anything else: a SIGTERM arriving during startup must still
  // be honoured, and as PID 1 there is no default disposition to fall back on.
  std::signal(SIGTERM, onSignal);
  std::signal(SIGINT, onSignal);

  const long frequency_hz = environmentLong("SAFEEDGE_FREQUENCY_HZ", 1000);
  const long rt_priority = environmentLong("SAFEEDGE_RT_PRIORITY", 80);

  if (frequency_hz <= 0 || frequency_hz > 100000) {
    logEvent("fatal", "SAFEEDGE_FREQUENCY_HZ out of range");
    return 2;
  }

  logEvent("info", "starting");

  // --- runtime -------------------------------------------------------------
  rt::CyclicExecutor::Config executor_config;
  executor_config.period = std::chrono::nanoseconds(1'000'000'000LL / frequency_hz);
  executor_config.guard_allocations = true;
  executor_config.thread.policy = rt::SchedulingPolicy::kFifo;
  executor_config.thread.priority = static_cast<int>(rt_priority);
  executor_config.thread.lock_memory = true;
  executor_config.thread.prefault_stack = true;

  safety::DualChannelSupervisor::Config supervisor_config;
  supervisor_config.limits.limited_speed = 10.0;
  supervisor_config.limits.standstill_speed = 0.1;
  supervisor_config.limits.standstill_window = 0.5;
  supervisor_config.limits.stop_time_limit_ns = 500'000'000;
  supervisor_config.discrepancy_tolerance_ns = 5'000'000;

  const safety::SafetyAddress address{0x0011, 0x2200, 0xDEADBEEF};

  ipc::SeqlockSlot<edge::RuntimeSnapshot> published;
  published.store(edge::RuntimeSnapshot{});

  std::atomic<bool> loop_running{false};

  // --- observability -------------------------------------------------------
  edge::HttpServer server;

  server.route("/healthz", [&loop_running]() -> edge::HttpResponse {
    // Liveness: is the process functioning at all. Deliberately does not
    // consider the safety state -- a machine correctly sitting in a latched
    // safe state is healthy, and restarting the container would throw away the
    // fault information an engineer needs while achieving nothing.
    edge::HttpResponse response;
    response.body = loop_running.load() ? "ok\n" : "starting\n";
    response.status = loop_running.load() ? 200 : 503;
    return response;
  });

  server.route("/readyz", [&published]() -> edge::HttpResponse {
    // Readiness: should traffic be sent here. A latched fault means no, so the
    // orchestrator stops routing to it -- without killing it.
    edge::RuntimeSnapshot snapshot;
    edge::HttpResponse response;
    if (!published.tryLoad(snapshot)) {
      response.status = 503;
      response.body = "snapshot unavailable\n";
      return response;
    }
    if (snapshot.fault_latched != 0) {
      response.status = 503;
      response.body = "safety fault latched\n";
      return response;
    }
    response.body = "ready\n";
    return response;
  });

  server.route("/metrics", [&published]() -> edge::HttpResponse {
    edge::RuntimeSnapshot snapshot;
    const bool fresh = published.tryLoad(snapshot);
    edge::HttpResponse response;
    response.content_type = "text/plain; version=0.0.4; charset=utf-8";
    response.body = edge::renderPrometheus(snapshot, fresh);
    return response;
  });

  if (!server.start(metrics_port)) {
    logEvent("fatal", "metrics server failed to start", server.error().c_str());
    return 3;
  }
  logEvent("info", "metrics server listening");

  // --- control loop --------------------------------------------------------
  std::thread control([&] {
    rt::CyclicExecutor executor(executor_config);
    const rt::ThreadConfigReport thread_report = executor.configureCallingThread();

    if (!thread_report.scheduling_applied) {
      // Reported, never swallowed. Without it, every latency number this
      // process exports would describe the machine's load rather than the
      // runtime, and nobody looking at the dashboard would know.
      logEvent("warn",
               "real-time scheduling not granted; latency metrics are indicative only",
               thread_report.what());
    } else {
      logEvent("info", "real-time scheduling granted");
    }

    safety::DualChannelSupervisor supervisor(supervisor_config);
    safety::SafetySender sender(address);
    safety::SafetyReceiver receiver(address, /*watchdog_ns=*/50'000'000);
    safety::Telegram telegram;

    std::array<std::uint8_t, safety::kMaxPayloadBytes> payload_out{};
    const std::array<std::uint8_t, 8> payload_in{1, 2, 3, 4, 5, 6, 7, 8};

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;

    auto cycle = [&](const rt::CycleContext& context) {
      const std::int64_t now_ns =
          context.period_ns * static_cast<std::int64_t>(context.index);

      // Safety telegram round trip. In a real deployment the two ends are
      // different processes over a real transport; here both live in the loop
      // so the runtime exercises the whole path.
      std::size_t received_size = 0;
      std::uint8_t status = 0;
      if (sender.encode(payload_in, 0, telegram)) {
        if (receiver.receive(telegram.view(), now_ns, payload_out, received_size,
                             status) == safety::ReceiveStatus::kValid) {
          ++accepted;
        } else {
          ++rejected;
        }
      }

      safety::SafetyInputs inputs;
      inputs.emergency_stop_asserted = false;
      inputs.communication_ok = !receiver.inSafeState();
      inputs.heartbeat_ok = true;
      inputs.self_test_passed = true;
      inputs.request = safety::SafetyFunction::kSafelyLimitedSpeed;
      inputs.speed_magnitude = 5.0;
      inputs.timestamp_ns = now_ns;

      const safety::SafetyOutputs outputs = supervisor.evaluate(inputs, inputs, now_ns);

      // Publish. Wait-free, so the loop never waits on a scrape.
      edge::RuntimeSnapshot snapshot;
      const rt::ExecutorStats& stats = executor.stats();
      snapshot.cycles_executed = stats.cycles_executed;
      snapshot.overruns = stats.overruns;
      snapshot.skipped_cycles = stats.skipped_cycles;
      snapshot.allocation_violations = stats.allocation_violations;
      snapshot.jitter_p50_ns = stats.wakeup_jitter.p50();
      snapshot.jitter_p99_ns = stats.wakeup_jitter.p99();
      snapshot.jitter_p999_ns = stats.wakeup_jitter.p999();
      snapshot.jitter_max_ns = stats.wakeup_jitter.max();
      snapshot.execution_p50_ns = stats.execution_time.p50();
      snapshot.execution_p99_ns = stats.execution_time.p99();
      snapshot.execution_max_ns = stats.execution_time.max();
      snapshot.safety_state = static_cast<std::uint32_t>(outputs.state);
      snapshot.fault_reason = static_cast<std::uint32_t>(outputs.fault);
      snapshot.torque_permitted = outputs.torque_permitted ? 1U : 0U;
      snapshot.fault_latched = outputs.fault_latched ? 1U : 0U;
      snapshot.telegrams_accepted = accepted;
      snapshot.telegrams_rejected = rejected;
      snapshot.realtime_scheduling_granted = thread_report.scheduling_applied ? 1U : 0U;
      snapshot.uptime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      published.store(snapshot);

      if (g_shutdown_requested != 0) {
        executor.requestStop();
      }
    };

    loop_running.store(true);
    executor.run(rt::CycleCallback(cycle), 0);
    loop_running.store(false);
  });

  // --- wait for shutdown ---------------------------------------------------
  while (g_shutdown_requested == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  logEvent("info", "shutdown requested");

  control.join();
  server.stop();

  edge::RuntimeSnapshot final_snapshot;
  if (published.tryLoad(final_snapshot)) {
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "cycles=%" PRIu64 " overruns=%" PRIu64 " allocations=%" PRIu64,
                  final_snapshot.cycles_executed, final_snapshot.overruns,
                  final_snapshot.allocation_violations);
    logEvent("info", "stopped", detail);
  } else {
    logEvent("info", "stopped");
  }
  return 0;
}
