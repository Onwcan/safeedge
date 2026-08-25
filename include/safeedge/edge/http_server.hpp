// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

namespace safeedge::edge {

struct HttpResponse {
  int status{200};
  std::string content_type{"text/plain; charset=utf-8"};
  std::string body;
};

/// A deliberately minimal HTTP/1.1 server for the observability endpoints.
///
/// Why write one rather than take a dependency
/// -------------------------------------------
/// It serves three routes, answers only GET, and is reachable only from inside
/// the container network. Pulling in an HTTP library for that would add a
/// dependency to scan, patch and license, would grow the image it is supposed
/// to keep small, and would widen the attack surface of a process that also
/// runs a safety supervisor. Roughly two hundred lines is a fair trade for
/// removing all of that.
///
/// It is not a general-purpose server and does not pretend to be: no keep-alive,
/// no chunked encoding, no TLS, no concurrency beyond one connection at a time.
/// Those limits are listed because a reader needs to know where the line is,
/// not because they are oversights.
///
/// Threading
/// ---------
/// Runs its accept loop on its own thread. Handlers therefore execute off the
/// real-time path -- which is the whole point, and why the data they read comes
/// from a seqlock snapshot rather than from the live runtime objects.
class HttpServer {
 public:
  // @satisfies REQ-EDGE-002
  using Handler = std::function<HttpResponse()>;

  HttpServer() = default;
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&) = delete;
  HttpServer& operator=(HttpServer&&) = delete;

  /// Registers a GET handler. Must be called before `start`.
  void route(const std::string& path, Handler handler);

  /// Binds and begins serving. Returns false on failure, with `error()` set.
  ///
  /// Binds to `0.0.0.0` because inside a container that is the only way the
  /// port is reachable from the compose network. The container itself is what
  /// limits exposure, not the bind address -- and pretending otherwise by
  /// binding to localhost would simply make the endpoint unreachable while
  /// looking careful.
  [[nodiscard]] bool start(std::uint16_t port);

  /// Stops the accept loop and joins the thread. Idempotent.
  void stop();

  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  /// Requests served since start. Used by the tests and worth exporting.
  [[nodiscard]] std::uint64_t requestsServed() const noexcept { return requests_.load(); }

 private:
  void acceptLoop();
  void serveConnection(int connection);

  int listen_fd_{-1};
  std::uint16_t port_{0};
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> requests_{0};
  std::unordered_map<std::string, Handler> routes_;
  std::string error_;
};

}  // namespace safeedge::edge
