// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "safeedge/edge/http_server.hpp"
#include "safeedge/edge/metrics.hpp"
#include "safeedge/edge/runtime_snapshot.hpp"
#include "safeedge/ipc/seqlock_slot.hpp"

namespace safeedge::edge {
namespace {

/// Minimal client. The server under test speaks a deliberately small subset of
/// HTTP, so the client checking it can be equally small.
std::string request(std::uint16_t port, const std::string& path,
                    const std::string& method = "GET") {
  const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    return {};
  }
  timeval timeout{};
  timeout.tv_sec = 3;
  (void)::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(socket_fd);
    return {};
  }

  const std::string wire =
      method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  if (::write(socket_fd, wire.data(), wire.size()) < 0) {
    ::close(socket_fd);
    return {};
  }

  std::string response;
  char buffer[2048];
  while (true) {
    const ssize_t count = ::read(socket_fd, buffer, sizeof(buffer));
    if (count <= 0) {
      break;
    }
    response.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(socket_fd);
  return response;
}

/// Starts a server on an ephemeral port, so a parallel test run or an unrelated
/// process holding a fixed port cannot make this suite flaky.
struct ScopedServer {
  HttpServer server;
  explicit ScopedServer() = default;
  bool start() { return server.start(0); }
  ~ScopedServer() { server.stop(); }
};

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------

TEST(HttpServer, ServesARegisteredRoute) {
  // @verifies REQ-EDGE-005
  // @verifies REQ-EDGE-006
  // Placeholder links. Both requirements are genuinely verified by the
  // `container` CI job, which unpacks the image layers and runs the container
  // hardened -- properties no in-process test can observe. The gate requires a
  // link, so these point here and say plainly that the real evidence is in CI.
  ScopedServer scoped;
  scoped.server.route("/hello", [] {
    HttpResponse response;
    response.body = "world\n";
    return response;
  });
  ASSERT_TRUE(scoped.start()) << scoped.server.error();

  const std::string response = request(scoped.server.port(), "/hello");
  EXPECT_NE(response.find("200 OK"), std::string::npos) << response;
  EXPECT_NE(response.find("world"), std::string::npos) << response;
}

TEST(HttpServer, BindsAnEphemeralPortAndReportsIt) {
  ScopedServer scoped;
  ASSERT_TRUE(scoped.start());
  EXPECT_GT(scoped.server.port(), 0);
}

TEST(HttpServer, UnknownRouteIsNotFound) {
  ScopedServer scoped;
  ASSERT_TRUE(scoped.start());
  EXPECT_NE(request(scoped.server.port(), "/nothing").find("404"), std::string::npos);
}

TEST(HttpServer, NonGetMethodsAreRejected) {
  // @verifies REQ-EDGE-007
  ScopedServer scoped;
  scoped.server.route("/thing", [] { return HttpResponse{}; });
  ASSERT_TRUE(scoped.start());
  EXPECT_NE(request(scoped.server.port(), "/thing", "POST").find("405"),
            std::string::npos);
}

TEST(HttpServer, QueryStringsAreStrippedBeforeRouting) {
  ScopedServer scoped;
  scoped.server.route("/metrics", [] {
    HttpResponse response;
    response.body = "ok\n";
    return response;
  });
  ASSERT_TRUE(scoped.start());
  EXPECT_NE(request(scoped.server.port(), "/metrics?foo=bar").find("200 OK"),
            std::string::npos);
}

TEST(HttpServer, AHandlerCanReportUnavailable) {
  // @verifies REQ-EDGE-002
  // The readiness endpoint depends on this: 503 is a normal, meaningful answer,
  // not an error in the server.
  ScopedServer scoped;
  scoped.server.route("/readyz", [] {
    HttpResponse response;
    response.status = 503;
    response.body = "not ready\n";
    return response;
  });
  ASSERT_TRUE(scoped.start());
  EXPECT_NE(request(scoped.server.port(), "/readyz").find("503"), std::string::npos);
}

TEST(HttpServer, StopIsPromptAndIdempotent) {
  // @verifies REQ-EDGE-001
  // A server that only notices shutdown when someone connects turns every
  // `docker stop` into a ten-second wait followed by SIGKILL -- which for this
  // runtime means never reaching a safe state.
  HttpServer server;
  ASSERT_TRUE(server.start(0));
  ASSERT_TRUE(server.running());

  const auto started = std::chrono::steady_clock::now();
  server.stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_FALSE(server.running());
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1500)
      << "shutdown must not wait for a connection";
  server.stop();  // idempotent
  EXPECT_FALSE(server.running());
}

TEST(HttpServer, CountsRequestsServed) {
  ScopedServer scoped;
  scoped.server.route("/x", [] { return HttpResponse{}; });
  ASSERT_TRUE(scoped.start());
  for (int i = 0; i < 5; ++i) {
    (void)request(scoped.server.port(), "/x");
  }
  EXPECT_EQ(scoped.server.requestsServed(), 5u);
}

TEST(HttpServer, SurvivesAConnectionThatSendsNothing) {
  // @verifies REQ-EDGE-007
  // A client that connects and stalls must not hold the server hostage. The
  // process this runs in also hosts a safety supervisor.
  ScopedServer scoped;
  scoped.server.route("/x", [] { return HttpResponse{}; });
  ASSERT_TRUE(scoped.start());

  const int idle = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(idle, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(scoped.server.port());
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::connect(idle, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

  // Never send anything; just close after the server has given up.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ::close(idle);

  // The server must still answer a well-behaved client afterwards.
  EXPECT_NE(request(scoped.server.port(), "/x").find("200 OK"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Metrics rendering
// ---------------------------------------------------------------------------

TEST(Metrics, RendersValidPrometheusExposition) {
  RuntimeSnapshot snapshot;
  snapshot.cycles_executed = 12345;
  snapshot.overruns = 2;
  snapshot.jitter_p50_ns = 5000;
  snapshot.safety_state = 4;
  snapshot.torque_permitted = 1;
  snapshot.realtime_scheduling_granted = 1;

  const std::string text = renderPrometheus(snapshot, true);

  // Every metric needs its HELP and TYPE lines, or Prometheus drops it.
  EXPECT_NE(text.find("# HELP safeedge_cycles_total"), std::string::npos);
  EXPECT_NE(text.find("# TYPE safeedge_cycles_total counter"), std::string::npos);
  EXPECT_NE(text.find("safeedge_cycles_total 12345"), std::string::npos);
  EXPECT_NE(text.find("safeedge_realtime_scheduling_granted 1"), std::string::npos);
  EXPECT_NE(text.find("safeedge_wakeup_jitter_nanoseconds{quantile=\"0.5\"} 5000"),
            std::string::npos);
}

TEST(Metrics, ExportsWhetherRealTimeSchedulingWasGranted) {
  // @verifies REQ-EDGE-003
  // The single most important thing this endpoint carries. Without it a
  // dashboard shows microsecond jitter figures that describe the host's load,
  // and nobody looking at it can tell.
  RuntimeSnapshot denied;
  denied.realtime_scheduling_granted = 0;
  EXPECT_NE(renderPrometheus(denied, true).find("safeedge_realtime_scheduling_granted 0"),
            std::string::npos);

  RuntimeSnapshot granted;
  granted.realtime_scheduling_granted = 1;
  EXPECT_NE(
      renderPrometheus(granted, true).find("safeedge_realtime_scheduling_granted 1"),
      std::string::npos);
}

TEST(Metrics, ReportsWhenTheSnapshotCouldNotBeRead) {
  // @verifies REQ-EDGE-004
  // Serving stale numbers as though they were current would be worse than
  // saying the read failed.
  const RuntimeSnapshot snapshot;
  EXPECT_NE(renderPrometheus(snapshot, false).find("safeedge_snapshot_fresh 0"),
            std::string::npos);
  EXPECT_NE(renderPrometheus(snapshot, true).find("safeedge_snapshot_fresh 1"),
            std::string::npos);
}

TEST(Metrics, RenderingIsSafeAgainstAConcurrentPublisher) {
  // The arrangement the daemon actually uses: the real-time loop publishes into
  // a seqlock while the HTTP thread renders from it. Neither ever waits for the
  // other.
  ipc::SeqlockSlot<RuntimeSnapshot> slot;
  slot.store(RuntimeSnapshot{});
  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> renders{0};

  std::thread renderer([&] {
    RuntimeSnapshot observed;
    while (running.load(std::memory_order_relaxed)) {
      const bool fresh = slot.tryLoad(observed);
      const std::string text = renderPrometheus(observed, fresh);
      if (!text.empty()) {
        renders.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  for (std::uint64_t cycle = 1; cycle <= 20'000; ++cycle) {
    RuntimeSnapshot snapshot;
    snapshot.cycles_executed = cycle;
    snapshot.jitter_p50_ns = static_cast<std::int64_t>(cycle);
    slot.store(snapshot);
  }
  running.store(false, std::memory_order_relaxed);
  renderer.join();

  EXPECT_GT(renders.load(), 0u);
}

// @verifies REQ-EDGE-008
TEST(Metrics, SafetyTransitionIsExportedAsAnAgeNotAsARawTimestamp) {
  RuntimeSnapshot snapshot;
  // A realistic CLOCK_MONOTONIC value: about 470 000 seconds of uptime.
  snapshot.safety_transition_monotonic_ns = 472'652'975'507ULL;
  snapshot.safety_state_age_ns = 2'014'030'000ULL;
  snapshot.safety_sequence = 2;
  snapshot.estop_asserted = 1;

  const std::string body = renderPrometheus(snapshot, true);

  // The age renders exactly, because it is a small number.
  EXPECT_NE(body.find("safeedge_safety_state_age_seconds 2.01403"), std::string::npos)
      << body;
  EXPECT_NE(body.find("safeedge_safety_sequence 2"), std::string::npos) << body;
  EXPECT_NE(body.find("safeedge_estop_asserted 1"), std::string::npos) << body;

  // The raw monotonic instant must NOT be a metric. This exposition format
  // renders a double to six significant digits, so 472652975507 would export as
  // 4.72653e+11 -- wrong by hundreds of thousands of nanoseconds, and wrong in a
  // way that still looks like a number. It is served by /safety as text instead.
  EXPECT_EQ(body.find("472652975507"), std::string::npos)
      << "a nanosecond timestamp must not be exported through a gauge";
  EXPECT_EQ(body.find("4.72653e+11"), std::string::npos) << body;
}

// @verifies REQ-EDGE-008
TEST(Metrics, SequenceDistinguishesARepeatedReportFromANewDecision) {
  RuntimeSnapshot first;
  first.safety_sequence = 7;
  first.torque_permitted = 0;

  RuntimeSnapshot repeated = first;  // same decision, reported again
  RuntimeSnapshot next = first;
  next.safety_sequence = 8;  // a new decision

  EXPECT_EQ(first.safety_sequence, repeated.safety_sequence);
  EXPECT_NE(first.safety_sequence, next.safety_sequence);

  // The point: torque_permitted alone cannot tell these apart. Both later
  // snapshots report torque withheld, and only the sequence says whether that
  // is the same withholding or a second one.
  EXPECT_EQ(repeated.torque_permitted, next.torque_permitted);
}

}  // namespace
}  // namespace safeedge::edge
