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
#include <new>
#include <string>
#include <thread>

#include "safeedge/edge/http_server.hpp"
#include "safeedge/edge/metrics.hpp"
#include "safeedge/edge/runtime_snapshot.hpp"
#include "safeedge/ipc/seqlock_slot.hpp"
#include "safeedge/ipc/shared_memory.hpp"
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
  // A failed flush cannot be reported through the same stream; the process
  // will still continue or terminate according to the event just written.
  static_cast<void>(std::fflush(stdout));
}

/// CLOCK_MONOTONIC in nanoseconds.
///
/// Monotonic time shares an epoch across every process on the machine (boot),
/// which is what lets a consumer in another process subtract our transition
/// stamp from its own observation and get a real interval. A wall clock would
/// not do: it steps, and a stepped clock produces negative intervals and
/// impossible velocities exactly once, at the worst moment.
std::uint64_t monotonicNanos() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

const char* environmentValue(const char* name) noexcept {
  // main snapshots every environment setting before it starts any worker
  // thread. getenv is only unsafe when another thread can mutate the process
  // environment concurrently.
  return std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
}

long environmentLong(const char* name, long fallback) {
  const char* raw = environmentValue(name);
  if (raw == nullptr || *raw == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  return (end != nullptr && *end == '\0') ? parsed : fallback;
}

std::string environmentString(const char* name, const char* fallback) {
  const char* raw = environmentValue(name);
  return (raw != nullptr && *raw != '\0') ? raw : fallback;
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

/// Watches a file whose existence means "emergency stop asserted".
///
/// Deliberately NOT checked from the control loop. A stat() is cheap, but it is
/// still a syscall on a path the filesystem may decide to make slow -- an NFS
/// mount, a full disk, a container layer under pressure -- and a control loop
/// whose period depends on the filesystem is not a control loop. The watcher
/// runs on its own thread and publishes to an atomic the loop reads for free.
///
/// A file rather than an HTTP verb because it needs no new surface, works with
/// `docker exec ... touch /tmp/estop`, and survives the metrics server being
/// wedged -- which is exactly when someone wants to stop the machine.
///
/// This is a demonstration input. A real emergency stop is a dual-channel
/// hardware circuit that removes power without asking software's permission;
/// nothing in this process is a substitute for one, and the supervisor treats
/// this input as a request, not as the stop itself.
class FileFlagWatcher {
 public:
  enum class Mode : std::uint8_t {
    /// Presence of the file IS the state. Removing it clears the input.
    /// Used for the emergency stop, which is a condition, not an event.
    kLevel,
    /// Appearance of the file is a one-shot event. The file is removed once
    /// seen, and the flag is delivered exactly once.
    ///
    /// Acknowledgement has to work this way. A level-triggered acknowledge file
    /// left in place would re-acknowledge every cycle, so a fault could never
    /// stay latched -- which defeats the entire point of latching it.
    kLatchOnce,
  };

  FileFlagWatcher(std::string path, std::chrono::milliseconds period, Mode mode,
                  const char* asserted_message, const char* cleared_message)
      : path_(std::move(path)),
        period_(period),
        mode_(mode),
        asserted_message_(asserted_message),
        cleared_message_(cleared_message) {
    thread_ = std::thread([this] { run(); });
  }

  ~FileFlagWatcher() {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  FileFlagWatcher(const FileFlagWatcher&) = delete;
  FileFlagWatcher& operator=(const FileFlagWatcher&) = delete;
  FileFlagWatcher(FileFlagWatcher&&) = delete;
  FileFlagWatcher& operator=(FileFlagWatcher&&) = delete;

  /// Level mode: the current state.
  [[nodiscard]] bool asserted() const noexcept {
    return asserted_.load(std::memory_order_relaxed);
  }

  /// Latch mode: true once per appearance, and false thereafter.
  bool takeOnce() noexcept {
    return asserted_.exchange(false, std::memory_order_acq_rel);
  }

 private:
  void run() {
    while (running_.load(std::memory_order_relaxed)) {
      const bool present = ::access(path_.c_str(), F_OK) == 0;
      if (mode_ == Mode::kLatchOnce) {
        if (present) {
          // Remove it before setting the flag, so a second appearance cannot be
          // lost between the loop reading the flag and the file being cleared.
          ::unlink(path_.c_str());
          asserted_.store(true, std::memory_order_release);
          logEvent("info", asserted_message_, path_.c_str());
        }
      } else if (present != asserted_.load(std::memory_order_relaxed)) {
        asserted_.store(present, std::memory_order_relaxed);
        logEvent("warn", present ? asserted_message_ : cleared_message_, path_.c_str());
      }
      std::this_thread::sleep_for(period_);
    }
  }

  std::string path_;
  std::chrono::milliseconds period_;
  Mode mode_;
  const char* asserted_message_;
  const char* cleared_message_;
  std::atomic<bool> asserted_{false};
  std::atomic<bool> running_{true};
  std::thread thread_;
};

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
  const auto previous_term_handler = std::signal(SIGTERM, onSignal);
  const auto previous_int_handler = std::signal(SIGINT, onSignal);
  if (previous_term_handler == SIG_ERR || previous_int_handler == SIG_ERR) {
    logEvent("fatal", "could not install shutdown signal handlers");
    return 2;
  }

  // Snapshot all remaining settings before worker threads start. Healthcheck
  // mode deliberately never reads these unrelated values.
  const long frequency_hz = environmentLong("SAFEEDGE_FREQUENCY_HZ", 1000);
  const long rt_priority = environmentLong("SAFEEDGE_RT_PRIORITY", 80);
  const std::string snapshot_shm = environmentString("SAFEEDGE_SNAPSHOT_SHM", "");
  const std::string estop_path =
      environmentString("SAFEEDGE_ESTOP_FILE", "/tmp/safeedge-estop");
  const std::string ack_path =
      environmentString("SAFEEDGE_ACK_FILE", "/tmp/safeedge-ack");

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

  // The published snapshot, optionally in shared memory so another process can
  // read it without going through HTTP.
  //
  // Same slot either way, so there is one publish rather than two -- the HTTP
  // handlers in this process and a reader in another are looking at the same
  // bytes. A second publish path would be a second thing to forget to update,
  // and the two would disagree the first time somebody added a field.
  //
  // This is what the ipc component was built for, and it is the first time it
  // carries a real payload across a real process boundary rather than a test's.
  using SnapshotSlot = ipc::SeqlockSlot<edge::RuntimeSnapshot>;
  SnapshotSlot local_slot;
  ipc::SharedMemoryRegion snapshot_region;
  SnapshotSlot* published = &local_slot;

  if (!snapshot_shm.empty()) {
    // A region left by a previous instance holds that instance's last state.
    // Adopting it would mean serving a dead runtime's numbers as though they
    // were current, so it is removed rather than reused.
    ipc::SharedMemoryRegion::unlinkName(snapshot_shm.c_str());
    snapshot_region =
        ipc::SharedMemoryRegion::create(snapshot_shm.c_str(), sizeof(SnapshotSlot));
    if (snapshot_region.valid()) {
      published = new (snapshot_region.data()) SnapshotSlot();
      logEvent("info", "publishing snapshot to shared memory", snapshot_shm.c_str());
    } else {
      // Reported, not swallowed. A reader waiting on this region would otherwise
      // wait forever with nothing saying why.
      logEvent("warn", "could not create snapshot shared memory; HTTP only",
               snapshot_shm.c_str());
    }
  }

  published->store(edge::RuntimeSnapshot{});

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

  server.route("/readyz", [published]() -> edge::HttpResponse {
    // Readiness: should traffic be sent here. A latched fault means no, so the
    // orchestrator stops routing to it -- without killing it.
    edge::RuntimeSnapshot snapshot;
    edge::HttpResponse response;
    if (!published->tryLoad(snapshot)) {
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

  // The endpoint that makes a reaction time measurable.
  //
  // Three integers, space separated: sequence, torque_permitted, and the
  // monotonic instant of the transition. Deliberately not JSON -- a consumer
  // parsing this sits in or beside a control loop, and the format should cost
  // it a sscanf rather than a parser.
  //
  // /readyz cannot serve this purpose. It answers "should traffic come here",
  // it carries no instant, and a consumer polling it can only ever stamp its
  // own observation. The pickcell integration could not report an end-to-end
  // number until this route existed.
  server.route("/safety", [published]() -> edge::HttpResponse {
    edge::RuntimeSnapshot snapshot;
    edge::HttpResponse response;
    response.content_type = "text/plain";
    if (!published->tryLoad(snapshot)) {
      // No snapshot is not a report of safety. 503 and no numbers: a consumer
      // must not be able to parse a permissive answer out of a failed read.
      response.status = 503;
      response.body = "snapshot unavailable\n";
      return response;
    }
    char line[128];
    static_cast<void>(std::snprintf(
        line, sizeof(line), "%llu %u %llu\n",
        static_cast<unsigned long long>(snapshot.safety_sequence),
        static_cast<unsigned>(snapshot.torque_permitted),
        static_cast<unsigned long long>(snapshot.safety_transition_monotonic_ns)));
    response.body = line;
    return response;
  });

  server.route("/metrics", [published]() -> edge::HttpResponse {
    edge::RuntimeSnapshot snapshot;
    const bool fresh = published->tryLoad(snapshot);
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

  // --- emergency stop input ------------------------------------------------
  FileFlagWatcher estop(estop_path, std::chrono::milliseconds(10),
                        FileFlagWatcher::Mode::kLevel, "emergency stop asserted",
                        "emergency stop cleared");
  logEvent("info", "emergency stop file watched", estop_path.c_str());

  // Clearing the emergency stop does NOT restart the machine, and that is not a
  // convenience to be smoothed away: IEC 60204-1 requires that restoring an
  // emergency stop device must not by itself restart anything. Coming back
  // needs a separate, deliberate acknowledgement -- a second person-shaped act.
  //
  // One-shot, because an acknowledge that stayed asserted would clear the latch
  // again on the next cycle, and a fault that cannot stay latched is not
  // latched.
  FileFlagWatcher acknowledge(ack_path, std::chrono::milliseconds(10),
                              FileFlagWatcher::Mode::kLatchOnce,
                              "fault acknowledgement received", "");
  logEvent("info", "acknowledge file watched", ack_path.c_str());

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

    auto last_safety_state = safety::SafetyState::kSafeTorqueOff;
    bool last_torque_permitted = false;
    std::uint64_t last_transition_ns = monotonicNanos();
    std::uint64_t safety_sequence = 0;

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
      inputs.emergency_stop_asserted = estop.asserted();
      inputs.acknowledge = acknowledge.takeOnce();
      inputs.communication_ok = !receiver.inSafeState();
      inputs.heartbeat_ok = true;
      inputs.self_test_passed = true;
      inputs.request = safety::SafetyFunction::kSafelyLimitedSpeed;
      inputs.speed_magnitude = 5.0;
      inputs.timestamp_ns = now_ns;

      const safety::SafetyOutputs outputs = supervisor.evaluate(inputs, inputs, now_ns);

      // A transition is what a consumer needs to react to, so it gets a stamp
      // and a sequence number. Both are read by pickcell to compute how long the
      // cell took to stop after this instant -- a figure that was not computable
      // before these existed, because /readyz carries no time.
      if (outputs.state != last_safety_state ||
          outputs.torque_permitted != last_torque_permitted) {
        last_safety_state = outputs.state;
        last_torque_permitted = outputs.torque_permitted;
        last_transition_ns = monotonicNanos();
        ++safety_sequence;
      }

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
      snapshot.safety_transition_monotonic_ns = last_transition_ns;
      snapshot.safety_sequence = safety_sequence;
      snapshot.safety_state_age_ns = monotonicNanos() - last_transition_ns;
      snapshot.estop_asserted = estop.asserted() ? 1U : 0U;
      snapshot.telegrams_accepted = accepted;
      snapshot.telegrams_rejected = rejected;
      snapshot.realtime_scheduling_granted = thread_report.scheduling_applied ? 1U : 0U;
      snapshot.uptime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      published->store(snapshot);

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
  if (published->tryLoad(final_snapshot)) {
    char detail[256];
    static_cast<void>(
        std::snprintf(detail, sizeof(detail),
                      "cycles=%" PRIu64 " overruns=%" PRIu64 " allocations=%" PRIu64,
                      final_snapshot.cycles_executed, final_snapshot.overruns,
                      final_snapshot.allocation_violations));
    logEvent("info", "stopped", detail);
  } else {
    logEvent("info", "stopped");
  }
  return 0;
}
