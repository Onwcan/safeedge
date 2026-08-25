// SPDX-License-Identifier: Apache-2.0
#include "safeedge/edge/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace safeedge::edge {
namespace {

constexpr std::size_t kMaxRequestBytes = 8192;
/// How long the accept loop waits before re-checking the stop flag.
constexpr int kPollIntervalMs = 200;
/// How long a connection may take to send its request line before being cut.
constexpr int kRequestTimeoutMs = 2000;

const char* reasonPhrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 503:
      return "Service Unavailable";
    default:
      return "Error";
  }
}

bool writeAll(int fd, const char* data, std::size_t length) {
  std::size_t written = 0;
  while (written < length) {
    const ssize_t count = ::write(fd, data + written, length - written);
    if (count <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  return true;
}

}  // namespace

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string& path, Handler handler) {
  routes_[path] = std::move(handler);
}

bool HttpServer::start(std::uint16_t port) {
  if (running_.load()) {
    return true;
  }

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    error_ = std::string("socket: ") + std::strerror(errno);
    return false;
  }

  // SO_REUSEADDR so a restart does not have to wait out TIME_WAIT. In a
  // container that matters: an orchestrator restarting a crashed process
  // within seconds would otherwise fail to bind and look like a different
  // fault entirely.
  const int enable = 1;
  (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    error_ = std::string("bind: ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 16) != 0) {
    error_ = std::string("listen: ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  // Read back the bound port, so a caller can pass 0 and discover what it got.
  // The tests rely on this: a fixed port makes them fail when something else on
  // the machine happens to hold it.
  socklen_t length = sizeof(address);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
    port_ = ntohs(address.sin_port);
  } else {
    port_ = port;
  }

  running_.store(true);
  thread_ = std::thread([this] { acceptLoop(); });
  error_.clear();
  return true;
}

void HttpServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void HttpServer::acceptLoop() {
  while (running_.load()) {
    // poll rather than a blocking accept, so stop() is observed within one
    // interval instead of hanging until the next connection arrives. A server
    // that only shuts down when someone talks to it turns `docker stop` into a
    // ten-second wait followed by SIGKILL.
    pollfd waiting{};
    waiting.fd = listen_fd_;
    waiting.events = POLLIN;
    const int ready = ::poll(&waiting, 1, kPollIntervalMs);
    if (ready <= 0) {
      continue;
    }

    const int connection = ::accept(listen_fd_, nullptr, nullptr);
    if (connection < 0) {
      continue;
    }
    serveConnection(connection);
    ::close(connection);
  }
}

// @satisfies REQ-EDGE-007
void HttpServer::serveConnection(int connection) {
  std::string request;
  request.reserve(512);

  // Read until the end of the headers, bounded in both size and time. An
  // unbounded read here is a denial of service against a process that is also
  // running a safety supervisor.
  char buffer[1024];
  while (request.find("\r\n\r\n") == std::string::npos &&
         request.size() < kMaxRequestBytes) {
    pollfd waiting{};
    waiting.fd = connection;
    waiting.events = POLLIN;
    if (::poll(&waiting, 1, kRequestTimeoutMs) <= 0) {
      return;  // client stalled; drop it
    }
    const ssize_t count = ::read(connection, buffer, sizeof(buffer));
    if (count <= 0) {
      return;
    }
    request.append(buffer, static_cast<std::size_t>(count));
  }

  HttpResponse response;
  const std::size_t method_end = request.find(' ');
  const std::size_t path_end = method_end == std::string::npos
                                   ? std::string::npos
                                   : request.find(' ', method_end + 1);

  if (method_end == std::string::npos || path_end == std::string::npos) {
    response.status = 400;
    response.body = "malformed request\n";
  } else {
    const std::string method = request.substr(0, method_end);
    std::string path = request.substr(method_end + 1, path_end - method_end - 1);
    if (const std::size_t query = path.find('?'); query != std::string::npos) {
      path.resize(query);
    }

    if (method != "GET") {
      response.status = 405;
      response.body = "only GET is supported\n";
    } else if (const auto found = routes_.find(path); found != routes_.end()) {
      response = found->second();
    } else {
      response.status = 404;
      response.body = "no such endpoint\n";
    }
  }

  std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " +
                     reasonPhrase(response.status) + "\r\n";
  head += "Content-Type: " + response.content_type + "\r\n";
  head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  // No keep-alive. One request per connection keeps the server to a single
  // state machine with no timeout bookkeeping, and a metrics scrape every few
  // seconds does not care.
  head += "Connection: close\r\n\r\n";

  if (writeAll(connection, head.data(), head.size())) {
    (void)writeAll(connection, response.body.data(), response.body.size());
  }
  requests_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace safeedge::edge
